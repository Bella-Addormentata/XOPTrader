# Redline MVP Plan - Midpoint Recycling
**Date:** 2026-04-13  
**Author:** GitHub Copilot (GPT-5.3-Codex)  
**Type:** Redline against the full plan in [CODEREVIEW-20260413-GitHubCopilot-GPT5.3-Codex-v1-Midpoint-Recycling-Plan.md](CODEREVIEW-20260413-GitHubCopilot-GPT5.3-Codex-v1-Midpoint-Recycling-Plan.md)  
**Status:** Proposed for review before coding.

---

## 1. Redline Intent

This version intentionally narrows scope to a minimum viable implementation that can be validated quickly and safely.

Primary goals of the redline:
1. Keep the core near-mid take logic.
2. Reuse existing Step 9 taker infrastructure.
3. Avoid schema migrations and deep Step 7/8 relist modifications in phase 1.
4. Add enough telemetry to evaluate go/no-go without overbuilding.

---

## 2. Keep, Change, Defer

## Keep
1. Add midpoint recycling under the existing arbitrage pipeline in Step 9.
2. Start with a single pair: XCH/wUSDC.b.
3. Require positive net expected edge after fees and toxicity buffer.
4. Enforce strict per-block and daily caps.
5. Keep instrumentation-first rollout.

## Change
1. Replace broad config surface with a smaller MVP config set.
2. Remove temporary relist-bias mechanics in MVP.
3. Use existing maker relisting behavior after take, without custom markup logic.
4. Use metrics-first observability; postpone new database table.

## Defer
1. New midpoint_recycling_log table and realized edge persistence.
2. Pair-level relist bias injection into Step 7 or liquidity engine.
3. Bid-side symmetric midpoint recycling.
4. Multi-pair expansion beyond XCH/wUSDC.b.
5. Fine-grained reject-reason labels as high-cardinality metrics.

---

## 3. Research-Grounded MVP Rationale

### Inventory discipline remains first-class
1. Avellaneda and Stoikov (2008): reservation-price logic implies midpoint takes must be inventory-gated.
2. Gueant, Lehalle, Fernandez-Tapia (2013): repeated one-sided takes can destabilize inventory if not capped.

### Signal quality and toxicity gating are mandatory
1. Cartea, Donnelly, Jaimungal (2018): order-book signals add value only with robust filtering.
2. Easley, Lopez de Prado, O'Hara (2012): toxicity-aware gates are required to avoid adverse flow.

### Why narrow to one pair first
1. Budish, Cramton, Shim (2015): stale signal risk increases sharply in fragmented, fast settings.
2. Gould et al. (2013): microstructure effects vary by instrument liquidity; transferability is weak.

---

## 4. MVP Strategy Definition

For XCH/wUSDC.b only:
1. Detect small external asks near fair value.
2. Estimate net edge conservatively.
3. Take at most one offer per block when all gates pass.
4. Let existing market-making flow relist naturally.

Net edge formula for MVP:

net_edge_bps = raw_edge_bps - fee_bps - toxicity_buffer_bps - slippage_buffer_bps

where:
1. raw_edge_bps = (fair_price - ask_price) / fair_price * 10000
2. fee_bps is estimated from recommended fee and intended notional
3. toxicity_buffer_bps and slippage_buffer_bps are fixed config buffers

Take condition:
1. net_edge_bps >= min_expected_edge_bps
2. cooldown not active
3. daily cap not exceeded
4. inventory ratio below cap
5. VPIN below cap
6. wallet circuit open is false
7. recovery mode is false

---

## 5. MVP Config Surface (Redlined)

Add these fields to ArbitrageSettings in [config.hpp](../../cpp/include/xop/config.hpp#L630), parser in [config.cpp](../../cpp/src/config.cpp#L1428), and YAML files [config.yaml](../../config.yaml), [config.example.yaml](../../config.example.yaml):

1. midpoint_recycling_enabled: false
2. midpoint_recycling_pairs: ["XCH/wUSDC.b"]
3. midpoint_recycling_band_bps: 20
4. midpoint_recycling_max_take_xch: 0.15
5. midpoint_recycling_max_takes_per_block: 1
6. midpoint_recycling_daily_take_xch_cap: 2.0
7. midpoint_recycling_min_expected_edge_bps: 15
8. midpoint_recycling_fee_buffer_bps: 8
9. midpoint_recycling_toxicity_buffer_bps: 12
10. midpoint_recycling_slippage_buffer_bps: 5
11. midpoint_recycling_cooldown_blocks: 4
12. midpoint_recycling_inventory_ratio_cap: 0.60
13. midpoint_recycling_require_cex_ref: true
14. midpoint_recycling_vpin_max: 0.70

Validation rules:
1. Non-negative bps and caps.
2. max_takes_per_block >= 1.
3. inventory_ratio_cap in (0, 1].
4. midpoint_recycling_pairs must not be empty when enabled.

---

## 6. File-by-File MVP Implementation

## 6.1 Config and Summary Logging

### [cpp/include/xop/config.hpp](../../cpp/include/xop/config.hpp)
1. Extend ArbitrageSettings with the 14 MVP fields above.
2. Keep defaults conservative and disabled.

### [cpp/src/config.cpp](../../cpp/src/config.cpp)
1. Extend parse_arbitrage to read and validate fields.
2. Extend log_config_summary to print midpoint recycling status and key thresholds.

### [config.yaml](../../config.yaml)
1. Add midpoint_recycling section under arbitrage with enabled false.

### [config.example.yaml](../../config.example.yaml)
1. Add comments that explicitly state this is experimental and pair-limited.

---

## 6.2 Engine State (Minimal)

### [cpp/include/xop/engine.hpp](../../cpp/include/xop/engine.hpp)
Add only minimal state:
1. std::unordered_map<std::string, BlockHeight> midpoint_last_take_block_
2. std::unordered_map<std::string, double> midpoint_daily_taken_xch_
3. BlockHeight midpoint_daily_reset_block_ (or reuse existing day-window pattern)

No relist-bias structure in MVP.

---

## 6.3 Step 9d in Engine

### [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L5790)
Add Step 9d immediately after existing Step 9c crossed-book block.

Execution sequence:
1. Global guards:
1. arbitrage enabled and midpoint_recycling_enabled
2. wallet circuit breaker closed
3. not in xch_recovery_mode
4. not dry-run unless in instrumentation phase

2. Pair filter:
1. process only configured midpoint_recycling_pairs
2. phase 1 allow only XCH/wUSDC.b

3. Candidate discovery:
1. fetch competing offers
2. keep external asks only
3. keep size <= midpoint_recycling_max_take_xch
4. keep abs((ask - fair)/fair) <= midpoint_recycling_band_bps

4. Candidate scoring:
1. compute raw edge versus fair value
2. estimate fee_bps from current recommended fee and candidate notional
3. net_edge_bps = raw - fee - toxicity - slippage

5. Risk gates:
1. net_edge_bps gate
2. VPIN gate
3. inventory ratio gate
4. cooldown gate
5. daily cap gate

6. Take path:
1. fetch full offer text via current Dexie offer_status path
2. verify offer still active
3. take via wallet take_offer
4. update midpoint_last_take_block and midpoint_daily_taken_xch

7. Exit condition:
1. stop after midpoint_recycling_max_takes_per_block successful takes

No explicit relist override in MVP; existing Step 7 and Step 8 behavior remains unchanged.

---

## 6.4 Metrics-Only Observability for MVP

### [cpp/src/monitoring/metrics.cpp](../../cpp/src/monitoring/metrics.cpp)
Add low-cardinality metrics only:
1. xop_midpoint_recycle_candidates_total{pair}
2. xop_midpoint_recycle_takes_total{pair}
3. xop_midpoint_recycle_net_edge_bps{pair}
4. xop_midpoint_recycle_daily_taken_xch{pair}

Do not add high-cardinality reject_reason labels in MVP.

### [cpp/src/database.cpp](../../cpp/src/database.cpp)
No schema changes in MVP redline.

Rationale:
1. Avoid migration risk while proving strategy viability.
2. Use logs and metrics for first evaluation cycle.

---

## 7. Explicit Pitfalls to Avoid in MVP

1. Fee-underestimation on tiny notional takes.
Mitigation: require fee_bps conversion using candidate notional, not fixed bps.

2. CEX reference staleness.
Mitigation: require fresh CEX timestamp when midpoint_recycling_require_cex_ref is true.

3. One-sided inventory accumulation.
Mitigation: strict inventory ratio cap plus daily take cap.

4. Pending-change and UTXO starvation after taker action.
Mitigation: reuse existing liberation and buy-only safety gates, plus cooldown.

5. False confidence from small sample windows.
Mitigation: minimum sample thresholds before activation expansion.

6. Interaction with crossed-book logic.
Mitigation: keep Step 9c untouched and isolate Step 9d feature-flagged path.

7. Label explosion in metrics.
Mitigation: pair-only labels in MVP.

8. Overfitting to one market regime.
Mitigation: run instrumentation and dry-run over multiple volatility regimes before live.

---

## 8. MVP Test Plan

## Unit tests
1. Net edge calculation and fee_bps conversion.
2. Candidate filtering by size and band.
3. Cooldown and daily-cap gating.
4. Config parser validation boundaries.

## Integration tests
1. Synthetic near-mid asks with positive edge produce takes.
2. High-fee scenario suppresses takes.
3. Recovery mode active suppresses Step 9d.
4. Existing Step 9c behavior unchanged when midpoint mode toggled.

## Regression tests
1. Build and runtime unchanged when midpoint_recycling_enabled is false.
2. No additional pending-change spikes under no-op mode.

---

## 9. Rollout Redline

## Phase 0 - Instrumentation only
1. Compute and log candidate and net edge.
2. No takes.

Gate to Phase 1:
1. Candidate quality stable.
2. No metric anomalies.

## Phase 1 - Dry-run decisioning
1. Simulate take decisions and track hypothetical edge.

Gate to Phase 2:
1. Positive hypothetical net edge after buffers.
2. Toxicity gate blocks loss-heavy windows.

## Phase 2 - Limited live
1. XCH/wUSDC.b only
2. one take per block max
3. daily cap 2 XCH

Gate to expansion:
1. positive realized net after fees
2. no wallet health regressions
3. inventory remains within cap

---

## 10. Deferred Work Package (Post-MVP)

1. Add midpoint_recycling_log table with realized edge lifecycle.
2. Add relist-bias mechanics with temporary spread/size controls.
3. Add bid-side symmetric recycling logic.
4. Add multi-pair support with pair-specific thresholds.
5. Add richer reject-reason telemetry.

---

## 11. Review Checklist

1. Confirm MVP should remain asks-only for first release.
2. Confirm no database migration in MVP.
3. Confirm default enabled is false.
4. Confirm strict caps and buffers are acceptable.
5. Confirm success criteria thresholds before coding begins.

---

## 12. Recommended Go-Forward

Approve this redline MVP first, then implement in one feature branch with:
1. config and parser changes,
2. Step 9d engine logic,
3. minimal metrics,
4. tests,
5. documentation updates.

This keeps blast radius low while giving statistically useful evidence for whether the broader full-plan features are warranted.
