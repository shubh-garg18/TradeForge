# Order Types Reference

All orders are submitted through `engine.process_order(Order*)`, which dispatches by `OrderType`. Stop orders use `engine.process_stop_order(Order*)` instead.

---

## LIMIT

Matches against the opposite side at the limit price or better. Any unfilled remainder rests on the book in FIFO order within its price level.

**Rests on book:** yes  
**Price required:** yes

**Status transitions:**

```
CREATED → OPEN              (no immediate fill)
OPEN    → PARTIALLY_FILLED  (partial fill, remainder rests)
OPEN    → COMPLETED         (full fill on arrival)
OPEN / PARTIALLY_FILLED → CANCELLED  (explicit cancel)
```

---

## MARKET

Sweeps the opposite side at any price until filled or liquidity is exhausted. Never rests. If the book runs dry before the order is fully filled, the remainder is dropped with status `PARTIALLY_FILLED`.

**Rests on book:** no  
**Price required:** no

**Status transitions:**

```
CREATED → COMPLETED        (fully filled)
CREATED → PARTIALLY_FILLED (exhausted liquidity, remainder dropped)
CREATED → CANCELLED        (zero liquidity available)
```

---

## IOC (Immediate-or-Cancel)

Matches at the limit price or better. Any unfilled remainder is cancelled immediately — it never rests. Semantically a limit order that self-cancels on partial fill.

**Rests on book:** no  
**Price required:** yes

**Status transitions:**

```
CREATED → COMPLETED        (fully filled)
CREATED → PARTIALLY_FILLED (partial fill, remainder cancelled)
CREATED → CANCELLED        (nothing crossed at limit price)
```

---

## FOK (Fill-or-Kill)

Pre-scans the book to verify the full quantity can be filled at the limit price or better. If not, the order is cancelled immediately with zero fills and the book is not mutated. If the pre-scan passes, runs the standard matching loop for a guaranteed full fill.

The pre-scan costs roughly 2× traversal but avoids rollback entirely.

**Rests on book:** no  
**Price required:** yes

**Status transitions:**

```
CREATED → COMPLETED  (pre-scan passed, fully filled)
CREATED → CANCELLED  (pre-scan failed, zero fills)
```

---

## STOP_LOSS

Rests in `pending_stops` (not on the book) until the trigger condition is met, then converts to a `MARKET` order and executes immediately.

**Trigger condition:**

- BUY stop: `last_trade_price >= stop_price`
- SELL stop: `last_trade_price <= stop_price`

Multiple stops at the same `stop_price` are triggered in FIFO insertion order.

**Rests on book:** no (waits in `pending_stops`)  
**Price required:** no (`price` field is unused)  
**`stop_price` required:** yes

**Status transitions:**

```
CREATED → OPEN       (waiting in pending_stops)
OPEN    → COMPLETED / PARTIALLY_FILLED / CANCELLED  (after trigger, same as MARKET)
```

---

## STOP_LIMIT

Same trigger logic as `STOP_LOSS`. On trigger, converts to a `LIMIT` order and is processed normally. May partially fill or rest on the book if insufficient liquidity is available at the limit price.

**Rests on book:** yes, after trigger (as a LIMIT order)  
**Price required:** yes (the limit price to rest/match at after trigger)  
**`stop_price` required:** yes

**Status transitions:**

```
CREATED → OPEN                              (waiting in pending_stops)
OPEN    → OPEN / PARTIALLY_FILLED / COMPLETED / CANCELLED  (after trigger, same as LIMIT)
```

---

> **Known limitation:** a pending (untriggered) stop order cannot be cancelled. `cancel_order` only covers orders resting on the book — `pending_stops` has no cancellation path yet. Tracked alongside the planned `StopOrderManager` refactor.

---

## Comparison Table

| Type       | Rests | Guaranteed fill | Cancels remainder | Stop condition |
| ---------- | ----- | --------------- | ----------------- | -------------- |
| LIMIT      | yes   | no              | no                | —              |
| MARKET     | no    | no              | yes (on dry book) | —              |
| IOC        | no    | no              | yes               | —              |
| FOK        | no    | yes or cancel   | n/a               | —              |
| STOP_LOSS  | no    | no              | yes (on dry book) | price trigger  |
| STOP_LIMIT | yes   | no              | no                | price trigger  |