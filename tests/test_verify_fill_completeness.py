"""The completeness sweep must exclude offers proven dead on-chain, and only those.

`scripts/verify_fill_completeness.py` is the house acceptance gate for fill
recording: every wallet trade record that reached CONFIRMED must exist in
trade_log or taker_fills.  On 2026-09-22 the wallet reported three offers
CONFIRMED that nobody took, and the engine booked them as fills (trade_log
1900-1902).  The phantom-fill repair (docs/PHANTOM-FILL-REPAIR-2026-09.md)
deletes those rows.  The wallet still says CONFIRMED for two of them, so
without an exclusion the sweep would FAIL on them for ever, and on every
future offer v0.10.26 proves dead while the wallet calls it CONFIRMED.  A
permanent FAIL is the one that eventually gets "fixed" by a backfill.  That
would re-book fills that never happened.

The exclusion has to be narrow in two ways:

* it keys on the engine's own verdict event -- event_type 'status_update',
  closure_reason exactly 'dead_on_chain' -- and not on any reason that merely
  STARTS with 'dead_on_chain' (the repair's 'phantom_fill_correction' events
  do, and an offer must not be excluded on the strength of those alone), nor
  on the same reason under another event type (a 'status_observation' is the
  verdict on a row that was already closed, and the script promises it is
  NOT excluded);
* it counts offers, not events: the repair writes two dead_on_chain-prefixed
  events per offer, and an offer is excluded once.

And the sweep has to stay a check on the engine, not an echo of it: every
excluded offer is a WARN with the command that re-proves it from the chain,
and --strict turns any exclusion into a nonzero exit.

The --since-days window must not drop an offer posted before the window and
taken inside it.  A maker fill usually has no accepted_at_time, and its
created_at_time can be days before the take, so the window also has a start
height.

Every test builds its own throwaway database under `tmp_path` and replaces the
wallet RPC with a fake.  Nothing here opens `data/xop_trader.db` or talks to a
wallet: `_run()` always passes `--db`, and `WalletRpc` is patched before
`main()` runs.
"""

from __future__ import annotations

import importlib.util
import sqlite3
import sys
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT = REPO_ROOT / "scripts" / "verify_fill_completeness.py"


def _load_script():
    """Import the sweep by path, the way the operator runs it (scripts/ is not a package)."""
    spec = importlib.util.spec_from_file_location("xop_fill_sweep_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules["xop_fill_sweep_test"] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop("xop_fill_sweep_test", None)
        raise
    return module


sweep = _load_script()

# Offer ids, in the wallet's 0x-prefixed lower-hex form.
REAL_FILL = "0x" + "a1" * 32      # in trade_log
REAL_TAKE = "0x" + "b2" * 32      # in taker_fills
DEAD = "0x" + "d6" * 32           # the repair's shape: verdict + correction events
MISSING = "0x" + "e5" * 32        # CONFIRMED, unrecorded, no dead verdict
CORRECTION_ONLY = "0x" + "c3" * 32  # only a phantom_fill_correction event
CORRECTION_EXACT = "0x" + "c4" * 32  # a correction event whose reason is exactly the verdict's
CANCEL_ONLY = "0x" + "f7" * 32    # only an ordinary price_adverse cancel event
OBSERVED_DEAD = "0x" + "0b" * 32  # the verdict on an already-closed row: a status_observation
NEAR_MISS = "0x" + "4e" * 32      # one near-miss event, added per test

DBX_ASSET_ID = "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"

# The wallet's height in the fake, for the --since-days window.
PEAK = 9_400_000
DAY = 86_400.0

# The schema of the three tables the sweep reads, as cpp/src/database.cpp
# creates them (only the columns that matter here are populated).
DDL = (
    """
    CREATE TABLE trade_log (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT NOT NULL,
        trade_id TEXT NOT NULL UNIQUE,
        pair_name TEXT NOT NULL,
        side TEXT NOT NULL CHECK(side IN ('bid','ask')),
        price_mojos INTEGER NOT NULL,
        size_mojos INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE taker_fills (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        trade_id TEXT UNIQUE,
        counterparty_offer_id TEXT
    )
    """,
    """
    CREATE TABLE offer_closure_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        offer_id TEXT NOT NULL,
        pair_name TEXT NOT NULL,
        event_type TEXT NOT NULL,
        previous_status TEXT,
        observed_status TEXT,
        closure_reason TEXT,
        resolved_block INTEGER,
        created_at TEXT DEFAULT CURRENT_TIMESTAMP,
        fee_mojos INTEGER
    )
    """,
)


def _event(con, offer_id, event_type, prev, obs, reason, block=9324680):
    con.execute(
        "INSERT INTO offer_closure_events (offer_id, pair_name, event_type,"
        " previous_status, observed_status, closure_reason, resolved_block)"
        " VALUES (?, 'XCH/DBX', ?, ?, ?, ?, ?)",
        (offer_id, event_type, prev, obs, reason, block),
    )


def _make_db(path: Path) -> Path:
    """A synthetic engine database. Never the live one."""
    con = sqlite3.connect(str(path))
    for ddl in DDL:
        con.execute(ddl)
    con.execute(
        "INSERT INTO trade_log (timestamp, trade_id, pair_name, side, price_mojos,"
        " size_mojos) VALUES ('2026-09-25T05:01:03.000Z', ?, 'XCH/DBX', 'ask',"
        " 85361000000000, 1000000000000)",
        (REAL_FILL,),
    )
    con.execute(
        "INSERT INTO taker_fills (trade_id, counterparty_offer_id) VALUES (?, ?)",
        (REAL_TAKE, "0x" + "99" * 32),
    )

    # DEAD: exactly the rows an offer carries after the phantom-fill repair --
    # the original cancel submission, the phantom fill event, the repair's
    # dead_on_chain verdict, and its correction record.  Two of those reasons
    # start with 'dead_on_chain'; the offer must be excluded once.
    _event(con, DEAD, "status_update", "pending", "cancel_pending",
           "price_adverse(1.648%)", 9324693)
    _event(con, DEAD, "status_update", "cancel_pending", "filled", "")
    _event(con, DEAD, "status_update", "filled", "cancelled", "dead_on_chain")
    _event(con, DEAD, "phantom_fill_correction", "filled", "cancelled",
           "dead_on_chain; phantom fill voided: ...; trade_log id=1900 deleted, "
           'original row: {"id":1900}; phantom-fill repair')

    # CORRECTION_ONLY: a reason that starts with 'dead_on_chain' but no verdict
    # event.  Must NOT be excluded -- the filter is the engine's verdict.
    _event(con, CORRECTION_ONLY, "phantom_fill_correction", "filled", "cancelled",
           "dead_on_chain; phantom fill voided: ...")
    # CORRECTION_EXACT: the verdict's exact reason, but on a correction record,
    # not on the engine's status_update.  Must NOT be excluded either.
    _event(con, CORRECTION_EXACT, "phantom_fill_correction", "filled", "cancelled",
           "dead_on_chain")

    # CANCEL_ONLY: an ordinary cancel verdict.  Nothing to do with the chain.
    _event(con, CANCEL_ONLY, "status_update", "cancel_pending", "cancelled",
           "price_adverse(2.247%)")

    # OBSERVED_DEAD: an ordinary cancel closed the row first, so the engine's
    # later dead_on_chain verdict was written as a status_observation
    # (Database::update_offer_status keeps a closed row's status).  The
    # script's docstring promises such an offer is NOT excluded.
    _event(con, OBSERVED_DEAD, "status_update", "cancel_pending", "cancelled",
           "price_adverse(1.203%)")
    _event(con, OBSERVED_DEAD, "status_observation", "cancelled", "cancelled",
           "dead_on_chain")
    con.commit()
    con.close()
    return path


def _add_event(db: Path, offer_id: str, event_type: str, reason: str) -> None:
    con = sqlite3.connect(str(db))
    _event(con, offer_id, event_type, "cancel_pending", "cancelled", reason)
    con.commit()
    con.close()


def _record(trade_id: str, *, mine: bool = True, status: str = "CONFIRMED",
            age_s: float = DAY, height: int = 9_324_680, accepted: bool = True,
            created_age_s: float | None = None) -> dict:
    """A wallet trade record, shaped like get_all_offers' output.

    accepted=False gives what a maker fill usually looks like: no
    accepted_at_time, only created_at_time (created_age_s ago, when given).
    """
    now = time.time()
    t = int(now - age_s)
    created = int(now - created_age_s) if created_age_s is not None else t - 60
    return {
        "trade_id": trade_id,
        "status": status,
        "is_my_offer": mine,
        "accepted_at_time": t if accepted else None,
        "created_at_time": created,
        "confirmed_at_index": height,
        "summary": {"offered": {"xch": 1_000_000_000_000},
                    "requested": {DBX_ASSET_ID: 102_976}},
    }


class _FakeWallet:
    """Stands in for the wallet RPC: synced, returning a fixed record list."""

    records: list[dict] = []
    height: int = PEAK
    calls: list[str] = []

    def call(self, method: str, payload: dict | None = None) -> dict:
        self.calls.append(method)
        if method == "get_sync_status":
            return {"synced": True}
        if method == "get_height_info":
            return {"height": self.height, "success": True}
        raise AssertionError(f"unexpected wallet RPC {method}")

    def fetch_all_trade_records(self) -> list[dict]:
        return list(self.records)


def _run(monkeypatch, capsys, db: Path, records: list[dict], *args: str,
         height: int = PEAK) -> tuple[int, str, list[str]]:
    """Run main() against `db` and a fake wallet: (exit code, stdout, RPCs made)."""
    calls: list[str] = []
    wallet = type("Wallet", (_FakeWallet,),
                  {"records": records, "height": height, "calls": calls})
    monkeypatch.setattr(sweep, "WalletRpc", wallet)
    monkeypatch.setattr(sys, "argv",
                        ["verify_fill_completeness.py", "--db", str(db), *args])
    code = sweep.main()
    return code, capsys.readouterr().out, calls


def _line(out: str, label: str) -> str:
    for line in out.splitlines():
        if line.strip().startswith(label):
            return line.split(":", 1)[1].strip()
    raise AssertionError(f"no {label!r} line in:\n{out}")


def _section(out: str, header: str) -> str:
    """The block of lines starting at the line that starts with `header`, up to
    the next blank line."""
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if line.startswith(header):
            block = []
            for ln in lines[i:]:
                if not ln.strip():
                    break
                block.append(ln)
            return "\n".join(block)
    raise AssertionError(f"no {header!r} section in:\n{out}")


def _sweep(records: list[dict], **overrides):
    """sweep_records with no window, nothing recorded, and a 10-minute grace."""
    kwargs = {
        "recorded": set(),
        "dead_on_chain": set(),
        "since_height": None,
        "since_time": None,
        "window_height": None,
        "grace_cutoff": time.time() - 600,
    }
    kwargs.update(overrides)
    return sweep.sweep_records(records, **kwargs)


# --- which events exclude an offer ------------------------------------------


def test_dead_offers_are_read_from_the_verdict_event_once_each(tmp_path):
    db = _make_db(tmp_path / "engine.db")
    trade_ids, taker_ids, dead = sweep._load_recorded_ids(db)

    assert trade_ids == {sweep._norm_id(REAL_FILL)}
    assert taker_ids == {sweep._norm_id(REAL_TAKE)}
    # One offer, although it has two events whose reason starts with
    # dead_on_chain; and neither CORRECTION_ONLY nor CORRECTION_EXACT, whose
    # only such event is a correction record, nor OBSERVED_DEAD, whose verdict
    # is a status_observation, is the engine's status_update verdict.
    assert dead == {sweep._norm_id(DEAD)}


def test_a_database_without_closure_events_excludes_nothing(tmp_path):
    db = tmp_path / "old.db"
    con = sqlite3.connect(str(db))
    for ddl in DDL[:2]:
        con.execute(ddl)
    con.commit()
    con.close()

    _, _, dead = sweep._load_recorded_ids(db)
    assert dead == set()


@pytest.mark.parametrize("reason", [
    "dead_on_chain_suspect",                     # longer, with the verdict as prefix
    "dead_on_chain; phantom fill voided: ...",   # the correction's reason on a status_update
    "DEAD_ON_CHAIN",                             # SQLite LIKE ignores case; '=' does not
    "deadXonXchain",                             # '_' is a LIKE wildcard
    " dead_on_chain",
    "dead_on_chain ",
])
def test_only_the_exact_verdict_reason_excludes(monkeypatch, capsys, tmp_path, reason):
    """A status_update whose reason is not exactly 'dead_on_chain' is not the verdict."""
    db = _make_db(tmp_path / "engine.db")
    _add_event(db, NEAR_MISS, "status_update", reason)

    _, _, dead = sweep._load_recorded_ids(db)
    # DEAD's exact verdict is still read, so this is not passing vacuously.
    assert dead == {sweep._norm_id(DEAD)}

    code, out, _ = _run(monkeypatch, capsys, db, [_record(DEAD), _record(NEAR_MISS)])
    assert code == 1, out
    assert NEAR_MISS in _section(out, "Missing CONFIRMED trades")
    assert NEAR_MISS not in _section(out, "WARN: Excluded")


@pytest.mark.parametrize("event_type", [
    "status_observation",       # the verdict on a row that was already closed
    "reconcile_observation",
    "phantom_fill_correction",
    "STATUS_UPDATE",
    "status_update ",
])
def test_only_the_engines_status_update_excludes(monkeypatch, capsys, tmp_path, event_type):
    """The exact verdict reason under any other event type is not the verdict."""
    db = _make_db(tmp_path / "engine.db")
    _add_event(db, NEAR_MISS, event_type, "dead_on_chain")

    _, _, dead = sweep._load_recorded_ids(db)
    assert dead == {sweep._norm_id(DEAD)}

    code, out, _ = _run(monkeypatch, capsys, db, [_record(DEAD), _record(NEAR_MISS)])
    assert code == 1, out
    assert NEAR_MISS in _section(out, "Missing CONFIRMED trades")
    assert NEAR_MISS not in _section(out, "WARN: Excluded")


# --- how exclusions are reported, and --strict --------------------------------


def test_only_dead_offers_unrecorded_passes_and_lists_them_as_excluded(
        monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    records = [_record(REAL_FILL), _record(REAL_TAKE, mine=False), _record(DEAD)]

    code, out, calls = _run(monkeypatch, capsys, db, records)

    # Without --strict an exclusion leaves the exit code alone.
    assert code == sweep.EXIT_PASS == 0, out
    assert "PASS with WARN" in out
    assert _line(out, "excluded (dead_on_chain)") == "1"
    assert _line(out, "MISSING maker fills") == "0"
    assert DEAD in _section(out, "WARN: Excluded")
    assert "Missing CONFIRMED trades" not in out
    # No window: the wallet's height is not asked for.
    assert calls == ["get_sync_status"]


def test_each_excluded_offer_carries_the_command_that_re_proves_it(
        monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    code, out, _ = _run(monkeypatch, capsys, db, [_record(REAL_FILL), _record(DEAD)])

    assert code == 0, out
    warn = _section(out, "WARN: Excluded")
    assert '"VERDICT: Dead"' in warn
    reprove = [ln for ln in warn.splitlines() if "re-prove:" in ln]
    assert len(reprove) == 1, warn
    assert f'"{sweep.PROVE_SCRIPT}" {DEAD}' in reprove[0]
    # The hint names a script this checkout actually has.
    assert sweep.PROVE_SCRIPT.name == "prove_fill_on_chain.py"
    assert sweep.PROVE_SCRIPT.is_file()
    assert "not in this checkout" not in out


def test_strict_exits_nonzero_on_an_exclusion_when_nothing_is_missing(
        monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    records = [_record(REAL_FILL), _record(REAL_TAKE, mine=False), _record(DEAD)]

    code, out, _ = _run(monkeypatch, capsys, db, records, "--strict")

    assert code == sweep.EXIT_STRICT_EXCLUDED == 3, out
    assert DEAD in _section(out, "WARN: Excluded")
    assert "STRICT: nothing is missing, but 1 offer(s) are excluded" in out
    assert "PASS" not in out


def test_strict_still_exits_1_when_something_is_missing(monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    records = [_record(REAL_FILL), _record(DEAD), _record(MISSING)]

    code, out, _ = _run(monkeypatch, capsys, db, records, "--strict")

    assert code == sweep.EXIT_FAIL == 1, out
    assert "FAIL: 1 CONFIRMED wallet trades are unrecorded" in out
    assert DEAD in _section(out, "WARN: Excluded")


def test_strict_passes_when_nothing_is_excluded(monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    records = [_record(REAL_FILL), _record(REAL_TAKE, mine=False)]

    code, out, _ = _run(monkeypatch, capsys, db, records, "--strict")

    assert code == 0, out
    assert "PASS: every CONFIRMED wallet trade in scope is recorded." in out
    assert "WARN" not in out


def test_unrecorded_offers_without_the_verdict_still_fail(monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    records = [
        _record(REAL_FILL),
        _record(REAL_TAKE, mine=False),
        _record(DEAD),
        _record(MISSING),
        _record(CORRECTION_ONLY),
        _record(CORRECTION_EXACT),
        _record(CANCEL_ONLY),
        _record(OBSERVED_DEAD),
    ]

    code, out, _ = _run(monkeypatch, capsys, db, records)

    assert code == 1, out
    assert "FAIL: 5 CONFIRMED wallet trades are unrecorded" in out
    assert _line(out, "excluded (dead_on_chain)") == "1"
    assert _line(out, "MISSING maker fills") == "5"
    missing_section = _section(out, "Missing CONFIRMED trades")
    for tid in (MISSING, CORRECTION_ONLY, CORRECTION_EXACT, CANCEL_ONLY, OBSERVED_DEAD):
        assert tid in missing_section
    assert DEAD not in missing_section
    warn_section = _section(out, "WARN: Excluded")
    assert DEAD in warn_section
    for tid in (MISSING, CORRECTION_ONLY, CORRECTION_EXACT, CANCEL_ONLY, OBSERVED_DEAD):
        assert tid not in warn_section


def test_a_duplicated_wallet_record_is_excluded_once():
    dead = {sweep._norm_id(DEAD)}
    res = _sweep([_record(DEAD), _record(DEAD.upper().replace("0X", "0x"))],
                 dead_on_chain=dead)
    assert len(res.excluded_dead) == 1
    assert res.missing == []


@pytest.mark.parametrize("status", ["PENDING_CANCEL", "CANCELLED", "PENDING_ACCEPT"])
def test_only_confirmed_records_are_classified(status):
    res = _sweep([_record(DEAD, status=status), _record(MISSING, status=status)],
                 dead_on_chain={sweep._norm_id(DEAD)})
    assert res.confirmed == 0
    assert res.excluded_dead == []
    assert res.missing == []


# --- the --since-days window ---------------------------------------------------


def test_the_window_start_height_reaches_back_at_least_n_days():
    # Peak height advances 4,608 blocks a day (18.75 s); the ~52 s figure is
    # transaction-block spacing, and using it here would shrink the window.
    assert sweep.BLOCKS_PER_DAY == 4608
    for days in (0.25, 1, 7, 8, 30):
        start = sweep.window_start_height(PEAK, days)
        nominal = PEAK - days * 4608
        assert start < nominal, days                    # reaches further back...
        assert start >= PEAK - days * 4608 * 1.5, days  # ...but not absurdly far
    assert sweep.window_start_height(1_000, 7) == 0


def test_an_offer_posted_before_the_window_and_taken_inside_it_is_checked():
    since_time = time.time() - 7 * DAY
    start = sweep.window_start_height(PEAK, 7)
    old = 10 * DAY
    ids = {name: "0x" + f"{i:02x}" * 32 for i, name in enumerate(
        ["taken_2d_ago", "taken_9d_ago", "at_start", "below_start",
         "accepted_inside_no_height", "posted_early_no_height"], start=0x21)}
    records = [
        # Maker fills: no accepted_at_time, posted 10 days ago.
        _record(ids["taken_2d_ago"], accepted=False, created_age_s=old,
                height=PEAK - 2 * 4608),
        _record(ids["taken_9d_ago"], accepted=False, created_age_s=old,
                height=PEAK - 9 * 4608),
        _record(ids["at_start"], accepted=False, created_age_s=old, height=start),
        _record(ids["below_start"], accepted=False, created_age_s=old, height=start - 1),
        # No confirmed_at_index: the record's time alone decides.
        _record(ids["accepted_inside_no_height"], age_s=DAY, height=0),
        _record(ids["posted_early_no_height"], accepted=False, created_age_s=old,
                height=0),
    ]

    res = _sweep(records, since_time=since_time, window_height=start)

    assert {r["trade_id"] for r in res.missing} == {
        ids["taken_2d_ago"], ids["at_start"], ids["accepted_inside_no_height"]}
    assert res.confirmed == 6
    assert res.checked_makers == 3


def test_since_days_checks_an_offer_confirmed_inside_the_window(
        monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    records = [
        _record(REAL_FILL),
        # Posted 10 days ago, taken 2 days ago: inside a 7-day window.
        _record(MISSING, accepted=False, created_age_s=10 * DAY, height=PEAK - 2 * 4608),
        # Posted 10 days ago, taken 9 days ago: outside it.
        _record(CANCEL_ONLY, accepted=False, created_age_s=10 * DAY,
                height=PEAK - 9 * 4608),
    ]

    code, out, calls = _run(monkeypatch, capsys, db, records, "--since-days", "7")

    assert code == 1, out
    missing_section = _section(out, "Missing CONFIRMED trades")
    assert MISSING in missing_section
    assert CANCEL_ONLY not in missing_section
    assert _line(out, "checked maker (mine)") == "2"
    assert f"height >= {sweep.window_start_height(PEAK, 7)}" in _line(out, "scope")
    assert "get_height_info" in calls


def test_since_days_without_a_wallet_height_is_an_operational_error(
        monkeypatch, capsys, tmp_path):
    db = _make_db(tmp_path / "engine.db")
    code, out, _ = _run(monkeypatch, capsys, db, [_record(REAL_FILL)],
                        "--since-days", "7", height=0)
    assert code == sweep.EXIT_ERROR == 2
    assert "PASS" not in out


@pytest.mark.parametrize("days", ["0", "-1"])
def test_since_days_must_be_positive(monkeypatch, capsys, tmp_path, days):
    db = _make_db(tmp_path / "engine.db")
    with pytest.raises(SystemExit) as exc:
        _run(monkeypatch, capsys, db, [_record(REAL_FILL)], "--since-days", days)
    assert exc.value.code == 2
