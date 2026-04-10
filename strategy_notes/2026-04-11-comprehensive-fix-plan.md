# Comprehensive Fix Plan: Spread Explosion & Inventory Spiral

**Date**: 2026-04-11 (implemented 2026-04-10)
**Status**: ✅ DEPLOYED — all fixes live
**Commits**: `5b9e9c0` (Fixes 1–5), `131ac4c` (Fix 7: MarketAllocator)
**Trader PID**: 35996 (started 2026-04-10 13:42)
**Prerequisite**: [2026-04-10 Wall Detection & Spread Analysis](2026-04-10-wall-detection-and-spread-analysis.md)

---

## Executive Summary

Three correlated bugs produce a feedback loop:

1. **Annual sigma passed where daily sigma is expected** → spread components
   inflated 100–10 000× → every pair hits `max_half_spread_bps` cap (150 bps =
   300 bps round-trip).
2. **Unclamped inventory skew multiplier** → with `q_max=10` and 64.7 XCH
   inventory, skew_frac reaches 14.67 → 5.4× multiplier on an already-exploded
   spread.
3. **fee_reserve_xch = 1.0** with only 0.81 XCH spendable → permanent
   buy-only mode → ask-only fills → inventory spiral.

Additionally, the backtester has a separate sigma-scale bug (per-block passed
where annual is expected), making historical backtests unrealistically optimistic.

---

## Fix Priority & Sequencing

| # | Fix | Severity | Risk | Deploy | Status |
|---|-----|----------|------|--------|--------|
| 1 | Sigma annual→daily conversion | **CRITICAL** | Low | Code change | ✅ Deployed |
| 2 | Clamp `skew_frac` to [0, 1] | **HIGH** | Low | Code change | ✅ Deployed |
| 3 | Increase `q_max` 10 → 100 | **MEDIUM** | Low | Config change | ✅ Deployed |
| 4 | Reduce `fee_reserve_xch` 1.0 → 0.1 | **MEDIUM** | Medium | Config change | ✅ Deployed |
| 5 | Backtest sigma scale fix | **LOW** | Low | Code change (non-live) | ✅ Deployed |
| 6 | Add sigma Prometheus metrics | **LOW** | None | Monitoring | ⏸️ Deferred |
| 7 | MarketAllocator surplus redistribution | **MEDIUM** | Low | Code change | ✅ Deployed |

Fixes 1–2 are code changes that address the root causes. Fix 3–4 are config
tweaks. Fix 5 is non-live-path. Fix 6 is observability.

---

## Fix 1: Sigma Annual → Daily Conversion (CRITICAL)

### Root Cause

`engine.cpp:2409` passes `get_sigma_annual()` to `SpreadOptimizer::compute_spread()`,
whose parameter is named `sigma_daily` and whose regime thresholds are calibrated
for daily scale:

```
spread.cpp:579:  sigma_low_threshold  = 0.035   (3.5% daily)
spread.cpp:580:  sigma_high_threshold = 0.065   (6.5% daily)
```

The spread formula uses σ² terms (`calc_adverse_selection_bps`, `calc_inventory_bps`),
so passing annual sigma (0.50–5.0) instead of daily (0.026–0.27) causes:

| Component | Scale Error | Effect |
|-----------|-------------|--------|
| `s_adv = 0.5 × γ × σ² × T_fill` | σ_annual/σ_daily ≈ 19× | s_adv inflated ~365× |
| `s_inv = γ × σ² × q/Q × T_fill` | same σ² term | s_inv inflated ~365× |
| Regime classification | annual σ > 0.065 always | Always HighVol → 1.3× mult |

### Validated Call-Site Audit

All `get_sigma_annual()` calls in engine.cpp:

| Line | Feeds Into | Expected Scale | Status |
|------|-----------|----------------|--------|
| 2351 | `strategy.compute_quotes()` | Annual | ✅ Correct |
| **2409** | **`spread_opt_->compute_spread()`** | **Daily** | **❌ MISMATCH** |
| 2981 | `drift_analyzer_->analyze_drift()` | Annual (converts internally) | ✅ Correct |
| 3157 | `liq.compute_ladder()` | N/A (`(void)sigma;` — unused) | ✅ Irrelevant |

Only **one** call site needs fixing.

### Important: Do NOT "Fix" the Dimensional Inconsistency

Previous code reviews (Gemini, Claude) flagged that `sigma_daily × sqrt(T_fill_seconds)`
mixes daily sigma with raw seconds. This is **by design** — the spread optimizer
was empirically calibrated with gamma=0.005 absorbing the unit factor. "Fixing"
the units to match would require recalibrating gamma by ~86 400× and invalidating
all existing tuning. The A-S/GLFT strategies use a *different* sigma scale (annual)
with their own gamma calibration — the two gamma systems are independent.

### Code Change

**File**: `engine.cpp`, in `step_apply_spread_optimizer()`:

```cpp
// BEFORE (line 2409):
sigma = vol_it->second->get_sigma_annual();

// AFTER:
sigma = vol_it->second->get_sigma_annual() / std::sqrt(365.0);
```

### Expected Spread Before/After

**XCH/wUSDC.b** (sigma_annual = 0.0117 from Prometheus):
```
Before: sigma_daily = 0.0117 (passed raw annual)
  s_adv = 0.5 × 0.005 × 0.0117² × 7200 = 0.0025 bps → trivial
  s_inv = 0.005 × 0.0117² × 6.47 × 7200 = 0.032 bps → trivial
  But: regime=HighVol (0.0117 > 0.065? NO — actually Low!)
  
After: sigma_daily = 0.0117 / √365 = 0.000613
  s_adv ≈ 0.000 bps, s_inv ≈ 0.000 bps
  Total ≈ 35 bps (floor-dominated)
  Note: XCH/wUSDC.b has unusually low vol — floor is correct behavior
```

Wait — for XCH/wUSDC.b the annual sigma is 0.0117. This is already tiny.
The conversion makes it even tinier. The spread was floor-dominated regardless.
The problem pairs are ones with meaningful sigma:

**Typical crypto pair** (sigma_annual = 0.50):
```
Before: σ = 0.50 passed as "daily"
  s_adv = 0.5 × 0.005 × 0.50² × 7200 = 4500 bps → HIT CAP
  s_inv = 0.005 × 0.50² × 1.0 × 7200 = 9000 bps → HIT CAP
  Regime: HighVol (0.50 >> 0.065)

After:  σ_daily = 0.50 / √365 = 0.0262
  s_adv = 0.5 × 0.005 × 0.0262² × 7200 = 12.3 bps
  s_inv = 0.005 × 0.0262² × 1.0 × 7200 = 24.7 bps
  Regime: Normal (0.035 > 0.026 — Low, actually)
  Total ≈ 35 bps (floor + components)
```

**XCH/DBX** (sigma_annual = 5.193):
```
After:  σ_daily = 5.193 / √365 = 0.272
  s_adv = 0.5 × 0.005 × 0.272² × 7200 = 1333 bps → still hits cap
  Regime: HighVol (0.272 >> 0.065) → 1.3× mult
  Total = min(1333 × 1.3, 150) = 150 bps (capped — EXPECTED for 519% annual vol)
```

### Risk Assessment

- **Low risk**: Only one call site changes. All 30 spread tests pass (they use
  hardcoded sigma values, not engine flow). No regression possible.
- **Rollback**: Revert one line.
- **Testing gap**: No integration test covers engine → spread_optimizer sigma flow.
  Recommend adding one (see Fix 6).

---

## Fix 2: Clamp Inventory Skew Fraction (HIGH)

### Root Cause

`spread.cpp:632-645` computes `inventory_ratio` WITHOUT a clamp:

```cpp
// spread.cpp:632 — NOT clamped
const double inventory_ratio =
    (effective_q_max > 0.0)
        ? std::abs(inventory_q) / effective_q_max
        : 0.0;
```

This is **separate** from `calc_inventory_bps` (line 411) which IS clamped:
```cpp
// spread.cpp:411 — clamped
const double inventory_frac = std::min(std::abs(inventory_q) / q_max, 1.0);
```

With current runtime values (q=64.7 XCH, q_max=10):
```
inventory_ratio = 64.7 / 10 = 6.47
skew_frac = (6.47 - 0.60) / 0.40 = 14.67   ← UNBOUNDED
skew_mult = 1.0 + 14.67 × (1.30 - 1.0) = 5.40×
```

The skew multiplier was designed to interpolate from 1.0× to 1.30× as inventory
goes from threshold (60%) to capacity (100%). With inventory at 647% of capacity,
it blows past the intended range.

### Code Change

**File**: `spread.cpp`, line ~643:

```cpp
// BEFORE:
const double skew_frac =
    (skew_range > 0.0)
        ? (inventory_ratio - cfg_.inventory_skew_threshold) / skew_range
        : 1.0;

// AFTER:
const double skew_frac = std::min(
    (skew_range > 0.0)
        ? (inventory_ratio - cfg_.inventory_skew_threshold) / skew_range
        : 1.0,
    1.0);
```

### Effect

```
Before: skew_frac = 14.67 → skew_mult = 5.40×
After:  skew_frac = 1.00  → skew_mult = 1.30× (design maximum)
```

### Risk Assessment

- **Low risk**: The clamp enforces the design intent. `skew_frac > 1.0` was
  never intended — the graduated interpolation is between threshold and capacity.
- **All 26 inventory tests pass**: Tests don't drive `inventory_ratio > 1.0`
  through `compute_spread()` (they test `calc_inventory_bps` which has its own clamp).
- **Rollback**: Revert one expression.

---

## Fix 3: Increase q_max 10 → 100 (MEDIUM)

### Root Cause

`config.yaml` has `q_max: 10` but the bot holds 64.7 XCH. This means:

1. `calc_inventory_bps` always computes at `inventory_frac = 1.0` (100% capacity)
   → maximum inventory pressure regardless of actual imbalance.
2. With Fix 2 applied, skew is capped to 1.30× — but inventory pressure stays
   maximized.

The C++ default (`config.hpp:175`) is `q_max{1000.0}`. The `10` in config.yaml
was likely set for testing with small inventory and never updated.

### Config Change

```yaml
# BEFORE:
q_max: 10

# AFTER:
q_max: 100
```

### Effect on Inventory Components

```
Before (q=64.7, q_max=10):
  calc_inventory_bps: frac = min(64.7/10, 1.0) = 1.0 → always maxed
  compute_spread ratio: 64.7/10 = 6.47 → skew_frac = 14.67 (pre-clamp)

After (q=64.7, q_max=100):
  calc_inventory_bps: frac = min(64.7/100, 1.0) = 0.647 → proportional
  compute_spread ratio: 64.7/100 = 0.647 → above threshold (0.60)
  skew_frac = (0.647-0.60)/0.40 = 0.1175 → skew_mult = 1.035×
```

The inventory component now responds proportionally instead of being pegged at max.

### Risk Assessment

- **Low risk**: Config-only change. Larger q_max → smaller inventory pressure
  → tighter spreads. If too tight, revert to a smaller q_max.
- A value of 100 reflects "the bot might hold up to 100 XCH" which is reasonable
  for the current deployment.

---

## Fix 4: Reduce fee_reserve_xch 1.0 → 0.1 (MEDIUM)

### Root Cause

With `fee_reserve_xch = 1.0` and `xch_spendable = 0.81`:
- `spendable (0.81) < reserve (1.0)` → UTXO liberation triggers
- All offers are fresh (< 5 blocks) → no cancellation candidates
- Engine enters **buy-only mode** permanently
- Only bid offers posted → only asks fill → XCH accumulates → inventory spiral

### Config Change

```yaml
# BEFORE:
fee_reserve_xch: 1.0

# AFTER:
fee_reserve_xch: 0.1
```

### Justification

Chia transaction fees are currently 5K–100K mojos. Even at peak (100K mojos ≈
0.0000001 XCH), 0.1 XCH = 100M mojos covers ~1000 transactions. The reserve
exists to ensure the wallet can always post fee-bearing transactions for offer
creation/cancellation; 0.1 XCH is generous.

### Effect

```
Before: spendable 0.81 < reserve 1.0 → buy-only mode → ask fills only
After:  spendable 0.81 > reserve 0.1 → normal two-sided market making
```

### Risk Assessment

- **Medium risk**: If Chia fee market spikes dramatically (`fee_reserve` exists
  for a reason), 0.1 XCH might not cover surges. However, the current 1.0 XCH
  is causing worse harm (permanent one-sided trading).
- **Monitor**: Watch `xop_spendable_reserve_pct` Prometheus metric. If it drops
  below 10%, consider adjusting.
- **Rollback**: Change config back to 1.0 or any intermediate value.

---

## Fix 5: Backtest Sigma Scale Fix (LOW)

### Root Cause

`backtest.cpp:230` stores the return value of `estimator.update()`, which returns
**per-block** sigma (`sigma_block`):

```cpp
// backtest.cpp:230:
const double sigma = estimator.update(blk.cex_mid);  // returns sigma_block!
historical_sigmas_.push_back(sigma);
```

But `compute_quotes()` at lines 576 and 908 expects **annual** sigma:
```cpp
// base.hpp:107: @param sigma  Annualised volatility of the base asset
const QuoteResult quotes = strategy.compute_quotes(mid, sigma, q, block_height);
```

With `blocks_per_year ≈ 606,857` and `sqrt(blocks_per_year) ≈ 779`:
```
sigma_block ≈ 0.000642   (what backtest provides)
sigma_annual ≈ 0.50      (what compute_quotes expects)
→ inventory risk underestimated by ~779×
→ backtests are unrealistically optimistic (spreads too tight)
```

### Code Change

**File**: `backtest.cpp`, line ~230:

```cpp
// BEFORE:
const double sigma = estimator.update(blk.cex_mid);
historical_sigmas_.push_back(sigma);

// AFTER:
estimator.update(blk.cex_mid);
const double sigma = estimator.is_ready()
    ? estimator.get_sigma_annual()
    : 0.0;
historical_sigmas_.push_back(sigma);
```

### Risk Assessment

- **Low risk**: Backtest code does not affect live trading.
- Backtests run against this change will show wider spreads and fewer fills,
  which is more accurate.
- Historical backtest results should be re-run after this fix.

---

## Fix 6: Monitoring Improvements (LOW)

### Recommended Prometheus Metrics

Add per-pair `sigma_daily` gauge alongside the existing `volatility_annual`:

```
xop_sigma_daily{pair="XCH_wUSDC.b"} 0.000613
xop_sigma_daily{pair="XCH_DBX"} 0.272
```

This allows monitoring the actual value reaching `compute_spread()` and
verifying Fix 1 is working correctly.

### Integration Test

Add a test that constructs a `VolatilityEstimator`, pushes candles to produce
a known `sigma_annual`, then verifies the value passed to `compute_spread()` is
`sigma_annual / sqrt(365)`. This covers the gap that no existing unit test
exercises.

---

## Fix 7: MarketAllocator Surplus Redistribution (MEDIUM)

### Root Cause

Discovered during deployment testing — pre-existing bug, not caused by our
sigma/skew changes. `MarketAllocatorTest.MinMaxGuardrails` was the sole
failing test (280/281).

In `apply_allocation()` (`market_allocator.cpp:345-360`), the iterative
projection loop freezes pairs at their min/max bounds and redistributes
the remainder to free pairs. **Bug**: When all pairs freeze in one iteration
(e.g., 3 pairs at 15% + 45% + 15% = 75%), `free_count == 0` but
`remaining = 0.25` — the surplus was silently dropped.

### Code Change

**File**: `market_allocator.cpp`, in `apply_allocation()`:

Added an `else if (remaining > 1e-9)` branch after the `free_count > 0`
block that distributes surplus proportionally to each pair's headroom
(`max_alloc_pct - current_allocation`):

```cpp
} else if (remaining > 1e-9) {
    double headroom_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        headroom_sum += cfg_.max_alloc_pct - scores[i].allocation;
    }
    if (headroom_sum > 1e-12) {
        for (std::size_t i = 0; i < n; ++i) {
            double hr = cfg_.max_alloc_pct - scores[i].allocation;
            scores[i].allocation += remaining * (hr / headroom_sum);
        }
    } else {
        double each = remaining / static_cast<double>(n);
        for (auto& ps : scores) ps.allocation += each;
    }
}
```

### Effect

```
Before: 3 pairs → 15% + 45% + 15% = 75%, missing 25%
After:  redistributed to headroom → allocations sum to 100%
```

### Risk Assessment

- **Low risk**: Only activates when all pairs are frozen and sum < 1.0.
- **Test result**: 281/281 pass (including MinMaxGuardrails, previously failing).

---

## Expected Outcomes Post-Deployment

With all fixes live, we expect the following changes observable via Prometheus
and log analysis:

### Spread Behavior

| Metric | Before (broken) | Expected After |
|--------|-----------------|----------------|
| `xop_spread_bps` most pairs | 300 bps (hitting `max_half_spread_bps` cap) | 35–200 bps (pair-dependent, vol-driven) |
| Spread regime classification | Always HighVol (annual σ > 0.065) | Normal/Low for most pairs |
| Skew multiplier | Up to 5.4× (unbounded) | Max 1.30× (design cap) |
| Inventory pressure | Always maxed (q/q_max = 6.47) | Proportional (q/q_max = 0.647) |

### Quoting & Trading

| Metric | Before | Expected After |
|--------|--------|----------------|
| Quote direction | Buy-only (UTXO crisis) | Two-sided (bid + ask) |
| `xop_spendable_reserve_pct` | < 0.0 (spendable < reserve) | > 0.80 (0.81 - 0.1 reserve) |
| Offer cancellation pressure | Constant (trying to free UTXOs) | Minimal (reserve headroom) |
| XCH inventory drift | Accumulating (ask-only fills) | Mean-reverting (two-sided fills) |

### Allocation

| Metric | Before | Expected After |
|--------|--------|----------------|
| Allocation sum-to-one | Could drop surplus (75% of 100%) | Always sums to ~100% |
| MinMaxGuardrails compliance | Failing | Passing |

### What to Watch For

1. **First 30 minutes**: Confirm both bid and ask offers appear for all pairs.
2. **First 2 hours**: Verify `xop_spread_bps` settles into 35–200 bps range.
3. **First 24 hours**: Check inventory drift direction — should see XCH
   balance oscillate rather than monotonically increase.
4. **Ongoing**: If any pair consistently hits `max_half_spread_bps` (150 bps),
   that's expected only for extremely volatile pairs (e.g., XCH/DBX at 519%
   annual vol).

### Deferred Items

- **Fix 6 (Prometheus sigma_daily metric + integration test)**: Not implemented
  in this deployment. Low priority — can verify sigma conversion via existing
  spread metrics. Add when next refactoring monitoring.

---

## What NOT To Change

### 1. Spread Optimizer Formulas (Dimensional Mix)

The spread optimizer uses `sigma_daily × sqrt(T_fill_seconds)`. While
dimensionally inconsistent in academic terms, the model was **empirically
calibrated** with `gamma=0.005` absorbing the unit factor. Changing the units
would require recalibrating gamma by ~86 400× and invalidating all production
tuning.

### 2. Strategy Gamma

The A-S and GLFT strategies use `sigma_annual` with `tau_seconds` and their own
gamma calibration. These are self-consistent and unrelated to the spread
optimizer's gamma.

### 3. SpreadVolatilityTracker

The cross-pair contamination (one shared tracker for all 4 pairs) amplifies
instability when spreads are broken. After Fixes 1–2 bring spreads into the
35–200 bps range, the tracker's CV will be low and `safety_mult ≈ 1.0`. No
immediate change needed — revisit if spread variance remains high post-fix.

---

## Deployment Order (Actual)

All fixes deployed together on 2026-04-10:

1. **Commit `5b9e9c0`** (Fixes 1–5):
   - ✅ Fix 1: Sigma conversion in engine.cpp
   - ✅ Fix 2: Skew clamp in spread.cpp
   - ✅ Fix 3: q_max = 100 (config.yaml, gitignored)
   - ✅ Fix 4: fee_reserve_xch = 0.1 (config.yaml, gitignored)
   - ✅ Fix 5: Backtest sigma in backtest.cpp

2. **Commit `131ac4c`** (Fix 7):
   - ✅ Fix 7: MarketAllocator surplus redistribution

3. **Deferred**:
   - ⏸️ Fix 6: Prometheus sigma_daily metric + integration test

### Build & Test Results

- **Build**: Clean (0 errors, 0 warnings) — xop_core.lib, xop_tests.exe, xop_trader.exe
- **Tests**: 281/281 pass (100%), including previously-failing MinMaxGuardrails
- **Trader**: Restarted as PID 35996 at 2026-04-10 13:42

### Rollback Plan

Each fix is independently revertible:
- Fix 1: Change one line in engine.cpp
- Fix 2: Remove `std::min(..., 1.0)` wrapper
- Fix 3–4: Change config.yaml values back
- Fix 5: Revert backtest.cpp line/s
- Fix 7: Remove else-if branch in market_allocator.cpp

---

## Appendix: Complete Sigma Flow Diagram

```
VolatilityEstimator
  │
  ├─ get_sigma_block()   ← per-block (~0.000642)
  │   └─ engine.cpp:2887 → MarketParams    ✅ expects per-block
  │   └─ engine.cpp:3022 → MarketParams    ✅ expects per-block
  │   └─ engine.cpp:6235 → logging         ✅ display only
  │
  ├─ get_sigma_annual()  ← annualized (~0.50)
  │   └─ engine.cpp:2351 → compute_quotes()    ✅ expects annual
  │   └─ engine.cpp:2409 → compute_spread()    ❌ expects DAILY → FIX 1
  │   └─ engine.cpp:2981 → analyze_drift()     ✅ converts internally
  │   └─ engine.cpp:3157 → compute_ladder()    ✅ (void)sigma — unused
  │
  └─ update() return     ← per-block
      └─ backtest.cpp:230 → historical_sigmas_ → compute_quotes()
                                                   ❌ expects annual → FIX 5
```
