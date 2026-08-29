#!/usr/bin/env python3
"""The carried-accrual analyzer must never certify what it did not observe.

`scripts/permuto_depth_analyze.py` produced the CONFIRMED result recorded for
C-0S3 in `TODO-COMPETITION.md`, and the whole eligibility plan is sized off
the dollar figure it prints. A false CONFIRMED is therefore the expensive
direction to be wrong in, and the tool had three independent routes to one:

  - a flat stretch of LIVE trading that happens to close the file reads
    exactly like a post-close freeze, and the selector certified it;
  - the sub-window corroboration that exists to reject one-off leaderboard
    backfill was computed over the whole session, so gains earned during live
    trading unlocked a verdict about a two-minute carried window;
  - one truncated JSONL record welded two probe runs into a single session,
    so an oracle transition observed by a different process hours earlier
    certified a later run's window.

These tests pin all three shut, plus the parsing and joining invariants the
verdict depends on.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent

# 16:00 ET on 2026-08-27, the cash close the recorded experiment straddled.
CLOSE = datetime(2026, 8, 27, 20, 0, tzinfo=UTC)

ORACLE_X = {"QQQ-VOL": 9.13}
ORACLE_Y = {"QQQ-VOL": 9.44}
ORACLE_Z = {"QQQ-VOL": 10.02}

A = "7c81d1c9deadbeef"
# Two accounts sharing the eight-character display prefix. Joining on the
# display form would silently merge them into one delta between two different
# accounts, which is worse than no answer.
P1 = "abcdefgh11111111"
P2 = "abcdefgh22222222"


def _load_analyzer():
    """Import scripts/permuto_depth_analyze.py by path -- scripts/ is not a package."""
    path = REPO_ROOT / "scripts" / "permuto_depth_analyze.py"
    spec = importlib.util.spec_from_file_location("xop_permuto_depth_analyze_test", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["xop_permuto_depth_analyze_test"] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop("xop_permuto_depth_analyze_test", None)
        raise
    return module


analyze = _load_analyzer()


def _ts(minutes):
    return (CLOSE + timedelta(minutes=minutes)).isoformat()


def _header(minutes=0, interval_s=60):
    return {
        "probe_start": _ts(minutes),
        "interval_s": interval_s,
        "stop_at": _ts(minutes + 240),
        "close_note": "20:00 UTC == 16:00 ET (EDT, UTC-4)",
    }


def _row(minutes, oracle=None, depths=None):
    """One probe sample. `depths` maps FULL user_id -> depth_seconds_5d."""
    row = {"ts": _ts(minutes)}
    if oracle is not None:
        row["oracle"] = oracle
    if depths is not None:
        row["mms"] = [
            {"user_id": uid, "u": uid[:8], "d5": d5, "d24": d5,
             "eq": 1000.0, "pnl": 0.0, "elig": True, "trades": 1}
            for uid, d5 in depths.items()
        ]
    return row


def _write(tmp_path, *records):
    path = tmp_path / "probe.jsonl"
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for rec in records:
            fh.write(rec if isinstance(rec, str) else json.dumps(rec))
            fh.write("\n")
    return path


def _run(monkeypatch, capsys, path, *args):
    monkeypatch.setattr(sys, "argv", ["permuto_depth_analyze.py", str(path), *args])
    analyze.main()
    return capsys.readouterr()


# --- parsing -------------------------------------------------------------


def test_each_header_starts_a_session_and_only_the_last_is_analysed(
    tmp_path, monkeypatch, capsys
):
    path = _write(
        tmp_path,
        _header(0),
        _row(1, ORACLE_X, {A: 1_000}),
        _row(2, ORACLE_X, {A: 2_000}),
        _header(180),
        _row(181, ORACLE_Y, {A: 9_000}),
        _row(182, ORACLE_Y, {A: 9_500}),
    )
    sessions, malformed = analyze.load_sessions(path)
    assert malformed == 0
    assert [len(s["rows"]) for s in sessions] == [2, 2]

    out = _run(monkeypatch, capsys, path).out
    assert "2 probe sessions" in out
    assert "samples      : 2 in session" in out
    # Both sessions have two rows, so the counts above cannot tell which one
    # was analysed. The printed session header can, and it is the only thing
    # naming the samples the verdict was computed from.
    assert f"session      : {_ts(180)[:19]}" in out
    assert _ts(0)[:19] not in out


def test_a_truncated_final_record_does_not_discard_the_samples_before_it(tmp_path):
    """A probe killed mid-write must cost one sample, not the whole run."""
    path = _write(
        tmp_path,
        _header(0),
        _row(1, ORACLE_X, {A: 1_000}),
        _row(2, ORACLE_X, {A: 2_000}),
        _row(3, ORACLE_X, {A: 3_000}),
        '{"ts": "2026-08-27T20:04:00+00:00", "ora',
    )
    sessions, malformed = analyze.load_sessions(path)
    assert malformed == 1
    assert len(sessions) == 1
    assert len(sessions[0]["rows"]) == 3


def test_a_truncated_record_breaks_the_session_rather_than_welding_two_runs(
    tmp_path, monkeypatch, capsys
):
    """The probe appends, so a killed run's partial line absorbs the next header.

    Skipping the unreadable line and carrying on rejoined the two runs, and
    the hours of unobserved downtime between them then counted as continuous
    evidence -- the exact failure load_sessions exists to prevent.
    """
    path = _write(
        tmp_path,
        _header(-180),
        _row(-179, ORACLE_X, {A: 1_000}),
        _row(-178, ORACLE_Y, {A: 2_000}),
        '{"ts": "2026-08-27T17:03:' + json.dumps(_header(180)),
        _row(181, ORACLE_Z, {A: 3_000}),
        _row(182, ORACLE_Z, {A: 4_000}),
    )
    sessions, malformed = analyze.load_sessions(path)
    assert malformed == 1
    assert len(sessions) == 2
    assert sessions[-1]["truncated"] is True
    assert [r["ts"] for r in sessions[-1]["rows"]] == [_ts(181), _ts(182)]

    # The count belongs next to the verdict, not only on stderr: whoever reads
    # the verdict is not reading the terminal's error stream.
    out = _run(monkeypatch, capsys, path).out
    assert "unreadable record" in out


# --- window selection ----------------------------------------------------


def test_frozen_window_refuses_a_flat_run_that_is_not_the_file_suffix():
    rows = ([_row(i, ORACLE_Y) for i in range(3)]
            + [_row(3 + i, ORACLE_X) for i in range(4)]
            + [_row(8)])  # oracle fetch failed: the run ends before the file does
    window, provenance = analyze.frozen_window(rows, 60)
    assert window == []
    assert provenance is None


def test_frozen_window_refuses_a_file_whose_oracle_never_moves():
    """No observed transition means no observed freeze."""
    rows = [_row(i, ORACLE_X) for i in range(6)]
    window, provenance = analyze.frozen_window(rows, 60)
    assert window == []
    assert provenance is None


def test_an_earlier_longer_flat_run_does_not_beat_the_trailing_one():
    """A quiet stretch of LIVE trading is routinely longer than the post-close run.

    The selector's own comment says "the most recent completed run, not the
    longest", and every fixture until now happened to have the trailing run
    be the longest as well -- so picking the longest passed the suite while
    handing back a window that is not the freeze the oracle most recently
    entered.
    """
    rows = [_row(i, ORACLE_Y) for i in range(10)]
    rows += [_row(10 + i, ORACLE_X) for i in range(4)]

    window, provenance = analyze.frozen_window(rows, 60)
    assert [r["ts"] for r in window] == [_ts(10), _ts(11), _ts(12), _ts(13)]
    assert provenance == "inferred"


def test_frozen_window_takes_the_run_after_the_last_transition():
    rows = [_row(i, ORACLE_Y) for i in range(3)] + [_row(3 + i, ORACLE_X) for i in range(4)]
    window, provenance = analyze.frozen_window(rows, 60)
    assert [r["ts"] for r in window] == [_ts(3), _ts(4), _ts(5), _ts(6)]
    assert provenance == "inferred"


def test_a_missing_oracle_reading_breaks_the_run():
    rows = [_row(0, ORACLE_Y), _row(1, ORACLE_X), _row(2, ORACLE_X), _row(3),
            _row(4, ORACLE_X), _row(5, ORACLE_X), _row(6, ORACLE_X)]
    window, _ = analyze.frozen_window(rows, 60)
    assert [r["ts"] for r in window] == [_ts(4), _ts(5), _ts(6)]


def test_a_sampling_gap_breaks_the_run():
    """Equality either side of a hole never proved the oracle held still inside it."""
    rows = [_row(0, ORACLE_Y), _row(1, ORACLE_X), _row(2, ORACLE_X),
            _row(20, ORACLE_X), _row(21, ORACLE_X), _row(22, ORACLE_X)]
    window, _ = analyze.frozen_window(rows, 60)
    assert [r["ts"] for r in window] == [_ts(20), _ts(21), _ts(22)]


def test_carried_since_narrows_the_window_and_records_its_provenance():
    rows = [_row(0, ORACLE_Y)] + [_row(i, ORACLE_X) for i in range(1, 7)]
    window, provenance = analyze.frozen_window(rows, 60, CLOSE + timedelta(minutes=4))
    assert [r["ts"] for r in window] == [_ts(4), _ts(5), _ts(6)]
    assert provenance == "asserted"


def test_carried_since_past_the_data_yields_no_window():
    rows = [_row(0, ORACLE_Y)] + [_row(i, ORACLE_X) for i in range(1, 7)]
    window, provenance = analyze.frozen_window(rows, 60, CLOSE + timedelta(minutes=6))
    assert window == []
    assert provenance is None


def test_an_oracle_move_inside_the_asserted_window_refutes_the_assertion():
    """The assertion and the data disagree, and only the operator can settle it.

    Filtering the already-selected trailing run by the boundary made an
    oracle transition AFTER the boundary silently narrow the window to the
    island on the far side of it -- so the samples showing the operator was
    wrong were the ones discarded, and the tool certified what was left and
    printed sizing off it.
    """
    rows = [_row(-1, ORACLE_Y)]
    rows += [_row(i, ORACLE_X) for i in range(0, 5)]
    rows += [_row(i, ORACLE_Z) for i in range(5, 10)]

    window, provenance = analyze.frozen_window(rows, 60, CLOSE)
    assert window == []
    assert provenance.startswith("contradicted")
    assert "MOVED" in provenance
    assert _ts(5) in provenance


def test_a_gap_inside_the_asserted_window_refutes_the_assertion():
    """Equality either side of a hole never proved the oracle held still."""
    rows = [_row(-1, ORACLE_Y)] + [_row(i, ORACLE_X) for i in (0, 1, 2, 20, 21)]

    window, provenance = analyze.frozen_window(rows, 60, CLOSE)
    assert window == []
    assert provenance.startswith("contradicted")
    assert "missing coverage" in provenance


def test_a_missing_oracle_inside_the_asserted_window_refutes_the_assertion():
    rows = [_row(-1, ORACLE_Y), _row(0, ORACLE_X), _row(1), _row(2, ORACLE_X)]

    window, provenance = analyze.frozen_window(rows, 60, CLOSE)
    assert window == []
    assert provenance.startswith("contradicted")
    assert "no oracle reading" in provenance


def test_a_refuted_assertion_is_reported_as_such_not_as_inconclusive(
    tmp_path, monkeypatch, capsys
):
    """"Nothing qualified" and "your assertion is wrong" are different answers."""
    rows = [_row(-1, ORACLE_Y, {A: 0})]
    rows += [_row(i, ORACLE_X, {A: 1_000 * i}) for i in range(0, 5)]
    rows += [_row(i, ORACLE_Z, {A: 1_000 * i}) for i in range(5, 10)]
    path = _write(tmp_path, _header(-1), *rows)

    out = _run(monkeypatch, capsys, path, f"--carried-since={_ts(0)}").out
    assert "REFUTED" in out
    assert "moved" in out.lower()
    assert "STRONG EVIDENCE" not in out
    # And it must not report a window it built out of the surviving island.
    assert "impl. $depth" not in out


# --- what the verdict is allowed to claim --------------------------------


def test_a_flat_tail_alone_cannot_certify_a_carried_session(tmp_path, monkeypatch, capsys):
    """Every sample here is an hour BEFORE the cash close, so nothing is carried.

    The oracle merely goes quiet for the last thirty minutes while one account
    accrues steadily through live trading. "Flat and something different came
    before it" is satisfied by ordinary intraday movement, so the tool used to
    print CONFIRMED -- and the dollar-sizing extrapolation with it.
    """
    rows = [_row(-120 + i, {"QQQ-VOL": 30 + i}, {A: 1_000 * i}) for i in range(30)]
    rows += [_row(-90 + i, ORACLE_X, {A: 30_000 + 1_000 * i}) for i in range(30)]
    path = _write(tmp_path, _header(-120), *rows)

    out = _run(monkeypatch, capsys, path).out
    assert "STRONG EVIDENCE" not in out
    assert "NOT CERTIFIED" in out
    # And it must say how to certify, rather than only that it will not.
    assert "--carried-since" in out


def test_an_asserted_boundary_still_certifies_a_genuine_carried_run(
    tmp_path, monkeypatch, capsys
):
    """The C-0S3-shaped run: the operator supplies the boundary the venue will not."""
    rows = [_row(-120 + i, {"QQQ-VOL": 30 + i}, {A: 1_000 * i}) for i in range(30)]
    rows += [_row(-90 + i, ORACLE_X, {A: 30_000 + 1_000 * i}) for i in range(30)]
    path = _write(tmp_path, _header(-120), *rows)

    out = _run(monkeypatch, capsys, path, f"--carried-since={_ts(-90)}").out
    assert "VERDICT: STRONG EVIDENCE" in out
    # [review] And it must NOT overclaim: incremental backfill is
    # observationally identical at this sampling rate.
    assert "NOT CONFIRMED" in out
    assert "ASSERTED" in out


def test_live_accrual_outside_the_window_cannot_corroborate_a_carried_gain(
    tmp_path, monkeypatch, capsys
):
    """36 live samples of steady accrual, then a carried tail holding one credit.

    A single credit inside the window is the leaderboard-backfill fingerprint
    the sub-window check was written to reject. Bucketing the whole session
    instead of the window let thirty-six live samples supply the corroboration
    for a four-minute carried window -- on the operator-asserted path.
    """
    rows = [_row(i, {"QQQ-VOL": 30 + i}, {A: 1_000 * i}) for i in range(36)]
    rows += [_row(36, ORACLE_X, {A: 35_000})]
    rows += [_row(36 + i, ORACLE_X, {A: 535_000}) for i in range(1, 4)]
    path = _write(tmp_path, _header(0), *rows)

    out = _run(monkeypatch, capsys, path, f"--carried-since={_ts(36)}").out
    assert "STRONG EVIDENCE" not in out
    assert "EVIDENCE, NOT CONFIRMATION" in out


def test_corroboration_is_bucketed_over_the_window_not_the_session(
    tmp_path, monkeypatch, capsys
):
    """A window long enough to bucket on its own still must be bucketed on its own.

    Sixty live samples then a thirty-sample carried tail that steps twice.
    Session-wide buckets land those two steps in separate buckets and call it
    sustained; the window's own buckets show a third of it flat.
    """
    rows = [_row(i, {"QQQ-VOL": 30 + i}, {A: 1_000 * i}) for i in range(60)]
    tail = [59_000] * 12 + [259_000] * 6 + [459_000] * 12
    rows += [_row(60 + i, ORACLE_X, {A: d5}) for i, d5 in enumerate(tail)]
    path = _write(tmp_path, _header(0), *rows)

    out = _run(monkeypatch, capsys, path, f"--carried-since={_ts(60)}").out
    assert "STRONG EVIDENCE" not in out
    assert "EVIDENCE, NOT CONFIRMATION" in out


def test_one_accounts_steady_rise_cannot_vouch_for_anothers_single_jump(
    tmp_path, monkeypatch, capsys
):
    """Corroboration has to come from the same ACCOUNT, not just the same window.

    `sustained` and `risers` were once only tested for non-emptiness, so a
    small steady riser unlocked STRONG EVIDENCE for a different account whose
    entire gain arrived in one step -- the leaderboard-backfill fingerprint --
    and the headline dollar figure was then quoted from that unchecked jump.
    """
    rows = [_row(-1, ORACLE_Y, {P1: 0, P2: 0})]
    # P1 rises in every sub-window; P2 is flat but for one 500,000 step.
    for i in range(30):
        rows.append(_row(i, ORACLE_X,
                         {P1: 1_000 * i, P2: 0 if i < 15 else 500_000}))
    path = _write(tmp_path, _header(-1), *rows)

    out = _run(monkeypatch, capsys, path, f"--carried-since={_ts(0)}").out
    assert "VERDICT: STRONG EVIDENCE" in out
    assert "1 of 2 accounts gained depth in every sub-window" in out
    # 29,000 depth-seconds over 1,740 s is ~$17 of resting depth. P2's
    # single 500,000 step over the same span reads as ~$287 in the per-account
    # table, and must not become the corroborated headline rate.
    assert "$17 of balanced depth" in out
    assert "$287 of balanced depth" not in out


def test_a_naive_carried_since_is_refused_before_the_run_starts(
    tmp_path, monkeypatch, capsys
):
    """A perfectly reasonable-looking argument used to fail deep in a compare.

    The rows are timezone-aware UTC, so a naive boundary raised TypeError
    inside the window scan -- in the observer's case after the output file
    already existed. It has to be rejected where the message can say what to
    type.
    """
    path = _write(tmp_path, _header(0),
                  _row(1, ORACLE_X, {A: 1_000}), _row(2, ORACLE_X, {A: 2_000}))

    with pytest.raises(SystemExit) as caught:
        _run(monkeypatch, capsys, path, "--carried-since=2026-08-27T20:00:00")
    assert "no UTC offset" in str(caught.value)


def test_an_unavailable_24h_reading_is_not_reported_as_a_swing(
    tmp_path, monkeypatch, capsys
):
    """Coercing a missing endpoint to 0 invents a delta the size of the other."""
    rows = [_row(0, ORACLE_Y, {A: 1_000}), _row(1, ORACLE_X, {A: 2_000}),
            _row(2, ORACLE_X, {A: 3_000})]
    for row in rows:
        row["mms"][0]["d24"] = None
    path = _write(tmp_path, _header(0), *rows)

    out = _run(monkeypatch, capsys, path).out
    assert "unavailable" in out
    assert "-3000" not in out and "+3000 " not in out


def test_accounts_sharing_a_display_prefix_get_independent_deltas(
    tmp_path, monkeypatch, capsys
):
    rows = [_row(0, ORACLE_Y, {P1: 0, P2: 0})]
    rows += [_row(i, ORACLE_X, {P1: 1_000 * i, P2: 7_000 * i}) for i in range(1, 5)]
    path = _write(tmp_path, _header(0), *rows)

    out = _run(monkeypatch, capsys, path).out
    assert out.count("abcdefgh") == 2
    assert "+3000" in out
    assert "+21000" in out


def test_a_bracketed_transition_row_is_excluded_from_the_frozen_window(tmp_path):
    """[review] The guard existed and had no callers.

    The probe reads the leaderboard first and the oracle after, so around the
    close one row can pair a LIVE leaderboard snapshot with an oracle already
    showing the frozen value. That row opened the frozen run and contributed
    live accrual to a carried verdict -- the exact false attribution the
    capture brackets were added to prevent.
    """
    mod = _load_analyzer()
    rows = []
    # Three live samples, then the straddling row, then the frozen tail.
    for i in range(3):
        rows.append({"ts": "2026-08-27T19:5%d:00+00:00" % i,
                     "oracle": {"QQQ-VOL": 0.07 + i * 0.001},
                     "t_leaderboard_start": "2026-08-27T19:5%d:00+00:00" % i,
                     "t_oracle": "2026-08-27T19:5%d:03+00:00" % i,
                     "leaderboard": []})
    straddle = {"ts": "2026-08-27T19:59:59+00:00",
                "oracle": {"QQQ-VOL": 0.09},
                "t_leaderboard_start": "2026-08-27T19:59:58+00:00",
                "t_oracle": "2026-08-27T20:00:02+00:00",
                "leaderboard": []}
    rows.append(straddle)
    for i in range(4):
        rows.append({"ts": "2026-08-27T20:0%d:00+00:00" % i,
                     "oracle": {"QQQ-VOL": 0.09},
                     "t_leaderboard_start": "2026-08-27T20:0%d:00+00:00" % i,
                     "t_oracle": "2026-08-27T20:0%d:03+00:00" % i,
                     "leaderboard": []})

    window, _prov = mod.frozen_window(rows, 60)
    assert window, "no frozen window was found at all"
    assert straddle not in window, "the straddling row entered the window"
    assert all(r["ts"] >= "2026-08-27T20:00:00+00:00" for r in window)


def test_an_unbracketed_transition_row_is_still_kept(tmp_path):
    """Older sessions carry no capture timestamps.

    Treating "unknown" as "straddling" would retroactively void every
    historical sample, which is a stronger claim than the evidence supports.
    """
    mod = _load_analyzer()
    rows = [{"ts": "2026-08-27T19:59:00+00:00", "oracle": {"Q": 0.07},
             "leaderboard": []}]
    rows += [{"ts": "2026-08-27T20:0%d:00+00:00" % i, "oracle": {"Q": 0.09},
              "leaderboard": []} for i in range(4)]

    window, _prov = mod.frozen_window(rows, 60)
    assert len(window) == 4
