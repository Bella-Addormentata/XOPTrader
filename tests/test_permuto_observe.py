"""The observer's one derived value, and the row it is recorded in.

`_ring_depth` produces `credit_usd`, which is the number every depth-sizing
conclusion in TODO-COMPETITION rests on, and it cannot be recomputed later
because the ladder it comes from is not kept. It had no tests.

The row shape matters for the same reason: a recorded zero and an
unrecorded book are different observations, and only one of them can be
averaged.
"""

from __future__ import annotations

import importlib.util
import json
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]


def _load():
    """Import scripts/permuto_observe.py by path -- scripts/ is not a package."""
    path = REPO_ROOT / "scripts" / "permuto_observe.py"
    spec = importlib.util.spec_from_file_location(
        "xop_permuto_observe_test", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["xop_permuto_observe_test"] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def obs():
    return _load()


def _lvl(price, size):
    return {"price": price, "size": size}


# --------------------------------------------------------------------------- #
# The rule the venue actually applies
# --------------------------------------------------------------------------- #

def test_credit_is_the_minimum_of_the_two_sides(obs):
    """Depth accrues on min(bid, ask). Not either side, and not the sum --
    which is why a one-sided book earns nothing at all."""
    ring = obs._ring_depth(
        bids=[_lvl(99.0, 10)],      # 990 notional
        asks=[_lvl(101.0, 1)],      # 101 notional
        oracle=100.0,
    )
    assert ring["bid_usd"] == pytest.approx(990.0)
    assert ring["ask_usd"] == pytest.approx(101.0)
    assert ring["credit_usd"] == pytest.approx(101.0)


def test_a_one_sided_book_earns_nothing(obs):
    ring = obs._ring_depth(bids=[_lvl(99.0, 10)], asks=[], oracle=100.0)
    assert ring["credit_usd"] == 0.0
    assert ring["ask_levels"] == 0


def test_levels_outside_the_ring_are_excluded(obs):
    ring = obs._ring_depth(
        bids=[_lvl(99.0, 1), _lvl(90.0, 1000)],   # second is -10%
        asks=[_lvl(101.0, 1), _lvl(110.0, 1000)],
        oracle=100.0,
    )
    assert ring["bid_levels"] == 1
    assert ring["ask_levels"] == 1
    assert ring["credit_usd"] == pytest.approx(99.0)


def test_the_two_percent_boundary_is_inclusive(obs):
    """Exactly +/-2% counts. The venue measures `within` the ring, and a
    level sitting on the boundary is inside it."""
    ring = obs._ring_depth(
        bids=[_lvl(98.0, 1)], asks=[_lvl(102.0, 1)], oracle=100.0)
    assert ring["bid_levels"] == 1
    assert ring["ask_levels"] == 1


def test_just_outside_the_boundary_is_excluded(obs):
    ring = obs._ring_depth(
        bids=[_lvl(97.9, 1)], asks=[_lvl(102.1, 1)], oracle=100.0)
    assert ring["bid_levels"] == 0
    assert ring["ask_levels"] == 0


# --------------------------------------------------------------------------- #
# Refusing rather than guessing
# --------------------------------------------------------------------------- #

@pytest.mark.parametrize("bad", [None, 0.0, -1.0])
def test_no_usable_oracle_yields_no_ring_at_all(bad, obs):
    """[review] The ring must be UNAVAILABLE, not computed against a
    fallback. A number a reader would trust is worse than a gap."""
    assert obs._ring_depth([_lvl(99.0, 1)], [_lvl(101.0, 1)], bad) is None


def test_malformed_levels_are_skipped_not_fatal(obs):
    """One bad level must not end a session that cannot be re-run."""
    ring = obs._ring_depth(
        bids=[{"price": "junk", "size": 1}, {"size": 5}, _lvl(99.0, 2)],
        asks=[_lvl(101.0, 2)],
        oracle=100.0,
    )
    assert ring["bid_levels"] == 1
    assert ring["bid_usd"] == pytest.approx(198.0)


def test_an_empty_book_is_zero_rather_than_none(obs):
    """Distinct from "no oracle": we looked, and there was nothing there."""
    ring = obs._ring_depth([], [], 100.0)
    assert ring is not None
    assert ring["credit_usd"] == 0.0


# --------------------------------------------------------------------------- #
# The band is a venue parameter, not a constant
# --------------------------------------------------------------------------- #

def test_the_ring_percentage_is_taken_from_meta_wherever_it_sits(obs):
    """`vol_aggressive_ring_pct` is published and documented as a DEFAULT.

    It was hard-coded at 2.0 while the venue served it, so a contest-time
    retune would have made every recorded credit_usd wrong and
    unrecomputable -- only eight levels of each ladder are kept.
    """
    assert obs._ring_pct_from_meta({"vol_aggressive_ring_pct": 3}) == 3.0
    assert obs._ring_pct_from_meta(
        {"config": {"bands": {"vol_aggressive_ring_pct": 1.5}}}) == 1.5
    assert obs._ring_pct_from_meta(
        {"markets": [{"vol_aggressive_ring_pct": 4}]}) == 4.0


def test_an_unreadable_meta_yields_no_ring_percentage(obs):
    assert obs._ring_pct_from_meta({"__error__": "URLError: nope"}) is None
    assert obs._ring_pct_from_meta({"flags": {}}) is None
    assert obs._ring_pct_from_meta({"vol_aggressive_ring_pct": "wide"}) is None


def test_a_wider_ring_admits_levels_the_default_excludes(obs):
    outside_two = ([_lvl(97.0, 1)], [_lvl(103.0, 1)])
    assert obs._ring_depth(*outside_two, 100.0)["bid_levels"] == 0
    assert obs._ring_depth(*outside_two, 100.0, 5.0)["bid_levels"] == 1


# --------------------------------------------------------------------------- #
# One tick, written down
# --------------------------------------------------------------------------- #

class _Clock:
    """A datetime stand-in the tick loop can be stepped through by hand.

    The loop sleeps up to a full TICK_S between samples, so a real clock
    makes this a five-second test with a race in it.
    """

    start = datetime(2026, 8, 28, 20, 0, tzinfo=UTC)
    current = start

    @classmethod
    def now(cls, tz=None):
        return cls.current

    @staticmethod
    def fromisoformat(text):
        return datetime.fromisoformat(text)


def _one_tick(obs, monkeypatch, tmp_path, responses):
    """Run main() for exactly one tick against canned endpoint responses."""
    _Clock.current = _Clock.start

    def fake_get(path, timeout=8):
        for prefix, value in responses:
            if path.startswith(prefix):
                if isinstance(value, Exception):
                    raise value
                return value
        raise AssertionError("unexpected request: " + path)

    class _Time:
        """Stands in for the `time` module: sleeping steps the clock."""

        @staticmethod
        def sleep(_seconds):
            _Clock.current = _Clock.current + timedelta(seconds=obs.TICK_S)

        @staticmethod
        def time():
            return _Clock.current.timestamp()

    out_path = tmp_path / "observe.jsonl"
    monkeypatch.setattr(obs, "get", fake_get)
    monkeypatch.setattr(obs, "datetime", _Clock)
    monkeypatch.setattr(obs, "time", _Time)
    monkeypatch.setattr(
        sys, "argv",
        ["permuto_observe.py", str(out_path),
         (_Clock.start + timedelta(seconds=1)).isoformat()])
    obs.main()

    lines = [json.loads(ln) for ln in
             out_path.read_text(encoding="utf-8").splitlines() if ln.strip()]
    return lines[0], lines[1:]


_ORACLE = {"prices": {"QQQ-VOL": 100.0, "NVDA-VOL": 100.0, "TSLA-VOL": 100.0}}
_BOOK = {"bids": [{"price": 99.0, "size": 10}],
         "asks": [{"price": 101.0, "size": 10}]}


def test_a_failed_l2_fetch_records_no_ring_at_all(obs, monkeypatch, tmp_path):
    """We did not look. The row must not say we looked and found nothing.

    `safe()` turns the failure into {"__error__": ...}, so bids/asks fell to
    [] and _ring_depth returned credit_usd 0.0 with zero levels -- a
    valid-looking measurement of a market that was never read, which any
    aggregation not joining on `err` averages in as a real zero. The oracle
    side already refused to guess; the book side did not.
    """
    _header, rows = _one_tick(obs, monkeypatch, tmp_path, [
        ("/info/meta", {"flags": {}, "vol_aggressive_ring_pct": 2}),
        ("/info/oracle", _ORACLE),
        ("/info/l2/QQQ-VOL-PERP", OSError("connection reset")),
        ("/info/l2/", _BOOK),
        ("/info/funding/", {"hourly_rate": 0.0}),
        ("/info/trades/", {"trades": []}),
    ])

    failed = rows[0]["l2"]["QQQ-VOL-PERP"]
    assert failed["ring"] is None
    assert failed["err"]
    assert failed["n_bid_levels"] is None
    # The oracle read fine, so the failure must be attributed to the book.
    assert failed["ring_oracle"] == 100.0
    assert failed["ring_oracle_err"] is None

    # A book that WAS read still records its zeros and its levels.
    read = rows[0]["l2"]["NVDA-VOL-PERP"]
    assert read["ring"]["credit_usd"] == pytest.approx(990.0)
    assert read["err"] is None


def test_the_ring_percentage_is_recorded_with_every_row(obs, monkeypatch, tmp_path):
    """The aggregate cannot be recomputed, so the band it used travels with it."""
    header, rows = _one_tick(obs, monkeypatch, tmp_path, [
        ("/info/meta", {"flags": {}, "vol_aggressive_ring_pct": 5}),
        ("/info/oracle", _ORACLE),
        ("/info/l2/", {"bids": [{"price": 97.0, "size": 10}],
                       "asks": [{"price": 103.0, "size": 10}]}),
        ("/info/funding/", {"hourly_rate": 0.0}),
        ("/info/trades/", {"trades": []}),
    ])

    assert header["ring_pct"] == 5.0
    assert header["ring_pct_src"] == "meta"
    assert rows[0]["ring_pct"] == 5.0
    # And it is USED: these levels are outside the 2% default.
    assert rows[0]["l2"]["QQQ-VOL-PERP"]["ring"]["bid_levels"] == 1


def test_an_unreadable_meta_says_so_rather_than_silently_assuming_two(
    obs, monkeypatch, tmp_path
):
    """A guessed band is still worth recording -- but not worth hiding.

    Blanking the session over one failed startup GET costs a run that cannot
    be repeated; presenting the documented default as a measurement costs
    the reader the ability to discount it. Record both the value and where
    it came from.
    """
    header, rows = _one_tick(obs, monkeypatch, tmp_path, [
        ("/info/meta", OSError("connection reset")),
        ("/info/oracle", _ORACLE),
        ("/info/l2/", _BOOK),
        ("/info/funding/", {"hourly_rate": 0.0}),
        ("/info/trades/", {"trades": []}),
    ])

    assert header["ring_pct"] == obs.DEFAULT_RING_PCT
    assert header["ring_pct_src"] == "default"
    assert rows[0]["ring_pct_src"] == "default"
    assert rows[0]["l2"]["QQQ-VOL-PERP"]["ring"]["credit_usd"] == pytest.approx(990.0)
