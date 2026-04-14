# Logic Review — Midpoint Recycling Implementation Plans
**Date:** 2026-04-13  
**Author:** GitHub Copilot (Claude Opus 4.6)  
**Scope:** Review of logic correctness in the three midpoint recycling planning documents.  
**Status:** Review complete. Changes applied inline to Full Plan and MVP Redline.  

Documents reviewed:
1. CODEREVIEW-20260413-...-Midpoint-Recycling-Plan.md (Full Plan)
2. CODEREVIEW-20260413-...-Midpoint-Recycling-Plan-REDLINE-MVP.md (MVP Redline)
3. CODEREVIEW-20260413-...-Midpoint-Recycling-Full-Rollout-Plan.md (Rollout Blueprint)

---

## Summary of Findings

16 issues identified: 6 critical logic errors, 4 design gaps, 6 improvements.  
All critical and design-gap items have been patched into the Full Plan and MVP Redline.

---

## Critical Logic Issues (C1–C6)

### C1. Edge formulas are inconsistent and both wrong

**Full Plan Section 6.3:**
```
expected_edge_bps = relist_markup_bps - fee_buffer_bps - toxicity_buffer_bps - slippage_penalty_bps
```
This omits the raw take discount (the gap between fair value and the ask we take). It's measuring expected relist profit only, ignoring the actual entry price.

**MVP Redline Section 4:**
```
net_edge_bps = raw_edge_bps - fee_bps - toxicity_buffer_bps - slippage_buffer_bps
```
Closer to correct but only accounts for a single fee, while the round-trip (take + relist) requires two blockchain fees.

**Corrected formula:**
```
take_discount_bps = (fair_value - ask_price) / fair_value × 10000
gross_edge_bps    = take_discount_bps
net_edge_bps      = gross_edge_bps - fee_bps_take - fee_bps_relist - toxicity_buffer_bps - slippage_buffer_bps
```

Where `fee_bps_take` and `fee_bps_relist` are each `fee_mojos / notional_mojos × 10000`. For a 0.15 XCH take at 100M mojos fee, each leg is ~6.6 bps, so the round-trip fee drag is ~13 bps — not ~6.6 bps. This nearly doubles the profitability threshold.

**Impact:** With min_expected_edge_bps at 15 and a single fee buffer of 8 bps, the strategy appeared profitable. With correct double-fee accounting, the true breakeven is higher. The fee_buffer_bps default has been raised accordingly.

### C2. Missing `slippage_buffer_bps` in Full Plan config

Full Plan Section 5 lists 15 config fields but omits `slippage_buffer_bps`. The edge formula references `slippage_penalty_bps` but there's no corresponding config field. MVP Redline correctly added it as field #10.

**Fix:** Added `midpoint_recycling_slippage_buffer_bps: 5` to Full Plan Section 5.

### C3. Missing pending_change gate

Step 8 of the engine has a hard block when `pending_change > 0`:
```cpp
if (pending_change > 0) {
    // Hard block: skip ALL posting until coins settle
    continue;
}
```

After Step 9c executes a take, `pending_change` goes non-zero. Step 9d runs immediately after 9c within the same coroutine. Without a pending_change gate, Step 9d may attempt a take with funds that are locked in a pending transaction.

**Fix:** Added pending_change check to the guard list in both Full Plan and MVP Redline.

### C4. Missing spendable coin pre-check

Step 9c has a 70+ line pre-check (engine.cpp lines 5829–5906) that:
1. Checks at least 0.25 XCH spendable for the take fee
2. If insufficient, cancels the worst pending offer to free a UTXO
3. Verifies the freed balance before proceeding

Step 9d plans omit this entirely. Since 9d runs after 9c (which may have consumed the last available coin), Step 9d needs its own fresh spendable check.

**Fix:** Added XCH spendable pre-check requirement to both plans. Step 9d should re-fetch `wallet_->get_wallet_balance(1)` spendable_balance before any take attempt.

### C5. Missing quote-currency spendable check

To take an ask on XCH/wUSDC.b (buy XCH, pay wUSDC.b), the wallet must hold spendable wUSDC.b. Step 9c doesn't need this check because crossed-book arb profits are immediate. But midpoint recycling pays quote currency now and only recoups it later via relist fills. Neither plan verifies quote currency availability.

**Fix:** Added quote-currency spendable gate: `spendable_quote >= candidate_size × candidate_price` before take.

### C6. Missing minimum candidate size filter

Both plans specify `max_take_xch` but no minimum. For micro-offers (e.g., 0.001 XCH), fee drag as bps is enormous:
```
fee_bps = 100M mojos / (1M mojos) × 10000 = 1,000,000 bps
```

The net_edge calculation would reject these, but only after wasting the Dexie API call to fetch offer text. A pre-filter avoids pointless RPC calls.

**Fix:** Added `midpoint_recycling_min_take_xch: 0.05` to both plans.

---

## Design Gaps (D1–D4)

### D1. Step 9c and 9d interaction within same heartbeat

Both sub-steps execute takes and share the same wallet. The plans don't address:
- Ordering guarantee (must be sequential, not overlapping)
- Balance state refresh between 9c and 9d
- Combined per-block take budget across both sub-steps

**Fix:** Added interaction protocol in both plans: 9d runs strictly after 9c returns, re-fetches wallet balances, and shares the wallet circuit breaker.

### D2. xch_ask_throttle interaction not addressed

When XCH inventory exceeds caution thresholds (config.strategy.xch_ask_throttle_caution_xch = 2.0), the engine suppresses ask competitiveness to discourage further XCH sales. Midpoint recycling on XCH-base asks accumulates XCH, potentially pushing into throttle territory.

**Fix:** Added to pitfalls section: gate midpoint takes when xch_ask_throttle is in caution or above.

### D3. Daily reset mechanism undefined

Both plans reference `midpoint_daily_taken_xch_` and a daily cap, but neither defines how "daily" maps to block height. Chia blocks average 18.75 seconds, so ~4608 blocks per day. Without a defined epoch, the cap is unenforceable.

**Fix:** Specified `midpoint_recycling_epoch_blocks: 4608` in config and defined the reset mechanism.

### D4. CEX reference staleness not quantified

MVP mentions "require fresh CEX timestamp" but never defines "fresh." CoinGecko data can lag 1–5 minutes. A stale reference makes fair-value inaccurate.

**Fix:** Added `midpoint_recycling_max_cex_age_blocks: 10` (~3 minutes) to config as staleness ceiling.

---

## Improvements (I1–I6)

### I1. Dry-run mode not specified for Step 9d
Step 9c has explicit dry-run mode support. Step 9d must also respect `dry_run_` for the instrumentation-only rollout phases.

### I2. Wallet sync check for Step 9d
Step 8 checks wallet sync before offer management. Step 9c relies on the wallet circuit breaker but not an explicit sync check. Step 9d should check both circuit breaker AND sync status.

### I3. Separate take discount from relist markup
The strategy's profit has two components: (a) entry discount vs fair value, (b) relist markup vs fair value. Current formulas conflate these. For the MVP with no relist bias, only (a) matters. For the full plan with relist bias, both contribute to gross edge.

### I4. Alert on midpoint take
Step 9c sends an alert on successful take. Step 9d should do the same for operational visibility.

### I5. Fee tracker integration
Step 9c records fees via `fee_tracker_->record_fee()` after takes. Step 9d must do the same to maintain accurate fee budget tracking.

### I6. Inventory ratio must specify direction
Plans say "inventory_ratio_cap: 0.60" without defining the ratio. Should be: `xch_value / (xch_value + quote_value)` for XCH-base pairs, matching the existing `peg_arb_max_inventory_ratio` semantics.

---

## Config Surface Changes Required

**Fields to add** (not in original plans):
- `midpoint_recycling_min_take_xch: 0.05` — minimum candidate size
- `midpoint_recycling_epoch_blocks: 4608` — daily cap reset period
- `midpoint_recycling_max_cex_age_blocks: 10` — CEX reference max staleness

**Fields with changed defaults** (due to double-fee accounting):
- `midpoint_recycling_fee_buffer_bps: 8 → 15` — must cover TWO blockchain fees
- `midpoint_recycling_min_expected_edge_bps: 15 → 20` — raised to maintain profitability with higher fee drag

---

## Changes Applied

1. Full Plan: edge formula corrected, config surface updated, guards expanded, new pitfalls added.
2. MVP Redline: edge formula corrected, config surface updated, guards and risk gates expanded, test plan amended.
3. Rollout Plan: no structural changes needed (operational, not logic-level).
