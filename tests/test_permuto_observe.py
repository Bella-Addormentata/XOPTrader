"""The observer's one derived value.

`_ring_depth` produces `credit_usd`, which is the number every depth-sizing
conclusion in TODO-COMPETITION rests on, and it cannot be recomputed later
because the ladder it comes from is not kept. It had no tests.
"""

from __future__ import annotations

import importlib.util
import sys
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
