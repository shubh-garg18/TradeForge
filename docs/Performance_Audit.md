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
