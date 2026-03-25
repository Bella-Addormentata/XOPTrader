# XOPTrader — Trading Strategy Catalog

> Comprehensive inventory of implemented, considered, and future strategies for
> Chia DEX market making.  Each entry includes a brief description, pros/cons,
> scholarly references where applicable, and current implementation status.

---

## Table of Contents

0. [Goals and Objectives](#0-goals-and-objectives)
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
   - [3.16 Strategic Loss-Taking for Inventory Rebalancing](#316-strategic-loss-taking-for-inventory-rebalancing)
   - [3.17 Offer Time-to-Live (TTL) Optimization](#317-offer-time-to-live-ttl-optimization)
   - [3.18 Chia Block-Cadence Synchronization](#318-chia-block-cadence-synchronization)
   - [3.19 CAT Ecosystem Correlation Trading](#319-cat-ecosystem-correlation-trading)
   - [3.20 Chialisp Programmable Offer Conditions](#320-chialisp-programmable-offer-conditions)
   - [3.21 Blockchain Transparency Competitor Intelligence](#321-blockchain-transparency-competitor-intelligence)
4. [Chia-Specific Competitive Advantages](#4-chia-specific-competitive-advantages)
5. [On Deliberate Loss-Taking and Inventory Balance](#5-on-deliberate-loss-taking-and-inventory-balance)
6. [Implementation Priority Ranking](#6-implementation-priority-ranking)
7. [Key Strategy Trade-offs](#7-key-strategy-trade-offs)
   - [7.10 Coexisting With Unknown Market Makers](#710-coexisting-with-unknown-market-makers)
8. [Strategy Interaction Matrix](#8-strategy-interaction-matrix)
9. [Strategy Selection Rationale & Confidence Assessment](#9-strategy-selection-rationale--confidence-assessment)
10. [References](#10-references)

---

## 0. Goals and Objectives

This section defines *what we are trying to achieve* with XOPTrader and *how
we measure success*.  Every strategy decision — which to implement, in what
order, and when to accept a loss — should be evaluated against these goals.

### 0.1 Primary Goal

**Maximize risk-adjusted spread income over time on Chia DEX.**

This is a *flow* goal, not a directional one.  XOPTrader is a market maker,
not a speculator.  Success is measured by cumulative spread captured per unit
of capital at risk, not by the direction of any single trade.

The core model:
```
Risk-adjusted PnL = Σ(spread captured per fill) − Σ(adverse selection losses)
                  − Σ(inventory drift losses) − Σ(operational costs)
```

### 0.2 Secondary Goals

| Goal | Why It Matters |
|------|---------------|
| **Maintain two-sided quoting** | Single-sided quoting earns zero spread on the missing side and exposes the other side to 100% adverse selection |
| **Preserve capital during stress** | A market maker who loses their capital base cannot quote; survival is prerequisite to profitability |
| **Minimize adverse selection** | Informed counterparties systematically extract value; detecting and avoiding them is essential |
| **Exploit Chia-specific edges** | The unique features of Chia DEX (§4) give structural advantages that centralized-market strategies miss |
| **Operate continuously and reliably** | Uptime = quoting time = income; every offline minute is missed opportunity and potential stale-quote risk |

### 0.3 Success Metrics

The following KPIs define a "healthy" deployment:

| Metric | Target | Notes |
|--------|--------|-------|
| **Fill Rate** | >30% of posted offers filled | Too low → spreads are too wide; too high → spreads are too narrow |
| **Inventory Imbalance** | \|q / q_max\| < 0.6 at all times | 0.60–0.75: intensify skew; >0.75: trigger deliberate rebalancing (§5) |
| **Realized Spread** | >50% of quoted spread | Measures adverse selection leakage |
| **Uptime** | >99% of blocks | Per-block heartbeat enables this; see block-cadence sync §3.18 |
| **Drawdown** | <10% of deployed capital / month | Circuit breaker if exceeded |
| **Capital Efficiency** | >60% of capital generating fills within 24 h | Idle capital is wasted risk capacity |

### 0.4 Implementation Milestones

The strategies are not intended to be deployed all at once.  The following
phased milestones provide a concrete road map from the current state to a
fully optimized deployment.  Each milestone is self-contained and improves
PnL incrementally.

| Milestone | Strategies to Add | Expected Impact |
|-----------|------------------|-----------------|
| **M1 — Operational Hardening** | §3.18 Block-cadence sync, §3.17 TTL optimization | Eliminates cancel/fill races; reduces stale-quote losses |
| **M2 — Inventory Discipline** | §3.16 Strategic loss-taking, §3.5 Dynamic position limits | Prevents inventory exhaustion; keeps two-sided quoting alive |
| **M3 — Volatility Awareness** | §3.2 GARCH volatility forecasting | Feeds better σ into A-S/GLFT; spread sizing tracks real-time risk |
| **M4 — CEX Signal Integration** | §3.3 Lead-lag CEX feeds, §3.11 Oracle anchoring | Reduces adverse selection from CEX-to-DEX price discovery lag |
| **M5 — Competitor Intelligence** | §3.21 Blockchain transparency intel | Shifts competitor response from reactive to predictive |
| **M6 — Portfolio Expansion** | §3.19 CAT correlation trading, §3.8 Multi-asset quoting | Diversifies income; reduces single-pair concentration risk |
| **M7 — Advanced Risk** | §3.1 CEX inventory hedging, §3.13 IL hedging | Eliminates residual directional risk; enables tighter spreads |

### 0.5 What We Have Not Implemented Yet (And Why)

The 21 strategies in §3 are all documented but not yet in code.  The most
impactful ones to implement *next* are identified in the priority ranking (§6).
The "Always Defer" category (end of §6) represents strategies where the
expected value under Chia's specific constraints is insufficient to justify
the engineering cost at this stage.

A strategy being "not implemented" is not a failure — it is a deliberate
choice to apply engineering resources where the marginal PnL improvement is
highest.  The catalog exists so that future contributors can find the right
strategy when conditions change (e.g., a richer CAT ecosystem makes §3.8
more valuable; Chialisp tooling maturing makes §3.20 practical).

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

### 3.16 Strategic Loss-Taking for Inventory Rebalancing

| Priority | **High** |
|----------|----------|
| **Effort** | Low |

**Description.**
Accept a below-reservation-price fill (or post an intentionally under-priced
offer) when one inventory side becomes dangerously skewed.  Taking a small,
controlled loss now prevents a much larger loss from holding a lopsided book
into a sustained directional move.

The rebalancing trigger is an inventory ratio threshold:

```
imbalance_ratio = |q| / q_max

if imbalance_ratio > threshold_rebalance:
    post one-shot offer at mid ± δ_rebalance   // δ_rebalance < normal half-spread
    accept fill at a known loss to restore q toward zero
```

Typical values: `threshold_rebalance = 0.75`, `δ_rebalance ≈ 0.5 × normal_spread`.

**When taking a deliberate loss is rational:**

| Situation | Rationale |
|-----------|-----------|
| Inventory at 75 %+ of limit | Prevents quoting only one side and missing all flow |
| Directional signal detected (OFI / VPIN spike) | Lock in a small loss before the market moves further against the skew |
| Time-sensitive capital release | Free the stuck position to redeploy into a higher-return opportunity |
| Tax-loss harvesting window | Crystallise a tax loss at end of fiscal period, immediately re-enter |
| Offer-book deadlock | Both sides have stale offers; cancel and post fresh at a tighter loss |

**Pros:**
- Keeps both bid and ask quoted simultaneously — the fundamental requirement of market making.
- Small, planned loss is less damaging than accumulating an unhedged directional position.
- Pairs naturally with GLFT/A-S inventory penalty: when the model says widen, also consider just rebalancing.
- Tax-efficient if timed to fiscal periods.

**Cons:**
- Explicit PnL hit per rebalancing event; must be tracked and minimized.
- Predictable rebalancing behaviour can be exploited by observers watching the on-chain book.
- Sizing the rebalancing offer (how far below mid) requires calibration.
- Too-frequent rebalancing turns the strategy into a taker strategy rather than a maker strategy.

---

### 3.17 Offer Time-to-Live (TTL) Optimization

| Priority | **Medium** |
|----------|------------|
| **Effort** | Low |

**Description.**
Unlike traditional limit orders, Chia offers have **no native expiry**.  A
signed offer file posted on Dexie remains valid until cancelled via an on-chain
transaction.  This is both an advantage and a risk:

- **Advantage:** An offer can be broadcast to many venues simultaneously; the
  first taker fills it, all other copies become automatically invalid (the coin
  is already spent).
- **Risk:** A stale offer posted during a low-volatility period may be sniped
  by an arbitrageur when price moves while our bot is offline.

TTL optimization decides the offer's *intended* lifetime and schedules an
on-chain cancellation if no fill occurs.  The cost is one cancellation
transaction per expired offer, so TTL must be balanced against fee cost.

```
optimal_ttl = f(spread, volatility, fill_rate, cancellation_fee_cost)

if volatility_high:
    ttl = 2–3 blocks    // refresh aggressively to avoid adverse selection
else:
    ttl = 10–20 blocks  // reduce cancellation cost
```

**Pros:**
- Dramatically reduces stale-quote adverse selection on Chia.
- Chia-specific: CEX and Ethereum DEX offer native order expiry; this is a
  unique operational concern and opportunity on Chia.
- Short TTL in high-vol regimes combined with wide spreads covers both sides
  of the risk.

**Cons:**
- Every cancellation is an on-chain transaction (small cost and 52-second latency).
- No native TTL means our bot must be reliably online to cancel.
- Race condition: cancellation and fill may land in the same block.

---

### 3.18 Chia Block-Cadence Synchronization

| Priority | **Medium** |
|----------|------------|
| **Effort** | Low |

**Description.**
Chia produces blocks every ~52 seconds on average.  All state changes
(fills, cancellations, new offers) take effect at block boundaries.  Using
wall-clock timers for strategy refresh introduces a random phase offset against
block finality.

Block-cadence synchronization subscribes to the Chia node's `new_peak`
websocket event and fires strategy evaluation only on confirmed new peaks.
This eliminates the class of race conditions where a refresh transaction
lands in the same block as a fill transaction on the same coin.

**Benefits over wall-clock timers:**
| Clock Type | Risk |
|------------|------|
| Wall-clock | Refresh lands mid-block; cancel/fill conflict possible |
| Block-cadence | Refresh fires after finality; state is deterministic |

Chia's per-block heartbeat is already the architectural basis of XOPTrader's
`boost::asio` loop (see `cpp/src/execution/market_data.cpp`); this strategy
codifies the best-practice configuration of that loop.

**Pros:**
- Eliminates cancel/fill race conditions inherent to wall-clock polling.
- Natural cadence for all strategy signals (VPIN buckets, OFI snapshots,
  regime re-evaluation all align to block boundaries).
- Reduces unnecessary CPU wake-ups (52-second sleep vs. 1-second polling).
- Unique to Chia: Ethereum and Solana have sub-second block times where this
  distinction matters less.

**Cons:**
- 52-second minimum reaction latency; cannot respond faster than one block.
- Empty blocks (no transactions) still fire the `new_peak` event — need to
  handle gracefully.
- Block times are not perfectly regular; `new_peak` can arrive early or late.

---

### 3.19 CAT Ecosystem Correlation Trading

| Priority | **Medium** |
|----------|------------|
| **Effort** | Medium |
| **Reference** | Engle & Granger (1987) [32]. |

**Description.**
The Chia CAT ecosystem contains 50+ tokens.  Many are correlated with XCH
(which underlies all blockchain operations) and with each other through
shared liquidity providers and market conditions.  Exploiting these correlations
enables:

1. **Cross-CAT leading indicators** — if a heavily-traded CAT moves first,
   use that signal to anticipate XCH or other CAT moves.
2. **Statistical arbitrage** — identify cointegrated pairs; trade the spread
   when it deviates from long-run equilibrium.
3. **Hedged CAT market making** — when accumulating a risky CAT through
   market making, hedge the resulting exposure using correlated XCH positions.

```
// Pair score for stat-arb
z_score = (price_A / price_B - mean_ratio) / std_ratio

if z_score > 2.0:
    sell A, buy B   // ratio too high
if z_score < -2.0:
    buy A, sell B   // ratio too low
```

**Chia-specific advantage:** The entire CAT order book is publicly visible
on-chain.  Cross-asset correlation data can be computed directly from
blockchain history without relying on a third-party exchange API.

**Pros:**
- Diversifies income across the Chia ecosystem rather than depending on
  a single pair.
- Statistical arbitrage is genuinely market-neutral when the hedge is live.
- Cointegration-based signals have lower false-positive rates than simple
  price-level correlations.
- Unique data source: full on-chain offer history with no API rate limits.

**Cons:**
- Correlation structure is unstable in crypto markets.
- Thin CAT markets amplify mean-reversion noise; signal-to-noise ratio is low.
- Requires multi-asset position tracking, increasing operational complexity.
- Transaction costs on Chia are low but non-zero; tight correlations may not
  be profitable after costs.

---

### 3.20 Chialisp Programmable Offer Conditions

| Priority | **Low (research stage)** |
|----------|--------------------------|
| **Effort** | High |

**Description.**
Chialisp is a Turing-complete on-chain programming language native to Chia.
Every coin's spending conditions are expressed as a Chialisp puzzle.  Advanced
market makers could embed strategic conditions directly into offer puzzles:

| Condition Type | Description |
|----------------|-------------|
| **Time-lock expiry** | Offer invalid after block N — native TTL without a cancel transaction |
| **Rate limiting** | Offer fillable at most once per N blocks |
| **Conditional pricing** | Fill price adjusts based on an on-chain oracle coin's state |
| **Batch-fill guard** | Reject fills that would move inventory past a limit |
| **Atomic multi-leg** | Single puzzle that fills two correlated offers simultaneously or neither |

This approach eliminates the cancel-transaction cost for TTL optimization
(§3.17) and enables strategies with no equivalent on any other DEX platform.

**Pros:**
- Unique competitive moat — no other DEX ecosystem offers native
  programmable offer conditions.
- Time-lock expiry removes cancel cost and cancel/fill race condition entirely.
- Conditional pricing could enable fully trustless dynamic spreads.
- Atomic multi-leg is impossible on offer-based DEXs without Chialisp.

**Cons:**
- Requires deep Chialisp expertise and significant engineering effort.
- Complex puzzles increase the smart-contract security audit burden.
- Tooling for Chialisp development and testing is less mature than Solidity.
- Counterparties (takers) must be able to introspect and trust the puzzle,
  which may limit adoption on public offer boards.

---

### 3.21 Blockchain Transparency Competitor Intelligence

| Priority | **Medium** |
|----------|------------|
| **Effort** | Medium |

**Description.**
Chia's fully public blockchain means every offer ever posted, filled, or
cancelled is permanently on-chain.  Unlike opaque CEX order books where
only the current snapshot is visible, XOPTrader can reconstruct the complete
historical action of every competitor:

- When do competitors refresh their offers? (Identify their refresh cadence)
- What spread levels do they target? (Map their pricing strategy)
- Do they widen spreads during whale events? (Detect if they have whale detection)
- Which price levels do they abandon first during volatile periods?
- Do they cluster their offers at round-number prices? (Exploit round-number bias)

Building a competitor behavior model enables *predictive* rather than
*reactive* competitor response — going beyond the current reactive
`CompetitorDetection` module (§1.7).

```
// Competitor behavior model update per block
for each competitor offer (offer_coin_id):
    if offer_cancelled and not filled:
        record(competitor_id, cancel_block, price, spread)
    if offer_filled:
        record(competitor_id, fill_block, price, spread, fill_direction)

// Predict next competitor action
if competitor_avg_ttl = 5 blocks and current_ttl = 4:
    expect competitor refresh next block
    → tighten spread temporarily to capture flow before their refresh
```

**Pros:**
- Chia-specific advantage — this level of competitor transparency is
  unavailable on CEX or Ethereum DEX (private order history, API rate limits).
- Enables "first mover" positioning ahead of predictable competitor refreshes.
- Historical competitor data requires no third-party API — pure on-chain.
- Directly extends the existing CompetitorDetection infrastructure.

**Cons:**
- Competitors can also analyze our behavior — arms-race dynamics.
- Privacy-preserving competitors can randomize timing and amounts to
  defeat behavioral fingerprinting.
- Requires blockchain indexing infrastructure to process offer history at scale.
- Model accuracy degrades when competitors change strategies.

---

## 4. Chia-Specific Competitive Advantages

This section catalogues the structural features of the Chia DEX environment
that XOPTrader can uniquely exploit.  Understanding these advantages informs
which strategies to prioritize (§6) and how to design them.

### 4.1 Offer File Portability — "Post Once, Fill Anywhere"

A Chia offer file is a signed, self-contained cryptographic artifact.  The
**same file** can be submitted to Dexie, OfferBin, Splash, a community Discord,
or a direct counterparty simultaneously.  Capital is **not fragmented** across
venues — the underlying coin is locked once and is filled by whoever takes the
offer first.  All other copies become invalid automatically.

**Market-making implication:** A single offer effectively has the liquidity
aggregated across all distribution channels.  A traditional DEX or CEX maker
must split capital across venues or use a smart-order-router.  On Chia, the
offer file *is* the smart-order-router.

**Exploit:** Maximize distribution of offer files to reach the widest
possible audience and improve fill rates without multiplying capital
requirements or increasing inventory risk.

---

### 4.2 Deterministic 52-Second Block Cadence

Chia produces one block approximately every 52 seconds (configurable via
the `SUB_SLOT_ITERS` constant).  Unlike Ethereum's variable block times
or Solana's sub-second blocks, Chia's cadence is predictable and slow enough
for a market maker to reason about within one block window.

**Market-making implication:** The block cadence is a natural heartbeat for
all strategy signals (VPIN bucket completion, OFI snapshot, regime re-evaluation).
Synchronizing to `new_peak` events (§3.18) eliminates an entire class of
race conditions.

---

### 4.3 No Gas Wars — Flat, Minimal Fees

Chia's transaction fees are small (currently fractions of a mojo) and are
not subject to EIP-1559-style gas auctions.  There is no "fee market" where
participants bid against each other for block inclusion.

**Market-making implication:**
- Cancellation is cheap — TTL optimization (§3.17) is economical.
- Aggressive rebalancing (§3.16) has negligible transaction overhead.
- Cancel/replace cycles can be frequent without eroding PnL.

On Ethereum, each cancel and replace costs real gas; makers must hold offers
longer to amortize fees, creating "sticky" quotes.  XOPTrader faces no
such constraint.

---

### 4.4 Atomic Trustless Settlement — No Counterparty Risk

Chia offers are trustless atomic swaps.  Either the exact amounts specified
in the offer change hands atomically, or nothing happens.  There is no:

- Settlement failure risk
- Partial fill risk (a taker takes only part of the offer)
- Counterparty default risk
- Custodial intermediary

**Market-making implication:** The maker can quote aggressively and at tight
spreads without worrying about settlement risk.  The spread need not include
a "counterparty risk premium" that traditional OTC market makers charge.
All risk-adjusted spread components (§1.3) can focus purely on market risk
and adverse selection, not counterparty risk.

---

### 4.5 Full UTXO Transparency — Complete Competitor Visibility

Every Chia offer is an on-chain UTXO.  The full history of all offers
(created, filled, cancelled) is publicly readable by any full node without
any API key, rate limit, or data subscription.

**Market-making implication:** The foundation for Blockchain Transparency
Competitor Intelligence (§3.21).  XOPTrader can build a complete,
real-time view of every competitor's offer book, fill history, and
pricing strategy — a data advantage unavailable on any CEX or
privacy-preserving DEX.

---

### 4.6 CAT Ecosystem Depth — 50+ Tokens With Variable Liquidity

The Chia CAT ecosystem comprises 50+ tokens ranging from well-known
(Stably USDS, wBTC, wETH) to niche community tokens.  Liquidity varies
enormously across pairs.

**Market-making implication:**
- **Illiquid pairs** offer wider natural spreads — higher per-trade
  profit at the cost of lower fill rates.
- **Liquid pairs** (XCH/USDC) offer tight spreads but high fill rates
  and volume.
- A diversified multi-pair strategy (§3.8) can allocate capital across
  the liquidity spectrum to optimize the risk/return trade-off.
- CAT Correlation Trading (§3.19) exploits the structural ties between
  XCH and tokens whose value is partly denominated in XCH.

---

### 4.7 No MEV (Miner/Maximal Extractable Value)

Chia's proof-of-space-and-time consensus does not allow farmers (Chia's
equivalent of miners) to freely reorder or insert transactions within a
block.  The coin-spending puzzle model and the farming reward structure
prevent the front-running MEV strategies common on Ethereum (sandwich
attacks, JIT liquidity attacks).

**Market-making implication:**
- Our offers cannot be sandwiched by a farmer who sees our cancel in
  the mempool.
- There is no "dark pool" advantage for well-connected nodes.
- The competitive playing field is closer to flat: execution quality
  depends on strategy and speed, not miner bribery.

---

### 4.8 Peer-to-Peer Offer Distribution

Offer files can be shared via any communication channel: email, Discord,
Telegram, a website, or a QR code.  They require no platform intermediary
and cannot be censored at the chain level.

**Market-making implication:** Direct large counterparty relationships are
possible.  A market maker could run a private OTC desk for institutional
buyers, offering bespoke pricing without listing on any public DEX.

---

## 5. On Deliberate Loss-Taking and Inventory Balance

This section addresses two questions raised in the issue: *Is it ever a
good strategy to take a loss on a position in order to achieve a different
goal?* and *Would holding too long make the balances uneven?*

### 5.1 When Is It Rational to Take a Deliberate Loss?

**Yes — taking a deliberate loss is often the correct strategic choice.**
The key insight is that market making is a *flow* business, not a *directional*
business.  The goal is to maximize total spread income over time, not to win
any individual trade.  A deliberate loss that restores the conditions for
profitable quoting has positive expected value.

#### Scenario 1: Inventory Rebalancing (Most Common)

When one side of the book is exhausted, the maker can only quote in one
direction.  A single-sided market maker earns zero spread on the missing side
and bears the full adverse-selection risk of the side that remains.

```
Example:
Starting state:  50 XCH + $2,500 USDC   (balanced)
After bull run:   2 XCH + $5,400 USDC   (all sells filled; no XCH to sell)

Options:
A) Wait: quote only buy offers; earn zero sell-side spread; risk continued
         price rise making the buy quotes progressively deeper out-of-money.
B) Rebalance: buy 25 XCH at market (slightly above mid); incur ~0.3% loss
              vs. mid; restore book; resume two-sided quoting immediately.

Expected value of B > Expected value of A when:
   spread_income_restored × expected_fill_rate × time_horizon
   > rebalancing_loss_amount
```

The A-S and GLFT models already model this mathematically: both produce a
*reservation price* skewed away from the excess-inventory side to encourage
natural rebalancing.  Strategic loss-taking is the manual equivalent when
natural rebalancing is too slow.

#### Scenario 2: Avoiding Larger Adverse-Selection Losses

When VPIN or OFI signals a sustained directional move, holding a large
inventory position on the wrong side will generate a series of fills at
progressively worse prices.  Closing out early at a known small loss
is superior to absorbing the full adverse move.

```
VPIN = 0.85 (highly toxic flow)
OFI  = +0.7 (strong buy pressure)
Our inventory: -30 XCH (short XCH — we've been selling into a rising market)

If we continue quoting:
   Expected loss = 30 XCH × expected_price_move × P(continued_rise)

If we close now at mid + δ (paying up to exit):
   Known loss = δ × 30 XCH

When VPIN = 0.85 and price_move > δ is likely: close now.
```

#### Scenario 3: Opportunity Cost Arbitrage

Capital locked in a stale, idle position earns nothing.  If a better
opportunity exists (new pair launch with wide spreads, temporary market
dislocation), exiting the stale position at a small loss to redeploy
capital can be net positive.

#### Scenario 4: Tax-Loss Harvesting

In jurisdictions where capital gains tax applies, crystallising an
unrealised loss before year-end and immediately re-entering provides a
tax benefit that may exceed the transaction cost of the exit and re-entry.
The *economic* position is unchanged (same inventory before and after)
but the *tax* position is improved.

---

### 5.2 Does Holding Too Long Make Balances Uneven?

**Yes — this is one of the most important practical risks in market making.**

#### The Inventory Drift Problem

In a market making system with symmetric spreads:

- Each fill moves inventory by ±1 unit.
- In a random walk, expected inventory after T fills is 0, but the
  *standard deviation* grows as √T.
- In a trending market, every fill is on the same side: inventory
  drifts linearly (not just as √T).

After N one-sided fills without rebalancing:
```
Bull market:  q ≈ −N  (sold N units; little XCH remains to sell)
Bear market:  q ≈ +N  (bought N units; little USDC remains to buy)
```

The maker eventually reaches a position where:
1. **One offer type cannot be posted** — insufficient inventory.
2. **The remaining quotes face 100 % adverse selection** — only
   informed traders who know the direction will take them.
3. **The spread captured on past fills is lost** to the subsequent
   adverse move on the accumulated position.

#### The Compounding Risk

Holding too long not only creates imbalance but *accelerates* the problem:

| Time | State | Risk |
|------|-------|------|
| T=0  | Balanced book | Normal quoting, two-sided |
| T=10 | Moderate imbalance (q = 0.6 × q_max) | A-S/GLFT naturally widens quotes |
| T=20 | Heavy imbalance (q = 0.9 × q_max) | Can only quote one side at wide spread |
| T=30 | Exhausted (q = q_max) | Zero quoting ability; full directional risk |

At T=30 the maker has transformed from a market maker into an involuntary
directional speculator — the worst possible outcome.

#### The Rebalancing Policy

The practical remedy is a tiered rebalancing policy:

| Imbalance Ratio | Action |
|-----------------|--------|
| 0 – 0.50 | Normal quoting; A-S/GLFT natural skew handles it |
| 0.50 – 0.75 | Intensify skew; post more aggressive one-sided offers |
| 0.75 – 0.90 | Active rebalancing: post a deliberate loss offer (§3.16) |
| > 0.90 | Emergency rebalancing: cancel all offers; market-buy/sell to target |

This policy ensures the maker never reaches the "involuntary speculator"
state while keeping deliberate losses small and infrequent.

---

## 6. Implementation Priority Ranking

The following table ranks all strategies — implemented (§1), considered (§2),
and future (§3) — in order of recommended implementation priority.  Priority
reflects expected value per unit of engineering effort in the context of
Chia's specific DEX environment.

### Tier 1 — Implement Soon (High Value, Low–Medium Effort)

| Rank | Strategy | Effort | Key Benefit |
|------|----------|--------|-------------|
| 1 | 3.18 Block-Cadence Synchronization | Low | Eliminates cancel/fill race conditions; operationally critical |
| 2 | 3.17 Offer TTL Optimization | Low | Reduces stale-quote adverse selection; Chia-specific edge |
| 3 | 3.16 Strategic Loss-Taking for Inventory Rebalancing | Low | Prevents inventory exhaustion; enables sustained two-sided quoting |
| 4 | 3.2 Volatility Forecasting (GARCH) | Medium | Directly improves A-S/GLFT spread sizing; well-understood model |
| 5 | 3.3 Lead-Lag CEX Signals | Medium | "Free" alpha; directly reduces adverse selection from CEX-to-DEX price discovery lag |

### Tier 2 — Implement Next (Medium Value, Medium Effort)

| Rank | Strategy | Effort | Key Benefit |
|------|----------|--------|-------------|
| 6 | 3.11 Oracle-Based Fair-Value Anchoring | Low | Prevents DEX book manipulation; improves mid-price accuracy on thin pairs |
| 7 | 3.21 Blockchain Transparency Competitor Intelligence | Medium | Predictive competitor modeling; extends existing CompetitorDetection infrastructure |
| 8 | 3.5 Dynamic Position Limits (VaR/CVaR) | Medium | Risk-adaptive inventory limits; integrates naturally with volatility forecasting |
| 9 | 3.19 CAT Ecosystem Correlation Trading | Medium | Unique Chia data advantage; diversified income across token ecosystem |
| 10 | 2.3 Cartea-Jaimungal Alpha Signal | Medium | Formalises OFI into explicit next-price predictor; builds on existing OFI infrastructure |

### Tier 3 — Consider After Tier 1–2 (Medium–High Value, Higher Effort)

| Rank | Strategy | Effort | Key Benefit |
|------|----------|--------|-------------|
| 11 | 3.7 Latency Arbitrage Defense | Medium | Prevents stale-quote sniping; pairs well with TTL optimization and CEX feeds |
| 12 | 3.1 Inventory-Aware CEX Hedging | High | Eliminates directional inventory risk; enables tighter spreads |
| 13 | 3.8 Multi-Asset Joint Quoting | High | Cross-pair risk awareness; better capital utilization |
| 14 | 3.13 IL Hedging for AMM LP | High | Enables fee-earning LP participation without full IL exposure |
| 15 | 2.4 Kyle Lambda | Medium | Principled trade-impact model; improves large-trade spread sizing |

### Tier 4 — Defer (Lower Priority or Speculative)

| Rank | Strategy | Effort | Reason to Defer |
|------|----------|--------|-----------------|
| 16 | 3.6 Toxic Flow ML | High | VPIN already covers this; labelled training data scarce on Chia DEX |
| 17 | 3.12 Spoofing Detection | Medium | Limited manipulation risk on transparent Chia blockchain |
| 18 | 3.4 Maker-Taker Fee Optimization | Low | Minimal fee differentiation in current Chia ecosystem |
| 19 | 3.15 Market Microstructure Invariance | Medium | Uncertain benefit; Chia microstructure differs from equities |
| 20 | 3.20 Chialisp Programmable Offer Conditions | High | High-value long-term but requires deep Chialisp expertise |

### Always Defer / Likely Won't Implement

| Strategy | Reason |
|----------|--------|
| 2.1 Bayesian PIN | VPIN was designed as a real-time PIN proxy; PIN adds complexity without proportionate benefit |
| 2.2 Predatory Trading Defense | Whale detection + asymmetric widening provides adequate partial defense |
| 2.5 Guilbaud-Pham SOC | HJB PDE numerically intensive; complexity/benefit ratio poor on thin Chia DEX |
| 3.9 Reinforcement Learning | Requires Chia DEX simulator that does not yet exist; sample efficiency very poor |
| 3.10 Cross-Chain Bridge Arbitrage | Bridge settlement risk and latency too high; bridge exploits are common |
| 3.14 Batch Auction Strategies | Chia DEX does not use batch auctions; purely speculative |

---

## 7. Key Strategy Trade-offs

Every market making decision involves genuine tension between competing
objectives.  This section catalogues the major trade-offs so that strategy
choices can be made consciously rather than by default.

> **Scholarly foundation:** The trade-offs below are not merely practical
> observations — they arise from fundamental results in market microstructure.
> Garman (1976) first formalized the dealer's inventory-versus-bankruptcy
> trade-off; Ho & Stoll (1981) [ref 23] extended it to return uncertainty;
> Foucault, Kadan & Kandel (2005) [ref 38] proved that the maker-vs-taker
> choice is itself an equilibrium outcome of queue priority and patience.
> The strategy-switching question — *when* to move between strategies — is
> addressed in §7.9 below.

### 7.1 Spread Width vs. Fill Rate

The most fundamental tension in market making.  Amihud & Mendelson (1986)
[ref 3] showed that wider spreads reduce adverse selection but also reduce
trading volume; the optimal spread balances these forces.

| Spread Choice | Fill Rate | Revenue per Fill | Adverse Selection | Inventory Velocity |
|---------------|-----------|-----------------|-------------------|-------------------|
| Narrow (1–2%) | High | Low | High | Fast (frequent rebalancing) |
| Medium (3–5%) | Moderate | Medium | Moderate | Balanced |
| Wide (5–10%)  | Low | High | Low | Slow (infrequent rebalancing) |

**Chia-specific nuance:** On thin DEX markets, extremely narrow spreads
attract informed flow disproportionately (no retail "noise" traders to
balance it).  Wider-than-CEX spreads are often *correct* on Chia, not
a sign of poor calibration.

**Optimization tool:** Thompson Sampling (§1.12) automates this exploration
rather than requiring a manual static choice.

---

### 7.2 Inventory Risk vs. Capital Efficiency

Garman (1976) [ref 35] proved that a dealer who ignores inventory will
eventually go bankrupt with probability 1.  The question is *how much*
inventory risk to tolerate — Avellaneda & Stoikov (2008) [ref 6] and
Guéant et al. (2013) [ref 19] both parameterize this as a risk-aversion
coefficient γ that trades off expected profit against inventory variance.

| Choice | Inventory Risk | Capital Efficiency | Scenario |
|--------|---------------|-------------------|----------|
| Large position limits (high q_max) | High | High | More capital deployed, bigger directional exposure |
| Small position limits (low q_max) | Low | Low | Less capital at risk, but spreads must be wider to compensate |
| Dynamic limits (VaR/CVaR, §3.5) | Adaptive | High in calm, Low in stress | Best of both but requires calibration |

**Key insight:** Inventory risk is not symmetric.  A long position in XCH
during a bear market compounds losses; a short position during a bull market
does the same.  GLFT's linear skew (§1.2) and the rebalancing policy (§5.2)
together address this asymmetry.

---

### 7.3 Strategy Complexity vs. Robustness

López de Prado (2018) [ref 39] devotes an entire chapter to the "backtest
overfitting" problem — complex strategies that look brilliant in-sample but
fail out-of-sample.  The Adaptive Markets Hypothesis (Lo, 2004 [ref 36])
argues that strategy fitness is *environment-dependent*: what works in one
regime may fail in the next, so robustness across regimes matters more than
peak performance within one.

| Complexity | Advantages | Disadvantages |
|------------|-----------|---------------|
| Simple (fixed spread, manual rebalancing) | Easy to debug; predictable behavior | Leaves PnL on the table; no adaptation |
| Moderate (A-S/GLFT + regime detection) | Good adaptation to conditions | Parameter sensitivity; requires calibration |
| High (ML-based, multi-asset, RL) | Potentially highest PnL | Overfitting; failure modes are hard to anticipate |

**Recommendation:** Prefer moderate-complexity strategies with well-understood
failure modes over high-complexity strategies with opaque behavior.  On a
thin, slow DEX like Chia, marginal signal improvements from additional
complexity are small relative to the debugging and operational cost.

This is why the priority ranking (§6) places block-cadence sync and TTL
optimization — operational correctness improvements — above ML-based
toxic flow classification.

---

### 7.4 Passive (Maker) vs. Active (Taker) Behavior

Foucault, Kadan & Kandel (2005) [ref 38] model this as an equilibrium: patient
traders post limit orders (maker), impatient traders cross the spread (taker).
The optimal mix depends on queue priority, time horizon, and urgency.  In Chia's
offer-file model, "maker" has zero explicit cost; "taker" costs the full spread.

| Behavior | Cost | Benefit | When Appropriate |
|----------|------|---------|-----------------|
| Pure maker (only post offers) | Zero taker fees; earn spread | May never fill on illiquid pairs | Normal quoting |
| Hybrid (post maker, take for rebalancing) | Taker fee + cross spread to exit | Reliable inventory management | When imbalance > 0.75 (§5.2) |
| Pure taker (always take existing offers) | Always pays spread | Instant execution | Never — defeats the purpose of market making |

**Key rule:** Never pay the spread for routine operations.  Taker behavior
is only justified for *emergency rebalancing* when the cost of holding the
imbalance exceeds the cost of crossing the spread.

---

### 7.5 Reactive vs. Predictive Quoting

Lehalle & Laruelle (2018) [ref 40] distinguish three quoting regimes:
*passive* (react to fills), *informed* (react to signals), and *predictive*
(lead the market using cross-venue information).  Each step up in
sophistication reduces lag but increases false-signal risk.

| Approach | Lag | False Signal Risk | Implementation Complexity |
|----------|-----|------------------|--------------------------|
| Reactive (respond after fills) | 1–2 blocks | Low | Low |
| Signal-driven (VPIN, OFI, whale) | 0–1 blocks | Medium | Medium |
| Predictive (lead-lag CEX, competitor model) | −1 to +1 blocks | High | High |

**Chia context:** The 52-second block time makes "predictive" quoting based
on CEX feeds (§3.3) especially valuable — there is a natural 10–60 second
window where CEX prices have moved but Chia DEX offers have not yet updated.
Lead-lag signals (§3.3) exploit this window directly.

---

### 7.6 DEX-Only vs. Cross-Venue Operation

| Scope | Capital Required | Hedging Ability | Operational Complexity |
|-------|-----------------|-----------------|----------------------|
| Chia DEX only | Low | None (directional inventory risk) | Low |
| Chia DEX + CEX inventory hedge | 2× | Full delta neutrality | High |
| Multi-DEX (Dexie + TibetSwap) | Medium | Partial (IL vs. offer risk) | Medium |

**Trade-off:** CEX hedging (§3.1) would enable the tightest possible spreads
since inventory risk is eliminated.  However, it requires capital on the CEX,
a CEX API integration, and careful management of the basis between CEX and
DEX prices.  The benefit is clear; the cost is operational complexity and
counter-party risk on the CEX side.

---

### 7.7 Short-Term PnL vs. Long-Term Market Presence

Menkveld (2013) [ref 37] shows that modern market makers who provide *continuous*
liquidity earn a structural informational advantage over intermittent ones —
taker flow preferentially routes to reliable quoters.  Withdrawing during
stress sacrifices this advantage permanently.

| Priority | Short-Term PnL | Long-Term Presence |
|----------|---------------|-------------------|
| **Extractive quoting** | Widen spreads aggressively, withdraw on volatility | Reduces fill frequency over time; other makers capture flow |
| **Supportive quoting** | Maintain competitive spreads through volatility | Lower short-term margin; stronger market position, higher fill share |

**Strategic consideration:** On Chia DEX, where the ecosystem is small and
the number of professional market makers is low, being a *reliable*, consistent
liquidity provider builds taker trust and repeat business.  Predatory widening
drives takers to AMM pools (TibetSwap), permanently reducing the offer-based
flow available to us.

The right balance is: use volatility signals (VPIN, OFI, regime) to widen
*proportionately* — not to withdraw entirely — during stress.

---

### 7.8 On-Chain Transparency: Advantage vs. Liability

Chia's full blockchain transparency (§4.5) cuts both ways:

| As an Advantage | As a Liability |
|-----------------|----------------|
| Full competitor history readable (§3.21) | Our own offer history is equally readable |
| Whale accumulation visible before it fully impacts price | Predictable rebalancing behaviour can be front-run |
| No hidden order flow; what you see is all there is | No dark pool or private OTC to conceal large moves |

**Mitigation:** Randomize rebalancing timing and amounts (within bounds) to
defeat behavioral fingerprinting.  Never rebalance at a fixed inventory ratio
with a fixed offer size — vary both by ±20% stochastically.

---

### 7.9 Strategy Switching: When and How to Change Strategies

A market maker should not run a single static strategy forever.  Market
conditions change, and the *optimal* strategy shifts with them.  Several
scholarly frameworks address this directly:

**Brock & Hommes (1998) [ref 41] — Performance-Based Strategy Switching.**
The foundational model of *adaptive belief systems*: agents track the recent
profitability of competing strategies and switch toward whichever performed
best (net of switching costs).  The key parameter is the *intensity of choice*
β — how aggressively agents abandon underperforming strategies.  Too high β
causes destabilizing herding; too low β leaves you stuck in a bad strategy.

*Application to XOPTrader:* We already have the building blocks — regime
detection (§1.5) identifies the current market state, and Thompson Sampling
(§1.12) explores spread parameters.  The Brock-Hommes insight suggests we
should extend this to *strategy-level* switching: track realized PnL
attribution per strategy component and increase weight on components that
are currently profitable, with a dampening factor to avoid oscillation.

**Lo (2004) [ref 36] — The Adaptive Markets Hypothesis (AMH).**
Argues that market efficiency varies over time as the population of
strategies in use evolves.  Strategies that are profitable attract imitators,
which erodes their edge; strategies that fail are abandoned, which restores
their edge later.  The implication: no strategy should be permanently ruled
out or permanently relied upon.

*Application:* The "Always Defer" category in §6 should be revisited
periodically.  If Chia DEX volume grows 10×, strategies like multi-asset
quoting (§3.8) and ML-based toxic flow classification (§3.6) become viable
because the signal-to-noise ratio improves with volume.

**Farmer & Joshi (2002) [ref 42] — Strategy Interaction Dynamics.**
Simulates what happens when multiple trading strategies coexist.  Key finding:
strategies that are profitable in isolation may become unprofitable when others
adopt them (crowding), and the *composition* of the strategy population matters
as much as individual strategy quality.

*Application:* On Chia DEX, where the competitor population is small and
observable (§3.21, §4.5), we can directly estimate strategy crowding.  If
blockchain analysis (§3.21) reveals that competitors have adopted A-S-style
inventory skewing, our edge from that approach shrinks — and the priority
ranking (§6) should shift toward less-crowded strategies.

**Menkveld (2013) [ref 37] — The Modern Market Maker as Multi-Strategy Agent.**
Documents that successful high-frequency market makers run *portfolios* of
strategies simultaneously, with dynamic capital allocation across them —
not sequential switching between monolithic strategies.  The allocation
shifts in real time based on current market conditions.

*Application:* Rather than switching *from* A-S *to* GLFT, run both with
dynamically weighted blending.  The regime detector (§1.5) can control the
blend: more A-S weight in mean-reverting regimes (where inventory mean-reverts
naturally), more GLFT weight in trending regimes (where inventory penalty
matters more).

**Practical Strategy-Switching Policy for XOPTrader:**

| Signal | Action | Scholarly Basis |
|--------|--------|-----------------|
| Regime detector signals shift from mean-reverting → trending | Increase GLFT weight, intensify inventory skew, widen spreads | Hamilton (1989) [ref 22], Brock & Hommes (1998) [ref 41] |
| VPIN rises above 0.7 | Activate asymmetric widening (§1.11), consider pausing quoting on informed side | Easley et al. (2012) [ref 14] |
| Realized spread < 30% of quoted for 24h | Widen spreads; the current width is being adversely selected through | Amihud & Mendelson (1986) [ref 3] |
| Competitor analysis shows crowding on A-S-style quoting | Shift toward TTL optimization (§3.17) and block-cadence sync (§3.18) — operational edges that can't be crowded | Farmer & Joshi (2002) [ref 42] |
| CEX lead-lag signal consistently predicts DEX price moves | Activate predictive quoting mode (§3.3) | De Jong & Nijman (1997) [ref 12], Lo (2004) [ref 36] |
| Monthly PnL attribution shows one strategy dominating | *Reduce* its allocation slightly — check for overfitting to recent conditions | López de Prado (2018) [ref 39] |

---

### 7.10 Coexisting With Unknown Market Makers

**Assumptions we must live with:**

1. We never know how many other market makers are active in a given pair.
2. We never know what software they run — it could be XOPTrader, a custom
   bot, a manual trader, or an AMM pool.
3. We cannot coordinate with them.  There is no shared state, no signaling
   channel, no way to negotiate who quotes where.

Under these constraints the question is not "how do we avoid competing"
but **"how do we find a profitable niche and stay in it while others do
the same thing?"**

#### Why feedback loops can arise — and the natural limits that contain them

A feedback loop occurs when two market makers with similar strategies
repeatedly fill each other's rebalancing offers: A accumulates inventory,
skews quotes to offload, B takes the other side (and vice versa), and both
pay transaction fees without earning spread income from real takers.

In theory this is a Bertrand competition problem (Bertrand, 1883) —
identical dealers in an undifferentiated product compress spreads to
marginal cost.  Farmer & Joshi (2002) [ref 42] confirmed computationally
that identical strategies crowding the same market destroy their own edge.

**In practice, however, several forces limit the damage:**

- **Capital constraints differ.**  Every operator has different wallet
  balances, risk tolerance, and rebalancing thresholds.  Even identical
  software with identical parameters will diverge after the first asymmetric
  fill because each instance's inventory — and therefore its quote skew —
  becomes unique.
- **Timing jitter.**  Each instance observes the blockchain at different
  moments, processes data at different speeds, and posts offers at different
  points in the ~52-second block cycle.  This natural desynchronization
  means "identical" quotes are rarely truly simultaneous.
- **Taker demand absorbs symmetry.**  Real takers (non-market-makers)
  choose the best-priced offer for their desired direction — a buyer takes
  the lowest ask, a seller takes the highest bid.  As long as genuine taker
  flow exists, two market makers don't exclusively trade with each other —
  they share the taker flow.  The feedback loop only becomes dominant in
  markets with near-zero organic taker volume.
- **Inventory limits are self-correcting.**  If an instance accumulates too
  much one-sided inventory through feedback trading, it will hit its position
  limit (§3.5) or its rebalancing threshold (§5.2) and pull quotes.  This
  breaks the loop automatically.

The real risk is not an infinite loop — it's **compressed spreads and
reduced per-trade profit** when another maker is active.  That's normal
competitive dynamics, not a bug.

#### The core strategy: find and fill the gaps

Rather than trying to outcompete other market makers at the same price
levels, XOPTrader should **seek out the parts of the order book where
nobody else is quoting** — or where existing quotes are stale, wide, or
thin.  This is the gap-filling approach, and it is the natural equilibrium
for multiple independent market makers:

**1. Quote where the book is empty.**
Use competitor detection (§1.7) to observe the existing offer landscape.
If other makers are quoting tight at the inside spread, don't race them —
instead place offers at the **second and third tiers** (§1.4) where there
is no liquidity.  These wider-spread offers fill less frequently but earn
more per fill and face no competition.

**2. Quote when others are absent.**
Block-cadence synchronization (§3.18) and TTL optimization (§3.17) let
XOPTrader maintain fresh offers across block boundaries.  Many competitors
leave stale offers or pull quotes during volatile blocks.  Being present
when others are absent is a structural edge.

**3. Quote the pairs others ignore.**
The CAT ecosystem (§4.6) has 50+ tokens.  Most market makers concentrate on
XCH/USDS and XCH/wBTC.  Illiquid pairs like niche CATs have wider natural
spreads and fewer competitors — higher profit per trade, lower feedback risk.

**4. Let the A-S/GLFT model find equilibrium naturally.**
The Avellaneda-Stoikov (§1.1) and GLFT (§1.2) models already solve this
problem mathematically: the optimal quote depends on _our_ inventory,
_our_ risk aversion (γ), and _our_ time horizon.  Two instances with
different inventories and γ values will naturally quote at different prices
even without explicit coordination.  **The key parameter is γ** — it
controls how aggressively we quote.  A higher γ means wider spreads and
less competition with other makers; a lower γ means tighter spreads and
more fills but more exposure to crowding.

**5. Don't chase competitors — differentiate from them.**
The competitor detection module (§1.7) alerts when someone else is quoting
tighter.  The temptation is to match their spread.  **Resist this unless
your model independently supports the tighter price.**  If another maker is
willing to quote at 50 bps and your model says 80 bps is correct for your
risk level, stay at 80 bps.  They'll absorb the toxic flow at 50 bps; you'll
pick up the rest at a more profitable width.  This is the Foucault, Kadan &
Kandel (2005) [ref 38] insight: in a market with both patient and impatient
takers, multiple spread levels coexist profitably.

#### Avoiding feedback loops without coordination

| Mechanism | How it prevents feedback | Already implemented? |
|-----------|------------------------|---------------------|
| **Inventory-driven quote skew** (A-S/GLFT) | Each instance's quotes diverge as inventory diverges after first asymmetric fill | ✅ Yes (§1.1, §1.2) |
| **Position limits** | Hard caps prevent runaway inventory accumulation | Future (§3.5) |
| **Rebalancing thresholds** (§5.2) | Instance pulls one-sided quotes when imbalance exceeds 0.75, breaking the cycle | Documented policy |
| **Competitor detection** (§1.7) | Detects when another maker is active; shifts to non-overlapping tiers instead of undercutting | ✅ Yes |
| **Fill-rate monitoring** | If fill rate drops below the normal-conditions target of 30% (§0.3), the market is likely crowded — widen spreads or rotate to less competitive pairs | ✅ Yes (Thompson sampling §1.12 tracks this) |
| **TTL + block-cadence timing** | Quote at moments when competitor coverage is thin, rather than when the book is already full | Future (§3.17, §3.18) |
| **Minimum spread floor** | Never quote below a configurable minimum spread (e.g., spread_min_bps = 50–100 for typical CAT pairs, lower for high-volume pairs like XCH/USDS) regardless of competitive pressure — this is the circuit breaker against Bertrand races | Config parameter (spread_min_bps) |

#### Other order-book interaction strategies

Gap-filling should be the **default** XOPTrader behavior on Chia DEX, but it
is not the only valid way to interact with the order book.  The broader
microstructure literature suggests a small menu of recurring tactics:

| Tactic | When to use it | Why it works |
|--------|----------------|--------------|
| **Join the inside queue** | Low toxicity, thin queue ahead, stable regime | Maximizes fill probability while still earning maker spread; appropriate when the best bid/ask is not overcrowded (Parlour, 1998 [ref 43]; Rosu, 2009 [ref 44]) |
| **Improve by one price increment** | Queue at best price is too deep but the spread is still wide enough to justify stepping in front | Buys queue priority at the cost of some spread; best used selectively and with small size so you do not trigger a price-improvement race (Foucault, Kadan & Kandel, 2005 [ref 38]; Lu & Abergel, 2018 [ref 46]) |
| **Step back / fade one tier** | VPIN or OFI rises, competitor crowding increases, or the inside spread becomes too tight | Preserves capital and avoids adverse selection; this is the anti-feedback-loop move when the inside quote is no longer attractive (Cartea, Jaimungal & Penalva, 2015 [ref 10]; Bouchaud, 2010 [ref 45]) |
| **Layer multiple tiers** | Uncertain short-term direction but desire for continuous presence | Captures both fast taker flow near mid and slower flow farther out; reduces all-or-nothing dependence on one quote level (§1.4; Foucault, Kadan & Kandel, 2005 [ref 38]) |
| **Asymmetric sizing** | Inventory is imbalanced but full rebalancing is unnecessary | Keep both sides quoted, but post larger size on the side that reduces inventory and smaller size on the side that increases it (Avellaneda & Stoikov, 2008 [ref 6]; Guéant, Lehalle & Fernandez-Tapia, 2013 [ref 19]) |
| **Hybrid maker-taker rebalancing** | Extreme imbalance, stale book, or urgent risk reduction | Stay passive most of the time, but cross the spread deliberately for a small amount when the expected future spread income from restoring balance exceeds the immediate loss (§5.1, §5.2; Guilbaud & Pham, 2013 [ref 21]) |

The key is to treat these as **state-dependent tactics**, not permanent
identities.  A healthy market maker may spend part of the day joining the
inside, part of the day stepping back, and part of the day filling gaps in
the second or third tier.

#### What about running multiple XOPTrader instances ourselves?

If _you_ control multiple instances (e.g., for scaling or redundancy), the
same principles apply but you can add explicit coordination:

- **Pair partitioning:** each instance trades non-overlapping pairs.
  No overlap = no self-competition.
- **Tier partitioning:** Instance A quotes tier 1 (tight, small), Instance B
  quotes tier 2 (wide, large) on the same pair.
- **Self-detection:** tag offers with an instance identifier so competitor
  detection ignores own-family offers.
- **Shared state:** for advanced multi-machine setups, share inventory
  position via a common database so instances quote collaboratively.

But the gap-filling and A-S equilibrium strategies above work regardless —
even against your own other instances.

#### The equilibrium outcome

In a market with multiple independent market makers (whether running
XOPTrader or not), the expected equilibrium is:

- **Spread compression** toward the level supported by actual taker
  flow and adverse selection risk — this is healthy competition that
  benefits takers.
- **Natural specialization** — makers with lower costs or higher risk
  tolerance capture the tight-spread, high-frequency segment; others
  migrate to wider spreads, less liquid pairs, or different timing
  windows.  This is the Lo (2004) [ref 36] Adaptive Markets prediction:
  strategies find niches rather than all converging on the same point.
- **Reduced per-trade profit but not zero profit** — as long as organic
  taker flow exists, spread income remains positive.  The Menkveld (2013)
  [ref 37] finding applies: successful market makers survive competition
  by running diversified strategy portfolios, not by winning a single
  spread-tightening contest.

#### Scholarly guidance on avoiding feedback loops

Yes — there is meaningful scholarly support for the idea that market makers
should avoid blindly matching each other and should instead differentiate,
back off, or relocate when the book becomes crowded:

- **Farmer & Joshi (2002) [ref 42]** is the most directly relevant paper in
  this document.  It shows that strategies which are profitable in isolation
  can become unprofitable once crowded by similar agents.  The implication is
  to monitor crowding and rotate away from saturated quote locations.
- **Bouchaud (2010) [ref 45]** frames many market instabilities as endogenous
  feedback loops between order flow, liquidity, and price impact.  The
  practical lesson is to avoid pro-cyclical quote chasing and avoid reacting
  mechanically to every competitor move.
- **Parlour (1998) [ref 43]** and **Rosu (2009) [ref 44]** model how the
  state of the order book affects whether traders place aggressive or passive
  orders.  Their core message is that quote placement should depend on queue
  state and book shape, not just on an abstract fair-value estimate.
- **Lu & Abergel (2018) [ref 46]** is especially useful for XOPTrader's
  practical design because it studies order-book-aware market making tactics
  such as joining, improving, and stepping back — exactly the tactical menu
  needed to avoid self-defeating feedback dynamics.
- **Menkveld (2013) [ref 37]** suggests that robust market makers survive by
  operating as diversified, adaptive agents rather than by defending one
  static quoting style.  In our context, that means gap-filling should be the
  baseline strategy, but not the only one.

**Key takeaway:** Don't try to know or control what other market makers
are doing.  Instead, (1) use the A-S/GLFT model to find _your_ optimal
price given _your_ inventory and risk, (2) fill the gaps in the order
book where others aren't quoting, (3) enforce a minimum spread floor to
prevent Bertrand races, and (4) monitor fill rates to detect crowding
early and rotate to less competitive opportunities.  The math finds
equilibrium; the gap-filling strategy avoids head-on collisions.

---

## 8. Strategy Interaction Matrix

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

## 9. Strategy Selection Rationale & Confidence Assessment

When multiple academic papers disagree about the best approach, we need a
principled framework for choosing which theory to implement and when to revisit
that choice.  This section documents **why** we chose each strategy, **how
confident** we are in that choice given the counter-research, and **what
evidence** would cause us to change direction.

### 9.1 Decision Framework

We evaluate competing academic approaches using five criteria, weighted by
relevance to CHIA's unique market microstructure:

| # | Criterion | Weight | Description |
|---|-----------|--------|-------------|
| 1 | **CHIA-specific fit** | 30% | Does the model's assumptions match CHIA's 52-second blocks, thin order books, atomic swaps, and ~1 fill/hour/pair? Models designed for high-frequency equity markets score lower. |
| 2 | **Empirical validation breadth** | 25% | Has the model been validated across multiple asset classes, time periods, and by independent researchers? Single-paper, single-market results score lower. |
| 3 | **Degradation characteristics** | 20% | When the model's assumptions are violated, does it degrade gracefully (wider spreads, slower convergence) or catastrophically (inverted signals, unbounded losses)? |
| 4 | **Implementation complexity** | 15% | Can we implement it correctly with available data? Simpler models with fewer tuning parameters score higher when theoretical advantage is marginal. |
| 5 | **Counter-research severity** | 10% | How strong is the academic opposition? A single methodological critique scores lower than multiple independent empirical rejections. |

**Decision rule:** When two approaches score within 10% of each other, we
implement both and let live performance arbitrate (via Thompson Sampling or
A/B testing over rolling windows).  When one approach clearly dominates, we
implement it but add inline `COUNTER-RESEARCH NOTE` comments documenting the
alternative.

### 9.2 Confidence Levels

We assign each implemented strategy a confidence level based on the balance
between supporting and counter evidence:

| Level | Symbol | Meaning |
|-------|--------|---------|
| **HIGH** | 🟢 | Strong theoretical foundation with broad empirical support.  Counter-research is minor, methodological, or applies only to edge cases.  We would need significant new evidence to change approach. |
| **MEDIUM** | 🟡 | Sound theoretical basis but meaningful counter-research exists.  The approach works well enough in practice, but a superior alternative may exist.  We should monitor live performance and consider switching if a concrete improvement is demonstrated. |
| **LOW** | 🔴 | Known significant limitations in our context.  We use this approach because alternatives are worse or unimplemented, not because we believe the theory is fully correct.  Active TODO items exist to address the limitations. |

### 9.3 Per-Strategy Confidence Rankings

#### 9.3.1 Avellaneda-Stoikov Optimal Market Making

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟡 MEDIUM |
| **Supporting papers** | Avellaneda & Stoikov (2008) [ref 6]; Guéant, Lehalle & Fernandez-Tapia (2013) [ref 19] |
| **Counter-research** | Cartea, Jaimungal & Penalva (2015) §10.3 [ref 10]; Fodra & Pham (2015) [ref 52]; Cont (2001) [ref 56] |
| **Counter-findings** | CR-3 (sawtooth tau exploitable in 24/7 markets), CR-8 (continuous-time optimal control deviates from discrete sparse-block settings) |

**Why we chose it:** A-S provides the only closed-form inventory-penalized
quoting formula with decades of empirical validation.  The GLFT extension
(§1.2 of this document) addresses some limitations.  No competitive alternative
offers the same interpretability and computational efficiency.

**What the counter-research says:** The sawtooth τ creates a deterministic
post-reset complacency window that sophisticated adversaries could exploit.
The continuous-time Brownian motion assumption does not match CHIA's 52-second
block cadence or fat-tailed return distribution.

**Our mitigation:** The GLFT asymptotic mode eliminates the horizon dependency.
We plan to replace the sawtooth with exponential decay (TODO T4-19, T5-CR3).
Regime detection adjusts σ dynamically, partially compensating for
non-Brownian dynamics.

**What would change our mind:** If backtesting shows that a simple
symmetric-spread rule with inventory-proportional skew outperforms A-S
on CHIA data, we would switch to the simpler model.

---

#### 9.3.2 GLFT Running-Inventory-Penalty Model

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟡 MEDIUM |
| **Supporting papers** | Guéant, Lehalle & Fernandez-Tapia (2013) [ref 19]; Guéant (2017) [ref 17] |
| **Counter-research** | Fodra & Pham (2015) [ref 52]; Daian et al. (2020) [ref 57]; Cartea, Jaimungal & Penalva (2015) [ref 10] |
| **Counter-findings** | CR-8 (discrete fill structure means inventory skew coefficient should be larger), CR-11 (model designed for CLOB, not DEX) |

**Why we chose it:** GLFT's infinite-horizon asymptotic eliminates the A-S
sawtooth problem and provides a natural way to handle running inventory
penalties.  The model's structure maps well to CHIA's order-book mechanics.

**What the counter-research says:** The continuous-time Poisson fill-intensity
assumption produces an inventory skew coefficient too small for CHIA's ~1
fill/hour environment.  The model was designed for central limit order books,
not atomic-swap DEX markets.

**Our mitigation:** We amplify the inventory skew by the sparse-fill correction
factor (TODO T5-CR8).  The core insight — penalize inventory continuously
rather than at a terminal horizon — remains valid regardless of fill frequency.

**What would change our mind:** Empirical evidence that the GLFT skew
coefficient is wrong by >2× would warrant a fundamental recalibration or
switch to Fodra-Pham's discrete-time formulation.

---

#### 9.3.3 Four-Component Spread Optimization

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟢 HIGH |
| **Supporting papers** | Stoll (1978) [ref 30]; Ho & Stoll (1981) [ref 23]; Madhavan (2000) [ref 34] |
| **Counter-research** | Stoll (1989) [ref 59] — spread decomposition shows adverse selection is only one of three components |
| **Counter-findings** | CR-13 (on thin DEX markets, order-processing and inventory costs dominate adverse-selection costs) |

**Why we chose it:** The four-component model (base + competition + regime +
signals) is a practical engineering framework rather than a single academic
theory.  It composes multiple well-validated signals without requiring any
single one to be correct.

**What the counter-research says:** Glosten-Milgrom's single-component
adverse-selection model underweights inventory and order-processing costs.
Stoll (1989) shows three components should be weighted separately.

**Our mitigation:** Our spread optimizer already separates these concerns:
competition component handles order-processing (matching competitor spreads),
regime component handles volatility risk, and signals (VPIN/OFI) handle
adverse selection.  This naturally implements a multi-component decomposition.

**What would change our mind:** Very little — the compositional architecture
is robust by design.  Individual component weights may need recalibration,
but the framework itself is sound.

---

#### 9.3.4 Multi-Tier Liquidity Provision

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟢 HIGH |
| **Supporting papers** | Ho & Stoll (1981) [ref 23]; Foucault, Kadan & Kandel (2005) [ref 38] |
| **Counter-research** | None identified |
| **Counter-findings** | None |

**Why we chose it:** Distributing liquidity across multiple price tiers
(tight/mid/wide) is standard practice for market makers.  The approach is
model-agnostic — it works regardless of which spread formula is used.

**What the counter-research says:** No academic paper argues against tiered
quoting.  The only debate is about optimal tier count, spacing, and sizing,
which are empirical calibration questions rather than theoretical disputes.

**Our mitigation:** N/A — no counter-research to mitigate.

**What would change our mind:** If CHIA's offer-book mechanics change to
penalize multi-tier posting (e.g., per-offer fees), we would consolidate
to fewer tiers.

---

#### 9.3.5 Market Regime Detection (Variance Ratio + HMM)

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🔴 LOW |
| **Supporting papers** | Lo & MacKinlay (1988) [ref 26]; Hamilton (1989) [ref 22] |
| **Counter-research** | Lo & MacKinlay (1989) [ref 50]; Boldin (1996) [ref 60]; Calvet & Fisher (2004) [ref 61] |
| **Counter-findings** | CR-5 (VR test has ~5–9% power at our window sizes), CR-14 (HMM likelihood multimodality causes regime identification fragility) |

**Why we chose it:** Regime detection is essential for adapting strategy
parameters to changing market conditions.  The VR test is simple,
interpretable, and widely cited.  HMM adds probabilistic regime labeling.

**What the counter-research says:** Lo & MacKinlay's *own* 1989 Monte Carlo
study shows the VR test has only 5–9% power at n=50–200 (our window sizes).
Regime classification is effectively operating on raw VR thresholds, not
statistically significant signals.  HMM suffers from likelihood multimodality
with short crypto histories, producing unstable regime labels.

**Our mitigation:** We use regime detection as a *soft signal* (spread
multiplier adjustment) rather than a *hard switch* (strategy selection).
Even with low power, the VR test provides directional information that is
better than assuming a fixed regime.  We plan to add Z-statistic significance
gating (TODO T5-CR5) and evaluate multifractal alternatives (TODO T5-CR14).

**What would change our mind:** If we demonstrate that a constant-regime
assumption produces equivalent or better P&L on CHIA data, we would simplify
to a volatility-only adaptive model.

---

#### 9.3.6 Cross-Platform Arbitrage

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟡 MEDIUM |
| **Supporting papers** | Shleifer & Vishny (1997) [ref 28]; practitioner models |
| **Counter-research** | Milionis et al. (2022) [ref 27] — LVR framework; Daian et al. (2020) [ref 57] |
| **Counter-findings** | CR-11 (TibetSwap arbitrage revenue structurally dependent on AMM design; LVR-reduction features could eliminate it) |

**Why we chose it:** Arbitrage is a fundamental market-making revenue source
and helps maintain cross-venue price consistency.  The theory is
uncontroversial — price convergence across venues.

**What the counter-research says:** TibetSwap's AMM design creates structural
arbitrage opportunities (LVR) that may be eliminated by future protocol
upgrades (dynamic fees, oracle integration).  Revenue from this source is
protocol-dependent, not market-structural.

**Our mitigation:** We treat arbitrage as supplemental revenue, not a core
strategy.  The market-making spread capture remains viable regardless of AMM
design evolution.  We monitor protocol changes (TODO T5-CR11).

**What would change our mind:** If TibetSwap v3 eliminates >80% of arb
opportunities, we would deprioritize this module.

---

#### 9.3.7 VPIN — Flow Toxicity Signal

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🔴 LOW |
| **Supporting papers** | Easley, López de Prado & O'Hara (2012) [ref 14] |
| **Counter-research** | Andersen & Bondarenko (2014) [ref 47] |
| **Counter-findings** | CR-1 (VPIN has no incremental predictive power beyond volume + volatility; can fire on pure noise-trader correlation) |

**Why we chose it:** VPIN provides a continuous [0,1] toxicity estimate that
is easy to integrate into the spread multiplier pipeline.  Even if it is
primarily a volatility proxy, that signal has value for spread widening.

**What the counter-research says:** Andersen & Bondarenko (2014) demonstrate
that VPIN has no incremental predictive power after controlling for volume and
volatility.  The "bulk volume" trade classification can reverse signal
direction.  Agent-based simulations show VPIN flags high toxicity even with
zero informed traders.

**Our mitigation:** We treat VPIN as a *supplemental* signal with capped
influence (max 50% widening).  We plan to validate VPIN activations against
realized adverse fills (TODO T5-CR1).  If validation shows <15% correlation,
we will attenuate or disable the signal.

**What would change our mind:** If post-validation correlation is negligible,
we would replace VPIN with a direct volume-volatility composite indicator.

---

#### 9.3.8 OFI — Order Flow Imbalance

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟡 MEDIUM |
| **Supporting papers** | Cont, Kukanov & Stoikov (2014) [ref 11] |
| **Counter-research** | Xu, Lehalle & Alfonsi (2023) [ref 58] |
| **Counter-findings** | CR-2 (best-level OFI explains 10–30% less return variance than multi-level OFI) |

**Why we chose it:** OFI is a leading indicator that detects order-book
pressure before fills confirm it.  The original Cont et al. regression
achieves R² ≈ 65% on equity data.  The signal is lightweight and maps
directly to CHIA's offer-book snapshots.

**What the counter-research says:** Computing OFI from best bid/ask only
discards information from deeper book levels.  Multi-level OFI (top 5–10
levels) explains 10–30% more return variance.  CHIA's shallow book (2–5
levels) makes this extension feasible.

**Our mitigation:** Current best-level OFI is a valid signal — it is simply
*weaker* than it could be.  We plan to extend to multi-level OFI (TODO
T5-CR2).  This is an enhancement, not a correction.

**What would change our mind:** Nothing — the direction is clear.  We will
implement multi-level OFI when prioritized.

---

#### 9.3.9 Thompson Sampling for Spread Learning

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟡 MEDIUM |
| **Supporting papers** | Thompson (1933) [ref 31]; Agrawal & Goyal (2012) [ref 1] |
| **Counter-research** | Besbes, Gur & Zeevi (2014) [ref 55] |
| **Counter-findings** | CR-7 (regret guarantees fail under non-stationarity; Beta posterior too slow to forget stale data) |

**Why we chose it:** Thompson sampling is the gold standard for multi-armed
bandit problems.  It naturally balances exploration and exploitation with
minimal tuning.  The Bayesian framework matches our sparse-feedback
environment.

**What the counter-research says:** Standard Thompson Sampling assumes
stationary reward distributions.  When CHIA's optimal spread shifts with
regime changes, the Beta posterior accumulates stale evidence that takes
hundreds of observations to dilute.

**Our mitigation:** We plan to implement discounted posteriors with
`α_new = α × γ + success, β_new = β × γ + failure` where γ ∈ [0.95, 0.99]
(TODO T5-CR7).  This geometric decay forgets stale evidence across regime
boundaries.

**What would change our mind:** If EXP3 or sliding-window UCB consistently
outperforms discounted Thompson Sampling on CHIA data, we would switch.

---

#### 9.3.10 Whale Detection & Spread Widening

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟢 HIGH |
| **Supporting papers** | Practitioner model; related theory in Kyle (1985) [ref 24] |
| **Counter-research** | Gatheral (2010) [ref 53]; Almgren et al. (2005) [ref 54] |
| **Counter-findings** | CR-10 (Kyle's linear permanent impact is empirically rejected; square-root impact preferred) |

**Why we chose it:** Large trades are empirically associated with adverse
selection risk on all exchanges.  Widening spreads after whale activity is
a standard market-making defensive measure.

**What the counter-research says:** Kyle's linear price-impact model, which
informs the sensitivity calibration, is empirically rejected in favor of
square-root impact.

**Our mitigation:** We use whale detection as a *binary signal* (large trade
detected → widen spread) rather than relying on Kyle's impact formula for
sizing.  The spread widening multiplier is configurable and calibrated from
observed adverse fill rates, not from the linear-impact model.

**What would change our mind:** Nothing — the core signal (large trade →
higher adverse-selection risk) is uncontroversial.  Only the impact
*magnitude* calibration may need updating.

---

#### 9.3.11 Competitor Detection & Response

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟢 HIGH |
| **Supporting papers** | Practitioner model; related to Foucault, Kadan & Kandel (2005) [ref 38] |
| **Counter-research** | None identified |
| **Counter-findings** | None |

**Why we chose it:** Monitoring competitor offers and adjusting spreads is
basic market-making hygiene.  No counter-research challenges this approach.

**What would change our mind:** If CHIA's market structure evolves to make
competitor detection infeasible (e.g., encrypted offer books), we would need
an alternative approach.

---

#### 9.3.12 Asymmetric Spread Widening

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🟢 HIGH |
| **Supporting papers** | Practitioner model combining whale detection with Glosten-Milgrom adverse selection theory |
| **Counter-research** | None directly; related CR-13 (Stoll 1989: adverse selection is only one spread component) |
| **Counter-findings** | None directly applicable |

**Why we chose it:** When directional flow is detected, widening the
spread on the toxic side while keeping the other side competitive is
strictly better than symmetric widening for both profitability and fill rate.

**What would change our mind:** Nothing — asymmetric widening dominates
symmetric widening in all theoretical frameworks.  The only question is
signal quality (whale detection accuracy), not the asymmetric response itself.

---

#### 9.3.13 Bayesian PIN (Considered, Not Implemented)

| Attribute | Assessment |
|-----------|------------|
| **Confidence** | 🔴 LOW |
| **Supporting papers** | Easley et al. (1996) [ref 13]; Easley, López de Prado & O'Hara (2016) [ref 15] |
| **Counter-research** | Duarte & Young (2009) [ref 48]; Collin-Dufresne & Fos (2015) [ref 49] |
| **Counter-findings** | CR-4 (PIN may measure illiquidity friction, not informed trading; lowest when known informed traders are most active) |

**Why we haven't implemented it:** The counter-research is particularly
damaging.  If PIN measures illiquidity rather than information, using it to
gate adverse-selection responses would be counterproductive — widening
spreads precisely when the market is most liquid.

**What would make us implement it:** If independent replication on DEX data
shows PIN correlates with ex-post adverse price moves, we would reconsider.

### 9.4 Summary: Confidence Dashboard

| # | Strategy | Confidence | Key Risk | Status |
|---|----------|------------|----------|--------|
| 1.1 | Avellaneda-Stoikov | 🟡 MEDIUM | Sawtooth τ exploitable; continuous-time mismatch | Fix planned (T5-CR3) |
| 1.2 | GLFT | 🟡 MEDIUM | Inventory skew too small for sparse fills | Fix planned (T5-CR8) |
| 1.3 | Spread Optimizer | 🟢 HIGH | Component weights need calibration | Monitor |
| 1.4 | Multi-Tier Liquidity | 🟢 HIGH | No academic challenge | Stable |
| 1.5 | Regime Detection | 🔴 LOW | VR power ~5–9%; HMM fragile | Fixes planned (T5-CR5, T5-CR14) |
| 1.6 | Arbitrage | 🟡 MEDIUM | Protocol-dependent revenue | Monitor (T5-CR11) |
| 1.7 | Competitor Detection | 🟢 HIGH | No academic challenge | Stable |
| 1.8 | Whale Detection | 🟢 HIGH | Impact model needs recalibration | Minor |
| 1.9 | VPIN | 🔴 LOW | No incremental predictive power | Validate (T5-CR1) |
| 1.10 | OFI | 🟡 MEDIUM | Best-level only; multi-level is stronger | Upgrade planned (T5-CR2) |
| 1.11 | Asymmetric Widening | 🟢 HIGH | Depends on signal quality | Stable |
| 1.12 | Thompson Sampling | 🟡 MEDIUM | Non-stationarity breaks guarantees | Fix planned (T5-CR7) |
| 2.1 | Bayesian PIN | 🔴 LOW | May measure illiquidity, not information | Deferred |

### 9.5 When to Revisit Choices

We revisit strategy confidence rankings when any of the following occur:

1. **New counter-research is published** that directly challenges our
   theoretical foundation (≥2 independent replications).
2. **Live performance data** accumulates ≥500 fills for A/B comparison
   between current and alternative approaches.
3. **CHIA market structure changes** (e.g., sub-second block times, encrypted
   offer books, new DEX mechanics) that invalidate current assumptions.
4. **A TODO item from Tier 5 is completed** and the fix materially changes
   the strategy's behavior or accuracy.

Each revisit should produce a dated addendum to this section documenting the
new evidence and the resulting confidence change.

---

## 10. References

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

32. Engle, R. F. & Granger, C. W. J. (1987). "Co-integration and error correction: representation, estimation, and testing." *Econometrica*, 55(2), 251–276.

33. Engle, R. F. (2002). "Dynamic conditional correlation: A simple class of multivariate generalized autoregressive conditional heteroskedasticity models." *Journal of Business & Economic Statistics*, 20(3), 339–350.

34. Madhavan, A. (2000). "Market microstructure: A survey." *Journal of Financial Markets*, 3(3), 205–258.

35. Garman, M. B. (1976). "Market microstructure." *Journal of Financial Economics*, 3(3), 257–275.

36. Lo, A. W. (2004). "The Adaptive Markets Hypothesis." *Journal of Portfolio Management*, 30(5), 15–29.

37. Menkveld, A. J. (2013). "High frequency trading and the new market makers." *Journal of Financial Markets*, 16(4), 712–740.

38. Foucault, T., Kadan, O. & Kandel, E. (2005). "Limit order book as a market for liquidity." *Review of Financial Studies*, 18(4), 1171–1217.

39. López de Prado, M. (2018). *Advances in Financial Machine Learning*. Wiley. (Ch. 11–12: backtesting pitfalls; Ch. 16–17: strategy selection and bet sizing.)

40. Lehalle, C.-A. & Laruelle, S. (2018). *Market Microstructure in Practice* (2nd ed.). World Scientific.

41. Brock, W. A. & Hommes, C. H. (1998). "Heterogeneous beliefs and routes to chaos in a simple asset pricing model." *Journal of Economic Dynamics and Control*, 22(8–9), 1235–1274.

42. Farmer, J. D. & Joshi, S. (2002). "The price dynamics of common trading strategies." *Journal of Economic Behavior & Organization*, 49(2), 149–171.

43. Parlour, C. A. (1998). "Price dynamics in limit order markets." *Review of Financial Studies*, 11(4), 789–816.

44. Rosu, I. (2009). "A dynamic model of the limit order book." *Review of Financial Studies*, 22(11), 4601–4641.

45. Bouchaud, J.-P. (2010). "The endogenous dynamics of markets: price impact, feedback loops and instabilities." *arXiv:1009.2928*.

46. Lu, X. & Abergel, F. (2018). "Order-book modelling and market making strategies." *FINEC Working Paper 2018-028*.

47. Andersen, T. G. & Bondarenko, O. (2014). "Reflecting on the VPIN dispute." *Journal of Financial Markets*, 17, 292–300. *(Counter-research to ref. 14: challenges VPIN predictive validity.)*

48. Duarte, J. & Young, L. (2009). "Why is PIN priced?" *Journal of Financial Economics*, 91(2), 119–138. *(Counter-research to ref. 13: PIN may measure illiquidity, not just informed trading.)*

49. Collin-Dufresne, P. & Fos, V. (2015). "Do prices reveal the presence of informed trading?" *Journal of Finance*, 70(4), 1555–1582. *(Counter-research to ref. 13: PIN is lowest when informed trading is documented; model may invert.)*

50. Lo, A. W. & MacKinlay, A. C. (1989). "The size and power of the variance ratio test in finite samples: A Monte Carlo investigation." *Journal of Econometrics*, 40(2), 203–238. *(Counter-research to ref. 26: VR test has ~5–9% power at XOPTrader's 50–200 block window sizes.)*

51. Molnár, P. (2012). "Properties of range-based volatility estimators." *International Review of Financial Analysis*, 23, 20–29. *(Counter-research to refs. 4 and Yang-Zhang 2000: Garman-Klass is empirically competitive in continuous markets without overnight gaps.)*

52. Fodra, P. & Pham, H. (2015). "High frequency trading and asymptotics for small risk aversion in a Markov renewal model." *SIAM Journal on Financial Mathematics*, 6(1), 656–684. *(Counter-research to refs. 6 & 19: GLFT/A-S continuous-time optimal control deviates from true optimum in sparse discrete-block settings.)*

53. Gatheral, J. (2010). "No-dynamic-arbitrage and market impact." *Quantitative Finance*, 10(7), 749–759. *(Counter-research to ref. 24: Kyle (1985) linear permanent-impact model violates no-dynamic-arbitrage; square-root impact preferred.)*

54. Almgren, R., Thum, C., Hauptmann, E. & Li, H. (2005). "Direct estimation of equity market impact." *Risk*, 18(7), 57–62. *(Counter-research to ref. 24: empirical square-root market-impact law in lieu of Kyle's linear lambda.)*

55. Besbes, O., Gur, Y. & Zeevi, A. (2014). "Stochastic multi-armed-bandit problem with non-stationary rewards." *Advances in Neural Information Processing Systems*, 27. *(Counter-research to refs. 1 & 31: Thompson Sampling regret guarantees fail under non-stationarity; discounted posteriors recommended.)*

56. Cont, R. (2001). "Empirical properties of asset returns: stylized facts and statistical issues." *Quantitative Finance*, 1(2), 223–236. *(Counter-research to ref. 6: A-S/GLFT Brownian motion assumption does not capture fat tails, volatility clustering, or jumps found in crypto.)*

57. Daian, P. et al. (2020). "Flash Boys 2.0: Frontrunning in decentralized exchanges, miner extractable value, and consensus instability." *IEEE Symposium on Security and Privacy 2020*. *(Counter-research to refs. 6 & 19: adversarial non-stationary fill processes on public-blockchain DEXs invalidate GLFT's Poisson arrival assumption.)*

58. Xu, K., Lehalle, C.-A. & Alfonsi, A. (2023). "Cross-impact of order flow imbalance in equity markets." *Quantitative Finance*, 23(7–8), 1167–1185. *(Counter-research to ref. 11: best-level OFI alone explains 10–30% less return variance than multi-level OFI; deeper book information is material.)*

59. Stoll, H. R. (1989). "Inferring the components of the bid-ask spread: Theory and empirical tests." *Journal of Finance*, 44(1), 115–134. *(Counter-research to ref. 21: Glosten-Milgrom attributes entire spread to adverse selection; Stoll decomposes into adverse selection, inventory, and order-processing components — on thin DEX markets, the latter two dominate.)*

60. Boldin, M. D. (1996). "A check on the robustness of Hamilton's Markov switching model approach to the economic analysis of the business cycle." *Studies in Nonlinear Dynamics & Econometrics*, 1(1), 35–46. *(Counter-research to HMM regime detection: likelihood multimodality causes regime identification fragility with short-history crypto data.)*

61. Calvet, L. E. & Fisher, A. J. (2004). "How to forecast long-run volatility: Regime switching and the estimation of multifractal processes." *Journal of Financial Econometrics*, 2(1), 49–83. *(Counter-research to HMM regime detection: pure Markov switching under-models multi-scale volatility dynamics.)*

62. Acharya, V. V. & Pedersen, L. H. (2005). "Asset pricing with liquidity risk." *Journal of Financial Economics*, 77(2), 375–410. *(Counter-research to ref. 12: static spread-return framework ignores liquidity risk dynamics; time-varying spread is more relevant for intermittent CHIA liquidity.)*
