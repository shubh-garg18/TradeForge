# How to Explain This Project — All Formats

Six ways to describe SystemDev, calibrated to who you're talking to and
how much time you have. Each is meant to be internalized and rephrased,
not read aloud.

---

## 1. One-Line Pitch (for resume / LinkedIn / "what's that repo?")

> A high-performance limit order book matching engine in C++20 with
> price-time priority, six order types, and a single-writer event loop
> for lock-free matching.

*Why this works:* every noun is concrete. "Limit order book" signals
finance/exchange context. "C++20" signals systems. "Price-time priority"
signals you know the domain. "Single-writer event loop" signals
concurrency awareness. No marketing fluff.

---

## 2. 30-Second Elevator Pitch (casual conversation, non-interview)

> I built a matching engine in C++ — basically the piece of a stock
> exchange that pairs up buy and sell orders. It handles six different
> order types, matches them by price and then by arrival time, and runs
> behind a thread-safe event queue so multiple clients can submit at
> once without corrupting the book. It's got about 170 assertions
> covering everything from basic limits to stop-loss triggering.

*Use this for:* recruiter phone screen, casual "what are you working
on?" chats, LinkedIn messages.

---

## 3. 90-Second Interview Walkthrough (MEMORIZE THIS)

When the interviewer says "walk me through your project," run this
verbatim. Times are approximate.

**(10s)** "I built a limit order book matching engine in modern C++.
It's a simplified version of what sits at the core of a stock exchange."

**(20s)** "It supports six order types and price-time priority matching.
The book is a `std::map` of price to intrusive linked lists — the map
gives me ordered best-bid and best-ask in log time, and the intrusive
list gives me O(1) cancel given an order pointer, which I look up in a
hash map by order ID."

**(20s)** "To support concurrent clients, the engine runs behind a
thread-safe event queue. Any number of producer threads can submit
orders, and a single worker thread owns all the matching state. That
removes the need for locks inside the hot path — classic single-writer
pattern."

**(20s)** "On top of that I have volume-tiered maker-taker fees,
trade publishing through an abstract interface so I can swap sinks,
and an L2 market-data snapshot."

**(15s)** "It ships with 166 test assertions covering every order type,
partial fills, global invariants, and the async event dispatch.
CMake-based build, C++20."

**(5s)** "Happy to dive into any piece."

**Key property of this script:** every sentence plants a hook the
interviewer can pull. Data-structure choice, concurrency pattern,
test discipline, build system. You control which hook they grab.

---

## 4. 5-Minute Deep Dive (for "tell me more")

Use this structure when the interviewer invites a longer explanation or
a follow-up like "go deeper on the matching loop."

1. **Domain context (45s)** — What an exchange matching engine does,
   why price-time priority is the standard, who uses one.

2. **Data structures (90s)** — `std::map<double, PriceLevel*>` for
   ordered price levels, intrusive doubly-linked list inside each
   PriceLevel for FIFO, `std::unordered_map<string, Order*>` for
   O(1) cancel-by-ID. Cached `best_bid`/`best_ask` pointers refreshed
   on every structural change give O(1) BBO reads.

3. **Order types (60s)** — Limit rests on the book. Market sweeps.
   IOC fills what it can and cancels the rest. FOK all-or-nothing via
   pre-scan. Stop-loss and stop-limit triggered by trade price, then
   promoted to market/limit respectively.

4. **Concurrency (60s)** — Thread-safe `EventQueue` with `std::mutex`
   and `std::condition_variable`. N producers, 1 consumer. All
   matching state lives inside the consumer's stack frame — no locks
   in the hot path.

5. **Testing (30s)** — 15+ named test functions, 166 assertions,
   including an async multi-threaded test that hammers the queue from
   producers and asserts the worker's final state.

6. **What's next (30s)** — REST API wrapper, multi-symbol routing,
   CI, structured logging. Then optional: WAL and an LLM tool-use
   frontend.

---

## 5. Bullet Summary (for README / resume)

### What it is

- C++20 limit order book matching engine
- Price-time priority, 6 order types (LIMIT, MARKET, IOC, FOK, STOP_LOSS, STOP_LIMIT)

### How it works

- `std::map<double, PriceLevel*>` for ordered price levels → O(log P) insert, O(1) BBO access
- Intrusive doubly-linked list inside each `PriceLevel` → O(1) enqueue & cancel
- `std::unordered_map<string, Order*>` for order-ID lookup → O(1) cancel-by-ID
- Thread-safe `EventQueue` (mutex + condition_variable) → N producers, 1 consumer
- Single-writer matching loop → zero locks on the hot path
- Abstract `TradePublisher` interface → swappable sinks (in-memory for tests, streaming for prod)
- Volume-tiered maker-taker fee engine

### Quality

- 166 assertions across 15+ named test functions
- CMake modular build (core, utils, io, fee_calculator)
- `-Wall -Wextra -Wshadow -Wconversion -Wpedantic` clean
- Documented complexity audit per operation

---

## 6. Audience-Tailored Versions

### For a non-technical recruiter / HR

> A stock exchange has something called a matching engine — the piece
> of software that pairs up buyers and sellers in the right order. I
> built one in C++ as a learning project. It has about 170 automated
> tests and handles concurrent clients safely.

### For an SDE peer interviewer

*Use the 90-second walkthrough (§3). They'll follow the hooks.*

### For a hiring manager / less-technical manager

> It's a C++ project that simulates the core of an exchange. The
> interesting parts are that it's concurrent — multiple clients can
> submit orders safely — and that I've been careful about data
> structures, so operations like cancel are constant-time. It's well
> tested and I'm extending it to expose a REST API next.

### For a systems / infrastructure interviewer

> Event-driven, single-writer architecture — producer threads push
> onto a thread-safe queue, a single consumer owns all mutable state.
> Data structures chosen for amortized O(1) on the critical paths:
> intrusive lists for FIFO, hash map for cancel-by-ID, `std::map` for
> ordered price levels with cached best pointers. No locks in the
> matching hot path.

---

## 7. Mental Model / Diagram (draw on a whiteboard)

```mermaid
   clients
     │ (many)
     ▼
 ┌──────────┐           producer-consumer
 │   API    │           boundary lives here
 └────┬─────┘
      │push
      ▼
 ┌──────────────┐
 │ EventQueue   │  ← mutex + cv, thread-safe
 └──────┬───────┘
        │pop (single worker)
        ▼
 ┌────────────────────────────┐
 │  MatchingEngine            │
 │   ┌─────────────────────┐  │
 │   │ OrderBook           │  │
 │   │   bids: map<p,level>│  │ ← best bid = rbegin()
 │   │   asks: map<p,level>│  │ ← best ask = begin()
 │   │   by_id: hash<id,*> │  │ ← O(1) cancel
 │   │   level = intrusive │  │ ← O(1) enqueue/remove
 │   └─────────────────────┘  │
 │   FeeCalculator (tiers)    │
 │   TradePublisher*          │  → trade stream out
 └────────────────────────────┘
```

Practice drawing this in 60 seconds with a marker in hand. If you can
draw and narrate simultaneously, you've won the architectural round.

---

## 8. Analogies (when the interviewer isn't a domain expert)

- **Matching engine** = like a waitlist for seats at a restaurant,
  except there are two sides (people willing to pay X, people willing
  to sell for Y) and they're matched when their prices overlap.

- **Price-time priority** = at the same price, first-come-first-served.

- **Intrusive linked list** = the list nodes aren't separate wrappers;
  each item carries its own prev/next pointers, like members of a
  club wearing the membership chain themselves instead of the chain
  being held separately.

- **Single-writer event loop** = a single receptionist fielding calls
  from many phones. Anyone can call in, only one person writes to the
  shared notebook.

---

## 9. Quick-Revise Cheat Sheet (read before walking into the interview)

**Elevator:** C++20, limit order book, 6 order types, price-time
priority, thread-safe event queue, single-writer loop.

**Data structures (cold answers):**

- Bids/asks: `std::map<double, PriceLevel*>` — O(log P) insert, O(1) BBO
- Level internals: intrusive doubly-linked list — O(1) enqueue/cancel
- Order lookup: `std::unordered_map<string, Order*>` — O(1) cancel

**Concurrency (cold answers):**

- `EventQueue`: `std::mutex` + `std::condition_variable`
- Pattern: N producers → 1 consumer
- Hot path: zero locks, single-writer

**Numbers:**

- 166 assertions
- 15+ test functions
- ~2,245 LOC

**Signature phrases to use:**

- "price-time priority"
- "single-writer event loop"
- "intrusive linked list"
- "amortized O(1) on the hot path"
- "abstract TradePublisher interface" → "strategy pattern"
- "producer-consumer"

**Do NOT say:**

- "It's a high-frequency trading system" — it isn't
- "Lock-free" — the queue uses a mutex
- "Production-ready" — it's a portfolio project
- "Blazing fast" — you haven't benchmarked it
