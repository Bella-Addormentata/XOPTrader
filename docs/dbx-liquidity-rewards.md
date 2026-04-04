# DBX Liquidity Rewards — Dexie Incentive Program

## Overview

[Dexie's Liquidity Incentive Program](https://dexie.space/incentives) rewards
market makers with **DBX** (dexie bucks) for maintaining qualifying offers
within 5% of the current market price. XOPTrader automatically claims these
rewards on every offer submission.

## How It Works

1. **Snapshots**: Dexie takes order-book snapshots at random ~5-minute intervals.
2. **Points**: Each qualifying offer earns points based on its size and proximity
   to the market price. Closer = more points.
3. **Distribution**: The daily reward pool (e.g. 100 DBX/day per side) is shared
   proportionally among all qualifying offers in that pair.

### XOPTrader Qualification

XOPTrader's offers automatically qualify because:
- Our ladder tiers (0.35%–3% from mid) fall well within the 5% spread limit.
- Individual offers stay below the 25-coin limit.
- We create simple (non-combined/non-aggregated) offers.

## Current Reward Rates (as of writing)

| Pair           | Side          | Rate          | APR   |
|----------------|---------------|---------------|-------|
| wUSDC.b → XCH  | bid (buy XCH) | 100 DBX/day   | 38%   |
| XCH → wUSDC.b  | ask (sell XCH)| 100 DBX/day   | 11%   |
| wUSDC → XCH    | bid (buy XCH) | 100 DBX/day   | 22%   |
| XCH → wUSDC    | ask (sell XCH)| 100 DBX/day   | 19%   |
| DBX → XCH      | bid (buy XCH) | 100 DBX/day   | 135%  |
| XCH → DBX      | ask (sell XCH)| 100 DBX/day   | 75%   |
| SBX → XCH      | bid           | 100 DBX/day   | 83%   |
| XCH → SBX      | ask           | 100 DBX/day   | 66%   |

*Rates subject to change by Dexie at any time.*

## Configuration

### Enabling Auto-Claim (default: ON)

In `config.yaml` under the `dexie` section:

```yaml
dexie:
  api_base: "https://api.dexie.space/v1"
  max_requests_per_10s: 50
  claim_rewards: true    # Set to false to disable auto-claiming
```

Or toggle via the GUI: **Settings → Connection → Dexie API → Auto-claim DBX
liquidity rewards**.

When enabled, every offer submitted to Dexie includes `claim_rewards: true` in
the API payload. Dexie batches and sends rewards daily to the maker wallet
address.

### Claiming Frequency & Dust

Rewards with `claim_rewards: true` are batched daily, which avoids excessive
dust. If you disable auto-claim, you can still claim manually via:

- **Web**: [dexie.space/upload](https://dexie.space/upload) → "Your Offers"
- **Python tool**: [github.com/dexie-space/dexie-rewards](https://github.com/dexie-space/dexie-rewards) (CLI, uses Chia Wallet RPC)
- **API**: `POST /v1/rewards/check` and `POST /v1/rewards/claim`

Unclaimed rewards expire **6 months** after the last accrual date.

## XCH/DBX Market

The XCH/DBX pair offers the highest APR (75–135%) but has a **very thin order
book**, meaning:

- **High inventory risk**: You may accumulate DBX that is hard to sell.
- **Wide effective spreads**: Few counterparties to fill your offers.
- **High reward-per-offer**: Less competition = larger share of the daily pool.

An XCH/DBX pair is included in `config.example.yaml` as **disabled** by
default. Enable it only if you have DBX inventory to seed both sides, and are
comfortable with the illiquidity risk.

```yaml
  - base_asset_id: "xch"
    quote_asset_id: "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"
    name: "XCH/DBX"
    enabled: false   # Enable to farm DBX rewards on this pair
```

**DBX asset ID**: `db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20`

## Eligibility Constraints

Offers are **not eligible** for rewards if they:
- Are combined/aggregated offers
- Exceed 25 involved coins
- Request or offer multiple assets in a single offer

XOPTrader creates simple single-pair offers, so these constraints do not apply
under normal operation.
