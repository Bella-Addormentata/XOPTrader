# Logic Review — XOPTrader CHIA DEX Market-Making Engine

**Date:** 2026-04-02  
**Reviewer:** GitHub Copilot (Claude Opus 4.6)  
**Scope:** Full trading logic — strategy formulas, decision flows, state machines, risk gates, execution pipeline  
**Commit Branch:** `main`

---

## Executive Summary

This review examines the **mathematical correctness, decision logic, and interaction semantics** of all trading subsystems in XOPTrader. The engine implements a 13-step heartbeat loop driving Avellaneda-Stoikov and GLFT optimal market-making strategies with four-component spread optimization, variance-ratio regime detection, flash-crash state machine, and multi-tier quote laddering.

**Overall Logic Quality: A-**

The core mathematical models are correctly implemented with proper direction verification and edge-case guards. Four medium-severity logic issues were identified — three involve subtle interactions between subsystems that could produce suboptimal behavior under specific market conditions, and one involves an initialization edge case in the drawdown circuit breaker.

---

## 1. Heartbeat Loop — Step Sequencing & Dependency Logic

### Step Order Analysis

The 13-step sequence establishes a data-dependency DAG:

```
Step 1  (market state)  ──┐
Step 2  (fills)         ──┤
                          ├──> Step 3  (analytics: vol, regime, adverse selection)
                          │        │
                          │        ├──> Step 4  (compute quotes: A-S or GLFT)
                          │        │        │
                          │        │        └──> Step 5  (spread optimizer + flash crash)
                          │        │                 │
                          │        │                 └──> Step 6  (risk limits)
                          │        │                          │
                          │        │                          └──> Step 7  (ladder generation)
                          │        │                                   │
                          │        │                                   └──> Step 8  (post/cancel offers)
                          │        │
                          │        └──> Step 9  (arbitrage check — read-only)
                          │
                          └──> Step 10 (hedging — uses NHE from Step 2)
                               Step 11 (PnL update)
                               Step 12 (metrics export)
                               Step 13 (alerts)
```

**Verified:** The ordering is correct. Each step reads only from data produced by earlier steps or from persistent state (State, Database). No circular dependencies exist.

### Concern L-1 (Medium): Stale Quotes on Step 1 Failure

When Step 1 fails (market state update), the engine continues with the **previous block's** market data. Steps 4–8 then compute and post quotes based on stale prices. The per-pair `market_data_valid` flag (T3-24) gates Steps 4–8 when Step 1 returns invalid data for a pair, but only when the ticker returns `price_last <= 0`. A successful HTTP response with a stale price (e.g., exchange caching) would pass the validity check and produce quotes against outdated market data.

**Impact:** On a 52-second block time, one block of staleness is unlikely to cause significant adverse selection. However, if the DEX API consistently returns cached data during high-volatility events (e.g., a CEX-driven XCH price move), the bot would quote stale prices for multiple blocks, exposing it to informed traders who observe the CEX move first.

**Recommendation:** Add a staleness detector that compares the current mid-price against the CoinGecko CEX reference (already fetched in Step 1). If the DEX/CEX divergence exceeds a threshold (e.g., 200 bps), flag the pair as potentially stale and widen spreads defensively.

### Concern L-2 (Low): Step 2 Fill Processing Under Circuit Breaker

When the wallet circuit breaker is open, Step 2 (fill processing) is skipped entirely. Fills that occur during the outage period are not detected until the circuit breaker re-closes and `detect_fills()` is called. The reorg-protection buffer (`pending_unconfirmed_fills_`) correctly handles the confirmation delay, but the confirmation-depth counter starts from the block at which the fill is finally *detected*, not from the block at which it was *confirmed on-chain*.

This means fills from block $N$ that are detected at block $N + 50$ (after a 50-block outage) have an effective confirmation depth of 0 at detection time, even though they've been on-chain for 50 blocks, adding an unnecessary additional `confirmation_depth_blocks` delay before processing.

**Impact:** Low — fills will eventually be processed correctly, just with extra latency. The bot's inventory state will lag reality during the outage, but the no-loss constraint on the ask side prevents catastrophic selling.

**Recommendation:** Compare `fill.block_height` against `block_height` at detection time rather than adding a fresh confirmation window on top of already-deep fills.

---

## 2. Avellaneda-Stoikov Strategy Logic

### 2.1 Reservation Price

$$r = S - q \cdot \gamma \cdot \sigma^2 \cdot \tau$$

**Direction verification (from source):**

| Inventory $q$ | Shift $(r - S)$ | Effect | Correct? |
|---------------|-----------------|--------|----------|
| $q > 0$ (long) | $< 0$ (below mid) | Encourages selling | ✓ |
| $q < 0$ (short) | $> 0$ (above mid) | Encourages buying | ✓ |
| $q = 0$ (flat) | $= 0$ (at mid) | Symmetric quotes | ✓ |

**Verified:** The code computes `r_raw` then applies `r = S + skew_mult * (r_raw - S)`, which correctly scales only the inventory-driven component while preserving the mid-price anchor.

### 2.2 Optimal Half-Spread

$$\delta^* = \frac{1}{\kappa} \ln\left(1 + \frac{\kappa}{\gamma}\right) + \frac{1}{2} \gamma \sigma^2 \tau$$

**Verified:** Both terms are present and correctly ordered. The first term (market-making spread) is independent of volatility; the second term (risk compensation) grows with $\sigma^2 \tau$.

### 2.3 Exponential Tau Decay (T5-CR3)

$$\tau(t) = \tau_{\max} \cdot e^{-\lambda \cdot \Delta b}$$

where $\Delta b$ = blocks since last fill, $\lambda = -\ln(\tau_{\min}/\tau_{\max}) / H$, $H$ = horizon blocks.

**Verified:** The lambda formula ensures $\tau$ decays from $\tau_{\max}$ to $\tau_{\min}$ over $H$ blocks without fills. The `tau_min > 0` and `tau_min < tau_max` constructor checks prevent degenerate behavior.

**Edge case analysis:**
- Zero blocks since fill: $\tau = \tau_{\max}$ → widest spread. **Correct** — just-filled, maximum risk horizon.
- $H$ blocks without fill: $\tau \approx \tau_{\min}$ → tightest spread allowed. **Correct** — horizon exhausted, minimize exposure.
- Fill resets `last_fill_block_`, restarting the decay. **Correct.**

### 2.4 Regime Interaction with A-S

The regime affects A-S through two multipliers applied in sequence:

1. **Skew multiplier** (Step 3 in `compute_quotes`): scales the inventory shift $(r - S)$.
2. **Spread multiplier** (Step 5): scales the half-spread $\delta$.

| Regime | Spread Mult | Skew Mult | Net Effect |
|--------|-------------|-----------|------------|
| Mean-Reverting | 0.80 | 0.50 | Tighter spread, less aggressive rebalancing |
| Random | 1.00 | 1.00 | No change |
| Momentum | 1.50 | 2.00 | Wider spread, aggressive rebalancing |

**Verified:** The multiplicative composition is associative and order-independent. Mean-reverting markets correctly receive tighter spreads (prices bounce back, less adverse selection risk) and reduced skew (the market itself corrects inventory). Momentum markets receive wider spreads and stronger skew to avoid directional exposure.

### Concern L-3 (Low): Skew Multiplier Direction in Mean-Reverting Regime

A `skew_mult` of 0.50 in mean-reverting regime *reduces* the inventory rebalancing pressure. This is correct under the assumption that mean-reversion will naturally bring the inventory back toward flat. However, if the mean-reversion classification is a false positive (VR test has ~5–9% power at $n = 50–200$ per Richardson & Smith 1991), the engine rebalances *less aggressively* than it should in what is actually a random or trending market.

The T5-CR5 Z-gate mitigates this by requiring $|Z| > 1.96$ before classifying as mean-reverting, and the hysteresis counter adds a multi-block confirmation. Together, these make false positives rare but not impossible.

**Impact:** Low — a false positive mean-reverting classification that persists through hysteresis would under-rebalance inventory for the duration of the regime, but the hard inventory limits in Step 6 provide a backstop.

---

## 3. GLFT Strategy Logic

### 3.1 Inventory Skew

$$\text{skew} = \phi \cdot \frac{q}{q_{\max}}$$

Applied to both bid and ask:

$$\text{ask} = S + \delta - \text{skew}, \quad \text{bid} = S - \delta - \text{skew}$$

**Direction verification (from source comments, confirmed against code):**

| Inventory $q$ | Skew Sign | Quote Shift | Effect | Correct? |
|---------------|-----------|-------------|--------|----------|
| $q > 0$ (long) | $+$ | Both quotes shift DOWN | Ask cheaper → easier to sell | ✓ |
| $q < 0$ (short) | $-$ | Both quotes shift UP | Bid more generous → easier to buy | ✓ |
| $q = 0$ (flat) | $0$ | Symmetric | No inventory effect | ✓ |

**Verified:** The skew is LINEAR in $q$ (not quadratic). A historical bug (noted in GLFT header comments) incorrectly applied `skew * q` (quadratic), which was subsequently fixed.

### 3.2 Sparse-Fill Correction (T5-CR8)

$$\phi_{\text{eff}} = \phi \cdot \min\left(\max\left(1, \frac{f_{\text{dense}}}{f_{\text{actual}}}\right), \text{cap}\right)$$

**Verified:** When `f_actual` = 1 fill/hour and `f_dense` = 100 fills/hour, the correction is capped at `sparse_correction_cap` (default 10). This correctly amplifies the skew coefficient to compensate for the slower-than-modeled fill rate on CHIA.

**Edge case:** If `f_actual` approaches zero, the ratio would explode, but the cap prevents this. Both `expected_dense_fills_per_hour > 0` and `actual_fills_per_hour > 0` are enforced in the constructor, preventing division-by-zero at the source.

### 3.3 Regime Interaction with GLFT

Identical mechanism to A-S: regime multipliers applied to half-spread (Step 3 in `compute_quotes`) and skew (Step 4). The skew multiplier scales $\phi_{\text{eff}}$ before the inventory computation.

**Verified:** Correct and consistent with A-S.

---

## 4. Spread Optimizer Logic

### 4.1 Four-Component Decomposition

$$s = s_{\text{adverse}} + s_{\text{inventory}} + s_{\text{cost}} + s_{\text{competition}}$$

Each component maps to a distinct market microstructure cost (Stoll 1989):

- $s_{\text{adverse}}$: Informed-trading compensation, driven by VPIN and adverse selection estimator.
- $s_{\text{inventory}}$: Holding-risk compensation, scaled by inventory concentration.
- $s_{\text{cost}}$: Blockchain fee + opportunity cost. Uses adaptive fee estimation when available.
- $s_{\text{competition}}$: Cap applied when competing offers undercut a threshold (T3-33 FIX: changed from additive widening to undercutting `best_competing - epsilon`).

**Verified:** The four components are additive, each is non-negative, and the floor constraint `s >= s_floor_bps` ensures a minimum viable spread.

### 4.2 Thompson Sampling

**Algorithm:** Discounted Thompson Sampling (Besbes, Gur & Zeevi 2014) with Beta posteriors.

```
For each grid level i:
  Draw X_i ~ Gamma(alpha_i, 1), Y_i ~ Gamma(beta_i, 1)
  sample_i = X_i / (X_i + Y_i)    // Beta sample
Select argmax_i(sample_i)
```

**Update rule (on observation):**

$$\alpha_i \leftarrow \max(\alpha_i \cdot \gamma, 1.0) + \mathbb{1}[\text{profit}]$$
$$\beta_i \leftarrow \max(\beta_i \cdot \gamma, 1.0) + \mathbb{1}[\neg\text{profit}]$$

**Verified:**
- The discount factor $\gamma = 0.97$ gives half-life $\approx 23$ observations ($\ln 0.5 / \ln 0.97$). At CHIA's ~1 fill/hour, this is about 23 hours — appropriate for tracking daily regime shifts.
- The floor of 1.0 prevents either parameter from decaying to zero, which would create a degenerate Beta distribution.
- The `x + y > 0` guard in `sample()` handles the numerical edge case where both Gamma draws are near zero.

### 4.3 Spread-of-Spread Volatility (T5-CR15)

The `SpreadVolatilityTracker` monitors the coefficient of variation (CV) of the total spread over a rolling window. When CV exceeds a threshold, the spread is widened by up to 50% to account for spread instability.

**Verified:** This acts as a meta-uncertainty layer — when the model's own output is volatile, the engine defensively widens to avoid rapid oscillation between tight and wide quotes.

### Concern L-4 (Medium): Interaction Between Thompson Sampling and Regime Multiplier

The spread optimizer computes a base spread via the four-component model, then applies the Thompson Sampler's recommendation (which selects a grid level), then applies the regime multiplier. However, the Thompson Sampler's profitability feedback is computed *after* the regime multiplier was applied. This means:

- In mean-reverting regime (0.8x spread), the Thompson Sampler observes fills at 80% of its selected spread.
- It may attribute the profitability to the selected grid level, when in fact the profitability was driven by the regime multiplier.
- When the regime switches to Momentum (1.5x), the Sampler carries forward beliefs calibrated at 0.8x, which may over-estimate the profitability of tight spread levels.

The discounted Thompson Sampling partially mitigates this (stale beliefs decay), but the adaptation lag is ~23 observations (~23 hours at 1 fill/hr), during which the Sampler could systematically select too-tight spreads for the new regime.

**Impact:** During regime transitions, the Thompson Sampler may recommend spreads that are ~20-50% too tight for the first 12–24 hours. The VR hysteresis counter delays regime transitions by $N$ blocks, partially serializing the transition. The 40 bps spread floor provides a hard backstop.

**Recommendation:** Reset or partially decay the Thompson Sampler's posteriors when a regime transition is confirmed. This gives the Sampler a "fresh start" in the new regime rather than carrying forward stale beliefs.

---

## 5. Risk Management Logic

### 5.1 No-Loss Constraint

$$\text{ask} \geq \text{cost\_basis} \times (1 + \text{margin\_fraction})$$

Enforced at **three layers**:
1. Strategy layer (`compute_quotes` in A-S/GLFT/new strategies).
2. Pre-trade risk check (`enforce_no_loss` in `limits.cpp`).
3. Offer manager (final gate before wallet RPC).

**Verified:** Triple-redundant enforcement. Any single layer's failure is caught by the next. The margin is computed as a fraction (not absolute), ensuring correct scaling across all asset prices.

**Edge case:** When `cost_basis <= 0` (no inventory or free acquisition), the constraint is vacuously satisfied (no floor applied). This is correct — with zero cost, any positive ask price yields profit.

### 5.2 Inventory Concentration Limits

```
if concentration >= hard_limit:
    bid_size = 0                    // Stop buying entirely
elif concentration >= soft_limit:
    reduction = (concentration - soft) / (hard - soft)
    bid_size *= (1 - reduction)     // Graduated reduction
```

**Verified:** The graduated reduction provides smooth degradation between soft and hard limits. The `soft <= hard` invariant is enforced in the `PreTradeCheck` constructor.

**Direction logic:**
- High base concentration → stop buying more base (reduce bid). **Correct.**
- The symmetric logic for quote concentration would reduce asks. **Correct.**

### 5.3 Flash Crash State Machine

```
Normal ──[any_pair_crash]──> Crash
Crash  ──[all_stable_50]───> Recovery
Crash  ──[still_crashing]──> Crash (stay)
Recovery ──[all_stable_100]──> Normal
Recovery ──[any_pair_crash]──> Crash (re-entry)
```

**Verified:**
- **Crash detection** uses `peak-to-current` percentage drop across all enabled pairs (MEDIUM-5 fix: no longer breaks after first pair).
- **Gate effect:** Step 8 (offer management) is fully suppressed during Crash and Recovery states. No new offers are posted.
- **Re-entry:** A re-crash during Recovery correctly returns to Crash state (no stuck-in-recovery bug).
- **Asymmetry:** Entry to Crash requires only 1 pair to crash; exit to Normal requires ALL pairs to be stable for 100 blocks. This conservative asymmetry is appropriate.

### Concern L-5 (Medium): Drawdown Circuit Breaker at Zero Peak

The drawdown calculation:

$$\text{drawdown} = \frac{\text{peak\_equity} - \text{current\_equity}}{\text{peak\_equity}}$$

When `peak_equity` is zero or negative (bot just started with no profits), any negative `current_equity` produces a drawdown fraction that is either infinite (division by zero) or clamped to 1.0 (100% drawdown).

**Impact:** A newly started bot that takes even a small initial loss would immediately trigger the maximum drawdown circuit breaker, pausing trading before the strategy has had a chance to establish itself.

**Recommendation:** Add a grace period (e.g., first 100 blocks or first $N$ fills) during which the drawdown circuit breaker is inactive, or require a minimum peak equity (e.g., 100,000 mojos) before the breaker engages.

---

## 6. Variance Ratio Regime Detection Logic

### 6.1 VR Computation

$$VR(q) = \frac{\text{Var}(r_q)}{q \cdot \text{Var}(r_1)}$$

where $r_q$ = overlapping $q$-block log returns.

**Verified:** Implementation uses the standard overlapping-return formulation from Lo & MacKinlay (1988). Dual horizons (`vr_q_short`, `vr_q_long`) provide cross-confirmation.

### 6.2 Z-Statistic Gate (T5-CR5)

$$Z = \frac{VR(q) - 1}{\sqrt{\frac{2(2q-1)(q-1)}{3 \cdot q \cdot n}}}$$

**Classification decision tree:**

```
if |Z_short| <= z_thresh AND |Z_long| <= z_thresh:
    => Normal (insufficient statistical significance)
elif VR_short < vr_lower AND VR_long < vr_lower:
    => MeanReverting
elif VR_short > vr_upper AND VR_long > vr_upper:
    => Momentum
else:
    => Normal (disagree or mixed)
```

**Verified:** The Z-gate correctly prevents spurious regime switches at low statistical power. Both short and long VR horizons must agree for a non-Normal classification, providing cross-confirmation robustness.

### 6.3 Hysteresis

The hysteresis counter requires $N$ consecutive blocks (default 3) of the same raw signal before committing a regime transition. **Exception:** No safety-critical bypass needed for regime detection (unlike the HybridRebalance bypass in the tactic selector).

**Verified:** The three-state logic (confirmed → pending → confirmed) correctly handles:
- Oscillation near a threshold (resets the pending counter).
- Rapid signal changes (restarts the counter for the new candidate).
- Sustained signals (commits the transition after $N$ blocks).

### Concern L-6 (Low): HMM and VR Regime Coexistence

Both the VR-based classifier and the optional Hidden Markov Model (HMM) run in parallel within `RegimeDetector`. The `get_regime()` method returns only the VR-based confirmed regime; the HMM state is accessible separately via `get_hmm_state()`. This means:

- The strategies always use the VR regime for multiplier selection.
- The HMM output is informational only — it does not affect trading decisions.
- If a user enables the HMM expecting it to influence regime classification, they will observe no behavioral change.

**Impact:** Low — the HMM is documented as experimental. However, the computation cost of Baum-Welch re-fitting (every `kHmmRefitInterval` updates) is incurred regardless of whether the output is used.

**Recommendation:** Either integrate the HMM into the regime decision (e.g., as a confirming vote alongside VR) or gate the computation behind `hmm_enabled` with a clear log message that it is advisory-only.

---

## 7. Order Book Tactics Logic

### 7.1 Priority Chain

The tactic selector follows a strict priority:

| Priority | Tactic | Trigger | Effect |
|----------|--------|---------|--------|
| 1 (highest) | HybridRebalance | $|\text{inv\_ratio} - 0.5| > \text{thresh}$ | Cross spread to rebalance |
| 2 | StepBack | VPIN high / whale / crowded | Widen spread |
| 3 | AsymmetricSize | Moderate inv skew + OFI confirmation | Asymmetric sizing |
| 4 | LayerMultiple | Deep book on both sides | Multi-tier distribution |
| 5 | ImproveByOne | Deep queue + wide spread | Tighten by one tick |
| 6 (lowest) | JoinInside | Default | Join at best price |

**Verified:** The priority order correctly escalates from benign (JoinInside) to critical (HybridRebalance). Risk-driven tactics receive higher priority than profit-driven ones.

### Concern L-7 (Medium): Asymmetric Sizing OFI Direction

In `eval_asymmetric()`, the sizing decision is based on the sign of `normalized_ofi`:

```cpp
if (state.normalized_ofi > 0.0) {
    // Buying pressure -- enlarge ask (sell) to shed long inventory.
    ask_sf = ratio;
    bid_sf = inv_ratio;
} else {
    // Selling pressure -- enlarge bid (buy) to cover short inventory.
    bid_sf = ratio;
    ask_sf = inv_ratio;
}
```

However, the trigger for AsymmetricSize is `abs_imbalance > 0.1 AND abs(OFI) > threshold`. The OFI direction may not align with the inventory direction. Consider:

- Bot is **long** (`inv_ratio = 0.7`, imbalance = 0.2 > 0.1).
- OFI is **negative** (selling pressure) → the code enlarges bid side (buys more).
- This **increases** the already-excessive long inventory instead of reducing it.

The commented intent says "OFI confirms the directional skew," but the logic doesn't verify that OFI and inventory are directionally aligned. The trigger's `abs(OFI) > threshold` test confirms OFI *magnitude* but not *direction relative to inventory*.

**Impact:** In the scenario above, the bot would increase position size on the wrong side, growing inventory concentration toward the hard limit rather than reducing it. The hard limit backstop in Step 6 would eventually cut off the bid side entirely, but the damage (larger position entered at potentially adverse prices) is already done.

**Recommendation:** Add a directional check: only fire AsymmetricSize when OFI direction agrees with the desired rebalancing direction. Specifically:
- Long inventory + positive OFI → enlarge ask (correct: OFI confirms buying pressure, shed longs).
- Long inventory + negative OFI → do NOT fire AsymmetricSize (OFI disagrees; fall through to JoinInside or LayerMultiple).
- Short inventory + negative OFI → enlarge bid (correct: OFI confirms selling pressure, cover shorts).
- Short inventory + positive OFI → do NOT fire AsymmetricSize.

### 7.2 Hysteresis Override for HybridRebalance

HybridRebalance bypasses the tactic hysteresis counter entirely, executing immediately when the inventory threshold is breached. This is correct: delaying a critical risk-management action by $N$ blocks could allow inventory to grow further into danger territory.

**Verified:** The bypass logic is clean and the rationale is well-documented in the source.

---

## 8. Fill Processing & Inventory Accounting

### 8.1 Weighted-Average Cost Basis

$$\text{avg\_cost} = \frac{\text{total\_cost}_{\text{old}} + \text{fill\_price} \times \text{fill\_qty}}{\text{total\_qty}_{\text{old}} + \text{fill\_qty}}$$

**Verified:** Uses `__int128` (or `double` on MSVC) for the intermediate product to prevent overflow. The cost basis update is atomic — total_cost and total_quantity are always updated together under the same lock.

### 8.2 Reorg Protection

Fills detected by `detect_fills()` are buffered in `pending_unconfirmed_fills_` and promoted to processing only when `block_height - fill.block_height >= confirmation_depth_blocks`.

**Verified:** This correctly prevents the bot from acting on fills that could be rolled back by a chain reorganization.

**Edge case:** If `confirmation_depth_blocks = 0`, fills are processed immediately (no reorg protection). This is an explicit opt-in for fast processing on trusted chains. **Correct.**

### Concern L-8 (Low): Fill State Divergence

As noted in the code review (E-1), if `inventory_->record_sell()` rejects a confirmed fill (e.g., selling more than the recorded inventory), the fill is persisted to the database but not reflected in the inventory tracker. The fill processing loop catches per-fill exceptions and `continue`s, so subsequent fills in the same block are still processed.

The concern is that a rejected sell creates a permanent divergence between the database (which records the fill) and the inventory tracker (which does not). On engine restart, the database replay would attempt to record the fill again, hitting the same rejection.

**Impact:** Low in practice — the reject condition (selling more than owned) should not occur under normal operation because the no-loss constraint and bid-size limiting prevent entering a short position. However, an extremely rapid sequence of fills in the same block could theoretically cause this if two offers are filled simultaneously.

---

## 9. Backtest Engine Logic

### 9.1 Fill Simulation Model

```
For each historical trade:
    if trade.side == Ask AND our_bid >= trade.price:
        filled (we bought from the historical seller)
    if trade.side == Bid AND our_ask <= trade.price:
        filled (we sold to the historical buyer)
```

**Verified:** The zero-market-impact assumption is realistic for CHIA's ~$2K/day DEX volume. The conservative fill model (we only fill when our price is at or better than the historical trade) avoids look-ahead bias.

### 9.2 Monte Carlo Price Paths

$$S_{t+1} = S_t \cdot \exp\left(\frac{\sigma_{\text{annual}}}{\sqrt{B}} \cdot W_t\right), \quad W_t \sim N(0, 1)$$

where $B$ = blocks per year.

**Verified:**
- Zero drift (conservative — no free alpha assumed). **Correct.**
- Deterministic sub-seeding: `master_seed XOR path_index`. **Correct** — reproducible across runs.
- GBM preserves price positivity ($S > 0$ always). **Correct.**

### 9.3 Walk-Forward Validation

**Verified:** The dual-profitability requirement (profitable in BOTH training and test windows) with rolling advances provides strong overfitting protection. The acceptance rate metric correctly quantifies robustness — lower acceptance rates indicate more robust parameter sets.

### Concern L-9 (Low): CSV Header Detection

```cpp
if (!line.empty() && !std::isdigit(static_cast<unsigned char>(line[0]))) {
    // Header line -- skip.
}
```

This fails for:
- Lines starting with a negative number (`-3.14,...`): treated as header.
- Lines with leading whitespace (` 123,...`): treated as header.
- Header lines starting with a digit (`1st_column,...`): treated as data.

**Impact:** Low — the CEX CSV format is typically controlled by the user and follows standard candle conventions (timestamp as first field, always positive). But the code should handle the general case.

---

## 10. Whale Detection & Quote Adjustment Logic

### 10.1 Classification

A trade is classified as "whale" if:

$$\text{size} \geq \text{whale\_threshold} \quad \text{OR} \quad \text{size} \geq 0.05 \times V_{24h}$$

**Verified:** The dual criteria correctly identify whales in both liquid (absolute-size threshold) and illiquid (volume-fraction threshold) markets.

### 10.2 Spread Widening

$$\text{spread\_mult} = 1 + (\text{max\_mult} - 1) \times \frac{|\text{whale\_events}|}{\text{window\_blocks}}$$

**Verified:** Linear interpolation from 1x (no whales) to `max_mult` (fully whale-saturated window). The sliding window evicts events older than `whale_window_blocks`.

### 10.3 Asymmetric Skew

When a whale trades on one side, the opposite side is tightened:

```
if whale_side == Bid:
    ask_spread_mult *= 1.0           // Normal ask
    bid_spread_mult *= (1 + skew)    // Widen bid (protect against buyer)
```

**Verified:** This correctly diverts flow away from the informed direction. A whale buying aggressively suggests upward price pressure; widening the bid protects the bot from selling too cheaply to the whale, while keeping the ask tight to capture the upward move.

---

## 11. Competitor Detection Logic

### 11.1 Self-Exclusion

The engine collects its own offer IDs from `state_->get_all_offers()` and passes them as an exclusion set to `ingest_competing_offers()`. This prevents the bot from treating its own offers as competitor activity.

**Verified:** Correct. Without this, the bot's own offers would trigger competitive responses (tightening spreads), creating a self-reinforcing feedback loop that drives spreads to the floor.

### 11.2 Competition Spread Component

The T3-33 fix changed the competition component from additive widening to undercutting:

```
s_competition = best_competing_spread - epsilon
```

**Verified:** This correctly positions quotes just inside the best competitor rather than widening defensively. The spread floor (`s_floor_bps`) prevents the undercutting from reaching uneconomic levels.

---

## 12. PnL Attribution Logic

### 12.1 Three-Component Attribution

$$\text{Total PnL} = \text{Spread PnL} + \text{Inventory PnL} + \text{Fee PnL}$$

- **Spread PnL** (realized): $(P_{\text{sell}} - P_{\text{cost\_basis}}) \times Q$
- **Inventory PnL** (unrealized): $Q_{\text{held}} \times (P_{\text{mid}} - P_{\text{cost\_basis}})$
- **Fee PnL**: Sum of blockchain fees (always negative)

**Verified:** The unit normalization (dividing by `kMojosPerXch`) correctly converts from mojos-squared to mojos for the realized PnL calculation. The comment at T4-13 explicitly documents this unit conversion.

### Concern L-10 (Low): Unrealized PnL Volatility

Inventory PnL is recomputed every heartbeat using the current mid-price. On CHIA's sparse DEX with wide spreads, the mid-price can oscillate significantly between blocks, causing large swings in reported PnL even without any trades. This could trigger:
- False drawdown alerts.
- Oscillating equity that crosses the drawdown threshold repeatedly.

**Impact:** Low — the realized PnL (actual fills) is stable, and the drawdown circuit breaker uses total PnL (realized + unrealized). The unrealized component adds noise but doesn't create false realized losses.

---

## 13. Signal Interaction Map

The following table shows how each microstructure signal flows through to quoting decisions:

| Signal | Source | Consumer | Effect on Quotes |
|--------|--------|----------|------------------|
| Yang-Zhang $\sigma$ | `VolatilityEstimator` | A-S/GLFT `compute_quotes` | Higher $\sigma$ → wider spread, stronger skew |
| Variance Ratio | `RegimeDetector` | A-S/GLFT via `regime_.spread_mult` | MR → tighten, Momentum → widen |
| VPIN | `MarketDataFeed` | `OrderBookTactician::StepBack` | High VPIN → widen via tactic adjustment |
| OFI | `MarketDataFeed` | `OrderBookTactician::AsymmetricSize` | Directional → asymmetric sizing |
| Whale Events | `MarketDataFeed` | Spread widening multiplier | Active whale → up to 3x wider |
| Competitor Depth | `MarketDataFeed` | Spread optimizer ($s_{\text{competition}}$) | Undercut best competitor |
| Flash Crash | `PreTradeCheck` | Engine state machine | Gate Step 8 entirely |
| Thompson Sampling | `SpreadOptimizer` | Grid level selection | Explore/exploit spread levels |
| Adverse Selection | `AdverseSelectionEstimator` | Spread optimizer ($s_{\text{adverse}}$) | Higher toxicity → wider spread |
| Block Cadence | `BlockCadenceAdaptiveSpread` | Spread multiplier | High variance → wider spread |
| Coin Age | `CoinAgeWeightedQuoting` | Ask pricing | Older coins → more urgency to sell |
| Mempool Sentinel | `MempoolSentinelStrategy` | Quote adjustment | Pending large orders → preemptive widening |

**No circular dependencies detected.** Each signal flows unidirectionally from estimation to quoting.

---

## Summary of Findings

| ID | Severity | Title | Section |
|----|----------|-------|---------|
| L-1 | Medium | Stale quotes on DEX API cache hits bypass validity check | §1 |
| L-4 | Medium | Thompson Sampling beliefs stale across regime transitions | §4.3 |
| L-5 | Medium | Drawdown circuit breaker fires at zero peak equity | §5.3 |
| L-7 | Medium | Asymmetric tactic OFI direction may oppose inventory direction | §7.1 |
| L-2 | Low | Fill confirmation delay unnecessarily extended after outage | §1 |
| L-3 | Low | False positive mean-reverting regime under-rebalances inventory | §2.4 |
| L-6 | Low | HMM computation cost incurred without affecting decisions | §6.3 |
| L-8 | Low | Fill rejection creates permanent DB/inventory divergence | §8.2 |
| L-9 | Low | CSV header detection fails on negative numbers | §9.3 |
| L-10 | Low | Unrealized PnL oscillation from sparse mid-price data | §12 |

---

## Recommendations (Priority Order)

1. **Fix Asymmetric Tactic OFI direction check** (L-7) — add a directional alignment test before firing AsymmetricSize.
2. **Add drawdown grace period** (L-5) — require minimum peak equity or initial $N$ fills before the circuit breaker engages.
3. **Decay Thompson Sampler on regime transitions** (L-4) — partially reset posteriors when the VR confirms a regime change.
4. **Add DEX/CEX divergence staleness detector** (L-1) — widen spreads when the DEX mid materially diverges from the CoinGecko reference.
5. **Use on-chain depth for fill confirmation** (L-2) — compare fill block height against current block at detection time to avoid double-delaying already-confirmed fills.
6. **Gate HMM computation behind usage** (L-6) — skip Baum-Welch re-fitting unless the HMM output feeds into the regime decision.
