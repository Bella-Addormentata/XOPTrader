# Implementation Roadmap: XOPTrader Chia Market Maker

## Table of Contents
1. [Project Scope and Goals](#1-project-scope-and-goals)
2. [Prerequisites and Setup](#2-prerequisites-and-setup)
3. [Phase 1: Foundation (Weeks 1-2)](#3-phase-1-foundation-weeks-1-2)
4. [Phase 2: Core Market Making (Weeks 3-4)](#4-phase-2-core-market-making-weeks-3-4)
5. [Phase 3: Strategy Optimization (Weeks 5-6)](#5-phase-3-strategy-optimization-weeks-5-6)
6. [Phase 4: Hardening and Scale (Weeks 7-8)](#6-phase-4-hardening-and-scale-weeks-7-8)
7. [Cost and Capital Requirements](#7-cost-and-capital-requirements)
8. [Decision Points and Trade-offs](#8-decision-points-and-trade-offs)
9. [Testing Strategy](#9-testing-strategy)
10. [Launch Checklist](#10-launch-checklist)

---

## 1. Project Scope and Goals

### Vision
Build an automated market maker (XOPTrader) that operates on Chia blockchain decentralized exchanges—primarily Dexie and TibetSwap—to provide liquidity, earn spread income and DBX rewards, and contribute to Chia DeFi ecosystem health.

### Success Metrics
| Metric | Target | Measurement |
|--------|--------|-------------|
| Monthly PnL | > $0 net positive | PnL tracking dashboard |
| Uptime | > 95% | Monitoring alerts |
| Active offers | ≥ 2 pairs maintained | Offer count metric |
| DBX rewards | Claim all available | Reward dashboard |
| Impermanent loss | < fee income | IL calculator |

### Out of Scope (v1)
- Centralized exchange integration for cross-venue arbitrage
- Non-XCH-paired CAT/CAT trading
- NFT market making
- Multi-wallet operation

---

## 2. Prerequisites and Setup

### Knowledge Prerequisites
Before starting development, ensure familiarity with:
- [ ] Chia blockchain concepts (coins, puzzles, offers) – see `docs/01-chia-ecosystem-overview.md`
- [ ] Chialisp fundamentals (not required to write, but need to read)
- [ ] Python asyncio programming
- [ ] Basic DeFi concepts (AMM, liquidity provision, impermanent loss)
- [ ] Market making concepts (spread, inventory, bid-ask) – see `docs/02-market-making-strategies.md`

### Hardware/Infrastructure Requirements
```
Development Machine:
  - Any modern laptop/desktop
  - NOT recommended to sync mainnet full node locally (takes days, 300+ GB)
  - Use Chia testnet (testnet11) for development

Production Server:
  - 2+ vCPUs
  - 8+ GB RAM
  - 500 GB SSD
  - Ubuntu 22.04 LTS recommended
  - Static IP or domain name
  - ~$30-50/month hosting cost
```

### Software Prerequisites
```bash
# Python 3.11+
python3 --version

# Chia blockchain (latest stable)
pip install chia-blockchain
chia version

# Development tools
pip install pytest pytest-asyncio black isort mypy

# Optional: Docker for containerized deployment
docker --version
```

### Chia Testnet Setup
**Always test on testnet11 before mainnet deployment.**

```bash
# Configure Chia for testnet
chia configure --testnet true
chia init

# Start testnet services
chia start node wallet

# Get testnet XCH from faucet (free testnet coins)
# Faucet: https://testnet11-faucet.chia.net/
chia wallet get_address

# Wait for sync (faster than mainnet, ~few hours)
chia show -s
```

---

## 3. Phase 1: Foundation (Weeks 1-2)

### Week 1: Environment and Basic Connectivity

#### Milestone 1.1: Chia Node Running
- [ ] Set up testnet Chia full node (or connect to trusted node)
- [ ] Wallet synced and receiving testnet XCH
- [ ] Verify wallet RPC is accessible:
  ```bash
  curl --insecure \
    --cert ~/.chia/testnet11/config/ssl/wallet/private_wallet.crt \
    --key ~/.chia/testnet11/config/ssl/wallet/private_wallet.key \
    -d '{}' https://localhost:9256/get_wallets
  ```

#### Milestone 1.2: Repository Structure
Create the initial project structure:
```
XOPTrader/
├── xoptrader/
│   ├── __init__.py
│   ├── config.py           # Configuration management
│   ├── wallet.py           # Chia wallet RPC client
│   ├── dexie.py            # Dexie API client
│   ├── tibetswap.py        # TibetSwap API client
│   ├── oracle.py           # Price oracle
│   ├── offers.py           # Offer lifecycle manager
│   ├── inventory.py        # Inventory manager
│   ├── strategy.py         # Strategy engine
│   └── bot.py              # Main bot orchestrator
├── tests/
│   ├── test_wallet.py
│   ├── test_dexie.py
│   ├── test_oracle.py
│   └── test_strategy.py
├── docs/
│   ├── 01-chia-ecosystem-overview.md
│   ├── 02-market-making-strategies.md
│   ├── 03-technical-architecture.md
│   └── 04-implementation-roadmap.md  (this file)
├── config.example.toml     # Example configuration
├── requirements.txt
├── requirements-dev.txt
├── Makefile
└── README.md
```

#### Milestone 1.3: Wallet RPC Client
Implement and test the Chia Wallet RPC client:
- [ ] `get_wallets()` – List all wallets
- [ ] `get_wallet_balance(wallet_id)` – Get balance
- [ ] `get_spendable_coins(wallet_id)` – List available coins
- [ ] `create_offer_for_ids(params)` – Create offer
- [ ] `get_all_offers()` – List offers
- [ ] `cancel_offer(trade_id)` – Cancel offer
- [ ] Error handling and retry logic

**Test**: Create a test offer on testnet, verify it appears in wallet.

#### Milestone 1.4: Dexie API Client
Implement and test the Dexie API client:
- [ ] `post_offer(offer_str)` – Submit to Dexie
- [ ] `get_offer(offer_id)` – Get offer status
- [ ] `get_offers(pair, side)` – Query orderbook
- [ ] WebSocket stream connection
- [ ] Rate limiting

**Test**: Submit a testnet offer to Dexie testnet API, verify visibility.

### Week 2: Price Oracle and State

#### Milestone 2.1: Price Oracle
- [ ] Implement CEX price fetching (OKX, MEXC, Gate.io)
- [ ] Implement TibetSwap spot price calculation
- [ ] Median aggregation across sources
- [ ] 1-hour rolling volatility calculation
- [ ] Price caching (avoid hammering APIs)
- [ ] Fallback when sources are unavailable

**Test**: Price oracle returns reasonable XCH/USD prices.

#### Milestone 2.2: Database Setup
- [ ] SQLite database with schema from `docs/03-technical-architecture.md`
- [ ] Offer CRUD operations
- [ ] Price history insertion
- [ ] Inventory snapshot recording
- [ ] Basic query functions for reporting

**Test**: Insert and retrieve offers, verify state transitions.

#### Milestone 2.3: Configuration System
```toml
# config.example.toml

[general]
network = "testnet11"        # "mainnet" or "testnet11"
log_level = "INFO"
dry_run = true               # Set to false for live trading

[wallet]
fingerprint = 0              # Set your wallet fingerprint
rpc_host = "localhost"
rpc_port = 9256

[strategy]
name = "basic_spread"        # Strategy to use
base_spread_pct = 1.0        # Base spread percentage
max_inventory_xch = 10.0     # Max XCH in play
max_offer_count = 10         # Max concurrent offers
offer_expiry_blocks = 100    # ~87 minutes

[pairs]
[[pairs.pair]]
base = "XCH"
quote = "USDS"
quote_asset_id = "6d95dae356e32a71db5ddcb42224754a02524c615c5fc35f568c2af04774e589"
lot_size_xch = 0.5           # XCH per offer
enabled = true

[price_sources]
okx = true
mexc = true
tibetswap = true

[alerts]
telegram_bot_token = ""      # Optional: Telegram bot for alerts
telegram_chat_id = ""
```

**Deliverable of Phase 1**: Bot can connect to Chia wallet, fetch prices, and read/write from database. `dry_run = true` mode shows what offers *would* be created without submitting them.

---

## 4. Phase 2: Core Market Making (Weeks 3-4)

### Week 3: Basic Spread Strategy

#### Milestone 3.1: Offer Manager
- [ ] Offer creation flow (wallet RPC → Dexie submission)
- [ ] Offer state polling (Dexie API)
- [ ] Offer cancellation (on-chain + Dexie)
- [ ] Offer staleness detection and refresh
- [ ] Offer deduplication (don't create duplicate offers)

**Test**: Full offer lifecycle on testnet: create → submit → monitor → cancel.

#### Milestone 3.2: Inventory Manager
- [ ] Query available (unlocked) coins per asset
- [ ] Coin selection algorithm (minimize coin fragmentation)
- [ ] Inventory balance tracking
- [ ] Rebalancing trigger detection
- [ ] Coin consolidation utility (merge small coins)

**Test**: Inventory correctly tracks coins locked in offers vs. available.

#### Milestone 3.3: Basic Spread Strategy
Implement the simplest viable strategy:

```python
class BasicSpreadStrategy:
    """
    Posts one buy offer and one sell offer at configurable spread.
    Refreshes when price moves > threshold or offer fills.
    """

    async def run_cycle(self):
        mid_price = await self.oracle.get_mid_price("XCH/USDS")
        spread = self.config.base_spread_pct / 100

        bid_price = mid_price * (1 - spread / 2)
        ask_price = mid_price * (1 + spread / 2)

        # Check if existing offers need refresh
        for offer in await self.offer_manager.get_active_offers("XCH/USDS"):
            if self.should_refresh(offer, mid_price):
                await self.offer_manager.cancel_offer(offer)

        # Post new offers if needed
        if not self.has_active_bid("XCH/USDS"):
            await self.post_buy_offer(bid_price, self.config.lot_size_xch)
        if not self.has_active_ask("XCH/USDS"):
            await self.post_sell_offer(ask_price, self.config.lot_size_xch)
```

#### Milestone 3.4: Main Bot Loop
- [ ] Strategy polling cycle (every 60 seconds)
- [ ] Offer monitoring cycle (every 30 seconds)
- [ ] Graceful shutdown handler (cancel all offers on exit)
- [ ] Error handling and recovery
- [ ] Logging and state persistence

**Test**: Run bot on testnet for 24 hours; verify offers are maintained, refreshed when stale.

### Week 4: Live Testnet Validation

#### Milestone 4.1: End-to-End Testnet Run
- [ ] Run bot on testnet for 48+ hours
- [ ] Verify offer creation and submission to Dexie testnet
- [ ] Simulate fills by manually accepting offers
- [ ] Verify inventory updates after fills
- [ ] Verify offer refresh logic works

#### Milestone 4.2: PnL Tracking
- [ ] Record every fill with price and amount
- [ ] Calculate spread income per fill
- [ ] Calculate approximate cost basis changes
- [ ] Daily PnL report generation
- [ ] Basic CLI command: `xoptrader status`

#### Milestone 4.3: Dry Run on Mainnet
Before committing real capital:
- [ ] Switch config to mainnet
- [ ] Keep `dry_run = true`
- [ ] Verify price oracle fetches real XCH prices correctly
- [ ] Verify Dexie mainnet API is accessible
- [ ] Verify wallet fingerprint and coin availability
- [ ] Log what offers *would* be created

**Deliverable of Phase 2**: Bot is fully functional in testnet mode. Dry-run on mainnet shows sensible offers. Ready for limited mainnet deployment.

---

## 5. Phase 3: Strategy Optimization (Weeks 5-6)

### Week 5: Enhanced Strategies

#### Milestone 5.1: Dynamic Spread
Replace static spread with volatility-adjusted spread:
- [ ] 1-hour rolling volatility calculation
- [ ] Spread formula: `spread = base + vol_multiplier * volatility`
- [ ] Configurable volatility sensitivity
- [ ] Back-test against historical XCH price data

#### Milestone 5.2: Grid Strategy
Add grid trading as an alternative strategy:
- [ ] Grid level calculation (geometric spacing)
- [ ] Multi-offer posting (N levels on each side)
- [ ] Grid rebalancing when a level fills
- [ ] Grid reset when price exits range
- [ ] Configurable parameters: levels, range, lot size

#### Milestone 5.3: TibetSwap Integration
Add passive liquidity provision:
- [ ] TibetSwap API client implementation
- [ ] LP deposit workflow
- [ ] LP position tracking
- [ ] Fee income calculation
- [ ] Impermanent loss monitoring
- [ ] LP withdrawal workflow

#### Milestone 5.4: DBX Reward Automation
- [ ] Integrate `dexie-rewards` for automatic claiming
- [ ] DBX balance tracking
- [ ] Reward claiming on schedule (daily)
- [ ] DBX value tracking (convert to USD for PnL)

### Week 6: Backtesting and Calibration

#### Milestone 6.1: Historical Data Collection
```bash
# Collect 30 days of XCH/USDS price history
# Sources: OKX OHLCV data, TibetSwap on-chain history

python xoptrader/tools/collect_history.py \
  --pair XCH/USDS \
  --days 30 \
  --output data/historical/xch_usds_30d.csv
```

#### Milestone 6.2: Backtesting Framework
```python
class Backtester:
    """Simulates strategy performance against historical price data."""

    def run(self, strategy, price_history: list[float]) -> BacktestResult:
        virtual_inventory = Inventory(xch=10.0, usds=50.0)  # Starting capital
        fills = []
        pnl = 0

        for i, price in enumerate(price_history):
            quotes = strategy.compute_quotes(price, virtual_inventory)
            # Simulate fills based on price movement
            if price <= quotes.bid:
                # Buy offer fills
                fill = Fill(side="BUY", price=quotes.bid, amount=quotes.size)
                fills.append(fill)
                virtual_inventory.apply_fill(fill)
                pnl += quotes.bid - price  # Simplified spread capture
            elif price >= quotes.ask:
                fill = Fill(side="SELL", price=quotes.ask, amount=quotes.size)
                fills.append(fill)
                virtual_inventory.apply_fill(fill)
                pnl += price - quotes.ask

        return BacktestResult(fills=fills, total_pnl=pnl, sharpe_ratio=...)
```

#### Milestone 6.3: Parameter Optimization
- [ ] Grid search over spread parameters
- [ ] Optimize for: Sharpe ratio, total PnL, max drawdown
- [ ] Document optimal parameters per market regime (trending vs. ranging)

**Deliverable of Phase 3**: Bot has multiple strategies, DBX reward automation, and backtested optimal parameters. Ready for live mainnet with real capital.

---

## 6. Phase 4: Hardening and Scale (Weeks 7-8)

### Week 7: Production Hardening

#### Milestone 7.1: Error Recovery
- [ ] Automatic restart on crash (systemd service)
- [ ] State recovery from checkpoint on restart
- [ ] Re-sync offer states with Dexie after restart
- [ ] Handle wallet RPC timeout/disconnect gracefully
- [ ] Handle Dexie API downtime gracefully (queue and retry)

#### Milestone 7.2: Security Audit
- [ ] Review: No private keys in code or logs
- [ ] Review: Config file has proper permissions (chmod 600)
- [ ] Review: All API communications use HTTPS/WSS
- [ ] Review: Hot wallet is funded only with operational capital
- [ ] Review: Database contains no secrets
- [ ] Optional: Code audit by a Chia community developer

#### Milestone 7.3: Monitoring Dashboard
- [ ] Prometheus metrics endpoint
- [ ] Grafana dashboard with key metrics
- [ ] Telegram/email alerting for critical events
- [ ] Daily PnL email summary

### Week 8: Scale and Expand

#### Milestone 8.1: Multi-Pair Support
- [ ] Add XCH/DBX trading pair
- [ ] Add XCH/WBTC.b trading pair
- [ ] Pair-specific configuration (spread, lot size, enabled)
- [ ] Cross-pair inventory management
- [ ] DBX reward tracking for reward-eligible pairs

#### Milestone 8.2: Arbitrage Module
- [ ] Monitor Dexie vs. TibetSwap price discrepancy
- [ ] Execute arbitrage when discrepancy > threshold + fees
- [ ] Configurable threshold and size limits
- [ ] Careful sizing to stay within pool impact limits

#### Milestone 8.3: Performance Review
After 2+ weeks of live trading:
- [ ] Analyze actual vs. expected PnL
- [ ] Identify optimization opportunities
- [ ] Review fill rate by pair and side
- [ ] DBX rewards received vs. projected
- [ ] Document lessons learned

**Deliverable of Phase 4**: Production-ready, multi-pair market maker with monitoring. Operating profitably (or with clear path to profitability) on Chia mainnet.

---

## 7. Cost and Capital Requirements

### Infrastructure Costs
| Resource | Monthly Cost | Notes |
|----------|-------------|-------|
| VPS (Chia node + bot) | $30-50 | DigitalOcean/Linode/Hetzner |
| Domain + TLS cert | ~$1 | Let's Encrypt is free |
| Monitoring (Grafana Cloud free tier) | $0 | Sufficient for basic monitoring |
| **Total Infrastructure** | **~$35-55/month** | |

### Capital Requirements
| Use Case | Minimum | Recommended | Notes |
|----------|---------|-------------|-------|
| Testing (testnet) | $0 | $0 | Free testnet XCH from faucet |
| XCH/USDS single pair | 1 XCH + $50 USDS | 5 XCH + $200 USDS | At ~$5 XCH price |
| Multi-pair operation | 10 XCH + $500 | 50 XCH + $2000 | For meaningful liquidity |
| DBX reward eligibility | Varies | Check dexie.space/docs | Depends on incentivized pairs |

### Break-Even Analysis
```
Monthly infrastructure cost:    $50
Required monthly spread income: $50

To break even at 0.5% spread on XCH/USDS:
  Needed monthly volume:        $50 / 0.005 = $10,000
  Daily volume needed:          $10,000 / 30 = $333/day

Current XCH/USDS daily volume on Dexie: ~$5,000-$50,000 (variable)
At 5% market share:                     $250-$2,500/day
Break-even is achievable at ~10-15% market share at low volume.

DBX rewards may significantly improve economics - check current reward rates.
```

### Risk Capital
- **Never invest more than you can afford to lose entirely.**
- XCH price has historically been highly volatile (dropped 99%+ from ATH).
- Chia DeFi is nascent; smart contract risk exists on TibetSwap.
- Market making risk: adverse selection, inventory risk, impermanent loss.

---

## 8. Decision Points and Trade-offs

### Decision 1: Build vs. Use GreenFloor
**GreenFloor** is an open-source Chia market maker already built for Dexie.

| Option | Pros | Cons |
|--------|------|------|
| Use GreenFloor | Faster start, battle-tested | Less flexible, dependency on someone else's code |
| Build custom (XOPTrader) | Full control, learn deeply, tailor to needs | More development time |
| Fork GreenFloor | Best of both worlds | Need to understand their codebase |

**Recommendation**: Start by studying GreenFloor's code, then build XOPTrader with your own architecture informed by it.

### Decision 2: Dexie Only vs. Dexie + TibetSwap
| Option | Pros | Cons |
|--------|------|------|
| Dexie only | Simpler, earns DBX rewards | Miss passive fee income from AMM |
| TibetSwap only | Fully passive | No control over pricing, IL risk |
| Both | Diversified income, optimized capital | More complexity |

**Recommendation**: Start with Dexie offers only (Phase 1-2), add TibetSwap LP in Phase 3.

### Decision 3: Start Spread Width
| Spread | Fill Rate | Risk | DBX Eligibility |
|--------|-----------|------|-----------------|
| 0.5% | High | Higher inventory risk | Likely eligible |
| 1.0% | Medium | Moderate risk | Likely eligible |
| 2.0% | Lower | Lower risk | May not qualify |
| 5.0% | Low | Low risk | Unlikely |

**Recommendation**: Start at 1.5% spread for a balance of fill rate and risk, then adjust based on backtesting and DBX reward requirements.

### Decision 4: Mainnet Launch Timing
**Launch conditions to consider:**
- [ ] 48+ hours of clean testnet operation without errors
- [ ] Dry run on mainnet showing sensible quotes
- [ ] Monitoring and alerting working
- [ ] Stop-loss logic tested
- [ ] Starting with small capital (< 1 XCH equivalent)

---

## 9. Testing Strategy

### Unit Tests
Each component should have unit tests:
```python
# tests/test_oracle.py
import pytest
from unittest.mock import AsyncMock, patch
from xoptrader.oracle import PriceOracle

@pytest.mark.asyncio
async def test_oracle_median_aggregation():
    """Oracle should return median of available prices."""
    oracle = PriceOracle(config)
    with patch.object(oracle, '_fetch_cex_price') as mock_fetch:
        mock_fetch.side_effect = [10.0, 10.5, 11.0]
        price = await oracle.get_mid_price("XCH/USDS")
        assert price == 10.5  # Median of [10.0, 10.5, 11.0]

@pytest.mark.asyncio
async def test_oracle_handles_source_failure():
    """Oracle should handle individual source failures gracefully."""
    oracle = PriceOracle(config)
    with patch.object(oracle, '_fetch_cex_price') as mock_fetch:
        mock_fetch.side_effect = [10.0, Exception("API error"), 11.0]
        price = await oracle.get_mid_price("XCH/USDS")
        assert price == 10.5  # Median of [10.0, 11.0] (ignoring failure)
```

### Integration Tests (Testnet)
```python
# tests/integration/test_offer_lifecycle.py
@pytest.mark.integration
@pytest.mark.asyncio
async def test_offer_create_submit_cancel(testnet_wallet, testnet_dexie):
    """Full offer lifecycle on testnet."""
    # Create offer
    offer = await testnet_wallet.create_offer(...)
    assert offer.status == OfferStatus.PENDING

    # Submit to Dexie
    await testnet_dexie.submit_offer(offer)
    assert offer.status == OfferStatus.ACTIVE

    # Cancel offer
    await testnet_wallet.cancel_offer(offer)
    assert offer.status == OfferStatus.CANCELLED
```

### Strategy Backtests
```bash
# Run backtests for all strategies
python -m xoptrader.backtest \
  --strategy basic_spread \
  --data data/historical/xch_usds_30d.csv \
  --spread 1.0 \
  --output results/backtest_basic_spread.json

python -m xoptrader.backtest \
  --strategy grid \
  --data data/historical/xch_usds_30d.csv \
  --grid-levels 10 \
  --grid-range 0.3 \
  --output results/backtest_grid.json
```

---

## 10. Launch Checklist

### Pre-Launch (Before Real Capital)
- [ ] All unit tests passing
- [ ] 48+ hours testnet run without errors
- [ ] Dry-run on mainnet showing correct prices
- [ ] Database backup configured
- [ ] Stop-loss limits set and tested
- [ ] Monitoring and alerting configured
- [ ] Telegram/Discord notification working
- [ ] Config file secured (chmod 600, not in git)
- [ ] Hot wallet funded with *small* test amount

### Soft Launch (Small Capital)
- [ ] Start with ≤ 0.5 XCH equivalent
- [ ] Run for 72 hours, monitor closely
- [ ] Verify at least one offer fills
- [ ] Verify PnL calculation is accurate
- [ ] Verify offer refresh is working
- [ ] Verify inventory tracking is accurate

### Scale Up (After Validation)
- [ ] Gradually increase capital (2x every week if healthy)
- [ ] Add second trading pair
- [ ] Enable DBX reward claiming
- [ ] Consider adding TibetSwap LP position
- [ ] Set up weekly PnL review

### Ongoing Operations
- [ ] Daily: Review logs for errors
- [ ] Weekly: Analyze PnL and fill rates
- [ ] Weekly: Claim DBX rewards
- [ ] Monthly: Review and adjust strategy parameters
- [ ] Monthly: Backup database
- [ ] Quarterly: Security review and dependency updates
