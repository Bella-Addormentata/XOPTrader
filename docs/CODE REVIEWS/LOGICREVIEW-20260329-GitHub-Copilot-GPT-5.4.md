# Logic Review — XOPTrader Heartbeat and Risk Flow
**Date:** 2026-03-29  
**Reviewer:** GitHub Copilot (GPT-5.4)  
**Scope:** Fresh end-to-end logic review of the 13-step engine cycle and downstream monitoring state  
**Methodology:** Manual trace of data flow through Step 1 to Step 13, with focus on unit contracts, risk-gate dependencies, and monitoring correctness.

---

## Executive Summary

The main trading loop remains structurally sound:

- Step ordering is still coherent.
- Fill handling is non-blocking and pair-aware.
- Strategy isolation per pair is preserved.
- The previously reported inventory-unit mismatch into Avellaneda/GLFT is fixed.

The current logic problems are no longer in the quote-generation core. They are in the **post-trade and monitoring chain**:

1. Step 11 does not mark positions to market using canonical asset IDs.
2. Step 13 constructs a partially synthetic `BotState`, disabling multiple alert rules.
3. Step 1/2 analytics still apply XCH-only normalization on CAT pairs, which poisons downstream toxicity and snapshot consumers.

---

## Findings

### LR-1: Step 11 inventory PnL is disconnected from the actual asset keys
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2585), [engine.cpp](../../cpp/src/engine.cpp#L2589), [engine.cpp](../../cpp/src/engine.cpp#L2594), [pnl.cpp](../../cpp/src/monitoring/pnl.cpp#L696), [state.cpp](../../cpp/src/state.cpp#L399), [inventory.cpp](../../cpp/src/risk/inventory.cpp#L590)

**Trace:**

1. Step 2 records fills against canonical asset IDs, for example `xch` or full CAT hashes.
2. Step 11 calls `pnl_->mark_to_market(...)`.
3. `PnLTracker::mark_to_market()` derives `base_asset` from the pair label by splitting on `'/'`.
4. The engine callbacks then query state and inventory with that derived label.
5. Both stores return default-empty results on misses.

**Consequence:** inventory PnL is zero or understated for any pair whose display label does not equal its canonical asset key. That includes:

- native XCH due to `XCH` vs `xch` casing,
- every CAT pair keyed by 64-hex asset IDs.

**System effect:** drawdown and unrealized PnL logic are no longer operating on the actual held inventory.

---

### LR-2: Step 13 safety monitoring is mostly synthetic state, not live state
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2719), [engine.cpp](../../cpp/src/engine.cpp#L2720), [engine.cpp](../../cpp/src/engine.cpp#L2731), [engine.cpp](../../cpp/src/engine.cpp#L2733), [engine.cpp](../../cpp/src/engine.cpp#L2764), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L667), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L683), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L709), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L732), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L753), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L774), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L819)

**Trace:**

1. Step 13 sets `avg_fill_rate_24h` equal to the current fill rate.
2. It hardcodes `consecutive_offer_failures` to zero.
3. It hardcodes `normal_spread_bps` to `100.0`.
4. It sets `recent_high` equal to the current mid on every cycle.
5. It hardcodes `max_inventory_ratio` to `0.5`.
6. It never populates `bs.assets` before the alert rules consume it.

**Rule-level consequence:**

- fill-rate-drop logic gets a permanent ratio of `1.0` and never alerts,
- flash-crash logic sees no drawdown because `recent_high == mid`,
- exposure-breach logic cannot trip from the assembled state,
- underwater-position and concentration-breach loops iterate an empty asset vector,
- offer-creation-failure logic is permanently disabled,
- spread-widening compares against a fabricated baseline.

**Bottom line:** the alerting layer is wired into the heartbeat, but the state it receives is not yet the engine’s real risk state.

---

### LR-3: CAT-pair unit normalization is inconsistent from Step 1 into Step 11
**Severity:** HIGH  
**Files:** [market_data.cpp](../../cpp/src/execution/market_data.cpp#L835), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L859), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L864), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1183), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1222), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1306), [inventory.cpp](../../cpp/src/risk/inventory.cpp#L270), [engine.cpp](../../cpp/src/engine.cpp#L2615)

**Trace:**

1. Dexie market prices enter `MarketDataFeed` as doubles in quote-per-base terms.
2. `publish_snapshot()` converts those doubles through an XCH-centric `* kMojosPerXch` fixed-point path.
3. Whale detection and related trade-size normalization also divide by `kMojosPerXch`.
4. Step 11 then feeds `mkt.mid_price` into `inventory_->inventory_ratio(...)`, which expects quote-mojo value semantics.

**Dimensional problem:** `inventory_ratio()` multiplies `base_qty * base_price` and compares it to raw quote mojos. For CAT pairs, `base_price` must already be in quote-mojos-per-base-mojo. The snapshot path is not enforcing that contract; it is applying an XCH fixed-point convention instead.

**Consequence:** CAT pair inventory ratios, whale fractions, VPIN volumes, and any snapshot-derived downstream analytics are not trustworthy.

---

### LR-4: Inventory dashboard underwater status is logically inverted into an existence test
**Severity:** MEDIUM  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2668)

**Issue:** Step 12 exports `underwater = (cost_basis > 0 && balance > 0)`. That is equivalent to “position exists with non-zero basis,” not “market price is below basis.”

**Consequence:** the monitoring surface reports nearly every open position as underwater, which defeats the purpose of the field and makes it harder to interpret real underwater risk.

---

## Verified Correct

The following core logic paths were re-checked and remain correct in the current tree:

- Step 2 updates inventory using the pair’s canonical base asset, not the pair label.
- Step 4 converts net inventory from mojos to base units before calling the A-S / GLFT strategy.
- Step 5 still preserves reservation-price skew instead of rebuilding symmetric quotes from the raw market mid.
- Step 8 remains fully asynchronous and no longer uses the old `co_spawn(..., use_future).get()` deadlock pattern.
- GUI startup now fails closed when backend initialization fails.

---

## Conclusion

The trading loop’s quoting path is materially healthier than in the earlier review cycle. The remaining logic defects are concentrated in **measurement and control surfaces** rather than in quote construction itself.

That distinction matters operationally:

- the engine can still quote,
- but the unrealized PnL, alerting, and CAT-pair analytics can misstate risk or suppress safety signals.

Those should be treated as pre-paper-trading blockers because they directly affect operator visibility and automated safety responses.
