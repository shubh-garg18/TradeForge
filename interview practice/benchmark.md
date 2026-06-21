# Interview Thread — Benchmarking the Matching Engine

A dedicated thread for the `bench/Benchmark.cpp` micro-benchmark. Same
format as `question1.md` / `answer1.md`: question, then the answer written
as *how you internalize it* — prose to absorb and rephrase, not a script to
recite.

This is the thread that turns "I think it's fast" into "I measured it." It
pairs with §5 of `Notes.md` (Benchmarking — What & How) and §C.6 of
`docs/Performance_Audit.md` (the measured results).

> ⚠️ Honesty rule (from `Notes.md` §9): these are **laptop, single-core,
> Release** numbers. Never round them up into "HFT" or "nanosecond" claims.
> The mature move is to *caveat the hardware first*, then give the figure.

---

## The numbers to know cold (N = 1,000,000)

| Scenario                            | orders/sec | p50 (µs) | p99 (µs) |
| ----------------------------------- | ---------: | -------: | -------: |
| Limit insert (resting, no match)    |    ~951 K  |    0.65  |    3.19  |
| Match 1:1 (full fill + level erase) |    ~2.02 M |    0.36  |    1.08  |
| Match FIFO (deep single level)      |    ~5.03 M |    0.17  |    0.21  |
| End-to-end (alloc + match)          |    ~2.13 M |    0.25  |    2.34  |

One-line takeaway to memorize: **FIFO matching is fastest by far (~5 M/s,
p50 0.17 µs); limit insert is slowest (~951 K/s) because it's the one
scenario that grows the `std::map` to a million levels — a big red-black
tree with poor cache locality on every insert.**

> ⚠️ These numbers swing run-to-run on a laptop core. FIFO-fastest is the
> only ranking that's stable across every run; the order among the
> map-touching scenarios (and the level-churn p99 especially) moves around
> with scheduler noise. Quote the *shape* — "FIFO ~5M, the rest ~1–2M" —
> not exact digits, and lead with the hardware caveat.

---

## Thread B — Benchmarking

**Q1.** How do you know your engine is fast? Did you measure it?

Yes — there's a micro-benchmark in `bench/Benchmark.cpp` that times the
real hot path, `MatchingEngine::process_order`, and nothing synthetic. It
pre-builds a deep book and pre-constructs every incoming order *outside*
the timed region, then submits a million orders and records per-order
latency with `steady_clock`. I report throughput plus p50, p99, mean and
max. On my laptop the fast path — deep single-level FIFO matching — does
around 5 million orders a second with a p50 near 0.17 microseconds.

**Q2.** Why did you exclude order allocation and book setup from the timing?

Because I wanted to isolate the matching cost, not measure my benchmark
harness. Allocating a million `Order` objects and building the resting
book is setup noise — if I timed it, the number would be dominated by
`new` and `std::deque` growth, not by matching. I do have one scenario,
"end-to-end," that *deliberately* puts allocation back inside the timed
region, so I can show both: pure matching, and matching-plus-allocation.

**Q3.** Walk me through the scenarios. Why more than one?

Because a single number hides where the cost is. I have four:
(1) limit insert with no crossing — pure `std::map` insertion, O(log P);
(2) one-to-one matching where every taker fully fills one resting order
and empties its level, which forces a map erase per match — worst-case
book maintenance; (3) deep single-level FIFO, where all liquidity sits at
one price so the level survives until the last fill — that isolates pure
FIFO matching and fee cost with no map churn; and (4) end-to-end, same
book as FIFO but allocation is timed too. Comparing them tells me *which*
operation dominates.

**Q4 [SHARP].** Your scenarios disagree by ~5x — FIFO does ~5M/s, limit
insert only ~950K/s. Why is *insertion* the slow one?

Because that gap is the cost of `std::map` structural operations, and
running more than one scenario is what exposes it. FIFO keeps all liquidity
at a single price, so the map never grows past one entry — each match is
just an O(1) intrusive-list head pop plus a fee lookup, with great cache
locality. Limit insert is the opposite: it's the one scenario whose map
grows to a million distinct levels, so every insert is an O(log P) descent
into an ever-larger red-black tree plus a BBO refresh, and the tree's
pointer-chasing destroys cache locality as it grows. That's the `std::map`
cost I flagged in my performance audit, surfacing under the *largest* tree —
exactly where the complexity model says it should hurt most. The
level-churn scenario also pays map cost (an erase per fill) and lands in the
middle. The honest caveat is that the ordering among the map-touching
scenarios shifts run-to-run on a laptop — what's stable is that the
single-level path with no tree growth wins every time.

**Q5 [CS].** Why `steady_clock` and not `system_clock`?

`steady_clock` is monotonic — it never jumps backward or forward from NTP
adjustments or a user changing the wall clock. For measuring elapsed
durations that's exactly what you want. `system_clock` tracks calendar
time and can step, which would corrupt a latency sample. Rule of thumb:
`system_clock` for "what time is it," `steady_clock` for "how long did
this take."

**Q6 [SHARP].** Your max latency is in the tens of milliseconds but p50 is
sub-microsecond. Isn't that a problem?

Those max values aren't the engine — they're OS scheduling and page-fault
outliers on a non-isolated laptop core. A single context switch or a page
fault during one timed `process_order` shows up as a millisecond-scale
spike. That's exactly why I report percentiles: p50 and p99 are robust to a
handful of outliers, max isn't. If I wanted trustworthy tail numbers I'd
pin the thread to an isolated core, lock memory, disable frequency scaling,
and use an HDR histogram — but for a portfolio benchmark, p50/p99 is the
honest level of precision.

**Q7.** Why a warm-up run before the measured runs?

The first pass through the code touches cold instruction and data caches
and an untrained branch predictor, so its timings are pessimistic and not
representative of steady state. I run one small warm-up scenario whose
result I throw away, so the reported numbers reflect a warm cache. It's a
standard micro-benchmark hygiene step.

**Q8 [CS].** How do you compute p99 from the samples?

I collect every per-order duration into a vector, sort it ascending, and
index at `floor(0.99 * (n - 1))`. It's the nearest-rank percentile — no
interpolation. For a million samples that's plenty accurate. The sort is
O(n log n) but it's outside the timed region, so it doesn't affect the
measurement. For streaming or much larger samples I'd switch to a
histogram (like HdrHistogram) to avoid storing every point.

**Q9 [SHARP].** What *doesn't* this benchmark tell you?

A few things, and I'd say so up front. It's single-threaded, so it doesn't
measure queue contention or the producer-consumer path — it calls
`process_order` directly, bypassing the `EventQueue`. It's one symbol, one
core, one machine. It doesn't model realistic order-flow distributions —
my takers are priced to always cross. And it doesn't measure end-to-end
latency through the API or publishing layers. So it's a *matching-core*
micro-benchmark, not a system benchmark. Naming that scope is more
convincing than pretending the number covers everything.

**Q10.** If you wanted higher throughput, what would the benchmark push you
to optimize first?

The map-heavy scenarios point straight at it: `std::map` structural
operations over a large tree. Limit insert — which grows the map to a
million levels — is the slowest, and level-churn pays an erase per fill, so
both say the same thing. The audit's planned fix is to replace the
price-level map with a cache-friendlier flat structure plus a hash index
for non-best lookups, which attacks exactly that O(log P) insert/erase/
refresh cost and the cache-locality loss on a big tree. I'd also move
prices from `double` to fixed-point `int64_t` ticks to kill comparison
drift. The benchmark gives me a before/after harness to prove any of those
actually helped instead of guessing.

---

## 30-second spoken version (if they just want the headline)

> "I benchmarked the matching core directly — a million orders through
> `process_order`, timed with `steady_clock`, reported as p50/p99. On my
> laptop the fast path — deep single-level FIFO matching — does around 5
> million orders a second at a p50 near 0.17 microseconds. The interesting
> result is that *insertion*, not matching, is the slow scenario: building a
> book of a million distinct price levels grows the `std::map` into a big
> red-black tree, so each insert pays O(log P) plus a cache-unfriendly tree
> walk and it drops under a million a second. That confirmed my complexity
> audit — the map maintenance is the first bottleneck — and it tells me
> exactly what to optimize next. These are single-core laptop numbers that
> swing run-to-run, so I keep them scoped."
