# Fix: Half-Spread Cap, Gate 2 XCH Deadlock, and Dust Threshold

**Date:** April 10, 2026  
**Status:** ✅ TESTED — 281/281 tests pass. Awaiting commit, trader restart.  
**Prerequisite:** [2026-04-11 Comprehensive Fix Plan](2026-04-11-comprehensive-fix-plan.md) (Fixes 1–9 already deployed as v0.7.34)  

---

## Executive Summary

Three interacting bugs prevented the engine from posting XCH bid offers, creating a one-sided (ask-only) quoting pattern. All three were diagnosed and fixed in this session.

| # | Fix | Severity | Files Modified | Status |
|---|-----|----------|---------------|--------|
| 10 | A-S/GLFT half-spread cap at 49% of mid | **CRITICAL** | avellaneda.cpp, glft.cpp, glft.hpp | ✅ Tested |
| 11 | Gate 2 XCH exemption (wallet_id 1) | **HIGH** | engine.cpp | ✅ Tested |
| 12 | min_offer_size_units 1.0 → 0.1 | **MEDIUM** | config.hpp, config.yaml | ✅ Tested |

---

## Situation Before Fixes

| Metric | Value | Problem |
|---|---|---|
| Portfolio allocation | 86% stablecoins ($311) / 14% XCH ($51) | Severely imbalanced |
| XCH locked in pending offers | 91% of total XCH | Only ~0.5 XCH "free" |
| XCH bid offers posted | **0** | Can't buy XCH to rebalance |
| A-S half-spread (absolute) | 3.806 XCH | Exceeds mid price (~2.30) |
| Bid price (mid - half-spread) | Negative → clamped to 0 | Impossible to place bids |
| Gate 2 spendable/confirmed | ~9% | Below 10% threshold → ask side also suppressed |
| Dust tier sizes (2 XCH / 6 tiers) | ~0.33 XCH each | All below 1.0 XCH min → all dropped |

### The Feedback Loop

```
A-S half-spread > mid price
  → bid price = 0 (clamped)
  → reservation_mid pushed far above market
  → no bid offers posted
  → can't buy XCH
  → inventory stays stablecoin-heavy
  → XCH locked in pending offers
  → Gate 2 blocks ask side (spendable < 10%)
  → can't sell either
  → total quoting paralysis
```

---

## Fix 10: A-S/GLFT Half-Spread Cap (CRITICAL)

### Root Cause

The Avellaneda-Stoikov half-spread formula:

$$\delta = \frac{1}{\kappa} \ln\left(1 + \frac{\kappa}{\gamma}\right)$$

With γ=0.005 and κ=1.5 produces an **absolute** half-spread of **3.806 units**. For XCH priced at ~$2.30, this means bid = mid - 3.806 = **negative**, which is clamped to 0.

The existing `max_half_spread_bps` cap (150 bps = 0.0345 XCH) was applied in the spread optimizer (Step 5), but the A-S strategy's reservation-mid calculation in Step 4 uses the raw uncapped half-spread, pushing the midpoint far above the actual market mid.

### Fix

Added a cap at 49% of mid price **before** the regime multiplier is applied:

**Files:** `cpp/src/strategy/avellaneda.cpp`, `cpp/src/strategy/glft.cpp`

```cpp
// Cap half-spread to max_half_spread_pct of mid price (default 49%)
// Applied BEFORE regime multiplier so regime-dependent scaling is preserved.
if (mid > 0.0) {
    const double max_delta = mid * cfg_.max_half_spread_pct;
    if (delta > max_delta)
        delta = max_delta;
}
```

**File:** `cpp/include/xop/strategy/glft.hpp`

Added `double max_half_spread_pct{0.49};` to `GlftConfig` struct.

### Why 49%?

- At 49%, bid = mid × (1 - 0.49) = 0.51 × mid → always positive
- Ask = mid × (1 + 0.49) = 1.49 × mid → still on the right side
- 50% would make bid = 0 at the cap, so 49% gives a minimum margin

### Why Before Regime Multiplier?

Initial implementation placed the cap after the regime multiplier. This caused `RegimeDetectionTest.MeanRevertingNarrowsSpread` to fail because both mean-reverting (×0.8) and random-walk (×1.0) regimes hit the same cap, producing identical spreads. Moving the cap before the multiplier preserves the regime differentiation: mean-reverting narrows the (capped) base, momentum widens it.

### Expected Impact

| Pair | Before | After |
|---|---|---|
| XCH/wUSDC.b (mid ~2.30) | δ=3.806 → bid=0 | δ=1.127 → bid=1.17, ask=3.43 |
| XCH/BYC (mid ~2.30) | Same problem | Same fix |
| XCH/DBX (mid ~2.30) | Same problem | Same fix |

---

## Fix 11: Gate 2 XCH Exemption (HIGH)

### Root Cause

Engine Step 8 Gate 2 ("Fractional Reserve") checks that `spendable / confirmed >= min_spendable_reserve_pct (10%)` before posting an offer for any asset. With 91% of XCH locked in the engine's own pending offers:

- **confirmed** = total XCH balance (~5.5 XCH)
- **spendable** = confirmed minus locked-in-offers (~0.5 XCH)
- **ratio** = 0.5/5.5 = 9% < 10% threshold

This suppresses the **ask** side for all XCH pairs. Combined with Fix 10's bid suppression, both sides are blocked.

### Why This Deadlock Exists

The XCH wallet's "spendable" balance reflects UTXO availability. When the engine creates offers, those UTXOs are locked until the offer resolves or is cancelled. The Gate 2 check was designed to prevent over-commitment, but for XCH (wallet_id 1) the `OfferManager`'s UTXO-lock pre-check already guards against spending unavailable coins. Gate 2 creates a deadlock: offers lock UTXOs → ratio drops → can't post new offers to replace stale ones → old offers expire → briefly unlocks → posts new offers → locked again.

### Fix

**File:** `cpp/src/engine.cpp` (~line 4224)

```cpp
// Skip Gate 2 for XCH (wallet_id 1): the offer_manager's UTXO-lock
// pre-check already guards XCH spendable balance. Applying the
// fractional reserve gate to XCH causes a deadlock when most XCH
// is locked in the engine's own pending offers.
if (confirmed > 0 && sb.wid != 1) {
```

### Risk Assessment

Low risk. The UTXO-lock pre-check in `OfferManager` prevents the engine from creating offers that require more XCH than is actually spendable at the coin level. Gate 2 was a belt-and-suspenders check that became a deadlock mechanism.

---

## Fix 12: Dust Filter Threshold (MEDIUM)

### Root Cause

With ~2 XCH free and 6 pricing tiers, each tier gets ~0.33 XCH. The `min_offer_size_units` default of 1.0 means all 6 tiers are dropped as dust, resulting in zero offers even when the half-spread and Gate 2 issues are resolved.

### Fix

**File:** `cpp/include/xop/config.hpp`

Changed default: `min_offer_size_units{1.0}` → `min_offer_size_units{0.1}`

**File:** `config.yaml`

Added explicit: `min_offer_size_units: 0.1`

### Expected Impact

Each tier now posts with ~0.33 XCH, well above the 0.1 threshold. Even with very low free inventory, at least some tiers will pass the dust filter.

---

## Test Results

```
281/281 tests passed (0 skipped)
```

Key tests validating the half-spread cap:
- `AvellanedaStoikovTest.HalfSpreadAlwaysPositive`
- `GlftStrategyTest.BaseHalfSpreadAlwaysPositive`
- `RegimeDetectionTest.MeanRevertingNarrowsSpread` (validates cap-before-regime ordering)

---

## Deployment Checklist

- [x] Code changes implemented
- [x] All 281 tests pass
- [ ] Commit and push changes
- [ ] Stop trader (PID 27852)
- [ ] Relink xop_trader.exe (currently file-locked by running process)
- [ ] Restart trader with new binary
- [ ] Monitor: verify bid offers appear on Dexie within 2 cycles
- [ ] Monitor: verify portfolio rebalancing begins (XCH allocation should increase from 14%)
- [ ] Version bump (0.7.34 → 0.7.35)
