# XOPTrader Strategy Priority Ranking

> Issue #9: Rigorous prioritization of all strategies -- existing (12 implemented,
> 5 considered) + future (15 planned) + 3 CHIA-native novel strategies -- by
> ROI-per-effort, calibrated to CHIA DEX at ~$2K/day volume.
>
> Generated 2026-03-24 | Reviewed against engine.hpp wiring, profitability model,
> loss_manager.hpp, drift_analyzer.hpp, and new_strategies.hpp.

---

## Table of Contents

1. [Methodology](#methodology)
2. [Complete Strategy Inventory](#complete-strategy-inventory)
3. [Tier 1: Critical -- Implement First](#tier-1-critical----implement-first)
4. [Tier 2: High Priority -- Next Sprint](#tier-2-high-priority----next-sprint)
5. [Tier 3: Medium Priority -- Novel CHIA-Native Strategies](#tier-3-medium-priority----novel-chia-native-strategies)
6. [Tier 4: Defer](#tier-4-defer)
7. [Cumulative Edge Analysis](#cumulative-edge-analysis)
8. [Dependency Graph](#dependency-graph)
9. [Implementation Roadmap -- Critical Path](#implementation-roadmap----critical-path)
10. [The Loss-Taking Question](#the-loss-taking-question)
11. [The Drift Question](#the-drift-question)
12. [CHIA-Specific Advantages and Strategy Mapping](#chia-specific-advantages-and-strategy-mapping)
13. [Honesty Check -- Self-Reflection](#honesty-check----self-reflection)

---

## Methodology

Each strategy is scored on five axes, then ranked by composite score:

| Axis | Weight | Scale | Rationale |
|------|--------|-------|-----------|
| **Marginal PnL** (edge in bps at current volume) | 30% | 0-100+ bps | Revenue and loss-prevention impact |
| **Implementation effort** (days of work) | 25% | Lower is better | Opportunity cost of developer time |
| **Risk of negative impact** (could it lose money?) | 20% | 0=safe, 5=dangerous | Never-sell-at-loss constraint amplifies this |
| **Dependency chain** (what must exist first?) | 15% | 0=standalone, 3=deep chain | Determines sequencing, not value |
| **Capital requirement** (extra capital needed?) | 10% | 0=none, 3=significant | Scarce resource at $10K portfolio |

**Key reality check**: At $2K/day DEX volume, the entire daily revenue opportunity
is approximately $10-60 (at 50-300 bps capture rate) from spread income, plus
$100-300/day from CEX-DEX arbitrage if execution works. Any strategy that costs more
than 2-3 days to implement must justify itself by either (a) protecting existing PnL,
(b) being required infrastructure for other strategies, or (c) preparing for volume
growth (June 2026 hard fork catalyst).

**Core constraint**: NEVER SELL AT A LOSS is hardcoded. All rankings respect this.
Strategic loss-taking is evaluated separately in the [Loss-Taking Question](#the-loss-taking-question) section.

---

## Complete Strategy Inventory

All 35 strategies across the codebase, sorted by tier and implementation priority.

| # | Strategy | Source | Status | Tier | Edge (bps) | Effort (days) | Complexity |
|---|----------|--------|--------|------|-----------|---------------|------------|
| 1 | Lead-Lag CEX Signals | 3.3 | Unimplemented | 1 | 50-150 | 2-3 | Low |
| 2 | Oracle Fair-Value Anchoring | 3.11 | Unimplemented | 1 | 30-80 | 1-2 | Low |
| 3 | GARCH Volatility Forecast | 3.2 | Unimplemented | 1 | 20-50 | 2-3 | Medium |
| 4 | A-S Tuning | 1.1 | Tune | 1 | 10-30 | 1-2 | Low |
| 5 | GLFT Tuning | 1.2 | Tune | 1 | 10-30 | 1-2 | Low |
| 6 | Maker-Taker Fee Optimization | 3.4 | Unimplemented | 1 | 5-15 | 0.5-1 | Low |
| 7 | Thompson Sampling Tuning | 1.12 | Tune | 1 | 10-25 | 1 | Low |
| 8 | Latency Arb Defense | 3.7 | Unimplemented | 1 | 30-100 | 2-3 | Medium |
| 9 | Four-Component Spread (core) | 1.3 | Implemented | 1 | -- | -- | -- |
| 10 | Multi-Tier Liquidity (core) | 1.4 | Implemented | 1 | -- | -- | -- |
| 11 | Asymmetric Spread Widening (core) | 1.11 | Implemented | 1 | -- | -- | -- |
| 12 | Regime Detection Tuning | 1.5 | Tune | 2 | 10-20 | 2-3 | Medium |
| 13 | Whale Detection Tuning | 1.8 | Tune | 2 | 10-20 | 1 | Low |
| 14 | VPIN Tuning | 1.9 | Tune | 2 | 5-15 | 1 | Low |
| 15 | OFI Tuning | 1.10 | Tune | 2 | 5-15 | 1 | Low |
| 16 | Competitor Detection Tuning | 1.7 | Tune | 2 | 5-10 | 0.5 | Low |
| 17 | Cross-Platform Arb Tuning | 1.6 | Tune | 2 | 50-200 | 2-3 | Medium |
| 18 | VaR/CVaR Position Limits | 3.5 | Unimplemented | 2 | 10-20 | 3-4 | Medium |
| 19 | Cartea-Jaimungal Alpha Signal | 2.3 | Considered | 2 | 15-40 | 3-5 | Medium |
| 20 | Kyle Lambda / Price Impact | 2.4 | Considered | 2 | 10-20 | 2-3 | Medium |
| 21 | IL Hedging for AMM LP | 3.13 | Unimplemented | 2 | 15-40 | 5-7 | High |
| 22 | **Coin-Age-Weighted Quoting** | NEW | **New** | **3** | **5-15** | **2-3** | **Low** |
| 23 | **Block-Cadence Adaptive Spread** | NEW | **New** | **3** | **3-8** | **1-2** | **Low** |
| 24 | **Mempool Sentinel Strategy** | NEW | **New** | **3** | **10-25** | **3-5** | **Medium** |
| 25 | CEX Hedging | 3.1 | Unimplemented | 4 | 20-50 | 7-10 | High |
| 26 | Bayesian PIN | 2.1 | Considered | 4 | 5-15 | 3-5 | Medium |
| 27 | Predatory Trading Defense | 2.2 | Considered | 4 | 5-15 | 4-6 | Medium |
| 28 | SOC Guilbaud-Pham | 2.5 | Considered | 4 | 10-25 | 10-15 | High |
| 29 | Multi-Asset Joint Quoting | 3.8 | Unimplemented | 4 | 10-20 | 7-10 | High |
| 30 | Toxic Flow ML Classification | 3.6 | Unimplemented | 4 | 10-25 | 10-14 | High |
| 31 | Cross-Chain Bridge Arb | 3.10 | Unimplemented | 4 | 20-80 | 7-10 | High |
| 32 | Spoofing Detection | 3.12 | Unimplemented | 4 | 0-5 | 4-6 | Medium |
| 33 | Microstructure Invariance | 3.15 | Unimplemented | 4 | 3-8 | 5-7 | Medium |
| 34 | Batch Auction Strategies | 3.14 | Unimplemented | 4 | 0 | 10-15 | High |
| 35 | RL Market Making | 3.9 | Unimplemented | 4 | Unknown | 30-60 | Very High |

---

## Tier 1: Critical -- Implement First

Core A-S/GLFT, spread optimizer, basic risk, and high-ROI unimplemented items.
These strategies either form the quoting backbone or have the highest edge-per-effort
ratio at current volume levels.

### 1. Lead-Lag Signals from CEX Price Feeds (3.3)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Unimplemented |
| **Effort** | 2-3 days |
| **Estimated edge** | 50-150 bps per adverse-selection-avoided cycle |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | MarketDataFeed (exists), CEX websocket client (partially exists via OKX feed) |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Quoting stale prices; most common and expensive failure mode on thin DEX |

**Justification**: This is the single highest-ROI unimplemented strategy. CEX
trades $2.4M/day vs DEX $2K/day -- CEX price moves lead DEX by minutes to hours.
Simply shifting the reference mid-price to the CEX TWAP prevents the most common
and most expensive failure mode: quoting stale prices that get picked off by
informed takers. The MarketDataFeed already ingests CEX data for arbitrage
detection; this just uses it as the primary fair-value anchor instead of the thin
DEX book. At current spreads of 300-1000 bps on DEX, avoiding even one stale-quote
snipe per day pays for itself.

---

### 2. Oracle-Based Fair-Value Anchoring (3.11)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Unimplemented |
| **Effort** | 1-2 days |
| **Estimated edge** | 30-80 bps (adverse selection reduction) |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | MarketDataFeed (exists) |
| **Capital requirement** | None |
| **Risk if NOT implemented** | All other strategies compute around a manipulable DEX mid |

**Justification**: Overlaps with Lead-Lag (3.3) and should be implemented together
as a single "CEX-anchored fair value" module. On a DEX with $2K/day volume, the
on-chain order book is trivially manipulable. Anchoring mid-price to aggregated
CEX TWAP is not optional -- it is a prerequisite for every other strategy to
function correctly. Without it, A-S and GLFT compute quotes around a manipulated
mid-price. This is 1-2 days of work because the CEX feed already exists; the change
is making it authoritative.

---

### 3. Volatility Forecasting -- GARCH (3.2)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Unimplemented |
| **Effort** | 2-3 days |
| **Estimated edge** | 20-50 bps (tighter spreads in calm, wider in storm) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | VolatilityEstimator (exists -- Yang-Zhang), CEX price feed |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Spreads lag reality; too tight before vol arrives, too wide after it passes |

**Justification**: The existing Yang-Zhang estimator is backward-looking. A
GARCH(1,1) on CEX block-level returns adds forward-looking volatility that
directly feeds into every spread computation. On a market with 3-10% spreads,
switching from trailing to predictive volatility lets us tighten 20-30% during
calm periods (capturing more fills) and widen pre-emptively before realized vol
confirms (avoiding adverse selection). GARCH(1,1) is approximately 50 lines of code
on top of the existing vol infrastructure. The CEX return series has 1000x more data
points than the DEX, making the GARCH fit robust.

---

### 4. Avellaneda-Stoikov Tuning (1.1) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Implemented -- needs calibration |
| **Effort** | 1-2 days (parameter calibration, not code) |
| **Estimated edge** | 10-30 bps (from better kappa/gamma calibration) |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | Backtesting framework, live fill data |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Textbook defaults misfit CHIA's unique fill dynamics |

**Justification**: The A-S implementation exists but the fill-rate parameter
kappa and risk-aversion gamma are likely initialized from textbook defaults, not
Chia-specific data. At $2K/day volume with ~52-second blocks, the fill intensity
curve is radically different from traditional equities. A systematic parameter
sweep using GPU backtesting can meaningfully improve quote placement. This is
calibration work, not new code.

---

### 5. GLFT Tuning (1.2) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Implemented -- needs calibration |
| **Effort** | 1-2 days (alongside A-S) |
| **Estimated edge** | 10-30 bps |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | Same as A-S tuning |
| **Capital requirement** | None |
| **Risk if NOT implemented** | GLFT (preferred for 24/7 markets) operates with uncalibrated phi |

**Justification**: GLFT's phi and gamma need Chia-specific calibration. Since
GLFT is the preferred model for 24/7 markets (no session end, time-invariant
reversion force per drift_analyzer.hpp analysis), getting its parameters right is
arguably more important than A-S tuning. Run both sweeps together.

**Drift connection**: drift_analyzer.hpp shows GLFT produces steady-state variance
Var[q] = q_max / (2 * phi * kappa). Getting phi right directly controls inventory
drift bounds.

---

### 6. Maker-Taker Fee Optimization (3.4)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Unimplemented |
| **Effort** | 0.5-1 day |
| **Estimated edge** | 5-15 bps (routing savings) |
| **Risk of negative impact** | None (0/5) |
| **Dependencies** | Multi-venue support (Dexie client exists) |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Paying 70-100 bps in unnecessary fees per non-Dexie trade |

**Justification**: Dexie charges 0% for native offers. TibetSwap charges 0.7%.
Hashgreen charges 0.9%. Simply routing passive orders to Dexie and using TibetSwap
only for rebalancing or AMM LP saves 70-100 bps per TibetSwap interaction. This is
a routing table, not an algorithm. Half a day of work for a permanent fee reduction
on every trade that touches non-Dexie venues.

---

### 7. Thompson Sampling Tuning (1.12) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Implemented -- needs grid adjustment |
| **Effort** | 1 day |
| **Estimated edge** | 10-25 bps (from grid refinement and prior tuning) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | Sufficient fill history |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Bandit explores wrong spread range; convergence delayed by weeks |

**Justification**: Thompson Sampling is implemented but may have a too-coarse
spread grid or inappropriate priors for Chia's wide-spread environment. The
default grid likely covers 10-100 bps, but optimal Chia spreads may be 100-500
bps given the thin book. Adjusting the grid range and bucket count to match
reality, plus warming up the Beta priors with simulated data, takes a day and
meaningfully improves convergence speed.

---

### 8. Latency Arbitrage Defense (3.7)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 -- Critical |
| **Status** | Unimplemented |
| **Effort** | 2-3 days |
| **Estimated edge** | 30-100 bps (loss prevention, not revenue generation) |
| **Risk of negative impact** | Low (1/5) -- may over-cancel in volatile markets |
| **Dependencies** | Lead-Lag / CEX feed (Strategy #1 above) |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Stale-quote sniping becomes primary PnL leak as spreads tighten |

**Justification**: Once CEX-anchored fair value is live (Strategy #1), this
becomes a natural extension: if CEX mid moves >X bps since our last quote refresh,
immediately cancel outstanding offers before they get sniped. On Chia's 52-second
blocks, a 5% CEX price move gives an arbitrageur a full block to take our stale
offer. This is the defensive complement to Lead-Lag's offensive improvement.
Build this now so it is ready when spreads tighten.

---

### Already-Implemented Core Pipeline (No Separate Tuning Entry)

| # | Strategy | Status | Notes |
|---|----------|--------|-------|
| 9 | Four-Component Spread (1.3) | Implemented | Core pipeline; tuned via components above |
| 10 | Multi-Tier Liquidity (1.4) | Implemented | Core pipeline; tuned via Thompson Sampling |
| 11 | Asymmetric Spread Widening (1.11) | Implemented | Core pipeline; tuned via whale detection |

---

**Tier 1 Summary**:

| Metric | Value |
|--------|-------|
| Strategies | 11 (8 actionable + 3 core pipeline) |
| Total effort | 11-18 days |
| Combined edge (new + tuning) | 175-480 bps (not additive; see cumulative analysis below) |
| Primary risk mitigated | Stale-quote sniping, uncalibrated parameters, fee leakage |

---

## Tier 2: High Priority -- Next Sprint

Regime detection, whale response, inventory management, arbitrage tuning.
These strategies have clear value but depend on Tier 1 items or require more
data to calibrate.

### 12. Regime Detection Tuning (1.5) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Implemented -- needs calibration |
| **Effort** | 2-3 days (window tuning, HMM re-calibration) |
| **Estimated edge** | 10-20 bps (better spread adaptation) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | GARCH vol (Tier 1 #3) improves regime accuracy |
| **Risk if NOT implemented** | Regime mis-classification causes wrong spread/skew for hours |

---

### 13. Whale Detection Tuning (1.8) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Implemented -- needs threshold calibration |
| **Effort** | 1 day |
| **Estimated edge** | 10-20 bps |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | None |
| **Risk if NOT implemented** | Whale trades hit us at normal spreads; adverse selection loss |

**Justification**: The 50 XCH / 5% volume thresholds are defaults. At $2K/day
DEX volume (~740 XCH), a 50 XCH trade is 6.8% of daily volume -- possibly too
high a threshold. Lowering to 20-30 XCH may catch more informed flow.

---

### 14. VPIN Tuning (1.9) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Implemented -- needs bucket size adjustment |
| **Effort** | 1 day |
| **Estimated edge** | 5-15 bps |
| **Dependencies** | None |
| **Risk if NOT implemented** | VPIN signal too slow at current bucket size for thin market |

---

### 15. OFI Tuning (1.10) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Implemented -- needs normalization adjustment |
| **Effort** | 1 day |
| **Estimated edge** | 5-15 bps |
| **Dependencies** | None |
| **Risk if NOT implemented** | OFI saturates on single large orders, producing noisy signals |

---

### 16. Competitor Detection Tuning (1.7) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Implemented -- needs verification |
| **Effort** | 0.5 day |
| **Estimated edge** | 5-10 bps |
| **Dependencies** | None |
| **Risk if NOT implemented** | Miss competitor entry; quote into void or get outcompeted unknowingly |

---

### 17. Cross-Platform Arbitrage Tuning (1.6) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Implemented -- needs live calibration |
| **Effort** | 2-3 days |
| **Estimated edge** | 50-200 bps per arb cycle |
| **Dependencies** | CEX API integration, Dexie/TibetSwap adapters |
| **Capital requirement** | Moderate -- needs capital on both CEX and DEX sides |
| **Risk if NOT implemented** | Leaving the largest single revenue source ($240/day potential) on the table |

**Justification**: CEX-DEX arb at 50-200 bps per cycle is the largest revenue
source. At $2.4M CEX daily volume, even capturing 0.01% of volume as arb profit
is $240/day -- more than the entire DEX spread income. Focus on arb where DEX price
is persistently stale (slow mean-reversion), not on fleeting CEX microstructure.

---

### 18. Dynamic Position Limits -- VaR/CVaR (3.5)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Unimplemented |
| **Effort** | 3-4 days |
| **Estimated edge** | 10-20 bps (risk reduction, not direct PnL) |
| **Dependencies** | GARCH volatility forecast (Tier 1 #3) |
| **Risk if NOT implemented** | Static q_max carries 2-3x more risk in high-vol regimes |

---

### 19. Cartea-Jaimungal Alpha Signal (2.3)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Considered |
| **Effort** | 3-5 days |
| **Estimated edge** | 15-40 bps (improved quote asymmetry) |
| **Dependencies** | OFI (implemented), VPIN (implemented), Lead-Lag (Tier 1 #1) |
| **Risk if NOT implemented** | OFI and VPIN remain post-hoc multipliers instead of integrated alpha |

---

### 20. Kyle Lambda / Permanent Price Impact (2.4)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Considered |
| **Effort** | 2-3 days |
| **Estimated edge** | 10-20 bps (better sizing for large trades) |
| **Dependencies** | Trade database (exists), CEX data for regression |
| **Risk if NOT implemented** | Offer sizes mismatched to permanent impact; take wrong side of large flow |

---

### 21. Impermanent Loss Hedging for AMM LP (3.13)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 -- High Priority |
| **Status** | Unimplemented |
| **Effort** | 5-7 days |
| **Estimated edge** | 15-40 bps on AMM capital (IL reduction) |
| **Dependencies** | TibetSwap LP integration |
| **Capital requirement** | Moderate -- needs capital in AMM pools |
| **Risk if NOT implemented** | Never-sell-at-loss constraint traps underwater AMM positions indefinitely |

---

**Tier 2 Summary**:

| Metric | Value |
|--------|-------|
| Strategies | 10 |
| Total effort | 21-34 days |
| Combined edge | 140-400 bps (with significant overlap) |
| Primary risk mitigated | Uncalibrated signals, missed arb revenue, static risk limits |

---

## Tier 3: Medium Priority -- Novel CHIA-Native Strategies

Three new strategies designed in `new_strategies.hpp` that exploit CHIA's unique
blockchain properties. These are placed in Tier 3 not because they lack value but
because they depend on Tier 1 and Tier 2 infrastructure being calibrated first.

### 22. Coin-Age-Weighted Quoting (NEW)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 -- Medium Priority |
| **Status** | New -- header designed in `new_strategies.hpp` |
| **Effort** | 2-3 days |
| **Estimated edge** | 5-15 bps from faster inventory turnover |
| **Implementation complexity** | Low |
| **Risk of negative impact** | Very low (0/5) -- tightens ask, widens bid; never violates no-loss floor |
| **Dependencies** | CoinManager (execution layer), cost-basis tracking |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Old inventory accumulates; capital locked in stale coins; bid/ask imbalance |

**What it does**: Exploits CHIA's coin-set (UTXO) model to track the exact age of
each inventory coin. Older coins have higher opportunity cost (locked capital) and
higher probability of stale cost basis. The strategy:

- Computes age-weighted urgency U = (1/n) * SUM[1 - exp(-lambda * age_i)]
- Tightens ask spread for old coins: ask_mult = (1 - alpha * U)
- Widens bid spread when inventory is old: bid_mult = (1 + beta * U)

**CHIA-specific advantage**: Only possible on coin-set (UTXO) chains where each
inventory unit has a deterministic creation block. Account-model chains (Ethereum)
cannot distinguish old from new balance units. On CHIA's thin market (~$2K/day),
inventory can sit for hours or days -- making coin age a material signal.

**Mathematical basis**: Amihud & Mendelson (1986) establish that holding costs
increase effective spread; Easley & O'Hara (1987) show that trade size and time
affect information content. Lambda_age default of 1/3600 means coins reach 63%
urgency after 1 hour, matching CHIA's 30-120 minute average fill time.

**Integration**: Plugs into the spread multiplier pipeline as (ask_mult, bid_mult)
alongside VPIN and OFI multipliers. Respects cost-basis floor via set_cost_basis().

---

### 23. Block-Cadence Adaptive Spread (NEW)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 -- Medium Priority |
| **Status** | New -- header designed in `new_strategies.hpp` |
| **Effort** | 1-2 days |
| **Estimated edge** | 3-8 bps from avoiding stale-quote sniping during fast blocks |
| **Implementation complexity** | Low |
| **Risk of negative impact** | Very low (0/5) -- only widens, never tightens |
| **Dependencies** | FullNode RPC (block timestamps) |
| **Capital requirement** | None |
| **Risk if NOT implemented** | Quotes stale during fast-block runs; overly conservative during slow blocks |

**What it does**: Exploits the variance in CHIA's 52-second block time (actual
inter-block intervals range from ~10s to ~200s+). The spread multiplier follows a
U-shaped curve centered on the target cadence:

- m_cadence = 1 + eta * (R - 1)^2, where R = dt_ema / dt_target
- Fast blocks (R < 1): widen because price discovery accelerates
- Slow blocks (R > 1): widen because accumulated uncertainty is higher
- Normal cadence (R = 1): no adjustment

Additionally adjusts the fill-intensity kappa:
- kappa_adjusted = kappa_base * (dt_target / dt_ema)

**CHIA-specific advantage**: CHIA's block-time variance is MUCH larger than
Ethereum (~12s +/- 1s) or Solana (~0.4s +/- 0.05s). The difference between a
10-second and a 200-second inter-block interval is a 20x range -- material for
spread sizing. No other chain has this degree of block-time variance combined with
a transparent offer model.

**Mathematical basis**: Ait-Sahalia & Yu (2009) establish that sampling frequency
affects volatility estimation and optimal quoting. Robert & Rosenbaum (2011) model
irregular time spacing in transaction data.

**Integration**: Outputs a spread multiplier and adjusted kappa that feed into the
spread optimizer and A-S/GLFT models. Plugs into the pipeline:
final_spread *= m_cadence.

---

### 24. Mempool Sentinel Strategy (NEW)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 -- Medium Priority (highest value of the three novel strategies) |
| **Status** | New -- header designed in `new_strategies.hpp` |
| **Effort** | 3-5 days |
| **Estimated edge** | 10-25 bps from anticipatory quote adjustment; potentially more during whale events |
| **Implementation complexity** | Medium (requires mempool RPC integration and spend-bundle parsing) |
| **Risk of negative impact** | Low (1/5) -- only adjusts future quotes, never front-runs |
| **Dependencies** | FullNode RPC (get_all_mempool_items), spend-bundle parser |
| **Capital requirement** | None |
| **Risk if NOT implemented** | 40+ seconds of advance warning about incoming trades goes unused |

**What it does**: Monitors CHIA's fully transparent mempool for pending offer-take
transactions. When someone submits a take-offer, it appears in the mempool ~40
seconds BEFORE it settles in a block. The strategy:

1. Detects imminent fills on OUR offers (preemptive next-tier cancellation)
2. Detects competitor offer takes (reveals market demand direction)
3. Detects large pending transactions (signals informed flow)

Computes:
- Pending net flow: F = SUM(size * sign(side))
- Flow intensity: I = |F| / avg_daily_volume
- Spread multiplier: m_mempool = 1 + psi * I
- Skew adjustment: skew = -phi * sign(F) * I

**CHIA-specific advantage**: This is the strongest CHIA-native edge. CHIA uniquely
combines: (a) long block time giving ~40 seconds of reaction time, (b) fully
transparent mempool via full-node RPC, and (c) atomic offers where the take-
transaction reveals direction, size, AND price. Compare to Ethereum (12s blocks,
private mempools via Flashbots), Solana (0.4s blocks, negligible window), or
Bitcoin (10min blocks but no atomic DEX offers). CHIA is the only chain where
mempool observation for market making is both possible and valuable.

**Ethical note**: This is NOT front-running. We do not insert transactions ahead of
the taker. We adjust FUTURE quotes (next block) in response to observed information.
This is analogous to a traditional market maker watching order flow.

**Mathematical basis**: Daian et al. (2020) "Flash Boys 2.0" establish that mempool
observation confers informational advantage. Eskandari et al. (2020) provide the
taxonomy of front-running attacks.

**Integration**: Outputs spread multiplier and skew adjustment. Fires imminent-fill
events to the OfferManager. Feeds VPIN and OFI estimators as pre-confirmed trades,
giving them a ~40-second head start.

---

**Tier 3 Summary**:

| Metric | Value |
|--------|-------|
| Strategies | 3 (all novel, CHIA-native) |
| Total effort | 6-10 days |
| Combined edge | 18-48 bps |
| Primary advantage | Exploits CHIA blockchain properties unavailable on other chains |

---

## Tier 4: Defer

These strategies are either over-engineered for $2K/day volume, require
infrastructure that does not yet exist, or have unfavorable effort-to-edge ratios.
Revisit when volume exceeds $10K/day or after the June 2026 hard fork.

### 25. Inventory-Aware Hedging via CEX Positions (3.1)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 7-10 days |
| **Edge** | 20-50 bps risk reduction |
| **Why defer** | Operational complexity disproportionate to $10K portfolio risk |
| **Revisit when** | Capital exceeds $30K or DEX volume exceeds $10K/day |

---

### 26. Bayesian PIN (2.1)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 3-5 days |
| **Edge** | 5-15 bps (marginally better than VPIN) |
| **Why defer** | VPIN already approximates PIN; MLE unstable with ~40 trades/day |
| **Revisit when** | DEX volume exceeds $20K/day (200+ daily trades) |

---

### 27. Predatory Trading Defense (2.2)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 4-6 days |
| **Edge** | 5-15 bps in rare scenarios |
| **Why defer** | No predators exist on CHIA DEX; near-zero competition |
| **Revisit when** | Sophisticated competitors appear |

---

### 28. Stochastic Optimal Control -- Guilbaud-Pham (2.5)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 10-15 days |
| **Edge** | 10-25 bps theoretical |
| **Why defer** | HJB PDE solver is a research project; A-S+GLFT achieve 90% of optimum |
| **Revisit when** | Never, unless CHIA adopts continuous order books |

---

### 29. Multi-Asset Joint Quoting (3.8)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 7-10 days |
| **Edge** | 10-20 bps |
| **Why defer** | 2-3 liquid pairs; correlation estimation from ~40 trades/day = noise |
| **Revisit when** | 5+ pairs each with >$500/day volume |

---

### 30. Toxic Flow Classification -- ML-based (3.6)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 10-14 days |
| **Edge** | 10-25 bps |
| **Why defer** | No labeled training data; 40 trades/day = insufficient for any ML model |
| **Revisit when** | 6+ months of labeled data accumulated at 200+ trades/day |

---

### 31. Cross-Chain Bridge Arbitrage (3.10)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 7-10 days |
| **Edge** | 20-80 bps per cycle (infrequent) |
| **Why defer** | Bridge settlement 15-60 min, exploit risk, multi-chain infra needed |
| **Revisit when** | Bridge volumes exceed $50K/day |

---

### 32. Quote Stuffing / Spoofing Detection (3.12)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 4-6 days |
| **Edge** | 0-5 bps |
| **Why defer** | 52-second blocks and real fees make spoofing impractical on CHIA |
| **Revisit when** | Regulatory mandate or competitive ecosystem emerges |

---

### 33. Market Microstructure Invariance (3.15)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Defer |
| **Effort** | 5-7 days |
| **Edge** | 3-8 bps |
| **Why defer** | Invariance hypothesis debated; hand-tuned thresholds work at 2-3 pairs |
| **Revisit when** | Operating 10+ pairs where manual tuning becomes infeasible |

---

### 34. Batch Auction Strategies (3.14)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Never |
| **Effort** | 10-15 days |
| **Edge** | 0 bps (mechanism does not exist on CHIA) |
| **Why defer** | CHIA uses offer/take; no batch auction proposal exists |

---

### 35. RL Market Making (3.9)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 -- Never |
| **Effort** | 30-60 days |
| **Edge** | Unknown (potentially negative) |
| **Why defer** | No simulator exists; 40 trades/day = 70 years to match RL training data needs |

---

**Tier 4 Summary**:

| Metric | Value |
|--------|-------|
| Strategies | 11 |
| Total effort if all built | 98-162 days |
| Why deferred | Volume too low, data too sparse, infrastructure absent, or mechanism mismatch |

---

## Cumulative Edge Analysis

Edges are not perfectly additive -- strategies interact through the multiplier
pipeline. The following estimates account for overlap using a conservative
independence-discounted model.

### Per-Tier Marginal Edge

| Tier | Raw Sum (bps) | Overlap Discount | Effective Marginal (bps) | Cumulative (bps) |
|------|--------------|------------------|--------------------------|-------------------|
| **Tier 1** | 175-480 | 40% overlap (shared fair-value/vol inputs) | **105-288** | **105-288** |
| **Tier 2** | 140-400 | 50% overlap (many tuning items, arb is independent) | **70-200** | **175-488** |
| **Tier 3** | 18-48 | 20% overlap (novel signals, mostly orthogonal) | **14-38** | **189-526** |
| **Tier 4** | 93-295 | 60% overlap (diminishing returns at low volume) | **37-118** | **226-644** |

### Edge Breakdown by Source Type

| Source | Edge (bps) | Revenue at $2K/day | Revenue at $10K/day |
|--------|-----------|-------------------|---------------------|
| Adverse selection prevention (Lead-Lag, Oracle, Latency Defense) | 80-250 | $1.60-$5.00/day | $8-$25/day |
| Spread optimization (A-S/GLFT, GARCH, Thompson) | 40-100 | $0.80-$2.00/day | $4-$10/day |
| Signal quality (VPIN, OFI, Whale, Regime) | 30-80 | $0.60-$1.60/day | $3-$8/day |
| CEX-DEX arbitrage (Arb tuning) | 50-200 | $100-$400/day | $100-$400/day |
| CHIA-native (Coin-Age, Block-Cadence, Mempool) | 14-38 | $0.28-$0.76/day | $1.40-$3.80/day |
| Fee savings (Routing) | 5-15 | $0.10-$0.30/day | $0.50-$1.50/day |

**Key insight**: CEX-DEX arbitrage dominates revenue at $2K/day DEX volume. All
other strategy edge combined contributes $3-10/day from DEX spread capture. This
shifts to a more balanced mix as DEX volume grows past $10K/day.

---

## Dependency Graph

```
                    +-----------------------+
                    |   CEX Price Feed      |
                    |   (MarketDataFeed)    |
                    +-----------+-----------+
                                |
                +---------------+---------------+
                |               |               |
                v               v               v
        +-------+------+ +-----+------+ +------+--------+
        | Lead-Lag     | | GARCH Vol  | | Oracle Fair   |
        | Signals (#1) | | Fore (#3)  | | Value (#2)    |
        +-------+------+ +-----+------+ +------+--------+
                |               |               |
        +-------+       +------+------+         |
        |               |             |         |
        v               v             v         |
 +------+-------+ +-----+------+ +---+--------+ |
 | Latency Arb  | | VaR/CVaR   | | Regime     | |
 | Defense (#8) | | Limits(#18)| | Tune (#12) | |
 +--------------+ +-----+------+ +---+--------+ |
                        |             |          |
                        v             v          |
                  +-----+------+ +---+--------+  |
                  | C-J Alpha  | | A-S/GLFT   +--+
                  | Signal(#19)| | Tuning     |
                  +-----+------+ | (#4/#5)    |
                        |        +---+--------+
                        |            |
                        v            v
                  +-----+------------+-----+
                  |   Final Quote Pipeline |
                  |   (Spread Opt + Tiers) |
                  +------------------------+
                              |
                              v
                  +-----------+------------+
                  |   Thompson Sampling    |
                  |   Tuning (#7)          |
                  +------------------------+

 CHIA-Native Branch (Tier 3):
   +------------------+     +-----------------------+
   | CoinManager      +---->| Coin-Age Quoting (#22)|---+
   | (execution layer) |     +-----------------------+   |
   +------------------+                                  |
   +------------------+     +-----------------------+    |
   | FullNode RPC     +---->| Block-Cadence (#23)   |----+---> Multiplier
   | (block times)    |     +-----------------------+    |     Pipeline
   +------------------+                                  |
   +------------------+     +-----------------------+    |
   | FullNode RPC     +---->| Mempool Sentinel (#24)|----+
   | (mempool items)  |     +-----------------------+
   +------------------+

 Independent (no upstream dependencies):
   +----------------+  +------------------+  +------------------+
   | Fee Opt (#6)   |  | Whale Tune (#13) |  | VPIN Tune (#14)  |
   +----------------+  +------------------+  +------------------+
   +----------------+  +------------------+
   | OFI Tune (#15) |  | Comp Tune (#16)  |
   +----------------+  +------------------+

 Arb Branch (semi-independent):
   +-----------+      +-------------------+
   | CEX API   +----->| Arb Tune (#17)    |
   | (existing)|      +--------+----------+
   +-----------+               |
                               v
                      +--------+----------+
                      | CEX Hedging (#25) | (Tier 4)
                      +-------------------+

 AMM Branch:
   +------------------+      +-----------------+
   | TibetSwap LP     +----->| IL Hedging (#21)|
   | Integration      |      +-----------------+
   +------------------+
```

---

## Implementation Roadmap -- Critical Path

```
Week 1:  Lead-Lag (#1) + Oracle (#2) + Fee Opt (#6)
         [All independent; can parallelize; ~4 days total]
         Deliverable: CEX-anchored fair value, fee-optimized routing

Week 2:  GARCH Vol (#3) + A-S/GLFT Tuning (#4/#5) + Thompson Tuning (#7)
         [GARCH depends on CEX feed from Week 1; tuning is independent]
         Deliverable: Forward-looking vol, calibrated quoting models

Week 3:  Latency Defense (#8) + Whale/VPIN/OFI/Competitor Tuning (#13-16)
         [Defense depends on Lead-Lag from Week 1; tuning items are independent]
         Deliverable: Stale-quote prevention, calibrated signal multipliers

Week 4:  VaR Limits (#18) + Regime Tuning (#12)
         [Both depend on GARCH from Week 2]
         Deliverable: Risk-adaptive position limits, calibrated regime detector

Week 5:  Arb Engine Tuning (#17) + Kyle Lambda (#20)
         [Arb is semi-independent; Lambda needs trade DB]
         Deliverable: Live arb execution, impact-aware sizing

Week 6:  C-J Alpha (#19) + IL Hedging (#21)
         [Alpha depends on Weeks 1-4; IL depends on TibetSwap integration]
         Deliverable: Integrated alpha model, hedged AMM positions

Weeks 7-8:  CHIA-Native Novel Strategies (#22-24)
            Block-Cadence (#23, 1-2 days) -> Coin-Age (#22, 2-3 days)
            -> Mempool Sentinel (#24, 3-5 days)
            Deliverable: Full CHIA-native edge stack

Total: ~8 weeks for Tiers 1-3 (all actionable strategies)
```

---

## The Loss-Taking Question

> "Is it ever a good strategy to take a loss on a position in order to achieve
> a different goal?"

This question is addressed rigorously in `loss_manager.hpp` via five scenarios.
The answer is: **yes, under specific quantifiable conditions, but the default
should remain never-take-a-loss.**

### The Five Scenarios

| # | Scenario | When Loss is Rational | When Loss is NOT Rational |
|---|----------|----------------------|--------------------------|
| 1 | **Inventory rebalancing** | break-even blocks < max_recovery_blocks AND the restored balanced quoting generates enough spread to recoup | break-even > 100 blocks (~87 min) -- the market is too sparse for spread income to recover the loss in reasonable time |
| 2 | **Adverse selection defense** | VPIN > 0.70 AND EV(cut) > EV(hold): the expected further loss from holding in toxic flow exceeds the certain loss from cutting | VPIN < 0.70 OR market is mean-reverting (VR < 0.85) -- price will likely revert and the loss is temporary |
| 3 | **Opportunity cost of locked capital** | Locked capital fraction > 30% AND carrying cost per block > spread recovery rate | Locked capital fraction < 15% -- the opportunity cost is negligible at $10K capital |
| 4 | **Tax-loss harvesting** | Marginal tax rate > 0 AND unrealized gains exist to offset | Not applicable for crypto in most jurisdictions; set marginal_tax_rate = 0 to disable |
| 5 | **Never-loss is genuinely optimal** | VR < 0.85 (mean-reverting), VPIN < 0.70, locked capital < 30% | This is the base case. When all three conditions hold, the never-loss policy is provably optimal |

### The Break-Even Formula

From `loss_manager.hpp`:

```
blocks_to_breakeven = loss_bps / (spread_bps * fill_rate * delta_ratio * 2)
```

At CHIA parameters:
- loss_bps = 50 (underwater by 50 bps)
- spread_bps = 200 (current half-spread)
- fill_rate = 0.03 (one fill every ~33 blocks)
- delta_ratio = 0.20 (inventory at 70/30 vs target 50/50)

```
break-even = 50 / (200 * 0.03 * 0.20 * 2) = 50 / 2.4 = 20.8 blocks ~ 18 minutes
```

This is well within the 100-block ceiling, suggesting a 50 bps loss to restore
balance IS rational at these parameters -- the restored two-sided quoting recovers
the loss in ~18 minutes.

At 200 bps underwater:
```
break-even = 200 / 2.4 = 83.3 blocks ~ 72 minutes
```

Still within ceiling, but marginal. Beyond ~250 bps underwater, the break-even
exceeds the ceiling and holding becomes the better option.

### The Adverse Selection EV

From `loss_manager.hpp`:

```
EV(hold) = -(1 - P_revert) * sigma * sqrt(tau) * k
EV(cut)  = -loss_bps
```

At CHIA parameters with elevated VPIN:
- P_revert = 0.40 (trending market, VR > 1.15)
- sigma per block = 0.50 * sqrt(52 / 31536000) = 0.000641
- tau = 100 blocks
- k = 1.5 (1.5 sigma tail risk)

```
EV(hold) = -(1 - 0.40) * 0.000641 * sqrt(100) * 1.5 * 10000
         = -0.60 * 0.00641 * 1.5 * 10000 = -57.7 bps

EV(cut at 30 bps loss) = -30 bps
```

Since EV(cut) = -30 > EV(hold) = -57.7, cutting at 30 bps is rational when
flow is toxic and the market is trending.

### Recommendation for XOPTrader

1. **Default**: `LossManagerConfig.enabled = false` (strict never-loss policy).
2. **Enable strategically**: Set `enabled = true` with `max_acceptable_loss_bps = 50`
   and `min_spread_recovery_blocks = 100` once the strategy engine is calibrated.
3. **Regime-dependent**: Automatically raise `mean_reversion_probability` to 0.80
   in mean-reverting regimes (VR < 0.85) and lower to 0.30 in trending regimes
   (VR > 1.15), making loss-taking more likely only when markets trend.
4. **Never override manually**: The StrategicLossManager is advisory. The final
   decision requires all four conditions to pass (enabled, within loss cap, within
   recovery horizon, and EV-positive). No single signal can force a loss.

---

## The Drift Question

> "Would holding too long make the balances uneven?"

This question is addressed rigorously in `drift_analyzer.hpp`. The answer is:
**yes, and it is the primary risk of the never-sell-at-loss policy on a 24/7 market.**

### Time-to-Breach Under Different Conditions

Derived from the drift_analyzer.hpp mathematical model with CHIA parameters:
- V_total = $10,000, XCH price = $2.70, delta_q = 50 XCH, lambda = 0.05 fills/block

| Market Condition | Soft Limit (60/40) | Hard Limit (80/20) |
|-----------------|-------------------|-------------------|
| **Random Walk** (balanced fills) | ~1,095 blocks (~15.8 hours) | ~9,875 blocks (~5.9 days) |
| **Trending 5%/day** | ~824 blocks (~11.9 hours) | ~2,474 blocks (~35.7 hours) |
| **Trending 10%/day** | ~412 blocks (~5.9 hours) | ~1,237 blocks (~17.9 hours) |
| **Mean-Reverting** (GLFT) | Steady-state contained | P(breach) < 5% with calibrated phi |

### The UTXO Feedback Loop

CHIA's coin-set model creates a positive feedback loop on drift:

1. Inventory skews toward base (e.g., 70/30)
2. Fewer quote-side coins available for bids
3. Fewer bids posted => fewer buys of quote
4. Inventory skews further toward base

The amplification factor at ratio r:

```
amplification = 1 / (1 - |2r - 1| * utxo_sensitivity)
```

At r = 0.70, sensitivity = 0.50: amplification = 1.25 (25% faster drift).
At r = 0.80, sensitivity = 0.50: amplification = 1.43 (43% faster drift).

This feedback accelerates precisely when drift is already problematic.

### The 24/7 Continuous Operation Risk

Compared to equity markets (6.5-hour sessions), CHIA's 24/7 operation gives:

```
sigma_q_24_7 / sigma_q_session = sqrt(24 / 6.5) = 1.92
```

The expected time-to-breach is ~3.7x shorter in calendar hours. There is no
forced session-end rebalancing, no overnight position reduction, and low-liquidity
periods (Asian night) can produce wider excursions with no corrective mechanism.

### A-S vs GLFT for Drift Control

The drift_analyzer.hpp comparison establishes GLFT as the superior model for 24/7:

| Property | A-S | GLFT |
|----------|-----|------|
| Reversion force | F = gamma * sigma^2 * tau * q (time-dependent) | F = phi * q / q_max (time-invariant) |
| Steady-state variance | Periodic reset creates vulnerability windows | Var[q] = q_max / (2 * phi * kappa) -- stable |
| 24/7 suitability | Weakens right after horizon reset | Constant reversion pressure |
| Drift containment | Periodic windows where drift is uncontrolled | OU process with well-defined bounds |

**Recommendation**: Use GLFT as the primary model for 24/7 operation. Use A-S as a
secondary signal for intraday quote sharpening when tau is small (urgent periods).

### Drift Mitigation Priority

| Action | When | Effect |
|--------|------|--------|
| Calibrate GLFT phi | Before going live | Sets steady-state drift bounds |
| Coin-Age Quoting (#22) | After core calibration | Accelerates turnover of old inventory |
| VaR/CVaR Limits (#18) | After GARCH | Reduces position limits in high-vol |
| Mempool Sentinel (#24) | After core calibration | Preemptive response to fill events |
| Strategic Loss Manager | After all above | Last resort: take rational loss to restore balance |

---

## CHIA-Specific Advantages and Strategy Mapping

CHIA's blockchain has five structural properties that create unique market-making
edges unavailable on other chains. The table below maps each property to the
strategies that exploit it.

### Property 1: Coin-Set (UTXO) Model

| Aspect | Detail |
|--------|--------|
| **What** | Every unit of inventory is a distinct coin with a known creation block |
| **Edge** | Deterministic age tracking per inventory unit |
| **Unavailable on** | Ethereum, Solana, all account-model chains |
| **Strategies that exploit this** | Coin-Age-Weighted Quoting (#22), UTXO drift feedback (drift_analyzer) |
| **Priority impact** | Makes #22 uniquely valuable; impossible to replicate on competitors |

### Property 2: Transparent Mempool

| Aspect | Detail |
|--------|--------|
| **What** | Full-node RPC exposes all pending transactions (get_all_mempool_items) |
| **Edge** | ~40 seconds advance warning of imminent trades |
| **Unavailable on** | Ethereum (Flashbots private mempools), Solana (no meaningful window) |
| **Strategies that exploit this** | Mempool Sentinel (#24), pre-confirmed VPIN/OFI feeds |
| **Priority impact** | Makes #24 the highest-alpha CHIA-native strategy |

### Property 3: Variable Block Cadence (52s target, 10-200s actual)

| Aspect | Detail |
|--------|--------|
| **What** | Inter-block intervals have 20x variance range |
| **Edge** | Spread/kappa adjustment for cadence deviation |
| **Unavailable on** | Ethereum (narrow variance), Solana (sub-second, negligible) |
| **Strategies that exploit this** | Block-Cadence Adaptive Spread (#23), adjusted kappa for A-S/GLFT |
| **Priority impact** | Makes #23 a low-effort, CHIA-only edge |

### Property 4: Atomic Offer Model (Not AMM, Not CLOB)

| Aspect | Detail |
|--------|--------|
| **What** | Offers are self-contained puzzles; taker completes the transaction atomically |
| **Edge** | No partial fills, no order-book manipulation, no queue priority games |
| **Unavailable on** | AMM chains (slippage curves), CLOB chains (queue priority) |
| **Strategies that exploit this** | Multi-Tier Liquidity (#10), Competitor Detection (#16), Offer cancellation strategy |
| **Priority impact** | Simplifies execution layer; eliminates latency arms race |

### Property 5: 24/7 No-Session-End Market

| Aspect | Detail |
|--------|--------|
| **What** | No market close, no opening auction, continuous operation |
| **Edge** | 3.7x more trading time than equities; also 3.7x more drift risk |
| **Risk** | No forced rebalancing event; inventory can drift unchecked |
| **Strategies that address this** | GLFT (time-invariant reversion), Drift Analyzer, Strategic Loss Manager |
| **Priority impact** | Makes GLFT > A-S for primary model; drift analysis is essential infrastructure |

### Strategy-to-Advantage Mapping Matrix

| Strategy | UTXO | Mempool | Block Variance | Atomic Offers | 24/7 |
|----------|------|---------|---------------|---------------|------|
| Coin-Age Quoting (#22) | **PRIMARY** | - | - | - | supports |
| Block-Cadence (#23) | - | - | **PRIMARY** | - | - |
| Mempool Sentinel (#24) | - | **PRIMARY** | supports | supports | - |
| GLFT (primary model) | - | - | - | - | **PRIMARY** |
| Multi-Tier Liquidity | - | - | - | **PRIMARY** | - |
| Drift Analyzer | supports | - | - | - | **PRIMARY** |
| Loss Manager | - | - | - | - | supports |
| A-S/GLFT kappa adj | - | - | supports | - | - |

---

## Honesty Check -- Self-Reflection

### Are any Tier 1 items overrated?

**Latency Arb Defense (#8)**: Possibly. At 300-1000 bps DEX spreads, stale-quote
sniping requires a price move of 3-10% within 52 seconds. This happens maybe
1-2 times per month for XCH. The defense is more important AFTER we tighten
spreads to 50-150 bps. Keep it in Tier 1 because it is a prerequisite for spread
tightening, and spread tightening is the path to volume growth.

**Thompson Sampling Tuning (#7)**: Also possibly overrated. With ~40 fills/day
across all pairs, Thompson Sampling converges slowly no matter how we tune the
grid. Keep in Tier 1 because it is only 1 day of work and the grid adjustment is
nearly free.

### Are any Tier 3 items actually more valuable than ranked?

**Mempool Sentinel (#24, Tier 3)**: This is the most underrated strategy. 40 seconds
of advance warning about incoming trades is an extraordinary informational edge --
comparable to the Flash Boys advantage that generated billions on Wall Street.
However, it requires spend-bundle parsing (which does not exist) and mempool RPC
integration. The 3-5 day effort estimate assumes parsing infrastructure. If the
mempool integration turns out to be simpler than expected, promote this to Tier 2.

**Coin-Age Quoting (#22, Tier 3)**: May deserve Tier 2 at current volumes where
inventory sits for hours. The 5-15 bps estimate may be conservative on a market
where average time-to-fill is 30-120 minutes -- urgency-weighted quoting could
meaningfully accelerate turnover.

### Are any Tier 4 items actually more valuable than ranked?

**CEX Hedging (#25, Tier 4)**: The most underrated deferred strategy. If XCH drops
30% (it has been at all-time lows), the never-sell-at-loss constraint locks up all
inventory indefinitely. CEX hedging (shorting XCH futures/perps on OKX) would
prevent this scenario entirely. At $10K capital the dollar risk of a 30% drawdown
is $3K, which is painful but survivable. At $30K+, hedging becomes essential.
If you plan to scale to $30K within 3 months, start building CEX hedging
infrastructure now (move to Tier 2).

### What matters most at $2K/day volume?

At $2K/day DEX volume, the total addressable daily PnL is approximately:
- Spread capture: $2,000 x 1.5% effective capture rate = ~$30/day
- CEX-DEX arb: $2,400,000 x 0.01% capture rate = ~$240/day (if execution works)
- DBX incentives: ~$5-10/day
- **Total: ~$275-280/day optimistic**

The honest truth: **CEX-DEX arbitrage is the primary revenue source**, not spread
capture on the DEX order book. This means Lead-Lag CEX Signals (#1) and Arb Engine
Tuning (#17) are the two most important revenue drivers.

The second honest truth: **at $2K/day DEX volume, most of the 35 strategies are
solving problems that do not exist yet.** The core need is:
1. Do not quote stale prices (Lead-Lag + Oracle)
2. Capture CEX-DEX dislocations (Arb tuning)
3. Set spreads that actually fill (A-S/GLFT/Thompson tuning)
4. Manage drift before it locks capital (GLFT calibration + Drift monitoring)
5. Collect DBX incentives (just show up with competitive quotes)

Everything else is preparation for the future or insurance against tail risk.
The CHIA-native strategies (#22-24) are investments in structural edge that will
compound as volume grows.

---

*CONSTRAINT: All strategies enforce NEVER SELL AT A LOSS unless the StrategicLossManager
(disabled by default) explicitly approves a loss after passing all four conditions:
enabled, within loss cap, within recovery horizon, and EV-positive.*

*Generated for XOPTrader Issue #9*
