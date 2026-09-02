"""The high/low estimator must not confuse a window median with "now".

`scripts/highlow_spread_estimator.py` reports two raw-spread figures per pair
and they are NOT interchangeable. `Bar.spread_bps` is the MEDIAN of every
quote sample in that day; `Bar.last_spread_bps` is the single most recent
sample. Only the second one answers "is this book dislocated right now".

The distinction was got wrong once: the median was assigned to a column
labelled "obs now" and used as the current posted book spread, so a late-day
dislocation stayed invisible until it dominated half the day's samples -- and
that mislabelled number was quoted to an operator as the live figure in an
argument about whether to re-enable a pair.

Nothing about that failure is type-checked or exercised by running the script,
because both figures are floats in the same units and either produces a
plausible-looking report. These tests pin the semantics directly.
"""

from __future__ import annotations

import importlib.util
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent


def _load_estimator():
    """Import scripts/highlow_spread_estimator.py by path."""
    path = REPO_ROOT / "scripts" / "highlow_spread_estimator.py"
    spec = importlib.util.spec_from_file_location("_hl_est", path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules["_hl_est"] = mod
    spec.loader.exec_module(mod)
    return mod


@pytest.fixture(scope="module")
def hl():
    return _load_estimator()


def _quotes_one_day(day: str, spreads_bps: list[float], mid: float = 1.0):
    """Build (created_at, bid, ask) rows whose spreads are `spreads_bps`.

    Ordered oldest-first, one minute apart, so the LAST entry is
    unambiguously the most recent sample of that day.
    """
    rows = []
    for i, s in enumerate(spreads_bps):
        half = mid * (s / 2.0) / 10000.0
        rows.append((f"{day} 00:{i:02d}:00", mid - half, mid + half))
    return rows


def test_last_spread_is_the_final_sample_not_the_median(hl):
    # Four calm samples then one violent one. The median stays ~10 bps; the
    # last sample is 5,000. A tool reporting the median as "now" would show
    # 10 and hide the dislocation entirely.
    quotes = _quotes_one_day("2026-09-01", [10.0, 10.0, 10.0, 10.0, 5000.0])
    bars = hl.build_bars(quotes, "quote-touch")
    assert len(bars) == 1
    bar = bars[0]

    assert bar.last_spread_bps == pytest.approx(5000.0, rel=1e-6), (
        "last_spread_bps must be the FINAL sample of the day"
    )
    assert bar.spread_bps < 100.0, (
        "spread_bps must remain the day's MEDIAN, unmoved by one late sample"
    )
    assert bar.last_spread_bps != bar.spread_bps


def test_last_sample_timestamp_matches_the_final_sample(hl):
    quotes = _quotes_one_day("2026-09-01", [10.0, 10.0, 5000.0])
    bar = hl.build_bars(quotes, "quote-touch")[0]
    assert bar.last_sample_at == "2026-09-01 00:02:00"


def test_ordering_is_enforced_not_assumed(hl):
    # The three "newest sample" fields would silently degrade to "whichever
    # row the query returned last" if the caller's ORDER BY were dropped or
    # two sources concatenated. Feed the samples SHUFFLED and require the
    # same answer as the ordered case.
    ordered = _quotes_one_day("2026-09-01", [10.0, 20.0, 30.0, 5000.0])
    shuffled = [ordered[2], ordered[0], ordered[3], ordered[1]]

    a = hl.build_bars(ordered, "quote-touch")[0]
    b = hl.build_bars(shuffled, "quote-touch")[0]

    assert a.last_spread_bps == pytest.approx(b.last_spread_bps)
    assert a.last_sample_at == b.last_sample_at == "2026-09-01 00:03:00"


def test_dislocation_flag_reads_the_last_sample(hl):
    # A day that is calm on median but dislocated at the end must flag.
    late = hl.build_bars(
        _quotes_one_day("2026-09-01", [10.0] * 8 + [9000.0]), "quote-touch"
    )[0]
    assert late.dislocated, (
        "the flag must read the last sample -- this is the late-day "
        "dislocation the median hides"
    )
    assert not late.median_dislocated, (
        "and the median must NOT flag here, or the fixture proves nothing"
    )

    # The mirror: dislocated for most of the day but calm at the close.
    early = hl.build_bars(
        _quotes_one_day("2026-09-01", [9000.0] * 8 + [10.0]), "quote-touch"
    )[0]
    assert not early.dislocated
    assert early.median_dislocated


def test_median_and_last_are_reported_as_separate_fields(hl):
    # Guard against a future refactor collapsing them back into one value.
    bar = hl.build_bars(
        _quotes_one_day("2026-09-01", [10.0, 10.0, 5000.0]), "quote-touch"
    )[0]
    for field in ("spread_bps", "last_spread_bps", "last_sample_at"):
        assert hasattr(bar, field), f"Bar lost the {field} field"


# ==========================================================================
# Across the Bar boundary: estimate_pair and the reporting layer
# ==========================================================================
#
# Everything above this line stops at build_bars and asserts on Bar
# attributes.  That is not enough, and a shipped bug proved it: the
# DISCREPANCY block printed "No discrepancy to look into." for XCH/BYC three
# lines above a DISLOCATION FLAG note reporting the same book at 14,667 bps,
# and every test in this file stayed green through it.  Bar can be perfect
# and estimate_pair can still read the wrong field off it, or print_report
# can read the right field and draw the wrong conclusion from it.
#
# So these tests cross the boundary.  They pin, at the layers that actually
# talk to an operator:
#
#   * estimate_pair maps the LAST SAMPLE to the current spread and the
#     MEDIAN to the window figure, and never the other way round;
#   * the dislocation verdict estimate_pair publishes reads the last sample;
#   * print_report never emits an all-clear about a book carrying the
#     dislocation flag -- and still emits one where that is the truth.
#
# No database is involved: estimate_pair takes bars, and print_report takes
# the dict estimate_pair returns.


def _ba(mid: float, spread_bps: float) -> tuple[float, float]:
    """(bid, ask) around *mid* carrying exactly *spread_bps* of raw spread."""
    half = mid * (spread_bps / 2.0) / 10000.0
    return (mid - half, mid + half)


def _quotes_ending_today(days: list[list[tuple[float, float]]]):
    """(created_at, bid, ask) rows for CONSECUTIVE days ending TODAY (UTC).

    Ending today is not cosmetic. estimate_pair's freshness gate ages the
    newest usable bar against datetime.now(), so a fixture pinned to literal
    calendar dates would pass on the day it was written and be REFUSED every
    day after -- these tests would rot into vacuous assertions about a
    refusal while appearing to still check the estimates.

    Five days of samples gives four adjacent day pairs, which is exactly
    MIN_USABLE_DAY_PAIRS: with fewer, every test below would be asserting
    against a sample-gate refusal instead of against an estimate.
    """
    today = datetime.now(UTC).date()
    span = len(days)
    rows = []
    for k, samples in enumerate(days):
        stamp = (today - timedelta(days=span - 1 - k)).isoformat()
        for i, (bid, ask) in enumerate(samples):
            rows.append((f"{stamp} {i:02d}:00:00", bid, ask))
    return rows


def _estimate(hl, days, pair="XCH/BYC"):
    bars = hl.build_bars(_quotes_ending_today(days), "quote-touch")
    return hl.estimate_pair(pair, bars, 0, 2.0, "offer_log", 30.0)


def _report_text(hl, capsys, result) -> str:
    """print_report output with whitespace collapsed.

    The body is wrapped at 74 columns, so any phrase worth asserting on is
    liable to be split across a newline. Collapsing makes these assertions
    about the WORDS rather than about where the wrapper happened to break.
    """
    hl.print_report([result], [], "offer_log", "quote-touch", 30.0, 2.0)
    return " ".join(capsys.readouterr().out.split())


def test_estimate_pair_reports_the_last_sample_as_the_current_spread(hl):
    # Four calm days, then a day that is calm for four samples and violent
    # on the fifth. Only a tool reading the LAST SAMPLE sees the 5,000.
    calm = [_ba(1.0, 10.0)] * 4
    days = [calm] * 4 + [calm + [_ba(1.0, 5000.0)]]
    r = _estimate(hl, days)

    assert r["refused"] is None, r["refused"]

    assert r["observed_spread_last_sample_bps"] == pytest.approx(
        5000.0, rel=1e-6), (
        "the CURRENT spread must be the last sample, not any median"
    )
    assert r["observed_spread_last_sample_at"].endswith("04:00:00"), (
        "and it must be stamped with that sample's own instant"
    )
    # The two medians are the WINDOW figures and must be untouched by one
    # late sample. If either had moved to 5,000 the fields are conflated.
    assert r["latest_raw_bar_median_bps"] < 100.0, (
        "latest_raw_bar_median_bps is the newest DAY's median, not now"
    )
    assert r["observed_spread_window_median_bps"] < 100.0, (
        "observed_spread_window_median_bps summarises the WINDOW, not now"
    )


def test_estimate_pair_dislocation_verdict_reads_the_last_sample(hl):
    calm_day = [_ba(1.0, 10.0)] * 3

    # Late dislocation: the median cannot see it, the last sample can.
    late = _estimate(hl, [calm_day] * 4
                     + [[_ba(1.0, 10.0)] * 8 + [_ba(1.0, 9000.0)]])
    assert late["refused"] is None, late["refused"]
    assert late["dislocated"] is True, (
        "the published verdict must read the last sample"
    )
    assert late["dislocated_by_median"] is False, (
        "and the median must NOT flag here, or the fixture proves nothing"
    )
    assert any("DISLOCATION FLAG" in n for n in late["notes"])

    # The mirror: dislocated for most of the day, calm at the close.
    early = _estimate(hl, [calm_day] * 4
                      + [[_ba(1.0, 9000.0)] * 8 + [_ba(1.0, 10.0)]])
    assert early["refused"] is None, early["refused"]
    assert early["dislocated"] is False
    assert early["dislocated_by_median"] is True
    assert not any("DISLOCATION FLAG" in n for n in early["notes"])


def test_discrepancy_never_all_clears_a_dislocated_book(hl, capsys):
    """The XCH/BYC contradiction, reproduced end to end.

    Bids pinned near 1.50 against asks near 5.00 -- the real shape -- puts
    the posted spread and the two-day estimate at the SAME magnitude, so the
    ratio lands at or below 1.0 while the dislocation flag is set. That is
    the exact combination that used to print an all-clear.
    """
    day_samples = [(1.50, 4.99), (1.52, 5.05), (1.48, 4.95)]
    r = _estimate(hl, [day_samples] * 5)

    assert r["refused"] is None, r["refused"]
    assert r["dislocated"] is True
    recent = max(r["latest_pair_cs_bps"] or 0.0,
                 r["latest_pair_ar_bps"] or 0.0)
    assert recent > 0.0
    ratio = r["observed_spread_last_sample_bps"] / recent
    assert ratio <= 1.0, (
        f"fixture must land in the ratio<=1.0 branch to prove anything, "
        f"got {ratio:.3f}x"
    )

    out = _report_text(hl, capsys, r)

    assert "No discrepancy to look into" not in out, (
        "an all-clear must NEVER be printed about a book carrying the "
        "dislocation flag -- that is the self-contradicting output"
    )
    assert "NOT AN ALL-CLEAR" in out
    assert "ESTIMATOR AGREES THE BOOK IS WIDE" in out, (
        "the branch must say what a small ratio MEANS here: the estimator "
        "agrees the book is wide, not that the book is fine"
    )
    # The magnitude and the threshold, so the reader can see why.
    assert f"{r['observed_spread_last_sample_bps']:,.1f} bps" in out
    assert "dislocation threshold" in out
    assert "DISLOCATION FLAG" in out


def test_discrepancy_still_all_clears_an_undislocated_book(hl, capsys):
    """The fix must not simply delete the all-clear.

    A genuinely narrow book (8 bps) whose mid swings intraday produces a
    positive two-day estimate far above the posted spread: ratio well below
    1.0, dislocation flag CLEAR. Here the all-clear is the truth and must
    still be printed -- otherwise the previous test is satisfiable by
    deleting the branch rather than by fixing it.
    """
    swing = [_ba(0.97, 8.0), _ba(1.03, 8.0), _ba(1.00, 8.0)]
    r = _estimate(hl, [swing] * 5, pair="XCH/DBX")

    assert r["refused"] is None, r["refused"]
    assert r["dislocated"] is False
    recent = max(r["latest_pair_cs_bps"] or 0.0,
                 r["latest_pair_ar_bps"] or 0.0)
    assert recent > 0.0
    ratio = r["observed_spread_last_sample_bps"] / recent
    assert ratio <= 1.0, f"fixture must land in the same branch, got {ratio}"

    out = _report_text(hl, capsys, r)

    assert "No discrepancy to look into" in out
    assert "NOT AN ALL-CLEAR" not in out
    assert "DISLOCATION FLAG" not in out


# ==========================================================================
# The dexie freshness gate
# ==========================================================================


def _dexie_payload(hl, age: timedelta) -> dict:
    stamped = (datetime.now(UTC) - age).isoformat()
    return {"markets": {hl.ASSET_IDS["XCH"]: [{
        "id": hl.ASSET_IDS["BYC"],
        "prices": {
            "high": {"daily": 4.0},
            "low": {"daily": 2.0},
            "last": {"price": 3.0, "date": stamped},
        },
    }]}}


def test_dexie_age_is_fractional_not_floored_to_whole_days(hl):
    # 2 days 23 hours. timedelta.days FLOORS that to 2, and 2 > 2.0 is
    # False, so the print sailed through a limit documented as 2 days.
    row = hl.dexie_high_low(
        _dexie_payload(hl, timedelta(days=2, hours=23)), "XCH/BYC")

    assert row["available"] is True
    assert row["last_trade_age_days"] == pytest.approx(2.958, abs=0.01), (
        "the age must be FRACTIONAL days -- flooring it to 2 is what let a "
        "2.96-day-old print pass a 2.0-day limit"
    )
    assert row["stale"] is True, (
        "2.96 days is older than the 2.0-day default limit and must refuse"
    )


def test_dexie_freshness_gate_uses_the_operator_max_age_days(hl):
    payload = _dexie_payload(hl, timedelta(days=2, hours=23))

    # The operator asked for 5 days and the report header prints 5, so the
    # gate must apply 5 rather than reaching for the module constant.
    lenient = hl.dexie_high_low(payload, "XCH/BYC", 5.0)
    assert lenient["stale"] is False
    assert lenient["max_age_days"] == 5.0

    # And the other direction, so this is not just a wider limit passing.
    strict = hl.dexie_high_low(payload, "XCH/BYC", 1.0)
    assert strict["stale"] is True
    assert strict["max_age_days"] == 1.0
