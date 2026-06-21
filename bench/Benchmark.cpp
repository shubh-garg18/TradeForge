/*
TradeForge — Matching Engine Micro-Benchmark

Measures the real hot path of the engine (MatchEngine::MatchingEngine::process_order),
nothing synthetic. Each scenario:
  1. Pre-builds a deep order book (setup, NOT timed).
  2. Pre-constructs every incoming order (allocation, NOT timed).
  3. Submits N orders, timing each process_order() call with steady_clock.

Reported per scenario:
  - throughput  : orders / second
  - p50 / p99   : per-order matching latency in microseconds
  - mean / max  : per-order matching latency in microseconds

The timed region contains only process_order(), which drives matching_loop(),
fee calculation (volume-tiered maker/taker), trade generation and book maintenance
(price-level insert/erase on std::map). Order allocation and book setup are excluded.

Build:  enabled as the `bench` target via CMake (Release -O3 -march=native recommended).
Run:    ./bench [N]            (default N = 1'000'000)
*/

#include "core/MatchingEngine.hpp"
#include "core/OrderBook.hpp"
#include "FeeCalculator/FeeCalculator.hpp"
#include "utils/TimeUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace MatchEngine;
using SteadyClock = std::chrono::steady_clock;

namespace {

struct Stats {
    std::string name;
    std::size_t n = 0;
    double ops_per_sec = 0.0;
    double p50_us = 0.0;
    double p99_us = 0.0;
    double mean_us = 0.0;
    double max_us = 0.0;
};

// latencies are per-order durations in nanoseconds; total_ns is their sum.
Stats summarize(std::string name, std::vector<double>& latencies_ns, double total_ns) {
    Stats s;
    s.name = std::move(name);
    s.n = latencies_ns.size();
    if (s.n == 0) return s;

    std::sort(latencies_ns.begin(), latencies_ns.end());

    auto pct = [&](double q) {
        std::size_t idx = static_cast<std::size_t>(q * static_cast<double>(s.n - 1));
        return latencies_ns[idx] / 1000.0; // ns -> us
    };

    s.p50_us = pct(0.50);
    s.p99_us = pct(0.99);
    s.max_us = latencies_ns.back() / 1000.0;
    s.mean_us = (total_ns / static_cast<double>(s.n)) / 1000.0;
    s.ops_per_sec = static_cast<double>(s.n) / (total_ns / 1e9);
    return s;
}

// ---------------------------------------------------------------------------
// Scenario 1: Limit-order insertion (no crossing) — pure book insertion O(log P).
// N resting BUY limits at distinct descending price levels, no asks present.
// ---------------------------------------------------------------------------
Stats bench_limit_insert(std::size_t N) {
    OrderBook book;
    FeeCalculator fees;
    MatchingEngine engine(book, fees);

    std::deque<Order> orders;
    for (std::size_t i = 0; i < N; ++i) {
        double price = 1.0 + static_cast<double>(i) * 0.01; // distinct levels
        orders.emplace_back("mm", std::to_string(i), Side::BUY, OrderType::LIMIT,
                            price, 1ULL, static_cast<TimeUtils::Timestamp>(i + 1));
    }

    std::vector<double> lat;
    lat.reserve(N);
    double total = 0.0;

    for (std::size_t i = 0; i < N; ++i) {
        Order* o = &orders[i];
        auto t0 = SteadyClock::now();
        engine.process_order(o);
        auto t1 = SteadyClock::now();
        double d = std::chrono::duration<double, std::nano>(t1 - t0).count();
        lat.push_back(d);
        total += d;
    }

    return summarize("Limit insert (resting, no match)", lat, total);
}

// ---------------------------------------------------------------------------
// Scenario 2: 1:1 matching with level churn.
// Pre-load N resting SELL limits at distinct ascending levels (each its own level),
// then submit N marketable BUY limits. Every taker fully fills one resting order,
// emptying its level -> std::map erase per match (worst-case book maintenance).
// ---------------------------------------------------------------------------
Stats bench_match_level_churn(std::size_t N) {
    OrderBook book;
    FeeCalculator fees;
    MatchingEngine engine(book, fees);
    engine.trades.reserve(N);

    // Resting liquidity (setup — not timed).
    std::deque<Order> resting;
    for (std::size_t i = 0; i < N; ++i) {
        double price = 1000.0 + static_cast<double>(i) * 0.01;
        resting.emplace_back("mm", "S" + std::to_string(i), Side::SELL, OrderType::LIMIT,
                             price, 1ULL, static_cast<TimeUtils::Timestamp>(i + 1));
        engine.process_order(&resting[i]);
    }

    // Incoming takers (allocation — not timed). Priced to always cross.
    std::deque<Order> takers;
    for (std::size_t i = 0; i < N; ++i) {
        takers.emplace_back("tk", "B" + std::to_string(i), Side::BUY, OrderType::LIMIT,
                            1e7, 1ULL, static_cast<TimeUtils::Timestamp>(N + i + 1));
    }

    std::vector<double> lat;
    lat.reserve(N);
    double total = 0.0;

    for (std::size_t i = 0; i < N; ++i) {
        Order* o = &takers[i];
        auto t0 = SteadyClock::now();
        engine.process_order(o);
        auto t1 = SteadyClock::now();
        double d = std::chrono::duration<double, std::nano>(t1 - t0).count();
        lat.push_back(d);
        total += d;
    }

    return summarize("Match 1:1 (full fill + level erase)", lat, total);
}

// ---------------------------------------------------------------------------
// Scenario 3: Deep single-level FIFO matching.
// Pre-load N resting SELL limits at one price (one deep FIFO level), then submit
// N marketable BUY limits. Each taker consumes the head order; the level survives
// until the last fill -> isolates pure FIFO match + fee cost (no map erase churn).
// ---------------------------------------------------------------------------
Stats bench_match_fifo(std::size_t N) {
    OrderBook book;
    FeeCalculator fees;
    MatchingEngine engine(book, fees);
    engine.trades.reserve(N);

    std::deque<Order> resting;
    for (std::size_t i = 0; i < N; ++i) {
        resting.emplace_back("mm", "S" + std::to_string(i), Side::SELL, OrderType::LIMIT,
                             1000.0, 1ULL, static_cast<TimeUtils::Timestamp>(i + 1));
        engine.process_order(&resting[i]);
    }

    std::deque<Order> takers;
    for (std::size_t i = 0; i < N; ++i) {
        takers.emplace_back("tk", "B" + std::to_string(i), Side::BUY, OrderType::LIMIT,
                            1e7, 1ULL, static_cast<TimeUtils::Timestamp>(N + i + 1));
    }

    std::vector<double> lat;
    lat.reserve(N);
    double total = 0.0;

    for (std::size_t i = 0; i < N; ++i) {
        Order* o = &takers[i];
        auto t0 = SteadyClock::now();
        engine.process_order(o);
        auto t1 = SteadyClock::now();
        double d = std::chrono::duration<double, std::nano>(t1 - t0).count();
        lat.push_back(d);
        total += d;
    }

    return summarize("Match FIFO (deep single level)", lat, total);
}

// ---------------------------------------------------------------------------
// Scenario 4: End-to-end ingest + match (allocation INCLUDED in timing).
// Same deep single-level book as the FIFO scenario, but each taker Order is
// constructed *inside* the timed region -> measures allocation + match together.
// ---------------------------------------------------------------------------
Stats bench_end_to_end(std::size_t N) {
    OrderBook book;
    FeeCalculator fees;
    MatchingEngine engine(book, fees);
    engine.trades.reserve(N);

    std::deque<Order> resting;
    for (std::size_t i = 0; i < N; ++i) {
        resting.emplace_back("mm", "S" + std::to_string(i), Side::SELL, OrderType::LIMIT,
                             1000.0, 1ULL, static_cast<TimeUtils::Timestamp>(i + 1));
        engine.process_order(&resting[i]);
    }

    std::deque<Order> takers; // kept alive; growth cost is part of the measurement
    std::vector<double> lat;
    lat.reserve(N);
    double total = 0.0;

    for (std::size_t i = 0; i < N; ++i) {
        auto t0 = SteadyClock::now();
        takers.emplace_back("tk", "B" + std::to_string(i), Side::BUY, OrderType::LIMIT,
                            1e7, 1ULL, static_cast<TimeUtils::Timestamp>(N + i + 1));
        engine.process_order(&takers.back());
        auto t1 = SteadyClock::now();
        double d = std::chrono::duration<double, std::nano>(t1 - t0).count();
        lat.push_back(d);
        total += d;
    }

    return summarize("End-to-end (alloc + match)", lat, total);
}

void print_row(const Stats& s) {
    std::cout << std::left << std::setw(38) << s.name << std::right
              << std::setw(14) << std::fixed << std::setprecision(0) << s.ops_per_sec
              << std::setw(12) << std::setprecision(3) << s.p50_us
              << std::setw(12) << std::setprecision(3) << s.p99_us
              << std::setw(12) << std::setprecision(3) << s.mean_us
              << std::setw(12) << std::setprecision(3) << s.max_us
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    std::size_t N = 1'000'000;
    if (argc > 1) {
        long v = std::atol(argv[1]);
        if (v > 0) N = static_cast<std::size_t>(v);
    }

    std::cout << "TradeForge matching-engine micro-benchmark\n";
    std::cout << "Orders per scenario (N) = " << N << "\n";
    std::cout << "Clock: std::chrono::steady_clock (per-order timing)\n\n";

    // Warm-up (not reported): stabilizes caches / branch predictor.
    { auto w = bench_match_fifo(std::min<std::size_t>(N, 50'000)); (void)w; }

    std::vector<Stats> results;
    results.push_back(bench_limit_insert(N));
    results.push_back(bench_match_level_churn(N));
    results.push_back(bench_match_fifo(N));
    results.push_back(bench_end_to_end(N));

    std::cout << std::left << std::setw(38) << "Scenario" << std::right
              << std::setw(14) << "orders/sec"
              << std::setw(12) << "p50 (us)"
              << std::setw(12) << "p99 (us)"
              << std::setw(12) << "mean (us)"
              << std::setw(12) << "max (us)" << '\n';
    std::cout << std::string(38 + 14 + 12 * 4, '-') << '\n';

    for (const auto& s : results) print_row(s);

    std::cout << "\nNote: timed region = process_order() only (full matching loop,\n"
                 "volume-tiered fee calc, trade generation, book maintenance).\n"
                 "Order allocation and book pre-load are excluded from timing.\n";
    return 0;
}