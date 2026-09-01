"""The close control sends live orders, so its sign must never be wrong."""

import pytest

from gui.services.permuto.close_out import (
    ClosePayloadError, order_was_accepted,
    close_positions, describe, plan_close, read_positions, send_close,
)


class _Client:
    def __init__(self, payload, fail=False):
        self.payload = payload
        self.sent = []
        self.fail = fail

    def account(self, now_s):
        return self.payload

    def place_order(self, leg, now_s):
        if self.fail:
            raise RuntimeError("venue said no")
        self.sent.append(leg)
        return {"status": "ok"}


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


def test_an_unreadable_side_RAISES_rather_than_vanishing():
    """An unsigned size makes a short look like a long, and then "reduce"
    means "sell more" -- the exact inversion that grew the contest short
    from 5,253 to 825,541 contracts in one session.

    [review] Dropping the row silently also dropped the FACT that the
    account was incomplete: with every row unreadable the operator was
    told "no open positions", and with one unreadable that exposure just
    vanished from the confirmation plan. Neither is survivable on the
    control someone reaches for to get out, so it fails closed.
    """
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sideways", 100)]})
    with pytest.raises(ClosePayloadError):
        read_positions(c, 0.0)


def test_a_genuinely_flat_row_is_ignored():
    """A zero size carries no exposure, so there is nothing to misreport."""
    c = _Client({"positions": [_pos("A", "sell", 0), {"market": "B"}]})
    assert read_positions(c, 0.0) == {}


def test_a_structurally_broken_row_FAILS_the_plan():
    """[review] The sixth fail-open, and the audit that should have come
    after the first.

    Every one of these was a `continue` that looked harmless alone. The
    rule: a row may be SKIPPED only when it structurally carries no
    exposure. Anything merely UNPARSEABLE might be a live position, and
    dropping it silently reports less exposure than exists.
    """
    for rows in (
        [{"side": "sell", "size": "5"}],        # a size with no market
        ["junk"],                                # not even a row
        [_pos("A", "sell", "lots")],             # unparseable size
        [{"market": "A", "side": "sell", "size": float("inf")}],
    ):
        with pytest.raises(ClosePayloadError):
            read_positions(_Client({"positions": rows}), 0.0)


def test_a_junk_oracle_price_does_not_block_the_confirmation():
    """[review] The opposite rule to the account parsing, deliberately.

    The notional is a NICETY; the plan is the point. A malformed public
    oracle value used to raise straight out of describe() and stop the
    operator ever reaching the dialog. The ACCOUNT must fail closed
    because it is the truth about exposure; the ORACLE is display data
    and must degrade quietly.
    """
    legs = plan_close({"QQQ-VOL-PERP": -1000.0}, 1.0)
    for bad in ("unavailable", float("nan"), float("inf"), None, -1.0):
        text = describe(legs, {"QQQ-VOL": bad})
        assert "QQQ-VOL-PERP" in text and "1000" in text, (
            "a junk oracle value (%r) hid the close plan" % (bad,))



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
# sending via send_close -- each leg goes through place_order not batch_upsert
# --------------------------------------------------------------------------- #

def test_it_sends_reduce_only_ioc_legs():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    res = send_close(c, 0.0, legs)
    assert res["ok"] and res["sent"] == 1
    leg = c.sent[0]
    assert leg["side"] == "buy" and leg["reduce_only"] is True
    assert leg["tif"] == "ioc"


def test_a_patient_close_can_ask_for_alo():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    send_close(c, 0.0, legs, tif="alo")
    assert c.sent[0]["tif"] == "alo"


def test_send_clamps_to_fresh_position_if_smaller():
    """If the position shrank between plan and send, send the fresh size."""
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 40)]})
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)  # planned for 100
    res = send_close(c, 0.0, legs)
    assert res["ok"] and res["sent"] == 1
    assert c.sent[0]["size"] == 40.0  # clamped to fresh 40, not planned 100


def test_send_skips_a_leg_when_position_flipped():
    """If the position went from short to long between plan and send, the
    original BUY direction would grow the long -- skip it entirely."""
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "buy", 50)]})
    legs = [{"market": "QQQ-VOL-PERP", "side": "buy",
             "size": 100.0, "reduce_only": True}]  # planned to close short
    res = send_close(c, 0.0, legs)
    assert res["sent"] == 0 and not c.sent


def test_send_skips_a_leg_when_already_flat():
    """A position that closed before the operator confirmed: skip cleanly."""
    c = _Client({"positions": []})
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    res = send_close(c, 0.0, legs)
    assert res["ok"] and res["sent"] == 0 and not c.sent


def test_no_positions_sends_nothing():
    c = _Client({"positions": []})
    res = close_positions(c, 0.0, 1.0)
    assert res["ok"] and res["sent"] == 0 and not c.sent


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


# --------------------------------------------------------------------------- #
# [review] Both documented account shapes, and 200-with-a-refusal
# --------------------------------------------------------------------------- #

def test_the_dict_position_shape_is_read_not_ignored():
    """The worst failure available to an emergency control.

    account() serves a signed DICT form as well as the list form -- both
    are pinned by test_positions_parse_from_a_dict_or_a_list -- and this
    read only the list. A dict payload therefore came back empty and the
    close said "Nothing to close" while the position was fully open, at
    the one moment the operator was trying to get out.
    """
    c = _Client({"positions": {"QQQ-VOL-PERP": -870648.0,
                               "NVDA-VOL-PERP": 5.0}})
    assert read_positions(c, 0.0) == {"QQQ-VOL-PERP": -870648.0,
                                      "NVDA-VOL-PERP": 5.0}


def test_the_dict_shape_is_already_signed():
    """Dict values carry their own sign; list rows carry a magnitude plus a
    side. Conflating the two is what hid the phantom-long bug."""
    c = _Client({"positions": {"A": -5.0}})
    assert read_positions(c, 0.0)["A"] == -5.0


def test_an_explicitly_empty_account_is_flat():
    for payload in ({"positions": []}, {"positions": {}}):
        assert read_positions(_Client(payload), 0.0) == {}


def test_a_MISSING_positions_field_is_unreadable_not_flat():
    """[review] Absence is not emptiness.

    A partial payload that simply did not carry `positions` read as "the
    venue reports no open positions" -- the same fail-open this function
    closes everywhere else, and the risk parser already treats a
    missing/wrong-typed field as unreadable.
    """
    with pytest.raises(ClosePayloadError):
        read_positions(_Client({}), 0.0)
    with pytest.raises(ClosePayloadError):
        read_positions(_Client({"balance": "500000"}), 0.0)


def test_an_unreadable_shape_raises_rather_than_reading_as_empty():
    """"I could not understand the account" and "you have no positions"
    must never look alike on a control used to escape a position."""
    with pytest.raises(ClosePayloadError):
        read_positions(_Client({"positions": "everything"}), 0.0)
    with pytest.raises(ClosePayloadError):
        read_positions(_Client("nonsense"), 0.0)


def test_a_non_numeric_dict_value_FAILS_the_plan():
    """[review] Skipping it produced a partial view of the account.

    {"A": "lots", "B": -5} yielded a confirmation showing only B -- and if
    A were the only row, "no open positions". The LIST branch already
    raised on the same class of junk; the dict branch recorded the row as
    unreadable and then returned without ever consulting the list. Both
    shapes now go through one shared check, because two copies of this
    rule is how they drifted apart in the first place.
    """
    c = _Client({"positions": {"A": "lots", "B": -5.0}})
    with pytest.raises(ClosePayloadError):
        read_positions(c, 0.0)


def test_a_non_finite_dict_value_FAILS_the_plan():
    """NaN and infinity were dropped silently while the list branch
    raised. One shape failing closed and the other failing open is worse
    than either rule applied consistently."""
    for bad in (float("nan"), float("inf"), float("-inf")):
        with pytest.raises(ClosePayloadError):
            read_positions(_Client({"positions": {"A": bad}}), 0.0)


def test_http_200_with_a_rejection_is_not_a_successful_close():
    """The client raises only for HTTP errors, and this venue refuses at
    the application level with a 200 body."""
    assert order_was_accepted({"status": "ok"})[0] is True
    assert order_was_accepted({"status": "rejected"})[0] is False
    assert order_was_accepted(
        {"status": "ok", "rejection_reason": "insufficient margin"})[0] is False
    assert "margin" in order_was_accepted(
        {"rejection_reason": "insufficient margin"})[1]


def test_a_refused_leg_is_reported_as_failed_not_sent():
    class _Refusing(_Client):
        def place_order(self, leg, now_s):
            return {"status": "rejected",
                    "rejection_reason": "reduce_only would increase position"}

    c = _Refusing({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    approved = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    res = send_close(c, 0.0, approved)
    assert res["ok"] is False, "a venue refusal was reported as a close"
    assert res["sent"] == 0
    assert "reduce_only" in res["note"]


def test_a_null_account_response_raises_rather_than_reading_as_empty():
    """[review] `or {}` turned a JSON null into an empty account.

    The emergency control then reported "no open positions" for a venue
    response it could not read at all -- the same conflation this function
    fails closed on everywhere else, arriving through the one line that
    looked like harmless defensiveness.
    """
    with pytest.raises(ClosePayloadError):
        read_positions(_Client(None), 0.0)


def test_skipped_markets_reach_the_operator_not_just_the_log():
    """"2 leg(s) sent" while an approved exposure was quietly dropped is
    the report that gets someone to walk away from a position they believe
    they closed."""
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    approved = plan_close({"QQQ-VOL-PERP": -100.0,
                           "TSLA-VOL-PERP": -500.0}, 1.0)
    res = send_close(c, 0.0, approved)          # TSLA is gone from the venue
    assert res["sent"] == 1
    assert "TSLA-VOL-PERP" in res["note"], res
    assert "TSLA-VOL-PERP" in " ".join(res.get("skipped", []))


def test_a_flipped_market_is_named_in_the_note():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "buy", 100)]})
    approved = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)   # approved a BUY
    res = send_close(c, 0.0, approved)
    assert res["sent"] == 0
    assert "flipped" in res["note"], res
