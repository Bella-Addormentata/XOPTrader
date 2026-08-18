"""The operator's Base gas reserve: ``warp.min_base_eth``.

Two floors govern Base gas and they are emphatically not the same number:

* ``_MIN_GAS_WEI`` (0.0003 ETH) is a protocol fact -- below it the engine
  cannot sign a relay at all, so it refuses to try;
* ``warp.min_base_eth`` is the operator's own line, the balance the GUI warns
  against so a refuel happens BEFORE work starts refusing.

The warning threshold used to be a constant compiled into two widgets, which
meant a wallet that relays often could not raise it and a wallet that never
relays could not silence it.  These tests pin the config knob, its validation,
and the snapshot key the widgets read it from.

Run directly:  .venv/Scripts/python.exe -m pytest gui/services/warp/tests/test_gas_reserve.py
"""

from __future__ import annotations

import pytest

from gui.services.warp import service as S

from .test_warp_service import build, default_params, new_store


# --------------------------------------------------------------------------- #
# Parsing
# --------------------------------------------------------------------------- #

def test_absent_key_gets_the_documented_default():
    """A config that predates the knob still has a working warning."""
    assert S.warp_params_from_config({}).min_base_eth == pytest.approx(0.005)
    assert S.warp_params_from_config(
        {"warp": {}}
    ).min_base_eth == pytest.approx(0.005)
    assert S.warp_params_from_config(None).min_base_eth == pytest.approx(0.005)


def test_explicit_value_is_honoured():
    p = S.warp_params_from_config({"warp": {"min_base_eth": 0.05}})
    assert p.min_base_eth == pytest.approx(0.05)


def test_zero_is_a_legal_value_and_means_off():
    """Zero is a choice, not a missing value.

    A wallet that never relays should be able to stop the nag without
    editing the source.  This is why the knob cannot reuse the positive-float
    parser the other warp floats use -- that one rejects 0.
    """
    p = S.warp_params_from_config({"warp": {"min_base_eth": 0}})
    assert p.min_base_eth == 0.0
    assert S.warp_params_from_config(
        {"warp": {"min_base_eth": "0"}}
    ).min_base_eth == 0.0


def test_blank_falls_back_rather_than_reading_as_zero():
    """A commented-out or emptied key must not silently disable the guard."""
    for blank in ("", "   ", None):
        p = S.warp_params_from_config({"warp": {"min_base_eth": blank}})
        assert p.min_base_eth == pytest.approx(0.005), blank


@pytest.mark.parametrize("bad", [-1, -0.001, "-0.5"])
def test_negative_is_refused(bad):
    """A negative floor would read as 'never warn' by accident."""
    with pytest.raises(S.WarpError, match="min_base_eth"):
        S.warp_params_from_config({"warp": {"min_base_eth": bad}})


@pytest.mark.parametrize("bad", ["abc", "0.1eth", [], {}])
def test_junk_fails_loudly(bad):
    with pytest.raises(S.WarpError):
        S.warp_params_from_config({"warp": {"min_base_eth": bad}})


# --------------------------------------------------------------------------- #
# The snapshot key the widgets consume
# --------------------------------------------------------------------------- #

def test_snapshot_publishes_the_configured_floor_in_wei():
    """The GUI must warn against the operator's number, not a constant.

    Both the Warp page and the Base Wallet page read ``min_base_eth_wei``;
    if the snapshot stopped carrying it they would silently fall back to the
    built-in 0.005 and quietly ignore a raised reserve.
    """
    engine, _ctx = build(new_store(), params=default_params(min_base_eth=0.02))
    assert engine.snapshot()["min_base_eth_wei"] == 20_000_000_000_000_000


def test_snapshot_floor_tracks_config_and_survives_zero():
    engine, _ctx = build(new_store(), params=default_params(min_base_eth=0.0))
    assert engine.snapshot()["min_base_eth_wei"] == 0

    engine2, _ctx2 = build(new_store(), params=default_params(min_base_eth=0.005))
    assert engine2.snapshot()["min_base_eth_wei"] == 5_000_000_000_000_000


def test_the_hard_floor_is_a_different_number_than_the_warning():
    """Guard against someone 'simplifying' the two floors into one.

    The engine's hard floor is what it takes to sign; the reserve is what the
    operator wants on hand.  Collapsing them would either spam warnings at a
    balance that still works, or stay silent right up to the refusal.
    """
    assert S._MIN_GAS_WEI == 300_000_000_000_000  # 0.0003 ETH
    default = S.warp_params_from_config({}).min_base_eth
    assert int(default * 10 ** 18) > S._MIN_GAS_WEI, (
        "the operator's reserve must sit ABOVE the protocol floor, or the "
        "warning fires only after jobs have already started refusing"
    )
