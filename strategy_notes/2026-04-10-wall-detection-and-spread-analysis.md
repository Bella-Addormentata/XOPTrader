# Deep Analysis: Wall Detection, 10M bps Spreads, and Ask-Only Posting

**Date:** April 10, 2026  
**Revisit By:** April 12, 2026  
**Status:** ✅ Resolved — all root causes fixed in v0.7.34 (see [comprehensive fix plan](2026-04-11-comprehensive-fix-plan.md))  

---

## Executive Summary

The engine consistently computes intermediate spreads of **10–14 million bps** before the global cap reduces them to 300 bps. Wall detection at Step 5 logs these values, making it appear to be the cause, but wall detection is a **symptom, not the root cause**. The engine's `max_half_spread_bps: 150` cap is the only thing keeping quotes sane.

Meanwhile, XCH spendable reserve has dropped to **3.6%** (below 10% threshold), triggering "XCH-buy-only mode" and UTXO liberation loops. The combination of always-capped spreads, UTXO scarcity, and near-zero CAT balances produces the ask-only pattern.

Three independent root causes interact:

1. **Sigma mismatch**: `get_sigma_annual()` passed to a function expecting `sigma_daily` — annual vol is 19× larger than daily, and the inventory term uses σ² making the error **361×**
2. **Unclamped inventory skew**: `q_max=10` with 64.7 XCH inventory creates a 5.4× unclamped multiplier
3. **UTXO liquidity crisis**: 10+ concurrent offers lock all spendable coins, leaving <1 XCH available

---

## Situation Before Analysis

| Metric | Value | Problem |
|---|---|---|
| XCH/wUSDC.b spread (pre-cap) | ~12M bps | 40,000× above healthy range |
| XCH/DBX spread (pre-cap) | ~10M bps | Same |
| XCH/BYC spread (pre-cap) | ~5.7M bps | Same |
| Post-cap spread (all pairs) | 300 bps | Cap is working but masks model failure |
| XCH spendable reserve | 3.6% | Below 10% threshold |
| Spendable XCH | 0.81 XCH | Below 1.0 fee reserve |
| Wall-skips per cycle | 1–2 (XCH/wUSDC.b only) | Minor — not the root cause |
| Pending offers | 5–11 | Locking nearly all UTXOs |
| UTXO liberation mode | Active, cancelling 3/cycle | Stuck in liberation loop |
| CAT balances | wUSDC.b: 162k, BYC: 65k, DBX: 8k mojos | Near zero |

### Prometheus Volatility Snapshot (Runtime)

| Pair | `volatility_annual` (startup) | Runtime Sigma (estimated) |
|---|---|---|
| XCH/wUSDC.b | 0.0117 | Likely higher after more data |
| XCH/BYC | 0.0 (not ready) | Now ready after 100+ blocks |
| BYC/wUSDC.b | 0.0 (not ready) | Now ready after 100+ blocks |
| XCH/DBX | 5.193 (519% annual) | High — microcap token |

---

## Root Cause #1: Sigma Annual vs Daily Mismatch

### The Bug

**File:** `engine.cpp` line 2229  
**Code:** `sigma = vol_it->second->get_sigma_annual();`  
**Problem:** `compute_spread()` parameter is named `sigma_daily` and the formula assumes daily-scale volatility.

The volatility estimator's `get_sigma_annual()` returns annualized standard deviation:
```
sigma_annual = sigma_block × sqrt(blocks_per_year)
            = sigma_block × 778.89
```

The spread model expects daily:
```
sigma_daily = sigma_annual / sqrt(365) = sigma_annual / 19.105
```

Passing annual as daily creates:
- **s_adverse** ∝ σ → 19× too large
- **s_inventory** ∝ σ² → **361× too large**

### Math: XCH/DBX with sigma_annual = 5.193

| Component | With σ_annual=5.193 (BUG) | With σ_daily=0.272 (FIXED) |
|---|---|---|
| s_adverse | gamma × 5.193 × √7200 × 0.15 × 10000 = **33,051 bps** | gamma × 0.272 × √7200 × 0.15 × 10000 = **1,730 bps** |
| s_inventory | gamma × 26.97 × 3600 × 1.0 × 10000 = **4,854,600 bps** | gamma × 0.074 × 3600 × 1.0 × 10000 = **13,305 bps** |
| Base total | **~4.9M bps** | **~15,000 bps** |

After inventory_skew (5.4×): 26M bps → still capped at 300. With q_max fix (see Root Cause #2), 15,000 × 1.0 = 15,000 bps → also capped.

**Conclusion:** Fixing sigma alone reduces 10M → 15K. Still above the 300 bps cap for volatile pairs. Must also fix q_max.

### Math: XCH/wUSDC.b with sigma_annual = 0.0117

| Component | With σ_annual (BUG) | With σ_daily=0.000613 (FIXED) |
|---|---|---|
| s_adverse | 0.005 × 0.0117 × 84.85 × 0.15 × 10000 = **7.4 bps** | 0.005 × 0.000613 × 84.85 × 0.15 × 10000 = **0.4 bps** |
| s_inventory | 0.005 × 0.000137 × 3600 × 1.0 × 10000 = **24.7 bps** | 0.005 × 0.000000376 × 3600 × 1.0 × 10000 = **0.07 bps** |
| Base total | **~37 bps** | **~5.5 bps** (dominated by s_cost) |

For XCH/wUSDC.b with correct daily sigma, the model spread would be 5.5 bps + floor(35 bps) = **35 bps** — a tight, competitive quote. The current 12M bps log suggests the **runtime** sigma has grown significantly beyond the startup value of 0.0117, likely after more candle data accumulated.

### Proposed Fix

In `engine.cpp` line 2229:
```cpp
// BEFORE (bug):
sigma = vol_it->second->get_sigma_annual();

// AFTER (fix):
sigma = vol_it->second->get_sigma_annual() / std::sqrt(365.0);
```

Or better: add `get_sigma_daily()` to `VolatilityEstimator`:
```cpp
double get_sigma_daily() const noexcept {
    std::shared_lock lock(mtx_);
    return sigma_annual_ / std::sqrt(365.0);
}
```

---

## Root Cause #2: Unclamped Inventory Skew Multiplier

### The Bug

**File:** `spread.cpp` lines 637–650  
**Code:** `skew_frac = (inventory_ratio - threshold) / skew_range`  
**Problem:** `skew_frac` is not clamped to [0, 1], so `inventory_ratio >> 1.0` produces unbounded multipliers.

| Current State | |
|---|---|
| `q_max` config | 10 |
| Actual XCH inventory | 64.7 XCH |
| `inventory_ratio` | `64.7 / 10 = 6.47` |
| `skew_frac` | `(6.47 - 0.60) / 0.40 = 14.67` |
| `skew_mult` | `1.0 + 14.67 × 0.30 = 5.40×` |

Note: `calc_inventory_bps()` correctly clamps `|q|/q_max` to 1.0 via `std::min()`, but the post-model `inventory_skew_multiplier` in `compute_spread()` does NOT clamp.

### Two-Part Fix

**A) Clamp skew_frac** in `spread.cpp` line 645:
```cpp
// BEFORE:
const double skew_frac = (inventory_ratio - cfg_.inventory_skew_threshold) / skew_range;

// AFTER:
const double skew_frac = std::min(
    (inventory_ratio - cfg_.inventory_skew_threshold) / skew_range,
    1.0);
```

**B) Increase q_max** in `config.yaml`:
```yaml
# BEFORE:
q_max: 10   # 10 XCH max — way below actual 64.7 XCH inventory

# AFTER:
q_max: 100  # Reflects realistic inventory range
```

With both fixes: `inventory_ratio = 64.7/100 = 0.647`. `skew_frac = (0.647-0.60)/0.40 = 0.118`. `skew_mult = 1.0 + 0.118 × 0.30 = 1.035×`. Minimal impact as intended.

---

## Root Cause #3: UTXO Liquidity Crisis

### Mechanism

The engine has **64.7 XCH** total but only **0.81 XCH spendable** (3.6%). The rest is locked in pending offers:

1. Each offer creation locks the coin used to fund it
2. With 10+ concurrent offers, nearly all coins are locked
3. Spendable drops below `fee_reserve_xch: 1.0` → UTXO liberation triggered
4. Liberation cancels 3 oldest offers per cycle, but cancellation doesn't immediately free coins (needs mempool confirmation)
5. Engine enters "XCH-buy-only mode" → can only post bids (which need CATs we don't have)
6. Cycle repeats: offers expire/cancel → briefly spendable → immediately re-locked by new offers

### Why This Causes Ask-Only Posting

- XCH-buy-only mode suppresses asks
- But we have no CAT inventory for bids
- Both sides suppressed → no new offers
- Only pre-existing ask offers (from before the crisis) remain pending
- These are the "ask only" pattern observed in trade history

### Observations from Logs

```
[Engine] UTXO liberation: spendable 0.810 XCH < reserve 1.000 XCH -- cancelling oldest
[Engine] UTXO liberation: cancelled 3 offer(s) but spendable still 0.810 XCH -- entering XCH-buy-only mode
[Engine] UTXO liberation cooldown: 4 heartbeats remaining -- XCH-buy-only mode
[Engine] Step 8: xch spendable reserve 3.6% < 10.0% threshold -- suppressing ask side
```

### Proposed Mitigations

**A) Reduce concurrent offers to free coins:**
```yaml
# BEFORE:
num_tiers: 6
coin_pool_target_count: 12

# AFTER:
num_tiers: 4          # 4 tiers × 4 pairs × 2 sides = 32 max; more realistic
coin_pool_target_count: 16  # More coins available for concurrent offers
coin_pool_target_xch: 1.5   # Smaller coins = more granular locking
```

**B) Lower fee_reserve_xch for existing balance:**
```yaml
# BEFORE:
fee_reserve_xch: 1.0    # Reserves 1 XCH — too much when spendable < 1

# AFTER:
fee_reserve_xch: 0.1    # 100M mojos still covers many txns at 5K-100K fee
```

**C) Consider per-pair offer limits** to prevent any single pair from consuming all coins.

---

## Wall Detection: Not the Root Cause

### What Wall Detection Actually Does

**Step 5 (line 2260):** When a competing offer > `wall_size_threshold_xch` (50 XCH) exists, multiply the spread by `1 + wall_niche_premium_pct` (1.05×). This is a **5% niche premium** — the smallest multiplier in the entire chain.

**Step 7 (line 3300):** For each tier, if the competing offer at that tier's rank exceeds the wall threshold, skip the competitive tightening for that tier. The tier keeps its already-computed price — it is NOT suppressed or removed.

### Impact Assessment

- The 1.05× wall premium turns 11.4M bps into 12M bps — trivially irrelevant since both are capped to 300 bps
- Wall-skips affect 1-2 tiers per cycle on XCH/wUSDC.b only — these tiers are still posted, just not tightened
- Wall detection is purely a **competitive adjustment**, not a spread generator

### Recommendation

No changes needed to wall detection logic itself. The current parameters (50 XCH threshold, 5% premium) are reasonable for Chia DEX market structure where offers are atomic-fill only.

---

## Cascade Analysis: What Fixing Sigma + Q_max Achieves

### Before Fix (Current State)

```
compute_spread() → 10M bps (broken model)
  → cap to 300 bps (safety net saves us)
    → offers posted at 300 bps spread
      → not competitive (market spread is 50-165 bps)
        → no fills → inventory doesn't rebalance
          → UTXO crisis persists
```

### After Fix (Projected)

```
compute_spread() → 35-200 bps (healthy model)
  → no cap needed (model is reasonable)
    → offers posted at actual model spread
      → competitive with market (50-165 bps range)
        → fills on BOTH sides → inventory rebalances
          → UTXO crisis resolves (fewer stuck offers)
```

### What It Fixes

| Issue | Fixed? | Explanation |
|---|---|---|
| 10M bps intermediate spreads | ✅ Yes | Model produces 35-200 bps |
| Spread always hitting cap | ✅ Yes | Model output < cap |
| Wall detection logging scary numbers | ✅ Yes | 5% of 150 bps = 157.5 bps (normal) |
| Bid-side competitiveness | ✅ Partially | Reasonable spreads make bid prices competitive |
| UTXO locking | ❌ No | Structural issue with concurrent offers |
| Near-zero CAT balances | ❌ No | Needs fills or manual funding |
| XCH spendable reserve crisis | 🟡 Indirect | Better fills → faster coin recycling |

### What It Won't Fix

1. **CAT inventory drought** — No wUSDC.b/BYC/DBX to fund bids even with competitive spreads
2. **UTXO starvation** — Even with reasonable spreads, 6-tier × 4-pair × 2-side = 48 potential offers vastly exceeds available UTXOs (12 target)
3. **Fee reserve threshold** — At 0.81 XCH spendable vs 1.0 reserve, the engine stays in buy-only mode regardless of spread quality

---

## Additional Improvement Ideas

### 1. Add get_sigma_daily() to VolatilityEstimator [HIGH PRIORITY]

Add a proper accessor so callers don't need ad-hoc conversions:
```cpp
double get_sigma_daily() const noexcept {
    std::shared_lock lock(mtx_);
    static constexpr double kSqrt365 = 19.10497317; // sqrt(365.0)
    return sigma_annual_ / kSqrt365;
}
```

Update all call sites (engine.cpp lines 2229, 2351, 2409, 2981, 3157) to use `get_sigma_daily()`.

### 2. Expose Runtime Sigma as Prometheus Metric [MEDIUM PRIORITY]

Currently `volatility_annual` is only populated from startup analysis. Add a runtime gauge:
```
xop_volatility_sigma_annual{pair_name="XCH/wUSDC.b"} 0.0117
xop_volatility_sigma_daily{pair_name="XCH/wUSDC.b"} 0.000613
```

This would have caught the sigma mismatch immediately during monitoring.

### 3. Per-Pair q_max [MEDIUM PRIORITY]

Different pairs have different inventory scales:
- XCH: ~64.7 XCH → q_max should be ~100
- BYC: ~0.065 BYC → q_max should be ~10
- wUSDC.b: ~0.162 USDC → q_max should be ~500

A global q_max of 10 is wrong for all of them. Add `q_max_override` to PairConfig.

### 4. Dynamic Tier Count Based on UTXO Budget [LOW PRIORITY]

The engine already has `Dynamic tier limit: 6 -> 1 tiers` logic. Consider making this more aggressive: if total spendable coins < 2× number of planned offers, reduce tiers proactively rather than waiting for coin exhaustion.

### 5. Spread Component Logging [LOW PRIORITY]

The existing debug log at engine.cpp line 2430 contains all components but is at DEBUG level. Consider promoting to INFO for the first N cycles after startup, or when spread exceeds a threshold (e.g., > 1000 bps pre-cap).

### 6. Regime Threshold Calibration [LOW PRIORITY]

The Low/Normal/High vol regime thresholds (0.035/0.065 daily sigma) are correct for *daily* sigma. After the sigma fix, verify that XCH/wUSDC.b (sigma_daily ≈ 0.0006) is correctly classified as Low regime and XCH/DBX (sigma_daily ≈ 0.27) as High regime.

---

## Recommended Change Sequence

### Phase 1: Immediate (Code Changes)

1. **Fix sigma mismatch** — Add `get_sigma_daily()` and update engine.cpp call sites
2. **Clamp inventory skew_frac** — `std::min(skew_frac, 1.0)` in spread.cpp
3. **Build and test** — Run unit tests, verify spread values in test outputs

### Phase 2: Config Tuning (After Build)

4. **Increase q_max** — `q_max: 100` (or add per-pair overrides)
5. **Reduce fee_reserve_xch** — `1.0 → 0.1`
6. **Reduce num_tiers** — `6 → 4` for fewer concurrent offers

### Phase 3: Monitoring (After Deploy)

7. **Add runtime sigma Prometheus metric** — Verify sigma values are reasonable
8. **Monitor spread component logs** — Check that model spreads are 35–200 bps
9. **Monitor fill rate** — Should see bid fills within 24 hours if spreads are competitive

---

## Pros of Proposed Changes

1. **Model output becomes meaningful** — Spread reflects actual market risk rather than a constant cap
2. **Dynamic spread adjustment works** — Regime, inventory, time-of-day multipliers have real effect instead of being overwhelmed
3. **Competitive on both sides** — Bid prices become competitive with 35-200 bps spreads
4. **Reduces log noise** — No more 10M bps warnings
5. **Unblocks all downstream logic** — Thompson sampling, order book tactics, competitive cap all work correctly when the input spread is reasonable

## Cons / Risks

1. **Sigma fix may make spreads TOO tight** — With sigma_daily of 0.0006 for XCH/wUSDC.b, the model produces ~5 bps. The s_floor (35 bps) protects against unprofitable quotes, but tight spreads increase adverse selection risk.
2. **q_max change affects all pairs** — Need to verify XCH/DBX doesn't under-inventory because q_max is now 100 but we only have 8k mojos of DBX.
3. **Fee reserve reduction** — At 0.1 XCH reserve, we might fail to pay fees during high-activity periods.
4. **Tier reduction means less depth** — 4 tiers instead of 6 means less order book presence.

---

## Pitfalls to Watch For

### 🔴 Critical

- **Adverse selection after tightening** — With 35 bps spreads, informed traders can pick off stale quotes faster than the engine updates. Monitor whether fills consistently move against us (we buy before price drops, sell before price rises).
- **Coin pool rebalance storm** — Changing q_max and fee_reserve simultaneously may trigger a wave of cancels + re-posts that exhausts the fee budget. Deploy changes one at a time.
- **Vol estimator instability** — The Yang-Zhang estimator on degenerate candles (>90% O=H=L=C on Chia) may produce noisy sigma estimates. After the sigma fix, verify that the daily sigma stays in a reasonable range (0.001–0.50) and doesn't oscillate wildly.

### 🟡 Important

- **The SpreadVolatilityTracker memory** — The shared tracker has a window of previous spreads. After the sigma fix, the first ~200 cycles will mix old 10M bps values with new 35-200 bps values, causing high CV and up to 1.5× safety multiplier. Consider resetting the tracker on deploy (or tolerate the transient widening).
- **Stablecoin pair interaction** — BYC/wUSDC.b has `max_half_spread_bps_override: 75` and is stablecoin-exempt from buy-only mode. After fixes, verify its spreads don't tighten below the 5 bps profit margin.
- **Thompson sampler convergence** — Grid is [30, 40, 50, 60, 80, 100] bps. After the fix, model spreads may be mostly in the 30-60 range, concentrating thompson exploration on the lower arms. This is correct behavior but the posteriors will shift.

### 🟢 Informational

- **Wall detection becomes cosmetic** — A 5% premium on 150 bps = 157.5 bps. This is a reasonable niche premium. The log messages will report normal values instead of millions.
- **The global cap becomes a safety net** — Currently the cap IS the spread. After the fix, it should rarely activate (only during genuine extreme volatility).
- **All five sigma call sites must be updated** — engine.cpp has `get_sigma_annual()` at 5 different locations (lines 2229, 2351, 2409, 2981, 3157). Missing any one creates inconsistent behavior.

---

## Rollback Plan

### Code Changes

```cpp
// Revert get_sigma_daily() calls back to get_sigma_annual()
// Revert the skew_frac clamp
```

### Config Changes

```yaml
# Revert to previous values:
q_max: 10
fee_reserve_xch: 1.0
num_tiers: 6
```

---

## Notes

- The 10M bps intermediate spread was caught by the global cap (`max_half_spread_bps: 150`), so actual offer prices have been reasonable despite the broken model. The cap was effectively the ONLY thing determining spread, making all other spread logic (regime multipliers, Thompson sampling, competitive cap, wall premium) dead code.
- The sigma mismatch has existed since the spread optimizer was first implemented. It was masked by the global cap.
- Priority order for maximum impact: (1) sigma fix, (2) skew clamp, (3) q_max increase, (4) fee_reserve reduction.
- Manual CAT seeding (buying some wUSDC.b and BYC via external swap) would immediately enable two-sided markets independent of code fixes.
