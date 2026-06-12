#include "core/MatchingEngine.hpp"

#include<cassert>
#include<algorithm>
#include<cmath>

namespace MatchEngine{

MatchingEngine::MatchingEngine(OrderBook& book, FeeCalculator& fee_calculator)
    :order_book(book), fees_calculator(fee_calculator){}

//Generate Trade with fees
Trade MatchingEngine::generate_trades(uint64_t trade_qty, Order* incoming, Order* resting){
    TimeUtils::Timestamp eng_ts = TimeUtils::now_ns();
    TimeUtils::Timestamp wall_ts = TimeUtils::wall_time_ns();

    double price=resting->price;
    std::string buy_id;
    std::string sell_id;

    if(incoming->side==Side::BUY){
        buy_id=incoming->order_id;
        sell_id=resting->order_id;
    }
    else{
        buy_id=resting->order_id;
        sell_id=incoming->order_id;
    }

    // Fees
    double notional=price*static_cast<double>(trade_qty);

    //MAKER=Resting, TAKER=Incoming
    if(incoming->user_id!=resting->user_id){
        fees_calculator.update_volume(resting->user_id, notional);
        fees_calculator.update_volume(incoming->user_id, notional);
    }

    double maker_fee=fees_calculator.maker_fee(resting->user_id, price, trade_qty);
    double taker_fee=fees_calculator.taker_fee(incoming->user_id,price, trade_qty);

    assert(taker_fee >= 0);
    assert(!std::isnan(maker_fee));
    assert(!std::isnan(taker_fee));

    //Generate Trade
    Trade t(
        incoming->user_id,
        std::move(buy_id),
        std::move(sell_id),
        price,
        trade_qty,
        eng_ts,
        wall_ts,
        maker_fee,
        taker_fee
    );

    //Publish Trade by TradePublisher
    if(trade_publisher){
        TradeEvent ev{
            t.user_id,
            t.buy_order_id,
            t.sell_order_id,
            t.price,
            t.quantity,
            t.engine_ts,
            t.wall_ts,
            t.maker_fee,
            t.taker_fee
        };
        trade_publisher->publish(ev);
    }

    return t;
}

// Order type dispatcher
void MatchingEngine::process_order(Order* order){
    assert(order->status==OrderStatus::CREATED);
    if (order->timestamp_ns == 0) order->timestamp_ns = TimeUtils::now_ns();
    switch(order->type){
        case OrderType::LIMIT: 
            process_limit_order(order); 
            break;
        case OrderType::MARKET: 
            process_market_order(order); 
            break;
        case OrderType::IOC: 
            process_ioc_order(order); 
            break;
        case OrderType::FOK: 
            process_fok_order(order); 
            break;
        case OrderType::STOP_LOSS:
        case OrderType::STOP_LIMIT:
            process_stop_order(order);
            break;
    }
}

// Run check
void MatchingEngine::run(EventQueue& queue) {
    running=true;

    while(running) {
        EngineEvent event;
        queue.pop(event);

        process_event(event);
    }
}

// Process event via EventType
void MatchingEngine::process_event(const EngineEvent& event) {
    switch(event.type) {
        case EventType::NEW_ORDER: {
            Order* order = event.order;
            order->timestamp_ns = TimeUtils::now_ns();
            process_order(order);
            break;
        }
        case EventType::CANCEL_ORDER:
            order_book.cancel_order(event.order_id);
            break;
        case EventType::STOP:
            running=false;
            break;
    }
}

// Matching Loop common for any type of order
void MatchingEngine::matching_loop(Order* order){
    Side side=order->side;
    bool any_trade=false;

    while(order->remaining_quantity()>0){
        PriceLevel* level=order_book.get_best_opposite(side);
        if(!level) break;
        assert(level->head != nullptr);

        if(order->type!=OrderType::MARKET){
            if(!cross(order, level)) break;
        }

        Order* resting=level->get_head_order();
        assert(resting);

        uint64_t trade_qty=std::min(order->remaining_quantity(), resting->remaining_quantity());
        assert(trade_qty>0);

        order->fill_quantity(trade_qty);
        resting->fill_quantity(trade_qty);
        level->reduce_quantity(trade_qty);

        Trade t=generate_trades(trade_qty, order, resting);
        trades.push_back(t);
        last_trade_price=t.price;
        any_trade=true;

        assert(t.quantity > 0);
        assert(t.price == resting->price);

        if(resting->is_filled()){
            level->remove_order(resting);
            if(level->is_empty()){
                order_book.remove_price_level(resting->side, level);
                level=nullptr;
            }
        }
    }
    if(any_trade) check_stop_orders();
}

// Insert for limit order
void MatchingEngine::process_limit_order(Order* order){
    assert(order);
    assert(order->price_level == nullptr);
    assert(order->type == OrderType::LIMIT);

    matching_loop(order);
    if(!order->is_filled()){
        order_book.insert_limit(order);
        if(order->filled_quantity){
            order->status=OrderStatus::PARTIALLY_FILLED;
        }
        else{
            order->status=OrderStatus::OPEN;
        }
    }
    else{
        order->status=OrderStatus::COMPLETED;
    }
}

// Insert for market order
void MatchingEngine::process_market_order(Order* order){
    assert(order);
    assert(order->price_level == nullptr);
    assert(order->type == OrderType::MARKET);

    matching_loop(order);
    if(!order->filled_quantity){
        order->status=OrderStatus::CANCELLED;
    }
    else if(order->remaining_quantity()){
        order->status=OrderStatus::PARTIALLY_FILLED;
    }
    else{
        order->status=OrderStatus::COMPLETED;
    }
    assert(order->status != OrderStatus::OPEN);

}

// Insert for IOC order
void MatchingEngine::process_ioc_order(Order* order){
    assert(order);
    assert(order->price_level == nullptr);
    assert(order->type == OrderType::IOC);

    matching_loop(order);
    if(!order->filled_quantity){
        order->status=OrderStatus::CANCELLED;// Documented in README 0 liquidity becomes CANCELLED
    }
    else if(order->remaining_quantity()){
        order->status=OrderStatus::PARTIALLY_FILLED;
    }
    else{
        order->status=OrderStatus::COMPLETED;
    }
    assert(order->status != OrderStatus::OPEN);
}

// Insert for FOK order
void MatchingEngine::process_fok_order(Order* order){
    assert(order);
    assert(order->price_level == nullptr);
    assert(order->type == OrderType::FOK);

    if(!order_book.can_fully_fill(order)){
        order->status=OrderStatus::CANCELLED;
        return;
    }
    matching_loop(order);
    assert(order->is_filled());
    order->status=OrderStatus::COMPLETED;
}

// Pre Scan loop
// Iterates the sorted map directly — PriceLevel::next is never maintained.
bool OrderBook::can_fully_fill(const Order* order) const{
    uint64_t required_qty = order->original_quantity;

    if (order->side == Side::BUY) {
        for (auto it = asks.begin(); it != asks.end() && required_qty > 0; ++it) {
            if (!MatchingEngine::cross(order, it->second)) break;
            required_qty -= std::min<uint64_t>(required_qty, it->second->total_quantity);
        }
    } else {
        for (auto it = bids.rbegin(); it != bids.rend() && required_qty > 0; ++it) {
            if (!MatchingEngine::cross(order, it->second)) break;
            required_qty -= std::min<uint64_t>(required_qty, it->second->total_quantity);
        }
    }

    return required_qty == 0;
}

// Helper function to check if price Level crosses
bool MatchingEngine::cross(const Order* order, const PriceLevel* level){
    Side side=order->side;
    bool crosses =
            (side == Side::BUY  && order->price >= level->price) ||
            (side == Side::SELL && order->price <= level->price);
    
   return crosses;
}

// Insert for stop loss orders
void MatchingEngine::process_stop_order(Order* order){
    assert(order);
    assert(order->type == OrderType::STOP_LOSS || order->type == OrderType::STOP_LIMIT);

    order_book.pending_stops.push_back(order);
    order->status=OrderStatus::OPEN;
}

// Check stop loss orders after every trade O(S) with vector, O(log S) with map
void MatchingEngine::check_stop_orders(){
    std::vector<Order*> triggered;

    for(auto* order: order_book.pending_stops){
        if(order->is_triggered) continue;

        bool should_trigger=false;

        if(order->side==Side::BUY) should_trigger = last_trade_price>=order->stop_price;
        else should_trigger = last_trade_price<=order->stop_price;
        
        if(should_trigger){
            order->is_triggered=true;
            triggered.push_back(order);
        }
    }

    for(auto* order: triggered){
        order_book.pending_stops.erase(
            std::remove(order_book.pending_stops.begin(),
                        order_book.pending_stops.end(),
                        order),
            order_book.pending_stops.end()
        );

        if(order->type==OrderType::STOP_LOSS){
            order->type=OrderType::MARKET;
            process_market_order(order);
        }

        else{
            order->type=OrderType::LIMIT;
            process_limit_order(order);
        }
    }
}

}// namespace MatchEngine