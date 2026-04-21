# Interview Questions — Current Project State (as of 2026-04-21)

Realistic FAANG / Big-Tech SDE interview threads for your project **as it
stands today** (before adding REST API, multi-symbol, CI, and logging).

Each thread starts open-ended and drills down. Some follow-ups pivot into
CS fundamentals — those are marked **[CS]**. Some are "sharp" follow-ups
an interviewer uses to probe depth — marked **[SHARP]**.

---

## Thread 1 — Project Walkthrough & Data-Structure Choices

**Q1.** Walk me through your project.

**Q2.** You said you use `std::map` for price levels and `std::unordered_map`
for the order-ID lookup. Why two different structures?

**Q3.** What's the complexity of inserting a *new* price level versus
inserting an order at an *existing* price level?

**Q4 [CS].** How is `std::map` implemented under the hood? Does the C++
standard actually mandate a specific data structure?

**Q5.** You mentioned cached `best_bid` and `best_ask` pointers. How do
they stay correct?

**Q6 [SHARP].** What happens to `best_bid` if the entire best-bid level
gets consumed in a single match?

---

## Thread 2 — Cancel-Order Flow

**Q1.** A client sends a cancel for an order. Walk me through what happens
inside the engine.

**Q2.** Where does the O(1) for cancel come from?

**Q3.** What if the order ID doesn't exist?

**Q4 [CS].** How does `std::unordered_map` handle hash collisions, and
what's the worst-case complexity of a lookup?

**Q5.** Once you've found the `Order*`, how do you remove it from the
`PriceLevel`'s list in O(1)?

**Q6 [CS].** What *is* an intrusive linked list, and why use one over a
`std::list<Order>`?

---

## Thread 3 — Concurrency Model

**Q1.** Multiple clients submit orders concurrently — how does your engine
handle that without data races?

**Q2.** Why a single worker thread? Aren't you wasting the other cores?

**Q3.** Walk me through what happens when `EventQueue` is empty and a new
order arrives.

**Q4 [CS].** What does `std::condition_variable::wait` do at the OS level?
Is the thread actually sleeping, or spinning?

**Q5 [CS].** What is a spurious wakeup, and how do you guard against it?

**Q6 [SHARP].** Two producer threads call `push()` at exactly the same
moment. What happens?

**Q7.** If you had to handle 100k orders/sec, would this architecture
still work?

---

## Thread 4 — FOK vs IOC Semantics

**Q1.** What's the difference between an IOC and a FOK order?

**Q2.** How do you implement FOK's "all-or-nothing" guarantee?

**Q3.** Why not just try to fill it and roll back if the fill was partial?

**Q4.** What's the extra cost of your pre-scan approach?

**Q5 [SHARP].** Is there any scenario where your FOK could end up executing
partially?

---

## Thread 5 — Stop Orders

**Q1.** How do stop-loss and stop-limit orders work in your engine?

**Q2.** When does a pending stop actually trigger?

**Q3.** So after every trade, you scan all pending stops. What's that
complexity?

**Q4.** How would you improve that?

**Q5 [SHARP].** Why haven't you done that optimization yet?

---

## Thread 6 — Testing Strategy

**Q1.** How do you test a matching engine?

**Q2.** You have 166 assertions — how are they organized?

**Q3.** Do you test the multi-threaded event queue?

**Q4 [CS].** What is a "flaky test" and why are concurrency tests
especially prone to it?

**Q5 [SHARP].** How do you test that a FOK which *cannot* be filled gets
rejected cleanly with no side-effects?

---

## Thread 7 — C++ Memory & Ownership

**Q1.** Who owns the `Order` objects in your system?

**Q2.** You use raw pointers — why not `std::shared_ptr` or `std::unique_ptr`?

**Q3.** When does an `Order` get deleted?

**Q4 [CS].** Explain RAII.

**Q5 [CS].** What is the "rule of 0, 3, and 5"? Which does your `Order`
class follow?

**Q6 [SHARP].** Is there any chance of a double-delete or use-after-free
in your current design?

---

## Thread 8 — Matching Algorithm

**Q1.** Explain price-time priority. Why is it fair?

**Q2.** A market buy of 1000 shares arrives. Walk me through the steps.

**Q3.** What's the complexity of matching?

**Q4 [SHARP].** Can matching starve orders at worse prices indefinitely?

**Q5.** How are maker and taker fees decided *during* matching?

---

## Thread 9 — Trade Publishing & Market Data

**Q1.** After a match, how do downstream consumers hear about the trade?

**Q2.** Why did you introduce an abstract `TradePublisher` interface
rather than writing trades directly?

**Q3 [CS].** That sounds like a design pattern — which one?

**Q4.** How does your L2 snapshot work, and what's its cost?

**Q5 [SHARP].** If a client polled L2 every millisecond, would the engine
slow down?
