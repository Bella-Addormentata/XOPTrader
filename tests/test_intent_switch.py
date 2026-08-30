"""[INTENT v0.10.7] The slider claims intent; the chip claims reality.

Policy-level tests for resolve_chip plus the Cancel All flag channel. The
window-level behaviour (clicks never refused, deferred starts, the
cancel-all convergence) is covered in
gui/services/permuto/tests/test_venue_control.py.
"""

from __future__ import annotations

import pytest

from gui.services.venue_control import (
    StatusChip,
    SwitchInputs,
    resolve_chip,
)


def _in(**kw) -> SwitchInputs:
    kw.setdefault("gates", frozenset())
    if not isinstance(kw["gates"], frozenset):
        kw["gates"] = frozenset(kw["gates"])
    return SwitchInputs(**kw)


# --------------------------------------------------------------------------- #
# Intent ON
# --------------------------------------------------------------------------- #

def test_on_and_clear_reads_quoting_with_the_count():
    chip = resolve_chip(_in(desired_on=True, book_is_empty=False,
                            resting_count=14.0))
    assert chip.text == "quoting -- 14 resting"
    assert chip.tone == "ok"
    assert not chip.offer_cancel_visible


def test_on_with_a_gate_reads_blocked_and_the_tooltip_names_it():
    chip = resolve_chip(_in(desired_on=True, gates={"engine_down"}))
    assert chip.text == "blocked"
    assert chip.tone == "warn"
    assert "engine is not running" in chip.tooltip


def test_on_with_unknown_count_still_reads_quoting():
    chip = resolve_chip(_in(desired_on=True, resting_count=-1.0))
    assert chip.text == "quoting"


# --------------------------------------------------------------------------- #
# Intent OFF
# --------------------------------------------------------------------------- #

def test_off_with_resting_offers_shows_the_drain_and_the_cancel_button():
    chip = resolve_chip(_in(desired_on=False, book_is_empty=False,
                            resting_count=12.0))
    assert chip.text == "stopped -- 12 resting, draining"
    assert chip.tone == "converging"
    assert chip.offer_cancel_visible, (
        "a draining book must offer the immediate way out")
    assert "TTL" in chip.tooltip


def test_off_with_an_unknown_book_neither_invents_nor_denies_offers():
    chip = resolve_chip(_in(desired_on=False, book_is_empty=False,
                            resting_count=-1.0))
    assert chip.text == "stopped -- book unknown"
    assert not chip.offer_cancel_visible, (
        "no cancel button for a book nothing can see")


def test_off_flat_verified_is_the_only_flat_claim():
    chip = resolve_chip(_in(desired_on=False, book_is_empty=True,
                            book_verified=True))
    assert chip.text == "stopped -- flat"

    chip = resolve_chip(_in(desired_on=False, book_is_empty=True,
                            book_verified=False))
    assert chip.text == "stopped -- book unverified"


def test_off_over_cancels_pending_gate_still_reads_stopped_not_blocked():
    # Gates only matter to an ON intent; an OFF book with a leftover gate
    # entry must not read as blocked.
    chip = resolve_chip(_in(desired_on=False, book_is_empty=True,
                            gates={"breaker"}))
    assert chip.text == "stopped -- flat"


# --------------------------------------------------------------------------- #
# [STOPDRAIN] The Cancel All flag channel
# --------------------------------------------------------------------------- #

def test_cancel_all_offers_writes_the_flag(tmp_path):
    from gui.services.engine_bridge import EngineBridge

    bridge = EngineBridge.__new__(EngineBridge)
    bridge._db_path = tmp_path / "db" / "xop.sqlite"
    bridge.cancel_all_offers()

    flag = tmp_path / "db" / "cancel_all.flag"
    assert flag.exists()


def test_bridge_advertises_direct_control():
    from gui.services.engine_bridge import EngineBridge
    assert EngineBridge.SUPPORTS_DIRECT_CONTROL is True
