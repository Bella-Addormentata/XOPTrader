# Market Making Strategies for Chia DEXs

## Table of Contents
1. [Market Making Fundamentals](#1-market-making-fundamentals)
2. [Strategy Options](#2-strategy-options)
3. [Dexie Offer-Based Market Making](#3-dexie-offer-based-market-making)
4. [TibetSwap LP-Based Market Making](#4-tibetswap-lp-based-market-making)
5. [Hybrid Strategies](#5-hybrid-strategies)
6. [Pricing and Spread Management](#6-pricing-and-spread-management)
7. [Inventory Management](#7-inventory-management)
8. [Risk Management](#8-risk-management)
9. [Performance Metrics](#9-performance-metrics)
10. [Strategy Comparison](#10-strategy-comparison)

---

## 1. Market Making Fundamentals

### What is Market Making?
A market maker provides liquidity to a market by simultaneously quoting buy (bid) and sell (ask) prices. The market maker earns the **bid-ask spread** as compensation for providing liquidity and bearing inventory risk.

**Core economics:**
- **Spread income**: Profit per round-trip trade = (ask - bid)
- **Inventory risk**: If prices move against held inventory, spread income may be wiped out
- **Volume**: More trades = more total spread income; need sufficient volume to cover costs

### Market Making on DEXs vs. CEXs
| Aspect | CEX Market Making | DEX (Chia Offers) Market Making |
|--------|------------------|---------------------------------|
| Order placement | API calls, near-instant | Create/submit offer files |
| Order cancellation | API call (fast) | Let offer expire or cancel (on-chain cost is zero for offer files) |
| Partial fills | Common | Not supported (all-or-nothing) |
| Price discovery | Real-time order book | Must query external price feeds |
| Transparency | Order book visible | Offers visible on Dexie |
| Custody | Exchange holds funds | User retains custody at all times |

### Chia-Specific Constraints
1. **No partial fills**: Offers must be fully filled. Plan lot sizes accordingly.
2. **Coin locking**: Creating an offer "locks" those coins until filled or canceled.
3. **Coinset model**: Managing UTXOs (coins) requires careful coin selection.
4. **Offer validity**: Offers have no automatic expiry unless a block height is specified.
5. **Transaction finality**: ~52-second block times; faster for low-value heuristic acceptance.

---

## 2. Strategy Options

### Strategy 2.1: Basic Spread Market Making (Dexie Offers)
**Concept**: Post symmetric bid and ask offers around the mid-market price, capturing the spread when both sides fill over time.

**How it works:**
1. Fetch current mid-market price (from CEX data, oracle, or TibetSwap quotes).
2. Post a buy offer at `mid * (1 - spread/2)`.
3. Post a sell offer at `mid * (1 + spread/2)`.
4. When one side fills, repost both sides at the updated mid price.
5. Collect the spread as profit.

**Suitable for**: Liquid pairs like XCH/USDS where external price reference exists.

**Advantages:**
- Simple to implement
- Earns DBX rewards on Dexie if on incentivized pairs
- Low capital requirement

**Disadvantages:**
- All-or-nothing fills (Chia offers are atomic)
- Exposure to adverse selection (informed traders fill one side)
- Requires active offer management

---

### Strategy 2.2: Grid Trading Strategy
**Concept**: Place multiple buy and sell offers at regular price intervals (a "grid"). The bot profits from oscillations as price moves up and down through the grid levels.

**How it works:**
1. Define a price range `[lower_price, upper_price]` and number of grid levels `N`.
2. Calculate grid step: `step = (upper_price - lower_price) / N`.
3. Place buy offers at prices: `lower_price, lower_price + step, ..., mid - step`.
4. Place sell offers at prices: `mid + step, ..., upper_price`.
5. When a buy offer fills, replace it with a sell offer one step higher (and vice versa).

**Grid parameters:**
| Parameter | Description | Typical Value |
|-----------|-------------|--------------|
| `N` (levels) | Number of grid intervals | 10–50 |
| `range` | Width of price range | 20%–100% of mid |
| `lot_size` | Asset amount per grid level | 0.1–1 XCH equivalent |
| `step_type` | Arithmetic or geometric spacing | Geometric recommended |

**Suitable for**: Sideways, range-bound markets with moderate volatility.

**Advantages:**
- Profits from price oscillation without directional bias
- Automated, works without constant monitoring
- Can be tuned to match observed volatility

**Disadvantages:**
- Underperforms in strongly trending markets (all buys fill as price drops; all sells fill as price rises → capital depleted)
- Requires more capital to cover all grid levels
- Chia offer atomicity limits granularity

---

### Strategy 2.3: Inventory-Based Market Making (Avellaneda-Stoikov)
**Concept**: Dynamically adjust bid-ask spread and quotes based on current inventory to minimize inventory risk.

**Core model** (simplified Avellaneda-Stoikov):
```
reservation_price = mid_price - gamma * inventory * sigma^2 * T
optimal_spread    = gamma * sigma^2 * T + (2/gamma) * ln(1 + gamma/kappa)
bid = reservation_price - optimal_spread / 2
ask = reservation_price + optimal_spread / 2
```

Where:
- `gamma` = risk aversion parameter (higher = wider spreads)
- `inventory` = current net inventory in base asset
- `sigma` = price volatility estimate
- `T` = time horizon
- `kappa` = order arrival rate

**Suitable for**: Any trading pair where you can estimate volatility and arrival rates.

**Advantages:**
- Theoretically optimal under certain assumptions
- Automatically manages inventory risk
- Adapts to changing market conditions

**Disadvantages:**
- Requires volatility estimation
- Complex calibration
- More difficult to implement correctly

---

### Strategy 2.4: Passive Liquidity Provision (TibetSwap AMM)
**Concept**: Deposit assets into a TibetSwap liquidity pool and passively earn swap fees from all trades in the pool.

**How it works:**
1. Choose a pool (e.g., XCH/USDS).
2. Deposit equal values of both tokens (or use single-sided entry).
3. Receive LP tokens representing your share of the pool.
4. Earn a pro-rata share of the 0.3% fee on every swap through the pool.
5. Withdraw at any time by burning LP tokens.

**Returns calculation:**
```
fee_income = volume_daily * 0.003 * your_pool_share
impermanent_loss = f(price_change_ratio)
net_return = fee_income - impermanent_loss
```

**Suitable for**: Long-term passive income; works best in high-volume, low-volatility pairs.

**Advantages:**
- Fully passive—no active management required
- No coin management complexity
- Earns from every trade in the pool

**Disadvantages:**
- **Impermanent loss** can outweigh fee income in volatile markets
- No control over pricing or spread
- Capital locked in the pool contract

---

### Strategy 2.5: Arbitrage Between Dexie and TibetSwap
**Concept**: Exploit price discrepancies between the Dexie offer orderbook and TibetSwap AMM prices.

**How it works:**
1. Continuously query Dexie for best offer prices and TibetSwap for AMM spot prices.
2. When a significant price discrepancy exists (`|dexie_price - tibetswap_price| > threshold`):
   - Buy the cheaper side
   - Sell on the more expensive side
3. The trades push prices toward equilibrium, earning a risk-free (or near-risk-free) profit.

**Suitable for**: Any pair listed on both Dexie and TibetSwap.

**Advantages:**
- Near risk-free when properly implemented
- Helps keep DEX prices efficient
- No directional market bias needed

**Disadvantages:**
- Fast execution required (others may front-run)
- Chia's block time (~52 seconds) limits pure cross-venue arbitrage
- Opportunities may be short-lived as others also arbitrage

---

### Strategy 2.6: Cross-DEX / Cross-Chain Arbitrage
**Concept**: Exploit price differences for XCH or Chia-wrapped assets between Chia DEXs and centralized exchanges.

**How it works:**
1. Monitor XCH price on CEXs (OKX, MEXC, Gate.io).
2. Monitor XCH/USDS price on TibetSwap or Dexie.
3. When CEX price > DEX price by more than fees + slippage:
   - Buy XCH on DEX
   - Sell XCH on CEX
4. Reverse when DEX price is higher.

**Suitable for**: Well-capitalized bots with CEX accounts and Chia wallets.

**Advantages:**
- Exploits the price lag between on-chain and off-chain markets
- Can be very profitable during volatile periods

**Disadvantages:**
- Requires CEX API integration + Chia DEX integration
- Withdrawal/deposit delays from CEX create timing risk
- CEX account risk (counterparty risk)

---

## 3. Dexie Offer-Based Market Making

### Offer Lifecycle Management

```
┌─────────────────────────────────────────────────────────────────┐
│                     Offer Lifecycle                              │
├─────────────────────────────────────────────────────────────────┤
│  1. CREATE   → Build offer file (lock coins)                     │
│  2. SUBMIT   → POST to Dexie API                                 │
│  3. MONITOR  → Poll Dexie API for fill status                    │
│  4. FILLED   → Coins received; repost new offer                  │
│  5. STALE    → Price moved; cancel old offer, post new one       │
│  6. CANCEL   → Submit cancellation spend bundle                  │
└─────────────────────────────────────────────────────────────────┘
```

### Offer Creation
Use the Chia wallet RPC or `chia-wallet-sdk` to create offer files:

```python
# Pseudocode for offer creation via Chia wallet RPC
offer_params = {
    "offer": {
        XCH_ASSET_ID: -100_000_000_000,  # offering 0.1 XCH (negative = offered)
        USDS_ASSET_ID: 10_000_000,       # requesting 10 USDS (positive = requested)
    },
    "fee": 0,                             # Optional: blockchain fee in mojos
    "min_coin_amount": 1000,
}
response = wallet_rpc.create_offer_for_ids(offer_params)
offer_file = response["offer"]
```

### Dexie API Integration
```python
import httpx

DEXIE_API = "https://api.dexie.space/v1"

# Submit offer
async def submit_offer(offer_str: str) -> dict:
    async with httpx.AsyncClient() as client:
        response = await client.post(
            f"{DEXIE_API}/offers",
            json={"offer": offer_str, "drop_only": False}
        )
        return response.json()

# Monitor offer status
async def get_offer_status(offer_id: str) -> str:
    async with httpx.AsyncClient() as client:
        response = await client.get(f"{DEXIE_API}/offers/{offer_id}")
        return response.json()["offer"]["status"]
        # Status values: "active", "pending", "cancelled", "completed"

# Stream real-time updates
async def stream_offers():
    import websockets
    async with websockets.connect("wss://api.dexie.space/v1/stream") as ws:
        async for message in ws:
            offer_update = json.loads(message)
            # Process offer fills, new offers, etc.
```

### Offer Expiry Management
Set a block height expiry to auto-invalidate stale offers:

```python
# Calculate expiry block height for ~1 hour from now
# Chia produces ~1 block per 52 seconds → ~69 blocks/hour
BLOCKS_PER_HOUR = 69
current_height = get_current_block_height()
expiry_height = current_height + BLOCKS_PER_HOUR

offer_params["min_height"] = None
offer_params["max_height"] = expiry_height
```

### Dexie Liquidity Rewards
Earn DBX tokens by maintaining offers on incentivized pairs:
- Reward pairs are announced on [dexie.space/docs](https://dexie.space/docs)
- Rewards are based on: spread width, offer depth, and uptime
- Use `dexie-rewards` CLI or direct API to claim:

```bash
# Install the rewards tool
pip install dexie-rewards

# List claimable rewards
dexie rewards list --fingerprint <WALLET_FINGERPRINT>

# Claim all rewards
dexie rewards claim --fingerprint <WALLET_FINGERPRINT> --yes
```

---

## 4. TibetSwap LP-Based Market Making

### Adding Liquidity
Interaction with TibetSwap pools is done via their API and on-chain transactions:

1. **Fetch pool info**: Query TibetSwap API for current pool reserves and exchange rate.
2. **Calculate deposit amounts**: Match the current pool ratio to minimize price impact.
3. **Submit add-liquidity transaction**: Spend coins into the pool singleton.
4. **Receive LP tokens**: TIBET-{CAT} tokens representing your pool share.

### Impermanent Loss Analysis
For a pool with initial price `P₀`, when price changes to `P₁`:

```
IL = 2 * sqrt(P₁/P₀) / (1 + P₁/P₀) - 1
```

| Price Change | Impermanent Loss |
|-------------|-----------------|
| 0% | 0% |
| ±25% | -0.6% |
| ±50% | -2.0% |
| ±100% (2x) | -5.7% |
| ±200% (3x) | -13.4% |
| ±400% (5x) | -25.5% |

### Fee APR Estimation
```
daily_fee_income = daily_volume * 0.003 * your_pool_share_fraction
annual_fee_income = daily_fee_income * 365
fee_APR = annual_fee_income / total_value_deposited * 100
```

### When LP is Profitable
LP is profitable when: `fee_income > impermanent_loss`

This is most favorable when:
- Trading volume is high relative to pool size
- Price of the pair is range-bound
- You have a long time horizon (fees accumulate)

---

## 5. Hybrid Strategies

### Hybrid 5.1: Dexie Active + TibetSwap Passive
- Deploy a portion of capital as TibetSwap LP (passive fee income)
- Deploy remaining capital as active Dexie offers (spread income + DBX rewards)
- The TibetSwap position provides a "floor" of passive income
- The Dexie position captures additional spread and rewards

### Hybrid 5.2: Wide Offers + AMM Arbitrage
- Post wide Dexie offers (5-10% spread) that activate only on larger price moves
- Simultaneously run an arbitrage bot between Dexie and TibetSwap for tighter spreads
- The wide offers act as a backstop for large market moves
- The arbitrage component captures opportunities from TibetSwap price discrepancies

---

## 6. Pricing and Spread Management

### Price Discovery Sources
Chia DEXs lack native price oracles; use these external sources:

1. **CEX Price Feeds** (most reliable for XCH):
   - OKX: `GET https://www.okx.com/api/v5/market/ticker?instId=XCH-USDT`
   - MEXC, Gate.io, Binance (via their respective APIs)

2. **TibetSwap AMM Spot Price** (for CAT prices):
   ```
   spot_price = pool_reserve_XCH / pool_reserve_CAT
   ```

3. **Weighted Average of Multiple Sources**:
   ```python
   mid_price = 0.6 * cex_price + 0.4 * tibetswap_price
   ```

### Spread Sizing Guidelines
| Market Condition | Recommended Spread |
|-----------------|-------------------|
| Stable, high volume (XCH/USDS) | 0.5% – 1.0% |
| Moderate volume (XCH/WBTC.b) | 1.0% – 2.0% |
| Low volume CAT pairs | 2.0% – 5.0% |
| High volatility | +50% to base spread |
| Low volatility | Base spread |

### Dynamic Spread Adjustment
Widen spreads when:
- Volatility (σ) is high (measure 1h rolling standard deviation of price)
- Inventory is heavily skewed to one side
- Order flow is toxic (consecutive adverse fills)

Tighten spreads when:
- Volatility is low
- Inventory is balanced
- DBX reward eligibility requires tighter quotes

---

## 7. Inventory Management

### Dual-Currency Inventory
A market maker must hold both sides of a trading pair:
- **Base asset** (e.g., XCH): Needed to fill buy orders from takers
- **Quote asset** (e.g., USDS): Needed to fill sell orders from takers

### Target Inventory Ratio
Maintain a target ratio to avoid one-sided depletion:
```
target_fraction = 0.5  # 50% in each asset
current_fraction = base_value / (base_value + quote_value)
skew = current_fraction - target_fraction
```

### Rebalancing Triggers
Rebalance when `|skew| > rebalance_threshold` (e.g., 20%):
1. **Via a single large offer**: Post a correcting offer to shift inventory balance.
2. **Via CEX trade**: If paired with a CEX account, trade to rebalance.
3. **Via TibetSwap swap**: Execute a swap to rebalance (incurs AMM fee + slippage).

### Coin Management (UTXO Management)
Chia's coinset model requires managing individual coins:

```python
# Coin consolidation: merge many small coins into one large coin
# Coin splitting: break large coins into smaller coins for offer creation

def get_available_coins(asset_id: str) -> list[Coin]:
    """Get coins not currently locked in offers."""
    all_coins = wallet.get_coins(asset_id)
    locked_coins = offer_manager.get_locked_coins()
    return [c for c in all_coins if c.name not in locked_coins]

def select_coins_for_offer(coins: list[Coin], amount: int) -> list[Coin]:
    """Select coins summing to at least `amount`."""
    sorted_coins = sorted(coins, key=lambda c: c.amount, reverse=True)
    selected, total = [], 0
    for coin in sorted_coins:
        if total >= amount:
            break
        selected.append(coin)
        total += coin.amount
    return selected if total >= amount else []
```

---

## 8. Risk Management

### Position Limits
```python
MAX_INVENTORY_XCH = 100  # Maximum XCH to hold in market making inventory
MAX_OFFER_SIZE_XCH = 10  # Maximum size per individual offer
MAX_TOTAL_OFFERS = 20    # Maximum concurrent open offers
MAX_DAILY_LOSS_USD = 500 # Stop trading if daily loss exceeds this
```

### Stop-Loss Conditions
Pause market making when:
1. **Daily PnL loss limit reached**: Stop all activities, alert operator.
2. **Extreme volatility**: Pause when 1h price move > 10%.
3. **Node disconnection**: Pause when Chia node is not synced.
4. **Low inventory**: If one side < 10% of target, stop posting that side.
5. **API errors**: Multiple consecutive API failures trigger a pause.

### Offer Staleness Management
```python
MAX_OFFER_AGE_BLOCKS = 200  # ~3 hours at 52s/block
PRICE_DEVIATION_THRESHOLD = 0.02  # 2% price move triggers offer replacement

def should_refresh_offer(offer: Offer, current_mid: float) -> bool:
    offer_mid = (offer.ask + offer.bid) / 2
    price_deviation = abs(current_mid - offer_mid) / offer_mid
    offer_age_blocks = current_height - offer.submitted_at_height
    return (price_deviation > PRICE_DEVIATION_THRESHOLD or
            offer_age_blocks > MAX_OFFER_AGE_BLOCKS)
```

---

## 9. Performance Metrics

### Key Performance Indicators (KPIs)

| Metric | Formula | Target |
|--------|---------|--------|
| **Spread Capture Rate** | Filled pairs / Total pairs posted | > 30% |
| **Inventory Turnover** | Volume traded / Average inventory value | > 1x/day |
| **Net PnL** | Revenue – Costs – Losses | > 0 |
| **Fee Income** | Sum of spread earned on all fills | Maximize |
| **DBX Earnings** | DBX rewards earned | Maximize on eligible pairs |
| **Impermanent Loss** | IL% on TibetSwap positions | Minimize |
| **Uptime** | % of time bot is actively quoting | > 95% |
| **Fill Rate** | % of posted offers that get filled | Track for optimization |

### PnL Attribution
```
total_pnl = spread_income
           + dbx_reward_value
           + tibetswap_fee_income
           - impermanent_loss
           - rebalancing_costs
           - operational_costs (infra, dev time)
```

---

## 10. Strategy Comparison

| Strategy | Complexity | Capital Req. | Risk Level | Profit Potential | Best For |
|----------|-----------|-------------|------------|-----------------|---------|
| Basic Spread (Dexie) | Low | Low | Medium | Low-Medium | Getting started |
| Grid Trading (Dexie) | Medium | Medium | Medium | Medium | Range markets |
| Avellaneda-Stoikov | High | Medium | Low-Medium | Medium-High | Experienced bots |
| TibetSwap LP | Low | Medium | Low-Medium | Low-Medium | Passive income |
| Arbitrage (Dexie↔Tibet) | Medium | Low | Low | Low-Medium | Fast execution |
| Cross-DEX/CEX Arb | High | High | Low-Medium | High | Well-capitalized |
| Hybrid Active+Passive | High | High | Low | Medium-High | Scaling up |

### Recommended Starting Strategy
For a new market maker on Chia:

1. **Start**: Basic spread market making on XCH/USDS via Dexie
2. **Optimize**: Add dynamic spread adjustment based on volatility
3. **Scale**: Expand to additional pairs (XCH/DBX for reward eligibility)
4. **Diversify**: Add TibetSwap LP position for passive fee income
5. **Advanced**: Implement Avellaneda-Stoikov model and cross-venue arbitrage
