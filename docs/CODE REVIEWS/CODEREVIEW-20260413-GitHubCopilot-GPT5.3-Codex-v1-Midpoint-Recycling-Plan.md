# Implementation Plan - Midpoint Recycling Strategy (Take Small Near-Mid Offers, Relist Profitably)
**Date:** 2026-04-13  
**Author:** GitHub Copilot (GPT-5.3-Codex)  
**Scope:** Design-only plan (no code changes in this document).  
**Status:** Proposed for review before implementation.

---

## 1. Objective and Non-Objectives

### Objective
Add a controlled strategy module that can:
1. Detect small competing offers near fair value (midpoint neighborhood).
2. Take only those offers with positive expected net edge after fees and risk buffers.
3. Relist inventory through normal maker flow at a target profitable spread.
4. Improve realized spread capture and top-of-book presence without destabilizing inventory.

### Non-Objectives
1. Do not attempt to manipulate markets or create artificial price pressure.
2. Do not bypass existing risk controls, wallet safety gates, or fee budgeting.
3. Do not introduce aggressive high-frequency churn that increases pending-change lockups.
4. Do not reorder the 13-step heartbeat in phase 1.

### Current Baseline in Repository
1. Crossed-book taker exists (Step 9c), but only for crossed books (`bid >= ask`) and minimum edge gates: [engine.cpp](../../cpp/src/engine.cpp#L5937), [engine.cpp](../../cpp/src/engine.cpp#L5944), [engine.cpp](../../cpp/src/engine.cpp#L5998).
2. Recovery taker exists, but only when XCH is severely low and with normal MM disabled: [engine.cpp](../../cpp/src/engine.cpp#L1428), [engine.cpp](../../cpp/src/engine.cpp#L6062).
3. Competitive anchoring and PID spread adaptation already exist in config/runtime: [config.yaml](../../config.yaml#L112), [config.yaml](../../config.yaml#L128).

---

## 2. Strategy Thesis

The strategy should be framed as **liquidity quality optimization**, not midpoint control. In thin books, consistently quoting and selectively recycling near-mid inventory can influence effective spread conditions for short windows, but durable control is unlikely unless we dominate top-of-book size and uptime.

We therefore optimize for:
1. Better expected edge per unit fee.
2. Better queue quality near the touch.
3. Controlled inventory drift.
4. Lower adverse-selection losses.

---

## 3. Research Backbone (How Literature Maps to Design)

### Inventory-aware quoting and reservation price
1. Avellaneda and Stoikov (2008), *High-frequency trading in a limit order book*.  
Applies directly to: inventory-aware relist pricing and skew after taker fills.
2. Gueant, Lehalle, Fernandez-Tapia (2013), *Dealing with the Inventory Risk*.  
Applies to: stabilizing inventory during repeated take/relist loops.

### Order-book signal quality and adverse selection
1. Cartea, Donnelly, Jaimungal (2018), *Enhancing Trading Strategies with Order Book Signals*.  
Applies to: filtering near-mid candidates using queue and imbalance signals.
2. Easley, Lopez de Prado, O'Hara (2012), *Flow Toxicity and Liquidity in a High-frequency World*.  
Applies to: VPIN/toxicity gating so we avoid taking toxic flow.
3. Cont, Stoikov, Talreja (2010), *A Stochastic Model for Order Book Dynamics*.  
Applies to: queue-position and fill-probability assumptions for relist decisions.

### Microstructure constraints and sniping risk
1. Budish, Cramton, Shim (2015), *The High-Frequency Trading Arms Race*.  
Applies to: avoiding sniped near-mid takes/relists in stale-signal windows.
2. Gould et al. (2013), *Limit Order Books: A Survey*.  
Applies to: practical caution on spread, depth, and short-horizon predictability.

### Market making operations and control
1. Cartea, Jaimungal, Penalva (2015), *Algorithmic and High-Frequency Trading*.  
Applies to: implementation discipline, risk budgets, and monitoring architecture.

---

## 4. High-Level Design

## 4.1 New Mode Name
`midpoint_recycling` (under `arbitrage` section for phase 1, to reuse Step 9 plumbing).

## 4.2 Signal-Action Loop
1. Build fair value reference (existing blended mid from market data).
2. Scan competing offers for small near-mid opportunities.
3. Score each candidate with expected edge net of fees and risk buffers.
4. Take at most a bounded number per block/day.
5. Let existing Step 7/8 relist with an additional temporary relist bias for affected pair.

## 4.3 Why Step 9 Extension (Not New Heartbeat Step in Phase 1)
1. Existing asynchronous taker code path already in Step 9c with all wallet wiring.
2. Keeps flow low risk: no scheduler reorder, no additional coroutine step wiring.
3. Uses existing fee and wallet-circuit guardrails.

---

## 5. Proposed Config Additions

Add to `ArbitrageSettings` in [config.hpp](../../cpp/include/xop/config.hpp#L630) and parser in [config.cpp](../../cpp/src/config.cpp#L1428):

1. `midpoint_recycling_enabled: false`
2. `midpoint_recycling_pairs: ["XCH/wUSDC.b"]`
3. `midpoint_recycling_band_bps: 25`  
Candidate ask/bid must be within this distance of fair value.
4. `midpoint_recycling_max_take_xch: 0.25`
5. `midpoint_recycling_max_takes_per_block: 1`
6. `midpoint_recycling_daily_take_xch_cap: 5.0`
7. `midpoint_recycling_min_expected_edge_bps: 12`
8. `midpoint_recycling_fee_buffer_bps: 5`
9. `midpoint_recycling_toxicity_buffer_bps: 10`
10. `midpoint_recycling_relist_markup_bps: 15`
11. `midpoint_recycling_relist_size_decay: 0.7`
12. `midpoint_recycling_cooldown_blocks: 3`
13. `midpoint_recycling_inventory_ratio_cap: 0.65`
14. `midpoint_recycling_require_cex_ref: true`
15. `midpoint_recycling_vpin_max: 0.75`

Validation rules:
1. Non-negative bps and caps.
2. `max_takes_per_block >= 1`.
3. `inventory_ratio_cap` in `(0,1]`.
4. `relist_size_decay` in `(0,1]`.

Also add to [config.example.yaml](../../config.example.yaml).

---

## 6. File-by-File Implementation Plan

## 6.1 Config and Wiring

### [cpp/include/xop/config.hpp](../../cpp/include/xop/config.hpp)
1. Extend `ArbitrageSettings` with `midpoint_recycling_*` fields.
2. Add comments consistent with current style and defaults rationale.

### [cpp/src/config.cpp](../../cpp/src/config.cpp)
1. Extend `parse_arbitrage()` to parse and validate new fields.
2. Extend `log_config_summary()` with a compact midpoint-recycling section.

### [config.yaml](../../config.yaml)
1. Add conservative defaults with `enabled: false` initially.
2. Enable only after review/backtest.

### [config.example.yaml](../../config.example.yaml)
1. Document each setting with exact meaning and safety guidance.

---

## 6.2 Engine State and Control

### [cpp/include/xop/engine.hpp](../../cpp/include/xop/engine.hpp)
Add state containers:
1. `std::unordered_map<std::string, BlockHeight> midpoint_last_take_block_;`
2. `std::unordered_map<std::string, double> midpoint_daily_taken_xch_;`
3. `struct MidpointRelistBias { double extra_markup_bps; double size_mult; BlockHeight expires_at; };`
4. `std::unordered_map<std::string, MidpointRelistBias> midpoint_relist_bias_;`

Helper declarations:
1. `bool midpoint_recycling_allowed_for_pair(const PairConfig&) const;`
2. `double compute_midpoint_expected_edge_bps(...) const;`
3. `void apply_midpoint_relist_bias(BlockHeight block_height);`

---

## 6.3 Step 9 Extension (Core)

### [cpp/src/engine.cpp](../../cpp/src/engine.cpp)
Inside `step_check_arbitrage(...)` (same region as Step 9c around [engine.cpp](../../cpp/src/engine.cpp#L5790)):

1. Add new subsection "Step 9d: Midpoint recycling" after crossed-book logic.
2. Guards:
1. `config_.arbitrage.enabled && midpoint_recycling_enabled`
2. `!wallet_circuit_open_`
3. Not in `xch_recovery_mode_`
4. Pair allowlist check.

3. Candidate extraction (phase 1: asks only on XCH-base pair):
1. Pull competing offers via existing `market_data_->get_competing_offers(pair.name)`.
2. Keep only active external asks with `size <= max_take`.
3. Keep only offers within `band_bps` of fair value.
4. Require CEX reference if configured.

4. Edge scoring:

`expected_edge_bps = relist_markup_bps - fee_buffer_bps - toxicity_buffer_bps - slippage_penalty_bps`

Only proceed when:
1. `expected_edge_bps >= min_expected_edge_bps`
2. VPIN/toxicity under threshold.
3. Inventory ratio below cap.
4. Cooldown and daily cap not violated.

5. Execution:
1. Fetch bech32 text with current Dexie status fetch pattern.
2. Take with fee from `fee_tracker_->get_recommended_fee(...)`.
3. On success, record counters and install relist bias for pair.

6. Relist bias interaction:
1. `midpoint_relist_bias_[pair] = {markup, size_mult, block + cooldown}`.
2. Step 7/8 uses this temporary bias to avoid immediate mean-reversion loss.

---

## 6.4 Relist Path Integration

### [cpp/src/engine.cpp](../../cpp/src/engine.cpp)
At or before Step 7 generation path:
1. Apply temporary pair-level spread markup and size multiplier for active bias window.
2. Expire bias automatically at `expires_at`.

### [cpp/src/strategy/liquidity.cpp](../../cpp/src/strategy/liquidity.cpp)
Optional (phase 2 if needed):
1. Add explicit hook to incorporate pair-level temporary relist offset in tier pricing.
2. Keep no-loss and spread sanity checks unchanged.

---

## 6.5 Persistence and Telemetry

### [cpp/src/database.cpp](../../cpp/src/database.cpp)
Add new table for auditability:

`midpoint_recycling_log`
1. `timestamp`
2. `block_height`
3. `pair_name`
4. `offer_id`
5. `side`
6. `take_price_mojos`
7. `take_size_mojos`
8. `expected_edge_bps`
9. `realized_edge_bps` (nullable until close)
10. `fee_mojos`
11. `decision_reason` (accepted/rejected reason)

Indexes:
1. `(pair_name, timestamp)`
2. `(decision_reason, timestamp)`

### [cpp/src/monitoring/metrics.cpp](../../cpp/src/monitoring/metrics.cpp)
Add metrics:
1. `xop_midpoint_recycle_candidates_total{pair}`
2. `xop_midpoint_recycle_takes_total{pair}`
3. `xop_midpoint_recycle_reject_total{pair,reason}`
4. `xop_midpoint_recycle_expected_edge_bps{pair}` (gauge)
5. `xop_midpoint_recycle_daily_taken_xch{pair}` (gauge)
6. `xop_midpoint_recycle_inventory_cap_hits_total{pair}`

---

## 7. Exact Decision Logic (Phase 1)

For each allowed pair and block:
1. Fetch fair value and CEX reference.
2. Enumerate candidate asks near fair value.
3. Reject candidate when any condition fails:
1. stale or inactive offer
2. outside near-mid band
3. size above cap
4. expected edge below threshold
5. VPIN above cap
6. inventory ratio above cap
7. cooldown active
8. daily take cap reached
9. fee budget exhausted

4. Execute at most `max_takes_per_block` successful takes.
5. Install relist bias for subsequent maker cycles.

---

## 8. Pitfalls and Mitigations (Critical)

### 8.1 Hidden Pitfall: Atomic offer semantics on Dexie
Risk: no partial fills; each take may be all-or-none and larger than desired.  
Mitigation: hard max-size filter and strict candidate sizing.

### 8.2 Hidden Pitfall: UTXO lockup after taker activity
Risk: successful takes and subsequent relists can starve spendable XCH.  
Mitigation: reuse existing liberation and buy-only safeguards; cap takes/day and per block.

### 8.3 Hidden Pitfall: Adverse selection near local extrema
Risk: near-mid ask may still be toxic right before downward repricing.  
Mitigation: add toxicity buffer, VPIN gate, and cooldown before aggressive relist.

### 8.4 Hidden Pitfall: Fee drag dominates micro-edge
Risk: frequent micro-takes are net negative after chain fees.  
Mitigation: expected-edge must be fee-inclusive; daily fee and take caps.

### 8.5 Hidden Pitfall: Self-competition and quote flicker
Risk: relist too quickly at too tight a level causes churn and pending-change gate stalls.  
Mitigation: cooldown blocks, size decay, preserve anti-churn cancellation discipline.

### 8.6 Hidden Pitfall: Midpoint-control overreach
Risk: strategy starts implicitly targeting spread movement rather than quality-making.  
Mitigation: explicit compliance guardrails and KPI focus on net PnL, not spread displacement.

### 8.7 Hidden Pitfall: Cross-pair inventory contamination
Risk: accumulating XCH on one pair can worsen risk on others.  
Mitigation: global inventory ratio caps and market-allocator-aware limits.

---

## 9. Test Plan (Must Pass Before Enable)

## 9.1 Unit Tests
1. Edge calculation correctness (fees, buffers, min edge).
2. Candidate filter behavior by band/size/cooldown/cap.
3. Inventory and VPIN gate logic.
4. Config parsing and boundary validation.

## 9.2 Integration Tests
1. Synthetic order book with near-mid small asks; verify take/relist sequence.
2. High-fee environment; verify strategy suppresses itself.
3. Pending-change stress; verify no uncontrolled churn.
4. Recovery-mode coexistence (midpoint recycling must stay off in recovery).

## 9.3 Regression Tests
1. Existing crossed-book arb unchanged when midpoint mode disabled.
2. Existing Step 7/8 posting cadence unchanged when no relist bias active.

---

## 10. Rollout Plan

## Phase 0 - Instrumentation Only
1. Compute candidates and rejection reasons.
2. No real takes; only log/metrics.
3. Duration: 3-7 days.

Exit criteria:
1. Stable candidate rate.
2. No unexpected pending-change spikes.

## Phase 1 - Paper Decisioning (Dry-run)
1. Simulate take decisions and expected edges.
2. Compare expected edge to realized counterfactual move after N blocks.

Exit criteria:
1. Positive expected edge quality after fee assumptions.
2. Toxicity filters reduce worst outcomes.

## Phase 2 - Limited Live
1. Single pair (`XCH/wUSDC.b`), tiny caps.
2. `max_takes_per_block = 1`, low daily cap.

Exit criteria:
1. Positive realized net edge after fees over sample period.
2. Inventory remains within cap bands.
3. No churn or wallet health regressions.

## Phase 3 - Controlled Expansion
1. Increase caps gradually.
2. Add additional pairs only with reliable reference pricing.

---

## 11. Acceptance Criteria

The implementation is accepted only if all are true:
1. Net realized edge from midpoint-recycling cohort is positive after fees.
2. No statistically significant increase in stuck/pending-change incidents.
3. Inventory drawdown and concentration metrics remain within current policy limits.
4. Existing crossed-book and recovery behaviors remain intact when midpoint mode is disabled.
5. Full observability exists for candidate, reject, execute, and relist outcomes.

---

## 12. Open Review Questions Before Coding

1. Phase 1 scope: asks-only on XCH-base pairs, or symmetric bid+ask from day 1?
2. Should relist use explicit temporary markup, or rely on existing PID/anchor adjustments first?
3. Do we require CEX reference for all midpoint-recycling pairs in phase 1?
4. What is the minimum statistical sample size for go/no-go after limited live?
5. Should this live under `arbitrage` config (faster integration) or a dedicated `midpoint_recycling` top-level section (cleaner long-term architecture)?

---

## 13. Suggested Initial Conservative Defaults

1. `midpoint_recycling_enabled: false`
2. `midpoint_recycling_pairs: ["XCH/wUSDC.b"]`
3. `midpoint_recycling_band_bps: 20`
4. `midpoint_recycling_max_take_xch: 0.15`
5. `midpoint_recycling_max_takes_per_block: 1`
6. `midpoint_recycling_daily_take_xch_cap: 2.0`
7. `midpoint_recycling_min_expected_edge_bps: 15`
8. `midpoint_recycling_fee_buffer_bps: 8`
9. `midpoint_recycling_toxicity_buffer_bps: 12`
10. `midpoint_recycling_relist_markup_bps: 20`
11. `midpoint_recycling_relist_size_decay: 0.6`
12. `midpoint_recycling_cooldown_blocks: 4`
13. `midpoint_recycling_inventory_ratio_cap: 0.60`
14. `midpoint_recycling_require_cex_ref: true`
15. `midpoint_recycling_vpin_max: 0.70`

---

## 14. Summary

This plan deliberately starts from existing infrastructure (Step 9 taker flow, fee tracker, inventory/risk controls, and Step 7/8 maker machinery) to minimize integration risk. The strategy should be evaluated as a constrained liquidity-improvement module, not a midpoint-control mechanism. With instrumentation-first rollout and explicit toxicity/fee/inventory gates, this can be tested safely and iterated with measurable evidence before broader activation.
