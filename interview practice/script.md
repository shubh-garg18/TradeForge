# Project Script — What to Say, In My Own Words

A spoken script for talking about this project end-to-end in an interview.
Unlike `explanation.md` (which gives you *formats* for different audiences),
this is one continuous narration you can deliver out loud — written the way
*I* would actually say it, in plain first-person language.

**How to use it:** read it aloud a few times until the *shape* is yours,
then throw the page away. Don't memorize word-for-word — memorize the
*beats*. The bracketed `[cues]` tell you what the move is, not what to say.
If the interviewer interrupts with a question, follow them; these beats are
a spine to return to, not a track to run.

---

## Beat 0 — The opening line (when they say "tell me about a project")

> "Sure. The one I'm most proud of is a limit order book matching engine I
> built in C++20 — basically the piece of software that sits at the core of
> a stock exchange and pairs up buy and sell orders. I built it to get deep
> on data structures, concurrency, and writing systems code that I can
> actually defend, not just get working."

[Why this line: it states *what*, *the domain*, and *why I built it* in
fifteen seconds, and it ends on "defend" — which invites them to probe.]

---

## Beat 1 — What it does (the domain, plainly)

> "At a high level: orders come in — somebody wants to buy 100 shares at
> $10, somebody wants to sell at $9.99 — and the engine's job is to match
> them fairly and produce trades. 'Fairly' has a specific meaning here:
> price-time priority. Best price wins first, and at the same price, whoever
> got there first wins. That's the standard every real exchange uses.
>
> It supports six order types — limit, market, IOC, FOK, and the two stop
> orders, stop-loss and stop-limit — and on top of matching it does
> volume-tiered maker-taker fees, publishes trades out through an interface,
> and can produce an L2 market-data snapshot of the book."

[If they're non-technical, stop here and let them steer. If they're an
engineer, keep rolling into Beat 2.]

---

## Beat 2 — The data structures (the heart of it)

> "The core of the engine is the order book, and the whole thing comes down
> to three data structures, each chosen for one job.
>
> First, the price levels. Bids and asks are each a `std::map` keyed by
> price. I use a map specifically because I need *ordering* — the best bid
> is the highest price, the best ask is the lowest — and a map gives me that
> for free: best bid is the last key, best ask is the first. Insert is
> O(log P) where P is the number of distinct price levels.
>
> Second, inside each price level the orders are a FIFO queue — but an
> *intrusive* doubly-linked list, not a `std::list`. Intrusive means the
> `Order` object itself carries the `prev` and `next` pointers. That buys me
> two things: one allocation per order instead of two, and O(1) removal
> given just an `Order*` — no searching.
>
> Third, to cancel an order by ID I keep an `unordered_map` from order ID to
> `Order*`. Hash lookup is O(1), and because the list is intrusive, once I
> have the pointer I can splice the order out of its level in O(1) too. So a
> cancel is O(1) end to end in the common case.
>
> And then best-bid / best-ask are cached pointers, so reads are O(1) — I
> pay the refresh cost at write time, when the structure of the book
> actually changes."

[This is the beat where most follow-ups come from. Every sentence above is
a deliberate hook — let them grab one.]

---

## Beat 3 — Concurrency (the part that sounds senior)

> "The engine has to handle many clients submitting at once without
> corrupting the book, and the way I did that is a producer-consumer design
> with a single writer.
>
> Any number of producer threads push events — new order, cancel — onto a
> thread-safe queue. That queue is the *only* place with shared mutable
> state, and it's protected by a mutex and a condition variable. Then a
> single worker thread pops events one at a time and runs all the matching.
>
> The key insight is that because only one thread ever mutates the book,
> there are no locks anywhere in the matching hot path. I don't have to lock
> individual price levels or orders — the single-writer design eliminates
> that whole class of race conditions by construction. The trade-off is that
> one book is capped at one core's throughput. I'm fine with that, and the
> way you scale it is to shard by symbol — one book, one queue, one worker
> per symbol — which scales linearly across cores without adding a single
> lock."

[The phrase to land: "single-writer, so no locks on the hot path." Then the
honest trade-off, then the scaling answer. That sequence — claim, cost,
fix — is what makes it sound mature.]

---

## Beat 4 — Performance, measured (don't skip this — it's the differentiator)

> "I didn't want to just *claim* it was fast, so I wrote a micro-benchmark
> that times the real matching call over a million orders and reports p50
> and p99 latency, not just an average.
>
> The headline is the fast path — matching against a deep single price
> level — does around 5 million orders a second at a sub-microsecond median.
> But the more interesting result is that *insertion* is the slow scenario,
> not matching: building a book of a million distinct price levels grows the
> `std::map` into a big red-black tree, so each insert pays O(log P) plus a
> cache-unfriendly tree walk and it drops under a million a second. And
> that's exactly what my complexity audit predicted would be the first
> bottleneck — the map maintenance. So the benchmark didn't just give me a
> number, it confirmed my mental model and pointed at what to optimize next.
>
> I'll caveat that those are single-core laptop numbers that swing run to
> run — I keep them scoped honestly rather than dressing them up."

[If they want to go deeper here, switch to the `benchmark.md` thread.]

---

## Beat 5 — Testing (the credibility beat)

> "On correctness — there are about 166 assertions across fifteen-plus named
> test functions. I test at three layers: each order type in isolation, then
> scenarios like cancel-after-partial-fill and a FOK that can't be filled,
> and then global invariants that have to hold after *any* sequence of
> operations — like 'no empty price levels are ever left behind.' That last
> layer is what catches the subtle matching bugs. There's also an async test
> that hammers the queue from multiple producer threads and checks the
> worker's final state."

[The invariant-testing point is the one that separates you from someone who
just wrote happy-path tests. Lean on it.]

---

## Beat 6 — Honest limitations (say these before they ask)

> "It's a portfolio project, so let me be clear about the edges. Prices are
> `double`, which can drift — a production system would use fixed-point
> integer ticks. The stop-order trigger is a linear scan, O(S), which is
> fine for hundreds of stops but not thousands — a sorted index by trigger
> price fixes that. And the trades vector grows unbounded — in production
> you'd stream them out and cap memory with a ring buffer. None of those are
> hard to fix; I prioritized breadth and correctness first."

[Volunteering limitations *before* being asked is a power move. It signals
you actually understand the system instead of having memorized its good
parts.]

---

## Beat 7 — What's next (forward momentum)

> "Where I'm taking it next: a REST API in front of the engine, multi-symbol
> routing so it's a map of symbol to order book, CI with GitHub Actions, and
> structured logging. After that the interesting stretch goals are a
> write-ahead log for crash recovery, and maybe an LLM tool-use frontend so
> you can talk to the book in natural language."

[Keep this short — it shows the project is alive, not finished-and-forgotten,
without turning into a roadmap monologue.]

---

## Beat 8 — The close (if they ask "anything else?")

> "The thing I'd point to is that every choice in here has a reason I can
> defend and a trade-off I can name — the map for ordering, the intrusive
> list for O(1) cancel, the single writer for lock-free matching, and a
> benchmark that backs the claims. That's really what I was practicing:
> building something I can reason about, not just something that runs."

---

## Cue card — the spine in eight words

**Domain → Data structures → Concurrency → Measured → Tested → Limits → Next → Close.**

If you blank, walk that line. Each word is one beat above.

---

## Three things to never say (carried over from `Notes.md` §9)

- Not "high-frequency trading" — it's a learning engine, different tier.
- Not "lock-free" — the queue uses a mutex; say "single-writer, no locks on
  the hot path."
- Not "blazing fast / production-ready" — say the measured p50/p99 and the
  scope instead.
