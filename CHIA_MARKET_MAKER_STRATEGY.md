# CHIA Market Maker Strategy - XOPTrader
## Comprehensive Implementation Plan

*Compiled from 20 parallel deep-research agents | March 2026*

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Market Opportunity](#2-market-opportunity)
3. [CHIA DEX Ecosystem](#3-chia-dex-ecosystem)
4. [CHIA Offer System & Technical Foundation](#4-chia-offer-system--technical-foundation)
5. [Market Making Algorithms](#5-market-making-algorithms)
6. [Spread Optimization](#6-spread-optimization)
7. [Capital Allocation](#7-capital-allocation)
8. [Inventory & Risk Management](#8-inventory--risk-management)
9. [Hedging Framework](#9-hedging-framework)
10. [Arbitrage Strategies](#10-arbitrage-strategies)
11. [Liquidity Provision Strategies](#11-liquidity-provision-strategies)
12. [Smart Contract Architecture](#12-smart-contract-architecture)
13. [Bot Architecture](#13-bot-architecture)
14. [Infrastructure](#14-infrastructure)
15. [Monitoring & Operations](#15-monitoring--operations)
16. [Backtesting Framework](#16-backtesting-framework)
17. [Competitive Landscape](#17-competitive-landscape)
18. [Regulatory Considerations](#18-regulatory-considerations)
19. [Profitability Model](#19-profitability-model)
20. [Implementation Roadmap](#20-implementation-roadmap)

---

## 1. Executive Summary

**Objective**: Become the dominant market maker on the CHIA blockchain's decentralized exchange ecosystem, using our own XCH capital to provide liquidity via CHIA's native offer system.

**Key Findings**:
- Total on-chain DEX volume is ~$2K/day -- near-zero competition, wide spreads (3-10%)
- CHIA's native offer system provides atomic settlement with zero counterparty risk
- Minimum profitable spread: 35-60 bps (well below current market spreads)
- Optimal starting capital: $10K-$30K for best return-on-capital
- Expected monthly returns: 2-5% (conservative), 3-8% (optimistic) with DBX incentive subsidies
- **Core constraint**: NEVER sell at a loss -- built into all algorithms and risk models

**Strategic Edge**: CHIA is a greenfield with near-zero professional market making. A single well-capitalized, automated market maker can dominate price discovery and earn both trading spreads and DBX incentive subsidies.

---

## 2. Market Opportunity

### Current CHIA Market State (March 2026)

| Metric | Value |
|--------|-------|
| XCH Price | ~$2.70-$2.88 |
| XCH Market Cap | ~$40-69M |
| XCH 24h CEX Volume | ~$2.4M (OKX dominant) |
| Dexie 24h Volume | ~$1,950 |
| Top Pair (SBX/XCH) | ~$1,385/day |
| Block Time | ~52 seconds |
| All-Time Low | $2.37 (Feb 2026) |

### Why Now

- XCH at all-time lows = cheap entry for capital deployment
- DeFi ecosystem maturing (TibetSwap v2, Dexie Combined Swap, warp.green bridge)
- Hard fork planned June 2026 -- volatility event creates opportunity
- DBX incentive program subsidizes market makers (100 DBX/day per side for stablecoin pairs)
- CHIP-0052 (partial offers) in draft -- will enable on-chain order book functionality

### Liquidity Gaps to Fill

1. **XCH/wUSDC stablecoin pairs** -- highest utility, actively incentivized
2. **Cross-bridge arbitrage** (wUSDC vs wUSDC.b) -- same underlying, different asset IDs
3. **DEX-to-CEX arbitrage** -- persistent price dislocations (CEX has 1000x more volume)
4. **CAT pairs beyond SBX** -- most CATs have zero active offers at any given time
5. **Cross-DEX arbitrage** -- dexie order book vs TibetSwap AMM price divergences

---

## 3. CHIA DEX Ecosystem

### Active Platforms

| Platform | Type | Fee | Key Feature |
|----------|------|-----|-------------|
| **dexie.space** | Offer aggregator | 0% (1% on Combined Swap) | Largest activity, DBX incentives, API |
| **TibetSwap v2** | AMM (x*y=k) | 0.7% | 50K+ XCH TVL, auto-compounding |
| **Hashgreen** | Order book + AMM | 0.9% (AMM) | 72 CATs listed |
| **OfferBin** | Bulletin board | 0% | Raw offer file posting |
| **Splash Network** | P2P protocol | 0% | Decentralized offer distribution |

### Complementary Infrastructure

- **warp.green** -- ETH/Base-to-CHIA bridge (wUSDC, wETH)
- **Circuit DAO** -- CDP stablecoin (BYC) + CRT governance
- **xch.trade** -- CAT price aggregator

### APIs for Programmatic Trading

| API | Rate Limit | Use |
|-----|-----------|-----|
| Chia Wallet RPC (:9256) | Local only | Create/take/cancel offers |
| Chia Full Node RPC (:8555) | Local only | Block/mempool monitoring |
| Dexie API (api.dexie.space/v1) | 50 req/10s | Market data, order books |
| Python: `dexie.py` | Via API | Client library |

---

## 4. CHIA Offer System & Technical Foundation

### How Offers Work

1. **Maker** creates an incomplete, partially signed spend bundle specifying assets offered and requested
2. Offer is serialized as a bech32-encoded text file and distributed (dexie, Splash, direct)
3. **Taker** completes the bundle by adding their complementary coin spends
4. Combined bundle settles atomically in one on-chain transaction (~52s block)

### Key Properties

- **Atomic**: all-or-nothing settlement, zero counterparty risk
- **Free to create**: only settlement pays fees (typically 0.0001 XCH)
- **Free to cancel**: spend any referenced coin to invalidate
- **Multi-asset**: XCH, CATs, NFTs in a single offer
- **Cross-platform**: same offer visible on multiple aggregators via Splash

### Wallet RPC Endpoints

| Endpoint | Function |
|----------|----------|
| `create_offer_for_ids` | Create new offer |
| `take_offer` | Accept an offer |
| `cancel_offer` / `cancel_offers` | Cancel offers |
| `check_offer_validity` | Verify offer status |
| `get_all_offers` | List offers with pagination |

### Coin Set Model Implications

- Coins are UTXOs -- each offer locks specific coins
- Pre-split coins into trading denominations for concurrent offers
- Cancellation requires spending a locked coin (~52s to confirm)
- A single coin cannot back offers on multiple venues simultaneously

---

## 5. Market Making Algorithms

### Avellaneda-Stoikov Optimal Market Making (Adapted for CHIA)

**Reservation price** (inventory-adjusted mid):
```
r(t, q) = S_t - q * gamma * sigma^2 * (T - t)
```

**Optimal half-spread**:
```
delta* = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * (T - t)
```

**CHIA adaptations**:
- Replace continuous time with block-counting: `(T-t) = (N-n) * 52 seconds`
- Per-block volatility: `sigma_block = sigma_annual * 0.000963`
- Fill intensity calibrated from historical dexie offer-take data

### GLFT Extensions (Running Inventory Penalty)

Preferred over A-S for CHIA (no natural "session end" -- 24/7 market):
```
ask = S_t + half_spread - skew * q_t
bid = S_t - half_spread - skew * q_t
```
Where `skew = phi * q / q_max` continuously nudges inventory toward zero.

### Regime Detection

- **Variance ratio test** (rolling 50-100 blocks): VR < 0.85 = mean-reverting, VR > 1.15 = momentum
- Mean-reverting: tighten spreads (0.8x), reduce inventory shedding (0.5x)
- Momentum: widen spreads (1.5x), aggressive inventory shedding (2.0x)

### Never-Sell-at-Loss Integration

The ask price is constrained:
```
ask = max(optimal_ask, cost_basis + min_profit_margin)
```
This transforms the unconstrained optimization into a constrained one where underwater inventory is held, not liquidated.

---

## 6. Spread Optimization

### Four-Component Spread Model

```
spread = s_adverse + s_inventory + s_cost + s_competition
```

1. **Adverse selection**: `gamma * sigma * sqrt(E[T_fill]) * PIN`
2. **Inventory**: `gamma * sigma^2 * tau * |q| / Q_max`
3. **Cost**: `(fee_blockchain + fee_aggregator) / trade_size`
4. **Competition**: `max(s_floor, best_competing + epsilon)`

### Minimum Profitable Spread: 35-60 bps

For XCH at 5% daily vol, PIN=0.15, 2hr average fill time:
- Adverse selection: ~15.3 bps
- Transaction costs: ~0.1 bps
- Opportunity cost: ~1.1 bps
- Inventory cost: ~2.1 bps
- **Half-spread floor**: ~18.6 bps → **Full spread: ~37 bps**

Current market spreads are 300-1000 bps -- massive room for profitable market making.

### Dynamic Adjustments

| Factor | Adjustment |
|--------|-----------|
| High volatility regime | Widen 80% |
| Low volatility regime | Tighten 30% |
| Weekend | Widen 15-20% |
| US+EU overlap hours | Tighten 10% |
| Inventory skewed >60% | Asymmetric widening |

### ML-Based Spread Setting

- **Thompson Sampling** (starting approach): Discrete spread grid, Bayesian updating on fill profitability
- **PPO Reinforcement Learning** (advanced): State = (inventory, vol, PIN, competition, time), Action = (bid_spread, ask_spread, sizes)

---

## 7. Capital Allocation

### Optimal Range: $10K-$30K

Below $1K: coin granularity limits concurrent offers
Above $50K: ecosystem can't absorb capital productively (~$2K/day volume)

### Allocation Framework

| Function | % of Capital | Purpose |
|----------|-------------|---------|
| Active Offers | 35-45% | Outstanding bid/ask offers |
| Rebalancing Buffer | 15-20% | Inventory rebalancing after fills |
| Emergency Reserve | 15-20% | Stablecoin or cold XCH |
| Opportunity Reserve | 10-15% | New pairs, flash events |
| Operational Float | 5-10% | Fees, dust, coin splitting |

### Asset Allocation

| Asset | Target % | Role |
|-------|---------|------|
| XCH | 40-50% | Base asset for all pairs |
| wUSDC/wUSDC.b | 15-25% | Stablecoin anchor |
| SBX | 8-12% | Primary CAT pair |
| DBX | 5-10% | Incentive + market making |
| Other CATs | 5-10% | Opportunistic |
| Reserve | 10-15% | Undeployed buffer |

### $10K Example Portfolio

```
XCH/wUSDC AMM (TibetSwap):     $3,000 (30%)
XCH/wUSDC Offers (Dexie):      $3,000 (30%)
Top CAT/XCH AMM:               $1,500 (15%)
Top CAT/XCH Offers:            $1,500 (15%)
Reserve:                        $1,000 (10%)
```

### Position Sizing: Half-Kelly

```
f* = (spread - sigma*sqrt(tau)) / (sigma^2 * tau)
Practical: ~2% of capital per pair per price level
```

---

## 8. Inventory & Risk Management

### CORE RULE: NEVER SELL AT A LOSS

This invalidates stop losses, timeout exits, and forced EOD closes. All risk management works around this constraint.

### Inventory Controls

| Control | Threshold | Action |
|---------|----------|--------|
| Soft limit | 60% in one asset | Begin aggressive quote skewing |
| Hard limit | 80% in one asset | Pull quotes on overweight side |
| Underwater position | cost_basis > market | Hold; only offer above cost_basis |
| Position aging >24h | Stale inventory | Widen ask spread, don't force exit |
| Single CAT cap | 12% of portfolio | Never exceed regardless of opportunity |

### Cost Basis Tracking

Track FIFO weighted-average cost basis per asset. Every ask must be >= cost_basis + min_margin.

### Risk Metrics

| Metric | Healthy | Unhealthy |
|--------|---------|-----------|
| Spread PnL / Total PnL | >80% | <50% |
| Inventory PnL mean | Near zero | Persistently negative |
| Fill rate per hour | Stable | Declining |
| Max inventory / allowed | <60% | >90% |
| Adverse selection rate | <25% | >40% |

### Emergency Playbook

- **Flash crash >20%**: PULL ALL QUOTES. Hold everything. Resume after 100+ stable blocks.
- **Smart contract exploit**: Exit AMM immediately. Cancel all offers.
- **Network congestion**: Increase rebalancing buffer to 25-30%.
- **All offers fill one-sided**: Investigate. Widen opposite side. NEVER panic-sell.

---

## 9. Hedging Framework

### 7-Layer Hedge Priority Stack

| Priority | Method | Cost |
|----------|--------|------|
| 1 | Inventory-based self-hedging (quote skewing) | Free |
| 2 | Natural two-sided balancing (maximize NHE) | Free |
| 3 | Portfolio-level netting across pairs | Free |
| 4 | Statistical pairs hedging (correlated CATs) | Free |
| 5 | XCH perp delta hedging (if available) | Moderate |
| 6 | Cross-asset proxy hedge (BTC/ETH) | Moderate + basis risk |
| 7 | Options tail hedge (BTC put spreads) | Fixed premium |

**Layers 1-4 are free and handle 70-80% of risk.**

### Key Constraint

All hedges respect never-sell-at-loss:
- If perp hedge is underwater: hold it, widen quotes
- If cross-asset hedge loses: hold, wait for convergence
- If inventory is underwater: hold, quote above cost basis
- **Patience is the ultimate hedge**

### Natural Hedge Efficiency Target

```
NHE = 1 - (|net_inventory_change| / total_volume)
Target: NHE > 0.70
```

---

## 10. Arbitrage Strategies

### CEX-DEX Arbitrage (Primary Opportunity)

XCH trades $2.4M/day on CEXes vs ~$2K on DEXes. Persistent price dislocations:
- Monitor CEX orderbooks (OKX, MEXC, Gate.io)
- Post competitive offers on dexie when DEX price diverges from CEX
- Expected edge: 50-200 bps per arbitrage cycle

### Cross-DEX Arbitrage

- dexie order book vs TibetSwap AMM price divergences
- dexie Combined Swap partially addresses this but a dedicated bot is faster
- Atomic: take on one venue, make on the other

### Triangular Arbitrage

```
XCH -> CAT_A -> CAT_B -> XCH
```
Route through multiple pools when cross-rates are inconsistent.

### Cross-Bridge Arbitrage

wUSDC (from ETH) vs wUSDC.b (from Base) -- same underlying, different CHIA asset IDs.

---

## 11. Liquidity Provision Strategies

### Hybrid AMM + Offer Strategy

| Layer | Capital | Strategy |
|-------|---------|----------|
| Foundation (30-40%) | TibetSwap AMM | Passive fee income, auto-compounding |
| Active Core (30-40%) | Dexie multi-tier offers | 4-tier bid/ask, inventory-aware |
| Opportunistic (10-15%) | Cross-venue arb | Capture price discrepancies |
| Reserve (10-15%) | Dry powder | Emergency + black swan buying |

### Multi-Tier Offer Quoting

```
SELL SIDE (example at $2.70 XCH):
  Tier 1: Sell 50 XCH @ $2.72  (0.6% above mid) -- tight, high fill
  Tier 2: Sell 75 XCH @ $2.75  (2.0% above mid) -- moderate
  Tier 3: Sell 100 XCH @ $2.84 (5.0% above mid) -- wide, inventory reduction
  Tier 4: Sell 200 XCH @ $2.97 (10% above mid)  -- black swan sell wall

BUY SIDE (mirror):
  Tier 1-4 at corresponding discounts below mid
```

### Rebalancing Triggers

| Trigger | Threshold | Action |
|---------|----------|--------|
| Price deviation | >2% from last rebalance | Cancel and re-post all tiers |
| Inventory skew | >60% one-sided | Asymmetric spread adjustment |
| Time decay | >1 hour stale | Refresh all offers |
| Volume spike | >3x average | Tighten spreads to capture fees |
| Volatility spike | >2x 7-day avg | Widen spreads 50-100% |

---

## 12. Smart Contract Architecture

### TibetSwap Pattern (Battle-Tested Foundation)

```
singleton_top_layer
  -> p2_merkle_tree (action selector)
    -> pair_inner_puzzle (state management)
      -> action puzzles (swap / add_liq / remove_liq)
```

### Constant-Product Formula in Chialisp

```clojure
;; output = (output_reserve * input * 993) / (input_reserve * 1000 + input * 993)
(defun-inline get_input_price (input_amount input_reserve output_reserve)
    (f (divmod
        (* output_reserve (* input_amount INVERSE_FEE))
        (+ (* input_reserve 1000) (* input_amount INVERSE_FEE)))))
```

### Security Checklist

- All solution values signed or committed via announcements
- Input sizes validated (`size_b32`) before hashing
- Spend bundle integrity via cross-coin announcement linking
- Rounding always favors the pool
- Flash loan scenarios analyzed
- Professional audit before mainnet deployment

---

## 13. Bot Architecture

### Python Async Architecture

```
xop_trader/
├── core/
│   ├── engine.py           # Main event loop (async, per-block heartbeat)
│   ├── config.py           # YAML/TOML configuration
│   └── state.py            # Global state machine
├── strategy/
│   ├── base.py             # Strategy interface (pluggable)
│   ├── avellaneda.py       # Avellaneda-Stoikov implementation
│   ├── glft.py             # GLFT with running inventory penalty
│   └── regime.py           # Regime detection (variance ratio + HMM)
├── execution/
│   ├── offer_manager.py    # Offer lifecycle (create/cancel/monitor)
│   ├── coin_manager.py     # Coin pool splitting and allocation
│   └── dex_clients/        # dexie, tibetswap, hashgreen adapters
├── risk/
│   ├── inventory.py        # Position tracking, cost basis (FIFO)
│   ├── limits.py           # Pre-trade checks, never-sell-at-loss
│   └── hedging.py          # 7-layer hedge stack
├── data/
│   ├── market_data.py      # Aggregated price feeds (CEX + DEX)
│   ├── volatility.py       # Yang-Zhang hybrid estimator
│   └── adverse_selection.py# Bayesian PIN estimator
├── monitoring/
│   ├── prometheus.py       # Metric exporter
│   ├── pnl.py              # PnL attribution (spread/inventory/fees)
│   └── alerts.py           # Telegram/Discord bot
└── tests/
    ├── unit/
    ├── integration/        # Chia testnet
    └── simulation/         # GPU-accelerated backtests
```

### Main Loop (Per-Block Heartbeat)

```python
async def on_new_block(block_data):
    # 1. Update market state (prices from CEX + DEX)
    # 2. Process any fills from this block
    # 3. Update volatility, PIN, regime estimates
    # 4. Compute optimal quotes (A-S/GLFT)
    # 5. Apply never-sell-at-loss constraint
    # 6. Apply Kelly position limits
    # 7. Cancel stale offers, post new ones
    # 8. Update PnL attribution
    # 9. Export metrics to Prometheus
```

---

## 14. Infrastructure

### System Architecture

```
┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐
│  Chia Full Node  │  │  Chia Wallet     │  │  CEX APIs       │
│  RPC :8555       │  │  RPC :9256       │  │  (OKX, Gate.io) │
└────────┬────────┘  └────────┬─────────┘  └────────┬────────┘
         │                    │                      │
         └────────────────────┼──────────────────────┘
                              │
                    ┌─────────▼──────────┐
                    │  XOPTrader Bot     │
                    │  (Python async)    │
                    └─────────┬──────────┘
                              │
         ┌────────────────────┼────────────────────┐
         │                    │                     │
┌────────▼───────┐  ┌────────▼───────┐  ┌──────────▼──────┐
│  PostgreSQL    │  │  Prometheus    │  │  Telegram Bot   │
│  (trade log)   │  │  (metrics)     │  │  (alerts)       │
└────────┬───────┘  └────────┬───────┘  └─────────────────┘
         │                    │
         └────────┬───────────┘
         ┌────────▼───────────┐
         │  Grafana           │
         │  (6 dashboards)    │
         └────────────────────┘
```

### Hardware Requirements

- Full node: 4+ cores, 8GB RAM, 500GB SSD
- Wallet: co-located with bot
- Multiple full nodes for redundancy recommended at scale

---

## 15. Monitoring & Operations

### 6 Grafana Dashboards

1. **Real-Time PnL** -- total, realized vs unrealized, per-pair breakdown
2. **Inventory** -- balances, cost basis vs market, position aging heatmap, underwater flags
3. **Market Data** -- prices, spreads, volumes across all DEXes
4. **System Health** -- node sync, wallet connectivity, offer latency
5. **Offer Lifecycle** -- pending/filled/cancelled/expired funnel, fill rate
6. **Risk Metrics** -- VaR, drawdown curve, exposure heatmap, concentration

### Alert Tiers

| Tier | Channel | Examples |
|------|---------|---------|
| CRITICAL | Telegram DM + Discord | Node desync, wallet unreachable, exposure breach |
| WARNING | Telegram + Discord | Spread widening, fill rate dropping, underwater position |
| INFO | Discord (batched hourly) | Daily PnL summary, volume anomaly |

### Audit Trail

PostgreSQL append-only `trade_log` table with full offer file archival -- ISO/IEC 27001 compliant.

---

## 16. Backtesting Framework

### Data Sources

- Historical offers from dexie.space API
- CEX reference prices from OKX/Gate.io
- On-chain data from Chia full node

### GPU-Accelerated Parameter Sweeps

Leverage existing GPU backtesting infrastructure (CUDA/CuPy/Numba) for:
- Monte Carlo simulation of MM strategies
- Walk-forward optimization (avoid overfitting)
- Parameter sweeps across (gamma, kappa, phi, spread_levels, TTL)

### Key Backtest Metrics

- Sharpe ratio, max drawdown, profit factor
- Fill rate, inventory turnover
- Adverse selection cost
- **Loss trade count = 0** (hard constraint verification)

---

## 17. Competitive Landscape

### Current State: Near-Zero Competition

- No known professional market makers on CHIA DEXes
- Existing liquidity is provided by retail LPs in TibetSwap pools
- AbandonedLand/MarketMaker is an open-source community bot (basic)
- **First-mover advantage is massive**

### Competitive Moats to Build

1. Fastest offer refresh cycle (tightest spreads)
2. Deepest order book across most pairs
3. Cross-venue presence (dexie + TibetSwap + Hashgreen simultaneously)
4. DBX incentive farming at scale
5. Relationships with CAT issuers for designated market making

---

## 18. Regulatory Considerations

### Key Points

- Market making on CHIA DEXes likely NOT an MSB (no custody of others' funds)
- XCH classification: likely commodity (proof-of-space, utility token)
- Every offer settlement is a taxable event -- track cost basis per coin
- Consider LLC structure for liability protection
- Maintain full trade log for tax compliance (Form 8949, FBAR if applicable)

---

## 19. Profitability Model

### Revenue Sources

| Source | Monthly Estimate ($10K capital) |
|--------|-------------------------------|
| Spread capture | $150-$400 |
| DBX incentives | $50-$150 |
| Arbitrage (CEX-DEX) | $50-$200 |
| AMM fee income | $30-$80 |
| **Total** | **$280-$830** |

### Cost Structure

| Cost | Monthly |
|------|---------|
| Infrastructure | ~$20 (VPS + node) |
| Blockchain fees | ~$5 (negligible) |
| Hedging costs | $0-$50 (mostly free layers) |
| **Total** | **~$25-$75** |

### Net Monthly Return

- **Conservative**: 1.5-3% ($150-$300 on $10K)
- **Moderate**: 3-5% ($300-$500)
- **Optimistic**: 5-8% ($500-$800)

---

## 20. Implementation Roadmap

### Phase 1: Foundation (Weeks 1-2)
- [ ] Set up Chia full node + wallet
- [ ] Clone and review XOPTrader repo
- [ ] Implement Chia RPC client (wallet + full node)
- [ ] Implement dexie.space API client
- [ ] Build coin management (splitting, tracking)
- [ ] Basic offer create/cancel/monitor loop

### Phase 2: Core Strategy (Weeks 3-4)
- [ ] Implement Avellaneda-Stoikov quote engine
- [ ] Add volatility estimator (Yang-Zhang hybrid CEX-DEX)
- [ ] Add cost basis tracker (FIFO)
- [ ] Implement never-sell-at-loss constraint
- [ ] Build multi-tier offer ladder
- [ ] Deploy on Chia testnet for paper trading

### Phase 3: Risk & Monitoring (Weeks 5-6)
- [ ] Implement inventory risk controls
- [ ] Set up Prometheus + Grafana dashboards
- [ ] Build Telegram alert bot
- [ ] PostgreSQL trade log + audit trail
- [ ] PnL attribution engine

### Phase 4: Live Deployment (Weeks 7-8)
- [ ] Deploy with minimal capital ($1K) on mainnet
- [ ] Start with 1-2 pairs (XCH/wUSDC, SBX/XCH)
- [ ] Calibrate parameters from live data
- [ ] Monitor fill rates, adverse selection, NHE

### Phase 5: Scaling (Months 3-6)
- [ ] Scale to $10K-$30K
- [ ] Add 4-6 more pairs
- [ ] Implement regime detection
- [ ] Add cross-DEX arbitrage
- [ ] Begin DBX incentive farming at scale
- [ ] Implement GPU backtesting for parameter optimization

### Phase 6: Dominance (Months 6-12)
- [ ] Become primary liquidity provider
- [ ] Add CEX hedging if needed
- [ ] Explore market-making-as-a-service for new CAT launches
- [ ] Participate in dexie governance to shape incentive structures

---

## Key Formulas Reference

| Component | Formula |
|-----------|---------|
| Reservation price | `r = S - q * gamma * sigma^2 * tau` |
| Optimal half-spread | `delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau` |
| Inventory skew | `skew = phi * q / q_max` |
| Adverse selection premium | `pi * sigma * sqrt(tau)` |
| Kelly position size | `f* = (spread - sigma*sqrt(tau)) / (sigma^2 * tau)` |
| Offer TTL upper bound | `TTL < (half_spread / sigma)^2 / block_time` |
| Block volatility | `sigma_block = sigma_annual * sqrt(52 / seconds_per_year)` |
| Fill intensity | `lambda(delta) = A * exp(-kappa * delta)` |
| Impermanent loss | `IL = (2*sqrt(r))/(1+r) - 1` where `r = new_price/old_price` |

---

*Generated by 20 parallel Claude research agents for XOPTrader*
*CONSTRAINT: All strategies enforce NEVER SELL AT A LOSS*
