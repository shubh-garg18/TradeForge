# Notes — Concepts & Reference Tables

Visual reference. Everything is in tables so it's fast to scan and
revise. Each table shows **what you picked in this project**, **what
you could have picked instead**, and **why the alternative wasn't a fit**.

---

## 1. Big-O Quick Reference (for YOUR operations)

| Operation                        | Complexity     | Data structure backing it |
|----------------------------------|----------------|---------------------------|
| Insert order at existing price   | **O(1)**       | Intrusive list tail append |
| Insert order at new price        | **O(log P)**   | `std::map` insert |
| Cancel order by ID               | **O(1)** avg   | Hash lookup + list splice |
| Best bid / best ask              | **O(1)**       | Cached pointer |
| Full match across K levels       | **O(K log P + F)** | Map walk + fills |
| L2 snapshot (depth D)            | **O(D)**       | Ordered map walk |
| Stop-order scan after trade      | **O(S)**       | Linear vector scan *(known weakness)* |
| FOK pre-scan                     | **O(K log P)** | Walk opposite side |

> P = distinct price levels · K = levels touched · F = fills · S = pending stops

**Memorize this. It answers ~30% of data-structure follow-ups.**

---

## 2. Data Structures — Used vs. Alternatives

| Data structure | Used for | Why I picked it | Alternative | Why NOT the alternative |
|---|---|---|---|---|
| `std::map<double, PriceLevel*>` | Bids & asks ordered by price | Ordered iteration, O(1) BBO via `rbegin()/begin()`, O(log P) insert | `std::unordered_map` | No ordering → can't get best-bid efficiently |
| **Intrusive doubly-linked list** | FIFO orders inside a price level | O(1) cancel given `Order*`, single allocation per order, cache-friendly | `std::list<Order>` | Extra heap allocation per node, can't cancel without scanning |
| `std::unordered_map<string, Order*>` | Cancel-by-ID lookup | O(1) average — no ordering needed | `std::map` | O(log N) lookup for zero benefit |
| **Cached `Order*` BBO pointers** | Best-bid / best-ask | O(1) reads, refreshed on structural change | Recompute each read | Forces O(log P) per read — slow for frequent BBO queries |
| `std::vector<Order*> pending_stops` | Stop orders waiting to trigger | Simple, correct | `std::multimap<double, Order*>` | Faster (O(log S)) but more code; deferred as known improvement |

---

## 3. C++ Concepts — Cheat Sheet

### RAII, Smart Pointers, Ownership

| Concept | What it means | Used in your project? | Notes |
|---|---|---|---|
| **RAII** | Resource lifetime = object lifetime; dtor releases | ✅ Implicit everywhere | `std::lock_guard` in the EventQueue is the canonical example |
| **Rule of 0** | Don't define dtor / copy / move — let compiler generate | ✅ `Order`, `PriceLevel` follow this | Members are POD + raw non-owning ptrs |
| **Rule of 3 / 5** | If you define one of dtor/copy/move, define all | ❌ Not needed | Because you followed Rule of 0 |
| **`unique_ptr<T>`** | Exclusive ownership, zero overhead | ❌ Used raw ptrs instead | Book has strict ownership discipline — simpler that way |
| **`shared_ptr<T>`** | Refcounted, thread-safe refcount | ❌ Avoided | Atomic refcount adds hot-path overhead |
| **`weak_ptr<T>`** | Non-owning ref to `shared_ptr` target | ❌ | Not needed |
| **Raw pointers** | Non-owning references, or owned-by-convention | ✅ Yes — book owns, everyone else borrows | Pragmatic choice for hot-path code |

### Move Semantics & `const`

| Concept | One-liner | Trap to avoid |
|---|---|---|
| `T&&` (rvalue ref) | Binds to temporaries | Doesn't bind to named variables |
| `std::move(x)` | Casts to `T&&` — doesn't actually move | After move, `x` is valid-but-unspecified |
| `const T&` | Won't mutate the referent | Can be bound to temporaries (with lifetime extension) |
| `T* const` | Const *pointer*, mutable data | Different from `const T*` |
| Member `void f() const` | Won't mutate `*this` | Cannot call non-const members |

### `std::map` vs `std::unordered_map`

| Property | `std::map` | `std::unordered_map` |
|---|---|---|
| Underlying structure | Red-black tree (typical) | Hash table (chained) |
| Average lookup | O(log N) | O(1) |
| Worst-case lookup | O(log N) | O(N) *(bad hash)* |
| Ordered iteration | ✅ yes | ❌ no |
| Range queries | ✅ yes (`lower_bound`) | ❌ no |
| Pointer stability | ✅ stable across insert/erase | ✅ stable (rehash invalidates only iterators, not `T*`) |
| Cache behavior | Poor (pointer-chasing nodes) | Poor (chaining) — `flat_hash_map` is better |
| **When to pick** | Need "best / next / range" | Pure key→value lookup |

---

## 4. Concurrency — Primitives & Patterns

### Primitives

| Primitive | What it does | Used in your project? | If asked: safe answer |
|---|---|---|---|
| `std::mutex` | Mutual exclusion — one thread at a time | ✅ Inside `EventQueue` | "Guards the queue's internal deque" |
| `std::condition_variable` | Sleep until notified | ✅ Worker blocks here when queue empty | "Wakes on push, guards against spurious wakeup with predicate" |
| `std::lock_guard<Mutex>` | RAII wrapper, locks on construct | ✅ Yes | "Exception-safe locking" |
| `std::unique_lock<Mutex>` | More flexible — unlock/relock, move | ✅ With `cv.wait` | "CV needs it because wait releases the lock" |
| `std::atomic<T>` | Lock-free scalar operations | ❌ Not used | "Single-writer design makes them unnecessary" |
| `std::thread` | OS thread | ✅ Worker thread | "Owned by the engine, joined on shutdown" |

### Patterns

| Pattern | Where you use it | One-sentence pitch |
|---|---|---|
| **Producer-Consumer** | `EventQueue` + worker | "N producers push, 1 consumer pops — queue is the synchronization boundary" |
| **Single-Writer** | Matching mutates only from worker | "Eliminates write-write races by construction" |
| **Event-Driven** | Dispatch from queue events | "Actions are queued, not called synchronously" |
| **Strategy** | `TradePublisher` interface | "Engine depends on abstraction, concrete impls plug in" |
| **Intrusive container** | `Order` carries `prev/next` | "No extra allocation, O(1) splice given pointer" |

### Spurious Wakeup — the one concurrency trap to know cold

| Term | Meaning |
|---|---|
| **Spurious wakeup** | `cv.wait()` returns with no `notify()` — kernel artifact, allowed by standard |
| **Guard** | Always use predicate form: `cv.wait(lock, [&]{ return !queue.empty(); })` |
| **Why the predicate?** | Re-checks condition on every wake; sleeps again if false |

### ⚠️ Things you do NOT claim about concurrency

| ❌ Don't say | ✅ Say instead |
|---|---|
| "Lock-free queue" | "Thread-safe queue using mutex and condition variable" |
| "Wait-free" | "Single-writer on the hot path" |
| "Nanosecond-latency" | "Measured p99 matching latency of X microseconds" |
| "Concurrent matching" | "Concurrent submission, serialized matching" |

---

## 5. Benchmarking — What & How (CORE-5)

| What to measure | How |
|---|---|
| **Matching latency** | `trade.ts − event.submit_ts` per trade |
| **Queue wait time** | `event.pop_ts − event.submit_ts` |
| **End-to-end latency** | submit → publish |
| **Throughput** | N orders / wall-clock elapsed |
| **Cancel latency** | cancel dispatch → complete |
| **L2 snapshot cost** | `std::chrono` around the call |

| Do | Why |
|---|---|
| Use `std::chrono::steady_clock` | Monotonic — no NTP jumps |
| Run N ≥ 1,000,000 orders | Stable tail statistics |
| Report min / avg / **p50 / p99** | Enough signal for a portfolio |
| Keep code separate (`bench/bench_main.cpp`) | Doesn't pollute production path |
| Caveat the numbers in README | "Measured on a laptop, single core" is a mature qualifier |

| Don't | Why not |
|---|---|
| Claim p99.9 or p99.99 | Needs huge samples + HDR histogram |
| Use `system_clock` | Non-monotonic |
| Benchmark in debug build | Numbers are meaningless |
| Compare with "HFT systems" | Different universe — stay humble |

---

## 6. Design Patterns Cheat Sheet

| Pattern | In your code | Pattern family |
|---|---|---|
| Producer-Consumer | `EventQueue` | Concurrency |
| Single-Writer | Worker loop | Concurrency |
| Strategy | `TradePublisher` interface | Behavioral |
| Event-Driven / Event Loop | Dispatch from queue | Architectural |
| Dependency Inversion | Engine depends on publisher abstraction | SOLID (D) |
| Open/Closed | Adding new publishers doesn't modify engine | SOLID (O) |

---

## 7. System Design Vocabulary

| Term | Quick meaning | How it relates to your project |
|---|---|---|
| **Sharding** | Partition data across workers | Your planned multi-symbol = shard by symbol |
| **Horizontal scaling** | More machines/workers | Per-symbol threads scale out horizontally |
| **Vertical scaling** | Bigger machine | What you do today — one worker, one core |
| **Backpressure** | Slow producers when consumer lags | Absent — your queue is unbounded (honest gap) |
| **Idempotency** | Same op twice = same result | Not built — good to mention as "future" |
| **At-least-once / exactly-once** | Delivery guarantees | Future topic if you add a message broker |
| **CAP theorem** | Under partition: pick C or A | Single-node → CAP doesn't really apply yet |
| **Write-Ahead Log (WAL)** | Append to log before mutating state | Optional future (STRETCH-1) |
| **Snapshot + log** | Periodic checkpoint + log tail | Standard durability story |
| **Strong consistency** | All readers see latest write | Matches your single-writer design |
| **Eventual consistency** | Readers converge over time | Not your model — matching must be strong |

---

## 8. Phrases That Score Points

| Phrase | When to drop it |
|---|---|
| "Amortized O(1)" | Cancel-by-ID, hash lookup |
| "Producer-consumer boundary" | Concurrency question |
| "Single-writer" | Explaining lock strategy |
| "Intrusive data structure" | Data-structure question |
| "Cache-friendly" | *Only* if backed by reasoning |
| "Strategy pattern" | `TradePublisher` |
| "Dependency inversion" | Anywhere abstractions are used |
| "Invariant" | Testing / correctness ("I assert the invariant that...") |
| "Trade-off" | Design choice questions |
| "Out of scope for this version" | Senior way to say "didn't build" |

---

## 9. Things You Do NOT Claim (Caught-Lying List)

| ❌ Don't say | 💥 Why it's dangerous |
|---|---|
| "Lock-free" | You use a mutex — easy to catch |
| "HFT / high-frequency" | Different performance tier entirely |
| "Nanosecond latency" | Haven't measured at that resolution |
| "Production-grade" | It's a portfolio project |
| "Scales to millions of orders/sec" | No benchmarks back it |
| "Distributed" | Single process |
| "Crash-safe" | No persistence yet |
| "Zero-copy" | You allocate per order |

---

## 10. Interview Soft Skills

| Move | Why seniors like it |
|---|---|
| Scope your claims | "Correct for the cases I tested" > "production-ready" |
| Own tradeoffs out loud | Shows you understand, not just memorized |
| Admit unknowns confidently | "I haven't benchmarked this" > "it's probably fast" |
| Follow the interviewer's lead | Don't pivot to your rehearsed topic |
| 3-second pause before hard answers | Beats 20 seconds of rambling |
| Ask clarifying questions | "More users or more orders per user?" |
| Number-check yourself | "Actually ~170, let me recount" — sounds rigorous |

---

## 11. Common Follow-Up Traps — One-Liner Safe Answers

| Question | Safe one-line answer |
|---|---|
| "What if requests arrive out of order?" | "Queue defines order — arrival order = processing order. Cross-symbol ordering isn't preserved and doesn't need to be." |
| "Using a test framework?" | "Assertion-based, no framework — minimal build. I'd adopt GoogleTest if I needed fixtures." |
| "What at 1M orders/sec?" | "Single worker caps at one core — scaling is per-symbol sharding, one worker per book." |
| "Why not Boost?" | "Wanted to learn the primitives. Standard library was enough." |
| "Any race conditions?" | "Only shared surface is the mutex-protected queue. Everything after pop is single-threaded." |
| "Bad input?" | "Validated before mutation — malformed events rejected, book never touched by bad input." |
| "What's the biggest weakness?" | "O(S) stop-order scan — linear in pending stops. `multimap` by trigger price fixes it to O(log S + K). On my list." |
| "Memory leaks?" | "Book owns orders, deletes on fill or cancel. Single-threaded mutations — no double-delete path. Valgrind-clean." |

---

## 12. Final-Day Checklist

### Morning of the interview

- [ ] Read `explanation.md` §9 (Quick-Revise Cheat Sheet)
- [ ] Say the 90-second walkthrough aloud once
- [ ] Sketch the architecture diagram on paper from memory
- [ ] Skim this file §1 (Big-O) and §9 (Don't-Claim list)
- [ ] Read 2–3 answers from `answer1.md` on your weakest thread
- [ ] Water. Deep breath. Go.

### After the interview (same day)

- [ ] Jot down questions that surprised you
- [ ] Note any wobble points → add better answers to `answer1/2.md`
- [ ] Pick 1 concept to study deeper before the next round
