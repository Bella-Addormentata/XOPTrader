# XOPTrader

**XOPTrader** is a Chia blockchain market maker that operates on decentralized exchanges (DEXs), providing liquidity on [Dexie](https://dexie.space) and [TibetSwap](https://v2.tibetswap.io) to earn trading spreads and liquidity rewards.

---

## Research & Planning Documents

This repository contains in-depth research and planning documents for building a Chia DEX market maker. Start here:

| Document | Description |
|----------|-------------|
| [01 - Chia Ecosystem Overview](docs/01-chia-ecosystem-overview.md) | Chia blockchain fundamentals, Chialisp, CATs, Offers protocol, DEX landscape (Dexie, TibetSwap), and market opportunities |
| [02 - Market Making Strategies](docs/02-market-making-strategies.md) | Strategy options: basic spread, grid trading, Avellaneda-Stoikov, AMM LP, arbitrage; pricing, inventory, and risk management |
| [03 - Technical Architecture](docs/03-technical-architecture.md) | System design, component breakdown, technology stack options, Chia integration, deployment, security, and reference implementations |
| [04 - Implementation Roadmap](docs/04-implementation-roadmap.md) | 8-week phased build plan, prerequisites, capital requirements, testing strategy, and launch checklist |

---

## Project Summary

### What is XOPTrader?
XOPTrader automates liquidity provision on Chia DEXs by:
1. **Posting Chia Offer Files** on [Dexie](https://dexie.space) to earn bid-ask spreads
2. **Providing AMM liquidity** on [TibetSwap](https://v2.tibetswap.io) to earn swap fees
3. **Claiming DBX rewards** from Dexie's Liquidity Incentive Program
4. **Running arbitrage** between Dexie orderbook and TibetSwap AMM prices

### Why Chia?
- **Low competition**: Few automated market makers operate on Chia DEXs
- **Wide spreads**: Many pairs trade at 2–5% bid-ask spreads
- **DBX incentives**: Dexie rewards market makers with governance tokens
- **Non-custodial**: Chia Offers keep funds in your wallet until a trade executes
- **Green blockchain**: Proof of Space and Time is energy-efficient

### Target Trading Pairs
1. XCH / USDS (Stably USD) – highest volume, DBX reward eligible
2. XCH / DBX (Dexie Bucks) – native DEX token, reward eligible
3. XCH / WBTC.b – correlated to Bitcoin for easier pricing

---

## Quick Start

> **Note**: The trading bot codebase is under development. See the implementation roadmap for the build plan.

### Prerequisites
- Python 3.11+
- Chia blockchain installed and synced
- Dexie account (optional, for reward tracking)

### Development Setup
```bash
# Clone repository
git clone https://github.com/dorkmo/XOPTrader.git
cd XOPTrader

# Install dependencies (once requirements.txt is created)
pip install -r requirements.txt

# Copy and edit configuration
cp config.example.toml config.toml
```

---

## Key Concepts

### Chia Offers
Chia's [Offers protocol](https://docs.chia.net/academy-offers/) enables atomic, trustless peer-to-peer trading. A market maker creates offer files specifying exactly what they'll trade, which takers can accept. No assets are locked in a smart contract—they stay in your wallet until a fill occurs.

### Dexie Liquidity Incentive Program
[Dexie](https://dexie.space) rewards market makers with **DBX** tokens for maintaining tight spreads on incentivized pairs. The tighter and deeper your offers, the more DBX you earn.

### TibetSwap AMM
[TibetSwap](https://v2.tibetswap.io) is Chia's primary Automated Market Maker (AMM), using a constant-product (`x * y = k`) model similar to Uniswap V1. Liquidity providers deposit both assets of a pair and earn 0.3% of every swap.

---

## Project Status

| Phase | Status | Description |
|-------|--------|-------------|
| Research & Planning | ✅ Complete | Ecosystem analysis, strategy design, architecture planning |
| Phase 1: Foundation | 🔜 Planned | Chia node, wallet RPC, Dexie API, price oracle, database |
| Phase 2: Core Market Making | 🔜 Planned | Basic spread strategy, offer lifecycle, testnet validation |
| Phase 3: Optimization | 🔜 Planned | Dynamic spreads, grid strategy, TibetSwap LP, backtesting |
| Phase 4: Production | 🔜 Planned | Monitoring, multi-pair, arbitrage, live trading |

---

## Resources

- [Chia Documentation](https://docs.chia.net/)
- [Chialisp Reference](https://chialisp.com/)
- [Dexie API Docs](https://dexie.space/api)
- [TibetSwap GitHub](https://github.com/Yakuhito/tibet)
- [GreenFloor (reference market maker)](https://github.com/hoffmang9/greenfloor)
- [dexie-rewards CLI](https://github.com/dexie-space/dexie-rewards)
- [Splash! Network](https://github.com/dexie-space/splash)

---

## License

MIT
