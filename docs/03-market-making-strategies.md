# Market Making Strategies for Chia DEX

## Overview

This document outlines practical market making strategies for Chia decentralized exchanges, including both AMM-based and offer-based approaches.

---

## Table of Contents

1. [Strategy Framework](#strategy-framework)
2. [AMM Liquidity Provider Strategies](#amm-liquidity-provider-strategies)
3. [Offer-Based Market Making Strategies](#offer-based-market-making-strategies)
4. [Hybrid Strategies](#hybrid-strategies)
5. [Risk Management](#risk-management)
6. [Performance Optimization](#performance-optimization)

---

## Strategy Framework

### Core Market Making Principles

Regardless of platform, successful market making requires:

1. **Liquidity Provision**: Making assets available for trading
2. **Spread Capture**: Earning the difference between buy and sell prices
3. **Inventory Management**: Balancing long and short positions
4. **Risk Control**: Limiting exposure to adverse price movements
5. **Operational Excellence**: Reliable execution and monitoring

### Strategy Selection Matrix

| Strategy Type | Capital Required | Time Commitment | Technical Complexity | Expected Return |
|---------------|------------------|-----------------|---------------------|-----------------|
| Simple LP | Medium | Low | Low | Low-Medium |
| Active LP | High | Medium | Medium | Medium |
| Basic Offer MM | Low | High | Medium | Medium |
| Advanced Offer MM | Medium | Medium | High | Medium-High |
| Hybrid | High | Medium-High | High | High |
| Arbitrage | High | High | Very High | Variable |

---

## AMM Liquidity Provider Strategies

### Strategy 1: Simple Passive LP

**Objective**: Earn trading fees with minimal management

**Approach**:
```
1. Select high-volume pair (e.g., XCH/USDC)
2. Deposit equal value of both tokens
3. Hold LP position long-term
4. Periodically collect fees
5. Rebalance quarterly or when IL exceeds 5%
```

**Best For**:
- Beginners
- Long-term holders of both assets
- Low time commitment investors

**Expected Returns**: 5-20% APR depending on volume

**Risks**:
- Impermanent loss
- Smart contract vulnerabilities
- Pool imbalances

**Example Configuration**:
```yaml
Pair: XCH/USDC
Initial: 10 XCH + $500 USDC
Strategy: Hold for 6 months
Rebalance: If IL > 5% or quarterly
Exit: After 6 months or if pool TVL drops 50%
```

---

### Strategy 2: Active LP Rotation

**Objective**: Maximize fee income by rotating between high-fee pools

**Approach**:
```
1. Monitor pool metrics daily (volume, fees, TVL)
2. Calculate fee APR for each pool
3. Allocate to top 2-3 pools by fee generation
4. Rotate weekly to highest performers
5. Compound fees into larger positions
```

**Best For**:
- Active investors
- Those comfortable with gas fees
- Medium to high capital

**Expected Returns**: 15-40% APR

**Risks**:
- Transaction costs eating into profits
- Timing risk on entries/exits
- Multiple IL events

**Pool Scoring Formula**:
```
Pool Score = (24h Volume * Fee %) / TVL * 365

Example:
Pool A: ($10,000 * 0.3%) / $100,000 * 365 = 10.95% APR
Pool B: ($5,000 * 0.3%) / $30,000 * 365 = 18.25% APR
→ Choose Pool B
```

---

### Strategy 3: Concentrated Liquidity (If Available)

**Objective**: Amplify returns by concentrating liquidity in active price ranges

**Note**: As of 2026, TibetSwap may or may not support concentrated liquidity (Uniswap V3 style). Check current features.

**Approach**:
```
1. Analyze historical price range (e.g., XCH $45-$55)
2. Set tight liquidity range (e.g., $48-$52)
3. Earn amplified fees when price is in range
4. Rebalance when price exits range
5. Adjust range weekly based on volatility
```

**Best For**:
- Advanced users
- High trading volume pairs
- Stable or range-bound assets

**Expected Returns**: 30-100%+ APR (when in range)

**Risks**:
- Out-of-range (no fees earned)
- Frequent rebalancing costs
- Higher impermanent loss

---

## Offer-Based Market Making Strategies

### Strategy 4: Basic Spread Capture

**Objective**: Post buy and sell offers to capture bid-ask spread

**Approach**:
```
1. Determine current market price (use CoinGecko, CEX)
2. Calculate comfortable spread (2-5%)
3. Post buy offer at -2% to -3%
4. Post sell offer at +2% to +3%
5. Wait for fills
6. Refresh offers every 2-4 hours
```

**Example**:
```
Market Price: 1 XCH = $50

Your Offers:
BUY:  1 XCH for $48.50 (3% below)
SELL: 1 XCH for $51.50 (3% above)

Target Spread: $3 profit per full cycle (6% return)
```

**Best For**:
- Small to medium capital
- Manual traders
- Learning market making

**Expected Returns**: 10-30% annual (if 1-2 cycles per week)

**Risks**:
- Inventory accumulation (stuck in one asset)
- Market moves before both sides fill
- Offer management overhead

---

### Strategy 5: Layered Order Book Simulation

**Objective**: Create depth with multiple offers at various price levels

**Approach**:
```
1. Divide capital into 5-10 tranches
2. Post offers at multiple price levels
3. Create artificial order book depth
4. Tighter spreads near market price
5. Wider spreads further away
```

**Example Offer Structure**:
```
SELL Offers (XCH → USDC):
$53.00: 0.5 XCH (6% above market)
$52.00: 1.0 XCH (4% above market)
$51.00: 2.0 XCH (2% above market)

Current Market: $50.00

BUY Offers (USDC → XCH):
$49.00: 2.0 XCH (2% below market)
$48.00: 1.0 XCH (4% below market)
$47.00: 0.5 XCH (6% below market)
```

**Benefits**:
- Higher probability of fills
- Better inventory management
- Capture large price movements
- Provide genuine market depth

**Best For**:
- Medium to large capital
- Automated systems
- Professional market makers

**Expected Returns**: 20-50% annual

---

### Strategy 6: Dynamic Spread Adjustment

**Objective**: Adjust spreads based on market volatility and conditions

**Approach**:
```
1. Calculate historical volatility (7-day, 30-day)
2. Widen spreads during high volatility
3. Tighten spreads during low volatility
4. Adjust position sizes inversely to volatility
5. Use real-time price feeds
```

**Volatility-Based Spread Table**:
```
Volatility (7d) | Min Spread | Position Size
----------------|------------|---------------
< 5%           | 1.0%       | 100%
5-10%          | 2.0%       | 80%
10-20%         | 3.5%       | 60%
20-30%         | 5.0%       | 40%
> 30%          | 7.5%       | 20%
```

**Best For**:
- Experienced traders
- Automated systems
- Risk-averse market makers

**Expected Returns**: 15-40% annual with lower risk

---

### Strategy 7: Automated Market Making Bot

**Objective**: Fully automated offer management and execution

**Approach**:
```
1. Deploy bot with Chia wallet integration
2. Configure pricing sources (APIs)
3. Set spread parameters
4. Define inventory limits
5. Implement auto-rebalancing
6. Monitor and adjust parameters
```

**Bot Features**:
```python
# Pseudo-code structure
class ChiaMarketMaker:
    def __init__(self):
        self.wallet = ChiaWallet()
        self.dex = [Dexie(), OfferBin()]
        self.spread = 0.03  # 3%
        self.max_position = 100  # XCH

    def get_market_price(self):
        # Fetch from CoinGecko, CEX

    def calculate_offers(self):
        # Determine bid/ask prices

    def post_offers(self):
        # Create and post to multiple DEXs

    def monitor_fills(self):
        # Track executed offers

    def rebalance(self):
        # Adjust inventory
```

**Best For**:
- Technical users
- Serious market makers
- High capital deployment

**Expected Returns**: 30-100%+ annual

**Requirements**:
- Programming skills (Python)
- Server/VPS for 24/7 operation
- API integrations
- Monitoring infrastructure

---

## Hybrid Strategies

### Strategy 8: AMM + Offer Combination

**Objective**: Use both AMM pools and active offers for diversification

**Capital Allocation**:
```
60% → TibetSwap LP (passive income)
40% → Active offers on Dexie (active management)
```

**Benefits**:
- Diversified income streams
- Reduced overall risk
- Flexibility in market conditions

**When AMM is Better**:
- High trading volume periods
- Stable prices (low IL)
- Hands-off management preferred

**When Offers are Better**:
- Low volume (better spreads available)
- Volatile prices (avoid IL)
- Active management possible

---

### Strategy 9: Cross-Platform Arbitrage

**Objective**: Exploit price differences between Chia DEXs and CEXs

**Approach**:
```
1. Monitor prices on:
   - Dexie
   - TibetSwap
   - Gate.io (CEX)
   - OKX (CEX)

2. When price difference > (fees + spread):
   - Buy on cheaper venue
   - Sell on expensive venue
   - Capture difference

3. Repeat continuously
```

**Example Arbitrage**:
```
Dexie:  1 XCH = $49.00
Gate.io: 1 XCH = $50.50
Spread: $1.50 (3%)

Action:
1. Buy XCH on Dexie for $49
2. Transfer to Gate.io (or sell wXCH if bridged)
3. Sell for $50.50
4. Profit: $1.50 - fees

If fees < $0.75, profitable
```

**Requirements**:
- Fast execution
- Low transfer fees
- Capital on both venues
- Automated monitoring

**Expected Returns**: Highly variable, 50-200%+ possible but inconsistent

---

## Risk Management

### Position Sizing

**Never risk more than you can afford to lose**

Conservative approach:
```
- Start with 5-10% of crypto portfolio
- Maximum 20% in any single strategy
- Keep 30% in stablecoins for opportunities
```

### Inventory Limits

Prevent getting stuck in one asset:

```python
# Example inventory rules
max_xch_position = 100  # Never hold > 100 XCH
max_usd_position = 5000  # Never hold > $5000 stables
rebalance_threshold = 0.7  # Rebalance if ratio < 0.7

if xch_holdings / target_xch > rebalance_threshold:
    # Post more sell offers
if usd_holdings / target_usd > rebalance_threshold:
    # Post more buy offers
```

### Stop Loss / Circuit Breakers

Protect against extreme events:

```
1. Pause trading if 24h price move > 30%
2. Cancel all offers if volatility spikes
3. Withdraw from AMM if IL > 10%
4. Reduce position sizes during uncertainty
```

### Monitoring and Alerts

Set up notifications for:
- Large price movements (±10%)
- Unusual volume spikes
- Failed transactions
- Low inventory levels
- Filled offers
- Smart contract events

---

## Performance Optimization

### Metrics to Track

```
1. Total Return (%)
2. Sharpe Ratio (return/volatility)
3. Fill Rate (% of offers filled)
4. Average Spread Captured
5. Inventory Turnover
6. Fee Costs vs. Revenue
7. Impermanent Loss (for AMM)
8. Capital Efficiency
```

### Strategy Adjustments

**Monthly Review**:
- Analyze which strategies performed best
- Adjust capital allocation
- Update spread parameters
- Review competitive landscape

**Quarterly Optimization**:
- Backtest alternative parameters
- Research new platforms/pairs
- Update technology stack
- Tax planning and reporting

---

## Strategy Comparison Summary

| Strategy | Complexity | Returns | Time | Best Use Case |
|----------|------------|---------|------|---------------|
| Simple LP | ⭐ | ⭐⭐ | ⭐ | Passive income |
| Active LP | ⭐⭐ | ⭐⭐⭐ | ⭐⭐ | Fee maximization |
| Basic Offer | ⭐⭐ | ⭐⭐ | ⭐⭐⭐ | Learning MM |
| Layered Offers | ⭐⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ | Professional MM |
| Dynamic Spread | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐ | Risk-adjusted |
| Automated Bot | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐ | Scale operations |
| Hybrid | ⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐ | Diversification |
| Arbitrage | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | Exploit inefficiencies |

---

## Getting Started Recommendation

**For Beginners**:
1. Start with Strategy 1 (Simple LP) with small capital ($500-1000)
2. Learn for 1-2 months
3. Gradually try Strategy 4 (Basic Offers) with $200-500
4. Monitor and learn from results

**For Intermediate**:
1. Implement Strategy 5 (Layered Offers) with automation
2. Add Strategy 2 (Active LP) for diversification
3. Build monitoring tools
4. Scale capital gradually

**For Advanced**:
1. Develop Strategy 7 (Automated Bot)
2. Implement Strategy 8 (Hybrid)
3. Explore Strategy 9 (Arbitrage)
4. Optimize continuously

---

## Next Steps

The next document covers technical implementation details including:
- Setting up Chia wallet and tools
- Writing market making bots
- API integrations
- Deployment and operations
- Security best practices
