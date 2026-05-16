# XOPTrader Database Schema

> **Source of truth:** `cpp/src/database.cpp` (DDL constants `kCreate*`).
> This file is documentation; the C++ DDL is authoritative.  Whenever a
> table is added or altered there, update this file in the same commit.

The engine uses a single SQLite database (path configured via
`secrets.yaml::database.path`, default `data/xop_trader.db`).  All monetary
values are stored as `INTEGER` mojos to avoid floating-point drift.
1 XCH = 10^12 mojos.  CAT tokens (wUSDC.b, BYC, DBX, USDS) use 10^3 mojos
per display unit.

> ⚠️  PnL values written to `trade_log.realized_pnl_mojos` and
> `snapshots.pnl_total_mojos` are in **quote-asset mojos** for the relevant
> pair, not raw XCH mojos.  Conversion to a single USD figure must apply
> the per-pair `quote_mojos_per_unit` (see
> `gui/services/database_service.py::fetch_reports::pnl_usdc_expr`).
> The 1e9-inflation bug fixed in v0.7.45 was caused by missing this
> per-pair factor; the canonical formula now lives in
> `xop::quote_mojos_for()` (`cpp/include/xop/types.hpp`).

---

## `trade_log` — confirmed fills

| Column                | Type    | Notes                                                |
|-----------------------|---------|------------------------------------------------------|
| `id`                  | INTEGER | PK autoinc                                           |
| `timestamp`           | TEXT    | ISO-8601 UTC of detection                            |
| `trade_id`            | TEXT    | UNIQUE; equals the offer_id of the filled offer      |
| `pair_name`           | TEXT    | e.g. `XCH/wUSDC.b`                                   |
| `side`                | TEXT    | `bid` or `ask` (CHECK)                               |
| `price_mojos`         | INTEGER | Engine pseudo-units: real_price * `kMojosPerXch`     |
| `size_mojos`          | INTEGER | Base-asset mojos                                     |
| `fee_mojos`           | INTEGER | On-chain fee paid (best-effort: offer-creation fee)  |
| `cost_basis_mojos`    | INTEGER | Weighted-avg cost basis at time of sell (mojos)      |
| `realized_pnl_mojos`  | INTEGER | **QUOTE-asset mojos**, 0 for buys                    |
| `block_height`        | INTEGER | Settlement block                                     |
| `offer_hash`          | TEXT    | Optional spend-bundle hash                           |
| `acquisition_ts`      | TEXT    | When the position being sold was acquired (ISO)      |
| `created_at`          | TEXT    | DB row insert time                                   |

Writers:
- `PnLTracker::record_fill` — single canonical writer
  (`cpp/src/monitoring/pnl.cpp`).

---

## `offer_log` — every offer ever posted

| Column                      | Type    | Notes                                          |
|-----------------------------|---------|------------------------------------------------|
| `id`                        | INTEGER | PK autoinc                                     |
| `offer_id`                  | TEXT    | UNIQUE; the wallet-assigned offer hash         |
| `pair_name`                 | TEXT    |                                                |
| `side`                      | TEXT    | `bid` / `ask`                                  |
| `price_mojos`               | INTEGER | Pseudo-units (* `kMojosPerXch`)                |
| `size_mojos`                | INTEGER | Base-asset mojos                               |
| `tier`                      | INTEGER | 0 = innermost                                  |
| `competitiveness_score`     | INTEGER | 0–10 from `score_offer_competitiveness`        |
| `queue_ahead_mojos`         | INTEGER |                                                |
| `queue_ahead_score`         | INTEGER | 0–10 from `score_queue_position`               |
| `execution_quality_score`   | INTEGER | Composite                                      |
| `status`                    | TEXT    | `pending`, `filled`, `cancelled`, `expired`    |
| `created_block`             | INTEGER |                                                |
| `resolved_block`            | INTEGER | Block when status left `pending`               |
| `fee_mojos`                 | INTEGER | Fee paid to create the offer                   |
| `cancel_reason`             | TEXT    | Free-form; e.g. `price_adverse`, `stuck`       |
| `book_best_bid`             | INTEGER | DEX top-of-book at creation                    |
| `book_best_ask`             | INTEGER |                                                |
| `created_at` / `resolved_at`| TEXT    |                                                |

Status terminal values: `filled`, `cancelled`, `expired` (see
`is_terminal_status` in `database.cpp`).

---

## `offer_closure_events` — append-only audit of every status change

Inserted by `Database::update_offer_status`.  Used by the post-mortem and
reorg-defence tooling.

| Column           | Type    | Notes                                          |
|------------------|---------|------------------------------------------------|
| `id`             | INTEGER | PK autoinc                                     |
| `offer_id`       | TEXT    |                                                |
| `pair_name`      | TEXT    |                                                |
| `event_type`     | TEXT    | e.g. `closed`, `stuck`, `reconcile`            |
| `previous_status`| TEXT    |                                                |
| `observed_status`| TEXT    |                                                |
| `closure_reason` | TEXT    | Mirrors `offer_log.cancel_reason` when set     |
| `resolved_block` | INTEGER |                                                |
| `created_at`     | TEXT    |                                                |

Stuck cancels (offers cancelled because they got wedged in the wallet) are
already emitted here via `update_offer_status(..., "stuck")`.

---

## `snapshots` — periodic engine state snapshots (one row per pair per block)

| Column              | Type    | Notes                                            |
|---------------------|---------|--------------------------------------------------|
| `id`                | INTEGER | PK autoinc                                       |
| `block_height`      | INTEGER |                                                  |
| `pair_name`         | TEXT    |                                                  |
| `mid_price_mojos`   | INTEGER | Engine pseudo-units                              |
| `spread_bps`        | REAL    |                                                  |
| `inventory_ratio`   | REAL    | 0.5 = balanced                                   |
| `sigma_block`       | REAL    | Per-block Yang-Zhang vol                         |
| `regime`            | TEXT    | `Random`, `Momentum`, `MeanReverting`, etc.      |
| `pnl_total_mojos`   | INTEGER | Quote-asset mojos                                |
| `xch_usd_rate`      | REAL    | XCH spot price in USD at snapshot time           |
| `pnl_total_usd`     | REAL    | Convenience derived value                        |
| `created_at`        | TEXT    |                                                  |

---

## `snapshots_1m` / `snapshots_15m` / `snapshots_1h` / `snapshots_1d` — long-horizon chart rollups

Built by `scripts/maintain_snapshot_rollups.py` for fast long-range chart
queries and bounded database growth.

| Column                  | Type    | Notes                                            |
|-------------------------|---------|--------------------------------------------------|
| `pair_name`             | TEXT    |                                                  |
| `bucket_start_unix`     | INTEGER | UTC bucket start (seconds since epoch)           |
| `bucket_start_iso`      | TEXT    | UTC ISO-8601 bucket start                        |
| `open_mid_price_mojos`  | INTEGER | First mid in bucket                              |
| `high_mid_price_mojos`  | INTEGER | Max mid in bucket                                |
| `low_mid_price_mojos`   | INTEGER | Min mid in bucket                                |
| `close_mid_price_mojos` | INTEGER | Last mid in bucket                               |
| `avg_spread_bps`        | REAL    | Mean spread across samples                       |
| `avg_inventory_ratio`   | REAL    | Mean inventory ratio across samples              |
| `avg_sigma_block`       | REAL    | Mean per-block sigma across samples              |
| `close_regime`          | TEXT    | Last observed regime in bucket                   |
| `close_pnl_total_mojos` | INTEGER | Last PnL mark in quote-asset mojos               |
| `avg_xch_usd_rate`      | REAL    | Mean XCH/USD mark for bucket                     |
| `close_pnl_total_usd`   | REAL    | Last USD PnL mark in bucket                      |
| `sample_count`          | INTEGER | Number of raw `snapshots` rows in bucket         |
| `source_first_block`    | INTEGER | First block represented in bucket                |
| `source_last_block`     | INTEGER | Last block represented in bucket                 |
| `updated_at`            | TEXT    | Last rollup update timestamp                     |

Primary key: `(pair_name, bucket_start_unix)`.

---

## `strategy_quotes` — every tier the strategy proposed (pre-suppression)

| Column         | Type    | Notes                       |
|----------------|---------|-----------------------------|
| `id`           | INTEGER | PK autoinc                  |
| `block_height` | INTEGER |                             |
| `pair_name`    | TEXT    |                             |
| `tier`         | INTEGER |                             |
| `side`         | TEXT    | `bid` / `ask` (CHECK)       |
| `price_mojos`  | INTEGER | Pseudo-units                |
| `size_mojos`   | INTEGER |                             |
| `created_at`   | TEXT    |                             |

---

## `sanity_failures` — every tier rejected by Step-8 sanity guards

| Column                 | Type    | Notes                                          |
|------------------------|---------|------------------------------------------------|
| `id`                   | INTEGER | PK autoinc                                     |
| `block_height`         | INTEGER |                                                |
| `pair_name`            | TEXT    |                                                |
| `side`                 | TEXT    | `bid` / `ask`                                  |
| `tier`                 | INTEGER |                                                |
| `proposed_price_mojos` | INTEGER |                                                |
| `reference_price_mojos`| INTEGER | Best bid for asks, best ask for bids           |
| `deviation_pct`        | REAL    |                                                |
| `failure_reason`       | TEXT    | e.g. `competitiveness_too_low`, `crossed_book` |
| `details`              | TEXT    | JSON-ish blob                                  |
| `created_at`           | TEXT    |                                                |

---

## Relationships

```
offer_log.offer_id  ─── 1:1 ─── trade_log.trade_id  (when status='filled')
offer_log.offer_id  ─── 1:N ─── offer_closure_events.offer_id
snapshots, strategy_quotes, sanity_failures: keyed by (block_height, pair_name)
```

## Suggested indexes

See `kIndex*` constants in `cpp/src/database.cpp`. Key ones:

- `idx_trade_log_timestamp` — drives PnL fetch range queries
- `idx_trade_log_pair` — per-pair PnL rollups
- `idx_offer_log_status` — pending-offer reconciliation
- `idx_offer_closure_events_offer_id` — post-mortem lookups
