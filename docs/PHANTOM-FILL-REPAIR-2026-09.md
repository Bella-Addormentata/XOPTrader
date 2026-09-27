# Phantom-fill repair, 2026-09: trade_log 1900-1902

Status as of **2026-09-26**: the repair is designed and reviewed, and its
scripts were hardened and re-verified in three passes on copies of the 14:52
snapshot of the live database. It has **NOT** been applied. The operator
applies it in the v0.10.26 install window, with the engine and the GUI
stopped, **before v0.10.26 starts for the first time**. v0.10.26's first boot
may post XCH or BYC auto-adjusts. Those would change the pre-state the repair
asserts, and the repair would then refuse to run. The execution record at the
end stays blank until the repair is applied.

**v0.10.25 must never run on the repaired database** (step g says why and what
to do instead).

Policy: `docs/ACCOUNTING-POLICY.md` §2 (the `reversal` event type, the
correction of an auto-adjust, and the treatment of a fill that never
happened). This branch carries that amendment. Code fix that stops a
recurrence: PR #171, released in v0.10.26 (CHANGELOG, "A fill is booked only
when the chain shows the offer was taken").

## What happened

On 2026-09-22 the engine booked three fills for offers nobody took:

| trade_log | offer | pair, side | offer_log | booked (UTC) |
|---|---|---|---|---|
| 1900 | `0xd6a8325c15af4fdb2674061afdd613ffb6c240be5fb0ef23aab6ddf31262d0d6` | XCH/DBX ask, 1 XCH at 102.976 DBX | 21185 | 2026-09-22 22:09:29 |
| 1901 | `0xdb63709cb9b2c3794cbf1a57bb15f2d5c1830dca620b16ac36926d0d60c6c556` | XCH/BYC bid, 1.103203898833 XCH for 1.864 BYC | 21311 | 2026-09-22 22:09:29 |
| 1902 | `0x83eef9df80a4d5557422c4e6b1789c8d7504e7e83759a47a818c103b8099511c` | XCH/DBX ask, 1 XCH at 100.212 DBX | 21243 | 2026-09-22 22:13:39 |

In each case one XCH maker coin had been spent by an unrelated transaction of
the bot's own, which paid a 15,000,000-mojo fee with it. After the 2026-09-22
wallet resyncs the wallet called the three trades CONFIRMED, and detect_fills
(v0.10.25 and earlier) booked every CONFIRMED offer as a fill. Each booking
wrote:

- a trade_log row;
- three ledger legs (base, quote, fee), ids 2622-2630;
- offer_log 'filled', which cleared the cancel cause;
- a status_update closure event, cancel_pending -> filled (33533-33535);
- the inventory tracker;
- a line in `data/trade_history/trades_live.csv`.

All three offers had already had a `price_adverse` cancel submitted (closure
events 33049, 33293 and 33157).

Three hours later, at 2026-09-23T01:17:52Z (block 9,330,325), the ledger
invariant control posted auto-adjust **2641: DBX -199,588**, labelled
"unexplained divergence reconciled to wallet". It absorbed the phantom DBX
quote legs (+203,188). No XCH or BYC adjust has been posted since the phantoms.
The last XCH adjust is id 1755 (09-03) and the last BYC adjust is id 2379
(09-19). So the phantom XCH and BYC legs are still in those ledger balances.

## Evidence

**On-chain verdicts.** A take spends every maker coin of the offer in one
block, and that block holds a settlement coin for exactly the amount offered.
None of the three offers meets this test.

| trade_log | spend that killed it | the other maker coins | wallet status (2026-09-26) |
|---|---|---|---|
| 1900 `0xd6a8325c15` | block 9,324,680 (created at 9,324,678), a fee transaction of the bot's own | unspent | CONFIRMED |
| 1901 `0xdb63709cb9` | block 9,325,694 (created at 9,325,693), a fee transaction of the bot's own | unspent | CONFIRMED |
| 1902 `0x83eef9df80` | block 9,325,004 (created at 9,325,003), a fee transaction of the bot's own | the second maker coin was spent later, at 9,339,728, into a single change coin (less a 15,000,000 fee). The coins behind trade 1903's offer were spent in the same block but come from two other coins: the spends share a block, not a lineage (re-traced on-chain 2026-09-27) | CANCELLED |

Maker coins spent at different heights, or some left unspent, mean the offer
died without being taken. v0.10.26 records exactly this case as `cancelled`
with the reason `dead_on_chain` at the height of the first spend. The heights
in the table are also the wallet's `confirmed_at_index`, which is already
stored in `offer_log.resolved_block`. Sources: CHANGELOG v0.10.26 for the
spend heights and the unspent coins, and Dexie shows all three cancelled;
read-only chain and wallet checks on 2026-09-26 for 1902's second coin and for
the wallet statuses.

**Trade 1903 is real and is not touched.** Trade 1903 is
`0x476a29ba40d986a9d4fc3d4574c1ff3c7755ea587a74d0906690530967c8fec2`, an
XCH/DBX ask of 1 XCH at 85.361 DBX, booked 2026-09-25 at block 9,340,539. It
has the same cancel_pending -> filled shape as the phantoms. A `ttl_expired`
cancel was submitted at 9,340,536, three blocks earlier. A read-only chain
check on 2026-09-26 (`prove_fill_on_chain.py`) found:

- both of its maker coins were spent together at block 9,340,539;
- that block holds the settlement coin for exactly the 1 XCH offered.

That is v0.10.26's proof of a take. 1903 stays as booked. Its cost basis
inherited a contamination of about 0.3% from phantom 1901 (see Residuals).
This is accepted and documented, not corrected.

## What the repair changes

The whole repair is one `BEGIN IMMEDIATE` transaction, so it applies completely
or not at all. The script refuses unless the database is in the exact
pre-repair state; the only tolerance is engine `status_observation` /
`reconcile_observation` events on the still-'filled' rows. It verifies every
post-condition inside the transaction before COMMIT. On a database already in
the post-repair state it prints `ALREADY APPLIED` and exits 0 without changing
anything; there, and only there, it also tolerates the engine events v0.10.26
may append afterwards (an S14 reopen and a re-close of the reopened row, in an
order the engine can write them). It makes **22 row changes**:

| Store | Change | Rows |
|---|---|---:|
| `offer_closure_events` | Appends 2 events per offer. (1) The verdict: `status_update` filled -> cancelled, `closure_reason` exactly `dead_on_chain`, at the spend height, fee NULL. Its event type, reason and height are v0.10.26's verdict for a dead offer, but its `previous_status` is 'filled', a shape the engine itself never writes (see below). (2) `phantom_fill_correction` filled -> cancelled. It records why, the restored cancel cause, the reversal event id, and **the full deleted trade_log row as JSON**. | +6 |
| `ledger_entries` | Appends 9 `reversal` legs, event_id `reversal:<offer_id>`, one per leg 2622-2630, each with the same leg, asset, pair and block and the delta negated. Appends 1 `adjust` leg, event_id `correction:adjust:<DBX>:9330325`, +203,188 DBX. | +10 |
| `offer_log` | 21185, 21243, 21311: status 'filled' -> 'cancelled'; cancel_reason '' -> `price_adverse(1.648%)`, `price_adverse(2.247%)`, `price_adverse(3.507%)`. Nothing else changes: `resolved_block` already holds the spend height, and `resolved_at` is kept (see below). | 3 updated |
| `trade_log` | Deletes 1900, 1901 and 1902, which are archived in the events above. `sqlite_sequence` stays at 1903, so no id is reused. | -3 |

**The verdict row is a new shape.** On a row whose status is already 'filled',
the engine's `update_offer_status` never writes a `status_update`: it appends
a `status_observation` or `reconcile_observation` and leaves the row alone. The
14:52 snapshot has no `status_update` whose `previous_status` is 'filled'. So
`event_type = 'status_update' AND previous_status = 'filled'` identifies the
repair's three verdict rows, and only them.

**`resolved_at` is kept, which is an exception to the engine's rule.** It holds
the time the engine wrote the wrong 'filled' verdict (2026-09-22 22:09:29 and
22:13:39), 21 to 27 hours after the dead spend, not the time the offer left the
book. v0.10.26's own dead verdict would stamp the time it is written, which
here would be the repair time. The repair changes only `status` and
`cancel_reason`, by design; policy §2 records the exception. Offer-lifetime
analytics that read `resolved_at` are wrong for these three rows either way
(see Residuals).

**Which rows carry the tag.** The ten ledger rows share one `entry_time`, the
repair time. 13 of the 16 appended rows end with the tag
`phantom-fill repair of trade_log 1900-1902 (follow-up to PR #171, v0.10.26)`:
the 10 ledger rows in `note`, and the 3 `phantom_fill_correction` events in
`closure_reason`. The 3 verdict events carry no tag: their `closure_reason` is
exactly `dead_on_chain`, as the engine writes it, and the `previous_status`
above marks them instead. These three queries find all 16:

```sql
-- 10: the reversal legs and the DBX correction
SELECT * FROM ledger_entries
 WHERE note LIKE '%phantom-fill repair of trade_log 1900-1902%';
-- 3: the correction events (each archives one deleted trade_log row)
SELECT * FROM offer_closure_events
 WHERE event_type = 'phantom_fill_correction';
-- 3: the verdict events (no tag)
SELECT * FROM offer_closure_events
 WHERE event_type = 'status_update' AND previous_status = 'filled'
   AND closure_reason = 'dead_on_chain';
```

On a repaired copy of the snapshot they return 10, 3 and 3 rows; on the
snapshot itself, 0, 0 and 0.

### Ledger change per asset

| Asset | Reversal legs | Correction | Net change | On the 14:52 snapshot |
|---|---|---:|---:|---|
| XCH | +1,000,000,000,000 (2622), -1,103,203,898,833 (2625), +1,000,000,000,000 (2628), +15,000,000 x 3 fees (2624, 2627, 2630) | | **+896,841,101,167** | 41,611,757,193,532 -> 42,508,598,294,699 |
| DBX | -102,976 (2623), -100,212 (2629) | +203,188 | **0** | 1,936,793 -> 1,936,793 |
| BYC | +1,864 (2626) | | **+1,864** | 75,505 -> 77,369 |

The live values at repair time will differ from the snapshot by whatever the
engine has posted since 14:52: rewards, takes and fills. The apply script
prints the live before and after sums, and the execution record keeps them.

- **XCH and BYC move by the whole phantom amount.** No auto-adjust absorbed
  their phantom legs, so reversing the legs does not count anything twice.
  The pre-state check refuses to run if any XCH, DBX or BYC adjust has been
  posted after id 2630 other than 2641. That is the S40 / `ef05578` trap.
- **The fee legs are reversed.** An offer's creation fee is paid only at
  settlement (policy §8), and these offers never settled. The three
  transactions that killed them really did pay 15,000,000 mojos each. Those
  transactions were the bot's own, and their fees are recorded nowhere in the
  ledger, the same family as the known cancel-fee gap (§8). After the repair
  they are among the unrecorded flows the XCH invariant will see. See "First
  start".
- **DBX does not move: the correction of adjust 2641.**
  - Adjust 2641 (-199,588) had already re-tied DBX to the wallet, and in doing
    so absorbed the phantom +203,188.
  - Reversing the DBX legs alone would put the DBX ledger 203,188 below the
    wallet and set off a second auto-adjust.
  - The correction leg backs the absorbed amount out: -199,588 + 203,188 =
    **+3,600**. That is the part of 2641 that is still unexplained: the wallet
    held 3,600 DBX mojos more than the ledger would have without the phantoms.
    The gap is below the ~9.7k (0.5%) alert band, so it would not have tripped
    the control on its own.
  - `SUM(adjust)` for DBX, the "unaccounted for" measure, goes from -834,877
    to -631,689 on the snapshot. For every other asset it is unchanged.
  - Row 2641 itself is not edited.
- **Why `reversal`, not `adjust`.** An `adjust` reversal would add 0.897 XCH of
  a known booking error to the "unaccounted for" measure. TODO S44(b) asks for
  the opposite.

### P&L

The engine rebuilds P&L from COUNT and SUM over every trade_log row at each
start. There is no status column, and nothing else persists P&L, so deleting
the three rows is what removes them from P&L. On the snapshot:

- XCH/DBX loses 2 fills, **44,876 DBX mojos of realized P&L** and 30,000,000
  XCH mojos of fees: (197, 84,125, 104,791,842) -> (195, 39,249, 74,791,842).
- XCH/BYC loses 1 fill and 15,000,000 XCH mojos of fees; its realized P&L is
  unchanged (the bid was a buy).
- The newest XCH/BYC trade becomes 1884. The GUI's last-price fallback reads
  1.885 again instead of the phantom 1.6896.

`offer_log` status counts on the snapshot: filled 1345 -> 1342, cancelled
20602 -> 20605. The pending and cancel_pending rows, which the engine restores
into State, are unchanged.

## What the repair does not change

- **Ledger rows 2622-2630 and 2641.** They stay. The ledger is append-only,
  and the original legs keep the UNIQUE key that stops a re-post.
- **Closure events 33049, 33157, 33293 and 33533-33535.** They stay. The S14
  cancel-submit-height query still returns 9,324,693, 9,325,015 and 9,325,705.
- **`inventory_state`.** Step 11 re-ties quantities to the wallet at every
  boot. The phantom lots' cost cannot be reconstructed, because unknown cost
  is never fabricated (§6).
- **trade_log 1903, `taker_fills`, `snapshots` and the rollups, every row of
  every other offer, and the schema.**
- **Anything outside SQLite**, including `data/trade_history/*.csv` and the
  cancel-intent files (runbook step c handles those).

## Residuals (document, do not force)

| Residual | Size | Treatment |
|---|---|---|
| XCH cost basis in `inventory_state` | **estimate: about +0.3-0.4% high** (about +$0.2). Phantom 1901 bought 1.103 XCH at about $1.69 against a basis of about $1.51, and Step 11 then re-added the missing XCH at the mark. Trade 1900's basis to trade 1902's stepped +0.329%, but that step also mixes in DBX/USD drift, so the figure is an estimate. | Accepted. It dilutes as the position turns over. |
| DBX cost basis in `inventory_state` | **estimate: about 1%** | Accepted. |
| Trade 1903's frozen `cost_basis_mojos` (79,237,807,461,772) and `realized_pnl_mojos` (6,123) | Computed on the contaminated XCH basis. **Estimate:** the basis is about 0.3% high, so realized P&L is understated by roughly 240-260 DBX mojos (about 0.25 DBX). | Accepted. P&L rehydrates the stored value. |
| DBX adjust 2641 | +3,600 of it is still unexplained after the correction. The split rests on one wallet observation; if it is wrong, only the attribution inside `SUM(adjust)` is off, by at most 3,600 mojos. | Documented. It stays in `SUM(adjust)`. |
| Snapshot P&L history | `snapshots.pnl_total_usd` rows from 09-22 22:09 to the repair carry about +$0.87 of phantom P&L. The GUI's realized series is rebuilt from trade_log and loses it retroactively. So between those times total minus realized is inflated by about $0.87, and the total curve steps down at the restart. | Not rewritten. Snapshots are history that the engine never reads back. |
| `data/trade_history/trades_live.csv` | Keeps the three phantom lines. The file is append-only and nothing reads it back. | Not edited: the lines are recorded in `db/schema.md` instead. `trades_full.csv` is regenerated at a later stop (Follow-ups). |
| `offer_log.resolved_at` for the three rows | Holds the phantom booking time (09-22 22:09:29 / 22:13:39), not the time the offer died, which was 1-2 blocks after creation on 09-21 / 09-22. | Kept. This is the documented exception in policy §2: the repair changes only status and cancel_reason. Offer-lifetime analytics on these rows are wrong by about a day. |
| Taker fills 631 and 636 (not phantoms of this kind; found in the 2026-09-27 pre-flight) | Each took a Dexie offer that the next fill (632, 637) took again. The wallet reports 631 FAILED and 636 CANCELLED, but their ledger `take` legs stand: XCH +1,057,670,000,000 net (base +476,400,000,000 and +581,300,000,000, fees 2 x -15,000,000) and BYC -2,421. After the repair, BYC ledger minus wallet is exactly -2,421, and about 1.058 XCH of the XCH gap is these two takes. | Not repaired here. The first v0.10.26 boot's XCH and BYC auto-adjusts will absorb them. Record the breakdown at h. A later reversal of 631 and 636 must then back out that part of those adjusts with `correction:` legs, as 2641 is here, or it double-counts. |
| The trade_id journal-first guard for these ids | Gone with the trade_log rows. | Harmless under v0.10.26, which books only on proof; these offers can only prove Dead. **Not harmless under v0.10.25**: a phantom it re-adopted could be journaled again, while its ledger legs are dropped by `INSERT OR IGNORE` (the original legs hold the key), leaving a trade_log row with no ledger legs. One reason v0.10.25 must never run on the repaired database. |

## First start after the repair (v0.10.26)

1. **P&L drops by the phantom amounts.** Lifetime realized P&L on XCH/DBX is
   44,876 DBX mojos lower, fees are 45,000,000 XCH mojos lower, and there are
   3 fewer fills. The GUI trade table and filled-offer list no longer show the
   three.
2. **Expect an XCH 'unexplained' auto-adjust.**
   - The repair raises the XCH ledger by 0.897 XCH. The phantom legs had told
     the ledger that 0.897 XCH left the wallet when it did not, and that hid
     part of an existing gap. The repair uncovers that gap; it does not create
     it.
   - Two estimates from the snapshot put the post-repair XCH ledger **+2.55 to
     +3.55 XCH above** the wallet's confirmed balance. They differ by take 641
     (+1 XCH, 09-26), which posts ledger legs but no tracker legs (S48). Cancel
     fees paid since widen the gap further.
   - Once the book is thin enough for the tolerance to be exceeded twice, the
     control alerts and posts an auto-adjust of **up to about -3.5 XCH**. The
     tolerance is about 0.2 XCH plus the exposure of live offers.
   - The live config has `pause_enabled: false` and `auto_adjust_enabled:
     true`, so the control does not pause. This runbook leaves both as they
     are: let the adjust post, and check it.
   - **3 x 15,000,000 mojos of that adjust are known**: the fees the three
     killing transactions paid.
   - **Check it against the numbers, not the estimate.** In step d, record the
     wallet's confirmed XCH. In step e, the apply script prints the
     post-repair XCH ledger sum (the `ledger xch` line). Wallet minus ledger is
     the adjust to expect, plus whatever moves in between (cancel fees, takes,
     rewards). In step h, compare the posted amount with it.
3. **BYC may auto-adjust too.** The repair adds 1,864 mojos to the BYC ledger.
   The estimated BYC gap, ledger below wallet, shrinks from about -4,300 to
   about -2,400 mojos. That is still more than the ~400-mojo band, so an
   adjust of about +2,400 BYC mojos is possible. **This is an estimate**;
   compare the wallet's confirmed BYC (step d) with the `ledger ae1536f5` line
   (step e) in the same way.
4. **DBX: no adjust from the repair.** The DBX ledger balance does not change.
5. **Nothing happens to the three offers**, normally.
   - 'cancelled' rows are not restored into State, and the startup wallet scan
     ignores CONFIRMED and CANCELLED records.
   - Only two things can bring one back: an entry in a cancel-intent file
     (step c), or the wallet reporting PENDING_CANCEL (step d). In chia 2.7.4
     a cancel marks every trade that shares one of its coins, CONFIRMED ones
     included, which is why step a answers **Keep**.
   - If one does come back, v0.10.26's S14 step reopens the row as
     cancel_pending (a `reopen_observation` event), and a later heartbeat
     closes it again with a `status_update` cancel_pending -> cancelled:
     `dead_on_chain` once the fill proof finds it Dead, or `wallet reported
     terminal` once the wallet flips it to CANCELLED. It books no fill: its
     maker coins were spent at different heights. The after-boot checker
     accepts that sequence.
6. **1903 is unaffected.** Before 1903 was proven real, a first-boot adjust
   could have absorbed its legs if it had been a phantom. That concern is
   closed.
7. **The plain checker is a pre-start tool.** It refuses the live database
   while an XOPTrader process runs, and its exact row-count checks fail by
   design once the engine has written anything. After the start, run it with
   `--after-boot` (step h), which allows for the engine's later rows.

## Pinned artefacts

Run **only** the scripts in `scripts/oneoff/phantom_fill_repair_2026_09/` of
this branch (the worktree `C:\GitHub\XOPTrader-phantomfix`, or any checkout of
it). The drafts the first pass wrote in a session Temp scratchpad are
superseded. **Never run anything from the live checkout
`C:\GitHub\XOPTrader` in the window.** (The trade-history export in Follow-ups
runs from there at a later stop, because it works nowhere else.)

**The pin is `SHA256SUMS` in that directory, and only that.** The apply script
checks it on every run and refuses the live database on any mismatch. Verify
the pins with `python apply_phantom_repair.py --print-hashes`, which reads no
database: each of its three lines must end `[matches SHA256SUMS]`, and each
`lf=` value must equal the table below. The hashes are of each file with CRLF
normalised to LF, so a git autocrlf checkout still matches; `Get-FileHash` on
a CRLF checkout reports a different (the `raw=`) value. Compare the `lf=`
values with this table, not only the `[matches SHA256SUMS]` tag: a stray
`--write-hashes` makes the tag match modified scripts, and the checker only
warns on a mismatch (Known edges).

No commit is pinned. A commit recorded in this document would itself move
HEAD, so HEAD could never equal it. The script prints the checkout's HEAD on
its `git :` line for information only, and the execution record keeps it after
the run.

| Artefact | Path | SHA256 |
|---|---|---|
| Apply script | `scripts/oneoff/phantom_fill_repair_2026_09/apply_phantom_repair.py` | `33194400b9b4feaabd32c50b4b01d07619be481ffdd4d8c2c32f5a84841418ac` (LF-normalised, as in `SHA256SUMS`) |
| Checker | `scripts/oneoff/phantom_fill_repair_2026_09/check_phantom_repair.py` | `b097a98cc95311163bf7e09bb9240d3c90c9346008f8b50d86116baa1443385c` (LF-normalised, as in `SHA256SUMS`) |
| Chain proof | `scripts/oneoff/phantom_fill_repair_2026_09/prove_fill_on_chain.py` | `faf230850c59b433309436382798626964cfd0097850cd0554988f966277353a` (LF-normalised, as in `SHA256SUMS`) |
| v0.10.26 installer | `xop_trader-installer-windows-x64-v0.10.26.exe`, from the GitHub release v0.10.26 | `ee5e15e4ae8fe0d385dd4f7115e2dcf3112e0cf21c38d98f193217858ce85abf` (the published sha256; plain `Get-FileHash`) |

The installer's file name follows release.yml's naming pattern, and its sha256
is the value published with the release. Neither could be re-checked offline
when this runbook was written: confirm both on the release page. If
`Get-FileHash` disagrees in the session setup, stop there (nothing has been
touched), download the installer again, and go on only once the file, the
release page and this table agree.

**Where this runbook is stricter than, or corrects, the apply script's
docstring.** The docstring is hash-pinned, so it was not edited for these
points. Follow this document:

- Its RUNBOOK step 9 allows relaunching v0.10.25 after a failed install when
  no phantom reads PENDING_CANCEL. This runbook never runs v0.10.25 on the
  repaired database (step g).
- Its step 8 offers running the first boot with `auto_adjust_enabled: false`.
  This runbook does not change the config (First start, item 2).
- Its step 1 leaves the GUI-close answer to the operator. This runbook answers
  **Keep** (step a).
- Its step 6 says "Exit 1, 2 or 3: nothing was written to the database; fix
  the cause and re-run". This runbook never retries a STATE MISMATCH (exit 2)
  and stops on a pin mismatch (exit 1); see step e.
- Its step 9 says "Run the installer with Launch checked". This runbook allows
  unchecking Launch (step g.2); either way f must have passed first.
- Its NOTES call the sweep's `dead_on_chain` exclusion "planned" and say the
  unfixed sweep "will otherwise report these three". The exclusion is built on
  this branch, and only 1900 and 1901 are CONFIRMED in the wallet, so an
  unfixed sweep reports two (Follow-ups).
- Its NOTES say to annotate `data/trade_history/trades_live.csv`. This runbook
  leaves that file as it is and records the three lines in `db/schema.md`
  (Follow-ups).
- Its ROLLBACK names `--restore-from` as the only way back inside the window.
  This runbook also sanctions a manual backup-API restore of `$Backup` in one
  dead end: exit 5 with an unknown or differing read-back, when nothing else
  has written (step e, "Exit 5 and f fails").

Statements in the docstrings that do not match the code are listed under
Known edges of the pinned scripts.

## Operator runbook (install window)

**Before the window.**

- The operator must be at the PC. The installer's UAC prompt expires, and it
  has timed out twice before (Inno exit 2, no log).
- Allow no reboot and no log-off during the window. `XOPTrader.lnk` in the
  Startup folder relaunches the GUI at logon, and the GUI starts the engine. A
  stop with nobody to answer the prompt uses the config default, which is
  Cancel.
  - The shortcut is
    `C:\Users\dorkm\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup\XOPTrader.lnk`
    (`$env:APPDATA\...`), and it starts
    `C:\Program Files\XOPTrader\xop_trader_gui.exe`, whichever version is
    installed there. After COMMIT and before the install, that is v0.10.25,
    which must never run on the repaired database. The session setup moves the
    shortcut into `$Out`, and step g puts it back. The installer does not
    create or remove it.
  - Pause Windows Update for the window (Settings > Windows Update > Pause
    updates) and resume it after step h. If a restart is already pending
    there, it can still be forced: choose another time for the window, or
    restart first (close the GUI and answer Keep before you restart).
- Download the installer: `xop_trader-installer-windows-x64-v0.10.26.exe`,
  109,612,886 bytes, from the v0.10.26 GitHub release (GitHub's published
  asset digest equals the pin). Its hash is checked in the session setup
  below.
- Close every agent session and every Python tool that works on XOPTrader.
  An agent session kept open to help with the window must launch no Python of
  its own (workflows, monitors, log pollers) from step b through step f. Its
  scratch paths contain `C--GitHub-XOPTrader`, so one such process gives
  exit 3 at b or e, or exit 4 after COMMIT. The script's process check
  refuses on:
  - `xop_trader.exe` and the GUI executables (`xop_trader_gui.exe`, and the
    older names `xoptrader-gui.exe` and `xoptrader_gui.exe`);
  - anything running from `C:\Program Files\XOPTrader`;
  - any Python process (`python`, `pythonw`, `python3`, `python3.x`, `py`,
    `pyw`) whose command line contains `gui`, `xoptrader`, `xop_trader`,
    `scheduled_db_maintenance` or `maintain_snapshot_rollups`, or whose
    command line cannot be read. That includes every `C:\GitHub\XOPTrader*`
    worktree and Claude session paths such as `...\C--GitHub-XOPTrader\...`.

  Step b lists any that remain.
- `python` must be Python 3.11 or newer (the scripts were verified on 3.13.12).

**Session setup.** Open one PowerShell window and keep it for the whole window.

How output is kept:

- Every Python command runs as `cmd /c "python -u ... > <file> 2>&1"`. That
  writes the script's output **and** its error stream to a file in `$Out`:
  argparse errors, a Python traceback (for example when
  `prove_fill_on_chain.py` cannot reach the node), the apply's audit-log
  WARNING, and the sweep's `WARNING` and `ERROR` lines. `-u` keeps those lines
  in order. `Get-Content` then shows the file.
- Windows PowerShell 5.1's transcript does **not** record a native program's
  own output or its error stream, and `| Out-File` drops the error stream. The
  transcript records what you typed and what `Get-Content` showed, so the
  files in `$Out` are the record.
- `cmd` splits its command line at spaces. None of the paths used here
  contains one; keep `$Repo` and `$Out` that way.
- `$rc = $LASTEXITCODE` right after `cmd /c` is the script's exit code. After
  a Ctrl+C the line `"exit $rc"` can show the previous command's code: read
  the file instead (for the apply, its last line `exit     : N  <meaning>`,
  which is also the last line of the audit log).
- Do not use `Tee-Object`: in Windows PowerShell 5.1 it writes UTF-16 files.
- The apply script also writes its own audit log beside the backup.
- **When you repeat a step, give its capture a new name** (`b_list_blockers_2.txt`,
  `e_apply_2.txt`, `e_probe_dryrun_2.txt`, ...). Never overwrite a capture:
  the first run's error stream is in no other file. The same goes for
  backups: a repeated real run gets a new `$Backup` (the exit-3 row, or
  `$Backup = $Probe` in the exit-5 case), and the record lists every backup
  file the window produced.
- A few commands are shown without `cmd /c` capture because they are
  interactive or trivial: `& $Chia wallet show`, the `@"..."@ | python -`
  here-string queries, and a re-run of `--list-blockers` inside R1 or the
  manual restore. For those, copy what the console shows into the record.
- **Stopping before g** (any stop: a refusal, exit 2, a verdict other than
  Dead, or your own decision) always ends the same way. Put the shortcut back
  (`Move-Item "$Out\XOPTrader.lnk" $Lnk`), resume Windows Update, run
  `Stop-Transcript`, and only then start anything. v0.10.25 may start only on
  an **unrepaired** database: one no real run has committed to (the exit-code
  table says which exits committed), or one R1 has restored with exit 0. If
  the repair is in the database, g.4's rule holds: never v0.10.25; install
  v0.10.26, or roll back with R1 first.

```powershell
$Repo   = 'C:\GitHub\XOPTrader-phantomfix'          # this branch, NOT C:\GitHub\XOPTrader
$Dir    = "$Repo\scripts\oneoff\phantom_fill_repair_2026_09"
$Apply  = "$Dir\apply_phantom_repair.py"
$Check  = "$Dir\check_phantom_repair.py"
$LiveDb = 'C:\GitHub\XOPTrader\data\xop_trader.db'
$Stamp  = (Get-Date).ToUniversalTime().ToString('yyyyMMdd_HHmmss')
$Out    = "C:\GitHub\XOPTrader-backups\phantom-repair-$Stamp"
New-Item -ItemType Directory -Force $Out | Out-Null
$Backup = "$Out\xop_trader_pre_phantom_repair_$Stamp.db"
Start-Transcript -Path "$Out\transcript.txt"
Remove-Item Env:XOP_REPAIR_FAKE_LIVE_DIR -ErrorAction SilentlyContinue   # verification only

python --version
cmd /c "python -u $Apply --print-hashes > $Out\0_hashes.txt 2>&1"; $rc = $LASTEXITCODE
Get-Content "$Out\0_hashes.txt"; "exit $rc"
$Installer = '<path to>\xop_trader-installer-windows-x64-v0.10.26.exe'
(Get-FileHash $Installer -Algorithm SHA256).Hash
(Get-FileHash $Installer -Algorithm SHA256).Hash -eq 'ee5e15e4ae8fe0d385dd4f7115e2dcf3112e0cf21c38d98f193217858ce85abf'

# The Startup shortcut: out of the Startup folder until step g
$Lnk = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\XOPTrader.lnk"
if (Test-Path $Lnk) { Move-Item $Lnk "$Out\XOPTrader.lnk" }
Test-Path $Lnk; Test-Path "$Out\XOPTrader.lnk"
```

`--print-hashes` must exit 0 with three `[matches SHA256SUMS]` lines whose
`lf=` values equal the Pinned artefacts table, and the installer's hash must be
`ee5e15e4...5abf`: the comparison prints `True`. (`Get-FileHash` prints the
hash in capitals; `-eq` ignores case.) The two `Test-Path` lines must print
`False`, then `True`. **If anything differs, stop here**: nothing has been
touched yet, and the bot is still running. (If you stop here, put the shortcut
back: `Move-Item "$Out\XOPTrader.lnk" $Lnk`.)

`--dry-run` validates the `--backup-to` path but writes no backup, so the dry
run and the real run use the same `$Backup`. The script's own logs are
`$Backup.dryrun.log` for the dry run, `$Backup.log` for the real run, and
`$Backup.restore.log` for a `--restore-from`. The last line of each is
`exit     : <code>  <meaning>`, except after a failed write to the log itself
(the console still ends with it; Known edges item 6) or a Ctrl+C inside that
last write (item 3).

**a. Close the GUI deliberately, at the PC, and answer Keep.**

1. Close the XOPTrader window. Closing it is the graceful engine stop; the
   GUI does not respawn an exited engine.
2. The S74 prompt asks what the stop does with the resting offers. Click
   **Keep offers on the book**. Do not press Enter: the preselected button is
   the config default, which is Cancel.
   - Why not Cancel: "Cancel all offers" starts with the wallet-wide secure
     sweep. In chia 2.7.4 a cancel marks every trade that shares one of its
     coins PENDING_CANCEL, CONFIRMED trades included. 1900 and 1901 still hold
     unspent maker coins that the wallet can reuse in new offers, so a sweep
     can turn a phantom PENDING_CANCEL, which makes the next boot reopen its
     repaired row (First start, item 5). Cancel also pays fees and writes a
     cancel-intent file.
   - Keep sends nothing and writes no intent. The book rests unmanaged while
     the bot is down; an offer taken meanwhile is booked on the next start,
     after v0.10.26 proves the take.
3. Write the answer in the execution record. If Cancel was clicked, record it:
   a PENDING_CANCEL is then possible in step d, and step c is more likely to
   find an intent file.

**b. Confirm that nothing is left running, with the script's own check.
Never `taskkill` first.**

```powershell
cmd /c "python -u $Apply $LiveDb --list-blockers > $Out\b_list_blockers.txt 2>&1"; $rc = $LASTEXITCODE
Get-Content "$Out\b_list_blockers.txt"; "exit $rc"
```

`--list-blockers` is read-only: it opens no SQLite connection and writes no
file. It runs the live run's own guards in the live run's order (the hash
pins; the process list, with every process kind listed under "Before the
window"; then, only once no XOPTrader process runs, the cancel-intent files,
the path checks and a momentary no-share open of the database, -wal and -shm)
and prints a `BLOCKER` line for each problem. It does **not** check the flags,
the `--backup-to` path or the database's state, so the real run can still
refuse with 1 (for example, `$Backup` already exists) or stop with 2 (STATE
MISMATCH).

- **Exit 0**, ending `RESULT   : nothing would stop the live run (exit 0)`:
  go on.
- **Exit 3**: a process or an open handle. While the GUI runs it is listed as
  `xop_trader_gui.exe`, normally twice, and the engine as `xop_trader.exe`. A
  graceful stop can take a few minutes, so wait and run the check again.
  Close any agent session or Python tool it names. A `BLOCKER` for an open
  handle names only the file (the database, `-wal` or `-shm`), not the
  process. To find the holder, open Resource Monitor (`resmon`), go to the
  CPU tab, type `xop_trader.db` in the Associated Handles search box, and
  close what it lists (a SQLite browser, a backup agent, an antivirus scan).
  If the engine stays, read
  the tail of `C:\GitHub\XOPTrader\logs\xop_trader.log` to see why. Use
  `taskkill /F` only as a last resort. If you do, record it: a hard kill can
  leave offers and cancel intents behind, so run steps b and c again
  afterwards.
- **Exit 1**: a pin mismatch (stop: these are not the reviewed scripts), an
  intent file naming a phantom (step c), or a path alias.

**c. The cancel-intent files must not name the three ids.**

Step b's `intent :` line lists each file that exists, or says none do. To see
the lines themselves:

```powershell
foreach ($n in 'uncancelled.txt', 'uncancelled.json', 'uncancelled.txt.tmp') {
  $f = "C:\GitHub\XOPTrader\data\$n"
  if (Test-Path $f) { "$f EXISTS"; Select-String -Path $f -Pattern 'd6a8325c15','db63709cb9','83eef9df80' }
  else { "$f absent" }
}
```

- On 2026-09-26 none of the three existed. Keep writes none; a Cancel stop or
  a hard kill can.
- `uncancelled.txt` is one `<id>` or `<id> <tag>` per line. If a line names a
  phantom, open the file in Notepad and delete **only** that line; leave every
  other line exactly as it is. `uncancelled.json` is the legacy name, read only
  when the `.txt` file is absent: treat it the same way.
- `uncancelled.txt.tmp` is the engine's write-then-rename temporary. The
  engine never reads it, but a leftover one that names a phantom makes the
  script refuse: delete that file (the engine is stopped).
- The apply script refuses (exit 1) while any of the three files names a
  phantom. It finds the id with or without `0x`, in any letter case, in UTF-8
  or UTF-16.

**d. Note the wallet statuses and the confirmed balances.**

The engine is stopped; the Chia wallet and node keep running. Chia is
installed per user, so find `chia.exe` on PATH or under
`%LOCALAPPDATA%\Programs\Chia`.

```powershell
$Chia = (Get-Command chia -ErrorAction SilentlyContinue).Source
if (-not $Chia) { $Chia = "$env:LOCALAPPDATA\Programs\Chia\resources\app.asar.unpacked\daemon\chia.exe" }
foreach ($id in '0xd6a8325c15af4fdb2674061afdd613ffb6c240be5fb0ef23aab6ddf31262d0d6',
                '0xdb63709cb9b2c3794cbf1a57bb15f2d5c1830dca620b16ac36926d0d60c6c556',
                '0x83eef9df80a4d5557422c4e6b1789c8d7504e7e83759a47a818c103b8099511c') {
  cmd /c "python -u $Dir\prove_fill_on_chain.py $id >> $Out\d_offers.txt 2>&1"
}
Get-Content "$Out\d_offers.txt"
& $Chia wallet show
```

- `prove_fill_on_chain.py` is the pinned, read-only proof (wallet `get_offer`
  and full-node coin records; it never opens the database). Each run prints
  `wallet status: ...` and a `VERDICT:` line. Expected: **CONFIRMED,
  CONFIRMED, CANCELLED**, and `VERDICT: Dead` for all three. If it cannot
  reach the node, the wallet status line is still printed first, followed by
  a traceback (both land in `d_offers.txt`); the Chia GUI's Offers list shows
  the statuses too.
- `wallet show` is run without a redirect because it may ask for a key
  (add `-f <fingerprint>` if it does). Its output is not in the transcript
  (see Session setup): copy the numbers into the record by hand.
- **Any verdict other than `VERDICT: Dead` stops the window before e.** An
  `Unknown` (an unsynced node, a coin the node does not know) or a traceback
  proves nothing; a `Settled` means the design is wrong for that offer.
  Either way, do not run e: put the Startup shortcut back, resume Windows
  Update, and restart v0.10.25 (it is safe on the **unrepaired** database),
  or install v0.10.26 and re-plan (policy §2). Record the output.
- If any offer shows **PENDING_CANCEL**, the repair is still correct. Expect
  the first v0.10.26 boot to reopen that row and a later heartbeat to close it
  again (First start, item 5). Write that expectation into the record.
- From `wallet show`, record the **Total Balance** (the confirmed balance) of
  XCH, DBX and BYC in mojos. Check that **Pending Total Balance** equals it,
  meaning no coins are in flight. If it does not, read it again later.

**No fourth phantom.** The apply checks only the three offers, and the checker
compares against `$Backup`, so a fill that v0.10.25 booked wrongly after
2026-09-26 would pass both unseen. Offer_log 21449 (`0x0fab2a2ca4...`, an
XCH/DBX ask) has the phantoms' exact shape: it has been `cancel_pending` since
09-23, one maker coin was spent before its cancel was sent, and its other
maker coin is phantom 1900's unspent coin. It proves Dead on-chain. List what
has been booked since 1903, read-only, with the engine stopped (paste the block
as it stands; the closing `"@` must be at column 0):

```powershell
@"
import sqlite3, pathlib
con = sqlite3.connect(pathlib.Path(r'$LiveDb').as_uri() + '?mode=ro', uri=True)
print('trade_log after 1903:', con.execute("SELECT id, trade_id, pair_name, side, block_height FROM trade_log WHERE id > 1903 ORDER BY id").fetchall())
print('offer_log 21449     :', con.execute("SELECT status, cancel_reason FROM offer_log WHERE id = 21449").fetchall())
con.close()
"@ | python -
```

- Expected (as on 2026-09-27 05:16 UTC): `trade_log after 1903: []` and
  `offer_log 21449 : [('cancel_pending', 'price_adverse(1.218%)')]`.
- Prove every row listed after 1903:
  `cmd /c "python -u $Dir\prove_fill_on_chain.py <trade_id> >> $Out\d_new_rows.txt 2>&1"`.
  A `VERDICT: Settled` row is a real fill: the repair does not touch it, so
  record it and go on.
- **Stop before e** if 21449 reads `filled`, or if any row after 1903 is not
  `VERDICT: Settled`. That is a fourth phantom, which this repair does not
  cover. Stop as in Session setup ("Stopping before g"): the database is
  still unrepaired, so v0.10.25 may run. Then re-plan.
- The query leaves an empty `-wal` and a `-shm` beside the database. That is
  normal SQLite behaviour; its handle closes when the query exits.

**e. Dry run, then the real run.**

```powershell
cmd /c "python -u $Apply $LiveDb --i-stopped-the-engine --backup-to $Backup --dry-run > $Out\e_dryrun.txt 2>&1"; $rc = $LASTEXITCODE
Get-Content "$Out\e_dryrun.txt"; "exit $rc"
```

The output appears when the run ends (under a minute). The dry run's last two
lines must be `DRY RUN OK: 22 row changes verified, then rolled back.` and
`exit     : 0  OK`, and `"exit $rc"` prints `exit 0`. Its `backup :` line
says the backup was validated and NOT written. A
`note : tolerated later observation event ...` line is fine. `ALREADY
APPLIED` means someone applied the repair before: see "ALREADY APPLIED"
below the exit table, and do not run the real run. Then run the repair for
real:

```powershell
cmd /c "python -u $Apply $LiveDb --i-stopped-the-engine --backup-to $Backup > $Out\e_apply.txt 2>&1"; $rc = $LASTEXITCODE
Get-Content "$Out\e_apply.txt"; "exit $rc"
```

Expected, in order: `backup : ... (SQLite backup API; quick_check ok; exact
pre-state ok)`; `APPLIED at <UTC>: 22 row changes.`; the ledger lines
`ledger xch ... (+896841101167)`, `ledger db1a9020 ... (+0)` and
`ledger ae1536f5 ... (+1864)`, each `before -> after`;
`re-check : after COMMIT no XOPTrader process and no open handle (process list
re-read)`; the `NEXT` and `ROLLBACK` commands; the sha256 of the database, the
backup and the three scripts; `exit     : 0  OK`. Copy the three ledger lines and the two
file hashes into the record. The post-repair `ledger xch` value is the number
to compare with the wallet's confirmed XCH from step d.

| Exit | Meaning | What to do |
|---|---|---|
| 0 | Applied (or, on the dry run, `DRY RUN OK`). Also `ALREADY APPLIED`. | Go to f. `ALREADY APPLIED` (dry run or real run) wrote **no** `$Backup`: do not go to f; see "ALREADY APPLIED" below. |
| 1 | **Refused.** Bad arguments, the path guard, a hash pin mismatch, an intent file naming a phantom, or a `--backup-to` file (or its `-wal`, `-shm` or `-journal`) that already exists. Also a Ctrl+C or an unexpected error during the guards, before the database is opened: `REFUSED: interrupted or failed before the database was opened (...)`. The database was not opened for writing; only the audit log may have been written. | Fix the typo, go back to c, or set a new `$Backup`. After an interruption during the guards, run the same command again; the same `$Backup` is still valid. A pin mismatch means you are not running the reviewed scripts: stop. |
| 2 | **Not committed; nothing written.** Only this: a STATE MISMATCH (the database is neither the pre- nor the post-repair state), a failed check inside the transaction, an error or Ctrl+C after the database was opened and before COMMIT (one during the guards exits 1), or a COMMIT that raised while a fresh re-read still finds the exact pre-state. | **Stop.** Keep the output. Do not edit the scripts in the window, and do not retry a STATE MISMATCH: the database has moved since the design (a likely cause is a new XCH, DBX or BYC auto-adjust), and the repair must be re-planned. A plainly transient failure can be fixed and the dry run repeated. If the output says a backup was written before the failure, pass a new `--backup-to` next time. **Exception:** if any `pre :` line says `repair ledger rows already exist`, the database was repaired in an earlier run and has changed since. Treat it like ALREADY APPLIED: stop, start nothing, and never run v0.10.25 on it. Otherwise the database is **unrepaired**, so either version may run on it. Preferred: install v0.10.26 anyway (it books no new phantom); the re-plan must then correct any auto-adjust its first boot posts (policy §2). Record the choice. |
| 3 | **Busy; nothing written** to the database. A process, an open handle or a lock, or the process list could not be read. The audit log ends `exit     : 3  BUSY: nothing written`. If the lock came at the backup stage or at COMMIT, a full or partial `$Backup` may also have been written. | Go back to b. Set a new `$Backup` if the old file exists (the run refuses an existing one with exit 1). |
| 4 | **Committed**, but the re-check after COMMIT found an XOPTrader process or an open handle, or could not run. A `!!` banner lists it. | The repair is in the database. Run `--list-blockers` (step b) and clear what it lists. An `xop_trader.exe` or `xop_trader_gui.exe` there is **v0.10.25 running on the repaired database** (the installer has not run yet), which must never happen: close the GUI at once and answer Keep; close any agent session or tool it names. Then run f. **If the banner named the engine or the GUI**, the rollback window closed when it started, whatever f says: do not roll back. If f passes, nothing was written; if it fails, assume the engine has written rows. Either way, continue to g (install v0.10.26), then run the after-boot check at h and follow its FAIL branch if it fails. **If the banner named only something else** (another Python process, an open handle, or a re-check that could not run) and no engine or GUI started, the window is open and f decides as usual. Record what appeared. |
| 5 | **Committed** (normally), but a step after COMMIT failed: the read-back differs, an error or Ctrl+C after COMMIT, a failed summary, log or hash write, or a COMMIT whose outcome could not be confirmed as rolled back. The output says the repair IS committed (a `POST-COMMIT FAILURE` block, or an `ERROR (COMMIT)`, `ERROR after COMMIT` or `INTERRUPTED OR FAILED AFTER COMMIT` line). | The rollback window is open. Run f **now**. If f passes, decide on rollback at f, exactly as for exit 0. If f fails, see "Exit 5 and f fails" below before R1: when the output says `re-read: unknown`, `READ-BACK DIFFERS`, or that the fresh connection `sees neither` state, the repair may not be in the database at all. The output normally prints the checker and rollback commands; they are also in f and R1. |

Exit 6 belongs to `--restore-from` only (R1).

**ALREADY APPLIED** (dry run or real run). The database is already in the
post-repair state, so the script changed nothing and wrote **no** `$Backup`.
f's `--baseline $Backup` would refuse (exit 2, `does not exist`), and R1 has no
source. **Stop.** Do not go on to f or g, and start nothing: v0.10.25 must
never run on a repaired database. Find out who applied it:

1. When the repair rows were written. This read-only query prints the
   repair's UTC `entry_time` and `10`. Paste the block as it stands:
   PowerShell accepts the closing `"@` only at column 0.

```powershell
@"
import sqlite3, pathlib
con = sqlite3.connect(pathlib.Path(r'$LiveDb').as_uri() + '?mode=ro', uri=True)
print(con.execute("SELECT MIN(entry_time), COUNT(*) FROM ledger_entries WHERE note LIKE '%phantom-fill repair of trade_log 1900-1902%'").fetchone())
con.close()
"@ | python -
```

2. Which run wrote them. List the earlier repair backups and their logs:
   `Get-ChildItem C:\GitHub\XOPTrader-backups -Recurse -Filter 'xop_trader_pre_phantom_repair_*' | Select-Object FullName, Length, LastWriteTime`,
   then `Select-String -Path <each .log> -Pattern 'APPLIED at','COMMITTED at','^exit'`
   (the `APPLIED at` line sits about 15 lines above the end, so `-Tail 3`
   misses it). The run that applied it has
   `APPLIED at <that time>` (or `COMMITTED at`) in its `.log`, and its backup
   is the `.db` of the same name. A `.restore.log` for that backup ending
   `exit     : 0  OK` means that run was rolled back and a later one applied
   the repair again.
3. If you find that backup, set `$Backup` to it and run f. `115 PASS, 0 FAIL`
   means nothing has written since that run, so its window is still open: go
   on from f. Any FAIL, or no backup found, means that run's window cannot be
   shown to be open: start nothing, keep every output, and get it reviewed
   before g.

**Exit 5 and f fails.** Read the apply output first.

- If it has none of `re-read: unknown`, `COMMITTED, BUT THE READ-BACK
  DIFFERS`, or an `ERROR (COMMIT)` line saying a fresh connection `sees
  neither the exact pre-repair state nor the repaired state`, the repair was
  committed and verified inside the transaction: roll back with R1, as at f.
- If it has one of them, R1 will refuse (exit 1), because it needs the
  database to be exactly `$Backup` plus the repair. Find the state with a dry run under a
  new, unused backup name. A dry run writes no backup, only
  `<name>.dryrun.log`:

  ```powershell
  $Probe = "$Out\probe_$((Get-Date).ToUniversalTime().ToString('HHmmss')).db"
  cmd /c "python -u $Apply $LiveDb --i-stopped-the-engine --backup-to $Probe --dry-run > $Out\e_probe_dryrun.txt 2>&1"; $rc = $LASTEXITCODE
  Get-Content "$Out\e_probe_dryrun.txt"; "exit $rc"
  ```

  - **`DRY RUN OK`, exit 0**: nothing was committed; the database is the exact
    pre-state. Treat it as a transient exit 2: you may repeat the real run
    once, with `$Backup = $Probe` (that file does not exist yet), and then go
    on to f with that `$Backup`. If it fails the same way again, stop as for
    exit 2.
  - **`STATE MISMATCH`, exit 2**: the database is in neither state. If nothing
    else has written (below), restore `$Backup` by hand, as follows. Besides
    the 14:52 backup, this is the only sanctioned manual restore.
    1. `--list-blockers` must exit 0.
    2. `$Src = $Backup`, then steps 1 and 2 of "Manual restore" (move the live
       .db, -wal and -shm aside together; restore with the backup-API block).
    3. `Get-FileHash $LiveDb, $Src` gives equal hashes, and the probe dry run
       above, run again with a new `$Probe`, says `DRY RUN OK`.
    4. The database is unrepaired again. Record it, and continue as for
       exit 2. Do not repeat the real run in this window unless the cause is
       understood.
  - **`ALREADY APPLIED`**: the repair is there, but f or R1 found more
    changed: something else has written, or the scripts disagree. Start
    nothing, keep every output, and get it reviewed.

  Nothing else has written when all three hold: the apply exited 5, not 4
  (its `re-check :` line, if the run got that far, says `no XOPTrader process
  and no open handle`); nothing has been started since step b (not the GUI,
  not the installer, not a SQLite tool; only this runbook's checker and dry
  runs); and `--list-blockers` exits 0 now. If any of them does not hold, or
  you are unsure, start nothing and get help: a manual restore would erase
  whatever wrote.

**f. Run the checker: the rollback decision point.**

```powershell
cmd /c "python -u $Check $LiveDb --baseline $Backup --allow-live-readonly > $Out\f_check.txt 2>&1"; $rc = $LASTEXITCODE
Get-Content "$Out\f_check.txt"; "exit $rc"
```

Before acting on it, check its first lines: three `sha256` lines ending
`[matches SHA256SUMS]` and no `WARNING  : the scripts do not match
SHA256SUMS`. The checker only warns on a mismatch and runs anyway; if the
warning is there, ignore the result and stop (Known edges).

The checker is read-only. Every read of the database happens in one read
transaction, and the baseline is opened immutable. On a repaired copy of the
snapshot it ends **`115 PASS, 0 FAIL`** and exits 0. The live count is the same
unless the live ledger has gained an asset or the database a table since 14:52;
what decides is **0 FAIL and exit 0**. Its exit codes: 0 every check PASS, 1 any
FAIL, 2 refused (bad arguments, a path the guard rejects, or an XOPTrader
process running).

Copy its two `INFO F12` lines (XCH and BYC ledger minus tracker) into the
record. The read-only connection leaves an empty `-wal` and a `-shm` beside the
database; that is normal SQLite behaviour.

- **All PASS**: keep the repair and go to g.
- **Any FAIL, or a refusal you cannot resolve**: roll back now with R1, before
  anything starts. Two exceptions: after an exit 4 whose banner named the
  engine or the GUI, do not roll back (exit-4 row); after an exit 5, read
  "Exit 5 and f fails" first.

**g. Run the v0.10.26 installer.**

1. Run the installer whose hash you checked, and answer UAC at once.
2. The last page's **"Launch XOPTrader"** is checked by default and starts
   v0.10.26 at once. Leave it checked, or uncheck it and start XOPTrader from
   the Start menu when you are ready to watch. Either way, the first start
   closes the rollback window, so f must have passed before you click Install.
3. The installer's PrepareToInstall runs `taskkill /F` on both executables
   (after step b there is nothing to kill), then **runs the old version's
   silent uninstaller**, then copies the new files. From that point v0.10.25
   may be gone.
4. **If UAC times out or the installer fails, start nothing.**
   - **Never relaunch v0.10.25 on the repaired database.** It has no fill
     proof and the same S14 reopen. If the wallet reports a phantom
     PENDING_CANCEL at its boot, it reopens the repaired row into State, and
     a later CONFIRMED report (the 09-22 sequence) books it as a fill again.
     With trade_log 1900-1902 deleted, the UNIQUE trade_id no longer stops
     that journal write, while the ledger legs are dropped by
     `INSERT OR IGNORE`.
   - **This overrides any general upgrade procedure**, including an agent's
     remembered rule to relaunch the old GUI when UAC times out (Inno exit 2
     with no log). In this window a timeout comes after COMMIT, while v0.10.25
     is still installed. The operator runs the wizard interactively (the UAC
     prompt is on the secure desktop, so no remote session or agent can answer
     it). Nobody runs it with `/SILENT` or relaunches a GUI. Nobody clicks the
     Start-menu or desktop XOPTrader shortcuts between e and g: they start
     whatever version is installed.
   - **Preferred: retry the installer.** The repair stays applied and valid,
     and the window stays open until something starts.
   - **If v0.10.26 cannot be installed in this window and the bot must
     trade:** restore with R1 first (the window is still open because nothing
     has started), then relaunch v0.10.25, **if it is still installed**. If
     PrepareToInstall already removed it, there are two installers: retry
     v0.10.26, or, only after R1 has exited 0, reinstall v0.10.25 from
     `C:\Users\dorkm\Downloads\xop_trader-installer-windows-x64-v0.10.25.exe`
     (downloaded 2026-09-21; this runbook does not pin its hash).
5. **Put the Startup shortcut back** once v0.10.26 is installed (or once R1
   has exited 0, when the database is unrepaired again), not before: until
   then it would start v0.10.25 at the next logon.

   ```powershell
   Move-Item "$Out\XOPTrader.lnk" $Lnk
   Test-Path $Lnk     # True
   ```

**h. Watch the first heartbeats.**

Read the engine log with one-shot reads, repeated as needed. **Never use
`Get-Content -Wait` or `tail -F` on a log that a live process rotates.** On
2026-08-31 that permanently froze `gui.log`. The engine log rotates every
10 MB (about every 2.3 hours at the current rate) into `xop_trader.1.log` to
`xop_trader.9.log`, so it keeps about 21 hours: search `xop_trader*.log`, and
save the matches within a day.

```powershell
Get-Content C:\GitHub\XOPTrader\logs\xop_trader.log -Tail 300
Select-String -Path C:\GitHub\XOPTrader\logs\xop_trader*.log -Pattern 'd6a8325c15','db63709cb9','83eef9df80' |
  Out-File -Encoding utf8 -Width 4096 "$Out\h_ids.txt"
Select-String -Path C:\GitHub\XOPTrader\logs\xop_trader*.log -Pattern 'Ledger invariant breach','LEDGER CONTROL','posted adjusting entry' |
  Out-File -Encoding utf8 -Width 4096 "$Out\h_ledger.txt"
Get-Content "$Out\h_ids.txt", "$Out\h_ledger.txt"
```

`-Width 4096` keeps each match on one line; without it `Out-File` wraps them
at the console width. Only lines timestamped after the first start count;
older ones are history.

- **The three ids**: expect no lines, unless step c or step d found something.
- **Ledger lines**: for each, record the asset, ledger, wallet, divergence,
  tol and exposure, and the amount of any adjusting entry. An XCH adjust is
  expected once the book is thin; compare its amount with wallet minus ledger
  from steps d and e (First start, item 2). A small BYC one is possible.
- **In the GUI**: lifetime P&L has fallen by the phantom amounts, 1900-1902
  have gone from the trade table, and the three offers have gone from the
  filled list.
- **About 20 blocks in**, Step 11 re-ties the tracker to the wallet.
- **After the first heartbeats**, run the checker in its after-boot mode. It
  may run while the engine runs.

  ```powershell
  cmd /c "python -u $Check $LiveDb --baseline $Backup --allow-live-readonly --after-boot > $Out\h_check_after_boot.txt 2>&1"; $rc = $LASTEXITCODE
  Get-Content "$Out\h_check_after_boot.txt"; "exit $rc"
  ```

  It must report 0 FAIL and exit 0. With no engine event on the three rows, or
  only observation events, it ends **`68 PASS, 0 FAIL`**. Each offer whose row
  the engine reopened or re-closed replaces two lines with one, so one such
  offer gives 67 and all three give 65; `INFO H2 ... engine event` lines name
  those events. As at f, its `sha256` lines must end `[matches SHA256SUMS]`.

**If the after-boot check exits 2 (refused).** It checked nothing: a bad
argument, a path refusal, a `--baseline` whose `-wal` is not empty, or the
apply module failing to load. The output names the cause. Fix it (for
example, point `--baseline` at `$Backup` again, and never at a file anything
has opened for writing) and run the same command again. Change nothing in the
database meanwhile. If it cannot be made to run, record the output and treat
it like exit 1, step 1 onwards.

**If the after-boot check exits 1 (any FAIL).** The rollback window closed at
the first start. R1 refuses, and a manual restore of `$Backup` or of any other
backup is **forbidden**: it would erase every row the engine has written
since the start. Change nothing in the database. Instead:

1. Keep `h_check_after_boot.txt`. Copy every `FAIL` line and every
   `INFO H2 ... engine event` line into the record.
2. Save the log matches again under new names (the two `Select-String`
   commands above, into `h_ids_fail.txt` and `h_ledger_fail.txt`); the logs
   rotate away within a day.
3. Re-prove the three offers from the chain: the step d loop, writing
   `$Out\h_prove.txt` instead of `d_offers.txt`.
4. Read them together:
   - Every offer still `VERDICT: Dead`, and the FAILs are on H2 or G1 and
     name an engine event or an order of events the checker does not know
     (for example a re-close reason outside its list): the checker fails
     closed on engine behaviour it was not written for. The repair stands.
     Record it and have the checker's finding reviewed.
   - `E1 no trade_log row for the three phantom offers` fails: one of them was
     booked as a fill again. If its proof says `VERDICT: Dead`, the engine
     booked a fill the chain disproves, which v0.10.26 must never do: record
     it and get it reviewed. The fix is a new, reviewed forward correction,
     never a restore.
   - Any other FAIL (F, H1, H3, H4, H6 or I lines): a row the repair wrote or
     kept has changed. Record it and get it reviewed.
5. **R2 applies only when the repair itself is shown wrong**: the chain proof
   for one of the three offers says `VERDICT: Settled (a real take)`. A
   checker FAIL on its own never justifies R2.

**i. Fill in the execution record** below, including the checkout's HEAD from
the script's `git :` line. First run `Stop-Transcript`: `transcript.txt` is
complete only once it is stopped. Commit the record (this document) together
with these files, copied from `$Out` into
`docs/phantom-fill-repair-2026-09-evidence/` on the repair branch: `transcript.txt`, `0_hashes.txt`, `b_list_blockers.txt`,
`d_offers.txt`, `e_dryrun.txt`, `e_apply.txt`, `f_check.txt`, `h_ids.txt`,
`h_ledger.txt`, `h_check_after_boot.txt`, `sweep.txt` once the completeness
sweep (Follow-ups) has run, `r1_restore.txt` if R1 ran, and any branch file
(`e_probe_dryrun.txt` with its `probe_*.db.dryrun.log`, `r1_state_check.txt`,
`h_ids_fail.txt`, `h_ledger_fail.txt`, `h_prove.txt`);
and the script's own logs `$Backup.dryrun.log`, `$Backup.log`, and
`$Backup.restore.log` if R1 ran. If those files stay in
`C:\GitHub\XOPTrader-backups`, commit their hashes instead.

## Rollback

**Rollback source.** Roll back from `$Backup`, the `--backup-to` file taken
**inside the repair transaction**.

- The script took it through the SQLite backup API while it held the write
  lock. It then checked it with `quick_check` and against the exact
  pre-state.
- So it is exactly the database the repair changed, at the moment it changed
  it.
- The dry run writes no backup; each real run writes one. If a real run was
  repeated (the exit-3 row, or the exit-5 probe case), there is more than one
  backup file: roll back from the `$Backup` of the run that committed, and
  keep the others.

The 14:52 backup,
`C:\GitHub\XOPTrader-backups\xop_trader_pre_v0.10.26_20260926_145222.db`, is
a **last resort only**. Restoring it loses every row the engine wrote after
14:52: closure events, reward, take and fill legs, offer_log rows and
trade_log rows. Offers posted after 14:52 would become untracked, and neither
v0.10.25 nor v0.10.26 adopts an untracked CONFIRMED trade, so a take of one of
them would never be booked. If you must use it, re-plan the gap it leaves.

**Rollback window: from COMMIT to the first start of the engine or the GUI**,
of any version. Within the window, a restore is exact. After it, a restore
would erase what the engine has written since, so an un-repair can only go
forward (R2).

### R1. Restore, inside the window

1. **Stop everything.** `python $Apply $LiveDb --list-blockers` must exit 0.
2. **Let the script restore.**

   ```powershell
   cmd /c "python -u $Apply $LiveDb --i-stopped-the-engine --restore-from $Backup > $Out\r1_restore.txt 2>&1"; $rc = $LASTEXITCODE
   Get-Content "$Out\r1_restore.txt"; "exit $rc"
   ```

   Its log is `$Backup.restore.log`. What the script does:
   - It refuses unless `$Backup` is the exact pre-state **and** the live
     database is `$Backup` plus exactly this repair, with every other row
     identical (read in one read transaction).
   - It builds the replacement beside the database with the SQLite backup
     API, as `xop_trader.db.restoring-<UTC>`, and verifies it.
   - It checks the process list and the file handles again.
   - It moves `xop_trader.db` and its -wal and -shm aside together (siblings
     first), as `xop_trader.db.rolledback-<UTC>`, and renames the replacement
     into place.
   - It checks the result again: `quick_check`, then the exact pre-state.

   | Exit | Meaning | What to do |
   |---|---|---|
   | 0 | `RESTORED:` the database is the exact pre-repair state again. The repaired database is kept in the data directory as three files that belong together: `xop_trader.db.rolledback-<UTC>` and its `-wal` and `-shm`. | Step 3. Keep the three files together. |
   | 1 | Refused; nothing touched. Usually the database is not exactly `$Backup` plus the repair: the window has closed, or this is the wrong backup. | **Stop.** Something besides the repair has changed the database, and a restore would lose it. Find out what first, and do not fall back to the manual procedure to force it. The one exception is an apply that exited 5 with `re-read: unknown`, `READ-BACK DIFFERS`, or an `ERROR (COMMIT)` line saying the fresh connection `sees neither` state: there the refusal is expected, and step e's "Exit 5 and f fails" applies. |
   | 2 | Failed before the swap; or the swap failed, or the restored database failed verification, and the swap was undone automatically (`UNDONE : the repaired database is back in place`). The database is exactly as it was: still repaired. | Read the output, fix the cause, and run R1 again. If it says `WARNING  : could not delete the temporary replacement; delete it by hand`, move the files it names (a copy of `$Backup`) into `$Out` once the script has exited, before anything else. |
   | 3 | Busy; nothing touched. The temporary replacement was deleted first (the output says so, or names what to delete by hand). The comparison's read-only open may leave an empty `-wal` and a `-shm` beside the database; they are harmless. | Back to step 1. |
   | 6 | **Incomplete**: the swap began and could not be finished or undone (including an undo after a failed verification). | **Start nothing.** The output lists where each file is and prints `move` lines that put the repaired database back. Run them exactly, in order, then decide again. If it printed **no** `move` lines but `ERROR after the restore settled`, see Known edges, item 5. |

3. **Record the rollback and why.** The database is back in its pre-repair
   state, with the three fills booked. It is unrepaired, so either version may
   run on it: v0.10.25 (the status quo), or v0.10.26, whose first boot may
   post auto-adjusts that a re-planned repair must then correct (policy §2).

### Manual restore: last resort only

Use this only for a restore the script cannot do by design, in one of two
cases:

- the 14:52 backup, and only after a deliberate decision that accepts losing
  every row written since that backup;
- `$Backup`, in the exit-5 dead end of step e ("Exit 5 and f fails"), and only
  when nothing else has written. There it loses nothing: `$Backup` was taken
  inside the repair transaction.

After the first start of the engine, never: a manual restore would erase what
the engine has written (step h, R2). The engine and the GUI must be stopped
(`--list-blockers` exit 0). `$Src` is the backup you decided to restore.

1. **Move the live files aside together**, the .db, -wal and -shm. Never
   delete them:

   ```powershell
   $Q = "$Out\rolled_back_$(Get-Date -Format 'yyyyMMdd_HHmmss')"
   New-Item -ItemType Directory $Q | Out-Null
   foreach ($f in $LiveDb, "$LiveDb-wal", "$LiveDb-shm") { if (Test-Path $f) { Move-Item $f $Q } }
   ```

2. **Restore with the SQLite backup API, never a file copy.** A leftover -wal
   beside a copied .db is replayed onto it. Paste the block below as it
   stands: PowerShell accepts the closing `"@` only at column 0.

```powershell
@"
import sqlite3, pathlib
src = sqlite3.connect(pathlib.Path(r'$Src').as_uri() + '?mode=ro', uri=True)
dst = sqlite3.connect(r'$LiveDb')
src.backup(dst)
print('quick_check', dst.execute('PRAGMA quick_check').fetchone()[0])
print('journal_mode', dst.execute('PRAGMA journal_mode').fetchone()[0])
dst.close(); src.close()
"@ | python -
```

   Expected output: `quick_check ok` and `journal_mode wal`. This block was
   tested on synthetic WAL databases in scratch: the restored file hashed
   equal to its source. It opens `$Src` read-only but not immutable, so it
   leaves an empty `$Src-wal` and a `$Src-shm` beside the backup; they are
   harmless (`--restore-from` and the checker accept a backup whose `-wal` is
   empty).

3. **Verify and record.**
   - `Get-FileHash $LiveDb, $Src` should give equal hashes.
   - Run `python $Apply $LiveDb --i-stopped-the-engine --backup-to <new file> --dry-run`
     (or the probe dry run of step e). It prints `DRY RUN OK` when the three
     offers are back in their pre-repair state.
   - Then re-plan whatever the older backup lost (nothing, for `$Backup`).

### R2. Un-repair, after the first start (forward only)

This re-books fills the chain proved never happened. Do it only if the repair
itself is shown to be wrong. No script exists for it. Write one and review it
first, with the same shape as the repair: dry-run gated, asserting its exact
pre-state, one transaction. Run it with the engine and the GUI stopped, after a
backup made with the backup API.

1. **Check which auto-adjusts came after the repair.** List every XCH, DBX and
   BYC `adjust` posted after the repair's rows. The first-start XCH adjust is
   the likely one. Any adjust that absorbed repair legs needs its own
   `correction:<its event_id>` leg, or the un-repair counts twice (policy §2,
   "Correcting an auto-adjust"). Also check for an S14 reopen of the three
   rows. This step is a design decision, not a mechanical one.
2. **Re-insert the archived trade_log rows** with their original ids 1900-1902,
   taken from the JSON in each `phantom_fill_correction` event. The ids are
   free: `sqlite_sequence` is at 1903 or above and never reused them. To
   extract a row, take `reason.split("original row: ", 1)[1]` and parse it with
   `json.JSONDecoder().raw_decode(...)`.
3. **Append counter-reversal legs**, one per repair row:
   - 9 legs with `event_type` `reversal`, `event_id`
     `reversal:reversal:<offer_id>`, each negating one leg of
     `reversal:<offer_id>` (same leg, asset, pair and block);
   - 1 `adjust` leg, `event_id` `correction:correction:adjust:<DBX>:9330325`,
     of -203,188 DBX, negating `correction:adjust:<DBX>:9330325`;
   - the corrections from step 1.
4. **Append closure events**, one per offer, cancelled -> filled, giving the
   reason and the evidence that the repair was wrong.
5. **Restore offer_log** 21185, 21243 and 21311 to status 'filled' and
   cancel_reason ''. Guard the UPDATE on id, offer_id and status 'cancelled'.
6. **Restart.** P&L rehydrates with the three fills.

## Known edges of the pinned scripts

The scripts are hash-pinned, so these are documented here, not fixed. Each
gives the behaviour, as read from the code or observed on copies in the
pass-3 verification, and what the operator does.

1. **Exit 5 with the COMMIT outcome unknown.** If COMMIT raises and a fresh
   read-only connection sees neither the exact pre-state nor the repaired
   state, or cannot open the database, the script does not guess: it treats
   the repair as committed and exits 5. It prints `ERROR (COMMIT): ... Treat
   it as COMMITTED`, then `COMMITTED at <UTC> (22 row changes in the
   transaction), BUT UNVERIFIED -- see above.`, the three `ledger` lines with
   post-repair values, and a `POST-COMMIT FAILURE` block that lists
   `COMMIT raised ...; re-read: unknown`. (If a Ctrl+C interrupted the COMMIT,
   only that `ERROR (COMMIT)` line and an `INTERRUPTED OR FAILED AFTER
   COMMIT` line appear.) The ledger values were computed inside the
   transaction and prove nothing about what was committed. The database may
   be unchanged: in the fault-injection test it was, f reported
   `68 PASS, 47 FAIL`, R1 refused with exit 1, and a dry run said `DRY RUN OK`.
   A read-back that differs after COMMIT (`COMMITTED, BUT THE READ-BACK
   DIFFERS`) leads to the same dead end. It needs COMMIT to raise and a
   read-only open to fail as well, so it is rare. **Do:** step e, "Exit 5 and
   f fails".
2. **A Ctrl+C or unexpected error during the guards exits 1, not 2.** The
   guards run before the database is opened: the hash pins, the path checks,
   opening the audit log, the process list (2-3 s, the likeliest moment for a
   Ctrl+C), the intent files and the handle test. An interruption there
   prints `REFUSED: interrupted or failed before the database was opened
   (KeyboardInterrupt: )` and exits 1; only the audit log was written. **Do:**
   run the same command again. The same `$Backup` is still valid, because the
   backup is taken only inside the transaction.
3. **A Ctrl+C at the very start or the very end escapes the script's
   handler.** One before the run starts (while the arguments are parsed, or
   during the read-only `--list-blockers`), or one inside the final write of
   the `exit :` line, ends in a Python `KeyboardInterrupt` traceback
   with Python's own exit status (not 0-6) and no `exit :` line. The second
   can come after COMMIT. **Do:** read the output file. An `APPLIED at` or
   `COMMITTED at` line means the repair is in the database: run f now.
   Otherwise nothing was committed: act on the output's last result line.
4. **A restored database that fails verification exits 2, not 6.** The
   docstring lists it under 6. The code undoes the swap and exits 2
   (`ERROR: the restored database failed verification (...).  Undoing the
   swap.`, then lower-case `undone` lines and `UNDONE : the repaired database
   is back in place, exactly as it was; the restore did not happen.`). It
   exits 6
   only if that undo fails too. **Do:** the R1 table, rows 2 and 6.
5. **`--restore-from` interrupted after its swap settled exits 6 with no file
   list and no `move` lines.** A Ctrl+C or error that lands after the swap
   finished and verified, or after a failed swap was undone, prints
   `ERROR after the restore settled (<error>); see the lines above` and exits
   6 (`RESTORE INCOMPLETE: start nothing; follow the printed moves`), but no
   moves were printed. Two states lead there:
   - (a) the restore finished. The database is the exact pre-state
     (`RESTORED:` may be missing from the output), and the repaired database
     is at `xop_trader.db.rolledback-<UTC>` with its `-wal` and `-shm`.
   - (b) the swap failed and was undone (lower-case `undone` lines above the
     error, but no upper-case `UNDONE` line). The database is still the
     repaired one, and the interruption hit while the temporary was being
     deleted, so `xop_trader.db.restoring-<UTC>` (a 323 MB copy of `$Backup`)
     may be left in the data directory.

   **Do:** start nothing, and find out which with the checker:

   ```powershell
   Get-ChildItem C:\GitHub\XOPTrader\data -Filter 'xop_trader.db*' | Select-Object Name, Length, LastWriteTime
   cmd /c "python -u $Check $LiveDb --baseline $Backup --allow-live-readonly > $Out\r1_state_check.txt 2>&1"; $rc = $LASTEXITCODE
   Get-Content "$Out\r1_state_check.txt"; "exit $rc"
   ```

   - `115 PASS, 0 FAIL`, exit 0: still repaired, state (b). Once the script
     has exited, move any `xop_trader.db.restoring-<UTC>` file, and any
     `-wal`, `-shm` or `-journal` beside it, into `$Out`. Then decide again
     at f; R1 may be run again.
   - Exit 1 with `FAIL  E1 no trade_log row for the three phantom offers`
     naming 1900, 1901 and 1902 (on a copy of the pre-state it ends
     `68 PASS, 47 FAIL`): the repair is gone. Confirm with the probe dry run
     of step e ("Exit 5 and f fails"): `DRY RUN OK` proves the exact
     pre-state, so the restore finished, state (a). Go to R1 step 3, and keep
     the three `.rolledback-<UTC>` files together.
   - Anything else: start nothing, leave every file where it is, and get
     help.
6. **`--restore-from` ignores an audit-log write failure.** If writing
   `$Backup.restore.log` fails after it opened (for example on a full disk),
   the error stream gets `WARNING  : the audit log ... could not be written
   (...); it is incomplete from here`, and the restore carries on, swaps and
   exits 0. The log then ends early, without its `exit :` line. (The apply
   treats the same failure as fatal: it refuses to COMMIT, exit 2, or turns
   0 into 5 after COMMIT.) If the log cannot be opened at all, the restore
   refuses with exit 1 and touches nothing. **Do:** trust `r1_restore.txt`,
   which also captures the error stream: `RESTORED:` and `exit     : 0  OK`
   there mean the restore happened. Record that the audit log is incomplete.
7. **The checker only warns on a pin mismatch.** It prints the three
   `sha256` lines and, on a mismatch, `WARNING  : the scripts do not match
   SHA256SUMS`, then runs and reports a result anyway, on the live database
   too. It imports the exact pre-state check and the engine-event rules from
   the apply script beside it, so a modified apply script changes its
   verdict. The apply refuses the live database on the same mismatch. **Do:**
   the session setup's `--print-hashes` check comes first and must pass, with
   the `lf=` values equal to the Pinned artefacts table. Before acting on f
   or h, check the checker's own `sha256` lines; if the warning is there,
   ignore the result and stop.
8. **`--write-hashes` ignores every other argument.** It is handled before
   anything else: it rewrites `SHA256SUMS` from the files on disk and exits
   0, so `apply_phantom_repair.py <db> --write-hashes` silently re-pins
   modified scripts, and the apply's own pin check then passes them. **Do:**
   never pass `--write-hashes`; nothing in this runbook uses it. The real
   check is the `lf=` values against the Pinned artefacts table in this
   document, not the `[matches SHA256SUMS]` tag alone.
9. **Read-only opens leave an empty `-wal` and a `-shm`.** A read-only SQLite
   connection cannot remove them, so the checker (its target), the restore's
   comparison of the live database, the ALREADY APPLIED query and the
   manual-restore block (its `$Src`) leave a 0-byte `-wal` and a 32 KB `-shm`
   beside the file they read. After a BUSY restore refusal they stay in the
   data directory (the `.restoring-` temporary does not). Immutable opens
   (every baseline and backup check) and `--list-blockers`, which opens no
   SQLite connection, leave nothing. **Do:** leave them. The engine opens the
   database normally, `--restore-from` and the checker accept a backup whose
   `-wal` is empty, and R1 moves them aside with the database.
10. **A failed close while the restore builds its replacement leaves the
    temporary.** If closing the replacement's connection raises, the backup's
    connection stays open, so `xop_trader.db.restoring-<UTC>` cannot be
    deleted: R1 exits 2 with `WARNING  : could not delete the temporary
    replacement; delete it by hand`, and the database is untouched. **Do:**
    the R1 table, row 2.

**Docstring statements this document overrides**, besides the policy points
listed under Pinned artefacts:

- Apply, EXIT CODES 1: it omits an interruption during the guards (item 2).
- Apply, EXIT CODES 5 and the output's "the repair IS committed": not
  certain when the re-read says `unknown` (item 1).
- Apply, EXIT CODES 6, "or the restored database failed verification": that
  exits 2 unless the undo fails too (item 4).
- Apply, EXIT CODES 6 and ROLLBACK, "the output names where each file is and
  the exact moves": not after an interruption once the swap settled (item 5).
- Apply, SAFETY, "Audit trail ... Its last line is the exit code": not for a
  restore whose log write failed (item 6), nor for a run interrupted in its
  last write (item 3).
- Apply, `--write-hashes` "after a REVIEWED change only": not enforced
  (item 8).
- Checker, "nothing is written": its read-only open of the target leaves an
  empty `-wal` and a `-shm` (item 9). Its pin check is a warning only
  (item 7); the apply docstring's "refused on any mismatch" is the apply's
  behaviour alone.

## Follow-ups (not part of the repair)

- **Completeness sweep.** This branch teaches
  `scripts/verify_fill_completeness.py` to list an offer as excluded, not
  missing, when it has a `status_update` event whose `closure_reason` is
  exactly `dead_on_chain`. It counts offers, not events. The repair's verdict
  rows are such events, so after the repair 1900 and 1901 are excluded.
  Without this the sweep would FAIL for ever on the two phantoms the wallet
  still calls CONFIRMED, and on every future dead offer the wallet calls
  CONFIRMED; a FAIL like that is the kind that gets "fixed" by a backfill.
  Run it after the first start. It is read-only (wallet RPC and a read-only
  database URI), needs the wallet running and synced, and needs the Python
  `requests` package. It may run while the engine runs.

  ```powershell
  # In a new session, first set $Out to the window's folder by hand, e.g.
  #   $Out = 'C:\GitHub\XOPTrader-backups\phantom-repair-20260926_...'
  if (-not $Out -or -not (Test-Path $Out)) { throw 'Set $Out to the window folder first' }
  cmd /c "python -u C:\GitHub\XOPTrader-phantomfix\scripts\verify_fill_completeness.py --db C:\GitHub\XOPTrader\data\xop_trader.db --since-height 9321000 > $Out\sweep.txt 2>&1"; $rc = $LASTEXITCODE
  Get-Content "$Out\sweep.txt"; "exit $rc"
  ```

  The redirect keeps the sweep's `WARNING` and `ERROR` lines, which it
  writes to the error stream (Session setup).

  - Run **this branch's copy, by absolute path**. The live checkout
    `C:\GitHub\XOPTrader` is at v0.10.23, and its copy has no exclusion: it
    would FAIL on 1900 and 1901. Once this branch is merged and the live
    checkout updated, its copy is the same.
  - `--since-height 9321000` starts the window at about the start of
    2026-09-21 UTC, before the first phantom offer was posted (block
    9,324,678), and does not shrink however late the sweep runs. A full-history
    run is not useful: it still lists the 348 known pre-window era trades.
  - What to expect depends on the wallet statuses when the sweep runs. It
    checks only CONFIRMED records, and each of 1900-1902 that it checks is
    excluded, not missing. Step d recorded the statuses, but they can change
    later: a Cancel sweep or a reused coin can turn a CONFIRMED phantom
    PENDING_CANCEL and then CANCELLED (First start, item 5).
    - If step d found CONFIRMED, CONFIRMED, CANCELLED and nothing has changed:
      `PASS with WARN: every CONFIRMED wallet trade in scope is recorded,
      except 2 excluded as proven dead on-chain -- re-prove them (WARN
      above).`, exit 0, with 1900 and 1901 in the `WARN: Excluded, not
      checked` section. 1902 is CANCELLED in the wallet, so the sweep does not
      check it.
    - If fewer of the three are CONFIRMED now, fewer are excluded; with none,
      the result is the plain `PASS: every CONFIRMED wallet trade in scope is
      recorded.`, exit 0.
    - If the `excluded (dead_on_chain):` count is not 2, run the step d loop
      again: the count must equal the number of the three the wallet now
      calls CONFIRMED.
  - A `FAIL` naming a trade taken in the last few minutes can be transient.
    The grace window is judged by an offer's accepted, else created, time, so
    a maker offer posted earlier and taken moments ago is not in it. Run the
    sweep again a few minutes later before investigating.
  - A `WARNING: wallet reports NOT synced` line means the result is not
    trustworthy: wait for the wallet to sync and run it again. An uncaught
    error (for example a missing `requests` package) also exits 1, like a
    FAIL; the traceback in `sweep.txt` tells them apart.
  - Under each excluded offer the sweep prints a `re-prove: python
    ".../prove_fill_on_chain.py" <trade_id>` line. Run each (it needs the local
    wallet and full node) and expect `VERDICT: Dead`. Any other verdict puts
    the engine's verdict in doubt: investigate the offer as a possibly real,
    unrecorded fill, and never backfill blindly.
  - Exit codes: 0 PASS (also PASS with WARN), 1 FAIL (a missing trade), 2 an
    operational error, 3 only with `--strict`: nothing missing, but an offer
    excluded.
- **Trade history files: a later maintenance task, with the engine stopped.**
  Not part of the window.
  - `data\trade_history\trades_live.csv` is **not edited**. The engine appends
    every fill to it, its lines have no column for a note, and
    `trades_full.csv` must keep the same columns. Its three phantom lines are
    recorded in `db/schema.md` (Durable trade-history files) and in this
    document. Never open it in Excel while the engine runs: Excel locks the
    file, and the engine's append of a fill then fails (the fill stays in
    `trade_log`, but its CSV line is lost).
  - `trades_full.csv` lists 1900-1902 until it is regenerated.
    `scripts/export_trade_history.py` takes no arguments: it reads
    `data\xop_trader.db` and overwrites `data\trade_history\trades_full.csv` in
    the checkout it sits in, so it works only from the live checkout (its copy
    there is the same as this branch's). At a later stop, after the repair:
    1. Close the GUI and answer Keep.
       `Get-Process xop_trader, xop_trader_gui -ErrorAction SilentlyContinue`
       must print nothing.
    2. Keep the old export:
       `Copy-Item C:\GitHub\XOPTrader\data\trade_history\trades_full.csv C:\GitHub\XOPTrader-backups\trades_full_before_phantom_repair.csv`
       (if the file exists).
    3. `python C:\GitHub\XOPTrader\scripts\export_trade_history.py`. It opens
       the database read-only (which leaves an empty `-wal` and a `-shm`,
       Known edges item 9) and writes nothing else.
    4. `Select-String -Path C:\GitHub\XOPTrader\data\trade_history\trades_full.csv -Pattern 'd6a8325c15','db63709cb9','83eef9df80'`
       must print nothing.
    5. Start the GUI again.
- **Engine follow-ups**, each needing its own PR:
  - S14 (`startup_pending_cancel_action`) must not reopen a 'cancelled' row
    that has a `dead_on_chain` closure event.
  - `recheck_terminal` must treat `proven_dead_` ids as StillTerminal, whatever
    the wallet status.
  - Re-adopting an offer the wallet reports CONFIRMED should create a
    fill-proof hold.
  - The trigger: a new offer built on an XCH coin that a pending transaction
    spends as its fee (CHANGELOG v0.10.26, "Not in this change").
- **Historical audit** of other dead offers. Find them through the closure
  events, not through offer_log's reason. Include offer_log 21449, which has
  been cancel_pending since 09-23, is Dead on-chain, and holds phantom 1900's
  unspent maker coin. Also include taker fills 631 and 636 (Residuals).

## Execution record (template: fill in at the window)

Operator: `________`  Date: `________`  Times in local time (CDT) unless
marked UTC.

| Step | Time | Result |
|---|---|---|
| Scripts checkout path, and HEAD from the script's `git :` line (informational; recorded here after the run) | | |
| `$Out` folder | | |
| Setup: `--print-hashes` exit code; do the three `lf=` values equal the Pinned artefacts table? | | |
| Setup: `python --version`; installer file and sha256 (= `ee5e15e4...5abf`?) | | |
| a. GUI closed; offers answer (Keep expected; if Cancel, why) | | |
| b. `--list-blockers` RESULT and exit code; what had to be closed; any `taskkill`? | | |
| c. `uncancelled.txt` / `.json` / `.txt.tmp`: absent / present; lines or file removed | | |
| d. Wallet status 1900 / 1901 / 1902, and each VERDICT | | `____` / `____` / `____` |
| d. Wallet confirmed (Total) XCH / DBX / BYC, in mojos; does Pending equal Total? | | |
| d. No fourth phantom: trade_log rows after 1903 (each re-proved?), offer_log 21449 status | | |
| e. Dry run: last line, exit code | | |
| e. Real run: `APPLIED at` (UTC), row changes, exit code | | |
| e. Ledger lines: xch / db1a9020 / ae1536f5, before -> after | | |
| e. Wallet confirmed XCH (d) minus post-repair XCH ledger (e): the XCH adjust to expect; same for BYC | | |
| e. `$Backup` path and sha256; live database sha256 after COMMIT (both from the apply output) | | |
| f. Checker: PASS count / FAIL count, exit code | | |
| f. INFO F12 lines: XCH and BYC ledger minus tracker | | |
| Rollback decision at f (none / R1 / manual); R1 exit code and `$Backup.restore.log` | | |
| g. Installer run: UAC answered, result, Launch checked? Any retry? | | |
| h. First engine start: version and time (the rollback window closes) | | |
| h. P&L after the start: XCH/DBX realized and fills, XCH/BYC fills | | |
| h. Ledger invariant lines for xch / BYC / DBX (divergence, tol, exposure) | | |
| h. Adjusting entries posted: asset, amount, ledger id, time; compared with the expectation from e | | |
| h. Any log line or closure event for the three ids | | |
| h. Checker `--after-boot`: PASS count / FAIL count, exit code | | |
| Completeness sweep after the start: result line, exit code, excluded ids; re-prove verdicts | | |
| Startup shortcut moved into `$Out` (setup) and put back (g.5); Windows Update paused and resumed | | |
| Branches taken, if any: exit 4 / exit 5 and f fails / ALREADY APPLIED / R1 exit 6 / after-boot FAIL; what was captured and decided | | |
| `trades_full.csv` regenerated at a later stop: date, old copy kept, the three ids absent | | |
| Any deviation from this runbook | | |
