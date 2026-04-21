# Interview Questions — Full Project (Current + 4 Planned Additions)

Same format as `question1.md`, but assumes you've shipped the 4 core
additions from `final.txt`:

  1. REST API (cpp-httplib)
  2. Multi-symbol routing (`unordered_map<symbol, OrderBook>`)
  3. GitHub Actions CI
  4. Structured logging (spdlog)

Threads 1–9 from `question1.md` still apply — the new ones below (10–14)
are the *additional* attack surface you open up by shipping these features.
Be ready for all of them.

Markers: **[CS]** = CS fundamentals pivot, **[SHARP]** = depth-probe.

---

## Thread 10 — REST API Design

**Q1.** Walk me through your REST layer.

**Q2.** Why REST and not gRPC?

**Q3.** How do you parse the JSON body? Any validation?

**Q4.** Two users `POST /orders` at the same instant. What happens?

**Q5 [SHARP].** The HTTP handler is on thread A; matching happens on
thread B. How does the HTTP response get the result back?

**Q6.** What status codes do you use, and for what?

**Q7 [CS].** What's the difference between HTTP 400, 404, 409, and 422?

**Q8.** How would you add rate limiting without breaking existing
clients?

**Q9 [SHARP].** What if a client sends a cancel before their order has
been processed by the worker?

---

## Thread 11 — Multi-Symbol Routing

**Q1.** How does the engine know which book an order belongs to?

**Q2.** Why `unordered_map` and not a BST?

**Q3 [CS].** What's the difference in cache behavior between
`unordered_map` and `std::map` when there are thousands of symbols?

**Q4.** What if you wanted per-symbol parallelism — one worker thread
per symbol?

**Q5.** Does ordering still hold across symbols?

**Q6 [CS].** What is "sharding" in a system-design sense, and how is
what you built similar?

**Q7 [SHARP].** Two symbols hash to the same bucket. Is that a
correctness problem or a performance problem?

---

## Thread 12 — CI / GitHub Actions

**Q1.** Walk me through your CI.

**Q2.** What triggers the workflow?

**Q3.** How long does it take? Would you be OK if it were 10 minutes?

**Q4.** A test starts failing intermittently. How do you triage it?

**Q5 [SHARP].** Suppose you changed the matching logic and a *test*
failed, but the test looked wrong to you. What do you do?

**Q6.** Do you have CD (continuous deployment) too?

**Q7 [CS].** What's the difference between CI and CD? What does "trunk-
based development" mean?

---

## Thread 13 — Structured Logging

**Q1.** How do you log? Why structured?

**Q2.** Why did you choose spdlog?

**Q3.** What does your log line look like?

**Q4 [SHARP].** Logging in the matching hot path — doesn't that slow the
engine down?

**Q5.** How would you scale to gigabytes of logs per day?

**Q6 [CS].** What's the difference between INFO, WARN, and ERROR log
levels? When would you pick each?

**Q7.** Sensitive data in logs — how do you avoid leaking it?

---

## Thread 14 — Cross-Cutting / System Design

**Q1.** You've got an API, a worker, a log, a book, and now symbols.
Draw the architecture on a whiteboard.

**Q2.** Where's the single point of failure?

**Q3.** If the worker thread crashes mid-match, what's the state of
the book?

**Q4 [SHARP].** Would persistence fix that? What would you persist and
when?

**Q5.** If I deployed this in two regions and wanted low-latency reads,
how would you structure it?

**Q6 [CS].** Explain CAP theorem, and which two does your system favor?

**Q7.** Where does this architecture break first as load grows?

---

## Thread 15 — Lessons & Reflection (senior probe)

These are "soft" questions that test maturity. Interviewers love them
because the answers reveal how you think.

**Q1.** What's the biggest design mistake you made along the way?

**Q2.** If you rewrote this from scratch, what would you do differently?

**Q3.** What's the weakest part of this project?

**Q4.** What did you learn about C++ specifically from building this?

**Q5.** Why did you build a matching engine instead of a web app?

**Q6 [SHARP].** Be honest — what part of this project would you have
trouble explaining if I grilled you for 5 minutes on it?

*(Tip: for Q6, name something small, own it, and say what you'd do to
fix it. This is a confidence signal, not a weakness signal.)*
