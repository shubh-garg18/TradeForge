# Performance Audit

## Overview

This document covers the time complexity, memory behavior, and hot path characteristics of the matching engine core. It does not cover I/O or persistence layers.

Goals:
- Prove correctness of performance assumptions
- Identify hot paths and known bottlenecks
- Establish a baseline for future optimization work

---

## C.1 Time Complexity

### Limit Order Insertion (non-crossing)

**Path:** `process_limit_order → matching_loop (0 iterations) → insert_limit`

| Step                              | Cost     |
| --------------------------------- | -------- |
| `std::map` price level lookup     | O(log P) |
| FIFO append within level          | O(1)     |
| BBO pointer refresh               | O(log P) |

**Total: O(log P)** — where P is the number of active price levels, not the number of orders.

---

### Matching Loop (aggressive orders)

**Path:** `matching_loop → iterate crossed levels → consume FIFO orders`

Let L = crossed price levels, K = resting orders consumed.

| Step                         | Cost |
| ---------------------------- | ---- |
| Per-level cross check        | O(L) |
| Per-order fill and FIFO pop  | O(K) |
| BBO pointer refresh per fill | O(log P) amortized per level removal |

**Total: O(L + K)** — theoretical lower bound for price-time priority matching.

Key properties:
- No scanning of irrelevant levels
- FIFO removal is O(1) via intrusive linked list
- No unbounded heap allocation inside the matching loop (the only growth is the amortized O(1) trade-record append)

---

### Order Type Summary

| Type   | Extra cost       | Total      |
| ------ | ---------------- | ---------- |
| MARKET | none             | O(L + K)   |
| IOC    | none             | O(L + K)   |
| FOK    | pre-scan + match | O(L + K)   |

FOK performs a full liquidity pre-check before matching, resulting in roughly 2× traversal, but stays linear. This avoids rollback complexity entirely.

---

### Cancel Order

**Path:** `unordered_map lookup → intrusive unlink → price level cleanup`

| Step                     | Cost     |
| ------------------------ | -------- |
| Hash map lookup          | O(1)     |
| Intrusive list unlink    | O(1)     |
| Level removal if empty   | O(log P) |
| BBO pointer refresh      | O(log P) |

**Total: O(1)** for the common case (level not emptied). O(log P) when the cancel empties a price level.

---

### BBO (Best Bid/Offer)

**Read:** O(1) — cached `best_bid` / `best_ask` pointers returned directly.

**Write:** O(log P) — pointers are refreshed via `prev(bids.end())` and `asks.begin()` on every structural operation: insert, cancel, and fill-driven level removal. The O(1) read guarantee holds only because writes pay the log P cost eagerly.

---

### L2 Snapshot

**Total: O(D)** — where D is the requested depth. Iterates bids in descending order and asks in ascending order up to D levels each.

---

### Stop Order Trigger Scan

After every fill, `check_stop_orders` scans `pending_stops` linearly.

| Step                                         | Cost |
| -------------------------------------------- | ---- |
| Scan all pending stops                       | O(S) |
| Erase triggered entries from vector          | O(S) per triggered order |
| Execute triggered order (MARKET or LIMIT)    | O(L + K) per order |

**Total per fill: O(S + T × (L + K))** — where S = pending stop count, T = triggered stops.

This is the primary scalability concern at high stop-order counts. A sorted index keyed on `stop_price` would reduce per-fill scan to O(log S + T).

---

## C.2 Hot Path Walk-Through

The most frequently executed path:

```
matching_loop
  └─ get_best_opposite       ← cached pointer, O(1)
  └─ cross check             ← single comparison
  └─ level->get_head_order   ← intrusive list head, O(1)
  └─ fill_quantity (×2)      ← integer subtract + status update
  └─ level->reduce_quantity  ← integer subtract
  └─ generate_trades         ← fee lookup + vector append
  └─ level->remove_order     ← intrusive unlink, O(1)
  └─ remove_price_level      ← map erase + BBO refresh, O(log P) if level emptied
```

**Memory access pattern:** PriceLevel pointers are stable. FIFO list traversal within a level is sequential. The only container growth on the path is the trade-record append, which is amortized O(1).

The loop is designed for strong cache locality and minimal branching on the critical path.

---

## C.3 Hidden Costs & Edge Cases

### `std::map` for price levels

Tree-based pointer chasing hurts cache locality. Every `begin()` / `prev(end())` call after a structural operation walks the red-black tree. Acceptable at current scale; likely the first bottleneck at very high throughput.

**Planned fix:** replace with a cache-friendly flat structure and a hash index for non-best-price lookups.

### `double` price keys

Floating-point precision drift can cause comparison instability — two logically equal prices hashing to different map keys. Acceptable for simulation. Production systems should use fixed-point integer ticks (`int64_t`).

### FOK pre-scan

Intentional 2× traversal. The alternative — match then roll back on failure — introduces state mutation risk and higher implementation complexity. Current approach is correct and simple.

### `pending_stops` as a vector

Linear scan and erase. Correct for small S; degrades at scale. Also, `MatchingEngine` accesses `OrderBook::pending_stops` directly — a coupling that should be encapsulated behind a `StopOrderManager` interface.

### Unbounded `engine.trades` vector

Grows without bound during long sessions. Production systems should stream trades externally via `TradePublisher` and use a ring buffer with a configurable cap in memory.

---

## C.4 Performance-Critical Invariants

The following must hold for the engine to meet its complexity guarantees:

- No empty price levels retained after fill or cancel
- FIFO order enforced via intrusive linked list within each level
- No unbounded heap allocation inside `matching_loop` (trade append is amortized O(1))
- No book mutation on FOK failure (pre-scan only)
- BBO pointers refreshed on every structural operation
- `pending_stops` scan happens only after a fill, never inside `matching_loop`

Breaking any of these invalidates the complexity claims above.

---

## C.5 Summary Table

| Operation          | Complexity          | Notes                                  |
| ------------------ | ------------------- | -------------------------------------- |
| Limit insert       | O(log P)            | P = active price levels                |
| Market / IOC match | O(L + K)            | L = levels crossed, K = fills          |
| FOK                | O(L + K)            | ~2× traversal, no rollback             |
| Cancel             | O(1) / O(log P)     | O(log P) only if level emptied         |
| BBO read           | O(1)                | Cached pointer                         |
| BBO write          | O(log P)            | Paid on every structural op            |
| L2 snapshot        | O(D)                | D = requested depth                    |
| Stop trigger scan  | O(S + T × (L + K)) | S = pending stops, T = triggered       |

---

## C.6 Empirical Micro-Benchmark

The complexity bounds above are validated by `bench/Benchmark.cpp`, a
micro-benchmark that times the real hot path — `MatchingEngine::process_order`
— and nothing synthetic. Order allocation and book pre-load are excluded from
the timed region (except the dedicated end-to-end scenario). Each scenario
submits N orders and records per-order latency with `std::chrono::steady_clock`.

**Build & run:**

```
cmake -DCMAKE_BUILD_TYPE=Release ..   # bench compiles with -O3 -march=native
make bench
./bench 1000000                       # default N = 1,000,000
```

### Scenarios

| # | Scenario | What it isolates |
| - | -------- | ---------------- |
| 1 | Limit insert (resting, no match) | Pure book insertion at distinct levels — `std::map` insert O(log P) |
| 2 | Match 1:1 (full fill + level erase) | Each taker empties a one-deep level → worst-case map-erase churn |
| 3 | Match FIFO (deep single level) | All liquidity at one price → pure FIFO match + fee cost, no erase churn |
| 4 | End-to-end (alloc + match) | Same as #3 but order construction is inside the timed region |

### Results (N = 1,000,000)

Measured on a developer laptop (WSL2, single thread, Release `-O3 -march=native`).
Numbers are indicative, not a hardware spec sheet — treat them as relative
signal between scenarios.

| Scenario                            | orders/sec | p50 (µs) | p99 (µs) | mean (µs) |
| ----------------------------------- | ---------: | -------: | -------: | --------: |
| Limit insert (resting, no match)    |    ~951 K  |    0.65  |    3.19  |    1.05   |
| Match 1:1 (full fill + level erase) |    ~2.02 M |    0.36  |    1.08  |    0.50   |
| Match FIFO (deep single level)      |    ~5.03 M |    0.17  |    0.21  |    0.20   |
| End-to-end (alloc + match)          |    ~2.13 M |    0.25  |    2.34  |    0.47   |

### Reading the numbers

- **FIFO matching is the fastest path** (~5.03 M orders/sec, p50 0.17 µs):
  the deep single level stays a single `std::map` entry, so each match is
  pure O(1) intrusive-list head pops and fee lookups — no tree growth, no
  tree mutation, excellent cache locality. This is the engine at its best.
- **Limit insert is the slowest** (~951 K orders/sec, p50 0.65 µs): it is the
  one scenario whose `std::map` grows to a million distinct levels, so every
  insert is an O(log P) descent into an ever-larger red-black tree plus a BBO
  refresh, and the tree's pointer-chasing wrecks cache locality as it grows.
  This is the `std::map` cost flagged in §C.3 surfacing under the *largest*
  tree — exactly where the complexity model says it should hurt most.
- **Level-churn matching and end-to-end sit in between** (~2 M orders/sec):
  both touch the map per order, but on a working tree that is smaller and/or
  shrinking on the timed path rather than the million-node tree that limit
  insert builds up.
- p99 is tight on a clean run (≤ ~3.2 µs across all scenarios), but **max**
  latency reaches the tens of milliseconds — those are OS scheduling and
  page-fault outliers on a non-isolated laptop core, not engine behavior.
  Trust p50/p99, not max.
- Run-to-run variance is significant on a shared laptop core: throughput and
  the ranking *among the map-touching scenarios* shift between runs, and the
  level-churn p99 in particular swings widely depending on scheduler noise.
  The one stable result across every run is that FIFO is fastest by a wide
  margin.

The ranking — limit insert < level-churn ≈ end-to-end < FIFO — is consistent
with the complexity model: the more (and the larger) the `std::map` structural
operations a scenario drives, the more it costs, and the single-level path
that avoids tree growth and mutation wins decisively.
