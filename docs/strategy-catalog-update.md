# XOPTrader -- Strategy Catalog (Merged Update)

> Comprehensive inventory of all strategies for Chia DEX market making.
> Merges the main-branch catalog (Sections 1-5) with the Copilot draft
> (Sections 0, 4-7) and new module work from the Claude agent fleet.
>
> Date: 2026-03-24

---

## Table of Contents

0. [Goals & Design Philosophy](#0-goals--design-philosophy)
1. [Chia-Specific Advantages](#1-chia-specific-advantages)
2. [Implemented Strategies](#2-implemented-strategies)
   - [2.1 Avellaneda-Stoikov Optimal Market Making](#21-avellaneda-stoikov-optimal-market-making)
   - [2.2 GLFT Running-Inventory-Penalty Model](#22-glft-running-inventory-penalty-model)
   - [2.3 Four-Component Spread Optimization](#23-four-component-spread-optimization)
   - [2.4 Multi-Tier Liquidity Provision](#24-multi-tier-liquidity-provision)
   - [2.5 Market Regime Detection](#25-market-regime-detection)
   - [2.6 Cross-Platform Arbitrage](#26-cross-platform-arbitrage)
   - [2.7 Competitor Detection & Response](#27-competitor-detection--response)
   - [2.8 Whale Trade Detection & Spread Widening](#28-whale-trade-detection--spread-widening)
   - [2.9 VPIN -- Flow Toxicity Signal](#29-vpin----flow-toxicity-signal)
   - [2.10 OFI -- Order Flow Imbalance](#210-ofi----order-flow-imbalance)
   - [2.11 Asymmetric Spread Widening](#211-asymmetric-spread-widening)
   - [2.12 Thompson Sampling for Spread Learning](#212-thompson-sampling-for-spread-learning)
3. [New Modules Under Development](#3-new-modules-under-development)
   - [3.1 Never-Sell-at-Loss Manager (loss_manager)](#31-never-sell-at-loss-manager-loss_manager)
   - [3.2 Inventory Drift Analyzer (drift_analyzer)](#32-inventory-drift-analyzer-drift_analyzer)
   - [3.3 Chia Edge Exploiter (chia_edge)](#33-chia-edge-exploiter-chia_edge)
   - [3.4 New Strategies Composite (new_strategies)](#34-new-strategies-composite-new_strategies)
4. [Considered but Not Yet Implemented](#4-considered-but-not-yet-implemented)
   - [4.1 Bayesian PIN (Probability of Informed Trading)](#41-bayesian-pin)
   - [4.2 Predatory Trading Defense (Brunnermeier-Pedersen)](#42-predatory-trading-defense)
   - [4.3 Cartea-Jaimungal Alpha Signal](#43-cartea-jaimungal-alpha-signal)
   - [4.4 Kyle Lambda / Permanent Price Impact](#44-kyle-lambda)
   - [4.5 Stochastic Optimal Control (Guilbaud-Pham)](#45-guilbaud-pham)
5. [Unimplemented -- Future Strategy TODO List](#5-unimplemented----future-strategy-todo-list)
6. [Loss-Taking Policy and Constraint Architecture](#6-loss-taking-policy-and-constraint-architecture)
7. [Strategy Interaction Matrix](#7-strategy-interaction-matrix)
8. [Priority Ranking and Deployment Sequence](#8-priority-ranking-and-deployment-sequence)
9. [Trade-Offs and Open Questions](#9-trade-offs-and-open-questions)
10. [13-Step Engine Heartbeat Integration Map](#10-13-step-engine-heartbeat-integration-map)
11. [References](#11-references)

---

## 0. Goals & Design Philosophy

XOPTrader aims to become the dominant market maker on Chia's decentralized
exchange ecosystem. The design is governed by three principles:

1. **Never sell at a loss.** This is the overriding constraint. It invalidates
   stop losses, timeout exits, and forced EOD closes. All algorithms, risk
   controls, and hedging layers are designed around this constraint.

2. **Spread capture is the primary revenue source.** The bot profits by
   consistently earning the bid-ask spread. Inventory PnL should trend to zero
   over time; if it does not, the inventory management layer is miscalibrated.

3. **Chia-native execution.** The bot exploits Chia's unique properties
   (atomic offers, zero-counterparty-risk settlement, free creation and
   cancellation) rather than treating Chia as a generic DEX.

---

## 1. Chia-Specific Advantages

The following Chia blockchain properties create structural edges that no
Ethereum/Solana-based market maker can replicate:

| Property | Edge | Strategy Impact |
|---|---|---|
| Atomic offer settlement | Zero counterparty risk | No need for escrow or trust assumptions |
| Free offer creation | Infinite quote refresh rate (bounded only by block time) | Cancel-and-recreate ladder every block at zero cost |
| Free cancellation | No sunk cost in stale quotes | Aggressive rebalancing without fee drag |
| 52-second blocks | Coarse time granularity reduces HFT competition | Latency arbitrage is structurally impossible |
| Coin-set (UTXO) model | Deterministic coin locking | Pre-split coins for concurrent multi-tier offers |
| DBX incentives | Direct subsidy for providing liquidity | 100 DBX/day per side on stablecoin pairs |
| Near-zero competition | Wide spreads (300-1000 bps) vs. theoretical minimum (35-60 bps) | Massive room for profitable quoting |

---

## 2. Implemented Strategies

### 2.1 Avellaneda-Stoikov Optimal Market Making

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/avellaneda.cpp`, `cpp/include/xop/strategy/avellaneda.hpp` |
| **Engine Step** | Step 4 (compute_quotes) |
| **Reference** | Avellaneda & Stoikov (2008), Quantitative Finance, 8(3), 217-224. |

Computes an inventory-penalized reservation price and optimal half-spread
using the classical A-S model adapted for Chia's 52-second block cadence.
Rolling N-block horizon replaces the fixed terminal time.

```
r = S - q * gamma * sigma^2 * tau
half_spread = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
```

### 2.2 GLFT Running-Inventory-Penalty Model

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/glft.cpp`, `cpp/include/xop/strategy/glft.hpp` |
| **Engine Step** | Step 4 (compute_quotes, selectable via config) |
| **Reference** | Gueant, Lehalle & Fernandez-Tapia (2013), Mathematics and Financial Economics, 7(4), 477-507. |

Linear skew proportional to normalized inventory. Preferred over A-S for Chia
because there is no natural "session end" on a 24/7 market.

```
skew = -phi * q / q_max
```

### 2.3 Four-Component Spread Optimization

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/spread.cpp`, `cpp/include/xop/strategy/spread.hpp` |
| **Engine Step** | Step 5 (apply_spread_optimizer) |

Decomposes the spread into four additive components: `s_adverse + s_inventory
+ s_cost + s_competition`. Dynamic multipliers adjust for volatility regime,
time-of-day, and weekend effects.

### 2.4 Multi-Tier Liquidity Provision

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/liquidity.cpp`, `cpp/include/xop/strategy/liquidity.hpp` |
| **Engine Step** | Step 7 (generate_ladder) |

Four-tier bid/ask ladder with configurable spacing (default [60, 200, 500,
1000] bps) and size allocation ([30%, 25%, 25%, 20%]). Inventory-aware skew
via phi parameter shifts tiers to rebalance positions.

### 2.5 Market Regime Detection

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/regime.cpp`, `cpp/include/xop/strategy/regime.hpp`, `cpp/src/data/volatility.cpp` |
| **Engine Step** | Step 3 (update_analytics) |
| **Reference** | Lo & MacKinlay (1988), Review of Financial Studies, 1(1), 41-66. |

Variance-ratio test classifies regimes as mean-reverting (VR < 0.85),
random (0.85-1.15), or momentum (VR > 1.15). Strategy parameters adjust
accordingly: tighten spreads for mean-reversion, widen for momentum.

### 2.6 Cross-Platform Arbitrage

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/arbitrage.cpp`, `cpp/include/xop/strategy/arbitrage.hpp` |
| **Engine Step** | Step 9 (check_arbitrage) |

Detects CEX-DEX, cross-DEX, triangular (N^2 enumeration), and cross-bridge
arbitrage. ArbitrageSignal emitted when divergence exceeds 50 bps threshold.

### 2.7 Competitor Detection & Response

| Attribute | Detail |
|---|---|
| **Status** | Implemented (data layer) / Partially wired (engine) |
| **Files** | `cpp/src/execution/market_data.cpp`, `cpp/include/xop/execution/market_data.hpp` |
| **Engine Step** | Step 1 (data ingestion -- needs wiring, see integration review) |

Monitors competing offers on Dexie. Computes best competing bid/ask spread.
Fires alerts when competitor spread < 50 bps. Feeds into `s_competition`
component of the spread optimizer.

**Integration gap:** `ingest_competing_offers()` is implemented but not yet
called from the engine heartbeat. The spread optimizer hardcodes
`best_competing_bps=0.0` instead of reading the competitor metrics.

### 2.8 Whale Trade Detection & Spread Widening

| Attribute | Detail |
|---|---|
| **Status** | Implemented (data layer) / Not yet wired (engine) |
| **Files** | `cpp/src/execution/market_data.cpp` |
| **Engine Step** | Step 1/2 (needs wiring) |
| **Tests** | 14 tests in `test_whale_detection.cpp` |

Detects trades >= 50 XCH or >= 5% of 24h volume. Spread multiplier ramps
linearly from 1.0x to 3.0x over a 10-block window.

**Integration gap:** `ingest_trade()` is implemented but not called from the
engine. The whale spread multiplier is never read by any engine step.

### 2.9 VPIN -- Flow Toxicity Signal

| Attribute | Detail |
|---|---|
| **Status** | Implemented (data layer) / Not yet wired (engine) |
| **Files** | `cpp/src/execution/market_data.cpp` |
| **Engine Step** | Step 1/2 (needs wiring) |
| **Tests** | 7 tests in `test_advanced_trading.cpp` |
| **Reference** | Easley, Lopez de Prado & O'Hara (2012), Review of Financial Studies, 25(5), 1457-1493. |

Volume-synchronized toxicity metric. VPIN in [0, 1] indicates fraction of
informed flow. Bucket size: 10 XCH, window: 50 buckets.

**Integration gap:** `ingest_trade_for_vpin()` is implemented but not called
from the engine.

### 2.10 OFI -- Order Flow Imbalance

| Attribute | Detail |
|---|---|
| **Status** | Implemented (data layer) / Not yet wired (engine) |
| **Files** | `cpp/src/execution/market_data.cpp` |
| **Engine Step** | Step 1 (needs wiring) |
| **Tests** | 6 tests in `test_advanced_trading.cpp` |
| **Reference** | Cont, Kukanov & Stoikov (2014), Quantitative Finance, 14(1), 109-126. |

Aggregates signed book-change events into a normalized [-1, 1] predictor
of short-term price moves. Leading indicator (detects pressure before fills).

**Integration gap:** `ingest_book_snapshot_for_ofi()` is implemented but not
called from the engine.

### 2.11 Asymmetric Spread Widening

| Attribute | Detail |
|---|---|
| **Status** | Implemented (data layer) / Not yet wired (engine) |
| **Files** | `cpp/src/execution/market_data.cpp` |
| **Tests** | 5 tests in `test_advanced_trading.cpp` |

Decomposes symmetric whale multiplier into per-side multipliers using
dominant-side tracking. Skew factor alpha (default 0.5) controls asymmetry.

**Integration gap:** `get_asymmetric_spread_multipliers()` always returns
{1.0, 1.0} until whale detection is wired.

### 2.12 Thompson Sampling for Spread Learning

| Attribute | Detail |
|---|---|
| **Status** | Implemented |
| **Files** | `cpp/src/strategy/spread.cpp` |
| **Engine Step** | Step 5 (optional, enabled via config) |
| **Reference** | Thompson (1933), Biometrika; Agrawal & Goyal (2012), COLT. |

Beta-distribution Thompson sampling over a discrete spread grid. Each
candidate spread is a bandit arm updated on fill/no-fill outcomes.

---

## 3. New Modules Under Development

These modules are being developed by the Claude agent fleet and Copilot
contributors. They represent the next wave of strategy enhancements.

### 3.1 Never-Sell-at-Loss Manager (loss_manager)

| Attribute | Detail |
|---|---|
| **Status** | Core logic implemented; wired into Steps 2, 4, 6 |
| **Files** | `cpp/include/xop/risk/inventory.hpp` (cost basis tracking), `cpp/include/xop/risk/limits.hpp` (enforce_no_loss) |
| **Engine Steps** | Step 2 (record fills, update cost basis), Step 4 (set_cost_basis on strategy), Step 6 (enforce_no_loss on quotes) |

This is not a standalone strategy but rather the overriding constraint that
shapes all other strategies. It is implemented across three subsystems:

1. **InventoryTracker** -- maintains weighted-average cost basis per asset
   via `record_buy()` and `record_sell()`. The `record_sell()` method rejects
   sells below cost basis when the no-loss constraint is active.

2. **StrategyBase::set_cost_basis()** -- injects the cost basis into the
   strategy so that `compute_quotes()` can floor the ask price at
   `cost_basis + min_profit_margin`.

3. **PreTradeCheck::enforce_no_loss()** -- applies the hard floor:
   `ask = max(optimal_ask, cost_basis + margin)`.

**Interaction with underwater positions:** When an asset's market price drops
below cost basis, the loss manager does NOT force a sale. Instead:
- Ask quotes are floored above cost basis (may result in no fills).
- Bid quotes continue normally (buying cheaply lowers the cost basis).
- Position aging (>24h stale) triggers wider ask spreads, not exits.

### 3.2 Inventory Drift Analyzer (drift_analyzer)

| Attribute | Detail |
|---|---|
| **Status** | Partially implemented (hedging layers 1-3) |
| **Files** | `cpp/include/xop/risk/hedging.hpp`, `cpp/src/risk/hedging.cpp` |
| **Engine Step** | Step 10 (run_hedging) |

Analyzes how inventory drifts from target allocation over time and suggests
corrective trades. Implements hedging layers 1-4:

| Layer | Method | Status |
|---|---|---|
| 1 | Inventory-based quote skewing (phi * q / q_max) | Implemented, active |
| 2 | Natural Hedge Efficiency (NHE target > 0.70) | Implemented, logged only |
| 3 | Portfolio-level netting across pairs | Implemented, logged only |
| 4 | Statistical pairs hedging (correlated CATs) | Interface defined, data feed pending |

**Missing integration:** Step 10 in `engine.cpp` computes exposure and NHE but
does not feed the results back into steps 4-7. The suggested rebalancing trades
from `suggest_rebalancing_trades()` are never executed.

### 3.3 Chia Edge Exploiter (chia_edge)

| Attribute | Detail |
|---|---|
| **Status** | Partially implemented |
| **Files** | `cpp/include/xop/execution/coin_manager.hpp`, `cpp/include/xop/rpc/chia_rpc.hpp`, `cpp/include/xop/rpc/dexie_client.hpp` |
| **Engine Steps** | Constructor (coin splitting), Step 8 (offer lifecycle) |

Exploits Chia-specific properties for market-making advantage:

1. **Free offer creation/cancellation** -- the engine refreshes the full tier
   ladder every block with no fee cost.
2. **Pre-split coin pools** -- `CoinManager` pre-splits XCH into trading
   denominations matching tier sizes to enable concurrent multi-tier offers.
3. **Atomic settlement** -- the `OfferManager` leverages Chia's all-or-nothing
   offer model to eliminate partial-fill risk.
4. **DBX incentive farming** -- active offers on stablecoin pairs earn
   100 DBX/day per side from the Dexie incentive program.

**Missing integration:** DBX incentive tracking is not yet connected to the
PnL engine. The coin-splitting logic in `CoinManager` is implemented but
the source file is commented out of `CMakeLists.txt`.

### 3.4 New Strategies Composite (new_strategies)

| Attribute | Detail |
|---|---|
| **Status** | Data layer implemented; engine wiring pending |
| **Files** | `cpp/src/execution/market_data.cpp` (all new methods) |

This represents the collection of new data-layer strategies added by Copilot:
whale detection, VPIN, OFI, competitor tracking, and asymmetric widening.
These are all implemented in `market_data.cpp` with full test coverage but
lack engine integration (see integration review for details).

**Wiring plan for the 13-step heartbeat:**

| Strategy | Ingestion Point | Output Consumer |
|---|---|---|
| Competitor tracking | Step 1 (`ingest_competing_offers`) | Step 5 (`best_competing_bps`) |
| Whale detection | Step 2 (`ingest_trade`) | Step 5 (spread multiplier) |
| VPIN | Step 2 (`ingest_trade_for_vpin`) | Step 5 (toxicity multiplier) |
| OFI | Step 1 (`ingest_book_snapshot_for_ofi`) | Step 5 (directional skew) |
| Asymmetric widening | Derived from whale metrics | Step 5 (per-side multipliers) |

---

## 4. Considered but Not Yet Implemented

### 4.1 Bayesian PIN

| Status | Considered |
|---|---|
| **Reference** | Easley, Kiefer, O'Hara & Paperman (1996), Journal of Finance, 51(4), 1405-1436. |

Classical PIN model requiring MLE over buyer/seller-initiated trade counts.
Deferred in favor of VPIN (its real-time approximation). The simplified
Bayesian adverse-selection estimator in `adverse_selection.cpp` provides a
practical alternative using post-fill price observations.

### 4.2 Predatory Trading Defense

| Status | Considered |
|---|---|
| **Reference** | Brunnermeier & Pedersen (2005), Journal of Finance, 60(4), 1825-1863. |

Models predatory front-running of distressed liquidators. Partially addressed
by whale detection + asymmetric widening. Full implementation requires
inventory-visibility analysis on Chia's transparent blockchain.

### 4.3 Cartea-Jaimungal Alpha Signal

| Status | Considered |
|---|---|
| **Reference** | Cartea, Jaimungal & Penalva (2015), Cambridge University Press, Ch. 10. |

Short-term price predictor from order flow. Partially addressed by OFI.
A formal alpha model would require building a next-price regression:
alpha = f(OFI, VPIN, whale_metrics, ...).

### 4.4 Kyle Lambda

| Status | Considered |
|---|---|
| **Reference** | Kyle (1985), Econometrica, 53(6), 1315-1335. |

Permanent price impact estimation. Deferred because OFI captures a similar
signal without the full regression infrastructure.

### 4.5 Guilbaud-Pham

| Status | Considered |
|---|---|
| **Reference** | Guilbaud & Pham (2013), Quantitative Finance, 13(1), 79-94. |

Joint limit/market order optimization via HJB PDE. Deferred because Chia's
offer model does not support market orders (all orders are limit offers).

---

## 5. Unimplemented -- Future Strategy TODO List

| # | Strategy | Priority | Complexity | Notes |
|---|---|---|---|---|
| 5.1 | Inventory-Aware Hedging via CEX Positions | Medium | High | Requires CEX API integration (OKX/Gate.io) |
| 5.2 | Volatility Forecasting (GARCH / Realized Vol) | Medium | Medium | Enhance Yang-Zhang with forecasting |
| 5.3 | Lead-Lag Signals from CEX Price Feeds | High | Medium | CEX moves first; DEX follows |
| 5.4 | Maker-Taker Fee Optimization | Low | Low | Dexie is 0% fee; relevant only for TibetSwap |
| 5.5 | Dynamic Position Limits (VaR / CVaR) | Medium | Medium | Replace static limits with risk-based |
| 5.6 | Toxic Flow Classification (ML) | Low | High | Requires labeled training data |
| 5.7 | Latency Arbitrage Defense | Low | Low | Not relevant on 52s blocks |
| 5.8 | Multi-Asset Joint Quoting | Medium | High | Cross-pair correlation in quote optimization |
| 5.9 | Adaptive MM with Reinforcement Learning | Low | Very High | PPO agent; requires GPU training |
| 5.10 | Cross-Chain Bridge Arbitrage | Medium | Medium | wUSDC vs wUSDC.b via warp.green |
| 5.11 | Oracle-Based Fair-Value Anchoring | Low | Medium | Requires on-chain oracle |
| 5.12 | Quote Stuffing / Spoofing Detection | Low | Medium | Minimal threat on Chia |
| 5.13 | Impermanent Loss Hedging for AMM LP | Medium | Medium | For TibetSwap LP positions |
| 5.14 | Batch Auction Strategies | Low | High | Requires CHIP-0052 (partial offers) |
| 5.15 | Market Microstructure Invariance | Low | Medium | Kyle-Obizhaeva scaling laws |

---

## 6. Loss-Taking Policy and Constraint Architecture

### Core Rule

**NEVER SELL AT A LOSS.** This is a hard constraint, not a guideline.

### Architectural Enforcement

The constraint is enforced at three independent layers, any one of which is
sufficient to prevent a loss-making sale:

| Layer | Module | Mechanism |
|---|---|---|
| Strategy | `StrategyBase::set_cost_basis()` | Floors ask price in quote computation |
| Risk | `PreTradeCheck::enforce_no_loss()` | Post-strategy ask-price clamp |
| Execution | `InventoryTracker::record_sell()` | Rejects sell if price < cost_basis |

### Consequences for Strategy Design

1. **No stop losses.** Underwater positions are held indefinitely.
2. **No timeout exits.** Stale inventory wider spreads, not forced sales.
3. **No forced EOD closes.** 24/7 market; no session boundary.
4. **Hedging respects the rule.** If a hedge position goes underwater, hold
   it and widen quotes rather than close at a loss.
5. **Patience is the ultimate hedge.** The bot waits for favorable exit prices.

### Underwater Position Handling

When market price drops below cost basis:
- Ask offers are posted at `cost_basis + min_margin` (may not fill).
- Bid offers continue normally (buying cheaper lowers the cost basis via
  weighted-average update).
- The drift_analyzer monitors position aging and alerts if >24h stale.
- DBX incentives continue to accrue on active offers, partially offsetting
  the opportunity cost of held inventory.

---

## 7. Strategy Interaction Matrix

Shows how each strategy module affects the outputs of other modules.
Read as: "row module's output is consumed by column module."

| Producer \ Consumer | A-S/GLFT | Spread Opt | Liquidity | Risk | Offers | Arb | Hedging | PnL |
|---|---|---|---|---|---|---|---|---|
| **A-S / GLFT** | -- | raw quotes | -- | -- | -- | -- | -- | -- |
| **Spread Optimizer** | -- | -- | spread_bps | -- | -- | -- | -- | -- |
| **Regime Detection** | spread_mult, skew_mult | regime_multiplier | -- | -- | -- | -- | -- | -- |
| **Liquidity Engine** | -- | -- | -- | -- | tier ladder | -- | -- | -- |
| **Competitor Detect** | -- | best_competing_bps | -- | -- | -- | -- | -- | -- |
| **Whale Detection** | -- | spread_multiplier | -- | -- | -- | -- | -- | -- |
| **VPIN** | -- | toxicity signal | -- | -- | -- | -- | -- | -- |
| **OFI** | -- | directional skew | -- | -- | -- | -- | -- | -- |
| **Asymmetric Widen** | -- | per-side multipliers | -- | -- | -- | -- | -- | -- |
| **Thompson Sampling** | -- | exploration spread | -- | -- | -- | -- | -- | -- |
| **Arbitrage Scanner** | -- | -- | -- | -- | -- | -- | -- | arb PnL |
| **Loss Manager** | ask floor | -- | -- | reject bad sells | -- | -- | -- | -- |
| **Drift Analyzer** | -- | -- | -- | rebalance suggestions | -- | -- | skew adj | -- |
| **Chia Edge** | -- | -- | -- | -- | atomic offers | -- | -- | DBX income |
| **Inventory Tracker** | cost_basis, q | inv_q, q_max | inv_ratio | risk_status | -- | -- | positions | -- |

### Key Interaction Chains

1. **Quote pipeline:** Regime -> A-S/GLFT -> Spread Optimizer (+ whale + VPIN + OFI + competitor + Thompson) -> Risk limits (+ loss manager) -> Liquidity ladder -> Offer Manager.

2. **Risk feedback:** Fills -> Inventory Tracker -> cost basis -> Loss Manager -> ask floor; Inventory Tracker -> drift_analyzer -> hedging suggestions.

3. **Market intelligence:** Dexie API -> competitor offers -> best_competing_bps -> s_competition; Fills -> whale detection -> spread multiplier; All trades -> VPIN -> toxicity; Book snapshots -> OFI -> directional skew.

---

## 8. Priority Ranking and Deployment Sequence

### Phase 1: Core (Current -- Weeks 1-4)

| Priority | Strategy | Status | Blocking? |
|---|---|---|---|
| P0 | A-S / GLFT quote engine | Implemented | No |
| P0 | Four-component spread optimizer | Implemented | No |
| P0 | Multi-tier liquidity ladder | Implemented | No |
| P0 | Never-sell-at-loss constraint | Implemented | No |
| P0 | Cost-basis tracking (FIFO w-avg) | Implemented | No |
| P1 | Regime detection (variance ratio) | Implemented | No |

### Phase 2: Intelligence Wiring (Weeks 5-6)

| Priority | Strategy | Status | Blocking? |
|---|---|---|---|
| P1 | Wire competitor detection into spread optimizer | Data ready, needs engine call | CMakeLists fix |
| P1 | Wire whale detection into spread pipeline | Data ready, needs engine call | CMakeLists fix |
| P1 | Wire VPIN into toxicity-aware quoting | Data ready, needs engine call | CMakeLists fix |
| P1 | Wire OFI into directional skew | Data ready, needs engine call | CMakeLists fix |
| P2 | Wire asymmetric widening into tier generation | Data ready, needs engine call | Whale wiring first |
| P2 | Thompson Sampling live activation | Implemented, config toggle | Fill feedback loop |

### Phase 3: Hedging & Analytics (Weeks 7-8)

| Priority | Strategy | Status | Blocking? |
|---|---|---|---|
| P2 | NHE monitoring and alerts | Implemented, logged only | None |
| P2 | Portfolio netting suggestions | Implemented, logged only | Execution wiring |
| P3 | Statistical pairs hedging | Interface only | Correlation data feed |
| P3 | CEX-DEX arbitrage execution | Signal detection only | CEX API client |

### Phase 4: Advanced (Months 3-6)

| Priority | Strategy | Status | Blocking? |
|---|---|---|---|
| P3 | Lead-lag CEX signals | Not started | CEX WebSocket feed |
| P3 | Cross-bridge arbitrage | Not started | warp.green API |
| P4 | RL-based adaptive quoting | Not started | GPU training infra |
| P4 | Dynamic VaR/CVaR limits | Not started | PnL history pipeline |

---

## 9. Trade-Offs and Open Questions

### Never-Sell-at-Loss vs. Capital Efficiency

The no-loss constraint can lock capital in underwater positions for extended
periods, especially during sustained market downtrends. The current design
accepts this trade-off because:
- XCH is near all-time lows ($2.70); further downside is limited.
- DBX incentives provide income on locked capital.
- The bot's time horizon is indefinite.

**Open question:** Should there be a "soft override" that relaxes the
constraint after extremely long holding periods (e.g., >30 days) if the
unrealized loss is within a configurable tolerance? Current answer: No.

### Spread Tightness vs. Adverse Selection Risk

Tighter spreads increase fill rate and DBX incentives but expose the bot to
higher adverse selection. The four-component model balances this, but:

**Open question:** Should VPIN directly modulate the `s_adverse` component,
or should it be a separate post-multiplier? A separate multiplier is simpler
but does not interact correctly with the competitive floor.

### Single-Asset vs. Multi-Asset Optimization

Currently each pair is quoted independently. Cross-pair correlations are
monitored (hedging layer 4) but do not feed back into quote generation.

**Open question:** Should the A-S/GLFT strategy accept a portfolio-level
inventory vector instead of a per-pair scalar? This is a significant
architectural change that would require a multi-asset strategy class.

---

## 10. 13-Step Engine Heartbeat Integration Map

Shows which strategy modules execute at each step of the per-block heartbeat.

| Step | Name | Active Modules | Status |
|---|---|---|---|
| 1 | Update market state | MarketDataFeed (ingest_dexie, refresh) | Active |
| | | Competitor tracking (ingest_competing_offers) | **NOT WIRED** |
| | | OFI (ingest_book_snapshot_for_ofi) | **NOT WIRED** |
| 2 | Process fills | OfferManager (detect_fills) | Active |
| | | InventoryTracker (record_buy/sell) | Active |
| | | Whale detection (ingest_trade) | **NOT WIRED** |
| | | VPIN (ingest_trade_for_vpin) | **NOT WIRED** |
| | | PnLTracker (record_fill) | Active |
| 3 | Update analytics | VolatilityEstimator (update) | Active |
| | | Regime detection (get_regime) | Active |
| | | AdverseSelectionEstimator | Active (PIN update after fills) |
| 4 | Compute quotes | A-S or GLFT (compute_quotes) | Active |
| | | set_cost_basis (loss_manager) | Active |
| 5 | Apply spread optimizer | SpreadOptimizer (compute_spread) | Active |
| | | Whale spread multiplier | **NOT WIRED** (hardcoded 0.0) |
| | | VPIN toxicity adjustment | **NOT WIRED** |
| | | OFI directional skew | **NOT WIRED** |
| | | Competitor best_competing_bps | **NOT WIRED** (hardcoded 0.0) |
| | | Asymmetric multipliers | **NOT WIRED** |
| | | Thompson Sampling | Active (optional, config toggle) |
| 6 | Apply risk limits | enforce_no_loss (loss_manager) | Active |
| | | apply_limits (inventory, Kelly, CAT cap) | Active |
| 7 | Generate ladder | LiquidityEngine (compute_ladder) | Active |
| | | apply_inventory_skew | Active |
| 8 | Manage offers | OfferManager (cancel_stale, post_quotes) | Active |
| | | Database (insert_offer) | Active |
| 9 | Check arbitrage | ArbitrageSignal detection | Active (signal only) |
| | | Arbitrage execution | Phase 2 |
| 10 | Run hedging | NHE computation | Active (logged only) |
| | | Portfolio netting | Active (logged only) |
| | | Rebalancing suggestions | Not executed |
| 11 | Update PnL | PnLTracker (mark_to_market) | Active |
| | | Database (insert_snapshots_batch) | Active |
| 12 | Export metrics | MetricsExporter (6 dashboards) | Active |
| 13 | Check alerts | AlertManager (14 rules) | Active |

**Summary:** 12 of 13 steps are actively executing. The primary gap is in
Step 1/2/5 where the new data-layer strategies (whale, VPIN, OFI, competitor,
asymmetric) have fully implemented logic but no engine call sites.

---

## 11. References

1. Avellaneda, M. & Stoikov, S. (2008). "High-frequency trading in a limit order book." Quantitative Finance, 8(3), 217-224.
2. Gueant, L., Lehalle, C.-A. & Fernandez-Tapia, J. (2013). "Dealing with the inventory risk: a solution to the market making problem." Mathematics and Financial Economics, 7(4), 477-507.
3. Amihud, Y. & Mendelson, H. (1986). "Asset pricing and the bid-ask spread." Journal of Financial Economics, 17(2), 223-249.
4. Stoll, H. (1978). "The supply of dealer services in securities markets." Journal of Finance, 33(4), 1133-1151.
5. Ho, T. & Stoll, H. (1981). "Optimal dealer pricing under transactions and return uncertainty." Journal of Financial Economics, 9(1), 47-73.
6. Lo, A. & MacKinlay, C. (1988). "Stock market prices do not follow random walks." Review of Financial Studies, 1(1), 41-66.
7. Hamilton, J. (1989). "A new approach to the economic analysis of nonstationary time series." Econometrica, 57(2), 357-384.
8. Easley, D., Lopez de Prado, M. & O'Hara, M. (2012). "Flow toxicity and liquidity in a high-frequency world." Review of Financial Studies, 25(5), 1457-1493.
9. Cont, R., Kukanov, A. & Stoikov, S. (2014). "The price impact of order book events." Quantitative Finance, 14(1), 109-126.
10. Thompson, W. (1933). "On the likelihood that one unknown probability exceeds another." Biometrika, 25(3/4), 285-294.
11. Agrawal, S. & Goyal, N. (2012). "Analysis of Thompson sampling for the multi-armed bandit problem." COLT 2012.
12. Easley, D., Kiefer, N., O'Hara, M. & Paperman, J. (1996). "Liquidity, information, and infrequently traded stocks." Journal of Finance, 51(4), 1405-1436.
13. Brunnermeier, M. & Pedersen, L. (2005). "Predatory trading." Journal of Finance, 60(4), 1825-1863.
14. Cartea, A., Jaimungal, S. & Penalva, J. (2015). "Algorithmic and High-Frequency Trading." Cambridge University Press.
15. Kyle, A. (1985). "Continuous auctions and insider trading." Econometrica, 53(6), 1315-1335.
16. Guilbaud, F. & Pham, H. (2013). "Optimal high-frequency trading with limit and market orders." Quantitative Finance, 13(1), 79-94.
17. Shleifer, A. & Vishny, R. (1997). "The limits of arbitrage." Journal of Finance, 52(1), 35-55.
18. Yang, D. & Zhang, Q. (2000). "Drift-independent volatility estimation." Journal of Business, 73(3), 477-491.

---

> This document consolidates the main-branch trading-strategies.md (Sections
> 1-5, produced by 20 Claude research agents) with the Copilot draft PR
> additions (goals, Chia advantages, loss-taking policy, priority ranking,
> trade-offs) and the current codebase state as of 2026-03-24.
>
> CONSTRAINT: All strategies enforce NEVER SELL AT A LOSS.
