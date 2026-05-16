# XOPTrader

**Automated CHIA DEX Market Maker** — a high-performance C++20 trading engine that provides liquidity on CHIA's decentralized exchanges using the native atomic offer system.

XOPTrader deploys capital across dexie.space, TibetSwap, and other CHIA DEX venues, quoting bid/ask offers using adaptive market-making algorithms (Avellaneda-Stoikov, GLFT) with real-time volatility estimation, regime detection, and multi-tier order ladders. It monitors fills, manages inventory, detects arbitrage, and exports full observability via Prometheus + Grafana + Telegram alerts.

---

## Table of Contents

- [How It Works](#how-it-works)
- [Architecture](#architecture)
- [The CHIA Offer System](#the-chia-offer-system)
- [Market Making Algorithms](#market-making-algorithms)
- [Spread Optimization](#spread-optimization)
- [Multi-Tier Offer Ladder](#multi-tier-offer-ladder)
- [Risk Management](#risk-management)
- [Hedging Framework](#hedging-framework)
- [Arbitrage Detection](#arbitrage-detection)
- [Monitoring and Alerts](#monitoring-and-alerts)
- [Backtesting](#backtesting)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Configuration](#configuration)
- [Running](#running)
- [Configuration Reference](#configuration-reference)
- [Key Formulas](#key-formulas)
- [Development](#development)
- [Roadmap](#roadmap)
- [Research Documentation](#research-documentation)

---

## How It Works

XOPTrader operates on a **per-block heartbeat** (approximately every 52 seconds, matching CHIA's block time). On each new block, the engine executes a 13-step cycle:

```
 Block N detected
   |
   v
 1. Fetch market data from dexie.space + CEX reference prices
 2. Detect fills — check if any of our pending offers were taken
 3. Update volatility (Yang-Zhang), adverse selection (Bayesian PIN), regime (VR + HMM)
 4. Compute optimal bid/ask quotes (Avellaneda-Stoikov or GLFT)
 5. Apply spread optimizer — 4-component model + dynamic adjustments
 6. Enforce risk limits — inventory caps, position sizing, optional no-loss constraint
 7. Generate multi-tier offer ladder (4 tiers per side by default)
 8. Cancel stale offers, post fresh ones via Chia wallet RPC + dexie submission
 9. Scan for arbitrage opportunities (CEX-DEX, cross-DEX, triangular, cross-bridge)
10. Run hedging layer — skew adjustment, natural hedge efficiency, portfolio netting
11. Record PnL attribution (spread / inventory / fee) + persist to SQLite
12. Export 24 metrics to Prometheus
13. Evaluate 14 alert rules, fire Telegram notifications if triggered
   |
   v
 Wait for Block N+1...
```

The bot connects to a locally running **Chia full node** (port 8555) and **Chia wallet** (port 9256) via mTLS-authenticated RPC, and to the **dexie.space** public API for order book data and offer distribution.

---

## Architecture

```
                     +---------------------+
                     |    config.yaml      |
                     | (YAML, validated)   |
                     +---------+-----------+
                               |
                               v
+----------------------------------------------------------------------+
|                        Engine (engine.hpp)                            |
|  Per-block heartbeat loop via boost::asio::io_context                |
|  Wires all subsystems, manages lifecycle, graceful shutdown           |
+---+--------+--------+--------+--------+--------+--------+--------+--+
    |        |        |        |        |        |        |        |
    v        v        v        v        v        v        v        v
+------+ +------+ +------+ +------+ +------+ +------+ +------+ +------+
|Chia  | |Dexie | |Market| |A-S / | |Spread| |Offer | |Risk  | |PnL   |
|RPC   | |Client| |Data  | |GLFT  | |Optim.| |Mgr   | |Engine| |Track |
|mTLS  | |REST  | |Agg.  | |Strat.| |4-comp| |Tiers | |Limits| |SQLite|
+------+ +------+ +------+ +------+ +------+ +------+ +------+ +------+
    |        |        |        |        |        |
    v        v        v        v        v        v
+------+ +------+ +------+ +------+ +------+ +------+
|Coin  | |Rate  | |Vol.  | |Regime| |Arb   | |Hedge |
|Mgr   | |Limit | |Y-Z   | |VR+HMM| |Detect| |7-Lyr |
|UTXO  | |Slider| |PIN   | |Hyster| |4-type| |Stack |
+------+ +------+ +------+ +------+ +------+ +------+
                               |
                    +----------+----------+
                    |                     |
               +----v----+         +-----v------+
               |Prometheus|         |Telegram    |
               |24 metrics|         |14 alerts   |
               |6 dashbds |         |3 tiers     |
               +----------+         +------------+
```

**Key design properties:**

- **All monetary values are `int64_t` mojos** (1 XCH = 10^12 mojos). No floating-point in financial paths. 128-bit intermediate arithmetic prevents overflow on multiplications.
- **Thread-safe state** via three independent `std::shared_mutex` instances (positions, offers, markets). Deadlock-free by construction — no method ever holds two locks.
- **Fault-isolated heartbeat** — each of the 13 steps has independent error handling. A transient RPC failure in step 1 does not prevent steps 2-13 from running with cached data.
- **Graceful shutdown** — SIGINT/SIGTERM triggers offer cancellation on-chain before exit. Second signal force-stops immediately.

---

## The CHIA Offer System

CHIA has a unique native trading mechanism unlike any other blockchain:

1. **Maker** creates an incomplete, partially-signed spend bundle specifying what they offer and what they want in return. This is serialized as a bech32m-encoded text string (`offer1...`).

2. The offer is distributed to aggregators (dexie.space, Splash network) — **free to create, free to cancel**.

3. **Taker** finds the offer, adds their complementary coin spends, and submits the completed bundle to the blockchain.

4. Settlement is **atomic** — all-or-nothing in a single block (~52 seconds). Zero counterparty risk.

**Implications for market making:**

| Property | Impact |
|----------|--------|
| Zero creation cost | Can maintain many concurrent offers at different price levels |
| Atomic settlement | No partial fills, no settlement risk |
| Off-chain cancellation | Instant quote updates (spend a locked coin to invalidate) |
| Coin-set (UTXO) model | Must pre-split coins for concurrent offers — CoinManager handles this |
| ~52s block time | Natural heartbeat interval; offers are stale after ~60 blocks (~52 min) |
| Multi-asset offers | Single offer can involve XCH + multiple CATs + NFTs |

**Wallet RPC endpoints used:**

| Endpoint | Purpose |
|----------|---------|
| `create_offer_for_ids` | Create bid/ask offers |
| `take_offer` | Accept arbitrage opportunities |
| `cancel_offer` | Invalidate stale offers |
| `get_all_offers` | Detect fills (status → CONFIRMED) |
| `check_offer_validity` | Verify offers are still live |
| `get_spendable_coins` | Coin inventory for splitting |
| `select_coins` | Choose coins for offer creation |

---

## Market Making Algorithms

### Avellaneda-Stoikov (Default)

The classic optimal market-making model, adapted for CHIA's discrete block time:

**Reservation price** (inventory-adjusted fair value):
```
r(t, q) = S - q * gamma * sigma^2 * tau
```
- `S` = current mid-price
- `q` = net inventory (positive = long)
- `gamma` = risk aversion (default 0.01)
- `sigma` = per-block volatility (Yang-Zhang estimate)
- `tau` = time remaining in horizon, counted in blocks: `(N - n) * 52 seconds`

**Optimal half-spread:**
```
delta* = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau
```
- `kappa` = fill intensity decay (default 1.5) — calibrated from historical dexie fill data

**Final quotes:**
```
bid = r - delta*
ask = r + delta*
```

When inventory is positive (long), the reservation price drops below mid, making the ask more aggressive (sell easier) and the bid less aggressive (buy harder). This naturally rebalances inventory toward zero.

### GLFT (Running Inventory Penalty)

Preferred for CHIA's 24/7 market (no natural "session end" like equity markets):

```
bid = S - half_spread - skew * q
ask = S + half_spread - skew * q
```

Where `skew = phi * q / q_max` continuously nudges both quotes in the inventory-shedding direction. The `phi` parameter controls how aggressively the model rebalances — higher values (e.g., 0.8) produce stronger quote skewing for faster inventory recovery. When `cross_pair_skew_enabled` is set, skew from related pairs (those sharing an asset) is blended via `cross_pair_skew_phi`, enabling coordinated rebalancing across pairs like XCH/BYC + BYC/wUSDC.b.

### Regime Detection

The strategy adapts to market conditions using a **variance ratio test** with **Hidden Markov Model** confirmation:

| Regime | VR Range | Spread Mult | Size Mult | Shedding Mult |
|--------|----------|-------------|-----------|---------------|
| Mean-Reverting | < 0.85 | 0.8x (tighter) | 1.2x | 0.5x (slower) |
| Normal | 0.85 - 1.15 | 1.0x | 1.0x | 1.0x |
| Momentum | > 1.15 | 1.5x (wider) | 0.6x | 2.0x (faster) |

Three-layer anti-whipsaw: dual-horizon VR (q=5, q=10), Lo-MacKinlay Z-significance gate (95%), and 5-block hysteresis before switching regimes.

---

## Spread Optimization

The spread is computed from four independent components:

```
spread = s_adverse + s_inventory + s_cost + s_competition
```

| Component | Formula | Purpose |
|-----------|---------|---------|
| Adverse selection | `gamma * sigma * sqrt(E[T_fill]) * PIN` | Compensate for informed traders |
| Inventory risk | `gamma * sigma^2 * tau * \|q\| / Q_max` | Wider spread when inventory is skewed |
| Transaction cost | `(fee_chain + fee_venue) / trade_size` | Cover blockchain + aggregator fees |
| Competition | `max(floor, best_competing + epsilon)` | Stay competitive but profitable |

**Minimum profitable spread: 35-60 basis points** (well below current CHIA DEX spreads of 300-1000 bps).

### Dynamic Adjustments

| Condition | Multiplier |
|-----------|-----------|
| High volatility regime | 1.80x wider |
| Low volatility regime | 0.70x tighter |
| Weekend | 1.175x wider |
| US+EU market overlap (14:00-18:00 UTC) | 0.90x tighter |
| Inventory skewed > 60% | Graduated asymmetric widening |

### Thompson Sampling (Adaptive Learning)

Optional Bayesian spread selection using Beta-distributed posteriors per discrete spread level. Explores the spread grid, learns which levels are most profitable, and converges toward the empirically optimal spread over time.

---

## Multi-Tier Offer Ladder

Instead of a single bid/ask, the bot maintains a **4-tier ladder** on each side (configurable):

```
SELL SIDE (example at $2.70 XCH):
  Tier 1: Sell 30% of capital @ mid + 0.6%   (tight, high fill probability)
  Tier 2: Sell 25% of capital @ mid + 2.0%   (moderate)
  Tier 3: Sell 25% of capital @ mid + 5.0%   (wide, inventory reduction)
  Tier 4: Sell 20% of capital @ mid + 10.0%  (black swan sell wall)

BUY SIDE: mirror at corresponding discounts below mid
```

Each tier is a separate on-chain CHIA offer. Tier spacing and size allocation are fully configurable via `tier_spacing_bps` and `tier_size_pct` in `config.yaml`.

**Inventory-aware tier adjustment:** When inventory is skewed, the engine applies asymmetric spread multipliers — tightening tiers on the side that reduces inventory, widening tiers on the side that would increase the imbalance.

**Rebalancing triggers** (any one triggers full ladder reconstruction):
1. Price moved > 2% since last rebalance
2. Inventory skew crossed 60% threshold
3. Offers stale > 1 hour (TTL exceeded)
4. Volume spike > 3x rolling average
5. Volatility spike > 2x 7-day average

---

## Risk Management

### Inventory Controls

| Control | Threshold | Action |
|---------|-----------|--------|
| Soft limit | 60% in one asset | Begin aggressive quote skewing |
| Hard limit | 80% in one asset | Pull quotes on the overweight side |
| Underwater position | cost > market | Hold; only offer above cost basis |
| Position aging > 24h | Stale inventory | Widen ask spread (never force exit) |
| Single CAT cap | 12% of portfolio | Never exceed regardless of opportunity |
| Max capital per pair | 85% (configurable) | Prevents any single pair from monopolizing capital |

### Position Sizing (Half-Kelly)

```
f* = kelly_fraction * (spread - sigma * sqrt(tau)) / (sigma^2 * tau)
```

Practical cap: ~2% of capital per pair per price level. This ensures no single fill can materially impact the portfolio.

### Mark-to-XCH Valuation

All risk calculations value positions in a common XCH numeraire. Per-heartbeat, the engine computes XCH exchange rates for each enabled pair:

- **XCH/CAT pairs**: `rate = kMojosPerXch / (mid_price × quote_mojos_per_unit)`
- **CAT/XCH pairs**: `rate = mid × kMojosPerXch / base_mojos_per_unit`

Rates are cached in `State::xch_rates_` and consumed by `PreTradeCheck::mark_to_xch()` for inventory concentration and max-capital-per-pair checks. This avoids the fragile pattern of probing market snapshots at risk-check time.

### Cost Basis Tracking

Weighted-average cost basis per asset, updated on every fill:
```
new_basis = (old_total_cost + fill_price * fill_qty) / (old_qty + fill_qty)
```

All cost basis math uses `int64_t` mojos with 128-bit intermediates to prevent overflow and eliminate floating-point drift.

### No-Loss Constraint (Optional)

When `enable_no_loss_constraint` is set in the strategy config:
```
ask = max(optimal_ask, cost_basis * (1 + min_profit_margin_bps / 10000))
```

This floors every sell offer above cost basis plus a configurable margin. Enforced in two independent places (PreTradeCheck and OfferManager) for defense in depth. **Disabled by default** — enable only if your strategy requires it.

### Emergency Playbook

| Scenario | Automated Response |
|----------|-------------------|
| Flash crash > 20% | PULL ALL QUOTES. Hold everything. Resume after 100+ stable blocks |
| One-sided fills | Widen opposite side. Never panic sell |
| Network congestion | Increase rebalancing buffer to 25-30% |
| Smart contract exploit | Cancel all offers, exit all AMM positions |

---

## Hedging Framework

7-layer priority stack (layers 1-4 are free and implemented in Phase 1):

| Priority | Method | Cost | Implemented |
|----------|--------|------|-------------|
| 1 | Inventory-based quote skewing | Free | Yes |
| 2 | Natural two-sided balancing (maximize NHE) | Free | Yes |
| 3 | Portfolio-level netting across pairs | Free | Yes |
| 4 | Statistical pairs hedging (correlated CATs) | Free | Yes |
| 5 | XCH perp delta hedging | Moderate | Planned |
| 6 | Cross-asset proxy hedge (BTC/ETH) | Moderate + basis risk | Planned |
| 7 | Options tail hedge (BTC put spreads) | Fixed premium | Planned |

**Natural Hedge Efficiency** target:
```
NHE = 1 - (|net_inventory_change| / total_volume)
Target: NHE > 0.70
```

---

## Arbitrage Detection

Four arbitrage types are continuously scanned:

### 1. CEX-DEX Arbitrage (Primary Opportunity)
XCH trades ~$2.4M/day on CEXes vs ~$2K on DEXes. When DEX price diverges from CEX by > 50 bps, the bot posts competitive offers to capture the convergence. Expected edge: 50-200 bps per cycle.

### 2. Cross-DEX Arbitrage
Compares dexie.space order book prices against TibetSwap AMM implied prices. TibetSwap uses a constant-product formula with 0.7% fee:
```
output = (output_reserve * input * 993) / (input_reserve * 1000 + input * 993)
```

### 3. Triangular Arbitrage
Scans all 3-hop routes through the pair graph:
```
XCH -> CAT_A -> CAT_B -> XCH
```
Executes when `product_of_exchange_rates * (1 - per_leg_cost)^3 > 1.0`.

### 4. Cross-Bridge Arbitrage
wUSDC (bridged from Ethereum) vs wUSDC.b (bridged from Base) — same underlying stablecoin, different CHIA asset IDs. Arb when price divergence exceeds bridge round-trip cost.

---

## Monitoring and Alerts

### Prometheus Metrics (24 gauges/counters/histograms)

Exposed on configurable HTTP port (default 9090), organized into 6 Grafana dashboards:

| Dashboard | Key Metrics |
|-----------|-------------|
| Real-Time PnL | Total, realized, unrealized, spread component, inventory component |
| Inventory | Per-asset balance, cost basis, skew ratio, underwater flags |
| Market Data | Mid price, spread bps, 24h volume per pair |
| System Health | Block height, sync status, wallet connectivity, offer latency histogram |
| Offer Lifecycle | Pending/filled/cancelled/expired counts, fill rate per hour |
| Risk | VaR 95%, max drawdown, portfolio concentration per asset |

### Telegram Alerts (14 rules, 3 tiers)

| Tier | Cooldown | Examples |
|------|----------|---------|
| CRITICAL | 1 per 60s | Node desync > 5 blocks, wallet unreachable, exposure breach, flash crash |
| WARNING | 1 per 5min | Fill rate drop, spread widening, underwater position, concentration breach |
| INFO | Batched hourly | PnL summaries, volume anomalies, arbitrage detected |

### SQLite Audit Trail

Every offer lifecycle event and trade settlement is recorded in an append-only SQLite database (WAL mode for concurrent safety). Full trade log with cost basis at time of execution, realized PnL, and Form 8949-compatible CSV export for tax reporting.

`offer_log` stores the current state of each offer, while `offer_closure_events` preserves the append-only closure history used for cancel-cause analytics. After the updated trader binary has run migrations against the target database, use `python scripts/report_offer_closure_events.py --top 15` for a compact breakdown of primary close causes, reconcile follow-ups, and recent closure observations.

---

## Backtesting

The backtesting framework supports parameter optimization before deploying live capital:

### Data Sources
- Historical offers from dexie.space API (JSON)
- CEX reference prices (CSV: OHLCV)
- On-chain block timestamps

### Simulation Modes

**Event-driven backtest:** Iterates through historical blocks, simulates fill matching against historical order flow. Accurately models CHIA's passive offer-take mechanics (all-or-nothing, no partial fills).

**Monte Carlo:** Generates N synthetic price paths via Geometric Brownian Motion with Yang-Zhang calibrated volatility. Produces distribution of returns, max drawdowns, and Sharpe ratios.

**Walk-forward optimization:** Anti-overfitting defense:
1. Split data: 70% train / 30% test
2. Optimize parameters on train set
3. Validate on test set (must be profitable on BOTH)
4. Roll forward by 1 week, repeat
5. Only accept parameters stable across multiple windows

**Parameter sweep:** Grid search across 9 dimensions (gamma, kappa, phi, spread levels, tier spacing, inventory limits, etc.) with parallel execution via `std::async`.

---

## Project Structure

```
XOPTrader/
+-- README.md                           This file
+-- CHIA_MARKET_MAKER_STRATEGY.md       Full strategy document (20-section research)
+-- config.example.yaml                 Configuration template
+-- pyproject.toml                      Python tooling config (linting, testing)
+-- cpp/
    +-- CMakeLists.txt                  Root CMake build (C++20)
    +-- vcpkg.json                      Dependency manifest
    +-- cmake/
    |   +-- dependencies.cmake          vcpkg + FetchContent dep resolution
    +-- include/xop/
    |   +-- types.hpp                   Core types: Mojo, AssetId, Side, Quote, Fill, etc.
    |   +-- state.hpp                   Thread-safe global state (positions, offers, markets)
    |   +-- config.hpp                  YAML config data classes + loader
    |   +-- engine.hpp                  Main engine orchestrator
    |   +-- database.hpp                SQLite persistence layer
    |   +-- backtest.hpp                Backtesting framework
    |   +-- rpc/
    |   |   +-- chia_rpc.hpp            Chia full-node + wallet mTLS RPC client
    |   |   +-- dexie_client.hpp        dexie.space REST API client (rate-limited)
    |   +-- strategy/
    |   |   +-- base.hpp                Strategy abstract interface
    |   |   +-- avellaneda.hpp          Avellaneda-Stoikov implementation
    |   |   +-- glft.hpp                GLFT with running inventory penalty
    |   |   +-- spread.hpp              4-component spread optimizer + Thompson Sampling
    |   |   +-- regime.hpp              Regime detection (variance ratio + HMM)
    |   |   +-- liquidity.hpp           Multi-tier offer ladder engine
    |   |   +-- arbitrage.hpp           4-type arbitrage detector
    |   +-- execution/
    |   |   +-- offer_manager.hpp       Offer lifecycle (create/cancel/detect fills)
    |   |   +-- coin_manager.hpp        UTXO coin splitting and lock tracking
    |   |   +-- market_data.hpp         Multi-source price aggregation
    |   +-- risk/
    |   |   +-- inventory.hpp           Cost basis, position sizing, Kelly criterion
    |   |   +-- limits.hpp              Pre-trade checks, circuit breakers
    |   |   +-- hedging.hpp             7-layer hedge stack (layers 1-4)
    |   +-- data/
    |   |   +-- volatility.hpp          Yang-Zhang estimator + variance ratio
    |   |   +-- adverse_selection.hpp   Bayesian PIN estimator
    |   +-- monitoring/
    |       +-- pnl.hpp                 PnL attribution (spread/inventory/fee)
    |       +-- metrics.hpp             Prometheus metric exporter
    |       +-- alerts.hpp              Telegram alert manager
    +-- src/                            28 implementation files (.cpp)
    |   +-- main.cpp                    Entry point, CLI args, signal handling
    |   +-- engine.cpp                  13-step heartbeat loop (1,094 lines)
    |   +-- ...                         (one .cpp per header)
    +-- tests/                          71 unit tests (Google Test)
        +-- CMakeLists.txt
        +-- test_avellaneda.cpp         14 tests — A-S/GLFT math verification
        +-- test_spread.cpp             19 tests — 4-component spread model
        +-- test_inventory.cpp          19 tests — cost basis, Kelly, limits
        +-- test_volatility.cpp          9 tests — Yang-Zhang, annualization
        +-- test_regime.cpp             10 tests — VR, HMM, hysteresis
```

**~28,000 lines of C++20** across 60 files.

---

## Prerequisites

### Required Software

| Software | Version | Purpose |
|----------|---------|---------|
| **Chia full node** | 2.x+ | Block data, coin records, mempool |
| **Chia wallet** | 2.x+ | Offer creation/cancellation, balance queries |
| **C++20 compiler** | MSVC 19.29+ / GCC 11+ / Clang 14+ | Coroutine support required |
| **CMake** | 3.24+ | Build system |
| **Boost** | 1.84+ | `boost::asio` coroutines, `boost::beast` HTTP (hard requirement — older versions lack `co_spawn` overloads used by the engine) |
| **vcpkg** | Latest | Dependency management |

### Required Capital

| Amount | Viability |
|--------|-----------|
| < $1K | Coin granularity limits concurrent offers |
| **$10K-$30K** | **Optimal range** for best return-on-capital |
| > $50K | Ecosystem can't absorb capital productively (~$2K/day DEX volume) |

### Chia Node Setup

```bash
# Install Chia
pip install chia-blockchain

# Initialize (generates SSL certs)
chia init

# Start full node + wallet
chia start node wallet

# Wait for sync (may take hours on first run)
chia show -s
```

The SSL certificates generated by `chia init` are used for mTLS authentication:
```
~/.chia/mainnet/config/ssl/
  full_node/private_full_node.crt + .key   (port 8555)
  wallet/private_wallet.crt + .key         (port 9256)
```

---

## Building

```bash
# Clone
git clone https://github.com/dorkmo/XOPTrader.git
cd XOPTrader/cpp

# Install dependencies via vcpkg
vcpkg install --triplet x64-windows  # or x64-linux, arm64-osx

# Configure
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Run tests
cd build && ctest --output-on-failure
```

### Dependencies (managed by vcpkg)

| Library | Purpose |
|---------|---------|
| boost-asio | Async I/O, coroutines, timers |
| boost-beast | WebSocket support (future) |
| boost-program-options | CLI argument parsing |
| libcurl + OpenSSL | HTTPS + mTLS for Chia RPC |
| nlohmann-json | JSON parsing (RPC responses, dexie API) |
| spdlog | Structured logging |
| yaml-cpp | Configuration file parsing |
| sqlite3 | Trade log, offer history, snapshots |
| prometheus-cpp | Metrics exposition for Grafana |

---

## Configuration

### GUI First Run (recommended)

On first launch, the GUI automatically creates `config.yaml` from
`config.example.yaml` when no config file exists, then opens the
Settings workflow so you can fill in machine-specific values.

### CLI / Manual Setup

```bash
# Copy template
cp config.example.yaml config.yaml

# Edit with your values
# REQUIRED: SSL cert paths, wallet fingerprint, asset IDs
# OPTIONAL: Telegram alerts, strategy tuning
```

Key sections to configure before first run:

1. **`chia.ssl_*_path`** — Point to your Chia SSL certificates
2. **`chia.wallet_fingerprint`** — Your wallet's fingerprint (`chia keys show`)
3. **`pairs[].quote_asset_id`** — Full 64-character hex asset IDs for the CATs you want to trade
4. **`monitoring.telegram_*`** — Bot token and chat ID for alerts (optional)

---

## Running

```bash
# Dry run (compute quotes, never submit offers)
./build/xop_trader --config config.yaml --dry-run --verbose

# Live trading
./build/xop_trader --config config.yaml

# With debug logging
./build/xop_trader --config config.yaml --verbose
```

### CLI Options

| Flag | Description |
|------|-------------|
| `-c, --config PATH` | Path to YAML config file (default: `config.yaml`) |
| `--dry-run` | Paper-trade mode — logs computed quotes but never creates offers |
| `-v, --verbose` | Enable DEBUG-level logging |

### Shutdown

- **SIGINT** (Ctrl+C) or **SIGTERM**: Gracefully cancels all pending offers on-chain, then exits.
- **Second signal**: Force-stops immediately (offers may remain on-chain until coins are spent).

---

## Configuration Reference

### Strategy Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `gamma` | 0.01 | Risk aversion — higher = wider spreads, more conservative |
| `kappa` | 1.5 | Fill intensity decay — higher = expects fills at wider spreads |
| `phi` | 0.5 | GLFT inventory skew strength — higher = more aggressive rebalancing (recommended: 0.8 for skewed portfolios) |
| `q_max` | 1000 | Maximum inventory in base units before hard limit |
| `min_profit_margin_bps` | 35 | Minimum margin above cost basis for asks (when no-loss enabled) |
| `offer_ttl_blocks` | 60 | Cancel and refresh offers after N blocks (~52 minutes) |
| `num_tiers` | 4 | Number of price tiers per side |
| `tier_spacing_bps` | [60, 200, 500, 1000] | Spread per tier in basis points from mid |
| `tier_size_pct` | [0.30, 0.25, 0.25, 0.20] | Capital fraction allocated to each tier |
| `cross_pair_skew_phi` | 0.30 | Cross-pair inventory skew coordination strength (0–1.0). When pairs share an asset, skew from other pairs influences quotes. Higher = stronger cross-pair rebalancing |

### Risk Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `soft_limit_pct` | 0.60 | Begin aggressive skewing at this inventory ratio |
| `hard_limit_pct` | 0.80 | Pull quotes on overweight side |
| `single_cat_cap_pct` | 0.12 | Maximum portfolio allocation to any single CAT |
| `kelly_fraction` | 0.5 | Half-Kelly position sizing (conservative) |
| `max_capital_per_pair_pct` | 0.85 | Maximum capital deployed to any single pair (set high to allow rebalancing when one asset dominates) |
| `max_drawdown_pct` | 0.10 | All-time HWM drawdown fraction that pauses the engine (circuit breaker) |
| `loss_window_blocks` | 1152 | Rolling window for time-windowed loss circuit breaker (~10 h at 52 s/block) |
| `max_window_loss_bps` | 500 | Maximum loss in basis points within the rolling window; 0 = disabled |

### Volatility Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `lookback_blocks` | 200 | Yang-Zhang estimation window (~2.9 hours) |
| `yz_alpha` | 0.34 | Yang-Zhang optimal blending weight |

---

## Key Formulas

| Component | Formula |
|-----------|---------|
| Reservation price | `r = S - q * gamma * sigma^2 * tau` |
| Optimal half-spread | `delta = (1/kappa) * ln(1 + kappa/gamma) + 0.5 * gamma * sigma^2 * tau` |
| GLFT inventory skew | `skew = phi * q / q_max` |
| Adverse selection | `gamma * sigma * sqrt(E[T_fill]) * PIN` |
| Half-Kelly size | `f* = kelly * (spread - sigma*sqrt(tau)) / (sigma^2 * tau)` |
| Offer TTL bound | `TTL < (half_spread / sigma)^2 / block_time` |
| Block volatility | `sigma_block = sigma_annual * sqrt(52 / 31536000)` |
| Fill intensity | `lambda(delta) = A * exp(-kappa * delta)` |
| TibetSwap output | `out = (out_reserve * in * 993) / (in_reserve * 1000 + in * 993)` |
| Natural Hedge Efficiency | `NHE = 1 - (\|net_inv_change\| / total_volume)` |

---

## Competitor Detection and Response

XOPTrader includes comprehensive competitor detection capabilities to adapt when other market makers appear on CHIA DEX exchanges.

### System Architecture

**New Types** (`types.hpp`):
- `CompetingOffer` — Individual offers from other market participants
- `CompetitorMetrics` — Aggregated statistics per trading pair

**MarketDataFeed Extensions** (`market_data.hpp/cpp`):
- `ingest_competing_offers()` — Parse individual offers from order book
- `compute_competitor_metrics()` — Analyze competing spreads and depth
- `get_best_competing_spread_bps()` — Feed into spread optimizer

**Key Features:**
- **Own-offer filtering**: Excludes our offers via offer ID matching
- **Dust filtering**: Ignores offers below minimum size (1 XCH default)
- **Best spread tracking**: Computes tightest competing two-sided spread
- **Alert system**: Logs warnings when tight competitors detected
- **Spread floor protection**: Never goes below profitable minimum (40 bps)

### Strategic Response

When competitors appear, the spread optimizer automatically adapts via the competition component:

```
s_competition = max(s_floor_bps, best_competing_bps + epsilon_bps)
```

**Behavior:**
- **No competitors**: Falls back to 40 bps floor
- **Wide competitors** (200 bps): We maintain tight spreads (202 bps)
- **Tight competitors** (45 bps): We improve by epsilon (47 bps)
- **Extremely tight**: Floor protection prevents race-to-zero (40 bps minimum)

**Configuration** (`config.yaml`):
```yaml
market_data:
  enable_competitor_tracking: true
  min_competitor_offer_size: 1000000000000  # 1 XCH in mojos
  competitor_alert_threshold_bps: 50.0

spread_optimizer:
  s_floor_bps: 40.0       # Minimum profitable spread
  epsilon_bps: 2.0        # Price improvement over competitor
```

### Implementation Status

| Component | Status |
|-----------|--------|
| Infrastructure (types, MarketDataFeed) | ✅ Complete |
| Competitor detection logic | ✅ Complete |
| Unit tests (10 test cases) | ✅ Complete |
| Documentation | ✅ Complete |
| Engine integration | ⚠️ Requires implementation |
| Prometheus metrics | ⚠️ Recommended |
| Telegram alerts | ⚠️ Recommended |

**For full implementation details**, see [`docs/competitor-detection.md`](docs/competitor-detection.md).

---

## Development

### Running Tests

```bash
cd cpp/build
ctest --output-on-failure

# Or run directly
./xop_tests
```

81 unit tests covering:
- **Avellaneda-Stoikov math** (14 tests) — reservation price, half-spread, tau rollover, fill intensity, GLFT skew
- **Spread optimizer** (19 tests) — all 4 components, regime multipliers, Thompson sampling convergence
- **Inventory/risk** (19 tests) — cost basis arithmetic, sell-at-loss rejection, Kelly sizing, limit thresholds
- **Volatility** (9 tests) — Yang-Zhang on synthetic data, annualization roundtrip, VR regime classification
- **Regime detection** (10 tests) — synthetic OU/trending processes, hysteresis, Z-significance
- **Competitor detection** (10 tests) — offer filtering, spread calculation, depth counts, new competitor alerts

### Code Quality

```bash
# Static analysis (if clang-tidy available)
cmake --build build --target clang-tidy

# Format check (if clang-format available)
cmake --build build --target cppcheck
```

The codebase follows:
- **ISO/IEC 27001:2022** — No hardcoded secrets, mTLS for all RPC, secrets redacted from logs
- **ISO/IEC 5055** — Static analysis targets, stack protector flags, FORTIFY_SOURCE
- **ISO/IEC 25000 (SQuaRE)** — Comprehensive test coverage, documented formulas

---

## Database Retention and Rollups

For long-term chart history without unbounded growth, run:

```bash
python scripts/maintain_snapshot_rollups.py --db data/xop_trader.db --backup
```

What it does:

- Builds/updates `snapshots_1m`, `snapshots_15m`, `snapshots_1h`, `snapshots_1d`
  from raw `snapshots`.
- Prunes old high-frequency rows from `snapshots` and `strategy_quotes`
  (default raw retention: 120 days).
- Keeps rollups for long-horizon chart queries.

Useful flags:

- `--dry-run`: show expected changes without writing.
- `--raw-retention-days N`: change high-frequency retention window.
- `--no-prune-strategy-quotes`: keep all `strategy_quotes` rows.
- `--vacuum`: reclaim on-disk space after pruning.

Note: run maintenance when the engine is idle when possible.

### Cross-Platform Scheduled Backups (No OS Scheduler Required)

To keep backups + rollups running on a fixed cadence across Linux/macOS/Windows,
run the built-in Python scheduler loop:

```bash
python scripts/scheduled_db_maintenance.py \
  --db data/xop_trader.db \
  --backup-dir data/backups \
  --interval-minutes 360 \
  --keep-last 30 \
  --keep-days 30 \
  --raw-retention-days 120 \
  --compress-backups
```

This process is platform-neutral and performs, each cycle:

- crash-consistent SQLite backup copy
- backup rotation (`keep-last` and `keep-days`)
- rollup/prune maintenance via `maintain_snapshot_rollups.py`
- status JSON update for monitoring
- single-instance lockfile enforcement

Useful options:

- `--once`: run one cycle and exit (good for validation)
- `--vacuum-every N`: run VACUUM every N cycles
- `--no-prune-strategy-quotes`: preserve all strategy quote history
- `--compress-backups`: store backups as `.sqlite3.gz`
- `--status-file PATH`: write machine-readable cycle health output
- `--lock-file PATH`: choose lockfile location
- `--stale-lock-minutes N`: auto-recover stale lockfiles

Status file fields include `status`, `exit_code`, `started_at`,
`completed_at`, `duration_seconds`, and backup/retention metadata.

---

## Roadmap

### Phase 1: Foundation (Current)
- [x] Chia RPC client (full node + wallet, mTLS)
- [x] dexie.space API client (rate-limited)
- [x] Avellaneda-Stoikov + GLFT strategy engine
- [x] 4-component spread optimizer
- [x] Multi-tier offer ladder
- [x] Inventory tracking + cost basis
- [x] Risk limits + circuit breakers
- [x] SQLite audit trail
- [x] Prometheus metrics
- [x] Backtesting framework
- [x] Competitor detection infrastructure
- [x] 81 unit tests

### Phase 2: Live Deployment
- [ ] Deploy with minimal capital ($1K) on mainnet
- [ ] Start with 1-2 pairs (XCH/wUSDC, SBX/XCH)
- [ ] Calibrate parameters from live fill data
- [ ] Grafana dashboard configuration

### Phase 3: Scaling
- [ ] Scale to $10K-$30K
- [ ] Add 4-6 more pairs
- [ ] GPU-accelerated parameter optimization (CUDA)
- [ ] DBX incentive farming
- [ ] CEX-DEX arbitrage execution

### Phase 4: Dominance
- [ ] Become primary liquidity provider
- [ ] Add CEX hedging (layers 5-7)
- [ ] Market-making-as-a-service for new CAT launches
- [ ] WebSocket integration for real-time fill notifications

---

## Market Context

| Metric | Value (March 2026) |
|--------|-------------------|
| XCH Price | ~$2.70-$2.88 |
| DEX Daily Volume | ~$2,000 |
| CEX Daily Volume | ~$2.4M |
| Current DEX Spreads | 300-1000 bps |
| Our Minimum Spread | 35-60 bps |
| Block Time | ~52 seconds |
| Settlement Cost | ~0.0001 XCH |
| Professional MM Competition | **Zero** |

This is a **greenfield opportunity** — no professional market makers exist on CHIA DEXes today.

---

*Built with 20 parallel Claude Opus 4.6 ultrathink agents. See [CHIA_MARKET_MAKER_STRATEGY.md](CHIA_MARKET_MAKER_STRATEGY.md) for the full research document.*

---

## Research Documentation

In-depth research and planning documents covering the ecosystem, strategies, architecture options, and implementation roadmap. See [docs/README.md](docs/README.md) for the full documentation index.

### Architecture & Implementation Reference (this PR)

| Document | Description |
|----------|-------------|
| [docs/01-chia-ecosystem-overview.md](docs/01-chia-ecosystem-overview.md) | Chia fundamentals, Chialisp, CATs, Offers protocol, DEX landscape (Dexie, TibetSwap), market opportunities and risks |
| [docs/02-market-making-strategies.md](docs/02-market-making-strategies.md) | Strategy options: basic spread, grid trading, Avellaneda-Stoikov model, AMM LP, arbitrage; pricing, inventory, and risk management |
| [docs/03-technical-architecture.md](docs/03-technical-architecture.md) | System design, component breakdown, technology stack options, Chia wallet RPC integration, deployment, and security |
| [docs/04-implementation-roadmap.md](docs/04-implementation-roadmap.md) | Phased build plan aligned to C++20 layout, capital requirements, testing strategy, and launch checklist |

### Comprehensive Research Series (from PR #3)

| Document | Description |
|----------|-------------|
| [docs/01-chia-market-maker-overview.md](docs/01-chia-market-maker-overview.md) | Chia introduction, offer system, market making fundamentals, and strategic considerations |
| [docs/02-dex-platforms-analysis.md](docs/02-dex-platforms-analysis.md) | Detailed analysis of TibetSwap, Dexie, OfferBin, and platform selection guide |
| [docs/03-market-making-strategies.md](docs/03-market-making-strategies.md) | Nine market making strategies from beginner ($100) to professional ($100K+) capital levels |
| [docs/04-technical-implementation.md](docs/04-technical-implementation.md) | Step-by-step Python bot implementation with code examples, wallet RPC, and deployment |
| [docs/05-risk-management-operations.md](docs/05-risk-management-operations.md) | Comprehensive risk management framework, circuit breakers, and operational procedures |
| [docs/06-implementation-roadmap.md](docs/06-implementation-roadmap.md) | Phased roadmap from beginner to professional market maker across 6 capital phases |

### Concise Research Brief (from PR #4)

| Document | Description |
|----------|-------------|
| [docs/chia-market-maker.md](docs/chia-market-maker.md) | Concise research brief: DEX landscape, four market-making models, risk controls, architecture playbook |
