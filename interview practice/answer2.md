# Interview Answers — Full Project (Current + 4 Planned Additions)

Answers for the *additional* threads (10–15) in `question2.md`. Threads
1–9 are already covered in `answer1.md` and still apply.

Same format: prose-style, truthful, scoped, follow-up-ready.

---

## Thread 10 — REST API Design

**Q1. Walk me through your REST layer.**

I use cpp-httplib, a single-header HTTP server. It exposes POST /orders
for placement, DELETE /orders/:id for cancel, GET /book/:symbol for an
L2 snapshot, and GET /trades for recent fills. The handlers parse the
JSON body, construct an `EngineEvent`, and push it onto the existing
thread-safe event queue. The worker thread still owns all matching
state, so from the engine's point of view nothing changed — the HTTP
layer is just one more producer.

**Q2. Why REST and not gRPC?**

Demo friendliness, mostly. REST is language-agnostic — anyone can hit
my endpoints with curl. gRPC is more efficient and has better type
safety via protobuf, and for a production internal service I'd lean
that way. For a portfolio project, the zero-friction "you can curl it"
story matters more than raw efficiency.

**Q3. JSON parsing and validation?**

cpp-httplib doesn't ship a JSON parser, so I use nlohmann/json
(header-only, simple). Each handler checks required fields exist, that
quantity is positive, that price is positive for limit orders, that
side is either BUY or SELL. Anything missing or malformed returns
HTTP 400 with a JSON error body naming the offending field.

**Q4. Two `POST /orders` at the same instant — what happens?**

Both handler threads build their `EngineEvent` independently (nothing
shared), then each acquires the queue mutex in turn to push. One wins
first, one waits briefly. Both events land on the queue in a
well-defined order. The worker processes them sequentially, so matching
order is deterministic given the queue order.

**Q5 [SHARP]. HTTP handler on thread A, matching on thread B — how does the response get the result?**

Two reasonable answers. The simple one: each `EngineEvent` carries a
`std::promise` from the handler; the worker fulfills the promise with
the result after processing, and the handler blocks on the paired
`std::future` before sending the HTTP response. The handler thread is
still the one writing the response, but it's synchronized with the
worker via the future.

The alternative is a fully async server — the handler pushes the event,
immediately returns 202 Accepted with a request ID, and the client
polls or subscribes for the result. That's heavier and I didn't do it.

**Q6. Status codes.**

- 200 OK — successful placement or cancel
- 201 Created — could also be used for new orders, stylistic call
- 400 Bad Request — malformed or missing fields
- 404 Not Found — cancel on unknown order ID
- 409 Conflict — cancel on already-filled order
- 429 Too Many Requests — once I add rate limiting
- 500 Internal Server Error — anything unexpected, logged for triage

**Q7 [CS]. 400 vs 404 vs 409 vs 422?**

400 = request is malformed at a syntactic level — can't even parse it.
404 = the resource doesn't exist. 409 = a state conflict — the
resource is in a state that precludes this operation (e.g. cancel
already-filled). 422 = request is *syntactically* fine but
*semantically* invalid — e.g. quantity is zero. Different codebases
draw these lines differently; I'd document my choice clearly and be
consistent.

**Q8. How would you add rate limiting without breaking existing clients?**

Per-client-IP token bucket in front of the handlers — start generous,
return 429 when exhausted with a `Retry-After` header. Existing clients
don't break because any reasonable client respects 429. I'd also expose
the current bucket state via a custom header for debuggability. Adding
it last means I can measure real traffic first and tune the limits.

**Q9 [SHARP]. Cancel arrives before the order has been processed?**

Both the place and the cancel are just events on the same queue. The
worker processes them in arrival order — so if the cancel arrives
first on the queue, the hash lookup misses and the cancel is rejected.
If the place is first, it's inserted, then the cancel finds it and
removes it. Either way, order within the queue determines semantics.
The only weird case is a cancel that *races* a place from the same
client — that's a client correctness issue, not an engine one.

---

## Thread 11 — Multi-Symbol Routing

**Q1. How does the engine know which book an order belongs to?**

Each `EngineEvent` carries a symbol string. The router — a thin layer
in front of the worker — has an `unordered_map<string, OrderBook*>` and
looks up the right book per event in O(1) before dispatching. The
worker itself is unchanged; it just gets a reference to which book
it's operating on.

**Q2. Why `unordered_map` over `std::map`?**

Symbol lookups are frequent, unordered iteration is fine, and hashing
strings is fast. I don't need sorted symbol order for any operation.
Hash map gives me O(1) per lookup where `std::map` would give me
O(log N) per symbol for no benefit.

**Q3 [CS]. Cache behavior with thousands of symbols?**

`std::map` is a tree of pointer-linked nodes — each node is its own
heap allocation, so traversal cache-misses a lot. `unordered_map` is
usually also pointer-linked internally (open hashing with chaining),
so it's not a huge cache win, but the O(1) average lookup wins
anyway. For tight memory layout I'd reach for something like
`absl::flat_hash_map`, but I haven't in this project.

**Q4. Per-symbol parallelism — one worker per symbol?**

Straightforward. Each symbol gets its own `EventQueue` and its own
worker thread. Orders on different symbols are independent so they
can match in parallel without any locks. The router becomes the
front door — it looks up the queue by symbol and pushes. You scale
near-linearly with number of symbols (until you run out of cores).

**Q5. Ordering across symbols?**

No — and that's fine, because there's no semantic meaning to "buy
AAPL then sell TSLA in that order" across different books. Within a
symbol, FIFO order is preserved because everything still funnels
through one queue per book.

**Q6 [CS]. Sharding.**

Partitioning data across multiple processing units so each unit owns a
disjoint subset and there's no cross-unit coordination on the hot path.
What I've built is sharding by symbol — classical horizontal scaling.
The same pattern appears in databases (shard by user ID), web servers
(shard by session), and distributed caches.

**Q7 [SHARP]. Two symbols hash to the same bucket — correctness or performance?**

Performance, not correctness. `unordered_map` handles collisions by
chaining multiple entries in the same bucket — lookup still works, it
just takes an extra comparison. Correctness never depends on which
bucket a key lands in. If I saw pathological collision behavior in
practice I'd rehash or switch hash functions.

---

## Thread 12 — CI / GitHub Actions

**Q1. Walk me through your CI.**

A single workflow file in `.github/workflows/ci.yml`. On every push or
PR to main: checkout, install CMake and g++, run CMake configure, run
CMake build, run the test binary. If any step fails, the workflow
fails and the PR can't be merged. There's a "build passing" badge in
the README.

**Q2. Trigger?**

`on: [push, pull_request]` scoped to the main branch. Keeps it simple
— every change runs the full suite.

**Q3. How long does it take?**

About a minute — install, configure, build, test. If it crept past 5
minutes I'd start caching dependencies; past 10 I'd look at what's
slow and either parallelize or move some tests to a nightly job.

**Q4. Flaky test — how do you triage?**

First step: reproduce locally. Flakiness that I can't reproduce points
at environmental factors — timing, filesystem state, port
conflicts. For concurrent tests specifically I check whether the
assertion relies on a particular scheduling outcome, and if so I
either make it deterministic (controlled barriers) or weaken it to
check an invariant that holds across all interleavings. I never retry
a flaky test and call it green — that's just deferring the bug.

**Q5 [SHARP]. Test failed but looks wrong to you — what do you do?**

First I fight the assumption that I'm right and the test is wrong —
that bias ends badly. I understand *why* the test was written —
what behavior it was asserting, what bug it was catching. If after
that I still believe the test's spec is stale, I update it together
with the code change, explicitly, with a clear commit message
explaining why the old assertion is no longer correct. If I can't
articulate why the old assertion was wrong, I haven't earned the
right to change it.

**Q6. CD?**

Not yet — this project isn't deployed anywhere. CI is build plus test.
Adding CD would mean containerizing, pushing to a registry, and
deploying on merge to main. That's the next step if this ran as a
service.

**Q7 [CS]. CI vs CD, trunk-based?**

CI = every change gets built and tested automatically. CD = every
green build gets deployed (continuous deployment) or at least is
*deployable* (continuous delivery). Trunk-based development = everyone
works off a single long-lived branch (main), merges are small and
frequent, long-lived feature branches are avoided. The three play
together: small merges to trunk, fast CI, continuous delivery.

---

## Thread 13 — Structured Logging

**Q1. How do you log? Why structured?**

spdlog at INFO/WARN/ERROR levels, formatted with a consistent pattern:
timestamp, level, component, message, and key=value fields. Structured
means every log line is grep-able on specific fields — `order_id=X`,
`symbol=Y` — so debugging is a grep away, not a regex puzzle.

**Q2. Why spdlog?**

Header-only or small compiled library, fast, proven in production
use. Async logging mode lets me keep the hot path cheap by deferring
I/O to a dedicated thread. Good balance of performance and ergonomics.

**Q3. Sample log line?**

`[2026-04-21 12:34:56.789] [INFO] [engine] order_matched order_id=abc
symbol=AAPL side=BUY qty=100 price=187.43 counterparty=xyz`

Every field is searchable.

**Q4 [SHARP]. Logging in the hot path slows the engine?**

It can. Two mitigations. First, async mode — the log call copies the
formatted string into a ring buffer and returns; a background thread
writes to disk. Second, compile-time log level filtering — at
release, anything below INFO is a no-op macro. That keeps the hot
path overhead in single-digit nanoseconds per call. I'd also sample
high-volume events (log 1 in N trades at INFO) if volume became
unreasonable.

**Q5. Gigabytes of logs per day?**

Rotation by size or time, compression of rotated files, and shipping
to a central log store (Loki, Elasticsearch, whatever the org uses).
At that volume you also start caring about log sampling and making
sure WARN/ERROR are *never* sampled — those are the lines that matter
when something goes wrong.

**Q6 [CS]. INFO vs WARN vs ERROR.**

INFO = normal flow, useful for context when debugging but not
actionable on its own. WARN = something unexpected happened but the
system recovered or worked around it — worth investigating
proactively. ERROR = something went wrong that affected a user or
operation — demands attention. DEBUG and TRACE sit below INFO for
deep diagnostics, usually disabled in production.

**Q7. Sensitive data in logs?**

You don't log it. For fields that might contain PII, the code either
omits them entirely or masks them (redact all but last 4 chars).
In this project the main risks are user IDs — I log them as-is
because they're opaque identifiers, not PII. If they were emails or
names, I'd mask.

---

## Thread 14 — Cross-Cutting / System Design

**Q1. Draw the architecture.**

```mermaid
  clients                     HTTP
     │                          │
     ▼                          ▼
 ┌────────────────────────────────────┐
 │ REST API (cpp-httplib, N threads)  │
 │   validate → build EngineEvent     │
 └────────────────┬───────────────────┘
                  │ push
                  ▼
          ┌──────────────┐
          │ EventQueue   │ (mutex + cv)
          └──────┬───────┘
                 │ pop
                 ▼
         ┌─────────────────┐
         │  SymbolRouter   │─→  log (spdlog async)
         │  hash: sym→book │
         └───┬──────┬──────┘
             │      │
    ┌────────▼─┐ ┌──▼───────┐
    │ OrderBook│ │ OrderBook│  ...
    │  (AAPL)  │ │  (TSLA)  │
    └──────────┘ └──────────┘
         │
         ▼
   TradePublisher (interface)
         │
         ├─ InMemory (tests)
         └─ Stream  (prod stub)
```

**Q2. Single point of failure?**

The worker thread. If it crashes, matching stops and the queue grows
unbounded. The HTTP layer keeps accepting orders — which is actually
a problem. A production version would have a health signal from the
worker so the HTTP layer can start returning 503.

**Q3. Worker crashes mid-match — book state?**

Corrupt, possibly. The matching loop mutates several structures per
trade — the price map, the hash map, the best-bid/ask cache. If the
process dies between two of those, the in-memory state is inconsistent.
Since there's no persistence today, recovery means starting fresh.
That's the motivation for adding a write-ahead log as a later step.

**Q4 [SHARP]. Would persistence fix that? What and when?**

Persistence fixes the durability half — you can reconstruct state on
restart by replaying events. What I'd persist: every accepted
`EngineEvent`, in order, to an append-only log, before acknowledging
the HTTP response. When: synchronously before the HTTP 200 returns,
so no client thinks an order was accepted that didn't hit disk. That
adds fsync latency, but for a demo project you'd at least fsync in
batches.

**Q5. Two regions with low-latency reads?**

Reads are the easy side — L2 snapshots are derived from book state
and can be streamed or cached per region. Writes are harder: you
still need a single authoritative book per symbol, so writes have to
go to the region that owns that symbol. Routing would be by symbol
affinity. For real exchanges this is solved by having one matching
engine per region with separate books, not by distributing a single
book across regions.

**Q6 [CS]. CAP and which two.**

CAP: in the presence of a Partition, you pick Consistency or
Availability. My engine is single-node single-book, so CAP doesn't
really apply — there's nothing to partition. If I distributed it,
I'd pick CP for matching — you can't tolerate inconsistent fills.
Availability would have to come from fast failover plus durable
state, not from accepting stale reads.

**Q7. Where does this architecture break first as load grows?**

The single worker thread per book. Once one symbol sustains enough
orders to saturate one core, throughput on that symbol caps. You can
shard across symbols but not within one. The next bottleneck after
that is the OS scheduler's ability to context-switch between HTTP
handler threads and worker threads — at that point you'd pin workers
to cores and move to a reactor-style HTTP layer.

---

## Thread 15 — Lessons & Reflection

**Q1. Biggest design mistake?**

Coupling stop-order management to `OrderBook` — the `MatchingEngine`
mutates the book's `pending_stops` vector directly. That's the kind
of cross-cutting concern that should live in its own class
(StopOrderManager). Today it works fine but it's a code-smell that
would block clean extension.

**Q2. If I rewrote it?**

Two changes. Fixed-point prices instead of `double` — FP equality on
map keys is a subtle footgun. And a clean StopOrderManager from day
one.

**Q3. Weakest part?**

Probably the stop-order O(S) scan. It's acknowledged and correct,
just slow at scale. Close second is the unbounded trade-history
vector — no backpressure if nobody consumes it.

**Q4. What did you learn about C++?**

Intrusive data structures for real — I'd read about them before but
building one from scratch and seeing the O(1) cancel fall out of the
design was concrete. Also a much better feel for move semantics,
`const` correctness, and when lambdas beat function objects.

**Q5. Why a matching engine?**

I wanted a project with real algorithmic content (data structures,
complexity) and real systems content (concurrency, event loops) that
wasn't a CRUD app. Matching engines sit right at that intersection —
non-trivial DSA, meaningful concurrency, and enough domain interest
to stay motivated.

**Q6 [SHARP]. Weakest area of your explanation?**

I'd be slower on very specific questions about the C++ memory model —
release/acquire semantics, memory barriers. I understand them
conceptually, but haven't had to reason about them in this project
because the single-worker design sidesteps the question. If I were
building a lock-free queue I'd need to dig deeper, and that's the
next thing I'd study.

*(Note: this answer demonstrates the senior pattern — pick something
specific, own it, say what you'd do to learn it. You sound mature,
not weak.)*
