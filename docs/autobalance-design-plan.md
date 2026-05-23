# Auto-Balance Design And Implementation Plan

## Goal
Maintain approximately equal portfolio value between base and quote per configured pair (default target ratio 0.50) while preserving existing fee/UTXO safety gates and no-loss constraints.

## Current State Summary
- Existing behavior is threshold/depletion driven, not target-ratio balancing.
- Current one-sided posting logic triggers when spendable is below `min_reserve_units` or `min_trading_units`.
- Drift analyzer provides recommendations but does not execute direct balancing actions.
- Recovery mode exists for low XCH, but it is emergency liquidity recovery, not steady-state 50/50 balancing.

## Root Gap
There is no explicit per-pair target-ratio controller with hysteresis and directional policy. The engine does not persist a rebalance intent (acquire base vs acquire quote) from value ratio deviation around 0.50.

## Design

### 1) New Strategy Config: Ratio Rebalance Controls
Add a ratio-based control block in `StrategyConfig`:
- `ratio_rebalance_enabled` (bool, default true)
- `ratio_target` (double, default 0.50)
- `ratio_band_enter` (double, default 0.10)
- `ratio_band_exit` (double, default 0.05)
- `ratio_force_one_sided` (bool, default true)
- `ratio_tier_size_scale_min` (double, default 0.25)
- `ratio_tier_size_scale_max` (double, default 1.25)
- `ratio_reprice_top_tier_bps` (double, default 3.0)

Validation rules:
- `0 < ratio_target < 1`
- `0 < ratio_band_exit <= ratio_band_enter < 0.5`
- `0 < ratio_tier_size_scale_min <= 1`
- `1 <= ratio_tier_size_scale_max <= 3`
- `0 <= ratio_reprice_top_tier_bps <= 100`

### 2) Pair Rebalance State Machine
Per pair:
- Neutral
- AcquireBase (ratio below lower enter threshold)
- AcquireQuote (ratio above upper enter threshold)

Thresholds:
- Enter AcquireQuote when `ratio >= ratio_target + ratio_band_enter`
- Enter AcquireBase when `ratio <= ratio_target - ratio_band_enter`
- Exit to Neutral only when ratio is inside `[ratio_target - ratio_band_exit, ratio_target + ratio_band_exit]`

This hysteresis avoids side-flip churn.

### 3) Step 8 Integration (Primary Control Point)
In `step_manage_offers` after existing wallet gates compute `can_bid/can_ask`, apply ratio intent:
- AcquireBase:
  - allow base-acquiring side only for this pair
  - for base/quote books, this is typically bid side
- AcquireQuote:
  - allow quote-acquiring side only
  - for base/quote books, this is typically ask side

Respect precedence:
- Existing hard wallet safety gates remain stronger.
- Ratio controller can only further restrict sides, never bypass safety gates.

### 4) Step 7 Integration (Secondary Tuning)
In `step_generate_ladder`, scale side sizes from rebalance intent:
- Overweight side size multiplier near `ratio_tier_size_scale_min`
- Underweight side size multiplier up to `ratio_tier_size_scale_max`

Optional top-tier repricing for active rebalance side:
- Reprice tightest tier by `ratio_reprice_top_tier_bps` toward immediate fill, bounded by existing profitability and order-book safety guards.

### 5) Drift Analyzer Alignment
Continue using drift analyzer for diagnostics/alerts.
Fix usage mismatch by passing true ratio (0..1 centered at 0.5) where currently `abs(ratio)` is used for drift decisions.

## Files To Change
- `cpp/include/xop/config.hpp`
- `cpp/src/config.cpp`
- `config.example.yaml`
- `config.yaml` (runtime defaults for this deployment)
- `cpp/include/xop/engine.hpp`
- `cpp/src/engine.cpp`

## Implementation Sequence
1. Add config fields + parsing + validation.
2. Add per-pair rebalance mode state in `Engine`.
3. Compute ratio intent once per pair per block (Step 7 or early Step 8).
4. Enforce one-sided posting from ratio intent in Step 8.
5. Add side-size scaling in Step 7.
6. Wire logging/metrics for ratio mode transitions.
7. Fix drift analyzer ratio input usage (`abs(ratio)` issue).
8. Build and run smoke test.

## Logging And Metrics
Add logs:
- mode transitions (Neutral -> AcquireBase/AcquireQuote -> Neutral)
- effective `can_bid/can_ask` after ratio controller
- side-size multipliers applied

Add metrics (if available in exporter):
- `xop_ratio_rebalance_mode{pair}` = 0/1/2
- `xop_inventory_ratio{pair}`
- `xop_ratio_rebalance_blocks_active{pair}`

## Test Plan

### Unit/logic
- Config validation boundaries.
- State machine transitions with hysteresis.
- Side mapping for base acquisition vs quote acquisition.

### Integration behavior
- Ratio high (> target+enter): only quote-acquiring side posts.
- Ratio low (< target-enter): only base-acquiring side posts.
- Ratio in exit band: two-sided posting resumes.
- Existing gates (pending_change, spendable reserve, fee reserve) still dominate.

### Regression checks
- No crossed-mid posts.
- No violation of no-loss floor.
- No unexpected offer churn due to mode flipping.

## Rollout Plan
- Phase 1: enable ratio controller with conservative band and only one-sided forcing.
- Phase 2: enable side-size scaling.
- Phase 3: optional top-tier repricing if needed.

Suggested initial values for this deployment:
- `ratio_target: 0.50`
- `ratio_band_enter: 0.10`
- `ratio_band_exit: 0.05`
- `ratio_force_one_sided: true`
- `ratio_tier_size_scale_min: 0.35`
- `ratio_tier_size_scale_max: 1.15`
- `ratio_reprice_top_tier_bps: 2.0`

## Known Risks And Mitigations
- Risk: over-trading/churn near center.
  - Mitigation: hysteresis and minimum dwell blocks if needed.
- Risk: too aggressive recovery causing poor fills.
  - Mitigation: conservative scaling caps and bps limits.
- Risk: conflicts with recovery mode.
  - Mitigation: recovery mode remains higher-priority emergency override.
