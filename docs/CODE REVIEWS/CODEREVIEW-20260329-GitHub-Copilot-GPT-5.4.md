# Code Review — XOPTrader Current Tree
**Date:** 2026-03-29  
**Reviewer:** GitHub Copilot (GPT-5.4)  
**Scope:** Fresh static review of the current repository state, with explicit re-validation of previously reported findings before reuse  
**Methodology:** Line-level source inspection across the C++ engine, monitoring, market-data, configuration, and GUI layers. No build or test run was performed in this pass.

---

## Executive Summary

The core engine wiring, async RPC architecture, per-pair strategy isolation, and most previously critical fixes remain intact. The current issues are concentrated in three places:

1. **PnL mark-to-market is keyed off pair labels instead of canonical asset IDs**, which breaks inventory PnL attribution.
2. **The alert-state assembly still uses placeholder or missing inputs**, leaving several safety alerts inert or misleading.
3. **The market-data layer still hardcodes XCH denomination assumptions on multi-asset paths**, which distorts CAT-pair analytics and any downstream consumer that trusts those normalized values.

No evidence was found for several older findings that were re-checked in this pass: the `pyproject.toml` backend and Python bounds are fixed, GUI startup initialization now fails closed, the example config has been corrected, and the prior production-`assert()` complaints no longer reproduce in the current source.

---

## Findings

### CR-1: Mark-to-market inventory PnL uses pair labels instead of canonical asset IDs
**Severity:** HIGH  
**Files:** [pnl.cpp](../../cpp/src/monitoring/pnl.cpp#L696), [engine.cpp](../../cpp/src/engine.cpp#L2585), [engine.cpp](../../cpp/src/engine.cpp#L2589), [engine.cpp](../../cpp/src/engine.cpp#L2594), [state.cpp](../../cpp/src/state.cpp#L399), [inventory.cpp](../../cpp/src/risk/inventory.cpp#L590)

**Issue:** `PnLTracker::mark_to_market()` derives the base asset as the substring before `'/'` in `pair_name`, for example `"XCH"`, `"BYC"`, or `"wmilliETH.b"`. The engine callbacks then look up balances and cost basis using that derived string as an `AssetId`.

That does not match the actual runtime keys:
- the state position map is keyed by canonical asset IDs such as `"xch"` or 64-hex CAT IDs;
- the inventory tracker is keyed the same way.

Because `State::get_position()` and `InventoryTracker::get_record()` both return empty defaults on misses, the mark-to-market path silently sees zero balances or zero cost basis for many pairs. For CAT pairs it is guaranteed wrong; even native XCH is vulnerable because the pair label uses `"XCH"` while the runtime asset ID is `"xch"`.

**Impact:** Inventory PnL is underreported or zeroed, which feeds directly into:
- total PnL,
- drawdown analytics,
- monitoring dashboards,
- any operator decision that depends on unrealized risk.

**Why this is a root-cause bug:** the code already has canonical pair-to-asset mapping in `config_.pairs`; this path bypasses it and reconstructs an asset key from the human-readable pair label.

---

### CR-2: AlertManager receives placeholder or missing state for several critical rules
**Severity:** HIGH  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2719), [engine.cpp](../../cpp/src/engine.cpp#L2720), [engine.cpp](../../cpp/src/engine.cpp#L2731), [engine.cpp](../../cpp/src/engine.cpp#L2733), [engine.cpp](../../cpp/src/engine.cpp#L2764), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L667), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L713), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L736), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L753), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L774), [alerts.cpp](../../cpp/src/monitoring/alerts.cpp#L819)

**Issue:** `Engine::step_check_alerts()` still populates large parts of `BotState` with placeholders rather than live values:

- `avg_fill_rate_24h = fill_rate_per_hour`
- `consecutive_offer_failures = 0`
- `normal_spread_bps = 100.0`
- `recent_high = current mid`
- `max_inventory_ratio = 0.5`
- `assets` is not populated at all before rules iterate it

The alert rules themselves assume these fields are real baselines or live risk measurements.

**Impact by rule:**
- `ExposureBreach` will never fire from the assembled `BotState` because `0.5` is below the configured hard limit in the default config.
- `FillRateDrop` becomes impossible because the numerator and denominator are equal, so the ratio is always `1.0`.
- `SpreadWidening` compares against a fabricated 100 bps baseline, producing false negatives on naturally wide pairs and false positives on normally tight pairs.
- `FlashCrash` cannot detect drawdowns because `recent_high` is reset to `mid_price` each cycle.
- `UnderwaterPosition` and `ConcentrationBreach` are dead because `state.assets` is empty.
- `OfferCreationFail` cannot fire because the counter is hardcoded to zero.

**Why this matters:** this subsystem exists to surface production safety conditions. In the current state, several of the highest-value alerts are either permanently disabled or driven by fabricated baselines.

---

### CR-3: CAT-pair analytics still use XCH-specific denomination assumptions
**Severity:** HIGH  
**Files:** [market_data.cpp](../../cpp/src/execution/market_data.cpp#L835), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L859), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L864), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1183), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1222), [market_data.cpp](../../cpp/src/execution/market_data.cpp#L1306), [inventory.cpp](../../cpp/src/risk/inventory.cpp#L270), [engine.cpp](../../cpp/src/engine.cpp#L2615)

**Issue:** multiple market-data paths still normalize prices, volumes, and trade sizes using `kMojosPerXch` for every pair:

- `publish_snapshot()` converts `mid_price` and `volume_24h` through an XCH fixed-point scale;
- whale detection converts trade `size` to units with `/ kMojosPerXch`;
- whale logs use the same XCH-only interpretation.

That is inconsistent with the pair model already present in configuration, where CAT pairs carry explicit `base_mojos_per_unit` and `quote_mojos_per_unit` values.

**Impact:** for CAT/XCH and CAT/CAT pairs, the following become dimensionally inconsistent:
- whale-size thresholds and size-as-fraction-of-volume,
- VPIN bar volume accumulation,
- persisted `MarketSnapshot` fields,
- snapshot-based `inventory_ratio` calculations that consume `mkt.mid_price` as though it were a quote-mojo price.

This repo is explicitly configured for non-XCH pairs in [config.example.yaml](../../config.example.yaml), so this is not a dormant edge case.

---

### CR-4: Inventory dashboard marks every non-empty position as underwater
**Severity:** MEDIUM  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L2668)

**Issue:** the inventory metrics export sets:

```cpp
is.underwater = (pos.cost_basis > 0 && pos.balance > 0);
```

That is not an underwater test. It only checks whether a position exists and has a non-zero cost basis. Any long position with recorded basis is exported as underwater even when current market price is above basis.

**Impact:** the Prometheus inventory view cannot distinguish healthy positions from underwater ones, which weakens operator trust in the monitoring surface and masks real underwater states in noise.

---

### CR-5: Viterbi backtracking still relies on unsigned wraparound termination
**Severity:** LOW  
**Files:** [regime.cpp](../../cpp/src/strategy/regime.cpp#L822), [regime.cpp](../../cpp/src/strategy/regime.cpp#L1172)

**Issue:** both loops use the `for (std::size_t t = T - 2; t < T; --t)` idiom and terminate only after unsigned underflow wraps `t` above the bound.

This is well-defined for unsigned integers, but it is non-obvious and brittle under maintenance or static-analysis review.

**Impact:** readability and maintainability risk only. No immediate behavioral bug was confirmed.

---

## Re-validated Non-Findings

The following older issues were explicitly checked and are **not** current findings in this pass:

- [pyproject.toml](../../pyproject.toml) now uses `setuptools.build_meta` and bounded Python dependencies.
- [gui/main.py](../../gui/main.py) now catches service initialization failures and exits with a modal startup error.
- [config.example.yaml](../../config.example.yaml) no longer uses the old truncated asset IDs / invalid fingerprint placeholder / inconsistent tier spacing example.
- A repo-wide search in `cpp/` no longer shows live production `assert(...)` usage in the previously flagged files.

---

## Testing Gaps

No evidence was found of tests that specifically lock down the currently failing paths:

- `PnLTracker::mark_to_market()` with canonical asset IDs that differ from display pair labels
- alert-rule assembly from `Engine::step_check_alerts()`
- CAT-pair whale / VPIN / snapshot unit normalization
- dashboard `underwater` export semantics

These are good candidates for narrow regression tests once the fixes are implemented.
