"""The per-pair ratio band the Wallet tab's Apply writes.

[RATIO-BAND-CAP 2026-11-08] Apply projects the "Target % +/-" column into
``strategy.ratio_band_enter_by_pair``.  XCH's +/-40 points used to make 0.49
bands, so the XCH pairs could not enter AcquireBase however far below target
they were.  The band is now capped at the engine's default (0.10); a narrow
tolerance can still make a pair stricter.
"""

from __future__ import annotations

import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

from gui.ratio_targets import RATIO_BAND_ENTER_MAX, pair_band_enter  # noqa: E402


def _uncapped(base: float, quote: float, base_tol: float, quote_tol: float) -> float:
    target = base / (base + quote)
    upper = (base + base_tol) / ((base + base_tol) + max(0.0, quote - quote_tol))
    lower = max(0.0, base - base_tol) / (max(0.0, base - base_tol) + (quote + quote_tol))
    return max(abs(upper - target), abs(target - lower))


def test_the_cap_is_the_engines_default_band():
    # config.hpp: double ratio_band_enter{0.10};
    assert RATIO_BAND_ENTER_MAX == pytest.approx(0.10)


@pytest.mark.parametrize(
    "pair, base, quote, base_tol, quote_tol, was",
    [
        # Dorkmo's targets and tolerances on 2026-11-08, and the bands the
        # old Apply wrote for them.
        ("XCH/DBX", 50.0, 15.0, 40.0, 10.0, 0.4835164835164836),
        ("XCH/BYC", 50.0, 5.0, 40.0, 4.0, 0.3827751196172249),
        ("XCH/wUSDC.b", 50.0, 20.0, 40.0, 15.0, 0.49),
        ("wmilliETH.b/XCH", 10.0, 50.0, 8.0, 40.0, 0.4761904761904763),
    ],
)
def test_wide_tolerances_are_capped(pair, base, quote, base_tol, quote_tol, was):
    assert min(0.49, _uncapped(base, quote, base_tol, quote_tol)) == pytest.approx(was), pair
    assert pair_band_enter(base, quote, base_tol, quote_tol) == pytest.approx(
        RATIO_BAND_ENTER_MAX), pair


def test_a_narrow_tolerance_still_makes_a_pair_stricter():
    band = pair_band_enter(50.0, 15.0, 2.0, 1.0)
    assert band is not None
    assert band == pytest.approx(_uncapped(50.0, 15.0, 2.0, 1.0))
    assert band < RATIO_BAND_ENTER_MAX


def test_units_do_not_matter():
    assert pair_band_enter(0.5, 0.15, 0.02, 0.01) == pytest.approx(
        pair_band_enter(50.0, 15.0, 2.0, 1.0))


@pytest.mark.parametrize(
    "base, quote, base_tol, quote_tol",
    [
        (50.0, 15.0, 0.0, 0.0),   # no tolerance: the global band applies
        (0.0, 0.0, 5.0, 5.0),     # no targets
        (-1.0, 15.0, 5.0, 5.0),   # malformed target
    ],
)
def test_no_override_without_a_usable_projection(base, quote, base_tol, quote_tol):
    assert pair_band_enter(base, quote, base_tol, quote_tol) is None


def test_the_wallet_tab_apply_uses_the_capped_helper():
    source = (_REPO / "gui" / "widgets" / "wallet_balances.py").read_text(encoding="utf-8")
    start = source.index("def _pair_band_enter_from_tolerances")
    end = source.index("def _apply_targets")
    body = source[start:end]
    assert "pair_band_enter(" in body
    assert "min(0.49" not in body
    assert "from gui.ratio_targets import pair_band_enter" in source
