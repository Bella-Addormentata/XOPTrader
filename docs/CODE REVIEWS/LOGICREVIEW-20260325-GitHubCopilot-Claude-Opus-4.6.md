# Logic Review — XOPTrader Trading Engine
**Date:** 2026-03-25  
**Reviewer:** GitHub Copilot (Claude Opus 4.6)  
**Scope:** End-to-end trading logic flow, formula verification, risk gate ordering  
**Methodology:** Manual trace of 13-step heartbeat cycle, formula dimensional analysis, sign verification  

---

## Executive Summary

The core trading logic is architecturally sound. The 13-step heartbeat cycle has correct dependency ordering, risk gates execute before order posting, fill processing is non-duplicating, and per-pair isolation prevents cross-contamination.

**One high-severity finding:** T1-12 (inventory units mismatch) remains `[~]` and dimensional analysis confirms the units are inconsistent — `q` (mojos) and `mid` (XCH) are passed to the same A-S formula without conversion, making the reservation price shift ~10^12× too large. This would cause all bid prices to clamp to zero and all ask prices to be dominated by the no-loss constraint, effectively disabling the strategy's inventory rebalancing.

All other formula signs, risk gate ordering, and spread composition logic are verified correct.

---

## Findings

### LR-1: T1-12 Inventory Units Mismatch — CONFIRMED HIGH
**Severity:** HIGH — Reservation price and sizing formulas produce nonsensical output  
**Files:** [engine.cpp](../../cpp/src/engine.cpp#L1144), [avellaneda.cpp](../../cpp/src/strategy/avellaneda.cpp#L364)

**Evidence chain:**

1. `net_inventory()` returns `Mojo` (int64_t) — [inventory.cpp line 255](../../cpp/src/risk/inventory.cpp#L255)
2. `get_mid_price()` returns XCH-denominated double — [market_data.cpp line 818-823](../../cpp/src/execution/market_data.cpp#L818-L823) confirms `to_mojos` multiplies by `kMojosPerXch` to convert the `mid_price` to mojos, proving `mid_price` is in XCH
3. Engine passes raw mojos to strategy — [engine.cpp line 1144](../../cpp/src/engine.cpp#L1144): `double q = static_cast<double>(inventory_->net_inventory(...))`
4. Config `q_max: 1000` documented as "base units" (not mojos) — [config.example.yaml line 39](../../config.example.yaml#L39)

**Dimensional analysis of reservation price:**
$$r = S - q \cdot \gamma \cdot \sigma^2 \cdot \tau$$

With typical values: $S = 30.0$ XCH, $q = 10^{15}$ mojos (1000 XCH), $\gamma = 0.01$, $\sigma = 0.5$, $\tau = 0.001$:
$$r = 30.0 - 10^{15} \times 0.01 \times 0.25 \times 0.001 = 30.0 - 2.5 \times 10^9$$

Result: $r \approx -2.5 \times 10^9$ → bid = max(0, r - δ) = 0. **All bids clamp to zero.**

**Sizing formula equally broken:**
$$q_{ratio} = \frac{q}{q_{max}} = \frac{10^{15}}{1000} = 10^{12}$$
$$bid\_size = q_{max} \times \max(0, 1 - 10^{12}) = 0$$

**All bid sizes are zero regardless of actual inventory.**

**Fix:** Either:
1. Convert q to base units in engine.cpp before passing to strategy:
   ```cpp
   double q = static_cast<double>(inventory_->net_inventory(...)) 
              / static_cast<double>(pair_cfg->base_mojos_per_unit);
   ```
2. Or set `q_max` in mojos in config and document accordingly.

Option 1 is strongly preferred — it aligns with the A-S academic convention where q is in base-asset units.

---

### LR-2: Spread Component Ordering — VERIFIED CORRECT
**File:** [spread.cpp](../../cpp/src/strategy/spread.cpp)

**Verified composition order:**
```
Base spread = s_adverse + s_inventory + s_cost      (additive)
Competition = min(base, best_competing - epsilon)    (cap/undercut, not widen)
Regime      = base × regime_mult × whale_mult × ...  (multiplicative)
Floor       = max(result, s_floor_bps)               (final hard floor)
Global cap  = min(result, max_half_spread_bps)        (in engine step 6)
```

**T3-33 fix verified:** Competition spread correctly undercuts (`best_competing - epsilon`), not widens.

**Floor applied AFTER all adjustments:** Correct — prevents over-tightening below profitability threshold.

---

### LR-3: Risk Gate Ordering — VERIFIED CORRECT
**File:** [engine.cpp](../../cpp/src/engine.cpp)

**Step execution order (verified):**
| Step | Function | Dependency | Verified |
|------|----------|------------|----------|
| 1 | Ingest market data | None | ✅ |
| 2 | Detect fills | Needs state | ✅ |
| 3 | Update volatility | Needs step 1 data | ✅ |
| 4 | Compute strategy quotes | Needs steps 1, 3 | ✅ |
| 5 | Apply spread optimizer | Needs step 4 | ✅ |
| 6 | Apply risk limits | Needs steps 4-5 | ✅ |
| 7 | Evaluate rebalance | Needs step 6 | ✅ |
| 8 | Post offers | **Gated by:** data_valid, flash != Crash | ✅ |
| 9 | Check arbitrage | Independent | ✅ |
| 10 | Compute NHE | Needs fill data | ✅ |
| 11 | Persist snapshots | Needs all above | ✅ |
| 12 | Heartbeat metrics | Needs all above | ✅ |
| 13 | Alert dispatch | Independent | ✅ |

**Per-pair gating confirmed:** Step 1 failure for pair X blocks steps 4-8 for pair X only. Other pairs continue.

---

### LR-4: Fill Processing — VERIFIED NO DOUBLE-COUNTING
**File:** [engine.cpp](../../cpp/src/engine.cpp) Step 2

**Verified flow:**
1. `detect_fills()` returns fills from wallet RPC (co_await, no .get())
2. Each fill: `record_buy()` or `record_sell()` updates inventory
3. `record_sell()` return value checked — rejection logged as error
4. Trade record inserted to DB with actual timestamp (not empty)
5. PnL tracker updated
6. Strategy `record_fill()` called (resets tau for A-S)

**Idempotency:** Database has UNIQUE constraint on `trade_id` — duplicate fills rejected at DB level. No double-counting path exists.

---

### LR-5: Flash Crash Gate — VERIFIED CORRECT
**File:** [engine.cpp](../../cpp/src/engine.cpp)

**State machine:**
```
Normal → Crash (any pair drops >15% from recent peak)
Crash → Recovery (50 blocks stable, deviation < 3%)
Recovery → Normal (100 blocks stable)
```

**Step 8 gating:** `if (flash_crash_state_ != FlashCrashState::Normal) { skip posting; }`

**All pairs checked:** No premature `break` — worst-case aggregation across all enabled pairs.

---

### LR-6: Inventory Skew Direction — VERIFIED CORRECT
**File:** [avellaneda.cpp](../../cpp/src/strategy/avellaneda.cpp)

**Sign convention:**
- **Long inventory (q > 0):** r < S → both bid and ask shift DOWN → sell incentivized ✅
- **Short inventory (q < 0):** r > S → both bid and ask shift UP → buy incentivized ✅

**Sizing:**
- **Long (q > 0):** bid_size = q_max × (1 - q/q_max) → shrinks bid; ask_size = q_max × (1 + q/q_max) → grows ask ✅
- Correct direction: reduces exposure on overweight side

---

### LR-7: Offer Ladder Skew Direction — VERIFIED CORRECT  
**File:** [offer_manager.cpp](../../cpp/src/execution/offer_manager.cpp)

**Verified:** `bid_size_mult = 1.0 - 0.5 * skew`, `ask_size_mult = 1.0 + 0.5 * skew`
- Positive skew (long base) → smaller bids, larger asks → correct rebalancing direction ✅

---

### LR-8: Drawdown Circuit Breaker — VERIFIED CORRECT
**File:** [engine.cpp](../../cpp/src/engine.cpp)

- HWM seeded on first cycle (not 0.0) — prevents false bypass
- 10% default max drawdown
- Engine transitions to Paused state (no further quoting)
- No profit-gate bypass (active from startup)

---

### LR-9: Tau Decay — VERIFIED CORRECT
**File:** [avellaneda.cpp](../../cpp/src/strategy/avellaneda.cpp)

**T5-CR3 fix verified:** τ uses exponential decay from τ_max after each fill:
$$\tau = \tau_{max} \times e^{-\lambda \times blocks\_since\_fill}$$

- Prevents deterministic sawtooth that adversaries could exploit
- Clamped to [τ_min, τ_max]
- First-fill protection: if no fills ever, τ = τ_max (widest spread)

---

### LR-10: Spread Optimizer Thompson Sampling — VERIFIED CORRECT
**File:** [spread.cpp](../../cpp/src/strategy/spread.cpp)

**Verified:**
- Thompson-sampled spread is used as a FLOOR, not a replacement: `max(model_spread, thompson_spread)`
- This ensures the sampled spread never undermines the model's risk floor
- Fill outcomes correctly attributed to the sampled tier index
- Hard floor (`s_floor_bps`) applied AFTER Thompson sampling — correct

---

## Cross-Verified Against Previous Reviews

| Previous Finding | Current Status | Verified |
|-----------------|----------------|----------|
| T1-10: TierQuote.size semantics | Consistently base quantity in builder | ✅ Correct direction |
| T1-12: q_max unit mismatch | **Still broken** — mojos vs base units | ❌ See LR-1 |
| T2-01: Strategy skew discarded | Reservation mid preserved through Step 6 | ✅ |
| T2-14: evaluate_rebalance asset IDs | Correct base/quote resolution | ✅ |
| T2-15: Tier ladder skew direction | Correct (bid shrinks when long) | ✅ |
| T2-16: Mark-to-market concentration | mark_to_xch via mid prices | ✅ |
| T3-07: Loss Manager wired | should_rebalance_at_loss() called | ✅ |
| T3-18: EV double-counting fixed | Carrying cost in rebalance only | ✅ |
| T3-33: Competition spread sign | Undercut (tighten), not widen | ✅ |

---

## Conclusion

The trading engine's logic is fundamentally sound with one significant exception: the T1-12 unit mismatch between mojos (inventory) and XCH (mid-price, q_max) passed to the A-S strategy renders the inventory rebalancing mechanism ineffective. This must be fixed before any live or paper trading.

All other formula signs, risk gate ordering, spread layering, fill processing, and crash protection logic are verified correct.
