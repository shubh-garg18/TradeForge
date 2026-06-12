# Architecture & Design Decisions

## Overview

The engine is built around a single `OrderBook` owned by a `MatchingEngine`. All matching, stop evaluation, and fee calculation happen synchronously in the matching loop. Async submission is layered on top via `EventQueue`.

> **Current scope:** single-symbol. Multi-symbol routing (a `symbol → OrderBook` map) is a planned enhancement — see [Future Enhancements](../README.md#future-enhancements).

---

## Component Map

```text
                ┌────────────────────┐
                │     EventQueue     │
                │ (async submission) │
                └─────────┬──────────┘
                          │  pop (single worker)
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                      MatchingEngine                         │
│  process_order → matching_loop → generate_trades           │
│  check_stop_orders (after every fill)                      │
└───────┬──────────────────┬───────────────────┬──────────────┘
        │ owns ref to      │ owns ref to       │ publishes to
        ▼                  ▼                   ▼
┌────────────────────┐ ┌───────────────┐ ┌────────────────┐
│     OrderBook      │ │ FeeCalculator │ │ TradePublisher │
│  bids  (map ↓)     │ └───────────────┘ └────────────────┘
│  asks  (map ↑)     │
│  orders (hash map) │
│  pending_stops     │
└────────────────────┘
```

---

## Components

### MatchingEngine

Central orchestrator. Owns references to `OrderBook` and `FeeCalculator`. Responsibilities:

- Dispatches incoming orders to the correct handler
- Runs the shared `matching_loop` (price-time priority)
- Calls `check_stop_orders` after every fill
- Generates `Trade` records with maker/taker fees
- Publishes `TradeEvent` to the registered `TradePublisher`

Entry point for all order submission is `process_order(Order*)`, which dispatches by `OrderType`.

### OrderBook

Owns all resting state. Two sorted price maps (`std::map<double, PriceLevel*>`) for bids (descending) and asks (ascending). Each `PriceLevel` holds an intrusive FIFO linked list of `Order*`.

Also holds `pending_stops` — a `vector<Order*>` of untriggered stop orders. This is a known coupling; `MatchingEngine` scans and mutates it directly after each fill. A dedicated `StopOrderManager` is the planned clean-up.

Cached `best_bid` and `best_ask` pointers are updated on every structural operation (insert, cancel, fill-driven removal). BBO reads are O(1); the pointer refresh is O(log P).

### EventQueue

Thread-safe command queue (`std::queue` + `std::mutex` + `std::condition_variable`). Three event types: `NEW_ORDER`, `CANCEL_ORDER`, `STOP`.

Call `engine.run(queue)` on a worker thread; push `EngineEvent`s from any producer. The engine blocks on `pop` when the queue is empty.

### TradePublisher

Interface decoupling the matching loop from downstream consumers. Register with `engine.set_trade_publisher(&publisher)`. `InMemoryTradePublisher` collects `TradeEvent` objects into a vector — useful for testing. Production use should stream externally.

### FeeCalculator

Tracks cumulative notional volume per user ID and selects the fee tier at trade time. Taker (incoming) orders pay a fee; maker (resting) orders pay nothing at the base tier and earn a rebate at higher volume tiers. Self-matches (same user on both sides) do not accumulate volume. Tier lookup and volume update happen inside `generate_trades`, not in the hot matching loop.

### Market Data

`BBO` — best bid/ask price and size. Updated on insert, cancel, and fill. O(1) read.

`L2Snapshot` — aggregated depth up to `D` levels. Bids descending, asks ascending. O(D).

`TradeEvent` — immutable fill record. Carries engine timestamp (monotonic nanoseconds) and wall-clock timestamp (UTC nanoseconds).

---

## Data Flow

```text
process_order(order)
    │
    ├─ process_limit_order
    │       matching_loop ──► generate_trades ──► TradePublisher
    │           │                                      │
    │           └─ check_stop_orders ◄─────────────────┘
    │                   │
    │                   └─ triggered stops → process_market/limit_order
    │
    ├─ process_market_order  ─► matching_loop
    ├─ process_ioc_order     ─► matching_loop
    ├─ process_fok_order     ─► can_fully_fill? → matching_loop
    └─ process_stop_order    ─► pending_stops.push_back
```

---

## Design Decisions

### `std::map` for price levels

Maintains sorted order so best-price retrieval is always a single `begin()` or `prev(end())` call. Insertion is O(log P) over price levels, not orders. The trade-off is tree-based pointer chasing and reduced cache locality. This is the first expected scalability bottleneck at very high throughput; replacement with a flat cache-friendly structure is tracked.

### `double` for prices

Simplifies simulation and testing. The risk is floating-point precision drift causing comparison instability — for example, two prices that should be equal hashing to different `std::map` keys. Acceptable at current scale. Production systems should use fixed-point integer ticks (`int64_t`).

### FOK pre-scan over rollback

FOK pre-scans the book to verify fillability before touching any state. The alternative — match then roll back on failure — is error-prone and requires either state copying or a transaction log. The ~2× traversal cost is accepted for correctness and simplicity.

### `pending_stops` as a public `vector` on `OrderBook`

`MatchingEngine` needs to scan and mutate the stop list after every fill. Ownership lives on `OrderBook` because it holds all resting state. The coupling is real and acknowledged — `MatchingEngine` reaches into `OrderBook`'s internals directly. The planned fix is a `StopOrderManager` with a clean interface.

### Stop trigger scan is O(S)

After every fill, `check_stop_orders` iterates all pending stops linearly. This is correct and simple, but degrades as S grows. A sorted index keyed on `stop_price` would reduce per-fill trigger evaluation to O(log S + T) where T is the number of triggered orders.

### Storing all trades in `engine.trades`

Provides an in-process audit trail with no external dependency. Unbounded memory growth during long sessions is the downside. Production replacement: stream via `TradePublisher`, evict from memory, use a ring buffer with a configurable cap.

### Single-symbol design

`MatchingEngine` holds a direct reference to one `OrderBook`. The migration path to multi-symbol is straightforward: replace the direct reference with a `std::unordered_map<string, OrderBook>` and add a symbol-routing step before dispatch.