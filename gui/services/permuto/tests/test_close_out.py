"""The close control sends live orders, so its sign must never be wrong."""

import pytest

from gui.services.permuto.close_out import (
    close_positions, describe, plan_close, read_positions,
)


class _Client:
    def __init__(self, payload, fail=False):
        self.payload = payload
        self.sent = None
        self.fail = fail

    def account(self, now_s):
        return self.payload

    def batch_upsert(self, legs, now_s):
        if self.fail:
            raise RuntimeError("venue said no")
        self.sent = legs
        return {"status": "batch_ok"}


def _pos(market, side, size):
    return {"market": market, "side": side, "size": str(size)}


# --------------------------------------------------------------------------- #
# reading the venue
# --------------------------------------------------------------------------- #

def test_a_short_reads_negative_and_a_long_positive():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100),
                               _pos("NVDA-VOL-PERP", "buy", 50)]})
    assert read_positions(c, 0.0) == {"QQQ-VOL-PERP": -100.0,
                                      "NVDA-VOL-PERP": 50.0}


def test_an_unreadable_side_is_dropped_not_guessed():
    """An unsigned size makes a short look like a long, and then "reduce"
    means "sell more" -- the exact inversion that grew the contest short
    from 5,253 to 825,541 contracts in one session."""
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sideways", 100)]})
    assert read_positions(c, 0.0) == {}


def test_zero_and_malformed_rows_are_ignored():
    c = _Client({"positions": [_pos("A", "sell", 0), {"market": "B"},
                               {"side": "sell", "size": "5"}, "junk"]})
    assert read_positions(c, 0.0) == {}


# --------------------------------------------------------------------------- #
# planning -- the sign is the whole ballgame
# --------------------------------------------------------------------------- #

def test_a_short_is_closed_by_BUYING():
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    assert legs == [{"market": "QQQ-VOL-PERP", "side": "buy",
                     "size": 100.0, "reduce_only": True}]


def test_a_long_is_closed_by_SELLING():
    legs = plan_close({"QQQ-VOL-PERP": 100.0}, 1.0)
    assert legs[0]["side"] == "sell"


def test_every_leg_is_reduce_only_always():
    legs = plan_close({"A": -100.0, "B": 250.0}, 0.5)
    assert legs and all(leg["reduce_only"] for leg in legs), \
        "a non-reduce-only leg could OPEN a position instead of closing one"


def test_a_fraction_closes_only_that_fraction():
    assert plan_close({"A": -1000.0}, 0.25)[0]["size"] == 250.0


def test_size_is_lot_quantised_downward():
    # 333.33 -> 333 at lot 1; never rounds UP past the position.
    legs = plan_close({"A": -1000.0}, 1.0 / 3.0, {"A": 1.0})
    assert legs[0]["size"] == 333.0


def test_a_close_never_exceeds_the_position():
    for frac in (0.1, 0.5, 1.0):
        legs = plan_close({"A": -777.0}, frac)
        assert legs[0]["size"] <= 777.0


def test_an_out_of_range_fraction_is_refused():
    for bad in (0.0, -0.5, 1.5):
        with pytest.raises(ValueError):
            plan_close({"A": -100.0}, bad)


def test_a_flat_market_produces_no_leg():
    assert plan_close({"A": 0.0}, 1.0) == []


# --------------------------------------------------------------------------- #
# sending
# --------------------------------------------------------------------------- #

def test_it_sends_reduce_only_ioc_legs():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    res = close_positions(c, 0.0, 1.0)
    assert res["ok"] and res["sent"] == 1
    leg = c.sent[0]
    assert leg["side"] == "buy" and leg["reduce_only"] is True
    assert leg["tif"] == "ioc"


def test_a_patient_close_can_ask_for_alo():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    close_positions(c, 0.0, 1.0, tif="alo")
    assert c.sent[0]["tif"] == "alo"


def test_no_positions_sends_nothing():
    c = _Client({"positions": []})
    res = close_positions(c, 0.0, 1.0)
    assert res["ok"] and res["sent"] == 0 and c.sent is None


def test_a_venue_refusal_is_reported_not_raised():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]},
                fail=True)
    res = close_positions(c, 0.0, 1.0)
    assert res["ok"] is False and "venue said no" in res["note"]


def test_the_summary_shows_contracts_and_notional():
    legs = plan_close({"QQQ-VOL-PERP": -1000.0}, 1.0)
    text = describe(legs, {"QQQ-VOL": 0.15376})
    assert "QQQ-VOL-PERP" in text and "BUY" in text and "1000" in text
    assert "$" in text, "the operator must see notional, not just contracts"


def test_the_summary_says_so_when_there_is_nothing_to_do():
    assert "Nothing to close" in describe([])
