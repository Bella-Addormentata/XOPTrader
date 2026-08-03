# Era remediation, 2026-08: April-June missing-trade history

Status as of **2026-08-02**: code fixes are live; the approved history
backfill has **NOT** been applied yet.  The completeness sweep (below) is
the acceptance gate -- it currently FAILs with 1,401 unrecorded CONFIRMED
wallet trades.  Do not consider this era closed until the sweep PASSes.

## What happened

Two independent omission mechanisms corrupted April-June 2026 trade history.
Both are fixed in code; the historical rows they dropped are still absent:

* **A. Maker fills misfiled as cancels** (phantom-removal race, fixed
  `cae2bfd`).  The forensic investigation confirmed 472 CONFIRMED maker
  fills inside its acid-test window (2026-04-24 onward) absent from
  `trade_log` (verdict text says 473; the verified trade_id set and its
  monthly breakdown Apr 17 / May 252 / Jun 130 / Jul 73 sum to 472).
  These are additional to the 37 rows backfilled on 2026-07-31 (zero
  trade_id overlap; those 37 carry `realized_pnl_mojos = NULL`).
* **B. Taker trades recorded nowhere** (`is_my_offer = false` paths, fixed
  `62ee4cb` / `record_taker_fill`).  579 CONFIRMED taker trades in the same
  window, pre-genesis.

Acid test: claimed history + A + B lands within 1.3-5.4% of wallet actuals
on all four assets.

The full-history sweep additionally finds trades **before** the forensic
window (same mechanisms, earlier era): 169 maker fills (2026-04-05 ..
2026-04-23) and 179 taker trades (2026-04-04 .. 2026-04-24).

## Unexplained residuals (document, do not force)

After crediting A + B, the remaining wallet-vs-history residuals are:

| Asset    | Residual |
|----------|----------|
| DBX      | -778     |
| wUSDC.b  | +56      |
| BYC      | +0.6     |
| XCH      | -11      |

These stay documented here; no adjustment entries are posted for them.

## Ledger constraint (verified 2026-08-02)

The ledger (`ledger_entries`) opened at genesis **2026-07-30T18:01:42Z /
height 9080132** with opening balances equal to actual wallet holdings.
Every A/B trade except two (see below) settled before genesis, so their
flows are already embedded in the opening balances.

`Database::ledger_balances()` (cpp/src/database.cpp) computes
`SELECT asset_id, SUM(delta_mojos) FROM ledger_entries GROUP BY asset_id`
with **no date or height filter**, and `step_check_ledger_invariant`
(cpp/src/engine.cpp) compares that total against live wallet balances every
cycle.  Therefore:

* **NO** new `ledger_entries` rows may be posted for pre-genesis trades, and
* **NO** nonzero-delta adjustment entries dated in the pre-genesis era --

either would double-count and put the invariant into permanent breach.
Backfills go to `trade_log` / `taker_fills` only, with
`realized_pnl_mojos = NULL` so PnLTracker rehydration and the GUI Total/24h
P&L (which filter `realized_pnl_mojos IS NOT NULL`) are undisturbed.

**Exception -- two post-genesis taker trades** (confirmed after genesis,
before the taker-recording fix was live; their flows are NOT in the opening
balances, so recording them would also require ledger treatment -- owner
decision needed):

```
0xe214945d98a7f2a6d30f7d768f3f77da152cd759e179852da3d7c6a379cf0d42  2026-07-30T20:23:01Z  h=9080575  2 XCH -> 2.587 BYC
0x0be54305477c6e19143ae3fa2ddd2e758884133816363e9ccb598262f28dd241  2026-07-30T20:46:45Z  h=9080645  2 XCH -> 2.511 BYC
```

## The completeness sweep: scripts/verify_fill_completeness.py

The single invariant that catches both mechanisms forever:

> Every wallet trade record that reaches CONFIRMED (both `is_my_offer`
> values) must exist in `trade_log` or `taker_fills`.

The script is strictly read-only (wallet RPC + read-only DB URI), prints
PASS/FAIL with every missing trade_id listed, and exits nonzero on FAIL:

```
.venv/Scripts/python.exe scripts/verify_fill_completeness.py                    # full history
.venv/Scripts/python.exe scripts/verify_fill_completeness.py --since-days 7     # weekly check
.venv/Scripts/python.exe scripts/verify_fill_completeness.py --since-height N   # from a block height
```

Requires the Chia wallet running and synced (localhost:9256).  A 10-minute
grace window (`--grace-minutes`) excludes just-confirmed trades the engine
has not persisted yet.  Exit codes: 0 PASS, 1 FAIL, 2 operational error.

### Sweep result 2026-08-02 (full history) -- FAIL

```
wallet trade records: 14374    CONFIRMED total: 2360
checked maker (mine): 1598     checked taker (theirs): 762
MISSING maker fills:  641      MISSING taker trades:   760
FAIL: 1401 CONFIRMED wallet trades are unrecorded (641 maker, 760 taker).
```

Reconciliation against the forensic verdict:

| Component                                   | Makers | Takers |
|---------------------------------------------|-------:|-------:|
| Forensic window (Apr 24 .. genesis)         |    472 |    579 |
| Pre-window early era (Apr 4 .. Apr 24)      |    169 |    179 |
| Post-genesis (ledger-relevant, see above)   |      0 |      2 |
| **Total missing**                           | **641**| **760**|

All fills recorded since the fixes went live (August rows in `trade_log`,
live rows in `taker_fills`) pass the sweep -- the missing set is entirely
the pre-fix era -- with one open observation: a `--since-days 2` run at
~2026-08-02T23:58Z flagged maker fill
`0x3bfc26449d10e8c375f02b4555ed8d80261e109f5768869608265c5b2bdfee94`
(accepted 2026-08-02T23:45:06Z, h=9095001, 1 XCH -> 1.738 wUSDC.b) as
unrecorded more than 10 minutes after acceptance.  This is either engine
recording lag longer than the sweep's grace window or a live straggler --
re-run `--since-days 1` and check `trade_log` for it before drawing
conclusions.

### Schedule

Run it **manually, weekly** (`--since-days 8` once the era backfill lands;
full-history until then), and after any engine change touching fill
recording.  `scripts/scheduled_db_maintenance.py` was considered as a host:
it has a natural hook (its `_run_cycle` already shells out to
`maintain_snapshot_rollups.py`, so a second subprocess call is a small
change), but it is deliberately wallet-independent -- it must keep taking
backups when the wallet is down, and a sweep failure would be conflated
with backup failure in its status file.  Keep the sweep manual until the
era is closed; if automation is wanted later, add an optional
`--run-fill-sweep` flag to `_run_cycle` that treats exit code 2 (wallet
unreachable) as a skip, not an error.

## Execution record (2026-08-02, user-approved)

Applied by `scratchpad/backfill_era.py` (session scratchpad; dry-run gated).
Backup: `data/backups/xop_trader_pre_era_backfill_20260802.db`.

Inserted: **472** maker fills -> trade_log (`realized_pnl_mojos NULL`),
**579** taker trades -> taker_fills (`strategy='historical_backfill'`),
**472** correction events -> offer_closure_events. No ledger writes
(all pre-genesis; flows already in opening balances).

End-to-end acid test at apply time (height window [8632223, 9080132)):

| asset | claimed | +makers | +takers | =combined | actual | residual |
|---|---|---|---|---|---|---|
| XCH | -626.33 | -486.95 | +1175.16 | +61.88 | +61.27 | **+0.61** |
| wUSDC.b | +1564.47 | +889.04 | -2505.30 | -51.79 | +4.24 | -56.03 |
| BYC | -186.24 | +124.96 | -60.72 | -122.01 | -139.75 | +17.74 |
| DBX | +7824.24 | +5939.37 | -19536.43 | -5772.82 | -6550.70 | +777.88 |

Residuals documented, not forced: wUSDC.b ~-56 (unattributed inflow, likely
bridge/reward), BYC ~+18 (unattributed outflow, 1.2% of gross), DBX ~+778
(non-trade flows incl. pre-e14a9c1 reward claims). Note the forensic
verdict's own acid table was internally inconsistent by ~9 XCH; every
inserted row is individually a wallet-CONFIRMED record.

Post-apply sweep: 1,401 missing -> **350**, exactly the expected exclusions
(169 + 179 pre-window Apr 4-24, before the first wallet anchor; 2
post-genesis takers needing ledger entries). The validated era is CLOSED.
