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
