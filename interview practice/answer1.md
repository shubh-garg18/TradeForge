# Interview Answers — Current Project State

Answers to every question in `question1.md`. These are written as *how a
candidate internalizes the answer* — prose, not a monologue script. Read
them, absorb the shape, rephrase in your own words on the day.

Each answer is calibrated to be:

- **Truthful** — never claim something you didn't build
- **Scoped** — 2–4 sentences, not a lecture
- **Follow-up-ready** — plants hooks the interviewer can pull on

---

## Thread 1 — Project Walkthrough & Data-Structure Choices

**Q1. Walk me through your project.**

I built a limit order book matching engine in modern C++20. It's a
simplified version of the core piece of a stock exchange: it accepts
orders, matches them by price-time priority, produces trades, and
publishes market data. It supports six order types — limit, market, IOC,
FOK, stop-loss, and stop-limit — and it runs behind a thread-safe event
queue so multiple clients can submit concurrently.

**Q2. Why `std::map` for price levels and `std::unordered_map` for the
order-ID lookup?**

They solve different problems. For price levels I need *ordered* access —
best bid is the highest key, best ask is the lowest — and ordered
iteration for the L2 snapshot. `std::map` is a balanced BST, so I get
that for O(log N) insert and O(1) on `rbegin()` / `begin()`. For order
cancellation I just need to find an order by its ID — no ordering needed
— so a hash map gives me O(1) amortized.

**Q3. Complexity of inserting a new price level vs. an existing one?**

New level: O(log P) where P is the number of distinct price levels, because
`std::map` has to walk the tree. Existing level: O(1), because I already
have a pointer to the `PriceLevel` and I'm just appending to its
intrusive linked list at the tail.

**Q4 [CS]. How is `std::map` implemented? Does the standard mandate it?**

The standard doesn't *mandate* red-black trees, but it mandates the
performance characteristics — logarithmic insert/find/erase and ordered
iteration — and every major implementation uses a red-black tree
because it hits those bounds with reasonable constants. Alternatives
like AVL trees would also satisfy the standard but no mainstream STL
uses them.

**Q5. How do the cached `best_bid` / `best_ask` pointers stay correct?**

They're refreshed on every structural change. When a new price level is
inserted, I compare it against the cached best and update if it's more
aggressive. When a level is emptied (fully consumed in a match, or last
order cancelled), I pop it from the map and grab the next-best via
`rbegin()` or `begin()`. So reads are O(1), but the amortized cost is
paid at write time.

**Q6 [SHARP]. What happens to `best_bid` if the entire best-bid level
gets consumed?**

The match loop notices the `PriceLevel` is now empty, removes it from
the bids map, and then re-points `best_bid` to the new top of the map
via `bids.rbegin()`. If the map is empty after that, `best_bid` becomes
null and a subsequent order sees "no bids." The test suite covers this
case explicitly because it's a nasty source of bugs in matching engines.

---

## Thread 2 — Cancel-Order Flow

**Q1. A cancel arrives. Walk me through.**

The cancel event gets pushed to the event queue. When the worker picks
it up, it looks the order up by ID in the `unordered_map<string, Order*>`.
That's an O(1) hit. Given the `Order*`, I know which `PriceLevel` it's
in and where it sits in that level's intrusive list. I splice it out of
the list in O(1), erase its hash-map entry, and if that level became
empty I pop it from the price map.

**Q2. Where does the O(1) come from?**

Two separate O(1) steps. Hashing the order ID gives me the `Order*`
directly. The `Order` itself carries `prev`/`next` pointers into its
enclosing list — that's the intrusive part — so removing it from the
list is pointer rewiring, no search.

**Q3. What if the order ID doesn't exist?**

The hash lookup misses, I return a "cancel rejected — unknown order"
result. No state mutation, no exception. It's the same code path a
client would hit if they cancelled an already-filled order.

**Q4 [CS]. Hash collisions and worst-case complexity of `unordered_map`?**

The standard uses separate chaining — each bucket is a linked list (or
recently, can be a small list). If N orders all hash to the same bucket,
lookup degrades to O(N). In practice with a decent hash function and a
reasonable load factor (under 1.0 by default), collisions stay bounded
and lookup is O(1) amortized. For adversarial inputs you'd want a
hash-DoS-resistant hash.

**Q5. How do you remove the order from the `PriceLevel`'s list in O(1)?**

Because the list is intrusive — the `Order` struct *itself* contains
`prev` and `next` pointers, and the `PriceLevel` holds `head`/`tail`
pointers. Removal is just: `order->prev->next = order->next` and the
symmetric update on `order->next->prev`. No traversal.

**Q6 [CS]. What is an intrusive linked list, and why over `std::list<Order>`?**

`std::list<Order>` stores each Order inside a separately-allocated node
that *contains* prev/next pointers plus the Order. You pay an extra heap
allocation per order, and iteration is cache-unfriendly. In an intrusive
list, the Order itself *is* the node — prev/next live inside Order.
You allocate once, pointer arithmetic is simpler, and you can hold a
single stable `Order*` that lets you both look up the order (by ID) and
splice it out of its containing list in O(1). That's the key property
for this engine.

---

## Thread 3 — Concurrency Model

**Q1. Multiple clients submit concurrently — how without races?**

I use a classic producer-consumer pattern. Any number of threads can
push `EngineEvent` objects into a thread-safe `EventQueue`. A single
worker thread pops them and runs all the matching logic. The queue
itself uses a mutex and a condition variable; everything *downstream*
of the queue is single-threaded, so there's no shared mutable state
inside the matching loop.

**Q2. Why a single worker thread — aren't you wasting cores?**

The matching logic mutates the order book — shared mutable state. If
multiple threads matched concurrently I'd need locks on every bid, ask,
and price level, which kills both latency and code simplicity.
Funneling all mutations through one thread eliminates that whole class
of races. The tradeoff is that per-book throughput is capped by one
core. For this project that's fine; to scale I'd shard by symbol and
run one worker per symbol, which gives linear scaling without adding
locks.

**Q3. EventQueue is empty and an order arrives — walk me through.**

Worker is blocked in `cv.wait(lock, predicate)` — the OS has parked it.
Producer acquires the mutex, pushes the event onto the deque, releases
the mutex, calls `cv.notify_one()`. The kernel wakes the worker, it
re-acquires the mutex, the predicate is now true, it pops the event,
releases the mutex, and runs the matching logic.

**Q4 [CS]. What does `cv.wait` do at the OS level?**

It atomically releases the mutex and blocks the thread in a kernel-level
wait queue — on Linux that's typically implemented via futex. The
thread is *not* spinning; it consumes zero CPU until the kernel wakes
it. When notified, it's placed back on the runqueue and competes with
other threads for a core.

**Q5 [CS]. Spurious wakeup — what and how do you guard?**

A spurious wakeup is when `cv.wait` returns even though no one called
`notify`. The C++ and POSIX standards both allow it for implementation
reasons. You guard against it by always calling `wait` with a predicate
— `cv.wait(lock, [&]{ return !queue.empty(); })` — which rechecks the
condition on every wakeup and goes back to sleep if it's false.

**Q6 [SHARP]. Two producers push() at the exact same moment.**

Both threads contend for the mutex inside `push`. One wins — the other
blocks in `lock()`. The winner appends to the deque and releases. Then
the loser runs the same code. Both events end up on the queue, in
well-defined (though arbitrary) order. The worker consumes them
sequentially in whatever order they landed on the deque.

**Q7. 100k orders/sec — does this architecture hold?**

Per single book, probably not — one worker thread becomes the bottleneck
at that rate. The fix is per-symbol sharding: one book, one event
queue, one worker thread per symbol. That scales linearly across cores
for well-distributed symbols. If a single symbol is hot, that's a
harder problem and would need either splitting the book by side or
accepting the per-core throughput ceiling.

---

## Thread 4 — FOK vs IOC Semantics

**Q1. IOC vs FOK.**

IOC — Immediate or Cancel — fills whatever it can right now, cancels
the rest, never rests on the book. FOK — Fill or Kill — either fills
*entirely* right now or cancels the whole thing, never partial.

**Q2. How do you implement FOK's all-or-nothing?**

With a pre-scan. Before mutating anything, I walk the opposite side of
the book and accumulate available quantity at prices that satisfy the
order. If the accumulated quantity is strictly less than the order's
quantity, I reject immediately — no mutations at all. Only if the
pre-scan succeeds do I actually execute the fills.

**Q3. Why not try to fill and roll back if partial?**

Rollback requires remembering every state change and undoing it — that's
a transaction log per FOK order. Pre-scan is simpler, correct by
construction, and avoids the bug surface of rollback code. I traded
some CPU (we traverse roughly twice on success) for much simpler
correctness.

**Q4. Extra cost of pre-scan?**

Roughly 2x traversal on successful FOK. On rejected FOK it's 1x and no
mutation. Given how infrequent FOK is in practice, this is well worth
the simplicity gain.

**Q5 [SHARP]. Could FOK partially execute?**

No — and the test suite has an explicit case that asserts it. The
pre-scan and the execution are both inside the single worker thread, so
nothing can interleave between the check and the action. If the
pre-scan fails, no fills happen; if it succeeds, every planned fill
executes because the book can't change between the scan and the match.

---

## Thread 5 — Stop Orders

**Q1. How do stop-loss and stop-limit work?**

A stop order sits dormant in a `pending_stops` vector until a trade
happens at (or past) its trigger price. On trigger, a stop-loss converts
into a market order and starts matching immediately; a stop-limit
converts into a limit order and either matches or rests.

**Q2. When does a pending stop trigger?**

After every trade, I scan `pending_stops` and check each one's trigger
condition against the trade price. Any stops that fire get converted
and injected into the matching path in the same worker loop, so they
see a consistent book state.

**Q3. Complexity?**

O(S) per trade, where S is the number of pending stops. That's a
linear scan. Fine for dozens to hundreds of stops, not fine for
thousands.

**Q4. How would you improve?**

Keep pending stops in two sorted structures — one indexed by trigger
price for buy stops (lowest first above current price), one for sell
stops (highest first below current price). After a trade at price P,
only scan the triggered range, which is O(log S + K) where K is the
number of actually triggered stops. A `std::multimap` keyed by trigger
price does the job.

**Q5 [SHARP]. Why haven't you done that?**

Honest answer: it's on my list and I prioritized shipping correctness
and breadth over this one algorithmic improvement. The current design
is correct and my test suite covers it; the optimization is a drop-in
replacement when it's needed.

---

## Thread 6 — Testing Strategy

**Q1. How do you test a matching engine?**

I test at three layers. Per order type — assertions that each order
behaves correctly in isolation. Per scenario — cancel-after-partial-
fill, FOK-not-fillable, stop triggered mid-match. Per invariant — after
any sequence of operations, the book must satisfy global properties
like "sum of open quantity at bids equals sum of all unfilled buys."
That last layer catches subtle bugs that per-order tests miss.

**Q2. 166 assertions — how organized?**

Fifteen-plus named test functions, each focused on a feature or
invariant: `test_limit`, `test_fok`, `test_cancel_partial`,
`test_global_invariants`, `test_event_queue_async`, and so on. There's
also a combined full-system test that runs a realistic mixed workload.

**Q3. Do you test the multi-threaded event queue?**

Yes — there's a specific async test that spawns producer threads pushing
random orders and asserts the worker processes all of them in the
expected end state. It doesn't try to prove correctness of the queue
primitive itself — that would need fuzzing — but it does prove the
integration works under concurrent submission.

**Q4 [CS]. Flaky tests, and why concurrent ones are prone to it.**

A flaky test is one that passes sometimes and fails sometimes without
the code changing — the test is non-deterministic. Concurrent tests
are prone because the OS scheduler doesn't give reproducible orderings;
if the test relies on a particular interleaving, it flakes. The fix is
either to control ordering explicitly (with barriers) or to only assert
properties that hold for *any* valid interleaving.

**Q5 [SHARP]. Testing FOK-that-can't-fill with no side effects.**

I place some limit orders, snapshot the book state, submit an oversized
FOK, assert it's rejected, then snapshot the book again and assert it
matches the earlier snapshot exactly. That confirms nothing mutated.

---

## Thread 7 — C++ Memory & Ownership

**Q1. Who owns Order objects?**

The `OrderBook` owns them. When a new order arrives, the book
heap-allocates an `Order`, inserts it into the hash map by ID and into
the intrusive list at the price level. When it's fully filled or
cancelled, the book removes it from both structures and deletes it.
Nothing outside the book holds a long-lived pointer.

**Q2. Why raw pointers?**

Performance and simplicity, honestly. `shared_ptr` adds atomic refcount
overhead on every copy, which on a matching hot path is measurable.
`unique_ptr` would be a reasonable fit for book-owned orders, but
since the book has strict ownership discipline — orders live from
insert to fill/cancel, and the book always knows — raw pointers kept
the code simpler. For an SDE interview, I'd call this a pragmatic
choice with a known tradeoff.

**Q3. When does an Order get deleted?**

Either the moment it's fully filled (all its quantity matched) or the
moment it's cancelled. In both cases the book unlinks it from the
hash map and the price-level list, then calls `delete` on the pointer.

**Q4 [CS]. RAII.**

Resource Acquisition Is Initialization. The idea: resource lifetimes
are tied to object lifetimes. You acquire in the constructor, release
in the destructor. Stack unwinding during exceptions then guarantees
cleanup without explicit `finally` blocks. `std::lock_guard`,
`std::unique_ptr`, and `std::fstream` are canonical examples.

**Q5 [CS]. Rule of 0/3/5. Which does `Order` follow?**

Rule of 3: if you define one of (destructor, copy constructor, copy
assignment), you usually need all three. Rule of 5 extends to move
constructor and move assignment. Rule of 0: prefer to not define any
of them — let the compiler-generated ones work, which happens when
your members are all RAII types. My `Order` is rule-of-0: it holds
POD fields and raw pointers it doesn't own (prev/next into the
containing list), so the defaults are correct.

**Q6 [SHARP]. Any double-delete or UAF risk?**

The invariant is that every `Order*` has exactly one owner — the
`OrderBook` — and is only deleted when unlinked from both the hash map
and the list. The risk would be if two events for the same order ID
tried to delete it concurrently, but since every mutation runs on the
single worker thread, that can't happen. A cancel for an already-gone
order is handled by the hash-lookup miss path and doesn't delete.

---

## Thread 8 — Matching Algorithm

**Q1. Price-time priority — why is it fair?**

Best price goes first; within the same price, earliest-arrived goes
first. That means price is the primary fairness dimension — nobody gets
filled at a worse price than a counterparty willing to trade at a
better one. Time-priority within a price breaks ties in a way that
rewards being first, which incentivizes liquidity provision.

**Q2. Market buy of 1000 shares — walk through.**

I look at the top of the asks side — lowest-price level. I take orders
from its FIFO list head until either the buy is fully filled or the
level is exhausted. If the level's exhausted and the buy still has
quantity, I pop the level from the asks map, move to the next-best
ask level, and repeat. For each filled order (fully or partially), I
emit a `TradeEvent` with the counterparty info and fees. When the buy
is done, or the asks side is empty, matching ends.

**Q3. Complexity?**

Per trade event: O(1) — FIFO head access, constant-time list
manipulation. Per price-level exhausted: O(log P) to pop from the map
and get the next best. Overall, matching a big order against K price
levels is O(K log P + F) where F is total fills.

**Q4 [SHARP]. Can orders at worse prices starve?**

Not in a typical sense — an order at a worse price will eventually
become "best" once better-priced orders are consumed or cancelled.
There's no mechanism that permanently prevents a resting order from
being filled as long as the counterparty side reaches its price. It's
fair-share by construction of price-time priority.

**Q5. How are maker/taker fees decided during matching?**

The resting order is the maker, the incoming order is the taker. Each
counterparty's fee rate depends on their rolling notional volume —
higher volume, lower rate, in tiered steps. The `FeeCalculator`
consults the current tier for each user and stamps the fee onto the
`TradeEvent` at emission time.

---

## Thread 9 — Trade Publishing & Market Data

**Q1. How do consumers hear about trades?**

Every fill produces a `TradeEvent` and hands it to an implementation of
the `TradePublisher` interface. In tests, an `InMemoryTradePublisher`
collects them into a vector so asserts can inspect. In production,
you'd plug in a publisher that streams to Kafka, a websocket feed,
whatever.

**Q2. Why an abstract publisher interface?**

Decoupling. The matching engine doesn't care *how* trades are
published — it just calls `publish()`. Swapping in a new sink is a
one-class change, and tests get a trivial no-I/O implementation. It
also keeps the core engine free of dependencies on networking or
messaging libraries.

**Q3 [CS]. Design pattern?**

Strategy pattern, or arguably a dependency-inversion application:
the engine depends on the abstraction, concrete publishers depend on
the abstraction. Either framing is fine.

**Q4. L2 snapshot mechanics and cost?**

`get_l2_snapshot(depth D)` walks the bids and asks in best-to-worst
order, aggregating quantity per price level, and returns two vectors
of `(price, total_quantity)` up to D levels deep. Cost is O(D).

**Q5 [SHARP]. Polling L2 every millisecond — does the engine slow down?**

Yes, somewhat — every snapshot is O(D) work on the caller's thread,
and if that thread is the worker, it steals cycles from matching.
In a production setup you'd push L2 as an event stream instead of
polling, so snapshots are computed once per change and fan out to
all listeners.
