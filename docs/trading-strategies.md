# XOPTrader — Trading Strategy Catalog

> Comprehensive inventory of implemented, considered, and future strategies for
> Chia DEX market making.  Each entry includes a brief description, pros/cons,
> scholarly references where applicable, and current implementation status.

---

## Table of Contents

1. [Implemented Strategies](#1-implemented-strategies)
   - [1.1 Avellaneda-Stoikov Optimal Market Making](#11-avellaneda-stoikov-optimal-market-making)
   - [1.2 GLFT Running-Inventory-Penalty Model](#12-glft-running-inventory-penalty-model)
   - [1.3 Four-Component Spread Optimization](#13-four-component-spread-optimization)
   - [1.4 Multi-Tier Liquidity Provision](#14-multi-tier-liquidity-provision)
   - [1.5 Market Regime Detection](#15-market-regime-detection)
   - [1.6 Cross-Platform Arbitrage](#16-cross-platform-arbitrage)
   - [1.7 Competitor Detection & Response](#17-competitor-detection--response)
   - [1.8 Whale Trade Detection & Spread Widening](#18-whale-trade-detection--spread-widening)
   - [1.9 VPIN — Flow Toxicity Signal](#19-vpin--flow-toxicity-signal)
   - [1.10 OFI — Order Flow Imbalance](#110-ofi--order-flow-imbalance)
   - [1.11 Asymmetric Spread Widening](#111-asymmetric-spread-widening)
   - [1.12 Thompson Sampling for Spread Learning](#112-thompson-sampling-for-spread-learning)
2. [Considered but Not Yet Implemented](#2-considered-but-not-yet-implemented)
   - [2.1 Bayesian PIN (Probability of Informed Trading)](#21-bayesian-pin-probability-of-informed-trading)
   - [2.2 Predatory Trading Defense (Brunnermeier-Pedersen)](#22-predatory-trading-defense-brunnermeier-pedersen)
   - [2.3 Cartea-Jaimungal Alpha Signal](#23-cartea-jaimungal-alpha-signal)
   - [2.4 Kyle Lambda / Permanent Price Impact](#24-kyle-lambda--permanent-price-impact)
   - [2.5 Stochastic Optimal Control (Guilbaud-Pham)](#25-stochastic-optimal-control-guilbaud-pham)
3. [Unimplemented — Future Strategy TODO List](#3-unimplemented--future-strategy-todo-list)
   - [3.1 Inventory-Aware Hedging via CEX Positions](#31-inventory-aware-hedging-via-cex-positions)
   - [3.2 Volatility Forecasting (GARCH / Realized Volatility)](#32-volatility-forecasting-garch--realized-volatility)
   - [3.3 Lead-Lag Signals from CEX Price Feeds](#33-lead-lag-signals-from-cex-price-feeds)
   - [3.4 Maker-Taker Fee Optimization](#34-maker-taker-fee-optimization)
   - [3.5 Dynamic Position Limits (VaR / CVaR)](#35-dynamic-position-limits-var--cvar)
   - [3.6 Toxic Flow Classification (ML-based)](#36-toxic-flow-classification-ml-based)
   - [3.7 Latency Arbitrage Defense](#37-latency-arbitrage-defense)
   - [3.8 Multi-Asset Joint Quoting](#38-multi-asset-joint-quoting)
   - [3.9 Adaptive Market-Making with Reinforcement Learning](#39-adaptive-market-making-with-reinforcement-learning)
   - [3.10 Cross-Chain Bridge Arbitrage](#310-cross-chain-bridge-arbitrage)
   - [3.11 Oracle-Based Fair-Value Anchoring](#311-oracle-based-fair-value-anchoring)
   - [3.12 Quote Stuffing / Spoofing Detection](#312-quote-stuffing--spoofing-detection)
   - [3.13 Impermanent Loss Hedging for AMM LP Positions](#313-impermanent-loss-hedging-for-amm-lp-positions)
   - [3.14 Batch Auction Strategies (Frequent Batch Auctions)](#314-batch-auction-strategies-frequent-batch-auctions)
   - [3.15 Market Microstructure Invariance](#315-market-microstructure-invariance)
4. [Strategy Interaction Matrix](#4-strategy-interaction-matrix)
5. [References](#5-references)

---

## 1. Implemented Strategies

### 1.1 Avellaneda-Stoikov Optimal Market Making

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/avellaneda.cpp`, `cpp/include/xop/strategy/avellaneda.hpp` |
| **Reference** | Avellaneda & Stoikov (2008), *"High-frequency trading in a limit order book"*, Quantitative Finance, 8(3), 217–224. |

**Description.**
The classical A-S model computes an inventory-penalized reservation price and
an optimal half-spread.  The reservation price skews quotes to encourage
inventory mean-reversion:

```
r = S − q · γ · σ² · τ
half_spread = γσ²τ + (2/γ) · ln(1 + γ/κ)
```

where `S` is mid-price, `q` is inventory, `γ` is risk aversion, `σ` is
volatility, `τ` is remaining time in horizon, and `κ` is the fill-rate
parameter.

Our implementation adapts A-S for Chia's 52-second block cadence by using a
rolling N-block horizon instead of a fixed terminal time.

**Pros:**
- Closed-form solution — fast to evaluate per block.
- Naturally skews quotes away from excessive inventory.
- Well-studied with decades of empirical validation.
- Adapts to volatility regime changes through σ parameter.

**Cons:**
- Assumes continuous-time dynamics; Chia's 52-second blocks are coarse.
- Fill-rate parameter κ must be estimated from sparse DEX data.
- Does not model adverse selection directly (informed vs. uninformed flow).
- Single-asset model; cross-pair correlations ignored.

---

### 1.2 GLFT Running-Inventory-Penalty Model

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/glft.cpp`, `cpp/include/xop/strategy/glft.hpp` |
| **Reference** | Guéant, Lehalle & Fernandez-Tapia (2013), *"Dealing with the inventory risk: a solution to the market making problem"*, Mathematics and Financial Economics, 7(4), 477–507. |

**Description.**
GLFT extends the Avellaneda-Stoikov framework with a running inventory penalty
term φ that produces a *linear* skew proportional to normalized inventory:

```
half_spread = (1/κ) · ln(1 + κ/γ) + 0.5 · γ · σ² · τ
skew = −φ · q / q_max
```

The linear penalty is milder than A-S's quadratic penalty near zero inventory
and stronger near limits, making it better suited for DEX venues where fill
probabilities are hard to estimate.

**Pros:**
- More robust than A-S when order-arrival rates are uncertain.
- Linear skew avoids excessive quote aggressiveness at small inventory.
- Gracefully approaches position limits via normalized q/q_max.
- Peer-reviewed extension of the foundational A-S model.

**Cons:**
- Requires calibration of both γ and φ — two risk-aversion parameters.
- Still assumes symmetric, Poisson-like fill dynamics.
- Does not incorporate order-book information or queue position.
- No closed-form for general intensity functions.

---

### 1.3 Four-Component Spread Optimization

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/spread.cpp`, `cpp/include/xop/strategy/spread.hpp` |
| **Reference** | Composite model drawing on Amihud & Mendelson (1986), Stoll (1978), and Ho & Stoll (1981). |

**Description.**
The spread optimizer decomposes the quoted spread into four additive
components, each addressing a distinct economic cost:

```
total_spread = s_adverse + s_inventory + s_cost + s_competition
```

| Component | Captures |
|-----------|----------|
| `s_adverse` | Adverse-selection cost — compensation for informed traders |
| `s_inventory` | Inventory risk — penalty for imbalanced positions |
| `s_cost` | Transaction costs — blockchain fees, venue fees |
| `s_competition` | Competitive pressure — tightening when rivals quote tighter |

Venue fees are pre-coded per platform (Dexie 0%, TibetSwap 0.7%,
Hashgreen 0.9%, OfferBin 0%, Splash 0%).

**Pros:**
- Economically grounded decomposition; each component is independently tunable.
- Explicitly models competition — critical on thin DEX markets.
- Transparent: operators can see *why* the spread is what it is.
- Easy to audit and explain to stakeholders.

**Cons:**
- Additive model may under- or over-estimate when components interact.
- Component weights need periodic re-calibration.
- Does not directly optimize expected profit or utility.
- Competition component is reactive, not predictive.

---

### 1.4 Multi-Tier Liquidity Provision

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/liquidity.cpp`, `cpp/include/xop/strategy/liquidity.hpp` |
| **Reference** | Practitioner model; related concepts in Cartea, Jaimungal & Penalva (2015), Ch. 11. |

**Description.**
Creates a multi-tiered offer ladder with configurable spacing and size
allocation.  Capital is divided into four layers:

| Layer | Allocation | Purpose |
|-------|-----------|---------|
| Foundation | 30–40% | Tight spreads for competitive quoting |
| Active Core | 30–40% | Medium spreads for bulk flow capture |
| Opportunistic | 10–15% | Wide spreads for volatility events |
| Reserve | 10–15% | Dry powder for rebalancing/emergencies |

Rebalance triggers include price deviation, inventory skew, offer staleness,
volume spikes, and volatility spikes.  Because Chia offers are immutable
on-chain, rebalancing follows a cancel-then-recreate model.

**Pros:**
- Captures flow across a range of market conditions.
- Reserve layer provides resilience during dislocations.
- Tier sizing is configurable per pair and regime.
- Natural depth display deters aggressive takers.

**Cons:**
- Cancel-then-recreate introduces latency on Chia (1–2 blocks).
- Tiers may overlap or gap during fast markets.
- Static allocation doesn't adapt to intraday flow patterns.
- More capital-intensive than single-level quoting.

---

### 1.5 Market Regime Detection

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/regime.cpp`, `cpp/include/xop/strategy/regime.hpp` |
| **Reference** | Lo & MacKinlay (1988), *"Stock market prices do not follow random walks"*, Review of Financial Studies, 1(1), 41–66.  Hamilton (1989), *"A new approach to the economic analysis of nonstationary time series"*, Econometrica, 57(2), 357–384. |

**Description.**
Classifies the current market into one of three regimes using a variance
ratio test and an optional Hidden Markov Model:

| Regime | VR(q) | Strategy Adjustment |
|--------|-------|---------------------|
| Mean-Reverting | < 1.0 | Tighter spreads, more aggressive quoting |
| Random Walk | ≈ 1.0 | Normal quoting parameters |
| Momentum | > 1.0 | Wider spreads, reduced inventory limits |

The Z-statistic follows Lo-MacKinlay's homoskedasticity-consistent formula.
The HMM uses three volatility states fit via Baum-Welch with hysteresis to
prevent whipsawing between regimes.

**Pros:**
- Statistically grounded regime classification (variance ratio test).
- HMM captures non-obvious regime transitions.
- Hysteresis prevents costly flip-flopping.
- Directly modulates strategy parameters (spread, skew multipliers).

**Cons:**
- Variance ratio test assumes stationary returns within window.
- HMM calibration requires sufficient history (cold-start problem).
- Three regimes may be too coarse for crypto's multi-modal dynamics.
- Lookback window length is a sensitive hyperparameter.

---

### 1.6 Cross-Platform Arbitrage

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/arbitrage.cpp`, `cpp/include/xop/strategy/arbitrage.hpp` |
| **Reference** | Practitioner model; related theory in Shleifer & Vishny (1997), *"The limits of arbitrage"*, Journal of Finance, 52(1), 35–55. |

**Description.**
Detects and evaluates four types of arbitrage opportunities:

| Type | Description |
|------|-------------|
| CEX-DEX | Price divergence between centralized and decentralized exchanges |
| Cross-DEX | Price differences between Chia DEX platforms |
| Triangular | Three-way cycles through ~50 CAT assets (O(N²) enumeration) |
| Cross-Bridge | Wrapped-asset pricing gaps across bridges |

Profit calculation accounts for buy/sell fees, blockchain settlement time,
bridge fees, and slippage.

**Pros:**
- Multiple arbitrage vectors increase opportunity surface.
- Automated profit calculation prevents unprofitable executions.
- Triangular arb captures hidden CAT mispricing.
- Cross-bridge arb monetizes wrapped-asset inefficiency.

**Cons:**
- Chia settlement latency (52-second blocks) limits arb speed.
- Bridge risk adds counterparty and smart-contract exposure.
- CEX-DEX arb requires capital on both sides.
- Triangular route enumeration is O(N²) — scales quadratically with assets.

---

### 1.7 Competitor Detection & Response

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/execution/market_data.cpp` |
| **Docs** | `docs/competitor-detection.md` |

**Description.**
Monitors other market makers' offers on Dexie.  Computes the best competing
bid/ask spread in basis points and fires alerts when a competitor appears with
spreads tighter than a configurable threshold (default 50 bps).

**Pros:**
- Prevents quoting into a void — knows when others are tighter.
- Alert threshold enables automated tightening or withdrawal.
- Crossed-book guard prevents erroneous quotes.

**Cons:**
- Purely reactive — doesn't anticipate competitor behavior.
- Chia DEX offers are pseudonymous; can't track *which* competitor.
- Alert logic is threshold-based, not predictive.

---

### 1.8 Whale Trade Detection & Spread Widening

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/execution/market_data.cpp` |
| **Docs** | `docs/whale-trader-response.md` |
| **Tests** | `cpp/tests/test_whale_detection.cpp` (14 tests) |

**Description.**
Detects abnormally large trades (≥ 50 XCH or ≥ 5% of 24-hour volume) within
a rolling window (10 blocks ≈ 8.7 min).  The spread multiplier ramps linearly
from 1.0× (no whales) to 3.0× (sustained whale activity):

```
multiplier = 1.0 + min(events_in_window / window_blocks, 1.0) × (max_mult − 1.0)
```

Tracks dominant side (Bid/Ask) for downstream asymmetric widening.

**Pros:**
- Directly mitigates adverse selection from informed large traders.
- Gradual ramp avoids over-reaction to single events.
- Runtime-tunable thresholds via validated setters.
- Thread-safe with dedicated mutexes and config locking.

**Cons:**
- Binary whale/non-whale classification; no flow-toxicity continuum.
- 10-block window may be too short for slow, patient whale accumulation.
- Does not distinguish informed from uninformed large trades.
- Volume fraction threshold sensitive to 24h volume estimation accuracy.

---

### 1.9 VPIN — Flow Toxicity Signal

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/execution/market_data.cpp` |
| **Docs** | `docs/advanced-trading-methods.md` |
| **Tests** | `cpp/tests/test_advanced_trading.cpp` (7 VPIN tests) |
| **Reference** | Easley, López de Prado & O'Hara (2012), *"Flow Toxicity and Liquidity in a High-frequency World"*, Review of Financial Studies, 25(5), 1457–1493. |

**Description.**
VPIN estimates the probability that order flow is informed by measuring
buy-sell imbalance across volume-synchronized bars:

```
VPIN = (1/n) · Σ |V_buy − V_sell| / bucket_size
```

Each bucket holds exactly `vpin_bucket_size` units of volume (default 10 XCH).
The rolling window covers the most recent `vpin_window_buckets` (default 50)
completed buckets.

| VPIN Range | Interpretation | Response |
|------------|---------------|----------|
| 0.0–0.2 | Balanced, uninformed | Normal quoting |
| 0.2–0.5 | Mildly imbalanced | Slight widening |
| 0.5–0.8 | Significantly toxic | Widen spreads, reduce size |
| 0.8–1.0 | Highly informed | Maximum widening or pause |

**Pros:**
- Continuous [0, 1] signal vs. binary whale flag — smoother modulation.
- Volume-synchronized bars adapt to trading activity naturally.
- Widely used in institutional market-making and surveillance.
- Computationally lightweight (running mean of absolute imbalances).

**Cons:**
- Requires trade-side classification (Chia DEX offers are two-sided).
- Bucket size is sensitive to pair liquidity; needs per-pair tuning.
- VPIN is a lagging indicator — only signals after volume trades through.
- Original paper calibrated for equities; crypto behavior may differ.

---

### 1.10 OFI — Order Flow Imbalance

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/execution/market_data.cpp` |
| **Docs** | `docs/advanced-trading-methods.md` |
| **Tests** | `cpp/tests/test_advanced_trading.cpp` (6 OFI tests) |
| **Reference** | Cont, Kukanov & Stoikov (2014), *"The Price Impact of Order Book Events"*, Quantitative Finance, 14(1), 109–126. |

**Description.**
OFI aggregates signed volume changes at best bid and ask across consecutive
order-book snapshots.  The bid-side event eᴮ and ask-side event eᴬ
follow the Cont et al. sign convention:

- eᴮ is positive when the bid strengthens (price rises or size increases).
- eᴬ is positive when the ask strengthens (price decreases or size increases).
- OFI = eᴮ − eᴬ → positive for buy pressure, negative for sell pressure.

The normalized OFI is clamped to [−1, 1] for use as a spread multiplier input.

**Pros:**
- *Leading* indicator — detects pressure before fills confirm.
- Directly tied to price-impact theory (Cont et al. regression R² ≈ 65%).
- Normalized output slots neatly into the spread multiplier pipeline.
- Lightweight: only requires best bid/ask snapshots.

**Cons:**
- Chia DEX order books are thin and update every ~52 seconds.
- Normalization heuristic may saturate in highly volatile periods.
- No concept of queue priority — Chia offers are not queued.
- Window size (default 20 snapshots ≈ 17 min) is a tuning parameter.

---

### 1.11 Asymmetric Spread Widening

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/execution/market_data.cpp` |
| **Docs** | `docs/advanced-trading-methods.md` |
| **Tests** | `cpp/tests/test_advanced_trading.cpp` (5 tests) |

**Description.**
Decomposes the symmetric whale spread multiplier into bid/ask–specific
multipliers using the detected dominant side:

```
excess = m − 1.0
high_mult = 1.0 + excess × (1 + α)    // informed side
low_mult  = 1.0 + excess × (1 − α)    // uninformed side
```

The average `(high + low) / 2 = m` is preserved.  The skew factor α
(default 0.5) controls the asymmetry: 0 = symmetric, 1 = fully one-sided.

**Pros:**
- Reduces inventory build-up on the toxic side.
- Preserves average spread — no net widening beyond the symmetric target.
- Single knob (α) makes tuning straightforward.
- Synergizes with whale detection's `dominant_side` tracking.

**Cons:**
- Depends on correct identification of dominant side.
- α is static; ideally it would adapt to signal confidence.
- Only considers whale-driven asymmetry; OFI could also drive skew.
- Average preservation is weaker than product preservation for profitability.

---

### 1.12 Thompson Sampling for Spread Learning

| Attribute | Detail |
|-----------|--------|
| **Status** | ✅ Implemented |
| **File** | `cpp/src/strategy/spread.cpp` |
| **Reference** | Thompson (1933), *"On the likelihood that one unknown probability exceeds another"*, Biometrika, 25(3/4), 285–294. Agrawal & Goyal (2012), *"Analysis of Thompson sampling for the multi-armed bandit problem"*, COLT 2012. |

**Description.**
Uses Beta-distribution Thompson sampling to learn optimal spread levels
from fill/no-fill outcomes.  Each candidate spread is a "bandit arm" with
a Beta(α, β) posterior updated on fill (α++) or timeout (β++).

**Pros:**
- Automatically explores and exploits; no manual spread grid tuning.
- Bayesian framework naturally balances exploration vs. exploitation.
- Sub-linear regret bound (√(T log T)).
- Works with sparse, delayed fill feedback typical of DEX markets.

**Cons:**
- Assumes stationary fill probabilities — may lag regime changes.
- Discretization of spread space limits resolution.
- Needs sufficient fills to converge; problematic for illiquid pairs.
- Does not account for adverse selection conditional on fill.

---

## 2. Considered but Not Yet Implemented

### 2.1 Bayesian PIN (Probability of Informed Trading)

| Attribute | Detail |
|-----------|--------|
| **Status** | 🔬 Considered |
| **Reference** | Easley, Kiefer, O'Hara & Paperman (1996), *"Liquidity, information, and infrequently traded stocks"*, Journal of Finance, 51(4), 1405–1436. Easley & O'Hara (1992), *"Time and the process of security price adjustment"*, Journal of Finance, 47(2), 577–605. |

**Description.**
The PIN model estimates the probability that a trade is driven by private
information using a mixture model of informed/uninformed arrivals.  A Bayesian
formulation updates the PIN posterior in real time as trades arrive.

**Pros:**
- Theoretically foundational model of informed trading.
- Decomposition into ε (uninformed), μ (informed), and α (event probability).
- Could replace or supplement VPIN with a more principled estimator.

**Cons:**
- Maximum-likelihood estimation is numerically unstable for boundary cases.
- Original model assumes Poisson arrivals; Chia's block-based arrival is bursty.
- VPIN was specifically designed as a more practical, real-time PIN proxy.
- Requires order-side classification, which is ambiguous on Chia DEX.

**Decision:** Deferred in favor of VPIN, which was designed as a real-time
approximation of PIN.  PIN could be revisited for offline calibration.

---

### 2.2 Predatory Trading Defense (Brunnermeier-Pedersen)

| Attribute | Detail |
|-----------|--------|
| **Status** | 🔬 Considered |
| **Reference** | Brunnermeier & Pedersen (2005), *"Predatory trading"*, Journal of Finance, 60(4), 1825–1863. |

**Description.**
Models how predatory traders front-run a distressed liquidator.  When our
inventory is visibly large and trending, predators may trade ahead to profit
from our forced rebalancing.

**Pros:**
- Directly relevant when whale activity pushes our inventory to limits.
- Could trigger "stealth liquidation" — slower, randomized unwinding.
- Improves survivability during cascading liquidations.

**Cons:**
- Requires detecting *whether* we're being front-run (hard on Chia).
- Stealth liquidation sacrifices speed for concealment.
- Model assumes a known distress level visible to predators.

**Decision:** Deferred.  Whale detection plus asymmetric widening provides
partial defense.  Full predatory-trading defense needs inventory visibility
analysis, which is complex on Chia's transparent blockchain.

---

### 2.3 Cartea-Jaimungal Alpha Signal

| Attribute | Detail |
|-----------|--------|
| **Status** | 🔬 Considered |
| **Reference** | Cartea, Jaimungal & Penalva (2015), *"Algorithmic and High-Frequency Trading"*, Cambridge University Press, Ch. 10. |

**Description.**
Models a short-term "alpha" signal α(t) that predicts the next price move
from recent order flow.  When alpha is high (e.g., post-whale), the maker
widens asymmetrically on the informed side.

**Pros:**
- Formalizes the directional signal from OFI into an explicit price predictor.
- Naturally integrates with the A-S/GLFT reservation price.
- Could improve quote placement by anticipating next-block price moves.

**Cons:**
- Signal estimation requires a regression model: α = f(OFI, VPIN, …).
- Overfitting risk on sparse Chia data.
- Calibration window is unclear for 52-second blocks.

**Decision:** Partially addressed by OFI and asymmetric widening.  A formal
alpha model would require building a next-price predictor, which is a
significant modelling effort.  Good candidate for a follow-up issue.

---

### 2.4 Kyle Lambda / Permanent Price Impact

| Attribute | Detail |
|-----------|--------|
| **Status** | 🔬 Considered |
| **Reference** | Kyle (1985), *"Continuous auctions and insider trading"*, Econometrica, 53(6), 1315–1335. |

**Description.**
Kyle's lambda (λ) measures the permanent price impact per unit of net order
flow.  Estimating λ from DEX data would let us decompose trade impact into
temporary and permanent components and size our quotes accordingly.

**Pros:**
- Foundational microstructure concept linking volume to price movement.
- Could improve spread sizing for large trades.
- Simple linear model: ΔP = λ · ΔQ.

**Cons:**
- Requires regression on trade-price data with sufficient observations.
- Lambda is time-varying and context-dependent.
- Chia DEX trades are sparse — regression may be noisy.

**Decision:** Deferred.  OFI captures a similar signal (order-book pressure
→ price move) without needing the full Kyle regression infrastructure.

---

### 2.5 Stochastic Optimal Control (Guilbaud-Pham)

| Attribute | Detail |
|-----------|--------|
| **Status** | 🔬 Considered |
| **Reference** | Guilbaud & Pham (2013), *"Optimal high-frequency trading with limit and market orders"*, Quantitative Finance, 13(1), 79–94. |

**Description.**
Extends A-S to jointly optimize limit and market order placement using
stochastic optimal control with a Hamilton-Jacobi-Bellman PDE.  The maker can
choose to aggress (take liquidity) when inventory risk is high.

**Pros:**
- Jointly optimizes passive and aggressive execution.
- Better inventory management under extreme conditions.
- Theoretically optimal in the model's assumptions.

**Cons:**
- HJB PDE is numerically intensive to solve.
- Requires continuous order-book presence — Chia's offer model is discrete.
- Market orders on Chia DEX (taking someone's offer) have different mechanics.
- Implementation complexity far exceeds the marginal benefit on thin DEX.

**Decision:** Deferred.  The complexity-to-benefit ratio is unfavorable for
Chia's offer-based model.  GLFT and A-S already provide good inventory control.

---

## 3. Unimplemented — Future Strategy TODO List

### 3.1 Inventory-Aware Hedging via CEX Positions

| Priority | Medium |
|----------|--------|
| **Effort** | High |
| **Reference** | Guéant (2017), *"Optimal market making"*, Applied Mathematical Finance, 24(2), 112–154. |

**Description.**
Maintain offsetting positions on centralized exchanges (e.g., Binance, OKX) to
hedge the inventory risk accumulated on DEX.  When DEX inventory exceeds a
threshold, place a hedging order on CEX to neutralize delta.

**Pros:**
- Eliminates directional risk from market making.
- Enables tighter DEX spreads (since inventory risk is hedged).
- Well-established practice in traditional market making.

**Cons:**
- Requires CEX API integration and capital on both sides.
- Cross-venue settlement latency (Chia finality vs. CEX).
- Operational complexity: two venues, two key-management systems.

---

### 3.2 Volatility Forecasting (GARCH / Realized Volatility)

| Priority | High |
|----------|------|
| **Effort** | Medium |
| **Reference** | Bollerslev (1986), *"Generalized autoregressive conditional heteroskedasticity"*, Journal of Econometrics, 31(3), 307–327. Andersen et al. (2003), *"Modeling and forecasting realized volatility"*, Econometrica, 71(2), 529–626. |

**Description.**
Implement σ forecasting to feed A-S/GLFT models with forward-looking
volatility instead of trailing realized vol.  Options include GARCH(1,1),
EWMA, and realized-volatility estimators on block-level returns.

**Pros:**
- Directly improves spread sizing (wider in high-vol, tighter in low-vol).
- GARCH is standard, well-understood, cheap to compute.
- Block-level returns naturally correspond to our quoting cadence.

**Cons:**
- GARCH assumes return normality; crypto returns are fat-tailed.
- Model selection (GARCH vs. EGARCH vs. GJR) adds complexity.
- Parameter re-estimation needed as market character evolves.

---

### 3.3 Lead-Lag Signals from CEX Price Feeds

| Priority | High |
|----------|------|
| **Effort** | Medium |
| **Reference** | De Jong & Nijman (1997), *"High frequency analysis of lead-lag relationships between financial markets"*, Journal of Empirical Finance, 4(2-3), 259–277. |

**Description.**
CEX prices (Binance, Kraken) typically lead DEX prices by seconds to minutes.
A lead-lag model would consume real-time CEX websockets and preemptively
adjust DEX quotes before on-chain price discovery catches up.

**Pros:**
- "Free" edge — public CEX data reveals future DEX fair value.
- Simple to implement: shift reference price by CEX mid.
- Directly reduces adverse selection (quotes reflect latest info).

**Cons:**
- Requires low-latency CEX websocket integration.
- Lead-lag relationship is time-varying and pair-dependent.
- Regulatory ambiguity around "off-chain informed quoting."
- CEX outages or delistings break the signal.

---

### 3.4 Maker-Taker Fee Optimization

| Priority | Low |
|----------|-----|
| **Effort** | Low |

**Description.**
Optimize venue selection based on maker/taker fee structures.  Route passive
orders to venues with maker rebates and aggressive orders where taker fees are
lowest.

**Pros:**
- Direct PnL improvement from fee differentials.
- Simple routing logic.

**Cons:**
- Chia DEX fees are currently minimal (Dexie 0%, TibetSwap 0.7%).
- Limited venue differentiation in the Chia ecosystem.
- Fee structures may change; hard-coded routing becomes stale.

---

### 3.5 Dynamic Position Limits (VaR / CVaR)

| Priority | Medium |
|----------|--------|
| **Effort** | Medium |
| **Reference** | Artzner et al. (1999), *"Coherent measures of risk"*, Mathematical Finance, 9(3), 203–228. |

**Description.**
Replace static `q_max` position limits with dynamic limits computed from
Value-at-Risk or Conditional VaR.  When volatility spikes, the system
automatically reduces allowable inventory.

**Pros:**
- Risk-adaptive position limits — less capital at risk during stress.
- CVaR captures tail risk better than VaR.
- Directly integrates with volatility forecasting.

**Cons:**
- VaR requires return-distribution estimation (parametric or historical).
- CVaR is harder to compute but more coherent.
- Dynamic limits may cause position-limit churn in volatile markets.

---

### 3.6 Toxic Flow Classification (ML-based)

| Priority | Low |
|----------|-----|
| **Effort** | High |
| **Reference** | Easley, López de Prado & O'Hara (2016), *"Discerning information from trade data"*, Journal of Financial Economics, 120(2), 269–286. |

**Description.**
Train a classifier (logistic regression, gradient-boosted trees, or neural
net) to predict whether an incoming trade is informed or uninformed, using
features: trade size, time-of-day, recent VPIN, OFI, volatility, spread level.

**Pros:**
- More nuanced than VPIN's pure volume-imbalance heuristic.
- Can incorporate many features beyond volume.
- Potentially higher accuracy with sufficient training data.

**Cons:**
- Requires labelled training data (hard to obtain for Chia DEX).
- Model drift: crypto markets are non-stationary.
- Inference latency must fit within the 52-second block cadence.
- Explainability concerns for risk management.

---

### 3.7 Latency Arbitrage Defense

| Priority | Low |
|----------|-----|
| **Effort** | Medium |
| **Reference** | Budish, Cramton & Shim (2015), *"The high-frequency trading arms race"*, Quarterly Journal of Economics, 130(4), 1547–1621. |

**Description.**
Detect when our quotes are stale relative to CEX price moves and preemptively
cancel or widen before a latency arbitrageur takes them.

**Pros:**
- Prevents "stale quote sniping" — a major PnL leak.
- Straightforward implementation if CEX feeds are available.
- Well-studied in traditional markets.

**Cons:**
- Chia's 52-second blocks limit how "stale" a quote can be exploited.
- Requires real-time CEX integration (see §3.3).
- May cause excessive cancellation in volatile markets.
- Chia's mempool is transparent — cancellations race against takers.

---

### 3.8 Multi-Asset Joint Quoting

| Priority | Medium |
|----------|--------|
| **Effort** | High |
| **Reference** | Guéant & Lehalle (2015), *"General intensity shapes in optimal liquidation"*, Mathematical Finance, 25(3), 457–495. |

**Description.**
Jointly optimize quotes across correlated pairs (e.g., XCH/USDT + XCH/wETH)
to account for cross-pair inventory risk and correlation-based hedging.

**Pros:**
- Captures cross-pair risk that single-asset models ignore.
- Enables natural hedging (sell XCH/USDT, buy XCH/wETH if correlated).
- Better capital utilization across pairs.

**Cons:**
- Correlation estimation on sparse DEX data is noisy.
- Optimization dimensionality grows quadratically with pairs.
- Implementation complexity for the quoting engine.

---

### 3.9 Adaptive Market-Making with Reinforcement Learning

| Priority | Low |
|----------|-----|
| **Effort** | Very High |
| **Reference** | Spooner et al. (2018), *"Market making via reinforcement learning"*, Proceedings of AAMAS 2018. Guéant & Manziuk (2019), *"Deep reinforcement learning for market making in corporate bonds"*, arXiv:1906.02312. |

**Description.**
Train an RL agent (DQN, PPO, or actor-critic) that directly maps market state
(inventory, OFI, VPIN, regime, order book) to quote actions (spread, skew,
size).

**Pros:**
- Can discover non-obvious strategies that hand-crafted models miss.
- Adapts continuously as market dynamics change.
- End-to-end optimization of the full strategy pipeline.

**Cons:**
- Requires a high-fidelity simulator — Chia DEX simulator doesn't exist yet.
- Sample efficiency: millions of episodes needed for convergence.
- Reward shaping is tricky (trade-off between PnL, inventory risk, etc.).
- Deployment risk: RL agents can behave unpredictably on out-of-distribution data.
- Regulatory scrutiny around "black box" strategies.

---

### 3.10 Cross-Chain Bridge Arbitrage

| Priority | Low |
|----------|-----|
| **Effort** | High |

**Description.**
Exploit pricing discrepancies between native assets and their wrapped/bridged
counterparts (e.g., native USDT on Ethereum vs. wUSDT on Chia via Portal).

**Pros:**
- Captures structural inefficiencies in bridge pricing.
- Can be very profitable during bridge congestion.

**Cons:**
- Bridge settlement is slow and risky (hours, counterparty risk).
- Requires multi-chain infrastructure and capital.
- Bridge exploits and bugs are common attack vectors.

---

### 3.11 Oracle-Based Fair-Value Anchoring

| Priority | Medium |
|----------|--------|
| **Effort** | Low |

**Description.**
Anchor the mid-price to an external oracle (Chainlink, Pyth, or aggregated
CEX TWAP) rather than computing it solely from DEX order-book mid.  Useful
when the DEX book is thin and easily manipulated.

**Pros:**
- Resistant to on-chain manipulation of the DEX book.
- More accurate fair value on illiquid pairs.
- Simple to implement if oracle is available.

**Cons:**
- Oracle latency and update frequency may lag true price.
- Single-point-of-failure if oracle goes down or is manipulated.
- Chia ecosystem oracle infrastructure is nascent.

---

### 3.12 Quote Stuffing / Spoofing Detection

| Priority | Low |
|----------|-----|
| **Effort** | Medium |
| **Reference** | Aitken et al. (2015), *"Trade-based manipulation and market efficiency"*, Journal of Financial Economics. |

**Description.**
Detect coordinated "wash trading" or spoofing patterns on Chia DEX: rapid
offer creation and cancellation, self-trading, or layered phantom offers
designed to manipulate the book.

**Pros:**
- Protects against being baited by fake liquidity.
- Regulatory goodwill (compliance with anti-manipulation norms).

**Cons:**
- Chia's blockchain transparency makes spoofing somewhat harder.
- Detection heuristics generate false positives.
- Limited regulatory mandate on Chia DEX currently.

---

### 3.13 Impermanent Loss Hedging for AMM LP Positions

| Priority | Medium |
|----------|--------|
| **Effort** | High |
| **Reference** | Milionis et al. (2022), *"Automated market making and loss-versus-rebalancing"*, arXiv:2208.06046. |

**Description.**
When providing liquidity in TibetSwap AMM pools, hedge the impermanent loss
(IL) by maintaining a counter-position.  The hedge ratio depends on the pool's
constant-product curve and the current price deviation from entry.

**Pros:**
- Enables LP participation without full IL exposure.
- Combines fee income from AMM with delta-hedged book income.

**Cons:**
- Hedging cost may exceed IL benefit on low-fee pools.
- Requires continuous hedge rebalancing (slow on 52-second blocks).
- IL model assumes constant-product — non-standard pools need different math.

---

### 3.14 Batch Auction Strategies (Frequent Batch Auctions)

| Priority | Low |
|----------|-----|
| **Effort** | High |
| **Reference** | Budish, Cramton & Shim (2015), *"The high-frequency trading arms race"*, QJE, 130(4). |

**Description.**
If Chia DEX ever adopts batch auctions (clearing all trades in a block at a
single price), our strategy would need to shift from continuous quoting to
sealed-bid, batch-optimal order submission.

**Pros:**
- Eliminates latency advantage entirely.
- Levels the playing field vs. faster participants.

**Cons:**
- Chia DEX does not currently use batch auctions.
- Requires game-theoretic bid optimization (different from continuous MM).
- Highly speculative at this stage.

---

### 3.15 Market Microstructure Invariance

| Priority | Low |
|----------|-----|
| **Effort** | Medium |
| **Reference** | Kyle & Obizhaeva (2016), *"Market microstructure invariance: Empirical hypotheses"*, Econometrica, 84(4), 1345–1404. |

**Description.**
Uses the invariance hypothesis — that the distribution of individual trade
value is invariant across markets when normalized by the "trading activity"
metric — to calibrate whale thresholds and VPIN bucket sizes automatically
from cross-sectional market data.

**Pros:**
- Principled way to set thresholds without ad-hoc tuning.
- Cross-sectional calibration from equities/crypto markets.

**Cons:**
- Invariance hypothesis is debated in the literature.
- Chia DEX microstructure may differ substantially from equities.
- Complex calibration procedure for uncertain benefit.

---

## 4. Strategy Interaction Matrix

How the implemented strategies compose and interact:

| Strategy | Feeds Into | Fed By |
|----------|-----------|--------|
| Regime Detection | A-S/GLFT (σ, skew multipliers), Spread Optimizer | Price history |
| A-S / GLFT | Final quote prices | Regime, Volatility |
| Spread Optimizer | Base spread (bps) | Competition, Regime |
| Whale Detection | Spread multiplier, Dominant side | Trade feed |
| VPIN | Toxicity multiplier | Trade feed |
| OFI | Directional multiplier | Order-book snapshots |
| Asymmetric Widening | Bid/ask split multipliers | Whale detection |
| Competitor Detection | Competition component | Offer-book snapshots |
| Multi-Tier Liquidity | Offer ladder | Base spread, Multipliers |
| Thompson Sampling | Spread grid learning | Fill/timeout outcomes |
| Arbitrage | Execution signals | CEX/DEX price feeds |

**Composition formula (full pipeline):**

```
// Base spread from the optimizer
base_bps = spread_optimizer.compute(regime, volatility, competition);

// Signal multipliers
vpin_mult = 1.0 + vpin * max_vpin_widening;
ofi_mult  = 1.0 + abs(ofi) * ofi_sensitivity;

// Asymmetric whale widening (already includes whale symmetric mult)
asym = get_asymmetric_spread_multipliers(pair);

// Final per-side spreads
final_bid_spread = base_bps * vpin_mult * ofi_mult * asym.bid_multiplier;
final_ask_spread = base_bps * vpin_mult * ofi_mult * asym.ask_multiplier;
```

---

## 5. References

1. Agrawal, S. & Goyal, N. (2012). "Analysis of Thompson sampling for the multi-armed bandit problem." *COLT 2012*.

2. Aitken, M. et al. (2015). "Trade-based manipulation and market efficiency: A cross-market comparison." *Journal of Financial Economics*.

3. Amihud, Y. & Mendelson, H. (1986). "Asset pricing and the bid-ask spread." *Journal of Financial Economics*, 17(2), 223–249.

4. Andersen, T. G. et al. (2003). "Modeling and forecasting realized volatility." *Econometrica*, 71(2), 529–626.

5. Artzner, P. et al. (1999). "Coherent measures of risk." *Mathematical Finance*, 9(3), 203–228.

6. Avellaneda, M. & Stoikov, S. (2008). "High-frequency trading in a limit order book." *Quantitative Finance*, 8(3), 217–224.

7. Bollerslev, T. (1986). "Generalized autoregressive conditional heteroskedasticity." *Journal of Econometrics*, 31(3), 307–327.

8. Brunnermeier, M. K. & Pedersen, L. H. (2005). "Predatory trading." *Journal of Finance*, 60(4), 1825–1863.

9. Budish, E., Cramton, P. & Shim, J. (2015). "The high-frequency trading arms race." *Quarterly Journal of Economics*, 130(4), 1547–1621.

10. Cartea, Á., Jaimungal, S. & Penalva, J. (2015). *Algorithmic and High-Frequency Trading*. Cambridge University Press.

11. Cont, R., Kukanov, A. & Stoikov, S. (2014). "The price impact of order book events." *Quantitative Finance*, 14(1), 109–126.

12. De Jong, F. & Nijman, T. (1997). "High frequency analysis of lead-lag relationships between financial markets." *Journal of Empirical Finance*, 4(2-3), 259–277.

13. Easley, D., Kiefer, N. M., O'Hara, M. & Paperman, J. (1996). "Liquidity, information, and infrequently traded stocks." *Journal of Finance*, 51(4), 1405–1436.

14. Easley, D., López de Prado, M. & O'Hara, M. (2012). "Flow toxicity and liquidity in a high-frequency world." *Review of Financial Studies*, 25(5), 1457–1493.

15. Easley, D., López de Prado, M. & O'Hara, M. (2016). "Discerning information from trade data." *Journal of Financial Economics*, 120(2), 269–286.

16. Easley, D. & O'Hara, M. (1992). "Time and the process of security price adjustment." *Journal of Finance*, 47(2), 577–605.

17. Guéant, O. (2017). "Optimal market making." *Applied Mathematical Finance*, 24(2), 112–154.

18. Guéant, O. & Lehalle, C.-A. (2015). "General intensity shapes in optimal liquidation." *Mathematical Finance*, 25(3), 457–495.

19. Guéant, O., Lehalle, C.-A. & Fernandez-Tapia, J. (2013). "Dealing with the inventory risk: a solution to the market making problem." *Mathematics and Financial Economics*, 7(4), 477–507.

20. Guéant, O. & Manziuk, I. (2019). "Deep reinforcement learning for market making in corporate bonds." *arXiv:1906.02312*.

21. Guilbaud, F. & Pham, H. (2013). "Optimal high-frequency trading with limit and market orders." *Quantitative Finance*, 13(1), 79–94.

22. Hamilton, J. D. (1989). "A new approach to the economic analysis of nonstationary time series." *Econometrica*, 57(2), 357–384.

23. Ho, T. & Stoll, H. R. (1981). "Optimal dealer pricing under transactions and return uncertainty." *Journal of Financial Economics*, 9(1), 47–73.

24. Kyle, A. S. (1985). "Continuous auctions and insider trading." *Econometrica*, 53(6), 1315–1335.

25. Kyle, A. S. & Obizhaeva, A. A. (2016). "Market microstructure invariance: Empirical hypotheses." *Econometrica*, 84(4), 1345–1404.

26. Lo, A. W. & MacKinlay, A. C. (1988). "Stock market prices do not follow random walks." *Review of Financial Studies*, 1(1), 41–66.

27. Milionis, J. et al. (2022). "Automated market making and loss-versus-rebalancing." *arXiv:2208.06046*.

28. Shleifer, A. & Vishny, R. W. (1997). "The limits of arbitrage." *Journal of Finance*, 52(1), 35–55.

29. Spooner, T. et al. (2018). "Market making via reinforcement learning." *Proceedings of AAMAS 2018*.

30. Stoll, H. R. (1978). "The supply of dealer services in securities markets." *Journal of Finance*, 33(4), 1133–1151.

31. Thompson, W. R. (1933). "On the likelihood that one unknown probability exceeds another." *Biometrika*, 25(3/4), 285–294.
