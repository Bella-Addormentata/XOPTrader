#!/usr/bin/env python3
"""Repair the three phantom fills booked on 2026-09-22 (trade_log 1900-1902).

ONE-OFF.  Run it only as the operator step in RUNBOOK below: engine and GUI
stopped, in the same maintenance stop as the v0.10.26 install, BEFORE the
first start of v0.10.26 (whose first boot may post XCH/BYC auto-adjusts that
change the pre-state this script asserts).

WHAT HAPPENED
    Three offers were never taken.  Each lost one XCH maker coin to another of
    the bot's own transactions, which spent it paying a 15,000,000-mojo fee
    (blocks 9,324,680 / 9,325,004 / 9,325,694; CHANGELOG v0.10.26), so their
    maker coins could never all be spent in one block -- the proof v0.10.26
    requires of a take.  After the 2026-09-22 wallet resync the wallet called
    them CONFIRMED, and the engine booked them as fills:
        trade_log 1900  XCH/DBX ask  0xd6a8325c15...  offer_log 21185
        trade_log 1901  XCH/BYC bid  0xdb63709cb9...  offer_log 21311
        trade_log 1902  XCH/DBX ask  0x83eef9df80...  offer_log 21243
    PR #171 (v0.10.26) books a fill only on on-chain proof, so it cannot
    recur, but the bad records remain.  Wallet statuses read 2026-09-26:
    1900 CONFIRMED, 1901 CONFIRMED, 1902 CANCELLED (its second maker coin was
    spent at block 9,339,728 and reused into trade 1903's coins).

    Trade 1903 (0x476a29ba40..., booked 09-25) is a REAL take, proven on chain
    2026-09-26 with prove_fill_on_chain.py: both maker coins were spent
    together at block 9,340,539 and that block holds the settlement coin for
    exactly the 1 XCH offered.  This script does not touch it.  Its stored
    cost basis inherited an estimated ~0.3% contamination of the XCH basis
    from phantom 1901's lot: an accepted, documented residual.

WHAT THIS SCRIPT DOES (one transaction, all or nothing: 22 row changes)
    offer_closure_events  APPEND 6 rows, 2 per offer:
        - 'status_update' filled -> cancelled, reason 'dead_on_chain', at the
          dead spend height: v0.10.26's verdict for a dead offer (event type,
          reason and height), but with previous_status 'filled' -- a shape the
          engine itself never writes (on a 'filled' row update_offer_status
          only appends an observation), so it also identifies these three
          repair rows.  Its closure_reason is exactly 'dead_on_chain' (no
          repair tag); the correction row below carries the tag.
        - 'phantom_fill_correction' filled -> cancelled: why, what was
          reversed, and the full original trade_log row as JSON (the archive
          of the deleted row).  Mirrors the era remediation's
          'era_backfill_correction' events.
    ledger_entries        APPEND 10 rows, never UPDATE/DELETE (policy s.2):
        - 9 legs of event_type 'reversal', event_id 'reversal:<offer_id>', one
          per original fill leg (ids 2622-2630) with the sign flipped, same
          leg/asset/pair/block.  The fee legs are reversed too: an offer's
          creation fee is paid only at settlement (policy s.8) and these never
          settled.
        - 1 'adjust' leg, event_id 'correction:<event_id of 2641>', +203,188
          DBX.  Auto-adjust 2641 (-199,588 DBX, "unexplained") absorbed the
          +203,188 phantom DBX quote legs; reversing those legs without this
          would pull the DBX ledger 203,188 below the wallet it was tied to.
          2641 + this row = +3,600, the part of 2641 still unexplained, so
          SUM(adjust) stays an honest "unaccounted for" measure.
        Net ledger change: xch +896,841,101,167; BYC +1,864; DBX 0.
        Why 'reversal' and not 'adjust': the engine reads SUM(event_type =
        'adjust') as the "unaccounted for" measure, and S44(b) keeps
        known-wrong bookings apart from unexplained ones.  REVERSAL_EVENT_TYPE
        is NOT a one-constant switch: changing it changes the accounting
        treatment and needs its own policy amendment and review.  (The
        checker derives its SUM(adjust) expectation from this constant, so
        the two scripts cannot silently disagree about it.)
    offer_log             UPDATE 3 rows: status 'filled' -> 'cancelled',
        cancel_reason '' -> the original price_adverse cause (the engine's
        S14 rule for a cancel_pending row closed by a verdict).  Kept as is:
        resolved_block, which already holds the dead spend height (and was the
        wallet's confirmed_at_index when the fills were booked); and
        resolved_at, which holds the time the engine wrote the (wrong)
        'filled' verdict (2026-09-22 22:09:29 / 22:13:39), 21-27 hours after
        the dead spend -- NOT the time the offer left the book.  This differs
        from what v0.10.26 would record: its dead verdict stamps the time it
        is written (CURRENT_TIMESTAMP), which here would be the repair time.
        Keeping the engine's own last verdict time is deliberate (the design
        rewrites only status and cancel_reason); offer-lifetime analytics
        that read resolved_at are wrong for these three rows either way.
    trade_log             DELETE rows 1900-1902.  The engine rebuilds P&L
        from COUNT/SUM over every trade_log row at start (no status column),
        so a row that stays is a fill that stays.  The full rows are archived
        in the correction closure events above; sqlite_sequence stays at
        1903, so no id is reused.

WHAT IT DELIBERATELY DOES NOT TOUCH
    inventory_state, trade_log 1903, taker_fills, snapshots and the rollups,
    every row of every other offer, and anything outside SQLite
    (data/trade_history/*.csv).  inventory_state: Step 11 re-ties each
    asset's quantity to the wallet once per asset per engine process (after a
    20-block grace, from a Step 8 wallet balance at most 10 blocks old), so
    the quantities come right at the first boot; the phantom lots'
    contribution to total_cost cannot be reconstructed from the DB (policy:
    unknown cost is never fabricated).

SAFETY
    Paths (an ALLOW-LIST, checked on the path string before the file is
    touched):
      (a) the live database C:\\GitHub\\XOPTrader\\data\\xop_trader.db, which
          needs --i-stopped-the-engine and --backup-to (or --restore-from);
      (b) a copy under the verification scratch directory SCRATCH_REPAIR_DIR;
      (c) <dir>\\xop_trader.db where <dir> is named by the environment
          variable XOP_REPAIR_FAKE_LIVE_DIR, treated EXACTLY like the live
          database (flags, backup, process list, intent file, hash pins).  It
          exists only so verification can exercise the live branch on a copy,
          and must be a real subdirectory of the temp directory.
      Refused: \\\\?\\ and \\\\.\\ prefixes, UNC paths, 8.3 short names (~),
      alternate data streams, any other file under the live data directory,
      anything under C:\\GitHub\\XOPTrader-backups (backups are never
      repaired in place) or elsewhere in the live checkout, the baseline
      snapshot.db, and -- once the flags and the process list allow touching
      the file -- a path whose realpath differs from its spelling (junction,
      symlink, subst drive), a reparse point, or a hard link (st_nlink > 1),
      for the database and its -wal/-shm/-journal.  The SQLite URI is built
      from that canonical path, mode=rw (never creates a database).
    Processes (live and fake-live): refuses while any process runs that is
      xop_trader.exe or an XOPTrader GUI executable, runs from
      C:\\Program Files\\XOPTrader, or is a python/pythonw/py whose command
      line contains gui, _run_gui, gui.main, gui\\main, xoptrader, xop_trader
      or a DB-maintenance script name; a python whose command line cannot be
      read counts as a match; a process list that cannot be read, or that
      does not contain this process, refuses (fail closed).  The list and
      the file handles are checked again AFTER COMMIT (exit 4 if anything
      appeared).
    Handles: refuses when another process has the database, -wal or -shm
      open (a Windows no-share open fails with a sharing violation).
    Intent files (live and fake-live): refuses while data\\uncancelled.txt,
      data\\uncancelled.json (the engine's legacy name, still read at boot) or
      data\\uncancelled.txt.tmp (the engine's write-then-rename temporary; the
      engine never reads it, but a leftover one is checked too, fail closed)
      names one of the three offers.  What it detects: the 64-hex offer id,
      with or without 0x, in any letter case, anywhere in the file, with the
      bytes decoded as UTF-8, as UTF-16LE and as UTF-16BE (with or without a
      BOM); a hit in any of the three refuses.  Read only after the process
      list is clear, so the check never holds the file while the engine may
      rename onto it.
    Hash pins: prints the sha256 of the three scripts and checks them against
      SHA256SUMS in this directory (sha256 of each file with CRLF normalised
      to LF, so a git autocrlf checkout still matches); the live and
      fake-live databases are refused on any mismatch.  The pin is
      SHA256SUMS ONLY: git HEAD and whether this directory is dirty are
      printed for the record and never required (recording a commit on the
      same branch would itself move HEAD).  Every git call sets
      GIT_OPTIONAL_LOCKS=0, so it never writes the index.
    Transaction: BEGIN IMMEDIATE, then asserts the exact pre-state (engine-
      appended status_observation / reconcile_observation closure events are
      tolerated); any mismatch -> ROLLBACK, exit 2.  Takes the --backup-to
      copy (SQLite backup API, never a file copy) inside the write lock, then
      applies, verifies every post-condition inside the transaction, and only
      then COMMITs.  --dry-run rolls back instead and writes NO backup (it
      validates the --backup-to path), so the same command can then be run
      for real.  A COMMIT that raises is not guessed at: the database is
      re-read on a fresh connection, and exit 2 is returned only when the
      exact pre-state is still there (otherwise 5).
    Idempotent: a database already in the post-repair state exits 0 without
      changing anything (engine events appended after the repair -- the
      observation events, an S14 reopen_observation and an engine re-close
      of a reopened row, see ENGINE_RECLOSE_REASONS -- are tolerated there,
      and only there, and only in the order the engine can write them).
    Audit trail: everything printed also goes to a log file: <backup>.log
      (<backup>.dryrun.log for a dry run, <backup>.restore.log for
      --restore-from), or <db>.phantom_repair.log for a scratch copy without
      a backup.  After COMMIT it prints the sha256 of the scripts, the backup
      and the database.  Its last line is the exit code and its meaning.
    --list-blockers: read-only.  Runs the live run's own guards in the live
      run's order -- hash pins, the process list, then (only once no
      XOPTrader process runs) the intent files, the path aliasing checks and
      the open-handle test -- and prints every blocker it finds.  It never
      opens the database with SQLite and writes no file (no log, no backup);
      the handle test is the live run's momentary no-share open of the
      database, -wal and -shm, made only when no XOPTrader process runs.

EXIT CODES (the audit log's last line records the code)
    0  applied, already applied, restored, --dry-run passed and rolled back,
       or --list-blockers found nothing that would stop the live run
    1  refused: bad arguments, path guard, hash pins, intent file, a
       --restore-from precondition.  The database was not opened for
       writing (only the audit log may have been written); --list-blockers:
       the live run would refuse with 1 (pins, intent file, path aliasing)
    2  NOT COMMITTED, NOTHING WRITTEN: state mismatch, a failed
       in-transaction check, or an error before COMMIT (rolled back); a
       COMMIT that raised and a re-read that still finds the exact
       pre-state; a --restore-from that failed before its file swap, or
       whose swap failed and was undone automatically (the database is
       exactly as it was).  Only this.
    3  busy, nothing written: an XOPTrader process runs, a file is held open,
       the process list cannot be read, or the database is locked (a
       --restore-from removes its temporary replacement first);
       --list-blockers: a process or open handle would stop the live run
    4  COMMITTED, and the re-check after COMMIT found an XOPTrader process or
       an open handle, or could not run: stop it (close the GUI), then run
       the checker before anything else.  4 wins over 5: a later failure
       never replaces it.
    5  COMMITTED; a post-COMMIT step failed (the read-back differs, an error
       or interruption after COMMIT, a failed summary, hash or audit-log
       write, or a COMMIT whose outcome a re-read could not confirm as
       rolled back).  The repair IS in the database.  Run the checker NOW;
       the rollback window is open (ROLLBACK below).
    6  --restore-from INCOMPLETE: the file swap began and could not be
       finished or undone, or the restored database failed verification.
       Start nothing; the output names where each file is and the exact
       moves that put the repaired database back.

RUNBOOK (operator, at the PC; one maintenance stop shared with the install)
    Beforehand
      a. The scripts are pinned by SHA256SUMS only.  The script checks it and
         refuses the live database on any mismatch; compare the hashes it
         prints (--print-hashes) with the execution record's table, and copy
         them and the printed HEAD into the record (HEAD is informational).
      b. PRECONDITION: docs/ACCOUNTING-POLICY.md s.2, db/schema.md and the
         database.hpp comment are amended for the 'reversal' event type and
         the correction of an auto-adjust, in the same change as the
         execution record (the policy changes with the treatment).
      c. The v0.10.26 installer is downloaded and hash-verified.  No reboot or
         log-off from step 1 to step 9: XOPTrader.lnk in the Startup folder
         launches the GUI at logon, and the GUI starts the engine.
    1. Close the GUI ONCE.  That is the graceful engine stop (PID-addressed
       shutdown.flag).  Answer the S74 keep/cancel-offers prompt on purpose:
       Keep avoids a fee-bearing cancel sweep; Cancel avoids unmanaged offers
       during the downtime.  The GUI does not respawn an exited engine.
       Never taskkill first.
    2. python apply_phantom_repair.py C:\\GitHub\\XOPTrader\\data\\xop_trader.db
           --list-blockers
       -> exit 0 ("RESULT   : nothing would stop the live run").  This is the
       real run's own detector, fail closed: besides xop_trader.exe and the
       GUI, any python/pythonw/py whose command line contains gui, xoptrader
       or xop_trader counts -- which includes every C:\\GitHub\\XOPTrader*
       worktree and Claude session paths such as ...\\C--GitHub-XOPTrader\\...
       -- as does a DB-maintenance script.  Close every process it lists
       (agent sessions included) and re-run it until it exits 0.  Exit 3 = a
       process or open handle; exit 1 = pins, an intent file (step 4) or a
       path alias.
    3. Read the wallet's confirmed XCH, DBX and BYC balances (the wallet
       service stays up).  Record each phantom's wallet status (read-only
       get_offer, e.g. prove_fill_on_chain.py <trade_id>).  2026-09-26: 1900
       CONFIRMED, 1901 CONFIRMED, 1902 CANCELLED.  If one reads
       PENDING_CANCEL, the first v0.10.26 boot will reopen that row (S14:
       reopen_observation, cancelled -> cancel_pending, resolved_block 0),
       and a later heartbeat will close it again (status_update
       cancel_pending -> cancelled, either 'dead_on_chain' once the fill
       proof finds it Dead or 'wallet reported terminal' once the wallet
       flips it to CANCELLED; resolved_at rewritten): write that expectation
       into the execution record.
    4. data\\uncancelled.txt, data\\uncancelled.json and
       data\\uncancelled.txt.tmp must not name the three ids (none existed on
       2026-09-26); the script refuses if one does (see SAFETY for exactly
       what it detects).  With the engine stopped, delete those lines (or a
       leftover .txt.tmp): the offers are proven dead and the repair records
       them closed.  Re-check after any Cancel All or cancelling stop made
       while one of them is back in State.
    5. Dry run: the step-6 command plus --dry-run.
    6. Real run:
         python apply_phantom_repair.py C:\\GitHub\\XOPTrader\\data\\xop_trader.db
             --i-stopped-the-engine
             --backup-to C:\\GitHub\\XOPTrader-backups\\xop_trader_pre_phantom_repair_<UTC>.db
       (--backup-to must be a new file outside the live checkout.)  Exit 0:
       step 7.  Exit 4: stop what appeared, then step 7, before anything
       else.  Exit 5: the repair IS committed; step 7 now.  Exit 1, 2 or 3:
       nothing was written to the database; fix the cause and re-run.
    7. python check_phantom_repair.py C:\\GitHub\\XOPTrader\\data\\xop_trader.db
           --baseline <that backup> --allow-live-readonly
       All PASS -> go on.  Any FAIL -> this is the rollback decision point
       (ROLLBACK below), still inside the window.
    8. Compare the step-3 wallet balances with the post-repair ledger sums the
       script printed (XCH 42,508,598,294,699 mojos on the 14:52 snapshot;
       the live figure differs by the real ledger movement since).  Expect
       the first v0.10.26 boot to post an 'unexplained' XCH auto-adjust of up
       to about 3.5 XCH once the book is empty (live config: auto_adjust
       true, pause false); 3 x 15,000,000 mojos of it are the fees the kill
       transactions paid (known; part of the cancel-fee gap).  The operator
       may run the first boot with auto_adjust_enabled: false until the gap
       is understood -- set it BEFORE step 9 (the installer's Launch starts
       the engine at once), edit config.yaml only with the GUI closed (a GUI
       config save clobbers disk edits), and set it back afterwards.
    9. Run the installer with Launch checked (if it was run earlier, with
       Launch unchecked).  Its PrepareToInstall runs taskkill /F; its
       postinstall launches the GUI.  If UAC times out BEFORE the installer
       reached PrepareToInstall (which runs the old version's uninstaller),
       relaunching the OLD GUI is acceptable ONLY if step 3 found no phantom
       reading PENDING_CANCEL: v0.10.25 has the same S14 reopen and no fill
       proof, so a reopened phantom that the wallet later reports CONFIRMED
       would be booked again.  Otherwise wait for v0.10.26.  THE ROLLBACK
       WINDOW CLOSES HERE.
   10. After the first heartbeats:
         python check_phantom_repair.py <live db> --baseline <backup>
             --allow-live-readonly --after-boot

ROLLBACK
    Window: from COMMIT to the first start of the engine or the GUI.  Inside
    it the database is exactly the --backup-to file plus these 22 row changes,
    and that backup is the ONLY rollback source.  (The 14:52 pre-install
    backup in C:\\GitHub\\XOPTrader-backups predates hours of real closure
    events, ledger legs, offers and trades.  Restoring it drops them, and
    neither v0.10.25 nor v0.10.26 adopts an untracked CONFIRMED trade, so a
    real take of an offer posted after 14:52 would never be booked.  Last
    resort only.)
    Restore, engine and GUI stopped:
        python apply_phantom_repair.py C:\\GitHub\\XOPTrader\\data\\xop_trader.db
            --i-stopped-the-engine --restore-from <the --backup-to file>
    It refuses unless the backup is the exact pre-state and the database is
    that backup plus exactly this repair (every other row identical).  It
    builds the replacement beside the database with the SQLite backup API
    (never a file copy: a leftover -wal would be replayed onto it) under a
    temporary name, verifies it, moves xop_trader.db and its -wal/-shm aside
    TOGETHER (siblings first) as xop_trader.db.rolledback-<UTC>, renames the
    replacement into place, and verifies it again (quick_check, exact
    pre-state).  Exit 0 = restored; 1 or 3 = refused, nothing touched (a
    BUSY refusal deletes its temporary xop_trader.db.restoring-<UTC> first);
    2 = failed before the swap or the swap was undone, the database is
    unchanged; 6 = INCOMPLETE, follow the printed moves before anything.
    After the window (the engine has run and written): corrections are
    forward-only; never restore a backup over it.  A forward un-repair, if
    ever needed, is a new reviewed change that appends in one transaction
    (event ids per docs/ACCOUNTING-POLICY.md s.2, "The rule applies to
    itself"):
      1. trade_log: re-insert 1900-1902 with their original ids from the JSON
         archived in each 'phantom_fill_correction' closure event (explicit
         ids below sqlite_sequence are allowed and these are unused);
      2. ledger_entries, never DELETE the repair rows:
         - per offer, event_type 'reversal', event_id
           'reversal:reversal:<offer_id>', one leg negating each 'reversal'
           leg of 'reversal:<offer_id>' (same leg/asset/pair/block);
         - one DBX leg, event_type 'adjust', leg 'adjust', event_id
           'correction:correction:<adjust event_id>' =
           'correction:correction:adjust:<DBX>:9330325', -203,188, negating
           'correction:adjust:<DBX>:9330325';
         - a 'correction:<event_id>' leg for every auto-adjust posted since
           the repair that absorbed repair legs (policy s.2, "Correcting an
           auto-adjust");
      3. offer_log: status back to 'filled', cancel_reason back to '';
      4. offer_closure_events: one event per offer recording the un-repair;
    after first checking that nothing the engine did since (an auto-adjust
    that absorbed the reversals, an S14 reopen) would make it double-count.

NOTES FOR THE EXECUTION RECORD
    - The 3,600-mojo DBX residual assumes adjust 2641 = the absorbed phantom
      quote legs (-203,188) + a real divergence (+3,600).  That rests on ONE
      wallet observation, not two: Step 8, the only writer of the cached
      wallet balances, sat at its sync gate until about 01:16 on 09-23 and
      Step 11 needs a balance at most 10 blocks old, so the tracker re-tie
      most likely happened around 01:17 from the same wallet read that
      posted 2641 (not at 09-22 22:15, as the first-pass design said).
      Whatever the true split, the correction leaves the DBX ledger balance
      unchanged; if the split is wrong, only the attribution inside SUM(adjust)
      is off, by at most 3,600 mojos -- under the DBX alert tolerance, whose
      percentage-of-balance term is about 9.7k mojos at the current balance
      (the floor_cat_mojos floor itself defaults to only 100).
    - Cost-basis residual (ESTIMATES, not measurements): about +$0.2, 0.3-0.4%
      of the XCH basis (reconstructed as -2*avg + 1.103*c_bid + 0.897*mark),
      and about 1% of the DBX basis.  The first-pass design called a +0.329%
      step between trades 1900 and 1902 "grounded"; it is not: their
      cost_basis_mojos are USD bases converted to DBX at each trade's time,
      so the step mixes DBX/USD drift with the phantom lot and leaves out
      Step 11's re-add at mark.  Trade 1903's stored cost basis and realized
      P&L carry the ~0.3% XCH contamination from phantom 1901.
    - The fee legs' reversal adds 45,000,000 XCH mojos to the ledger.  The
      kill transactions really paid those fees; they are unbooked (no ledger
      or taker_fills row at those heights), belong to the cancel-fee gap, and
      will surface inside the first XCH auto-adjust.  Immaterial.
    - GUI P&L history: database_service.fetch_pnl_history rebuilds realized
      from trade_log, so it retroactively loses the phantom realized P&L from
      09-22 22:09; total_usd comes from snapshots.pnl_total_usd, which keeps
      it.  From 09-22 22:09 to the repair, total minus realized is inflated
      by about $0.87, and total steps down at the restart.  No data change:
      snapshots are never read back.
    - Both appended closure events per offer have a closure_reason starting
      'dead_on_chain'.  A dead-offer audit (and the planned
      verify_fill_completeness.py exclusion) must count DISTINCT offer_id or
      filter on event_type = 'status_update', or it counts each offer twice.
    - Follow-ups outside this script: the dead_on_chain exclusion in
      scripts/verify_fill_completeness.py (it will otherwise report these
      three as missing CONFIRMED trades -- do not let that prompt a
      backfill); annotate data/trade_history/trades_live.csv and regenerate
      trades_full.csv; snapshots keep the phantom P&L (about +$0.87).

USAGE
    python apply_phantom_repair.py <db> [--dry-run] [--backup-to NEW_FILE]
    python apply_phantom_repair.py <live db> --i-stopped-the-engine
        --backup-to NEW_FILE [--dry-run]
    python apply_phantom_repair.py <db> [--i-stopped-the-engine]
        --restore-from BACKUP
    python apply_phantom_repair.py <db> --list-blockers
    python apply_phantom_repair.py --print-hashes | --write-hashes
"""

# ruff: noqa: S608, S603, S607, N818
# S608: only table/column identifiers and "?" placeholder lists are interpolated;
#       every value is bound.  S603/S607: fixed argument lists, no user input.

from __future__ import annotations

import argparse
import datetime as dt
import getpass
import hashlib
import json
import os
import re
import socket
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths and exit codes
# ---------------------------------------------------------------------------

LIVE_CHECKOUT = r"C:\GitHub\XOPTrader"
LIVE_DATA_DIR = LIVE_CHECKOUT + r"\data"
DB_NAME = "xop_trader.db"
LIVE_DB = LIVE_DATA_DIR + "\\" + DB_NAME
BACKUPS_DIR = r"C:\GitHub\XOPTrader-backups"
INSTALL_DIR = r"C:\Program Files\XOPTrader"
# The session scratch directory the repair was designed and verified in.  It
# is only an allow-list entry for verification copies; harmless if absent.
SCRATCH_REPAIR_DIR = (r"C:\Users\dorkm\AppData\Local\Temp\claude\C--GitHub-XOPTrader"
                      r"\c1d0f99c-3459-4370-aa6c-6cff8d6107c8\scratchpad\repair")
# The read-only 2026-09-26 14:52 snapshot of the live DB the design was made
# from: always refused as a repair target.
SNAPSHOT_DB = SCRATCH_REPAIR_DIR + r"\snapshot.db"
FAKE_LIVE_ENV = "XOP_REPAIR_FAKE_LIVE_DIR"
# Engine S46 cancel-intent files in the data directory (engine.cpp ~1094).
INTENT_FILES = ("uncancelled.txt", "uncancelled.json", "uncancelled.txt.tmp")

SCRIPT_PATH = Path(__file__).resolve()
SCRIPT_DIR = SCRIPT_PATH.parent
PINNED_FILES = ("apply_phantom_repair.py", "check_phantom_repair.py",
                "prove_fill_on_chain.py")
SUMS_NAME = "SHA256SUMS"

EXIT_OK = 0
EXIT_REFUSED = 1
EXIT_STATE = 2                      # NOT committed, nothing written -- only that
EXIT_BUSY = 3
EXIT_RACE = 4                       # committed; something appeared after COMMIT
EXIT_COMMITTED_UNVERIFIED = 5       # committed; a post-COMMIT step failed
EXIT_RESTORE_INCOMPLETE = 6         # --restore-from's swap neither finished nor undone
EXIT_MEANING = {
    EXIT_OK: "OK",
    EXIT_REFUSED: "REFUSED (the database was not opened for writing)",
    EXIT_STATE: "NOT COMMITTED: rolled back or unchanged, nothing written",
    EXIT_BUSY: "BUSY: nothing written",
    EXIT_RACE: "COMMITTED, but something appeared after COMMIT (or the re-check could "
               "not run): stop it, then run the checker",
    EXIT_COMMITTED_UNVERIFIED: "COMMITTED; a post-commit step failed: run the checker "
                               "NOW; the rollback window is open",
    EXIT_RESTORE_INCOMPLETE: "RESTORE INCOMPLETE: start nothing; follow the printed moves",
}

# ---------------------------------------------------------------------------
# The incident, as recorded in the 2026-09-26 14:52 snapshot of the live DB
# ---------------------------------------------------------------------------

XCH = "xch"
DBX = "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"
BYC = "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac"

OFFER_D6 = "0xd6a8325c15af4fdb2674061afdd613ffb6c240be5fb0ef23aab6ddf31262d0d6"
OFFER_DB = "0xdb63709cb9b2c3794cbf1a57bb15f2d5c1830dca620b16ac36926d0d60c6c556"
OFFER_83 = "0x83eef9df80a4d5557422c4e6b1789c8d7504e7e83759a47a818c103b8099511c"
OFFERS = (OFFER_D6, OFFER_DB, OFFER_83)

# spend_height: the block where another of the bot's own transactions spent
# one maker coin paying a fee (CHANGELOG v0.10.26 "A fill is booked only when
# the chain shows the offer was taken"); it is also the value already stored
# in offer_log.resolved_block (the wallet's confirmed_at_index when the fills
# were booked), and the height v0.10.26 records for a dead offer.
PHANTOMS = (
    dict(offer_id=OFFER_D6, trade_log_id=1900, offer_log_id=21185, pair="XCH/DBX",
         spend_height=9324680, cancel_cause="price_adverse(1.648%)",
         ledger_ids=(2622, 2623, 2624)),
    dict(offer_id=OFFER_DB, trade_log_id=1901, offer_log_id=21311, pair="XCH/BYC",
         spend_height=9325694, cancel_cause="price_adverse(3.507%)",
         ledger_ids=(2625, 2626, 2627)),
    dict(offer_id=OFFER_83, trade_log_id=1902, offer_log_id=21243, pair="XCH/DBX",
         spend_height=9325004, cancel_cause="price_adverse(2.247%)",
         ledger_ids=(2628, 2629, 2630)),
)
TRADE_LOG_IDS = tuple(p["trade_log_id"] for p in PHANTOMS)
OFFER_LOG_IDS = tuple(p["offer_log_id"] for p in PHANTOMS)

TRADE_COLS = ("id", "timestamp", "trade_id", "pair_name", "side", "price_mojos",
              "size_mojos", "fee_mojos", "cost_basis_mojos", "realized_pnl_mojos",
              "block_height", "offer_hash", "acquisition_ts", "created_at")
EXPECTED_TRADES = {
    1900: (1900, "2026-09-22T22:09:29.734Z", OFFER_D6, "XCH/DBX", "ask",
           102976000000000, 1000000000000, 15000000, 79026193350524, 23950,
           9324680, OFFER_D6, None, "2026-09-22 22:09:29"),
    1901: (1901, "2026-09-22T22:09:29.734Z", OFFER_DB, "XCH/BYC", "bid",
           1689624195465, 1103203898833, 15000000, 0, 0,
           9325694, OFFER_DB, None, "2026-09-22 22:09:29"),
    1902: (1902, "2026-09-22T22:13:39.803Z", OFFER_83, "XCH/DBX", "ask",
           100212000000000, 1000000000000, 15000000, 79286343671647, 20926,
           9325004, OFFER_83, None, "2026-09-22 22:13:39"),
}

LEDGER_COLS = ("id", "entry_time", "event_type", "event_id", "leg", "asset_id",
               "delta_mojos", "pair_name", "block_height", "note", "created_at")
_T1 = "2026-09-22T22:09:29.734Z"
_T2 = "2026-09-22T22:13:39.803Z"
_C1 = "2026-09-22 22:09:29"
_C2 = "2026-09-22 22:13:39"
EXPECTED_FILL_LEGS = {
    2622: (2622, _T1, "fill", OFFER_D6, "base", XCH, -1000000000000, "XCH/DBX", 9324680, "", _C1),
    2623: (2623, _T1, "fill", OFFER_D6, "quote", DBX, 102976, "XCH/DBX", 9324680, "", _C1),
    2624: (2624, _T1, "fill", OFFER_D6, "fee", XCH, -15000000, "XCH/DBX", 9324680, "", _C1),
    2625: (2625, _T1, "fill", OFFER_DB, "base", XCH, 1103203898833, "XCH/BYC", 9325694, "", _C1),
    2626: (2626, _T1, "fill", OFFER_DB, "quote", BYC, -1864, "XCH/BYC", 9325694, "", _C1),
    2627: (2627, _T1, "fill", OFFER_DB, "fee", XCH, -15000000, "XCH/BYC", 9325694, "", _C1),
    2628: (2628, _T2, "fill", OFFER_83, "base", XCH, -1000000000000, "XCH/DBX", 9325004, "", _C2),
    2629: (2629, _T2, "fill", OFFER_83, "quote", DBX, 100212, "XCH/DBX", 9325004, "", _C2),
    2630: (2630, _T2, "fill", OFFER_83, "fee", XCH, -15000000, "XCH/DBX", 9325004, "", _C2),
}
ADJ_2641_ID = 2641
ADJ_2641_EVENT_ID = "adjust:" + DBX + ":9330325"
EXPECTED_ADJ_2641 = (2641, "2026-09-23T01:17:52.100Z", "adjust", ADJ_2641_EVENT_ID,
                     "adjust", DBX, -199588, "", 9330325,
                     "unexplained divergence reconciled to wallet",
                     "2026-09-23 01:17:52")
# Adjusts for these assets posted after the phantom legs.  Only 2641 is
# allowed: any other means the reconciler may already have absorbed the
# phantom XCH/BYC legs, and reversing them would double-count (S40, ef05578).
LAST_PHANTOM_LEDGER_ID = 2630
ALLOWED_LATER_ADJUSTS = (ADJ_2641_ID,)

OFFER_COLS = ("id", "offer_id", "pair_name", "side", "price_mojos", "size_mojos",
              "tier", "status", "created_block", "resolved_block", "created_at",
              "resolved_at", "fee_mojos", "cancel_reason", "book_best_bid",
              "book_best_ask", "competitiveness_score", "queue_ahead_mojos",
              "queue_ahead_score", "execution_quality_score")
EXPECTED_OFFERS_PRE = {
    21185: (21185, OFFER_D6, "XCH/DBX", "ask", 102976246523133, 1000000000000, 1,
            "filled", 9324678, 9324680, "2026-09-21 18:51:30", "2026-09-22 22:09:29",
            15000000, "", 83333400000000, 83438000000000, 1, 525000000000000, 1, 1),
    21243: (21243, OFFER_83, "XCH/DBX", "ask", 100212213548818, 1000000000000, 0,
            "filled", 9325003, 9325004, "2026-09-21 20:45:37", "2026-09-22 22:13:39",
            15000000, "", 83333400000000, 83438000000000, 1, 525000000000000, 1, 1),
    21311: (21311, OFFER_DB, "XCH/BYC", "bid", 1689031116604, 1103203898833, 2,
            "filled", 9325693, 9325694, "2026-09-22 00:39:30", "2026-09-22 22:09:29",
            15000000, "", 2817528243752, 2873940861145, 1, 16273, 9, 3),
}

CLOSURE_COLS = ("id", "offer_id", "pair_name", "event_type", "previous_status",
                "observed_status", "closure_reason", "resolved_block", "created_at",
                "fee_mojos")
EXPECTED_CLOSURE_OLD = {
    33049: (33049, OFFER_D6, "XCH/DBX", "status_update", "pending", "cancel_pending",
            "price_adverse(1.648%)", 9324693, "2026-09-21 18:58:24", None),
    33157: (33157, OFFER_83, "XCH/DBX", "status_update", "pending", "cancel_pending",
            "price_adverse(2.247%)", 9325015, "2026-09-21 20:49:15", None),
    33293: (33293, OFFER_DB, "XCH/BYC", "status_update", "pending", "cancel_pending",
            "price_adverse(3.507%)", 9325705, "2026-09-22 00:44:20", None),
    33533: (33533, OFFER_D6, "XCH/DBX", "status_update", "cancel_pending", "filled",
            "", 9324680, "2026-09-22 22:09:29", None),
    33534: (33534, OFFER_DB, "XCH/BYC", "status_update", "cancel_pending", "filled",
            "", 9325694, "2026-09-22 22:09:29", None),
    33535: (33535, OFFER_83, "XCH/DBX", "status_update", "cancel_pending", "filled",
            "", 9325004, "2026-09-22 22:13:39", None),
}
LAST_OLD_CLOSURE_ID = 33535
# update_offer_status on a 'filled' row can only append one of these (it never
# changes the row), so an engine run before the repair may add them: harmless.
# This is the ONLY tolerance in the pre-state, which is otherwise exact.
TOLERATED_PRE_CLOSURE_TYPES = ("status_observation", "reconcile_observation")

# ---------------------------------------------------------------------------
# The repair rows (deterministic; no dates in the text, so a re-run can match)
# ---------------------------------------------------------------------------

REPAIR_TAG = "phantom-fill repair of trade_log 1900-1902 (follow-up to PR #171, v0.10.26)"
# Not 'adjust' -- see the module docstring.  check_phantom_repair.py imports
# this constant and derives its SUM(adjust) expectation from it.
REVERSAL_EVENT_TYPE = "reversal"
CORRECTION_EVENT_TYPE = "adjust"
CORRECTION_EVENT_ID = "correction:" + ADJ_2641_EVENT_ID
CLOSURE_VERDICT_TYPE = "status_update"
CLOSURE_VERDICT_REASON = "dead_on_chain"
CLOSURE_CORRECTION_TYPE = "phantom_fill_correction"

DBX_PHANTOM_TOTAL = sum(r[6] for r in EXPECTED_FILL_LEGS.values() if r[5] == DBX)   # 203188
EXPECTED_LEDGER_NET = {XCH: 896841101167, DBX: 0, BYC: 1864}
EXPECTED_ROW_CHANGES = 3 + 10 + 3 + 6      # deletes + ledger + offer_log + closure
N_LEDGER_ROWS = 10
N_CLOSURE_ROWS = 6


def reversal_event_id(offer_id: str) -> str:
    return "reversal:" + offer_id


def _death(height: int) -> str:
    # Accurate for all three whatever happened to the other maker coins later
    # (1902's second coin was spent at 9,339,728, reused into trade 1903).
    return (f"one maker coin was spent at block {height} by another of the bot's own "
            f"transactions paying a fee, so the maker coins were never all spent in "
            f"one block and the offer was never taken")


def expected_ledger_repair_rows() -> list[tuple]:
    """(event_type, event_id, leg, asset_id, delta_mojos, pair_name, block_height, note)."""
    out = []
    for p in PHANTOMS:
        for lid in p["ledger_ids"]:
            _id, _t, _et, ev, leg, asset, delta, pair, blk, _n, _c = EXPECTED_FILL_LEGS[lid]
            note = (f"reverses ledger_entries id={lid} (fill {ev} leg={leg}); "
                    f"dead_on_chain: {_death(p['spend_height'])}; booked as a fill "
                    f"without on-chain proof (fixed by PR #171, v0.10.26); trade_log "
                    f"id={p['trade_log_id']} voided; {REPAIR_TAG}")
            out.append((REVERSAL_EVENT_TYPE, reversal_event_id(ev), leg, asset,
                        -delta, pair, blk, note))
    dbx_ids = [r[0] for r in EXPECTED_FILL_LEGS.values() if r[5] == DBX]
    note = (f"explains {-DBX_PHANTOM_TOTAL} of adjust ledger_entries id={ADJ_2641_ID} "
            f"({ADJ_2641_EVENT_ID}, {EXPECTED_ADJ_2641[6]}, 'unexplained divergence "
            f"reconciled to wallet'): that adjust absorbed the phantom fill quote legs "
            f"id={dbx_ids[0]} and id={dbx_ids[1]} (+{DBX_PHANTOM_TOTAL} DBX), now reversed "
            f"by events {reversal_event_id(OFFER_D6)} and {reversal_event_id(OFFER_83)}; "
            f"this entry backs the absorbed amount out so the DBX ledger stays tied to "
            f"the wallet, and leaves {EXPECTED_ADJ_2641[6] + DBX_PHANTOM_TOTAL:+d} of "
            f"id={ADJ_2641_ID} as the part still unexplained; {REPAIR_TAG}")
    out.append((CORRECTION_EVENT_TYPE, CORRECTION_EVENT_ID, "adjust", DBX,
                DBX_PHANTOM_TOTAL, "", EXPECTED_ADJ_2641[8], note))
    return out


def expected_closure_repair_rows() -> list[tuple]:
    """(offer_id, pair_name, event_type, previous_status, observed_status,
    closure_reason, resolved_block, fee_mojos), in insertion order."""
    out = []
    for p in PHANTOMS:
        h = p["spend_height"]
        out.append((p["offer_id"], p["pair"], CLOSURE_VERDICT_TYPE, "filled", "cancelled",
                    CLOSURE_VERDICT_REASON, h, None))
        archived = json.dumps(dict(zip(TRADE_COLS, EXPECTED_TRADES[p["trade_log_id"]],
                                       strict=True)),
                              separators=(",", ":"))
        ids = ",".join(str(i) for i in p["ledger_ids"])
        reason = (f"dead_on_chain; phantom fill voided: the wallet reported this offer "
                  f"CONFIRMED after the 2026-09-22 resync, but {_death(h)}; it was booked "
                  f"as a fill without on-chain proof (fixed by PR #171, v0.10.26); "
                  f"offer_log set back to cancelled keeping the cancel cause "
                  f"{p['cancel_cause']}; ledger fill legs id={ids} reversed by event "
                  f"{reversal_event_id(p['offer_id'])}; trade_log id={p['trade_log_id']} "
                  f"deleted, original row: {archived}; {REPAIR_TAG}")
        out.append((p["offer_id"], p["pair"], CLOSURE_CORRECTION_TYPE, "filled", "cancelled",
                    reason, h, None))
    return out


def expected_offers_post() -> dict[int, tuple]:
    out = {}
    for p in PHANTOMS:
        row = list(EXPECTED_OFFERS_PRE[p["offer_log_id"]])
        row[OFFER_COLS.index("status")] = "cancelled"
        row[OFFER_COLS.index("cancel_reason")] = p["cancel_cause"]
        out[p["offer_log_id"]] = tuple(row)
    return out


def expected_adjust_kpi_delta() -> dict[str, int]:
    """Change of SUM(delta) over event_type='adjust', per asset, derived from
    the repair rows' own event types (so REVERSAL_EVENT_TYPE drives it)."""
    d: dict[str, int] = {}
    for et, _ev, _leg, asset, delta, _pair, _blk, _note in expected_ledger_repair_rows():
        if et == "adjust":
            d[asset] = d.get(asset, 0) + delta
    return d


# Engine events that may follow the repair rows for these offers (v0.10.26):
#   - observations (update_offer_status on a row it does not change);
#   - the S14 boot reopen of a 'cancelled' row the wallet reports
#     PENDING_CANCEL (database.cpp reopen_cancelled_as_cancel_pending):
#     reopen_observation cancelled -> cancel_pending; the row becomes
#     cancel_pending, resolved_block 0, resolved_at NULL, cause kept;
#   - a re-close of that reopened row: update_offer_status(cancelled) on a
#     cancel_pending row (database.cpp, the S14 "wallet verdict completes a
#     submitted cancel" branch) writes status_update cancel_pending ->
#     cancelled, keeps the cause, stamps resolved_at, and stores the event's
#     resolved_block -- or, when that is 0, the first cancel-submit height
#     (kQueryCancelSubmitBlock).  Every engine writer of that transition is
#     listed; any other reason is NOT tolerated (fail closed).
ENGINE_REOPEN_TYPE = "reopen_observation"
ENGINE_RECLOSE_REASONS = (      # engine.cpp line numbers in v0.10.26
    CLOSURE_VERDICT_REASON,       # 'dead_on_chain': Step 2, the fill proof found it
                                  #   Dead (~5815)
    "wallet reported terminal",   # Step 2, the S25 terminal recheck: the wallet
                                  #   flipped it to CANCELLED (~6066)
    "s46_startup_db_leg",         # boot, S46: the wallet says CANCELLED/FAILED for a
                                  #   DB-pending row (~2668)
    "on_chain_reconcile",         # Step 8: the on-chain reconciler found it stale (~15431)
    "s46_intent_recovery",        # S46 cancel-intent recovery found it dead (~22660)
)
# The engine's kQueryCancelSubmitBlock (database.cpp): the resolved_block a
# re-close stores when its writer passes 0.
CANCEL_SUBMIT_SQL = """
    SELECT resolved_block FROM offer_closure_events
    WHERE offer_id = ?1 AND event_type = 'status_update'
      AND observed_status IN ('cancel_pending', 'cancelled')
      AND COALESCE(resolved_block, 0) > 0
    ORDER BY id ASC LIMIT 1
"""


def is_engine_reopen(r: tuple) -> bool:
    """r is a CLOSURE_COLS row."""
    return r[3] == ENGINE_REOPEN_TYPE and r[4] == "cancelled" and r[5] == "cancel_pending"


def is_engine_reclose(r: tuple) -> bool:
    return (r[3] == "status_update" and r[4] == "cancel_pending" and r[5] == "cancelled"
            and r[6] in ENGINE_RECLOSE_REASONS)


def is_tolerated_post_event(r: tuple) -> bool:
    """One engine event after the repair rows (order is checked by
    replay_engine_events)."""
    return r[3] in TOLERATED_PRE_CLOSURE_TYPES or is_engine_reopen(r) or is_engine_reclose(r)


def is_status_changing_post_event(r: tuple) -> bool:
    return is_engine_reopen(r) or is_engine_reclose(r)


def cancel_submit_block(con, offer_id: str) -> int:
    r = con.execute(CANCEL_SUBMIT_SQL, (offer_id,)).fetchone()
    return int(r[0]) if r and r[0] else 0


def replay_engine_events(events: list[tuple], submit_block: int
                         ) -> tuple[list[str], tuple | None]:
    """Replay one offer's tolerated engine events (CLOSURE_COLS rows after the
    repair rows, in id order) from the repaired 'cancelled' row, as the engine
    can write them: a reopen only from 'cancelled', a re-close only from
    'cancel_pending'.  Returns (problems, expected), expected None when no
    event changed the row, else (status, resolved_block, resolved_at_is_null,
    description) of the offer_log row the last change implies."""
    problems: list[str] = []
    state, expected = "cancelled", None
    for e in events:
        if is_engine_reopen(e):
            if state != "cancelled":
                problems.append(f"event {e[0]}: reopen_observation while the row is "
                                f"already {state}")
            state = "cancel_pending"
            expected = ("cancel_pending", 0, True,
                        f"REOPENED by the engine (S14 PENDING_CANCEL path, event {e[0]}), "
                        f"awaiting a re-close")
        elif is_engine_reclose(e):
            if state != "cancel_pending":
                problems.append(f"event {e[0]}: re-close '{e[6]}' of a row that is "
                                f"{state}, not reopened")
            state = "cancelled"
            blk = int(e[7] or 0) or submit_block
            expected = ("cancelled", blk, False,
                        f"re-closed by the engine '{e[6]}' (event {e[0]}) at {blk}")
        elif e[3] not in TOLERATED_PRE_CLOSURE_TYPES:
            problems.append(f"event {e[0]} is not a tolerated engine event: {e!r}")
    return problems, expected


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def qmarks(n: int) -> str:
    return ",".join("?" * n)


def rows(con: sqlite3.Connection, sql: str, args=()) -> list[tuple]:
    return [tuple(r) for r in con.execute(sql, args).fetchall()]


def select_cols(cols) -> str:
    return ", ".join(f'"{c}"' for c in cols)


ISO_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")


def utc_iso_now() -> str:
    now = dt.datetime.now(dt.UTC)
    return now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}Z"


def utc_stamp() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")


def file_sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def is_busy_error(ex: BaseException) -> bool:
    return isinstance(ex, sqlite3.OperationalError) and any(
        k in str(ex).lower() for k in ("locked", "busy"))


class Out:
    """print(), and the same line to the audit log once it is open (lines
    printed before that are buffered and written when it opens).  It never
    raises once open: a failed console or log write sets .failed (the apply
    refuses to COMMIT with it set, and after COMMIT it turns exit 0 into 5)
    and the output goes on through whichever channel still works."""

    def __init__(self) -> None:
        self._f = None
        self._buf: list[str] = []
        self.path: str | None = None
        self.failed: list[str] = []

    def __call__(self, msg: str = "") -> None:
        try:
            print(msg, flush=True)
        except Exception as ex:
            self.failed.append(f"console: {type(ex).__name__}: {ex}")
        if self._f is not None:
            try:
                self._f.write(msg + "\n")
                self._f.flush()
            except Exception as ex:
                self.failed.append(f"audit log {self.path}: {type(ex).__name__}: {ex}")
                try:
                    print(f"WARNING  : the audit log {self.path} could not be written "
                          f"({ex}); it is incomplete from here", file=sys.stderr,
                          flush=True)
                except Exception:  # noqa: S110 -- nothing left to report through
                    pass
                self._close_quietly()
        else:
            self._buf.append(msg)

    def _close_quietly(self) -> None:
        f, self._f = self._f, None
        try:
            if f is not None:
                f.close()
        except Exception as ex:
            self.failed.append(f"audit log close: {type(ex).__name__}: {ex}")

    def open(self, path: str) -> None:
        try:
            self._f = open(path, "a", encoding="utf-8", newline="\n")
            self.path = path
            self._f.write("\n" + "=" * 78 + "\n")
            for m in self._buf:
                self._f.write(m + "\n")
            self._f.flush()
        except OSError as ex:
            self._close_quietly()
            raise Refused(f"cannot write the audit log {path} ({ex})") from None
        self._buf.clear()
        self(f"log      : {path}")

    def close(self) -> None:
        self._close_quietly()

    def finish(self, code: int) -> int:
        """Record the final exit code (the audit log's last line) and close."""
        self(f"exit     : {code}  {EXIT_MEANING.get(code, '?')}")
        self.close()
        return code


# ---------------------------------------------------------------------------
# State classification
# ---------------------------------------------------------------------------

def _common(con, problems: list[str]) -> None:
    """Checks that hold identically before and after the repair."""
    # The original fill legs and adjust 2641 are never modified (append-only).
    got = rows(con, f"SELECT {select_cols(LEDGER_COLS)} FROM ledger_entries "
                    f"WHERE event_id IN ({qmarks(3)}) ORDER BY id", OFFERS)
    if got != [EXPECTED_FILL_LEGS[k] for k in sorted(EXPECTED_FILL_LEGS)]:
        problems.append(f"ledger fill legs for the 3 offers differ from the expected "
                        f"ids 2622-2630: {got!r}")
    got = rows(con, f"SELECT {select_cols(LEDGER_COLS)} FROM ledger_entries WHERE id = ?",
               (ADJ_2641_ID,))
    if got != [EXPECTED_ADJ_2641]:
        problems.append(f"ledger adjust id=2641 differs from the expected row: {got!r}")
    # No taker_fills row references these offers.
    got = rows(con, f"SELECT id FROM taker_fills WHERE trade_id IN ({qmarks(3)}) "
                    f"OR counterparty_offer_id IN ({qmarks(3)})", OFFERS + OFFERS)
    if got:
        problems.append(f"taker_fills rows reference the offers: {got!r}")
    # Closure events that must exist unchanged.
    got = rows(con, f"SELECT {select_cols(CLOSURE_COLS)} FROM offer_closure_events "
                    f"WHERE id IN ({qmarks(len(EXPECTED_CLOSURE_OLD))}) ORDER BY id",
               tuple(sorted(EXPECTED_CLOSURE_OLD)))
    if got != [EXPECTED_CLOSURE_OLD[k] for k in sorted(EXPECTED_CLOSURE_OLD)]:
        problems.append(f"closure events 33049/33157/33293/33533-33535 differ: {got!r}")


def _closure_extras(con) -> list[tuple]:
    """Closure events for the three offers other than the six old ones."""
    return rows(con, f"SELECT {select_cols(CLOSURE_COLS)} FROM offer_closure_events "
                     f"WHERE offer_id IN ({qmarks(3)}) AND id NOT IN "
                     f"({qmarks(len(EXPECTED_CLOSURE_OLD))}) ORDER BY id",
                OFFERS + tuple(sorted(EXPECTED_CLOSURE_OLD)))


def _is_tolerated_pre(r: tuple) -> bool:
    return r[3] in TOLERATED_PRE_CLOSURE_TYPES and r[0] > LAST_OLD_CLOSURE_ID


def check_pre(con) -> tuple[list[str], list[str]]:
    """The EXACT pre-repair state (the only tolerance: engine observation
    events on the still-'filled' rows)."""
    problems: list[str] = []
    notes: list[str] = []
    _common(con, problems)

    got = rows(con, f"SELECT {select_cols(TRADE_COLS)} FROM trade_log "
                    f"WHERE trade_id IN ({qmarks(3)}) OR offer_hash IN ({qmarks(3)}) "
                    f"OR id IN ({qmarks(3)}) ORDER BY id",
               OFFERS + OFFERS + TRADE_LOG_IDS)
    if got != [EXPECTED_TRADES[k] for k in sorted(EXPECTED_TRADES)]:
        problems.append(f"trade_log rows for the offers differ from the expected "
                        f"1900-1902: {got!r}")

    got = rows(con, f"SELECT {select_cols(OFFER_COLS)} FROM offer_log "
                    f"WHERE offer_id IN ({qmarks(3)}) ORDER BY id", OFFERS)
    if got != [EXPECTED_OFFERS_PRE[k] for k in sorted(EXPECTED_OFFERS_PRE)]:
        problems.append(f"offer_log rows differ from the expected 'filled' rows: {got!r}")
    for p in PHANTOMS:
        if EXPECTED_OFFERS_PRE[p["offer_log_id"]][OFFER_COLS.index("resolved_block")] \
                != p["spend_height"]:
            problems.append(f"offer_log {p['offer_log_id']}: resolved_block is not the "
                            f"dead spend height {p['spend_height']}")

    extra = _closure_extras(con)
    other = [r for r in extra if not _is_tolerated_pre(r)]
    if other:
        problems.append(f"unexpected closure events for the offers: {other!r}")
    for r in extra:
        if _is_tolerated_pre(r):
            notes.append(f"tolerated later observation event id={r[0]} "
                         f"({r[3]} {r[4]}->{r[5]} '{r[6]}')")

    repair_ids = tuple(sorted({r[1] for r in expected_ledger_repair_rows()}))
    got = rows(con, f"SELECT id, event_id FROM ledger_entries WHERE event_id IN "
                    f"({qmarks(len(repair_ids))})", repair_ids)
    if got:
        problems.append(f"repair ledger rows already exist: {got!r}")

    got = [r[0] for r in rows(
        con, "SELECT id FROM ledger_entries WHERE event_type = 'adjust' AND asset_id "
             "IN (?, ?, ?) AND id > ? ORDER BY id",
        (XCH, DBX, BYC, LAST_PHANTOM_LEDGER_ID))]
    if tuple(got) != ALLOWED_LATER_ADJUSTS:
        problems.append(
            f"adjust rows for xch/DBX/BYC after the phantom legs are {got}, expected only "
            f"{list(ALLOWED_LATER_ADJUSTS)}: the reconciler may have absorbed the phantom "
            f"legs since the design was made (precedent S40/ef05578: a correction it "
            f"already absorbed double-counts) -- re-plan before applying")
    return problems, notes


def _ledger_repair_rows_in_db(con) -> list[tuple]:
    repair_ids = tuple(sorted({r[1] for r in expected_ledger_repair_rows()}))
    return rows(con, f"SELECT {select_cols(LEDGER_COLS)} FROM ledger_entries WHERE "
                     f"event_id IN ({qmarks(len(repair_ids))}) ORDER BY id", repair_ids)


def check_post(con, tolerant: bool) -> tuple[list[str], list[str]]:
    """The post-repair state.  tolerant=False (inside the transaction, the
    read-back after COMMIT, and the restore precondition): exact, apart from
    engine observation events that predate the repair.  tolerant=True (only
    to recognise ALREADY APPLIED): also accepts the engine events v0.10.26 may
    append after the repair -- observations, an S14 reopen_observation and a
    re-close of the reopened row (ENGINE_RECLOSE_REASONS), in an order the
    engine can write them -- and the offer_log states they imply.  Callers
    that need one consistent view run it inside one transaction."""
    problems: list[str] = []
    notes: list[str] = []
    _common(con, problems)

    got = rows(con, f"SELECT id FROM trade_log WHERE trade_id IN ({qmarks(3)}) "
                    f"OR offer_hash IN ({qmarks(3)}) OR id IN ({qmarks(3)})",
               OFFERS + OFFERS + TRADE_LOG_IDS)
    if got:
        problems.append(f"trade_log still holds rows for the offers: {got!r}")

    # Ledger repair rows: all fields but id/entry_time/created_at must match.
    got = _ledger_repair_rows_in_db(con)
    want = sorted(expected_ledger_repair_rows(), key=repr)
    have = sorted(((r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9]) for r in got), key=repr)
    if have != want:
        problems.append(f"ledger repair rows differ from the expected {N_LEDGER_ROWS}: "
                        f"{got!r}")
    else:
        times = {r[1] for r in got}
        if len(times) != 1 or not ISO_RE.match(next(iter(times))):
            problems.append(f"ledger repair rows do not share one ISO entry_time: {times}")
        if min(r[0] for r in got) <= EXPECTED_ADJ_2641[0]:
            problems.append("ledger repair rows have ids below 2641 -- not appended")

    # Closure events: the six repair rows (matched by content), engine
    # observation events before them, and (tolerant only) engine events after.
    extra = _closure_extras(con)
    want_c = expected_closure_repair_rows()

    def key(r):
        return r[1:8] + (r[9],)

    repair = [r for r in extra if key(r) in want_c]
    post_by_offer: dict[str, list[tuple]] = {o: [] for o in OFFERS}
    if sorted((key(r) for r in repair), key=repr) != sorted(want_c, key=repr):
        problems.append(f"closure repair events differ from the expected "
                        f"{N_CLOSURE_ROWS}: {[r for r in extra if not _is_tolerated_pre(r)]!r}")
    else:
        lo, hi = min(r[0] for r in repair), max(r[0] for r in repair)
        if lo <= LAST_OLD_CLOSURE_ID:
            problems.append("closure repair events have ids below 33535 -- not appended")
        for r in extra:
            if r in repair:
                continue
            if r[0] < lo and _is_tolerated_pre(r):
                notes.append(f"tolerated later observation event id={r[0]} before the "
                             f"repair ({r[3]} {r[4]}->{r[5]})")
            elif r[0] > hi and tolerant and is_tolerated_post_event(r):
                post_by_offer[r[1]].append(r)
                notes.append(f"tolerated engine event id={r[0]} after the repair "
                             f"({r[3]} {r[4]}->{r[5]} '{r[6]}')")
            else:
                problems.append(f"unexpected closure event for the offers: {r!r}")

    # offer_log rows.
    post = expected_offers_post()
    got_rows = {r[0]: r for r in rows(
        con, f"SELECT {select_cols(OFFER_COLS)} FROM offer_log "
             f"WHERE offer_id IN ({qmarks(3)}) ORDER BY id", OFFERS)}
    if sorted(got_rows) != sorted(post):
        problems.append(f"offer_log rows for the offers: ids {sorted(got_rows)}, "
                        f"expected {sorted(post)}")
        return problems, notes
    i_status = OFFER_COLS.index("status")
    i_rblk = OFFER_COLS.index("resolved_block")
    i_rat = OFFER_COLS.index("resolved_at")
    for p in PHANTOMS:
        oid = p["offer_log_id"]
        got_r, want_r = got_rows[oid], post[oid]
        seq_problems, exp = replay_engine_events(post_by_offer.get(p["offer_id"], []),
                                                 cancel_submit_block(con, p["offer_id"]))
        problems += [f"offer_log {oid}: {x}" for x in seq_problems]
        if exp is None:
            if got_r != want_r:
                problems.append(f"offer_log {oid} differs from the expected repaired row: "
                                f"{got_r!r}")
            continue
        # The engine reopened and/or re-closed the row after the repair.
        free = (i_status, i_rblk, i_rat)
        if [v for i, v in enumerate(got_r) if i not in free] != \
                [v for i, v in enumerate(want_r) if i not in free]:
            problems.append(f"offer_log {oid} differs from the repaired row outside "
                            f"status/resolved_block/resolved_at: {got_r!r}")
            continue
        st, blk, rat_null, state = exp
        if (got_r[i_status] == st and got_r[i_rblk] == blk
                and (got_r[i_rat] is None) == rat_null):
            notes.append(f"offer_log {oid}: {state}")
        else:
            problems.append(f"offer_log {oid} is inconsistent with the engine's events "
                            f"after the repair ({state}): {got_r!r}")
    return problems, notes


# ---------------------------------------------------------------------------
# Measurements used to verify the transaction
# ---------------------------------------------------------------------------

def user_tables(con) -> list[str]:
    return [r[0] for r in rows(con, "SELECT name FROM sqlite_master WHERE type='table' "
                                    "ORDER BY name")]


def table_digest(con, table: str, where: str = "", args=()) -> tuple[int, str]:
    """(row count, sha256 over every row incl. rowid, in rowid order)."""
    h = hashlib.sha256()
    n = 0
    cur = con.execute(f'SELECT rowid, * FROM "{table}" {where} ORDER BY rowid', args)
    while True:
        batch = cur.fetchmany(20000)
        if not batch:
            break
        for r in batch:
            h.update(repr(tuple(r)).encode())
            h.update(b"\n")
        n += len(batch)
    return n, h.hexdigest()


def untouched_where(table: str, max_ledger: int, max_closure: int) -> tuple[str, tuple]:
    """The part of a table the repair must leave byte-identical."""
    if table == "trade_log":
        return f"WHERE rowid NOT IN ({qmarks(3)})", TRADE_LOG_IDS
    if table == "offer_log":
        return f"WHERE rowid NOT IN ({qmarks(3)})", OFFER_LOG_IDS
    if table == "ledger_entries":
        return "WHERE rowid <= ?", (max_ledger,)
    if table == "offer_closure_events":
        return "WHERE rowid <= ?", (max_closure,)
    return "", ()


def max_ids(con) -> tuple[int, int]:
    return (con.execute("SELECT COALESCE(MAX(id), 0) FROM ledger_entries").fetchone()[0],
            con.execute("SELECT COALESCE(MAX(id), 0) FROM offer_closure_events")
            .fetchone()[0])


def measure(con, max_ledger: int, max_closure: int) -> dict:
    m: dict = {}
    m["counts"] = {t: con.execute(f'SELECT COUNT(*) FROM "{t}"').fetchone()[0]
                   for t in user_tables(con)}
    m["seq"] = dict(rows(con, "SELECT name, seq FROM sqlite_sequence"))
    m["ledger"] = dict(rows(con, "SELECT asset_id, SUM(delta_mojos) FROM ledger_entries "
                                 "GROUP BY asset_id"))
    m["kpi"] = dict(rows(con, "SELECT asset_id, SUM(delta_mojos) FROM ledger_entries "
                              "WHERE event_type = 'adjust' GROUP BY asset_id"))
    m["pnl"] = {r[0]: r[1:] for r in rows(
        con, "SELECT pair_name, COUNT(*), COALESCE(SUM(realized_pnl_mojos),0), "
             "COALESCE(SUM(fee_mojos),0) FROM trade_log GROUP BY pair_name")}
    m["status"] = dict(rows(con, "SELECT status, COUNT(*) FROM offer_log GROUP BY status"))
    # Every row the repair must not touch, in the tables it does touch, plus
    # the small stores it must leave alone.  (check_phantom_repair.py hashes
    # every table, snapshots and strategy_quotes included.)
    m["untouched"] = {}
    for t in ("trade_log", "offer_log", "ledger_entries", "offer_closure_events",
              "inventory_state", "taker_fills"):
        where, args = untouched_where(t, max_ledger, max_closure)
        m["untouched"][t] = table_digest(con, t, where, args)
    m["schema"] = rows(con, "SELECT type, name, tbl_name, sql FROM sqlite_master "
                            "ORDER BY type, name")
    return m


def verify_transition(before: dict, after: dict, changes: int) -> list[str]:
    bad: list[str] = []
    if changes != EXPECTED_ROW_CHANGES:
        bad.append(f"total_changes {changes} != {EXPECTED_ROW_CHANGES}")
    want_counts = dict(before["counts"])
    want_counts["trade_log"] -= 3
    want_counts["ledger_entries"] += N_LEDGER_ROWS
    want_counts["offer_closure_events"] += N_CLOSURE_ROWS
    if after["counts"] != want_counts:
        bad.append(f"row counts {after['counts']} != expected {want_counts}")
    want_seq = dict(before["seq"])
    want_seq["ledger_entries"] += N_LEDGER_ROWS
    want_seq["offer_closure_events"] += N_CLOSURE_ROWS
    if after["seq"] != want_seq:
        bad.append(f"sqlite_sequence {after['seq']} != expected {want_seq}")
    for a in sorted(set(before["ledger"]) | set(after["ledger"])):
        d = after["ledger"].get(a, 0) - before["ledger"].get(a, 0)
        if d != EXPECTED_LEDGER_NET.get(a, 0):
            bad.append(f"ledger net change for {a[:12]} is {d}, expected "
                       f"{EXPECTED_LEDGER_NET.get(a, 0)}")
    want_kpi = expected_adjust_kpi_delta()
    for a in sorted(set(before["kpi"]) | set(after["kpi"]) | set(want_kpi)):
        d = after["kpi"].get(a, 0) - before["kpi"].get(a, 0)
        if d != want_kpi.get(a, 0):
            bad.append(f"SUM(adjust) change for {a[:12]} is {d}, expected "
                       f"{want_kpi.get(a, 0)}")
    want_pnl = {k: list(v) for k, v in before["pnl"].items()}
    for t in EXPECTED_TRADES.values():
        pair, realized, fee = t[3], t[9], t[7]
        want_pnl[pair][0] -= 1
        want_pnl[pair][1] -= realized
        want_pnl[pair][2] -= fee
    want_pnl = {k: tuple(v) for k, v in want_pnl.items() if v[0] > 0}
    if after["pnl"] != want_pnl:
        bad.append(f"per-pair trade_log aggregates {after['pnl']} != expected {want_pnl}")
    want_status = dict(before["status"])
    want_status["filled"] -= 3
    want_status["cancelled"] = want_status.get("cancelled", 0) + 3
    if after["status"] != want_status:
        bad.append(f"offer_log status counts {after['status']} != expected {want_status}")
    for t, dg in before["untouched"].items():
        if after["untouched"].get(t) != dg:
            bad.append(f"{t}: rows outside the repair changed ({dg} -> "
                       f"{after['untouched'].get(t)})")
    if after["schema"] != before["schema"]:
        bad.append("schema changed")
    return bad


# ---------------------------------------------------------------------------
# The repair itself
# ---------------------------------------------------------------------------

def apply_repair(con, entry_time: str) -> None:
    # 1. Closure events first: they archive the trade_log rows about to go.
    for r in expected_closure_repair_rows():
        con.execute("INSERT INTO offer_closure_events (offer_id, pair_name, event_type, "
                    "previous_status, observed_status, closure_reason, resolved_block, "
                    "fee_mojos) VALUES (?, ?, ?, ?, ?, ?, ?, ?)", r)
    # 2. Compensating ledger rows.  Plain INSERT, not OR IGNORE: a collision
    #    must fail loudly, never be skipped.
    for (et, ev, leg, asset, delta, pair, blk, note) in expected_ledger_repair_rows():
        con.execute("INSERT INTO ledger_entries (entry_time, event_type, event_id, leg, "
                    "asset_id, delta_mojos, pair_name, block_height, note) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (entry_time, et, ev, leg, asset, delta, pair, blk, note))
    # 3. offer_log: filled -> cancelled, restoring the cancel cause.
    for p in PHANTOMS:
        cur = con.execute("UPDATE offer_log SET status = 'cancelled', cancel_reason = ? "
                          "WHERE id = ? AND offer_id = ? AND status = 'filled' "
                          "AND resolved_block = ?",
                          (p["cancel_cause"], p["offer_log_id"], p["offer_id"],
                           p["spend_height"]))
        if cur.rowcount != 1:
            raise RuntimeError(f"offer_log update for {p['offer_log_id']} changed "
                               f"{cur.rowcount} rows")
    # 4. trade_log: delete the three phantom fills (archived in step 1).
    for p in PHANTOMS:
        cur = con.execute("DELETE FROM trade_log WHERE id = ? AND trade_id = ?",
                          (p["trade_log_id"], p["offer_id"]))
        if cur.rowcount != 1:
            raise RuntimeError(f"trade_log delete of {p['trade_log_id']} removed "
                               f"{cur.rowcount} rows")


# ---------------------------------------------------------------------------
# Guards: which file (allow-list), who else is using it, which script version
# ---------------------------------------------------------------------------

class Refused(Exception):
    def __init__(self, message: str, code: int = EXIT_REFUSED) -> None:
        super().__init__(message)
        self.code = code


def _nc(p: str) -> str:
    return os.path.normcase(p)


def _under(child: str, parent: str) -> bool:
    """child equals parent or lies inside it (both normcase'd absolute paths)."""
    try:
        return os.path.commonpath([child, parent]) == parent
    except ValueError:        # different drives
        return False


_DRIVE_ABS = re.compile(r"^[A-Za-z]:\\")


def plain_abspath(p: str, what: str) -> str:
    """The absolute path, after string-only checks.  Touches no file:
    os.path.abspath is GetFullPathNameW (pure string work plus the cwd)."""
    if not isinstance(p, str) or not p.strip() or "\x00" in p:
        raise Refused(f"{what}: empty or invalid path {p!r}")
    s = p.replace("/", "\\")
    if s.startswith("\\\\"):
        raise Refused(f"{what} {p!r}: \\\\?\\ and \\\\.\\ prefixes and UNC paths are "
                      f"refused; pass a plain drive-letter path")
    if "~" in s:
        raise Refused(f"{what} {p!r}: 8.3 short names (~) are refused; pass the long "
                      f"path")
    a = os.path.abspath(s)
    if a.startswith("\\\\") or "~" in a:
        raise Refused(f"{what} {p!r} resolves to {a!r} (the current directory is a UNC "
                      f"or short-name path); pass an absolute drive-letter path")
    if not _DRIVE_ABS.match(a) or ":" in a[2:] or any(c in a for c in '*?"<>|'):
        raise Refused(f"{what} {p!r} -> {a!r}: not a plain drive-letter path (alternate "
                      f"data streams and wildcards are refused)")
    return a


def fake_live_dir() -> str | None:
    """The verification-only stand-in for the live data directory, or None."""
    v = os.environ.get(FAKE_LIVE_ENV, "")
    if not v:
        return None
    a = plain_abspath(v, FAKE_LIVE_ENV)
    n = _nc(a)
    tmp = _nc(os.path.realpath(tempfile.gettempdir()))
    if not _under(n, tmp) or n == tmp:
        raise Refused(f"{FAKE_LIVE_ENV}={v!r} must name a subdirectory of the temp "
                      f"directory {tmp}: it exists only so verification can exercise the "
                      f"live branch on a copy")
    for bad in (LIVE_CHECKOUT, BACKUPS_DIR, INSTALL_DIR):
        b = _nc(bad)
        if _under(n, b) or _under(b, n):
            raise Refused(f"{FAKE_LIVE_ENV}={v!r} overlaps {bad}")
    if not os.path.isdir(a):
        raise Refused(f"{FAKE_LIVE_ENV}={v!r} is not an existing directory")
    if _nc(os.path.realpath(a)) != n:
        raise Refused(f"{FAKE_LIVE_ENV}={v!r} resolves to {os.path.realpath(a)!r} "
                      f"(junction or symlink)")
    return a


class Target:
    def __init__(self, path: str, kind: str, data_dir: str | None = None) -> None:
        self.path = path            # plain absolute path (string-validated only)
        self.kind = kind            # 'live' | 'fake-live' | 'scratch'
        self.data_dir = data_dir    # the (fake) live data directory

    @property
    def live_like(self) -> bool:
        return self.kind in ("live", "fake-live")


def classify_db_path(p: str, fake: str | None) -> Target:
    """The allow-list, on the path string alone (no file-system access)."""
    a = plain_abspath(p, "database path")
    n = _nc(a)
    if _under(n, _nc(BACKUPS_DIR)):
        raise Refused(f"{a} is under {BACKUPS_DIR}: backups and rollback sources are "
                      f"never repaired in place; repair a copy in {SCRATCH_REPAIR_DIR}")
    if n == _nc(SNAPSHOT_DB):
        raise Refused("that is the read-only baseline snapshot.db; repair a fresh copy")
    if _under(n, _nc(LIVE_DATA_DIR)):
        if n != _nc(LIVE_DB):
            raise Refused(f"{a}: under the live data directory only the live database "
                          f"{LIVE_DB} itself is accepted")
        return Target(a, "live", LIVE_DATA_DIR)
    if fake is not None and _under(n, _nc(fake)):
        fdb = os.path.join(fake, DB_NAME)
        if n != _nc(fdb):
            raise Refused(f"{a}: under {FAKE_LIVE_ENV} only {fdb} is accepted (it is "
                          f"treated exactly like the live database)")
        return Target(a, "fake-live", fake)
    if _under(n, _nc(LIVE_CHECKOUT)):
        raise Refused(f"{a} is inside the live checkout {LIVE_CHECKOUT} but is not the "
                      f"live database: not on the allow-list")
    s = _nc(SCRATCH_REPAIR_DIR)
    if _under(n, s) and n != s:
        return Target(a, "scratch")
    raise Refused(f"{a} is not on the allow-list: the live database {LIVE_DB} (with "
                  f"--i-stopped-the-engine), a copy under {SCRATCH_REPAIR_DIR}, or "
                  f"%{FAKE_LIVE_ENV}%\\{DB_NAME} for verification")


def _check_not_aliased(path: str, what: str) -> None:
    st = os.lstat(path)
    if stat.S_ISLNK(st.st_mode) or (getattr(st, "st_file_attributes", 0)
                                    & stat.FILE_ATTRIBUTE_REPARSE_POINT):
        raise Refused(f"{what} {path} is a symlink or reparse point")
    if not stat.S_ISREG(st.st_mode):
        raise Refused(f"{what} {path} is not a regular file")
    n = os.stat(path).st_nlink
    if n != 1:
        raise Refused(f"{what} {path} has {n} hard links: a hard link can alias the live "
                      f"database (and would ignore its -wal)")


def verify_on_disk(a: str, what: str) -> str:
    """The first file-system access to a candidate: exists, regular, not a
    link, spelled canonically (realpath == the path), one hard link; the same
    for its -wal/-shm/-journal.  Returns the canonical path."""
    try:
        os.lstat(a)
    except FileNotFoundError:
        raise Refused(f"{what} {a} does not exist (this script never creates a "
                      f"database)") from None
    except OSError as ex:
        raise Refused(f"{what} {a}: {ex}") from None
    _check_not_aliased(a, what)
    try:
        real = os.path.realpath(a, strict=True)
    except OSError as ex:
        raise Refused(f"{what} {a}: cannot resolve its canonical path ({ex})") from None
    if _nc(real) != _nc(a):
        raise Refused(f"{what} {a} resolves to {real}: a junction, symlink, subst drive "
                      f"or short name is in the path; pass the canonical path")
    for sfx in ("-wal", "-shm", "-journal"):
        if os.path.lexists(a + sfx):
            _check_not_aliased(a + sfx, what + sfx)
    return real


def validate_new_file(p: str, what: str, target: Target, fake: str | None) -> str:
    """A file this script will create (the backup), or whose name it derives
    a log from.  Outside the live checkout and the fake live dir; must not
    exist; its directory must exist and be spelled canonically."""
    a = plain_abspath(p, what)
    n = _nc(a)
    for bad, why in ((LIVE_CHECKOUT, "the live checkout (keep the backup outside it, "
                                     "e.g. in " + BACKUPS_DIR + ")"),
                     (fake, FAKE_LIVE_ENV)):
        if bad and _under(n, _nc(bad)):
            raise Refused(f"{what} {a} is inside {why}")
    if n in (_nc(SNAPSHOT_DB), _nc(target.path)):
        raise Refused(f"{what} {a} is the database or the snapshot itself")
    parent = os.path.dirname(a)
    if not os.path.isdir(parent):
        raise Refused(f"{what}: the directory {parent} does not exist")
    if _nc(os.path.realpath(parent)) != _nc(parent):
        raise Refused(f"{what}: {parent} resolves to {os.path.realpath(parent)} "
                      f"(junction, symlink or short name)")
    for sfx in ("", "-wal", "-shm", "-journal"):
        if os.path.lexists(a + sfx):
            raise Refused(f"{what} {a + sfx} already exists (pass a new file name)")
    return a


def validate_restore_source(p: str, target: Target, fake: str | None) -> str:
    a = plain_abspath(p, "--restore-from")
    n = _nc(a)
    if _under(n, _nc(LIVE_DATA_DIR)) or (fake and _under(n, _nc(fake))):
        raise Refused(f"--restore-from {a} is inside the (fake) live data directory")
    if n == _nc(target.path):
        raise Refused("--restore-from is the database itself")
    real = verify_on_disk(a, "--restore-from")
    wal = a + "-wal"
    if os.path.exists(wal) and os.path.getsize(wal) > 0:
        raise Refused(f"{wal} is not empty: the backup is not self-contained")
    return real


def file_held_open(path: str) -> str | None:
    """None when no other process has the file open; otherwise why not.
    Opens the file with no sharing: Windows refuses (error 32) while any other
    handle exists -- the engine, the GUI's read-only connection, a sqlite3
    shell, a maintenance script."""
    import ctypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                ctypes.c_void_p]
    k32.CreateFileW.restype = ctypes.c_void_p
    k32.CloseHandle.argtypes = [ctypes.c_void_p]
    generic_read, open_existing, attr_normal = 0x80000000, 3, 0x80
    h = k32.CreateFileW(path, generic_read, 0, None, open_existing, attr_normal, None)
    if h is None or h == ctypes.c_void_p(-1).value:
        err = ctypes.get_last_error()
        if err == 32:
            return "another process has the file open (sharing violation)"
        if err == 2:
            return None                  # vanished in between: nobody has it
        return f"could not open it exclusively (Windows error {err})"
    k32.CloseHandle(h)
    return None


def held_files(db_path: str, attempts: int = 4, pause: float = 0.5) -> list[str]:
    """Files among the database, -wal and -shm that another handle holds.  A
    hit is re-checked a few times, so a momentary scanner handle (antivirus
    on a just-closed file) is not reported; a real holder persists."""
    out: list[str] = []
    for i in range(attempts):
        out = []
        for p in (db_path, db_path + "-wal", db_path + "-shm"):
            if os.path.exists(p):
                why = file_held_open(p)
                if why:
                    out.append(f"{p}: {why}")
        if not out:
            return out
        if i + 1 < attempts:
            time.sleep(pause)
    return out


_PY_NAME = re.compile(r"^(pythonw?[0-9.]*|pyw?)\.exe$")
XOP_EXE_NAMES = ("xop_trader.exe", "xop_trader_gui.exe", "xoptrader-gui.exe",
                 "xoptrader_gui.exe")
PY_CMD_MARKERS = ("gui", "_run_gui", "gui.main", "gui\\main", "gui/main", "xoptrader",
                  "xop_trader", "scheduled_db_maintenance", "maintain_snapshot_rollups")


def list_processes() -> list[dict]:
    """Every process (Win32_Process).  Raises when the list cannot be read or
    cannot be trusted; callers fail closed."""
    ps = ("[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
          "Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId,Name,"
          "ExecutablePath,CommandLine | ConvertTo-Json -Compress")
    exe = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"),
                       r"System32\WindowsPowerShell\v1.0\powershell.exe")
    r = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-Command", ps],
                       capture_output=True, timeout=180)
    if r.returncode != 0:
        raise RuntimeError(f"powershell exited {r.returncode}: "
                           f"{r.stderr.decode('utf-8', 'replace')[:300]}")
    data = json.loads(r.stdout.decode("utf-8-sig", "replace"))
    if isinstance(data, dict):
        data = [data]
    if not isinstance(data, list) or len(data) < 5:
        raise RuntimeError("implausibly short process list")
    if os.getpid() not in {p.get("ProcessId") for p in data if isinstance(p, dict)}:
        raise RuntimeError("the process list does not include this process")
    return data


EXCLUDED_PIDS: list[int] = []     # set by xoptrader_processes(), for --list-blockers


def xoptrader_processes() -> list[str]:
    data = list_processes()
    by_pid = {p.get("ProcessId"): p for p in data}
    # Exclude this process and its python/py launcher ancestors only (a venv
    # launcher or py.exe carries the same script path in its command line).
    mine = {os.getpid()}
    pid = by_pid[os.getpid()].get("ParentProcessId")
    while pid in by_pid and pid not in mine and \
            _PY_NAME.match((by_pid[pid].get("Name") or "").lower()):
        mine.add(pid)
        pid = by_pid[pid].get("ParentProcessId")
    EXCLUDED_PIDS[:] = sorted(mine)
    install = _nc(INSTALL_DIR)
    hits = []
    for p in data:
        pid = p.get("ProcessId")
        if pid in mine:
            continue
        name = (p.get("Name") or "").lower()
        exe = p.get("ExecutablePath") or ""
        cl = p.get("CommandLine")
        why = None
        if name in XOP_EXE_NAMES:
            why = "XOPTrader executable"
        elif exe and _under(_nc(exe), install):
            why = f"runs from {INSTALL_DIR}"
        elif _PY_NAME.match(name):
            if cl is None:
                why = "python process whose command line cannot be read (fail closed)"
            else:
                m = [k for k in PY_CMD_MARKERS if k in cl.lower()]
                if m:
                    why = f"python command line contains {m[0]!r}"
        if why:
            hits.append(f"pid {pid} {name} ({why}): {cl or exe}")
    return hits


INTENT_DECODINGS = ("utf-8", "utf-16-le", "utf-16-be")


def _intent_primary_text(raw: bytes) -> tuple[str, str]:
    """(encoding the file most likely uses, its text) -- for the line count."""
    if raw[:2] == b"\xff\xfe":
        return "UTF-16LE (BOM)", raw[2:].decode("utf-16-le", "replace")
    if raw[:2] == b"\xfe\xff":
        return "UTF-16BE (BOM)", raw[2:].decode("utf-16-be", "replace")
    if raw[:3] == b"\xef\xbb\xbf":
        return "UTF-8 (BOM)", raw[3:].decode("utf-8", "replace")
    return "UTF-8", raw.decode("utf-8", "replace")


def intent_file_hits(data_dir: str) -> tuple[list[str], list[str]]:
    """(files present, the offers each names).  Detects the 64-hex offer id
    (with or without 0x, any letter case, anywhere in the file) in the bytes
    decoded as UTF-8, UTF-16LE and UTF-16BE, BOM or not; a hit in any one of
    the three decodings counts.  Any read error raises Refused (fail closed)."""
    seen, hits = [], []
    for name in INTENT_FILES:
        p = os.path.join(data_dir, name)
        try:
            if not os.path.lexists(p):
                continue
            raw = Path(p).read_bytes()
        except OSError as ex:
            raise Refused(f"could not read {p} ({ex})") from None
        enc, primary = _intent_primary_text(raw)
        texts = [raw.decode(e, "replace").lower() for e in INTENT_DECODINGS]
        seen.append(f"{p} ({len(raw)} bytes, {enc}, {len(primary.splitlines())} lines)")
        for o in OFFERS:
            found = [e for e, t in zip(INTENT_DECODINGS, texts, strict=True)
                     if o[2:].lower() in t]
            if found:
                hits.append(f"{p} names {o} (decoded as {'/'.join(found)})")
    return seen, hits


# -- hash pins ---------------------------------------------------------------

def _pin_hashes(path: Path) -> tuple[str, str]:
    """(sha256 of the bytes, sha256 with CRLF normalised to LF)."""
    b = path.read_bytes()
    return hashlib.sha256(b).hexdigest(), hashlib.sha256(b.replace(b"\r\n", b"\n")).hexdigest()


def read_pins() -> dict[str, str] | None:
    p = SCRIPT_DIR / SUMS_NAME
    if not p.exists():
        return None
    raw = p.read_bytes()
    txt = raw.decode("utf-16") if raw[:2] in (b"\xff\xfe", b"\xfe\xff") \
        else raw.decode("utf-8-sig")
    pins = {}
    for line in txt.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) == 2 and re.fullmatch(r"[0-9a-f]{64}", parts[0]):
            pins[parts[1].lstrip("*")] = parts[0]
    return pins


def pin_report() -> tuple[bool, list[str]]:
    pins = read_pins()
    ok = pins is not None
    lines = [] if pins is not None else [f"pins     : {SCRIPT_DIR / SUMS_NAME} not found"]
    for name in PINNED_FILES:
        f = SCRIPT_DIR / name
        if not f.exists():
            ok = False
            lines.append(f"sha256   : {name} MISSING next to this script")
            continue
        raw, lf = _pin_hashes(f)
        pinned = (pins or {}).get(name)
        if pinned == lf:
            status = "matches SHA256SUMS"
        else:
            ok = False
            status = "NOT IN SHA256SUMS" if pinned is None else f"MISMATCH, pinned {pinned}"
        lines.append(f"sha256   : {name} lf={lf} raw={raw} [{status}]")
    return ok, lines


def write_pins() -> str:
    lines = ["# sha256 of each file with CRLF normalised to LF (stable across a git",
             "# autocrlf checkout).  Regenerate after a REVIEWED change with:",
             "#   python apply_phantom_repair.py --write-hashes"]
    for name in PINNED_FILES:
        lines.append(f"{_pin_hashes(SCRIPT_DIR / name)[1]}  {name}")
    path = SCRIPT_DIR / SUMS_NAME
    path.write_bytes(("\n".join(lines) + "\n").encode("ascii"))
    return str(path)


def git_env() -> dict[str, str]:
    """Every git call: GIT_OPTIONAL_LOCKS=0, so `git status` never takes or
    refreshes the index (the worktree's gitdir lives inside the live
    checkout's .git)."""
    return dict(os.environ, GIT_OPTIONAL_LOCKS="0")


def git_state() -> str:
    """For the record only: nothing requires HEAD to equal anything (the pin
    is SHA256SUMS)."""
    try:
        head = subprocess.run(["git", "-C", str(SCRIPT_DIR), "rev-parse", "HEAD"],
                              capture_output=True, text=True, timeout=30, env=git_env())
        if head.returncode != 0:
            return "not a git checkout"
        dirty = subprocess.run(["git", "-C", str(SCRIPT_DIR), "status", "--porcelain",
                                "--", "."], capture_output=True, text=True, timeout=30,
                               env=git_env())
        return (f"HEAD {head.stdout.strip()}, this directory "
                f"{'has uncommitted changes' if dirty.stdout.strip() else 'clean'} "
                f"(informational; the pin is SHA256SUMS)")
    except Exception as ex:           # informational only
        return f"unavailable ({type(ex).__name__})"


# ---------------------------------------------------------------------------
# SQLite plumbing
# ---------------------------------------------------------------------------

def sqlite_uri(canonical_path: str, mode: str, immutable: bool = False) -> str:
    u = Path(canonical_path).as_uri() + f"?mode={mode}"
    return u + "&immutable=1" if immutable else u


def header_journal_mode(path: str) -> str:
    """From the file header (bytes 18/19 are 2 for WAL); an immutable
    connection would report 'delete' for a WAL-mode file."""
    with open(path, "rb") as f:
        head = f.read(100)
    return "wal" if len(head) == 100 and head[18] == 2 and head[19] == 2 else "rollback"


def verify_pre_copy(path: str, what: str) -> str:
    """quick_check and the exact pre-state on a closed, self-contained copy
    (read as immutable, so no -wal/-shm is created beside it; callers make
    sure no non-empty -wal belongs to it).  Returns its journal mode."""
    chk = sqlite3.connect(sqlite_uri(path, "ro", immutable=True), uri=True)
    try:
        qc = chk.execute("PRAGMA quick_check").fetchone()[0]
        if qc != "ok":
            raise RuntimeError(f"{what} {path} failed quick_check: {qc}")
        problems, _ = check_pre(chk)
        if problems:
            raise RuntimeError(f"{what} {path} is not in the pre-repair state: {problems}")
    finally:
        chk.close()
    return header_journal_mode(path)


def take_backup(src_real: str, dest: str) -> None:
    """SQLite backup API (never a file copy: the DB runs in WAL mode), read
    through its own mode=ro connection -- which may read while the caller
    holds the write lock -- then checked."""
    src = sqlite3.connect(sqlite_uri(src_real, "ro"), uri=True)
    dst = sqlite3.connect(sqlite_uri(dest, "rwc"), uri=True)
    try:
        src.backup(dst)
    finally:
        dst.close()
        src.close()
    verify_pre_copy(dest, "backup")


# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------

class Result:
    """What do_apply did.  main() creates it and passes it in, so an
    interruption anywhere still tells main whether COMMIT was attempted or
    happened (the exit code depends on nothing else)."""

    def __init__(self) -> None:
        self.code = EXIT_STATE
        self.stage = "start"
        self.commit_attempted = False
        self.committed = False          # True once the repair is (treated as) committed
        self.entry_time = ""
        self.changes = 0
        self.before: dict = {}
        self.after: dict = {}
        self.race: list[str] = []            # after_commit's re-check: exit 4, never masked
        self.post_failures: list[str] = []   # failed post-COMMIT steps: exit 5


def final_apply_code(res: Result) -> int:
    """The exit code of a run that COMMITTED: 4 wins over 5, 5 over 0."""
    if res.race:
        return EXIT_RACE
    if res.post_failures or res.code != EXIT_OK:
        return EXIT_COMMITTED_UNVERIFIED
    return EXIT_OK


def commit_outcome(real: str) -> str:
    """After a COMMIT that raised (the connection closed): 'pre' when a fresh
    connection still sees the exact pre-repair state, 'post' when it sees the
    repair, 'unknown' otherwise (including when it cannot read)."""
    try:
        con = sqlite3.connect(sqlite_uri(real, "ro"), uri=True, isolation_level=None,
                              timeout=5)
    except Exception:
        return "unknown"
    try:
        con.execute("BEGIN")
        pre, _ = check_pre(con)
        if not pre:
            return "pre"
        post, _ = check_post(con, tolerant=False)
        return "post" if not post else "unknown"
    except Exception:
        return "unknown"
    finally:
        try:
            if con.in_transaction:
                con.execute("ROLLBACK")
            con.close()
        except Exception:  # noqa: S110 -- a read-only connection; nothing to undo
            pass


def resolve_uncertain_commit(real: str, ex: BaseException, out: Out, res: Result) -> None:
    """COMMIT raised: never guess.  Exit 2/3 only if the exact pre-state is
    provably still there; otherwise treat it as committed (exit 5)."""
    what = f"{type(ex).__name__}: {ex}"
    outcome = commit_outcome(real)
    if outcome == "pre":
        busy = is_busy_error(ex)
        out(f"{'BUSY' if busy else 'ERROR'} (COMMIT): {what}. A fresh connection still sees "
            f"the exact pre-repair state: rolled back; nothing written.")
        res.code = EXIT_BUSY if busy else EXIT_STATE
        return
    res.committed = True
    res.post_failures.append(f"COMMIT raised {what}; re-read: {outcome}")
    res.code = EXIT_COMMITTED_UNVERIFIED
    if outcome == "post":
        out(f"ERROR (COMMIT): {what}, but a fresh connection sees the repaired state: the "
            f"repair IS committed.  Run check_phantom_repair.py now.")
    else:
        out(f"ERROR (COMMIT): {what}, and a fresh connection sees neither the exact "
            f"pre-repair state nor the repaired state (or could not read).  Treat it as "
            f"COMMITTED: run check_phantom_repair.py now.")


def do_apply(target: Target, real: str, backup: str | None, dry_run: bool,
             out: Out, res: Result) -> None:
    """Sets res.code, and res.committed once the repair is in the database."""
    con = None
    res.stage = "connect"
    uncertain: BaseException | None = None
    try:
        con = sqlite3.connect(sqlite_uri(real, "rw"), uri=True,
                              isolation_level=None, timeout=5)
        con.execute("PRAGMA busy_timeout = 5000")
        # Take the write lock FIRST: from here to COMMIT/ROLLBACK nothing else
        # can write, so the state asserted below is the state that is changed.
        res.stage = "BEGIN IMMEDIATE"
        con.execute("BEGIN IMMEDIATE")
        res.stage = "transaction"
        try:
            jm = con.execute("PRAGMA journal_mode").fetchone()[0]
            out(f"database : {real}  (kind={target.kind}, journal_mode={jm})")
            pre_problems, notes = check_pre(con)
            if pre_problems:
                post_problems, post_notes = check_post(con, tolerant=True)
                if not post_problems:
                    for n in post_notes:
                        out(f"note     : {n}")
                    out("ALREADY APPLIED: the database is in the post-repair state. "
                        "Rolled back the (empty) transaction; nothing changed.")
                    res.code = EXIT_OK
                    return
                out("STATE MISMATCH: neither the expected pre-repair nor the post-repair "
                    "state. Rolled back; nothing changed.")
                for p in pre_problems:
                    out(f"  pre : {p}")
                for p in post_problems:
                    out(f"  post: {p}")
                res.code = EXIT_STATE
                return
            for n in notes:
                out(f"note     : {n}")

            if backup and not dry_run:
                # A second, read-only connection may read while this one holds
                # the (not yet used) write lock, so the backup is exactly the
                # state asserted above.
                res.stage = "backup"
                take_backup(real, backup)
                out(f"backup   : {backup} (SQLite backup API; quick_check ok; exact "
                    f"pre-state ok)")
                res.stage = "transaction"
            elif backup:
                out(f"backup   : dry run -- NOT written; {backup} was validated and the "
                    f"real run will write it")

            max_ledger, max_closure = max_ids(con)
            res.before = measure(con, max_ledger, max_closure)
            changes0 = con.total_changes
            res.entry_time = utc_iso_now()
            apply_repair(con, res.entry_time)
            res.changes = con.total_changes - changes0

            post_problems, _ = check_post(con, tolerant=False)
            res.after = measure(con, max_ledger, max_closure)
            bad = post_problems + verify_transition(res.before, res.after, res.changes)
            qc = con.execute("PRAGMA quick_check").fetchone()[0]
            if qc != "ok":
                bad.append(f"quick_check: {qc}")
            if bad:
                out("POST-CHECK FAILED. Rolled back; nothing written.")
                for b in bad:
                    out(f"  {b}")
                if backup and not dry_run:
                    out(f"  (the backup {backup} was written before the failure; it is a "
                        f"valid pre-state copy -- pass a new --backup-to next time)")
                res.code = EXIT_STATE
                return

            if dry_run:
                out(f"DRY RUN OK: {res.changes} row changes verified, then rolled back.")
                res.code = EXIT_OK
                return
            if out.failed:
                raise RuntimeError(f"the console or the audit log could not be written "
                                   f"({out.failed[-1]}); refusing to COMMIT without a "
                                   f"complete audit trail")
            res.stage = "COMMIT"
            res.commit_attempted = True
            con.execute("COMMIT")
            res.committed = True
            res.stage = "read-back"
        finally:
            if not res.committed and con.in_transaction:
                try:
                    con.execute("ROLLBACK")
                except sqlite3.Error as ex:
                    out(f"WARNING: ROLLBACK raised {ex}; SQLite rolls back an unfinished "
                        f"transaction when the connection closes")

        # Committed.  Read the result back in one read transaction.
        con.execute("BEGIN")
        try:
            post_problems, _ = check_post(con, tolerant=False)
        finally:
            con.execute("ROLLBACK")
        if post_problems:
            out("COMMITTED, BUT THE READ-BACK DIFFERS (the repair IS in the database; "
                "run the checker now):")
            for p in post_problems:
                out(f"  {p}")
            res.post_failures.append("the read-back after COMMIT differs")
            res.code = EXIT_COMMITTED_UNVERIFIED
            return
        res.code = EXIT_OK
    except Exception as ex:
        if res.committed:
            out(f"ERROR after COMMIT ({res.stage}): {type(ex).__name__}: {ex}. The repair "
                f"WAS written; run check_phantom_repair.py on this database now.")
            res.post_failures.append(f"{res.stage}: {type(ex).__name__}: {ex}")
            res.code = EXIT_COMMITTED_UNVERIFIED
        elif res.commit_attempted:
            uncertain = ex           # resolved below, once the connection is closed
        elif is_busy_error(ex):
            out(f"BUSY ({res.stage}): {ex}. Something else holds a lock on this "
                f"database; nothing written.")
            res.code = EXIT_BUSY
        else:
            out(f"ERROR ({res.stage}): {type(ex).__name__}: {ex}. Rolled back; nothing "
                f"written.")
            res.code = EXIT_STATE
        if res.stage == "backup" and not res.committed and backup:
            out(f"  (a partial or unverified backup may exist at {backup}: it is NOT a "
                f"rollback source; delete it and pass a new --backup-to)")
    finally:
        if con is not None:
            try:
                con.close()
            except Exception as ex:
                out(f"WARNING: closing the database connection raised {ex}")
                if res.committed:
                    res.post_failures.append(f"close: {ex}")
    if uncertain is not None:
        resolve_uncertain_commit(real, uncertain, out, res)


def after_commit(target: Target, real: str, backup: str | None, res: Result,
                 out: Out) -> int:
    """Summary, the post-COMMIT race re-check, the next steps and rollback
    command, and the hashes.  Each step is guarded: a failure is reported,
    turns exit 0 into 5, and never skips a later step or masks exit 4 (the
    connection is closed, so the -wal has been checkpointed away)."""

    def guarded(name: str, fn) -> None:
        try:
            fn()
        except Exception as ex:
            res.post_failures.append(f"{name}: {type(ex).__name__}: {ex}")
            out(f"ERROR after COMMIT ({name}): {type(ex).__name__}: {ex}. The repair WAS "
                f"written; this step's output is missing -- run the checker now.")

    def summary() -> None:
        if res.code == EXIT_OK:
            out(f"APPLIED at {res.entry_time}: {res.changes} row changes.")
        else:
            out(f"COMMITTED at {res.entry_time} ({res.changes} row changes in the "
                f"transaction), BUT UNVERIFIED -- see above.")
        out("  offer_closure_events +6 (status_update dead_on_chain + "
            "phantom_fill_correction per offer)")
        out("  ledger_entries       +10 (9 reversal legs + 1 DBX correction of adjust 2641)")
        out("  offer_log            3 rows filled -> cancelled (cancel cause restored)")
        out("  trade_log            -3 rows (1900, 1901, 1902; archived in the closure "
            "events)")
        # Same line format as the first pass (the execution record quotes it).
        for a, name in ((XCH, "XCH"), (DBX, "DBX"), (BYC, "BYC")):
            out(f"  ledger {a[:8]:<8} {res.before['ledger'].get(a, 0):>18} -> "
                f"{res.after['ledger'].get(a, 0):>18} ({EXPECTED_LEDGER_NET[a]:+d})  "
                f"<- compare with the wallet's confirmed {name}")

    def race_recheck() -> None:
        race: list[str] = []
        try:
            race += [f"open handle: {h}" for h in held_files(target.path)]
        except Exception as ex:
            race.append(f"could not re-check file handles ({ex})")
        if target.live_like:
            try:
                race += [f"process: {p}" for p in xoptrader_processes()]
            except Exception as ex:
                race.append(f"could not re-read the process list ({ex})")
        res.race = race                 # recorded before anything is printed
        if race:
            out("")
            out("!" * 78)
            out("!! WARNING: SOMETHING APPEARED BETWEEN THE CHECKS AND COMMIT (or the")
            out("!! re-check could not run).  The repair IS committed.  An engine that")
            out("!! started in that window loaded pre-repair P&L into memory and will later")
            out("!! overwrite inventory_state.  Stop it (close the GUI), then run")
            out("!! check_phantom_repair.py before anything else.")
            for r in race:
                out(f"!!   {r}")
            out("!" * 78)
            out("")
        else:
            out("re-check : after COMMIT no XOPTrader process and no open handle"
                + (" (process list re-read)" if target.live_like else ""))

    def next_steps() -> None:
        checker = SCRIPT_DIR / "check_phantom_repair.py"
        if backup:
            flag = " --allow-live-readonly" if target.live_like else ""
            out("NEXT     : verify (the rollback decision point):")
            out(f'           python "{checker}" "{target.path}" --baseline "{backup}"{flag}')
            out("ROLLBACK : window = from this COMMIT to the first start of the engine or "
                "GUI.")
            out("           Inside it, and only from this backup:")
            out(f'           python "{SCRIPT_PATH}" "{target.path}"'
                + (" --i-stopped-the-engine" if target.live_like else "")
                + f' --restore-from "{backup}"')
            out("           After the engine has run: forward-only un-repair (docstring, "
                "ROLLBACK).")
        else:
            out("NEXT     : verify against the pre-repair copy:")
            out(f'           python "{checker}" "{target.path}" --baseline <pre-repair copy>')
            out("ROLLBACK : no backup was taken (a scratch copy).")

    def hashes() -> None:
        wal = target.path + "-wal"
        wal_note = ""
        if os.path.exists(wal) and os.path.getsize(wal) > 0:
            wal_note = f" (NOTE: {wal} is non-empty, so the file hash alone is not the state)"
        out(f"sha256   : database {real} = {file_sha256(real)}{wal_note}")
        if backup:
            out(f"sha256   : backup   {backup} = {file_sha256(backup)}")
        _ok, pin_lines = pin_report()
        for line in pin_lines:
            out(line)

    guarded("summary", summary)
    guarded("race re-check", race_recheck)
    guarded("next steps", next_steps)
    guarded("hashes", hashes)
    if out.failed:
        res.post_failures.append(f"output: {out.failed[-1]}")
    code = final_apply_code(res)
    if code == EXIT_COMMITTED_UNVERIFIED:
        out("POST-COMMIT FAILURE: the repair IS committed, but these steps failed:")
        for f in res.post_failures:
            out(f"  {f}")
        out("  Run the checker (NEXT above) now; the rollback window is open.")
    return code


# ---------------------------------------------------------------------------
# Restore (the rollback inside the window)
# ---------------------------------------------------------------------------

def db_is_backup_plus_repair(db_real: str, bak_real: str, out: Out) -> list[str]:
    """[] when the database is exactly the backup plus this repair.  The
    database is read in one read transaction."""
    problems: list[str] = []
    b = sqlite3.connect(sqlite_uri(bak_real, "ro", immutable=True), uri=True)
    d = sqlite3.connect(sqlite_uri(db_real, "ro"), uri=True, isolation_level=None)
    try:
        d.execute("BEGIN")
        post, _ = check_post(d, tolerant=False)
        problems += [f"post-state: {p}" for p in post]
        bl, bc = max_ids(b)
        bt, dtabs = user_tables(b), user_tables(d)
        if bt != dtabs:
            problems.append(f"tables differ: {bt} vs {dtabs}")
            return problems
        want_delta = {"trade_log": -3, "ledger_entries": N_LEDGER_ROWS,
                      "offer_closure_events": N_CLOSURE_ROWS}
        for t in bt:
            if t == "sqlite_sequence":
                continue
            nb = b.execute(f'SELECT COUNT(*) FROM "{t}"').fetchone()[0]
            nd = d.execute(f'SELECT COUNT(*) FROM "{t}"').fetchone()[0]
            if nd - nb != want_delta.get(t, 0):
                problems.append(f"{t}: {nb} -> {nd} rows (expected "
                                f"{want_delta.get(t, 0):+d}): the engine has written")
                continue
            where, args = untouched_where(t, bl, bc)
            if table_digest(b, t, where, args) != table_digest(d, t, where, args):
                problems.append(f"{t}: rows outside the repair differ from the backup")
        bs = dict(rows(b, "SELECT name, seq FROM sqlite_sequence"))
        ds = dict(rows(d, "SELECT name, seq FROM sqlite_sequence"))
        want_s = dict(bs)
        want_s["ledger_entries"] = want_s.get("ledger_entries", 0) + N_LEDGER_ROWS
        want_s["offer_closure_events"] = want_s.get("offer_closure_events", 0) + N_CLOSURE_ROWS
        if ds != want_s:
            problems.append(f"sqlite_sequence {ds} != backup + repair {want_s}")
        if rows(b, "SELECT type, name, sql FROM sqlite_master ORDER BY type, name") != \
                rows(d, "SELECT type, name, sql FROM sqlite_master ORDER BY type, name"):
            problems.append("schema differs from the backup")
    finally:
        try:
            if d.in_transaction:
                d.execute("ROLLBACK")
        finally:
            d.close()
            b.close()
    return problems


class RestoreProgress:
    """Where --restore-from got to (main reads it after an interruption)."""

    def __init__(self) -> None:
        self.real: str | None = None
        self.tmp: str | None = None
        self.aside: str | None = None
        self.swap_started = False
        self.settled = False       # the swap finished and verified, was undone, or
                                   # the manual moves were printed


def _remove_db_files(base: str) -> list[str]:
    """Delete base and its -wal/-shm/-journal (retrying briefly: a scanner may
    hold a just-closed file).  Returns what could not be deleted."""
    left: list[str] = []
    for sfx in ("", "-wal", "-shm", "-journal"):
        p = base + sfx
        for i in range(8):
            try:
                if os.path.lexists(p):
                    os.remove(p)
                break
            except OSError as ex:
                if i == 7:
                    left.append(f"{p} ({ex})")
                else:
                    time.sleep(0.25)
    return left


def _discard_replacement(prog: RestoreProgress, out: Out) -> bool:
    """Delete the temporary replacement (and its sidecars)."""
    if not prog.tmp:
        return True
    left = _remove_db_files(prog.tmp)
    if left:
        out("WARNING  : could not delete the temporary replacement; delete it by hand "
            "before anything else:")
        for p in left:
            out(f"             {p}")
        return False
    out(f"cleanup  : deleted the temporary replacement {prog.tmp}")
    return True


def _undo_swap(prog: RestoreProgress, out: Out) -> bool:
    """Put the repaired database back, working from what is on disk (so an
    interruption between a rename and its bookkeeping cannot mislead it)."""
    real, tmp, aside = prog.real, prog.tmp, prog.aside
    assert real and tmp and aside
    ok = True
    try:
        if os.path.lexists(aside) and os.path.lexists(real) and not os.path.lexists(tmp):
            os.rename(real, tmp)             # the replacement was already in place
            out(f"undone   : {real} -> {tmp}")
        for sfx in ("", "-shm", "-wal"):
            if not os.path.lexists(aside + sfx):
                continue
            if os.path.lexists(real + sfx):
                ok = False
                out(f"cannot undo: both {aside + sfx} and {real + sfx} exist")
                continue
            os.rename(aside + sfx, real + sfx)
            out(f"undone   : {aside + sfx} -> {real + sfx}")
    except BaseException as ex:
        out(f"could not undo the swap: {type(ex).__name__}: {ex}")
        return False
    return ok and not os.path.lexists(aside)


def _print_manual_moves(prog: RestoreProgress, out: Out) -> None:
    real, tmp, aside = prog.real, prog.tmp, prog.aside
    out("RESTORE INCOMPLETE.  Start NOTHING.  Files now:")
    for p in (real, tmp, aside):
        for sfx in ("", "-wal", "-shm"):
            q = f"{p}{sfx}"
            out(f"  {'present' if os.path.lexists(q) else 'absent '}  {q}")
    out("To put the repaired database back (cmd or PowerShell, in this order):")
    if aside and real and tmp and os.path.lexists(aside) and os.path.lexists(real) \
            and not os.path.lexists(tmp):
        out(f'  move "{real}" "{tmp}"')
    for sfx in ("", "-shm", "-wal"):
        if aside and os.path.lexists(aside + sfx):
            out(f'  move "{aside + sfx}" "{real + sfx}"')
    out(f"then delete {tmp} and re-run --restore-from if the restore is still wanted.")
    prog.settled = True


def do_restore(target: Target, real: str, bak_real: str, out: Out,
               prog: RestoreProgress) -> int:
    out(f"restore  : {real} <- {bak_real}")
    try:
        jm = verify_pre_copy(bak_real, "backup")
        bak_sha = file_sha256(bak_real)
    except Exception as ex:
        out(f"REFUSED: {ex}")
        return EXIT_REFUSED
    out(f"backup   : quick_check ok, exact pre-state ok, journal_mode={jm}, "
        f"sha256 {bak_sha}")
    try:
        problems = db_is_backup_plus_repair(real, bak_real, out)
    except Exception as ex:
        if is_busy_error(ex):
            out(f"BUSY: {ex}; the database was NOT touched")
            return EXIT_BUSY
        out(f"ERROR while comparing: {type(ex).__name__}: {ex}; the database was NOT "
            f"touched")
        return EXIT_STATE
    if problems:
        out("REFUSED: the database is not exactly this backup plus the repair, so the "
            "rollback window has closed (or this is the wrong backup). Restoring would "
            "lose what was written since; use the forward un-repair (docstring, "
            "ROLLBACK).")
        for p in problems:
            out(f"  {p}")
        return EXIT_REFUSED
    out("database : exactly backup + repair (every other row identical)")
    try:
        out(f"repaired : sha256 {file_sha256(real)} (it will be kept aside)")
    except Exception as ex:           # informational only; nothing touched yet
        out(f"repaired : sha256 unavailable ({type(ex).__name__}: {ex})")

    stamp = utc_stamp()
    tmp = real + f".restoring-{stamp}"
    aside = real + f".rolledback-{stamp}"
    for p in (tmp, aside):
        for sfx in ("", "-wal", "-shm", "-journal"):
            if os.path.lexists(p + sfx):
                out(f"REFUSED: {p + sfx} already exists")
                return EXIT_REFUSED
    prog.real, prog.tmp, prog.aside = real, tmp, aside
    try:
        src = sqlite3.connect(sqlite_uri(bak_real, "ro", immutable=True), uri=True)
        dst = sqlite3.connect(sqlite_uri(tmp, "rwc"), uri=True)
        try:
            src.backup(dst)
        finally:
            dst.close()
            src.close()
        jm2 = verify_pre_copy(tmp, "replacement")
        tmp_sha = file_sha256(tmp)
    except Exception as ex:
        out(f"ERROR building or verifying the replacement {tmp}: {type(ex).__name__}: "
            f"{ex}; the database was NOT touched.")
        _discard_replacement(prog, out)
        return EXIT_STATE
    out(f"replace  : {tmp} verified (journal_mode={jm2}, sha256 {tmp_sha})")

    held: list[str] = []
    try:
        held += held_files(real)
    except Exception as ex:
        held.append(f"could not re-check file handles ({ex})")
    if target.live_like:
        try:
            held += [f"process: {p}" for p in xoptrader_processes()]
        except Exception as ex:
            held.append(f"could not re-read the process list ({ex})")
    if held:
        out("BUSY: something appeared; the database was NOT touched:")
        for h in held:
            out(f"  {h}")
        _discard_replacement(prog, out)
        return EXIT_BUSY

    prog.swap_started = True
    try:
        for sfx in ("-wal", "-shm", ""):          # siblings first, then the file
            if os.path.lexists(real + sfx):
                os.rename(real + sfx, aside + sfx)
                out(f"moved    : {real + sfx} -> {aside + sfx}")
        os.rename(tmp, real)
        out(f"moved    : {tmp} -> {real}")
    except BaseException as ex:
        out(f"ERROR during the swap: {type(ex).__name__}: {ex}.  Undoing it.")
        if _undo_swap(prog, out):
            prog.settled = True
            _discard_replacement(prog, out)
            out("UNDONE   : the repaired database is back in place, exactly as it was; "
                "the restore did not happen.")
            return EXIT_STATE
        _print_manual_moves(prog, out)
        return EXIT_RESTORE_INCOMPLETE
    try:
        jm3 = verify_pre_copy(real, "restored database")
    except BaseException as ex:
        out(f"ERROR: the restored database failed verification ({type(ex).__name__}: "
            f"{ex}).  Undoing the swap.")
        if _undo_swap(prog, out):
            prog.settled = True
            _discard_replacement(prog, out)
            out("UNDONE   : the repaired database is back in place, exactly as it was; "
                "the restore did not happen.")
            return EXIT_STATE
        _print_manual_moves(prog, out)
        return EXIT_RESTORE_INCOMPLETE
    prog.settled = True
    out(f"RESTORED: {real} is the exact pre-repair state again (journal_mode={jm3}); "
        f"sha256 {tmp_sha}.  The repaired database is kept at {aside}.")
    return EXIT_OK


# ---------------------------------------------------------------------------
# --list-blockers (read-only)
# ---------------------------------------------------------------------------

def list_blockers(db_path: str) -> int:
    """The live run's own guards, in the live run's order, reporting every
    blocker instead of stopping at the first.  No SQLite connection and no
    file written; the intent files and the database's open-handle test are
    touched only once the process list is clear, exactly as the live run
    does.  Returns the code the live run would stop with (0 = none)."""
    first = EXIT_OK

    def block(code: int, msg: str) -> None:
        nonlocal first
        print(f"BLOCKER  : {msg}", flush=True)
        if first == EXIT_OK:
            first = code

    print(f"script   : {SCRIPT_PATH}  (list-blockers: read-only; no SQLite access; "
          f"writes nothing)", flush=True)
    print(f"when     : {utc_iso_now()}  user {getpass.getuser()}@{socket.gethostname()}")
    print(f"git      : {git_state()}")
    pins_ok, pin_lines = pin_report()
    for line in pin_lines:
        print(line)
    try:
        fake = fake_live_dir()
        target = classify_db_path(db_path, fake)
    except Refused as ex:
        print(f"REFUSED: {ex}")
        return ex.code
    print(f"target   : {target.path}  (kind={target.kind}; allow-list ok)")
    if target.live_like and not pins_ok:
        block(EXIT_REFUSED, f"the scripts do not match {SUMS_NAME} (above): the live run "
                            f"refuses (exit 1)")
    clear = True
    if target.live_like:
        try:
            procs = xoptrader_processes()
        except Exception as ex:
            clear = False
            block(EXIT_BUSY, f"the process list cannot be read ({ex}): the live run "
                             f"refuses (exit 3)")
        else:
            print(f"excluded : this process and its python launcher ancestors, pids "
                  f"{EXCLUDED_PIDS}")
            for p in procs:
                clear = False
                block(EXIT_BUSY, f"process {p}")
            if clear:
                print("processes: no XOPTrader engine, GUI or maintenance process running")
        if clear:
            try:
                seen, hits = intent_file_hits(target.data_dir)
            except Refused as ex:
                block(ex.code, str(ex))
            else:
                for h in hits:
                    block(EXIT_REFUSED, f"intent file: {h} (with the engine stopped, "
                                        f"delete those lines)")
                if not hits:
                    print("intent   : " + ("; ".join(seen) + " -- none of the three "
                                           "offers named" if seen else
                                           "no uncancelled.txt/.json/.txt.tmp"))
    else:
        print("processes: not checked for a scratch copy (the run checks them only for "
              "the live and fake-live databases)")
    if clear:
        try:
            real = verify_on_disk(target.path, "database")
        except Refused as ex:
            block(ex.code, str(ex))
        except OSError as ex:
            block(EXIT_REFUSED, f"database {target.path}: {ex}")
        else:
            print(f"path     : {real} (canonical, one hard link, no reparse point)")
            try:
                held = held_files(real)
            except Exception as ex:
                held = [f"could not test the handles ({ex})"]
            for h in held:
                block(EXIT_BUSY, f"open handle: {h}")
            if not held:
                print("handles  : nobody else has the database, -wal or -shm open")
    else:
        print("handles  : NOT tested while an XOPTrader process runs or the process list "
              "is unreadable (the live run refuses on the processes before it touches any "
              "file); stop them and re-run --list-blockers")
    if first == EXIT_OK:
        print("RESULT   : nothing would stop the live run (exit 0)")
    else:
        print(f"RESULT   : the live run would stop with exit {first} "
              f"({EXIT_MEANING[first]}); clear every BLOCKER above and re-run")
    return first


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

class _Parser(argparse.ArgumentParser):
    """Bad arguments exit 1 (refused), never argparse's default 2, which this
    script reserves for "not committed, nothing written"."""

    def error(self, message: str):
        self.print_usage(sys.stderr)
        self.exit(EXIT_REFUSED, f"{self.prog}: error: {message}\n")


def _parser() -> argparse.ArgumentParser:
    head, _, rest = (__doc__ or "").partition("\n\n")
    ap = _Parser(description=head, epilog=rest,
                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db_path", nargs="?",
                    help="the database: the live DB (engine stopped), or a copy under "
                         "the scratch directory")
    ap.add_argument("--i-stopped-the-engine", action="store_true",
                    help="required for the live (or fake-live) database; never for a copy")
    ap.add_argument("--backup-to", metavar="NEW_FILE",
                    help="write a SQLite-backup-API copy here, inside the write lock, "
                         "before changing anything (required for the live DB; must not "
                         "exist; outside the live checkout; not written by --dry-run)")
    ap.add_argument("--dry-run", action="store_true",
                    help="apply and verify inside the transaction, then ROLLBACK")
    ap.add_argument("--restore-from", metavar="BACKUP",
                    help="ROLLBACK inside the window: replace the database with this "
                         "backup (see ROLLBACK)")
    ap.add_argument("--list-blockers", action="store_true",
                    help="read-only: print every process, intent-file line and open "
                         "handle the live run would refuse on (no SQLite access, no file "
                         "written); exit = the code the live run would stop with")
    ap.add_argument("--print-hashes", action="store_true",
                    help="print the scripts' hashes and their SHA256SUMS status")
    ap.add_argument("--write-hashes", action="store_true",
                    help="(re)write SHA256SUMS next to this script -- after a REVIEWED "
                         "change only")
    return ap


def _run(args, out: Out, res: Result, prog: RestoreProgress, ctx: dict) -> int:
    mode = "restore" if args.restore_from else ("dry-run" if args.dry_run else "apply")
    out(f"script   : {SCRIPT_PATH}  ({mode})")
    out(f"when     : {utc_iso_now()}  user {getpass.getuser()}@{socket.gethostname()}  "
        f"python {sys.version.split()[0]}  sqlite {sqlite3.sqlite_version}")
    out(f"argv     : {ctx['argv']}")
    out(f"git      : {git_state()}")
    pins_ok, pin_lines = pin_report()
    for line in pin_lines:
        out(line)

    try:
        # -- which file: string-only allow-list, before the file is touched --
        fake = fake_live_dir()
        target = classify_db_path(args.db_path, fake)
        ctx["target"] = target
        if target.live_like:
            if not args.i_stopped_the_engine:
                raise Refused(f"{target.path} is the {target.kind} database. Close the "
                              f"GUI (the graceful engine stop), confirm with --list-blockers "
                              f"that nothing is left, then re-run with "
                              f"--i-stopped-the-engine --backup-to <new file>.")
            if not args.backup_to and not args.restore_from:
                raise Refused("the live database needs --backup-to <new file> (SQLite "
                              "backup API), also for --dry-run, which validates it "
                              "without writing it.")
            if not pins_ok:
                raise Refused(f"the scripts do not match {SUMS_NAME} (above); run only "
                              f"the reviewed version (regenerate the pins only with a "
                              f"reviewed change).")
        out(f"target   : {target.path}  (kind={target.kind}; allow-list ok)")
        if not pins_ok:
            out("WARNING  : the scripts do not match SHA256SUMS (refused for the live "
                "database; allowed for a scratch copy)")

        # -- the files this run creates ------------------------------------
        backup = bak_real = None
        if args.backup_to:
            backup = validate_new_file(args.backup_to, "--backup-to", target, fake)
            log_path = backup + (".dryrun" if args.dry_run else "") + ".log"
        elif args.restore_from:
            bak_real = validate_restore_source(args.restore_from, target, fake)
            log_path = bak_real + ".restore.log"
        else:
            log_path = target.path + ".phantom_repair" + (".dryrun" if args.dry_run
                                                          else "") + ".log"
        ctx["backup"] = backup
        if target.live_like:
            out.open(log_path)

        # -- is anything else using it (live: before touching the file) -------
        if target.live_like:
            try:
                procs = xoptrader_processes()
            except Exception as ex:
                raise Refused(f"could not list processes to confirm the engine and GUI "
                              f"are stopped ({ex}).", EXIT_BUSY) from None
            if procs:
                raise Refused("XOPTrader processes are running (close the GUI; it is "
                              "the graceful engine stop):\n  " + "\n  ".join(procs),
                              EXIT_BUSY)
            out("processes: no XOPTrader engine, GUI or maintenance process running")
            seen, hits = intent_file_hits(target.data_dir)
            if hits:
                raise Refused("an S46 cancel-intent file names a phantom offer; with the "
                              "engine stopped, delete those lines (or the leftover "
                              ".txt.tmp): the offers are proven dead and the repair "
                              "records them closed:\n  " + "\n  ".join(hits))
            out("intent   : " + ("; ".join(seen) + " -- none of the three offers named"
                                 if seen else "no uncancelled.txt/.json/.txt.tmp"))

        real = verify_on_disk(target.path, "database")
        ctx["real"] = real
        if not target.live_like:
            out.open(log_path)
        try:
            held = held_files(real)
        except Exception as ex:
            raise Refused(f"could not test whether another process has the database "
                          f"open ({ex})", EXIT_BUSY) from None
        if held:
            raise Refused("\n  ".join(["file held open (stop the engine, the GUI and any "
                                       "sqlite session on it first):"] + held), EXIT_BUSY)
    except Refused as ex:
        out(f"REFUSED: {ex}")
        return ex.code

    if args.restore_from:
        ctx["phase"] = "restore"
        return do_restore(target, real, bak_real, out, prog)
    ctx["phase"] = "apply"
    do_apply(target, real, backup, args.dry_run, out, res)
    if not res.committed:
        return res.code
    return after_commit(target, real, backup, res, out)


def _on_unexpected(ex: BaseException, out: Out, res: Result, prog: RestoreProgress,
                   ctx: dict) -> int:
    """An interruption (Ctrl+C) or an unexpected error that escaped: the code
    follows from how far the run got, never from the exception."""
    what = f"{type(ex).__name__}: {ex}"
    phase = ctx.get("phase", "guards")
    if phase == "guards":
        out(f"REFUSED: interrupted or failed before the database was opened ({what})")
        return EXIT_REFUSED
    if phase == "restore":
        if prog.swap_started and not prog.settled:
            out(f"ERROR during --restore-from after the swap began ({what})")
            _print_manual_moves(prog, out)
            return EXIT_RESTORE_INCOMPLETE
        if prog.swap_started:
            out(f"ERROR after the restore settled ({what}); see the lines above")
            return EXIT_RESTORE_INCOMPLETE
        if prog.tmp:
            _discard_replacement(prog, out)
        out(f"ERROR during --restore-from before any file was moved ({what}); the "
            f"database was NOT touched.")
        return EXIT_STATE
    # apply
    if not res.committed and res.commit_attempted:
        resolve_uncertain_commit(ctx.get("real") or "", ex, out, res)
        if not res.committed:
            return res.code
    if res.committed:
        target, backup = ctx.get("target"), ctx.get("backup")
        out(f"INTERRUPTED OR FAILED AFTER COMMIT ({what}): the repair IS committed.  "
            f"Run the checker now; the rollback window is open.")
        if target is not None and backup:
            out(f'  python "{SCRIPT_DIR / "check_phantom_repair.py"}" "{target.path}" '
                f'--baseline "{backup}"'
                + (" --allow-live-readonly" if target.live_like else ""))
            out(f'  rollback: python "{SCRIPT_PATH}" "{target.path}"'
                + (" --i-stopped-the-engine" if target.live_like else "")
                + f' --restore-from "{backup}"')
        res.post_failures.append(what)
        return EXIT_RACE if res.race else EXIT_COMMITTED_UNVERIFIED
    out(f"INTERRUPTED OR FAILED BEFORE COMMIT ({what}): the transaction was rolled back; "
        f"nothing written.")
    if res.stage == "backup" and ctx.get("backup"):
        out(f"  (a partial or unverified backup may exist at {ctx['backup']}: it is NOT a "
            f"rollback source; delete it and pass a new --backup-to)")
    return EXIT_STATE


def main(argv: list[str]) -> int:
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:  # noqa: S110 -- a stream without reconfigure keeps its default
        pass
    ap = _parser()
    args = ap.parse_args(argv)

    if args.write_hashes:
        print(f"wrote {write_pins()}")
        return EXIT_OK
    if args.print_hashes:
        ok, lines = pin_report()
        print("\n".join(lines))
        return EXIT_OK if ok else EXIT_REFUSED
    if not args.db_path:
        ap.error("db_path is required")
    if sys.platform != "win32":
        print("REFUSED: this one-off runs only on the Windows host (its guards are "
              "Windows-specific).")
        return EXIT_REFUSED
    if args.restore_from and (args.backup_to or args.dry_run):
        print("REFUSED: --restore-from cannot be combined with --backup-to or --dry-run.")
        return EXIT_REFUSED
    if args.list_blockers:
        if args.backup_to or args.dry_run or args.restore_from:
            print("REFUSED: --list-blockers is read-only; it cannot be combined with "
                  "--backup-to, --dry-run or --restore-from.")
            return EXIT_REFUSED
        return list_blockers(args.db_path)

    out = Out()
    res = Result()
    prog = RestoreProgress()
    ctx: dict = {"phase": "guards", "argv": argv}
    try:
        code = _run(args, out, res, prog, ctx)
    except BaseException as ex:       # Ctrl+C, or a defect: the code follows the state
        code = _on_unexpected(ex, out, res, prog, ctx)
    return out.finish(code)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
