# Technical Architecture: Chia DEX Market Maker

## Table of Contents
1. [System Architecture Overview](#1-system-architecture-overview)
2. [Component Design](#2-component-design)
3. [Technology Stack Options](#3-technology-stack-options)
4. [Chia Blockchain Integration](#4-chia-blockchain-integration)
5. [DEX Integration Layer](#5-dex-integration-layer)
6. [Data Storage and State Management](#6-data-storage-and-state-management)
7. [Deployment Options](#7-deployment-options)
8. [Security Architecture](#8-security-architecture)
9. [Monitoring and Alerting](#9-monitoring-and-alerting)
10. [Reference Implementations](#10-reference-implementations)

---

## 1. System Architecture Overview

### High-Level Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                          XOPTrader System                               │
│                                                                          │
│  ┌──────────────┐     ┌──────────────────────┐     ┌────────────────┐  │
│  │   Strategy   │────▶│   Market Making Core  │────▶│  Offer Manager │  │
│  │   Engine     │◀────│   (Pricing & Spreads) │◀────│  (Lifecycle)   │  │
│  └──────────────┘     └──────────────────────┘     └────────────────┘  │
│         │                        │                          │           │
│         ▼                        ▼                          ▼           │
│  ┌──────────────┐     ┌──────────────────────┐     ┌────────────────┐  │
│  │    Price     │     │   Inventory Manager   │     │  Wallet / RPC  │  │
│  │    Oracle    │     │   (Coin Management)   │     │   Interface    │  │
│  └──────────────┘     └──────────────────────┘     └────────────────┘  │
│         │                        │                          │           │
└─────────┼────────────────────────┼──────────────────────────┼──────────┘
          │                        │                          │
          ▼                        ▼                          ▼
   ┌─────────────┐        ┌─────────────────┐        ┌──────────────┐
   │  CEX APIs   │        │  SQLite / Postgres│        │  Chia Node   │
   │  (Price)    │        │  (State/History) │        │  Full Node   │
   └─────────────┘        └─────────────────┘        └──────────────┘
          │                                                    │
          ▼                                                    ▼
   ┌─────────────┐                                   ┌──────────────┐
   │  OKX/MEXC   │                                   │  Dexie API   │
   │  Gate.io    │                                   │  TibetSwap   │
   └─────────────┘                                   └──────────────┘
```

### Data Flow

```
External Price Feed ──▶ Price Oracle
                              │
                              ▼
Offer Status Feed ──▶ Strategy Engine ──▶ New Offer Parameters
                              │
                              ▼
                      Pricing Calculator
                       (spread, size)
                              │
                              ▼
                      Wallet RPC ──▶ Offer File Creation
                              │
                              ▼
                       Dexie API ──▶ Offer Submission
                              │
                              ▼
                       State Database ──▶ Monitoring / Alerting
```

---

## 2. Component Design

### 2.1 Strategy Engine
**Responsibility**: Decides *when* and *how* to quote prices.

```python
class StrategyEngine:
    """Core decision-making component."""

    def __init__(self, config: StrategyConfig):
        self.config = config
        self.price_oracle = PriceOracle(config.price_sources)
        self.inventory_manager = InventoryManager(config.target_ratios)

    async def compute_quotes(self, pair: TradingPair) -> QuoteResult:
        """Compute bid/ask quotes for a trading pair."""
        mid_price = await self.price_oracle.get_mid_price(pair)
        inventory = await self.inventory_manager.get_inventory(pair)
        volatility = await self.price_oracle.get_volatility(pair)
        
        spread = self.calculate_spread(volatility, inventory)
        bid = mid_price * (1 - spread / 2)
        ask = mid_price * (1 + spread / 2)
        size = self.calculate_size(inventory)
        
        return QuoteResult(bid=bid, ask=ask, size=size, valid_for_blocks=100)

    def calculate_spread(self, volatility: float, inventory: Inventory) -> float:
        """Dynamic spread based on volatility and inventory skew."""
        base_spread = self.config.base_spread
        vol_adj = volatility * self.config.vol_multiplier
        inv_adj = abs(inventory.skew) * self.config.skew_penalty
        return base_spread + vol_adj + inv_adj
```

### 2.2 Price Oracle
**Responsibility**: Aggregates price data from multiple sources.

```python
class PriceOracle:
    """Multi-source price aggregator."""

    SOURCES = {
        "okx": "https://www.okx.com/api/v5/market/ticker?instId={pair}-USDT",
        "mexc": "https://api.mexc.com/api/v3/ticker/price?symbol={PAIR}USDT",
        "gate": "https://api.gateio.ws/api/v4/spot/tickers?currency_pair={pair}_USDT",
    }

    async def get_mid_price(self, pair: TradingPair) -> float:
        """Fetch and aggregate price from multiple CEX sources."""
        prices = await asyncio.gather(*[
            self._fetch_cex_price(source, pair)
            for source in self.config.enabled_sources
        ], return_exceptions=True)
        valid_prices = [p for p in prices if isinstance(p, float)]
        if not valid_prices:
            raise PriceOracleError(f"No valid prices for {pair}")
        return statistics.median(valid_prices)

    async def get_volatility(self, pair: TradingPair, window_hours: int = 1) -> float:
        """Compute rolling price volatility."""
        historical = await self._fetch_historical_prices(pair, window_hours)
        returns = [log(historical[i+1]/historical[i]) for i in range(len(historical)-1)]
        return statistics.stdev(returns) if len(returns) > 1 else 0.0
```

### 2.3 Offer Manager
**Responsibility**: Manages the full lifecycle of Chia Offers.

```python
class OfferManager:
    """Manages offer creation, submission, monitoring, and cancellation."""

    async def create_offer(self, params: OfferParams) -> Offer:
        """Create an offer via Chia wallet RPC."""
        # create_offer_for_ids keys are wallet IDs (int), not asset IDs (str).
        # Use wallet_id=1 for XCH; for CATs, look up the wallet ID from the wallet list.
        result = await self.wallet_rpc.create_offer_for_ids({
            "offer": {
                params.offered_wallet_id: -params.offered_amount,
                params.requested_wallet_id: params.requested_amount,
            },
            "fee": params.blockchain_fee,
            "max_height": params.expiry_block_height,
        })
        offer = Offer(
            id=result["trade_id"],
            file=result["offer"],
            status=OfferStatus.PENDING_SUBMISSION,
        )
        await self.db.save_offer(offer)
        return offer

    async def submit_offer(self, offer: Offer) -> None:
        """Submit offer to Dexie."""
        result = await self.dexie_client.post_offer(offer.file)
        offer.dexie_id = result["id"]
        offer.status = OfferStatus.ACTIVE
        await self.db.update_offer(offer)

    async def cancel_offer(self, offer: Offer, secure: bool = True) -> None:
        """Cancel an offer on-chain."""
        await self.wallet_rpc.cancel_offer({
            "trade_id": offer.id,
            "secure": secure,  # True = on-chain cancel, False = just mark local
            "fee": 0,
        })
        offer.status = OfferStatus.CANCELLED
        await self.db.update_offer(offer)

    async def monitor_offers(self) -> None:
        """Background task: monitor all active offers for fills/expiry."""
        while True:
            active_offers = await self.db.get_active_offers()
            for offer in active_offers:
                status = await self.dexie_client.get_offer_status(offer.dexie_id)
                if status == "completed":
                    offer.status = OfferStatus.FILLED
                    await self.db.update_offer(offer)
                    await self.on_offer_filled(offer)
            await asyncio.sleep(30)  # Poll every 30 seconds
```

### 2.4 Inventory Manager
**Responsibility**: Tracks asset balances and manages coin selection.

```python
class InventoryManager:
    """Manages wallet inventory and coin selection."""

    async def get_inventory(self, pair: TradingPair) -> Inventory:
        """Get current inventory for a trading pair."""
        # get_spendable_coins takes a wallet_id (int), not an asset_id (str).
        # Use pair.base_wallet_id / pair.quote_wallet_id resolved via get_wallet_id_for_asset().
        base_coins = await self.wallet_rpc.get_spendable_coins(pair.base_wallet_id)
        quote_coins = await self.wallet_rpc.get_spendable_coins(pair.quote_wallet_id)
        locked = await self.get_locked_coins()  # Coins in open offers
        
        available_base = sum(c.amount for c in base_coins if c.name not in locked)
        available_quote = sum(c.amount for c in quote_coins if c.name not in locked)
        
        return Inventory(
            base_asset=pair.base_wallet_id,
            quote_asset=pair.quote_wallet_id,
            base_amount=available_base,
            quote_amount=available_quote,
        )

    def select_coins(self, coins: list[Coin], target: int) -> list[Coin]:
        """Select minimal set of coins to cover target amount."""
        # Prefer fewest coins (reduces transaction size)
        coins.sort(key=lambda c: c.amount, reverse=True)
        selected, total = [], 0
        for coin in coins:
            if total >= target:
                break
            selected.append(coin)
            total += coin.amount
        if total < target:
            raise InsufficientInventoryError(f"Need {target}, have {total}")
        return selected
```

### 2.5 Wallet RPC Interface
**Responsibility**: Interfaces with the Chia wallet daemon.

```python
class ChiaWalletRPC:
    """Interface to Chia wallet daemon via JSON-RPC."""

    def __init__(self, hostname: str, port: int, cert_path: str, key_path: str):
        self.base_url = f"https://{hostname}:{port}"
        self.session = create_ssl_session(cert_path, key_path)

    async def create_offer_for_ids(self, params: dict) -> dict:
        return await self._call("create_offer_for_ids", params)

    async def get_all_offers(self, **kwargs) -> list[dict]:
        return await self._call("get_all_offers", kwargs)

    async def cancel_offer(self, params: dict) -> dict:
        return await self._call("cancel_offer", params)

    async def get_spendable_coins(self, wallet_id: int) -> list[dict]:
        return await self._call("get_spendable_coins", {"wallet_id": wallet_id})

    async def _call(self, method: str, params: dict) -> dict:
        async with self.session.post(
            f"{self.base_url}/{method}",
            json=params
        ) as resp:
            result = await resp.json()
            if not result.get("success"):
                raise WalletRPCError(f"{method} failed: {result}")
            return result
```

---

## 3. Technology Stack Options

### Option A: Python (Recommended)
**Why Python**: Best ecosystem support for Chia, native `chia-blockchain` library.

| Component | Library | Notes |
|-----------|---------|-------|
| Core Runtime | Python 3.11+ | Type hints, asyncio |
| Async HTTP | `httpx` | Modern async HTTP client |
| WebSocket | `websockets` | Real-time Dexie stream |
| Chia SDK | `chia-blockchain` | Official Python package |
| Database | `aiosqlite` / `asyncpg` | SQLite for simple, Postgres for scale |
| Scheduling | `apscheduler` | Task scheduling |
| Config | `pydantic` | Type-safe config |
| Testing | `pytest-asyncio` | Async test support |
| Logging | `structlog` | Structured logging |

**Advantages:**
- Direct access to `chia-blockchain` library
- Large number of community Chia Python tools
- Rapid development

**Disadvantages:**
- Python's GIL can limit concurrency (mitigated by asyncio)
- Slower execution than compiled languages

### Option B: TypeScript/Node.js
**When to choose**: If team has stronger JavaScript expertise.

| Component | Library |
|-----------|---------|
| Core Runtime | Node.js 20+ / TypeScript |
| HTTP | `axios` / `node-fetch` |
| Chia Integration | `chia-crypto-utils` / custom RPC wrapper |
| Database | `better-sqlite3` / `pg` |
| Scheduling | `node-cron` |

**Limitations:**
- Less native Chia tooling in TypeScript
- Would need to implement wallet RPC wrapper

### Option C: Go
**When to choose**: High-performance requirements, low latency.

| Component | Library |
|-----------|---------|
| HTTP Client | `net/http` (stdlib) |
| WebSocket | `gorilla/websocket` |
| Chia Integration | Custom via JSON-RPC |
| Database | `database/sql` + `mattn/go-sqlite3` |

**Advantages:**
- Excellent concurrency with goroutines
- Low memory footprint
- Fast execution

**Limitations:**
- No official Chia Go SDK; must implement RPC manually
- Higher initial development effort

### Recommended Stack Decision
```
Use Python if:  - Team knows Python
                - Want fastest time to market
                - Need direct chia-blockchain library access

Use TypeScript: - Team has strong JS/TS background
                - Need WebSocket-heavy real-time features

Use Go:         - Performance is critical (arbitrage bot)
                - Team has Go experience
                - Long-term maintenance priority
```

---

## 4. Chia Blockchain Integration

### Node Requirements
The bot requires access to a Chia full node and wallet daemon:

**Option 1: Run Your Own Full Node (Recommended)**
```
# Minimum hardware requirements
CPU:    2+ cores
RAM:    8 GB+
Disk:   400 GB+ (SSD recommended, ~300 GB blockchain)
Network: Stable internet connection
```

**Option 2: Use a Trusted Node Provider**
- More convenient, but introduces trust assumption
- `Chia Friends` or other community nodes
- Use HTTPS and verify TLS certificates

### Wallet Configuration
```bash
# Start Chia services
chia start wallet

# Get wallet fingerprint
chia wallet show

# Find wallet RPC certificate paths
ls ~/.chia/mainnet/config/ssl/wallet/

# Test RPC connection (using Chia's CA cert for proper TLS verification)
curl --cacert ~/.chia/mainnet/config/ssl/ca/chia_ca.crt \
     --cert ~/.chia/mainnet/config/ssl/wallet/private_wallet.crt \
     --key ~/.chia/mainnet/config/ssl/wallet/private_wallet.key \
     -H "Content-Type: application/json" \
     -d '{}' \
     https://localhost:9256/get_wallets
# Note: add --insecure only as a last resort for local debugging, never against remote hosts.
```

### Asset ID Management
Each trading asset needs its Chia asset ID (TAIL hash for CATs, or 0 for XCH):

```python
# Known asset IDs (mainnet)
ASSET_IDS = {
    "XCH": None,      # None or 0 in offer params means XCH
    "USDS": "6d95dae356e32a71db5ddcb42224754a02524c615c5fc35f568c2af04774e589",
    "DBX":  "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20",
    "WBTC.b": "46cd4c5629d79d51e95b6c2aa0d3b99f34b02b52f30f9a3fcfd13c55db9ecf91",
    "WETH.b": "a628c1c2c6fcb74d53746157e438e108eab5c0bb3e5c80ff9b1910b3e4832913",
    "MRMT":   "8ebf855de6eb146db5602f0456d2f0cbe750d57f821b6f91a8592ee9f1d4cf31",
}

def get_wallet_id_for_asset(asset_id: str | None) -> int:
    """Get the Chia wallet ID for a given asset."""
    if asset_id is None:
        return 1  # XCH wallet is always ID 1
    # For CATs, find their wallet ID from the wallet list
    wallets = wallet_rpc.get_wallets()
    for wallet in wallets:
        if wallet.get("data", {}).get("asset_id") == asset_id:
            return wallet["id"]
    raise ValueError(f"No wallet found for asset {asset_id}")
```

### Offer File Structure Reference
```python
# Offer creation parameters (wallet RPC)
# Resolve asset IDs to wallet IDs first (see get_wallet_id_for_asset above)
USDS_ASSET_ID = ASSET_IDS["USDS"]
xch_wallet_id = get_wallet_id_for_asset(ASSET_IDS["XCH"])     # Typically 1
usds_wallet_id = get_wallet_id_for_asset(USDS_ASSET_ID)

offer_request = {
    "offer": {
        # Positive = requesting, Negative = offering
        # XCH amount in mojos (1 XCH = 1e12 mojos)
        # CAT amount in CAT mojos (depends on token precision)
        xch_wallet_id: -100_000_000_000,   # Offer 0.1 XCH
        usds_wallet_id: 10_000_000,        # Request 10 USDS (6 decimals)
    },
    "fee": 0,                              # Chia network fee in mojos
    "validate_only": False,                # Set True to validate without creating
    "driver_dict": {},                     # NFT metadata (empty for fungible assets)
    "min_coin_amount": 0,                  # Minimum coin size to use
    "max_coin_amount": None,               # Maximum coin size to use
    "coin_announcements": [],              # Additional announcement assertions
    "puzzle_announcements": [],            # Additional puzzle announcement assertions
    "solver": None,                        # Custom solver for complex offers
    "reuse_puzhash": False,                # Whether to reuse puzzle hash for change
}
```

---

## 5. DEX Integration Layer

### Dexie Client Implementation
```python
class DexieClient:
    """Client for the Dexie DEX API."""

    BASE_URL = "https://api.dexie.space/v1"
    WS_URL = "wss://api.dexie.space/v1/stream"

    async def post_offer(self, offer_str: str) -> dict:
        """Submit an offer to Dexie."""
        async with httpx.AsyncClient() as client:
            response = await client.post(
                f"{self.BASE_URL}/offers",
                json={
                    "offer": offer_str,
                    "drop_only": False,
                    "claim_rewards": True,  # Auto-claim DBX rewards
                }
            )
            response.raise_for_status()
            return response.json()

    async def get_offer(self, offer_id: str) -> dict:
        """Get details of a specific offer."""
        async with httpx.AsyncClient() as client:
            response = await client.get(f"{self.BASE_URL}/offers/{offer_id}")
            response.raise_for_status()
            return response.json()["offer"]

    async def get_best_offers(self, offered_coin: str, requested_coin: str,
                               limit: int = 5) -> list[dict]:
        """Get the best available offers for a pair."""
        params = {
            "offered": offered_coin,
            "requested": requested_coin,
            "status": 1,  # Active offers only
            "sort": "price_asc",
            "count": limit,
        }
        async with httpx.AsyncClient() as client:
            response = await client.get(f"{self.BASE_URL}/offers", params=params)
            response.raise_for_status()
            return response.json()["offers"]

    async def stream_offers(self, callback):
        """Stream real-time offer updates via WebSocket."""
        import websockets
        async with websockets.connect(self.WS_URL) as ws:
            async for message in ws:
                data = json.loads(message)
                await callback(data)
```

### TibetSwap Client Implementation
```python
class TibetSwapClient:
    """Client for TibetSwap AMM API."""

    BASE_URL = "https://v2.tibetswap.io/api"

    async def get_pairs(self) -> list[dict]:
        """Get all available trading pairs."""
        async with httpx.AsyncClient() as client:
            response = await client.get(f"{self.BASE_URL}/pairs")
            return response.json()

    async def get_pair_info(self, asset_id: str) -> dict:
        """Get pool info for a specific CAT/XCH pair."""
        async with httpx.AsyncClient() as client:
            response = await client.get(f"{self.BASE_URL}/pair/{asset_id}")
            return response.json()

    async def get_quote(self, asset_id: str, xch_amount: int,
                         xch_to_cat: bool) -> dict:
        """Get a swap quote from TibetSwap."""
        params = {
            "asset_id": asset_id,
            "xch_amount": xch_amount,
            "xch_to_cat": xch_to_cat,
        }
        async with httpx.AsyncClient() as client:
            response = await client.get(
                f"{self.BASE_URL}/quote", params=params
            )
            return response.json()

    def calculate_spot_price(self, pair_info: dict) -> float:
        """Calculate current spot price from pool reserves."""
        xch_reserve = pair_info["xch_reserve"]
        cat_reserve = pair_info["token_reserve"]
        if cat_reserve == 0:
            return 0.0
        return xch_reserve / cat_reserve  # XCH per CAT
```

---

## 6. Data Storage and State Management

### Database Schema

```sql
-- Offers table: tracks all offer lifecycle
CREATE TABLE offers (
    id              TEXT PRIMARY KEY,       -- Chia wallet trade ID
    dexie_id        TEXT,                   -- Dexie offer ID
    pair            TEXT NOT NULL,          -- e.g., "XCH/USDS"
    side            TEXT NOT NULL,          -- "BUY" or "SELL"
    offered_asset   TEXT,                   -- NULL for XCH
    offered_amount  INTEGER NOT NULL,       -- In mojos
    requested_asset TEXT,                   -- NULL for XCH
    requested_amount INTEGER NOT NULL,      -- In mojos
    price           REAL NOT NULL,          -- Effective price
    status          TEXT NOT NULL,          -- ACTIVE, FILLED, CANCELLED
    created_at      INTEGER NOT NULL,       -- UNIX timestamp
    created_height  INTEGER NOT NULL,       -- Block height at creation
    filled_at       INTEGER,                -- UNIX timestamp (if filled)
    cancelled_at    INTEGER,                -- UNIX timestamp (if cancelled)
    offer_file      TEXT NOT NULL           -- Full offer string
);

-- Trades table: records of completed trades
CREATE TABLE trades (
    id              TEXT PRIMARY KEY,
    offer_id        TEXT REFERENCES offers(id),
    pair            TEXT NOT NULL,
    side            TEXT NOT NULL,
    amount          INTEGER NOT NULL,
    price           REAL NOT NULL,
    executed_at     INTEGER NOT NULL,
    pnl_estimate    REAL                    -- Estimated PnL for this trade
);

-- Price history table
CREATE TABLE price_history (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    pair        TEXT NOT NULL,
    source      TEXT NOT NULL,              -- "okx", "tibetswap", etc.
    price       REAL NOT NULL,
    timestamp   INTEGER NOT NULL
);

-- Inventory snapshots
CREATE TABLE inventory_snapshots (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    asset_id    TEXT,                       -- NULL for XCH
    amount      INTEGER NOT NULL,           -- In mojos
    usd_value   REAL,
    timestamp   INTEGER NOT NULL
);
```

### State Management Patterns

```python
class StateManager:
    """Manages bot state with persistence and recovery."""

    async def save_checkpoint(self):
        """Save current state for recovery after restart."""
        state = {
            "active_offers": [o.to_dict() for o in self.offer_manager.get_active()],
            "last_mid_prices": self.price_oracle.get_cached_prices(),
            "session_pnl": self.pnl_tracker.get_session_pnl(),
            "timestamp": time.time(),
        }
        async with aiofiles.open("state_checkpoint.json", "w") as f:
            await f.write(json.dumps(state, indent=2))

    async def recover_from_checkpoint(self):
        """Restore state after restart."""
        try:
            async with aiofiles.open("state_checkpoint.json") as f:
                state = json.loads(await f.read())
            # Re-sync with Dexie to check which offers are still active
            await self.sync_offer_states(state["active_offers"])
        except FileNotFoundError:
            logger.info("No checkpoint found, starting fresh")
```

---

## 7. Deployment Options

### Option A: Single VPS/Cloud VM
**Best for**: Getting started, low cost

```yaml
# Recommended specs
CPU:    2 vCPUs
RAM:    8 GB (4 GB for Chia node, 2 GB for bot, 2 GB OS)
Disk:   500 GB SSD (blockchain + OS)
OS:     Ubuntu 22.04 LTS
Cost:   ~$20-40/month (DigitalOcean, Linode, Vultr)
```

**Deployment steps:**
```bash
# Install Chia
pip install chia-blockchain
chia init
chia start node wallet

# Wait for node sync (~1-2 days initially)
chia show -s

# Clone and install XOPTrader
git clone https://github.com/dorkmo/XOPTrader.git
cd XOPTrader
# Install dependencies as described in README.md
# (C++20 engine: see cpp/ build instructions; Python deps: pip install chia-blockchain)

# Configure environment
cp config.example.yaml config.yaml
# Edit config.yaml with wallet fingerprint, trading pairs, etc.

# Run with systemd (after creating /etc/systemd/system/xoptrader.service)
sudo systemctl daemon-reload
sudo systemctl enable xoptrader
sudo systemctl start xoptrader
```

### Option B: Separate Node + Bot
**Best for**: Reliability, bot restarts don't affect node

```
Server 1 (Chia Node):   ~$30/month  - Full node + wallet daemon
Server 2 (Trading Bot): ~$10/month  - Lightweight bot process
```

The bot connects to the remote Chia node via SSL-authenticated RPC. This separation allows:
- Bot restarts without re-syncing the node
- Bot updates without downtime risk to the node
- Different scaling for each component

### Option C: Docker Compose
**Best for**: Reproducible deployments, easy updates

```yaml
# docker-compose.yml
version: "3.8"
services:
  chia-node:
    image: ghcr.io/chia-network/chia:latest
    volumes:
      - chia_data:/root/.chia
    ports:
      - "8444:8444"   # Peer connections
    restart: unless-stopped

  xoptrader:
    build: .
    volumes:
      - ./config.toml:/app/config.toml:ro
      - chia_data:/root/.chia:ro   # Read-only access to Chia config/certs
    depends_on:
      - chia-node
    environment:
      - CHIA_ROOT=/root/.chia
      - WALLET_FINGERPRINT=${WALLET_FINGERPRINT}
    restart: unless-stopped

  prometheus:
    image: prom/prometheus
    volumes:
      - ./monitoring/prometheus.yml:/etc/prometheus/prometheus.yml
    ports:
      - "9090:9090"

  grafana:
    image: grafana/grafana
    ports:
      - "3000:3000"

volumes:
  chia_data:
```

### Option D: Cloud Functions (Serverless)
**Best for**: Very low volume, minimal infra management

**Limitations for this use case:**
- Not recommended; market making requires persistent state and frequent polling
- Cold start latency unacceptable for time-sensitive offer management
- Would still need a persistent Chia node somewhere

---

## 8. Security Architecture

### Private Key Protection

```
NEVER hardcode private keys or mnemonics in code.
Use the Chia wallet daemon for all key operations.
The bot only needs the wallet fingerprint and RPC access.
```

**Key management approach:**
```python
# config.toml (never commit this file)
[wallet]
fingerprint = 1234567890    # Wallet fingerprint (not the key itself)
rpc_host = "localhost"
rpc_port = 9256
cert_path = "~/.chia/mainnet/config/ssl/wallet/private_wallet.crt"
key_path  = "~/.chia/mainnet/config/ssl/wallet/private_wallet.key"

# All signing happens INSIDE the Chia wallet daemon
# The bot never touches the actual private key
```

### Secrets Management
```bash
# Use environment variables or a secrets manager, never hardcode
export WALLET_FINGERPRINT=1234567890
export DEXIE_API_KEY=your_api_key_if_needed

# For cloud deployments: use AWS Secrets Manager, HashiCorp Vault, etc.
```

### Hot Wallet vs. Cold Wallet Strategy
```
Cold Storage (Hardware wallet):
  - 90% of total funds
  - Never used for trading

Hot Wallet (Chia software wallet on the server):
  - 10% of total funds (operational capital)
  - Used for active market making
  - Top up from cold storage manually as needed
```

### Network Security
```bash
# Firewall: only expose necessary ports
ufw default deny incoming
ufw allow ssh
ufw allow 8444/tcp   # Chia peer connections (outbound only ideally)
ufw deny 9256/tcp    # Wallet RPC: NEVER expose publicly
ufw deny 9255/tcp    # Full node RPC: NEVER expose publicly
ufw enable
```

### API Rate Limiting Compliance
```python
# Respect Dexie API rate limits
# As of current docs: no published limit but be respectful
# Recommended: max 1 request/second for polling

class RateLimitedDexieClient(DexieClient):
    def __init__(self):
        super().__init__()
        self._rate_limiter = asyncio.Semaphore(1)
        self._min_interval = 1.0  # seconds between requests

    async def _request(self, *args, **kwargs):
        async with self._rate_limiter:
            result = await super()._request(*args, **kwargs)
            await asyncio.sleep(self._min_interval)
            return result
```

---

## 9. Monitoring and Alerting

### Metrics to Track
```python
# Key metrics for market maker health

METRICS = {
    # Operational
    "bot_uptime_seconds": "Total uptime since last restart",
    "active_offers_count": "Number of currently active offers",
    "last_price_update_age": "Seconds since last price fetch",
    "node_sync_status": "1 if synced, 0 if not",

    # Trading performance
    "total_fills_count": "Total number of filled offers",
    "total_spread_income_usd": "Cumulative spread income in USD",
    "current_dbx_rewards": "Pending DBX rewards",
    "daily_pnl_usd": "PnL for current day",

    # Inventory
    "xch_inventory_amount": "XCH available for trading (mojos)",
    "usds_inventory_amount": "USDS available for trading",
    "inventory_ratio": "Base asset fraction (0.5 = balanced)",

    # Risk
    "max_offer_age_blocks": "Age of oldest active offer",
    "price_deviation_pct": "% deviation from target mid price",
}
```

### Alerting Rules
```yaml
# Prometheus alerting rules
groups:
  - name: xoptrader_alerts
    rules:
      - alert: BotDown
        expr: up{job="xoptrader"} == 0
        for: 2m
        annotations:
          summary: "XOPTrader bot is down"

      - alert: NodeNotSynced
        expr: chia_node_synced == 0
        for: 5m
        annotations:
          summary: "Chia node is not synced"

      - alert: LowInventory
        expr: xch_inventory_amount < 1_000_000_000_000   # < 1 XCH
        annotations:
          summary: "XCH inventory critically low"

      - alert: HighDailyLoss
        expr: daily_pnl_usd < -100
        annotations:
          summary: "Daily loss exceeds $100 threshold"

      - alert: StaleOffers
        expr: max_offer_age_blocks > 300   # > ~4 hours
        annotations:
          summary: "Offers are stale and should be refreshed"
```

### Telegram/Discord Notifications
```python
async def send_alert(message: str, level: str = "info"):
    """Send a Telegram notification for critical events."""
    if level in ("warning", "error") and TELEGRAM_BOT_TOKEN:
        async with httpx.AsyncClient() as client:
            await client.post(
                f"https://api.telegram.org/bot{TELEGRAM_BOT_TOKEN}/sendMessage",
                json={
                    "chat_id": TELEGRAM_CHAT_ID,
                    "text": f"[XOPTrader] [{level.upper()}] {message}",
                }
            )
```

---

## 10. Reference Implementations

### GreenFloor (Dexie Market Maker)
**GitHub**: [github.com/hoffmang9/greenfloor](https://github.com/hoffmang9/greenfloor)

GreenFloor is an open-source market making system for Chia built on top of Dexie. Key features:
- Manager CLI and daemon for automated offer building
- Expiry-first offer lifecycle policies
- Inventory tracking and rebalancing
- Uses `chia-wallet-sdk`

**What to learn from it:**
- How to structure offer lifecycle management
- Dexie API integration patterns
- Inventory and coin management approaches

### TibetSwap (AMM Contracts)
**GitHub**: [github.com/Yakuhito/tibet](https://github.com/Yakuhito/tibet)

Reference AMM implementation in Chialisp. Key files:
- `tibet/clsp/` – Chialisp contract source
- `tibet/tibet_swap.py` – Python SDK for interacting with pools
- `tibet/pair.py` – Pool state management

**What to learn from it:**
- How AMM pool state is managed in Chia's coinset model
- Singleton pattern implementation in Chialisp
- On-chain swap execution flow

### dexie-rewards
**GitHub**: [github.com/dexie-space/dexie-rewards](https://github.com/dexie-space/dexie-rewards)

Python CLI for claiming Dexie liquidity rewards. Key code patterns:
- Wallet RPC integration
- Dexie API interaction
- Reward claiming workflow

### Hummingbot (General DEX MM Framework)
**GitHub**: [github.com/hummingbot/hummingbot](https://github.com/hummingbot/hummingbot)

While not Chia-specific, Hummingbot is the most mature open-source market maker framework. Study its architecture for:
- Strategy pattern implementation
- Exchange connector abstraction
- Order management system design
- Risk management patterns
