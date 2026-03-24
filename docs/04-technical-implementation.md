# Technical Implementation Guide - Chia Market Maker

## Overview

This document provides detailed technical guidance for implementing a market maker on Chia's decentralized exchanges, including setup, development, and operational considerations.

---

## Table of Contents

1. [Prerequisites and Setup](#prerequisites-and-setup)
2. [Chia Wallet Configuration](#chia-wallet-configuration)
3. [Understanding Offer Files](#understanding-offer-files)
4. [Building a Market Making Bot](#building-a-market-making-bot)
5. [DEX Integration](#dex-integration)
6. [Price Feed Integration](#price-feed-integration)
7. [Deployment and Operations](#deployment-and-operations)
8. [Security Best Practices](#security-best-practices)

---

## Prerequisites and Setup

### System Requirements

**Minimum**:
- OS: Linux (Ubuntu 20.04+), macOS, or Windows 10+
- CPU: 4 cores
- RAM: 8 GB
- Storage: 100 GB SSD (for full node) or 10 GB (light wallet)
- Network: Stable internet connection

**Recommended for Production**:
- OS: Linux (Ubuntu 22.04 LTS)
- CPU: 8+ cores
- RAM: 16+ GB
- Storage: 250+ GB NVMe SSD
- Network: Dedicated server/VPS with high uptime

### Software Dependencies

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y python3.10 python3-pip git build-essential

# Python packages
pip3 install chia-blockchain
pip3 install requests
pip3 install python-dotenv
pip3 install schedule  # For task scheduling
```

### Development Environment

```bash
# Clone Chia blockchain (if building from source)
git clone https://github.com/Chia-Network/chia-blockchain.git
cd chia-blockchain

# Install Chia
sh install.sh
. ./activate

# Initialize Chia
chia init
```

---

## Chia Wallet Configuration

### Setting Up Chia Wallet

#### Option 1: Full Node (Recommended for Production)

```bash
# Start Chia full node
chia start node

# Start wallet
chia start wallet

# Check sync status
chia show -s

# Wait for full sync (may take hours/days for first sync)
```

#### Option 2: Light Wallet (Faster Setup)

```bash
# Configure light wallet mode
chia configure --set-log-level INFO
chia configure --enable-upnp false

# Start wallet in light mode
chia start wallet-only
```

### Wallet Management

```bash
# Create new wallet
chia keys generate

# Show wallet address
chia wallet get_address

# Check balance
chia wallet show

# Get transaction history
chia wallet get_transactions
```

### Security Setup

```bash
# Backup mnemonic seed (24 words) - CRITICAL!
# Store securely offline in multiple locations

# Set wallet password
chia keys set_label -i <fingerprint> -l "MarketMaker_Wallet"

# For production: use separate hot/cold wallets
# Hot wallet: Small amount for trading
# Cold wallet: Bulk of funds (offline)
```

---

## Understanding Offer Files

### Offer File Structure

Chia offers are cryptographically signed transactions stored as binary or text files:

```python
# Offer file contains:
{
    "offered": {
        "xch": 1000000000000,  # 1 XCH in mojos (1 XCH = 10^12 mojos)
    },
    "requested": {
        "cat_asset_id": 100000000000,  # Amount of CAT requested
    },
    "fee": 100000000,  # Transaction fee in mojos (0.0001 XCH)
    "signatures": [...],  # Cryptographic signatures
    "coin_spends": [...]  # Coin information
}
```

### Creating Offers via CLI

#### Basic XCH to CAT Offer

```bash
# Offer 1 XCH for 100 USDC (CAT)
chia wallet make_offer \
  -o 1:1 \  # Offer 1 XCH (wallet ID 1)
  -r <CAT_ASSET_ID>:100 \  # Request 100 CAT
  -p ~/offers/xch_to_usdc_offer.txt \
  -m 0.0001  # Fee in XCH
```

#### CAT to XCH Offer

```bash
# Offer 100 USDC for 1 XCH
chia wallet make_offer \
  -o <WALLET_ID>:100 \  # Offer 100 CAT
  -r 1:1 \  # Request 1 XCH
  -p ~/offers/usdc_to_xch_offer.txt \
  -m 0.0001
```

### Accepting Offers

```bash
# View offer details
chia wallet get_offer_summary -f ~/offers/received_offer.txt

# Accept offer
chia wallet take_offer \
  -f ~/offers/received_offer.txt \
  -m 0.0001  # Fee for acceptance
```

### Canceling Offers

```bash
# Cancel an offer (spend the coins used in offer)
chia wallet cancel_offer -id <OFFER_ID>
```

---

## Building a Market Making Bot

### Architecture Overview

```
┌─────────────────────────────────────────────────┐
│           Market Making Bot                      │
├─────────────────────────────────────────────────┤
│                                                  │
│  ┌──────────────┐    ┌──────────────┐          │
│  │ Price Feed   │───▶│ Strategy     │          │
│  │ Module       │    │ Engine       │          │
│  └──────────────┘    └──────┬───────┘          │
│                              │                   │
│  ┌──────────────┐           ▼                   │
│  │ Risk         │    ┌──────────────┐          │
│  │ Management   │◀──▶│ Offer        │          │
│  └──────────────┘    │ Manager      │          │
│                       └──────┬───────┘          │
│  ┌──────────────┐           │                   │
│  │ Monitoring   │           ▼                   │
│  │ & Alerts     │    ┌──────────────┐          │
│  └──────────────┘    │ Chia Wallet  │          │
│                       │ Interface    │          │
│                       └──────┬───────┘          │
│                              │                   │
│                       ┌──────▼───────┐          │
│                       │ DEX APIs     │          │
│                       │ (Dexie, etc) │          │
│                       └──────────────┘          │
└─────────────────────────────────────────────────┘
```

### Basic Bot Implementation

```python
#!/usr/bin/env python3
"""
Chia Market Maker Bot - Basic Implementation
"""

import subprocess
import json
import time
import requests
from decimal import Decimal
from typing import Dict, List, Optional
import logging

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

class ChiaWalletInterface:
    """Interface to Chia wallet CLI"""

    def __init__(self):
        self.xch_wallet_id = 1
        self.mojos_per_xch = 1_000_000_000_000

    def get_balance(self, wallet_id: int = 1) -> Decimal:
        """Get wallet balance"""
        try:
            result = subprocess.run(
                ['chia', 'wallet', 'show', '-w', str(wallet_id)],
                capture_output=True,
                text=True,
                check=True
            )
            # Parse balance from output
            # (Implementation depends on output format)
            return Decimal('0')  # Placeholder
        except subprocess.CalledProcessError as e:
            logger.error(f"Failed to get balance: {e}")
            return Decimal('0')

    def create_offer(
        self,
        offer_amount: Decimal,
        offer_wallet_id: int,
        request_amount: Decimal,
        request_wallet_id: int,
        fee: Decimal = Decimal('0.0001')
    ) -> Optional[str]:
        """Create a new offer"""
        try:
            offer_file = f"offer_{int(time.time())}.txt"

            cmd = [
                'chia', 'wallet', 'make_offer',
                '-o', f'{offer_wallet_id}:{offer_amount}',
                '-r', f'{request_wallet_id}:{request_amount}',
                '-p', offer_file,
                '-m', str(fee)
            ]

            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                check=True
            )

            # Read offer file content
            with open(offer_file, 'r') as f:
                offer_content = f.read()

            logger.info(f"Created offer: {offer_file}")
            return offer_content

        except subprocess.CalledProcessError as e:
            logger.error(f"Failed to create offer: {e}")
            return None

class PriceFeed:
    """Price feed from external sources"""

    def __init__(self):
        self.coingecko_api = "https://api.coingecko.com/api/v3"
        self.cache = {}
        self.cache_ttl = 60  # seconds

    def get_xch_price(self) -> Optional[Decimal]:
        """Get current XCH price in USD"""
        try:
            response = requests.get(
                f"{self.coingecko_api}/simple/price",
                params={
                    'ids': 'chia',
                    'vs_currencies': 'usd'
                },
                timeout=10
            )
            response.raise_for_status()
            data = response.json()

            price = Decimal(str(data['chia']['usd']))
            logger.info(f"XCH price: ${price}")
            return price

        except Exception as e:
            logger.error(f"Failed to fetch XCH price: {e}")
            return None

class MarketMakingStrategy:
    """Market making strategy logic"""

    def __init__(
        self,
        spread_pct: Decimal = Decimal('0.03'),  # 3% spread
        position_size: Decimal = Decimal('1.0')  # 1 XCH per offer
    ):
        self.spread_pct = spread_pct
        self.position_size = position_size

    def calculate_offers(
        self,
        market_price: Decimal
    ) -> Dict[str, Decimal]:
        """Calculate bid and ask prices"""

        half_spread = self.spread_pct / 2

        bid_price = market_price * (1 - half_spread)
        ask_price = market_price * (1 + half_spread)

        return {
            'bid': bid_price,
            'ask': ask_price,
            'size': self.position_size
        }

class RiskManager:
    """Risk management and position limits"""

    def __init__(
        self,
        max_xch_position: Decimal = Decimal('100'),
        max_usd_position: Decimal = Decimal('5000')
    ):
        self.max_xch_position = max_xch_position
        self.max_usd_position = max_usd_position

    def check_position_limits(
        self,
        current_xch: Decimal,
        current_usd: Decimal
    ) -> Dict[str, bool]:
        """Check if positions are within limits"""

        return {
            'can_buy_xch': current_xch < self.max_xch_position,
            'can_sell_xch': current_usd < self.max_usd_position,
            'xch_usage': current_xch / self.max_xch_position,
            'usd_usage': current_usd / self.max_usd_position
        }

class DexieAPI:
    """Interface to Dexie DEX"""

    def __init__(self):
        self.api_base = "https://api.dexie.space/v1"  # Check actual API

    def post_offer(self, offer_content: str) -> bool:
        """Post offer to Dexie"""
        try:
            # Implementation depends on Dexie API
            # This is a placeholder
            logger.info("Posted offer to Dexie")
            return True
        except Exception as e:
            logger.error(f"Failed to post to Dexie: {e}")
            return False

    def get_active_offers(self) -> List[Dict]:
        """Get your active offers from Dexie"""
        # Placeholder implementation
        return []

class MarketMakerBot:
    """Main market maker bot orchestrator"""

    def __init__(self):
        self.wallet = ChiaWalletInterface()
        self.price_feed = PriceFeed()
        self.strategy = MarketMakingStrategy()
        self.risk_manager = RiskManager()
        self.dexie = DexieAPI()

        self.running = False
        self.update_interval = 300  # 5 minutes

    def run_cycle(self):
        """Execute one market making cycle"""

        logger.info("Starting market making cycle")

        # 1. Get current price
        market_price = self.price_feed.get_xch_price()
        if not market_price:
            logger.error("Failed to get market price, skipping cycle")
            return

        # 2. Check positions
        xch_balance = self.wallet.get_balance(wallet_id=1)
        usd_balance = self.wallet.get_balance(wallet_id=2)  # USDC wallet

        position_check = self.risk_manager.check_position_limits(
            xch_balance,
            usd_balance
        )

        logger.info(f"Position check: {position_check}")

        # 3. Calculate offers
        offers = self.strategy.calculate_offers(market_price)

        # 4. Create and post offers
        if position_check['can_buy_xch']:
            # Create buy offer (offer USD, request XCH)
            buy_offer = self.wallet.create_offer(
                offer_amount=offers['bid'] * offers['size'],
                offer_wallet_id=2,  # USD wallet
                request_amount=offers['size'],
                request_wallet_id=1  # XCH wallet
            )

            if buy_offer:
                self.dexie.post_offer(buy_offer)

        if position_check['can_sell_xch']:
            # Create sell offer (offer XCH, request USD)
            sell_offer = self.wallet.create_offer(
                offer_amount=offers['size'],
                offer_wallet_id=1,  # XCH wallet
                request_amount=offers['ask'] * offers['size'],
                request_wallet_id=2  # USD wallet
            )

            if sell_offer:
                self.dexie.post_offer(sell_offer)

        logger.info("Market making cycle completed")

    def start(self):
        """Start the bot"""
        self.running = True
        logger.info("Market maker bot started")

        while self.running:
            try:
                self.run_cycle()
                time.sleep(self.update_interval)
            except KeyboardInterrupt:
                logger.info("Received shutdown signal")
                self.stop()
            except Exception as e:
                logger.error(f"Error in main loop: {e}")
                time.sleep(60)  # Wait before retrying

    def stop(self):
        """Stop the bot"""
        self.running = False
        logger.info("Market maker bot stopped")

# Main entry point
if __name__ == "__main__":
    bot = MarketMakerBot()
    bot.start()
```

### Configuration File

```python
# config.py
from decimal import Decimal

# Strategy parameters
SPREAD_PERCENTAGE = Decimal('0.03')  # 3%
POSITION_SIZE_XCH = Decimal('1.0')   # 1 XCH per offer
UPDATE_INTERVAL_SECONDS = 300         # 5 minutes

# Risk limits
MAX_XCH_POSITION = Decimal('100')
MAX_USD_POSITION = Decimal('5000')

# Wallet IDs
XCH_WALLET_ID = 1
USDC_WALLET_ID = 2  # Update with actual CAT wallet ID

# Fees
TRANSACTION_FEE_XCH = Decimal('0.0001')

# API endpoints
COINGECKO_API = "https://api.coingecko.com/api/v3"
DEXIE_API = "https://api.dexie.space/v1"  # Verify actual endpoint

# Monitoring
ENABLE_ALERTS = True
ALERT_EMAIL = "your-email@example.com"
```

---

## DEX Integration

### Dexie Integration

```python
class DexieIntegration:
    """Complete Dexie DEX integration"""

    def __init__(self, api_key: Optional[str] = None):
        self.base_url = "https://api.dexie.space"
        self.api_key = api_key

    def post_offer(self, offer_data: str) -> Dict:
        """Post offer to Dexie"""
        endpoint = f"{self.base_url}/v1/offers"

        headers = {
            'Content-Type': 'application/json'
        }

        if self.api_key:
            headers['Authorization'] = f'Bearer {self.api_key}'

        payload = {
            'offer': offer_data,
            'compression': 'none'
        }

        response = requests.post(
            endpoint,
            json=payload,
            headers=headers
        )

        return response.json()

    def get_offers(self, pair: str = "XCH-USDC") -> List[Dict]:
        """Get current offers for a pair"""
        endpoint = f"{self.base_url}/v1/offers"

        params = {
            'pair': pair,
            'status': 'active'
        }

        response = requests.get(endpoint, params=params)
        return response.json().get('offers', [])

    def cancel_offer(self, offer_id: str) -> bool:
        """Cancel an offer on Dexie"""
        endpoint = f"{self.base_url}/v1/offers/{offer_id}"

        response = requests.delete(endpoint)
        return response.status_code == 200
```

---

## Price Feed Integration

### Multiple Price Sources

```python
class EnhancedPriceFeed:
    """Aggregate prices from multiple sources"""

    def __init__(self):
        self.sources = {
            'coingecko': self.get_coingecko_price,
            'gate_io': self.get_gateio_price,
            'dexie': self.get_dexie_price
        }

    def get_coingecko_price(self) -> Optional[Decimal]:
        """Get price from CoinGecko"""
        try:
            response = requests.get(
                'https://api.coingecko.com/api/v3/simple/price',
                params={'ids': 'chia', 'vs_currencies': 'usd'},
                timeout=10
            )
            return Decimal(str(response.json()['chia']['usd']))
        except:
            return None

    def get_gateio_price(self) -> Optional[Decimal]:
        """Get price from Gate.io"""
        try:
            response = requests.get(
                'https://api.gateio.ws/api/v4/spot/tickers',
                params={'currency_pair': 'XCH_USDT'},
                timeout=10
            )
            return Decimal(str(response.json()[0]['last']))
        except:
            return None

    def get_dexie_price(self) -> Optional[Decimal]:
        """Get price from Dexie offers"""
        # Calculate mid-price from best bid/ask on Dexie
        # Implementation depends on Dexie API
        return None

    def get_aggregate_price(self) -> Optional[Decimal]:
        """Get median price from all sources"""
        prices = []

        for name, func in self.sources.items():
            price = func()
            if price:
                prices.append(price)
                logger.info(f"{name}: ${price}")

        if not prices:
            return None

        # Return median
        prices.sort()
        mid = len(prices) // 2

        if len(prices) % 2 == 0:
            return (prices[mid-1] + prices[mid]) / 2
        else:
            return prices[mid]
```

---

## Deployment and Operations

### Systemd Service (Linux)

```ini
# /etc/systemd/system/chia-market-maker.service

[Unit]
Description=Chia Market Maker Bot
After=network.target

[Service]
Type=simple
User=chia
WorkingDirectory=/home/chia/market-maker
ExecStart=/usr/bin/python3 /home/chia/market-maker/bot.py
Restart=always
RestartSec=30
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

```bash
# Enable and start service
sudo systemctl enable chia-market-maker
sudo systemctl start chia-market-maker

# Check status
sudo systemctl status chia-market-maker

# View logs
sudo journalctl -u chia-market-maker -f
```

### Docker Deployment

> **Note**: The Dockerfile below is illustrative pseudocode showing the general structure. Adapt it to your actual project layout (e.g., replace `requirements.txt` with your real dependency file and `bot.py` with your actual entrypoint).

```dockerfile
# Dockerfile (illustrative — adapt to your project layout)
FROM python:3.10-slim

# Install system dependencies
RUN apt-get update && apt-get install -y \
    git \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Install Chia
RUN pip install chia-blockchain

# Copy bot code
WORKDIR /app
COPY . /app

# Install Python dependencies (replace with your actual dependency file)
# RUN pip install -r requirements.txt

# Run bot (replace with your actual entrypoint)
CMD ["python", "bot.py"]
```

```yaml
# docker-compose.yml
version: '3.8'

services:
  market-maker:
    build: .
    container_name: chia-market-maker
    restart: unless-stopped
    volumes:
      - ./config:/app/config
      - ./logs:/app/logs
      - ~/.chia:/root/.chia  # Mount Chia wallet data
    environment:
      - CHIA_ROOT=/root/.chia
    networks:
      - chia-network

networks:
  chia-network:
    driver: bridge
```

### Monitoring Setup

```python
# monitoring.py
import smtplib
from email.message import EmailMessage

class AlertSystem:
    """Send alerts for important events"""

    def __init__(self, smtp_config: Dict):
        self.smtp_config = smtp_config

    def send_alert(self, subject: str, message: str):
        """Send email alert"""
        try:
            msg = EmailMessage()
            msg['Subject'] = f"[MM Bot] {subject}"
            msg['From'] = self.smtp_config['from']
            msg['To'] = self.smtp_config['to']
            msg.set_content(message)

            with smtplib.SMTP(
                self.smtp_config['server'],
                self.smtp_config['port']
            ) as server:
                server.starttls()
                server.login(
                    self.smtp_config['username'],
                    self.smtp_config['password']
                )
                server.send_message(msg)

        except Exception as e:
            logger.error(f"Failed to send alert: {e}")
```

---

## Security Best Practices

### Wallet Security

1. **Use Separate Wallets**:
   - Hot wallet: Small trading amount
   - Cold wallet: Bulk of funds (offline)

2. **Backup Mnemonics**:
   - Store 24-word seed phrase offline
   - Multiple physical locations
   - Never store digitally

3. **Key Management**:
   ```python
   # Never hardcode keys in code
   # Use environment variables
   import os
   from dotenv import load_dotenv

   load_dotenv()
   WALLET_FINGERPRINT = os.getenv('CHIA_WALLET_FINGERPRINT')
   ```

### API Security

```python
# Use environment variables for API keys
API_KEY = os.getenv('DEXIE_API_KEY')

# Implement rate limiting
from functools import wraps
import time

def rate_limit(max_per_minute):
    min_interval = 60.0 / max_per_minute
    def decorate(func):
        last_called = [0.0]
        @wraps(func)
        def rate_limited(*args, **kwargs):
            elapsed = time.time() - last_called[0]
            left_to_wait = min_interval - elapsed
            if left_to_wait > 0:
                time.sleep(left_to_wait)
            ret = func(*args, **kwargs)
            last_called[0] = time.time()
            return ret
        return rate_limited
    return decorate
```

### Operational Security

1. **Access Control**:
   - Use SSH keys (not passwords)
   - Implement firewall rules
   - Regular security updates

2. **Monitoring**:
   - Log all transactions
   - Alert on unusual activity
   - Regular audits

3. **Disaster Recovery**:
   - Regular backups
   - Tested recovery procedures
   - Documentation

---

## Testing

### Unit Tests

```python
import unittest

class TestMarketMaking(unittest.TestCase):

    def setUp(self):
        self.strategy = MarketMakingStrategy()
        self.price_feed = PriceFeed()

    def test_calculate_offers(self):
        """Test offer calculation"""
        market_price = Decimal('50.00')
        offers = self.strategy.calculate_offers(market_price)

        self.assertLess(offers['bid'], market_price)
        self.assertGreater(offers['ask'], market_price)
        self.assertGreater(offers['ask'], offers['bid'])

    def test_risk_limits(self):
        """Test risk management"""
        risk_mgr = RiskManager()

        result = risk_mgr.check_position_limits(
            Decimal('50'),
            Decimal('2500')
        )

        self.assertTrue(result['can_buy_xch'])
        self.assertTrue(result['can_sell_xch'])

if __name__ == '__main__':
    unittest.main()
```

### Integration Testing

```python
# Test on testnet first
class TestnetBot(MarketMakerBot):
    """Bot configured for Chia testnet"""

    def __init__(self):
        super().__init__()
        # Use testnet configuration
        # Test with fake money first!
```

---

## Performance Optimization

### Database for State Management

```python
import sqlite3

class StateManager:
    """Persist bot state to database"""

    def __init__(self, db_path: str = "bot_state.db"):
        self.conn = sqlite3.connect(db_path)
        self.create_tables()

    def create_tables(self):
        self.conn.execute('''
            CREATE TABLE IF NOT EXISTS offers (
                id INTEGER PRIMARY KEY,
                offer_id TEXT UNIQUE,
                created_at TIMESTAMP,
                side TEXT,
                price REAL,
                amount REAL,
                status TEXT
            )
        ''')

        self.conn.execute('''
            CREATE TABLE IF NOT EXISTS trades (
                id INTEGER PRIMARY KEY,
                executed_at TIMESTAMP,
                side TEXT,
                price REAL,
                amount REAL,
                profit REAL
            )
        ''')

        self.conn.commit()
```

### Async Operations

```python
import asyncio
import aiohttp

class AsyncPriceFeed:
    """Async price fetching"""

    async def fetch_price(self, url: str) -> Optional[Decimal]:
        async with aiohttp.ClientSession() as session:
            async with session.get(url) as response:
                data = await response.json()
                return self.parse_price(data)

    async def get_all_prices(self):
        """Fetch all prices concurrently"""
        tasks = [
            self.fetch_price(url)
            for url in self.price_sources
        ]
        return await asyncio.gather(*tasks)
```

---

## Next Steps

This technical guide provides the foundation for building a Chia market maker. The next document covers:
- Risk management in detail
- Advanced optimization techniques
- Operational runbooks
- Troubleshooting guide
