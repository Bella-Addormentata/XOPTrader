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

## `inventory_state` — persisted cost-basis records (2026-07-30)

Persists the `InventoryTracker` per-asset accounting so realized-P&L
attribution survives engine restarts (the missing piece behind "P&L never
worked": every restart re-seeded all assets at a sentinel basis, so almost
every sell recorded `realized_pnl_mojos = 0`).

| Column                   | Type    | Notes                                              |
|--------------------------|---------|----------------------------------------------------|
| `asset_id`               | TEXT PK | `xch` or 64-hex CAT id                             |
| `total_quantity`         | INTEGER | Tracked holdings in mojos                          |
| `total_cost`             | REAL    | Σ(price × qty) in **USD-normalized pseudo-units × mojos** — exceeds int64 range by design |
| `basis_is_seed_sentinel` | INTEGER | 1 when the basis is synthetic (wallet seed)        |
| `updated_at`             | TEXT    |                                                    |

Writers: `Database::save_inventory_state` (engine, after every fill / seed /
reconcile).  Reader: `Database::load_inventory_state` (engine startup,
before wallet seeding).

> ℹ️  Cost-basis units changed on 2026-07-30: the `InventoryTracker` now
> stores basis in **USD-normalized pseudo-units** (USD-per-base-unit ×
> 1e12), converted per pair via `Engine::quote_usd_factor` (wUSDC*/USDS/BYC
> = $1/unit, DBX cross-derived).  This fixes the cross-currency blending of
> one shared XCH basis across wUSDC.b-, BYC- and DBX-quoted pairs.
> `trade_log.cost_basis_mojos` continues to store the **pair's own quote
> pseudo-units** (converted on write) so per-row `(price − basis)` stays
> dimensionally sound.

---

## `ledger_entries` — double-entry accounting ledger (2026-07-30)

Every event that changes what the bot believes it holds posts a **balanced
set of legs**. `SUM(delta_mojos) GROUP BY asset_id` is the ledger's implied
balance, which the reconciliation control ties to the wallet's *confirmed*
balance each heartbeat.

| Column         | Type    | Notes                                              |
|----------------|---------|----------------------------------------------------|
| `id`           | INTEGER | PK autoinc                                         |
| `entry_time`   | TEXT    | ISO-8601 UTC of the event                          |
| `event_type`   | TEXT    | `opening`, `fill`, `fee`, `take`, `adjust`         |
| `event_id`     | TEXT    | trade_id, or `genesis:<asset>`                     |
| `leg`          | TEXT    | `base`, `quote`, `fee`, `opening`, `adjust`        |
| `asset_id`     | TEXT    | Canonical (`xch` or 64-hex) — never a display symbol |
| `delta_mojos`  | INTEGER | Signed: **+ inflow, − outflow**                    |
| `pair_name`    | TEXT    | Context                                            |
| `block_height` | INTEGER | Settlement block, 0 if n/a                         |
| `note`         | TEXT    | Free-form provenance                               |
| `created_at`   | TEXT    |                                                    |

`UNIQUE(event_id, leg, asset_id)` is the idempotency key — a fill re-detected
after a crash re-posts identical legs, which are ignored rather than doubled.

Legs per event:

| Event | Legs |
|---|---|
| `opening` | one per asset = wallet confirmed balance at genesis |
| `fill` (bid) | base `+size`, quote `−quote_mojos`, `xch −fee` |
| `fill` (ask) | base `−size`, quote `+quote_mojos`, `xch −fee` |

**Append-only.** Never `UPDATE` or `DELETE`; corrections are new `adjust`
legs. Genesis deliberately does **not** replay `trade_log` — that table
disagrees with the wallet by ~665 XCH, and replaying it would import the
corruption the ledger exists to detect.

Writer: `Database::append_ledger_entries` (engine, on genesis and every fill).
Reader: `Database::ledger_balances` (the invariant control).

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

## Durable trade-history files (`data/trade_history/`, 2026-07-30)

Plain-text mirrors of `trade_log` so the record survives engine restarts,
GUI restarts, reboots, and loss of the SQLite file.  SQLite remains
authoritative; these are append-only / regenerable mirrors.  (`data/` is
gitignored, so this section is the version-controlled spec.)

| File | Writer | Contents |
|------|--------|----------|
| `trades_live.csv` | Engine, one row per fill at settlement (`PnLTracker::append_history_csv`) | Fills from engine builds ≥ 2026-07-30 |
| `trades_full.csv` | `scripts/export_trade_history.py` (manual, idempotent) | Every `trade_log` row, all eras |

Shared column layout:

```
timestamp_utc, trade_id, pair, side,
price_pseudo_mojos, size_base_mojos,
price_quote_per_base, size_base_units, quote_amount,
fee_xch_mojos, cost_basis_pseudo_mojos,
realized_pnl_quote_mojos, realized_pnl_usd, block_height
```

- `price_pseudo_mojos` — raw stored price.  Rows before 2026-04-14 use the
  legacy encoding (quote mojos per base unit, ~2000–3000); later rows use
  pseudo-units (`quote_units_per_base × 1e12`).  The display columns already
  account for both; the exporter auto-detects via a 1e9 threshold.
- `fee_xch_mojos` — XCH mojos.  In `trades_full.csv` this is backfilled from
  `offer_log` because `trade_log.fee_mojos` was 0 from June 2026 until the
  2026-07-30 fee fix.
- `realized_pnl_quote_mojos` — the pair's QUOTE-asset mojos; `0` means a buy
  **or** a sell whose cost basis was unknown at fill time.
- `realized_pnl_usd` — USD for USD-pegged quotes; blank for DBX.

> ⚠️  `realized_pnl_mojos` is ~all zeros before 2026-07-30 (cost basis was
> lost on every restart and overflowed for XCH pairs).  For truthful
> historical P&L use `scripts/compute_actual_pnl.py` (cash-flow method),
> which needs only prices and sizes.

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
