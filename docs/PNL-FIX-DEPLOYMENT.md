# Deploying the P&L overhaul (v0.8.0, 2026-07-30)

The engine currently running was built **before** these fixes, so nothing
changes until it is restarted with the new binary. The GUI-side fixes
(Reports page crash, USD conversions) take effect as soon as the GUI is
restarted, independently of the engine.

Nothing here has been done for you — the bot is live and stopping it is your
call.

## 1. Take a database backup first

The new engine adds a table (`inventory_state`) and starts writing real
values into columns that were previously empty. Back up before the first
run. Use the SQLite backup API, **not** a file copy — a plain copy misses the
`-wal` contents:

```bash
C:\GitHub\XOPTrader\.venv\Scripts\python.exe -c "import sqlite3,datetime; s=sqlite3.connect(r'file:C:/GitHub/XOPTrader/data/xop_trader.db?mode=ro',uri=True); d=sqlite3.connect(r'C:\GitHub\XOPTrader\data\xop_trader.db.pre_v080_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S')+'.bak'); s.backup(d); d.close(); s.close(); print('backup done')"
```

## 2. Stop the engine

Use the GUI's stop control (preferred — it cancels outstanding offers
cleanly). The running process is PID 8452 from
`cpp\build\Release\xop_trader.exe`. The exe file is **locked while it runs**,
which is why the new build went to `ReleaseFix` instead.

## 3. Swap in the new binary

```bash
copy /Y C:\GitHub\XOPTrader\cpp\build\ReleaseFix\xop_trader.exe C:\GitHub\XOPTrader\cpp\build\Release\xop_trader.exe
```

The old binary is preserved at `cpp\build\Release\xop_trader_backup.exe` if
you need to roll back; `ReleaseFix\` keeps the new one either way.

## 4. Start the engine and check the log

Expect these lines in `logs/xop_trader.log`:

| Log line | Meaning |
|----------|---------|
| `XOPTrader v0.8.0 starting` | The new binary is actually running |
| `PnLTracker: rehydrated N pairs from trade_log (fills=774 ...)` | Cumulative P&L no longer resets to zero |
| `Restored 0 inventory cost-basis records` | **Expected on the first start only** — the table is new. Every later restart should show a non-zero count |
| `Step 11: upgraded sentinel cost basis for xch to USD-pseudo ...` | Wallet-seeded holdings marked at current market price |
| `Step 11: reconciled ... against wallet` | Per-asset deposit/withdrawal reconcile (only if quantities drifted) |
| `Ledger: posted N opening balance legs (genesis)` | Double-entry ledger established from wallet balances — **first start only** |
| `Ledger invariant breach: asset=... divergence=...` | The books and the wallet disagree beyond tolerance — see below |

You should **no longer** see `[Position] Overflow in cost basis -- addition
rejected for asset=xch` — that line appeared 20 times on 2026-07-29 and meant
State was rejecting every XCH buy. Its absence confirms the second overflow
fix is active.

Two lines are informational rather than problems:

- `no USD valuation available for <pair> -- applying fill to quantity only`:
  a fill arrived before market data was warm. The basis is deliberately left
  untouched and repaired on a later heartbeat; it is not lost.
- `PnLTracker::insert_trade: trade_id=... already recorded`: a fill was
  re-detected after an interrupted run. It is counted once.

## 5. Verify P&L is live

- **Dashboard / status bar** should now show real dollar amounts instead of a
  permanent `$0.00`.
- **Reports page** should render (it has thrown `UnboundLocalError` on every
  refresh since 2026-04-21 and shown only placeholders).
- **`data/trade_history/trades_live.csv`** gains a row per fill from now on.
- New Prometheus gauge `xop_pnl_usd` is the display-safe total.

## What this does and does not fix

**Going forward:** realized P&L is measured against a real, persisted cost
basis, survives restarts, and is reported in USD with correct per-pair units.

**Historically:** it cannot be reconstructed. Cost basis was never recorded
correctly, so `trade_log.realized_pnl_mojos` is ~all zeros for the 774
existing fills (only 14 non-zero, summing to about −$2.28). The first restart
establishes basis at *current market price* ("mark at first observation"),
not at true acquisition cost, because that cost is unknowable from the data.

### Historical P&L cannot be derived from `trade_log` at all

An earlier version of this document said to use
`scripts/compute_actual_pnl.py` and quoted roughly **+$629**. **That is
wrong — do not use it.** The problem is not the method; it is the data.
`trade_log` is not a faithful record of what happened on-chain.

Wallet balances observed directly in the engine logs:

| | 2026-04-24 | 2026-07-30 |
|---|---|---|
| XCH | 61.685 | 122.957 |
| wUSDC.b | 5.744 | 9.985 |
| BYC | 167.593 | 27.847 |
| DBX | 7,325.983 | 775.285 |

What `trade_log` claims happened between those dates, versus what the wallet
actually did:

| asset | trade_log claims | wallet actually did | mismatch |
|---|---|---|---|
| XCH | −604.33 | **+61.27** | −665.60 |
| wUSDC.b | +1,560.90 | +4.24 | +1,556.66 |
| DBX | +7,824.24 | **−6,550.70** | +14,374.94 |

The recorded fills claim a ~604 XCH outflow across a period in which the XCH
balance *rose*, and ~1,561 wUSDC.b of sale proceeds that never arrived. For
DBX even the sign is inverted. Reconstructing "profit" from those rows is how
+$629 appeared for a wallet worth ~$218 — and taken literally the same rows
imply impossible negative opening balances (−1,731 wUSDC.b, −8,311 DBX).

**Ground truth for that window**, each date valued at its own prices:

| | value |
|---|---|
| 2026-04-24 portfolio | $446.24 |
| 2026-07-30 portfolio | $217.75 |
| April basket re-valued at July prices (buy-and-hold) | $365.92 |
| Actual vs simply holding | **−$148.17** |

Much of the drop is market movement (XCH fell ~45%, $2.50 → $1.37); the
−$148 is the part not explained by holding. All of this is **before**
deposits and withdrawals, which nothing in the system tracks — the +61 XCH
looks like a deposit, and if so the shortfall is worse.

Until fill recording is verified against the wallet, the only defensible P&L
is **wallet-balance deltas plus explicit deposit/withdrawal accounting**.

## The ledger is now the instrument for the open question below

A double-entry ledger (`ledger_entries`) and a reconciliation control ship in
this build. Full rationale in [ACCOUNTING-POLICY.md](ACCOUNTING-POLICY.md);
what you need operationally:

- On first start it posts **opening balances** from the wallet's confirmed
  balances. It deliberately does *not* replay `trade_log` — that would import
  the corruption it exists to detect.
- Every fill then posts balanced legs (base, quote, fee) using the bot's own
  belief about the fill.
- Each heartbeat it compares `SUM(ledger legs)` per asset against the wallet's
  confirmed balance, and alerts when they diverge beyond tolerance for two
  consecutive same-signed checks.

**It is alert-only by default** (`accounting.pause_enabled: false`). Taker
fills from the arbitrage/drift steps and external deposits still have no
ledger event, so auto-pausing would halt trading on legitimate activity. Leave
it off until the log runs clean.

**What to watch for.** If the phantom-fill bug is still live, you should see
`Ledger invariant breach` lines where the divergence is a near-exact multiple
of 1 XCH (the offer size) — the ledger will be *short* relative to the wallet,
because the books recorded a sale the wallet never made. That number tells you
directly how much phantom volume is being recorded and how often, which is the
measurement the diagnosis below has been missing.

## Open question this fix does NOT answer

The fixes above make cost basis and P&L *arithmetic* correct going forward.
They do **not** explain why `trade_log` disagrees with the wallet by ~665 XCH.
That is a separate defect in fill *detection*, upstream of everything fixed
here, and it must be resolved before any P&L number can be trusted.

Leading hypotheses, in rough order of likelihood:

1. **Cancelled or expired offers being recorded as fills.** `offer_log` has
   12,053 cancelled vs 725 filled offers, and 123 `trade_log` rows map to
   offers whose final status is `cancelled`. `detect_fills` treats wallet
   trade status `CONFIRMED` as a completed fill without verifying settled
   amounts (`offer_manager.cpp` ~606).
2. **Posted size assumed to be settled size.** A `Fill` copies the offer's
   originally posted price and size; nothing reads what actually settled, so
   a partially-taken or re-priced offer records at full size.
3. **Double counting across restarts.** Offers restored from `offer_log` on
   startup can be re-detected; `trade_id` uniqueness prevents exact
   duplicates but not re-posted offers with new ids.

A cheap decisive test, once the engine is restarted: for one trading day,
compare `data/trade_history/trades_live.csv` against the wallet's own
`get_all_offers(include_completed=true)` records and the actual balance
deltas over the same day. Any pair that disagrees identifies the mechanism.

## Known gaps (not addressed here)

- DBX liquidity-reward income has no ingestion path (`PnLTracker::record_fee`
  still has no callers), so reward inflows are invisible to P&L.
- The reorg confirmation gate is delay-only; it does not re-query wallet
  status before promoting a buffered fill.
- Cancelled-offer fees (~32× the filled-offer fees) live only in `offer_log`
  and are not part of the P&L fee leg.
- Snapshot rollups (`snapshots_1m/15m/1h/1d`) still contain the old, wrong
  historical P&L closes; recomputation would not fix chart history.
