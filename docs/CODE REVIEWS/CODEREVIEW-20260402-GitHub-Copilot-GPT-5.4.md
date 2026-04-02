# Code Review — XOPTrader Current Tree
**Date:** 2026-04-02  
**Reviewer:** GitHub Copilot (GPT-5.4)  
**Scope:** Fresh static review of the current repository state on `main`  
**Methodology:** Line-level inspection across configuration, engine, execution, monitoring, and tests. No build or test run was performed in this pass.

---

## Executive Summary

The highest-risk defects in the current tree are concentrated in two areas:

1. **The per-pair denomination contract is broken for non-XCH pairs.** The repository now models per-asset mojo scales, but the YAML loader never populates them and several runtime paths still hardcode `kMojosPerXch`.
2. **The monitoring tail of the engine is still partially disconnected from live state.** Step 11 mark-to-market uses display labels instead of canonical asset IDs, and Step 13 alert evaluation still runs on placeholder inputs for several critical rules.

The result is that the bot may still behave plausibly on `XCH/*CAT*` happy paths while being materially wrong on CAT-base or XCH-quote pairs, and simultaneously misstate risk in the PnL / alerting surfaces.

---

## Findings

### CR-1: The per-pair denomination contract is broken end-to-end for non-XCH pairs
**Severity:** CRITICAL  
**Files:** [config.cpp](../../cpp/src/config.cpp#L420-L590), [config.hpp](../../cpp/include/xop/config.hpp#L118-L122), [base.hpp](../../cpp/include/xop/strategy/base.hpp#L31), [engine.cpp](../../cpp/src/engine.cpp#L1818-L1819), [engine.cpp](../../cpp/src/engine.cpp#L2138-L2144), [engine.cpp](../../cpp/src/engine.cpp#L1445), [offer_manager.cpp](../../cpp/src/execution/offer_manager.cpp#L822-L857), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L871-L876), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1006), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1195-L1234), [config.example.yaml](../../config.example.yaml#L77), [config.example.yaml](../../config.example.yaml#L93)

**Issue:** `PairConfig` now exposes `base_mojos_per_unit` and `quote_mojos_per_unit`, but `parse_pairs()` never reads either field from YAML. Every pair therefore retains the struct defaults of `1e12` for base and `1e3` for quote, regardless of whether the configured pair is CAT/CAT or CAT/XCH.

That would already be dangerous on its own, but the runtime also still hardcodes `kMojosPerXch` in multiple places:

- Step 4 normalizes inventory by `pair_cfg->base_mojos_per_unit`, which is wrong for CAT-base pairs when the loader leaves the default `1e12` in place.
- Step 6 converts every strategy price and size to mojos with `kMojosPerXch`, even though the strategy contract says quotes are in natural `quote-per-base` units.
- Step 2 realized PnL attribution still divides by `kMojosPerXch`.
- `MarketDataFeed` still converts snapshots, competitor prices, and whale sizes with XCH-specific scale assumptions.
- `OfferManager::build_offer_dict()` does use `pair.quote_mojos_per_unit`, but because the loader never populates it, XCH-quoted pairs still carry the wrong denominator into actual offer creation.

**Impact:** configured pairs such as `BYC/wUSDC.b` and `wmilliETH.b/XCH` cannot satisfy a consistent unit model. Depending on the path, the bot can mis-size inventory by $10^9$, mis-encode quote amounts, distort whale / VPIN analytics, and miscompute realized PnL. This is a pre-paper-trading blocker.

---

### CR-2: Step 11 mark-to-market still uses display pair labels instead of canonical asset IDs
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2874-L2887), [pnl.cpp](../../cpp/src/monitoring/pnl.cpp#L700-L704), [config.example.yaml](../../config.example.yaml#L67-L69)

**Issue:** `PnLTracker::mark_to_market()` reconstructs the base asset by splitting `pair_name` at `'/'`, then uses that derived string for the balance and cost-basis callbacks. In the current repo, asset storage is keyed by canonical IDs such as `xch` or 64-hex CAT IDs, not by display labels like `XCH` or `BYC`.

Because the state and inventory lookups return empty defaults on misses, this fails silently instead of loudly. Native XCH already loses on case (`XCH` vs `xch`), and CAT pairs lose entirely because the display token symbol is not the canonical asset ID.

**Impact:** inventory PnL in Step 11 can be understated or zeroed, which propagates into total PnL, drawdown monitoring, persisted snapshots, and operator-visible dashboards.

---

### CR-3: The alert engine still receives synthetic or missing inputs for multiple critical rules
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L3012-L3057), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L668-L825), [alerts.hpp](../../cpp/include/xop/monitoring/alerts.hpp#L137-L152)

**Issue:** `Engine::step_check_alerts()` still assembles a partially fabricated `BotState`:

- `avg_fill_rate_24h` is set equal to the current fill rate.
- `consecutive_offer_failures` is hardcoded to zero.
- `normal_spread_bps` is hardcoded to `100.0`.
- `recent_high` is reset to the current mid each cycle.
- `max_inventory_ratio` is hardcoded to `0.5`.
- `assets` is never populated before the alert rules iterate it.

The alert rules in `alerts.cpp` assume those are real baselines or live measurements.

**Impact:** several safety rules are currently inert or misleading:

- `ExposureBreach` cannot fire from the assembled state.
- `FillRateDrop` permanently sees a ratio of `1.0`.
- `FlashCrash` sees no drawdown because `recent_high == mid` every cycle.
- `UnderwaterPosition` and `ConcentrationBreach` iterate an empty asset vector.
- `OfferCreationFail` is permanently disabled.

This is not just an observability gap; it weakens the repo's intended production safety controls.

---

### CR-4: The inventory dashboard marks every open position as underwater
**Severity:** MEDIUM  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2961)

**Issue:** Step 12 exports:

```cpp
is.underwater = (pos.cost_basis > 0 && pos.balance > 0);
```

That is an existence test, not a valuation test. Any non-empty long position with a recorded basis is flagged as underwater even when the current market price is above basis.

**Impact:** the Prometheus inventory surface cannot distinguish genuinely underwater inventory from healthy inventory, which erodes trust in the dashboard and hides real underwater states in noise.

---

### CR-5: The current test suite does not exercise the failing denomination, PnL, or alert paths
**Severity:** MEDIUM  
**Files:** [cpp/tests/CMakeLists.txt](../../cpp/tests/CMakeLists.txt#L40-L52), [test_advanced_trading.cpp](../../cpp/tests/test_advanced_trading.cpp#L110), [test_whale_detection.cpp](../../cpp/tests/test_whale_detection.cpp#L85)

**Issue:** the test target includes no `PnLTracker` tests, no `AlertManager` state-assembly tests, and no coverage for per-pair denomination parsing. The analytics-heavy tests that do exist are mostly anchored on `"XCH/wUSDC"`, which leaves CAT-base and XCH-quote paths effectively unexercised.

**Impact:** the highest-risk defects in the current tree had no realistic chance of being caught by CI. If the denomination contract is repaired, narrow regression tests should be added immediately around:

- pair denominator parsing,
- Step 6 quote/size conversion for CAT-base and XCH-quote pairs,
- Step 11 mark-to-market with canonical asset IDs,
- Step 13 alert-state assembly.

---

## Positive Observations

- The repo now has the right abstractions for per-pair denominations in [config.hpp](../../cpp/include/xop/config.hpp#L118-L122); the main problem is that the runtime does not consistently honor them yet.
- `OfferManager::build_offer_dict()` is already written to consume `quote_mojos_per_unit`, which means the execution layer is structurally fixable without redesign.
- The high-water-mark initialization fix in Step 13 remains present, so the earlier startup drawdown blind spot does not appear to have regressed in this pass.

---

## Testing Gaps

No build or test run was performed in this review pass. The findings above are all from static inspection, and the current automated tests still leave the highest-risk denomination / monitoring boundaries uncovered.