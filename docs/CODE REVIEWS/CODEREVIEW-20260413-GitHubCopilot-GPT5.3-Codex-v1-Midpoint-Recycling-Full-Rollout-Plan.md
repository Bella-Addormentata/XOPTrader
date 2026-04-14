# Full Rollout Plan - Midpoint Recycling Strategy
Date: 2026-04-13  
Author: GitHub Copilot (GPT-5.3-Codex)  
Status: Proposed for review before implementation  
Depends on:
1. CODEREVIEW-20260413-GitHubCopilot-GPT5.3-Codex-v1-Midpoint-Recycling-Plan.md
2. CODEREVIEW-20260413-GitHubCopilot-GPT5.3-Codex-v1-Midpoint-Recycling-Plan-REDLINE-MVP.md

---

## 1. Purpose

This document defines the complete production rollout sequence for midpoint recycling, from implementation through broad live deployment and steady-state operations.

Primary outcomes:
1. Improve net realized spread capture after fees.
2. Improve top-of-book quality participation without destabilizing inventory.
3. Preserve existing safety invariants in wallet health, risk limits, and recovery logic.

---

## 2. Rollout Principles

1. Safety-first over speed.
2. Instrumentation before execution.
3. Single-variable progression at each phase.
4. Feature flags and hard kill switches at every layer.
5. Quantitative go and no-go criteria.
6. Fast rollback path with no schema-coupled dependency for first live wave.

---

## 3. Research-Linked Controls

The rollout controls are explicitly grounded in microstructure research:

1. Inventory control discipline
Source: Avellaneda and Stoikov (2008), Gueant et al. (2013)
Control: strict inventory ratio caps, per-block and daily take caps, asymmetric expansion.

2. Toxicity-aware execution
Source: Easley, Lopez de Prado, O'Hara (2012), Cartea et al. (2018)
Control: VPIN ceiling, toxicity buffers, stale-reference rejection.

3. Queue and spread realism
Source: Cont, Stoikov, Talreja (2010), Gould et al. (2013)
Control: near-mid band gating, pair-specific liquidity constraints, reject oversized atomic offers.

4. Latency and sniping risk
Source: Budish, Cramton, Shim (2015)
Control: cooldown blocks, conservative slippage buffers, canary-only ramp.

---

## 4. Scope by Wave

## Wave A (MVP)
1. Single pair: XCH/wUSDC.b.
2. Ask-side near-mid takes only.
3. No custom relist bias.
4. Metrics-first observability.
5. No midpoint-specific database migration.

## Wave B (Expanded Engine)
1. Optional relist bias mechanics.
2. Bid-side symmetry where validated.
3. Pair-specific thresholds.
4. Optional midpoint decision log table.

## Wave C (Portfolio Rollout)
1. Multi-pair deployment.
2. Dynamic per-pair caps from allocator and risk signals.
3. Automated regime-specific parameter switching.

---

## 5. Workstreams and Owners

## Workstream 1 - Strategy and Engine
1. Implement Step 9d midpoint logic in engine.
2. Add state tracking for cooldown and daily cap.
3. Integrate inventory and toxicity gates.

## Workstream 2 - Config and Controls
1. Add midpoint_recycling settings to config structs and parser.
2. Add config summary output and validation guards.
3. Add runtime feature-flag kill switch.

## Workstream 3 - Observability
1. Add midpoint metrics in exporter.
2. Add dashboard panels and alert rules for key failure modes.
3. Add rollout-specific runbook alerts.

## Workstream 4 - Testing and QA
1. Unit tests for edge math and gates.
2. Integration tests for Step 9c and Step 9d coexistence.
3. Replay and dry-run test campaigns.

## Workstream 5 - Operations and Governance
1. Phase gate reviews.
2. Daily decision log.
3. Rollback and incident drills.

---

## 6. Detailed Phase Plan

## Phase 0 - Design Lock and Safety Contract (2 to 3 days)

Objectives:
1. Freeze MVP functional scope.
2. Lock metrics, alerts, and acceptance criteria.

Entry criteria:
1. Full plan and MVP redline approved.
2. Pair scope approved: XCH/wUSDC.b only.

Deliverables:
1. Signed parameter sheet.
2. Signed kill-switch and rollback policy.
3. Test matrix and owner assignment.

Exit criteria:
1. No open critical design questions.
2. All no-go triggers documented.

---

## Phase 1 - Implementation and CI Hardening (4 to 7 days)

Objectives:
1. Implement midpoint recycling MVP behind feature flag.
2. Add required tests and metrics.

Code surface:
1. cpp/include/xop/config.hpp
2. cpp/src/config.cpp
3. cpp/src/engine.cpp
4. cpp/src/monitoring/metrics.cpp
5. config.example.yaml
6. config.yaml (disabled by default)
7. test files under cpp/tests

Entry criteria:
1. Phase 0 exit complete.

Required tests:
1. Net edge formula and fee_bps conversion.
2. Candidate filtering by near-mid band and size caps.
3. Cooldown, daily cap, inventory cap, VPIN cap gates.
4. Recovery mode and wallet-circuit suppression.
5. Step 9c unchanged when midpoint disabled.

Exit criteria:
1. All tests pass.
2. No new critical lint or build errors.
3. Feature disabled by default and no-op behavior verified.

No-go triggers:
1. Step 9 regression on crossed-book path.
2. Safety gates bypassable in unit or integration test.

---

## Phase 2 - Shadow and Instrumentation-Only (5 to 10 days)

Objectives:
1. Run live data with decisioning enabled but no actual takes.
2. Validate candidate quality and gate behavior.

Runtime mode:
1. midpoint_recycling_enabled true.
2. execution action set to observe-only or dry-run path.

KPIs tracked daily:
1. candidate_count
2. would_take_count
3. expected_net_edge_bps distribution
4. reject reasons aggregate
5. overlap with high VPIN periods
6. projected fee drag versus projected edge

Entry criteria:
1. Phase 1 complete.

Exit criteria:
1. Stable candidate stream.
2. Positive projected net edge after fee and toxicity buffers.
3. No persistent spikes in pending_change or stuck offer metrics.

No-go triggers:
1. Projected edge remains negative for 3 consecutive days.
2. Candidate stream dominated by high-toxicity windows.

---

## Phase 3 - Canary Live Execution (7 to 14 days)

Objectives:
1. Execute real takes at very low risk budget.
2. Validate realized edge and operational stability.

Canary parameters:
1. Pair: XCH/wUSDC.b only.
2. max_takes_per_block: 1.
3. daily_take_cap_xch: 1.0 to 2.0.
4. midpoint band slack above the derived actionable floor: conservative.
5. strict cooldown and inventory cap.

Live KPIs:
1. realized_net_edge_bps per take
2. realized_net_pnl_mojos from midpoint cohort
3. fee_to_gain ratio
4. inventory drift versus baseline
5. pending_change and stuck offer deltas
6. fill quality after relisting

Entry criteria:
1. Phase 2 exit complete.

Exit criteria:
1. Positive realized net edge with statistical confidence threshold met.
2. No material degradation in core market-making KPIs.
3. No safety incident requiring emergency disable.

No-go triggers:
1. Negative realized net edge for 2 review windows.
2. Inventory concentration breach attributable to midpoint flow.
3. Wallet health degradation beyond threshold.

Rollback policy:
1. Immediate disable flag.
2. Revert to baseline Step 9 behavior only.
3. Post-incident review within 24 hours.

---

## Phase 4 - Limited Production Ramp (2 to 4 weeks)

Objectives:
1. Increase notional gradually.
2. Validate robustness across changing volatility regimes.

Ramp schedule:
1. Week 1: increase daily cap by 25 percent if all KPIs green.
2. Week 2: increase by additional 25 percent if still green.
3. Week 3+: hold or reduce based on risk and quality signals.

Additional controls:
1. Dynamic tightening of edge thresholds in stressed regimes.
2. Automatic cap reduction when VPIN or spread stress rises.

Exit criteria:
1. Sustained positive realized net edge.
2. No regression in drawdown profile.
3. Operational burden acceptable for on-call team.

No-go triggers:
1. Increased cancel churn and pending lockups.
2. Repeated kill-switch events.

---

## Phase 5 - Feature Expansion (Wave B, optional)

Objectives:
1. Introduce deferred capabilities after proven MVP success.

Potential additions:
1. Relist bias module.
2. Bid-side recycling symmetry.
3. Midpoint decision log persistence table.
4. Pair-specific adaptive parameter sets.

Precondition:
1. Phase 4 stable for at least 2 full review cycles.

Exit criteria:
1. Each added capability passes same canary gate process.

---

## Phase 6 - Multi-Pair Portfolio Rollout (Wave C)

Objectives:
1. Add additional pairs with calibrated thresholds.
2. Coordinate with allocator and cross-pair risk controls.

Pair admission checklist:
1. Reliable fair-value reference quality.
2. Sufficient liquidity and stable competitor depth.
3. No unresolved denomination or pricing anomalies.

Expansion rule:
1. Add one new pair at a time.
2. Repeat canary process per pair.

Exit criteria:
1. Portfolio-level net benefit remains positive after fees and operational cost.

---

## 7. Parameter Evolution Plan

Start values:
1. band_bps 20 (slack above derived actionable floor)
2. min_expected_edge_bps 15
3. fee_buffer_bps 8
4. toxicity_buffer_bps 12
5. slippage_buffer_bps 5
6. cooldown_blocks 4
7. daily_take_cap_xch 2.0
8. inventory_ratio_cap 0.60

Adjustment cadence:
1. No parameter change more than once per 48 hours.
2. Change one parameter family at a time.
3. Keep full audit trail for each parameter move.

Parameter change preconditions:
1. At least one full review window of stable data.
2. No active incident.

---

## 8. SLOs, KPIs, and Phase Gates

Primary SLOs:
1. Realized midpoint cohort net edge remains positive.
2. No increase above threshold in pending_change and stuck offers.
3. Inventory concentration remains within policy limits.

Core KPI definitions:
1. Realized net edge per take.
2. Midpoint cohort PnL after fees.
3. Fee-to-gain ratio.
4. Candidate-to-execution conversion rate.
5. Rejection profile concentration.
6. Time to disable and recover after kill-switch event.

Gate policy:
1. Every phase requires explicit go decision.
2. Any red no-go trigger halts progression automatically.

---

## 9. Alerting and Operational Runbook

Required alerts:
1. Midpoint realized edge below threshold.
2. Daily cap reached unexpectedly early.
3. Inventory cap near breach.
4. VPIN regime stress while midpoint takes active.
5. Pending-change persistence increase.
6. Wallet circuit breaker transitions.

Runbook actions:
1. First-level response is midpoint feature disable only.
2. Second-level response is rollback to last known good build if behavior persists.
3. Incident timeline, root cause, and corrective action documented within one business day.

---

## 10. Rollback and Disaster Recovery

Rollback levels:
1. Level 1: disable midpoint_recycling flag at runtime.
2. Level 2: rollback config to last stable parameter set.
3. Level 3: rollback release artifact to previous commit.

Rollback SLO:
1. Feature disable in under 5 minutes.
2. Full rollback in under 30 minutes.

Recovery criteria:
1. System metrics and wallet health return to baseline envelope.
2. Root cause identified and documented.

---

## 11. Data Review Cadence and Governance

Cadence:
1. Daily quick review during Phases 2 and 3.
2. Twice-weekly deep review during Phases 4 and 5.
3. Weekly executive summary during portfolio rollout.

Governance checklist for each review:
1. KPI status green, yellow, red.
2. Parameter changes approved or denied.
3. Incidents and near-misses.
4. Recommendation for hold, ramp, or rollback.

---

## 12. Compliance and Market Conduct Guardrails

1. Strategy objective is passive liquidity quality improvement, not artificial price movement.
2. No spoofing-like behavior or non-economic order activity.
3. No bypass of existing risk, fee, or wallet safeguards.
4. Every midpoint take must pass net economic and safety gates.

---

## 13. Full Rollout Timeline (Indicative)

1. Week 1: Phase 0 and Phase 1 complete.
2. Week 2: Phase 2 shadow complete.
3. Weeks 3 to 4: Phase 3 canary live.
4. Weeks 5 to 8: Phase 4 limited production ramp.
5. Weeks 9 to 12: Phase 5 optional enhancements.
6. Week 13 onward: Phase 6 pair-by-pair portfolio expansion.

Timeline adjustments:
1. Any red trigger pauses timeline automatically.
2. Stability windows take priority over date targets.

---

## 14. Final Go-Live Checklist

1. Feature flag default off verified.
2. Kill switch tested in production-like environment.
3. Unit, integration, and regression suites green.
4. Dashboard and alerts verified end-to-end.
5. On-call runbook dry-run complete.
6. Data review owners assigned and available.

---

## 15. Decision Log Template

For each phase gate, record:
1. Date and phase.
2. KPI summary.
3. Incidents since last gate.
4. Parameter changes applied.
5. Decision: go, hold, rollback.
6. Sign-off by strategy, engineering, and operations reviewers.

---

## 16. Summary

This full rollout plan converts midpoint recycling from a design concept into an operational program with strict safety gates, quantitative phase criteria, and deterministic rollback paths. It starts with low blast radius and expands only on evidence of positive realized net value and unchanged core system stability.
