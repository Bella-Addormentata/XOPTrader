# Logic Review — XOPTrader Heartbeat, Units, and Risk Flow
**Date:** 2026-04-02  
**Reviewer:** GitHub Copilot (GPT-5.4)  
**Scope:** Fresh end-to-end logic review of the live 13-step engine cycle and downstream monitoring state  
**Methodology:** Manual trace through Step 2, Step 4, Step 6, Step 8, Step 11, Step 12, and Step 13, with emphasis on unit contracts, canonical asset identity, and safety-rule activation. No build or test run was performed in this pass.

---

## Executive Summary

The heartbeat sequencing is still broadly coherent, but the current tree has two logic breaks that matter operationally:

1. **Non-XCH pair units are not conserved across the cycle.** The repo models per-pair denominations, but the loader and several runtime boundaries still behave as if every pair were XCH-denominated.
2. **The measurement and control tail is still disconnected from live state.** PnL mark-to-market and alert evaluation do not consistently operate on the same canonical asset model that the fill-processing path uses.

That means the bot can continue to quote and update state while still mis-sizing non-XCH trades and understating or suppressing risk signals.

---

## Findings

### LR-1: The unit contract for non-XCH pairs breaks across Step 2, Step 4, Step 6, Step 8, and market-data
**Severity:** CRITICAL  
**Files:** [config.cpp](../../cpp/src/config.cpp#L420-L590), [config.hpp](../../cpp/include/xop/config.hpp#L118-L122), [base.hpp](../../cpp/include/xop/strategy/base.hpp#L31), [engine.cpp](../../cpp/src/engine.cpp#L1818-L1819), [engine.cpp](../../cpp/src/engine.cpp#L2138-L2144), [engine.cpp](../../cpp/src/engine.cpp#L1445), [offer_manager.cpp](../../cpp/src/execution/offer_manager.cpp#L822-L857), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L871-L876), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1006), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1195-L1234), [config.example.yaml](../../config.example.yaml#L77), [config.example.yaml](../../config.example.yaml#L93)

**Trace:**

1. `PairConfig` declares `base_mojos_per_unit` and `quote_mojos_per_unit`, but `parse_pairs()` never populates them from YAML.
2. Step 4 normalizes inventory `q` by `pair_cfg->base_mojos_per_unit`, so CAT-base pairs inherit the wrong scale before quote generation even starts.
3. Step 6 then converts strategy prices and sizes with `kMojosPerXch` for every pair, even though the strategy layer advertises prices and sizes in natural `quote-per-base` units.
4. Step 8 / `OfferManager` converts quote amounts using `pair.quote_mojos_per_unit`, but because the loader left the default in place, XCH-quoted pairs still carry the wrong denominator into wallet RPC encoding.
5. Market-data snapshots, competitor metrics, whale sizing, and realized PnL attribution also keep using XCH-specific scale assumptions.

**Consequence:** pairs such as `BYC/wUSDC.b` and `wmilliETH.b/XCH` do not preserve a consistent monetary unit across ingest, quote construction, offer encoding, fill attribution, or analytics. In practice that means prices, sizes, PnL, and toxicity metrics can all be wrong by orders of magnitude. This should be treated as a pre-paper-trading blocker.

---

### LR-2: Step 11 inventory PnL is disconnected from the canonical asset keys used elsewhere in the engine
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2874-L2887), [pnl.cpp](../../cpp/src/monitoring/pnl.cpp#L700-L704)

**Trace:**

1. Step 2 and the inventory tracker record holdings against canonical asset IDs such as `xch` or full CAT hashes.
2. Step 11 calls `pnl_->mark_to_market()`.
3. `PnLTracker::mark_to_market()` splits the human-readable pair label and uses the substring before `'/'` as the asset key.
4. The callbacks then query state and inventory with that display label rather than the canonical configured asset ID.

**Consequence:** Step 11 can report zero or understated inventory PnL for native XCH and effectively every CAT pair. The engine's unrealized PnL, drawdown inputs, and persisted snapshot summaries are therefore not aligned with the actual held inventory.

---

### LR-3: Step 13 alerting still runs on synthetic state, so several safety rules cannot fire correctly
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L3012-L3057), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L668-L825), [alerts.hpp](../../cpp/include/xop/monitoring/alerts.hpp#L137-L152)

**Trace:**

1. Step 13 sets `avg_fill_rate_24h = fill_rate_per_hour`.
2. It hardcodes `consecutive_offer_failures = 0`.
3. It hardcodes `normal_spread_bps = 100.0`.
4. It resets `recent_high = mid_price` every cycle.
5. It hardcodes `max_inventory_ratio = 0.5`.
6. It never populates `bs.assets` before the alert rules iterate it.

**Rule-level consequence:**

- `FillRateDrop` permanently compares the current fill rate with itself.
- `FlashCrash` sees no drawdown window.
- `ExposureBreach` cannot trip from the assembled state.
- `UnderwaterPosition` and `ConcentrationBreach` iterate an empty asset list.
- `OfferCreationFail` is effectively disabled.

**Bottom line:** the alert engine is present in the heartbeat, but several of its highest-value rules are still not operating on live engine state.

---

### LR-4: Step 12 exports “underwater” as “position exists,” not “market is below basis”
**Severity:** MEDIUM  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2961)

**Issue:** Step 12 marks a position as underwater whenever `cost_basis > 0 && balance > 0`.

**Consequence:** the monitoring surface reports nearly every open long as underwater, which destroys the semantic value of the metric and makes it harder for an operator to spot truly trapped inventory.

---

## What Appears Sound

- Per-pair strategy instances and canonical asset IDs are still the intended architecture; the engine is not suffering from cross-pair state bleed.
- The high-water-mark seeding fix for drawdown protection remains in place, so the earlier startup blind spot does not appear to have regressed.
- The execution layer already has a place to consume per-pair quote denominations; the main logic problem is that the denomination contract is not consistently carried through the full cycle.

---

## Operational Conclusion

The quoting loop is not structurally chaotic; the current failures are concentrated at **module boundaries**:

- natural-unit strategy output to mojo-denominated execution,
- canonical asset IDs to human-readable pair labels,
- live engine state to monitoring / alert evaluation.

Those boundary failures matter more than a cosmetic bug because they affect whether the bot sizes orders correctly and whether operators can trust the risk signals that follow. I would treat the denomination issue and the Step 11 / Step 13 monitoring disconnects as blockers before any serious live or paper deployment of non-XCH pairs.

---

## Testing Gaps

The current tests still do not exercise non-XCH denomination handling, `PnLTracker::mark_to_market()`, or `AlertManager` state assembly. Those should be the first regression tests added once fixes begin.