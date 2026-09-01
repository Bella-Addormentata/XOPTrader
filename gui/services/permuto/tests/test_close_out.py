"""The close control sends live orders, so its sign must never be wrong."""

import pytest

from gui.services.permuto.close_out import (
    ClosePayloadError, filled_size, order_verdict, order_was_accepted,
    close_positions, describe, plan_close, read_positions, send_close,
)


class _Client:
    def __init__(self, payload, fail=False):
        self.payload = payload
        self.sent = []
        self.fail = fail
        #: Overridable so a test can hand back a real venue body
        #: (a part-fill, a refusal) instead of a bare success.
        self.order_response = {"status": "ok"}

    def account(self, now_s):
        return self.payload

    def place_order(self, leg, now_s):
        if self.fail:
            raise RuntimeError("venue said no")
        self.sent.append(leg)
        return self.order_response


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
    """A zero size carries no exposure, so there is nothing to misreport.

    [review] This used to assert the same for {"market": "B"} -- a row with
    no size FIELD -- which is the opposite case: an explicit zero is the
    venue telling us the market is flat, while an absent size is the venue
    telling us nothing at all. See the test below.
    """
    c = _Client({"positions": [_pos("A", "sell", 0), _pos("B", "buy", 0)]})
    assert read_positions(c, 0.0) == {}


def test_a_row_with_no_size_field_FAILS_the_plan():
    """[review] `or 0.0` collapsed "absent" into "zero".

    A truncated row may carry the entire position -- there is nothing in
    {"market": "B", "side": "sell"} that says the exposure is small. It
    used to be dropped as flat, so an account with real exposure could
    render as "Nothing to close" on the one control an operator uses to
    get out.
    """
    c = _Client({"positions": [_pos("A", "sell", 100), {"market": "B",
                                                        "side": "sell"}]})
    with pytest.raises(ClosePayloadError) as excinfo:
        read_positions(c, 0.0)
    assert "B" in str(excinfo.value)


def test_a_blank_market_key_with_live_size_FAILS_the_plan():
    """The dict-shape twin of the row above: a nameless key we cannot act
    on, carrying exposure we must not pretend is absent."""
    c = _Client({"positions": {"": -250.0, "A": -100.0}})
    with pytest.raises(ClosePayloadError):
        read_positions(c, 0.0)


def test_a_blank_market_key_that_is_flat_is_still_ignored():
    """...but a nameless key at zero carries nothing, so it must not
    block a close that is otherwise perfectly readable."""
    c = _Client({"positions": {"": 0.0, "A": -100.0}})
    assert read_positions(c, 0.0) == {"A": -100.0}


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


def test_a_post_only_close_is_refused_rather_than_sent_priceless():
    """[review] The "patient close" could never have worked.

    tif="alo" built the identical price-less payload as IOC, and ALO is a
    post-only LIMIT instruction -- the venue has nothing to rest. Better
    to refuse the call than to advertise an option that can only come back
    rejected.
    """
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    with pytest.raises(ValueError) as excinfo:
        send_close(c, 0.0, legs, tif="alo")
    assert "limit price" in str(excinfo.value)
    assert not c.sent, "a leg went out despite the refusal"


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


def test_an_empty_or_absent_body_is_not_an_acknowledgement():
    """[review] "Nothing to contradict, trust the 200" counted zero bytes
    of venue evidence as a completed close.

    The transport manufactures {} for an empty 200, so this is not a
    hypothetical shape -- and the operator was shown "N leg(s) sent" on
    the strength of it.
    """
    for junk in (None, {}, [], "ok", 7):
        verdict, detail = order_verdict(junk)
        assert verdict == "unknown", (
            "%r was classified %r -- it is neither an acceptance nor "
            "evidence of refusal" % (junk, verdict))
        assert detail
        # ...and the two-valued view still refuses to count it as sent.
        assert order_was_accepted(junk)[0] is False


def test_the_action_key_is_read_on_both_sides():
    """`action` is the vocabulary the venue actually speaks -- every
    acknowledgement captured from it is shaped {"action": ..., "fills":
    ..., "order_id": ...}. It was not consulted at all, so a plain
    {"action": "rejected"} was reported as sent."""
    verdict, detail = order_verdict({"action": "rejected"})
    assert verdict == "refused", verdict
    # ...and the operator is told it was REFUSED, not that we could not
    # parse the answer. Both are False, but only one is actionable.
    assert "rejected" in detail and "unrecognised" not in detail
    assert order_was_accepted({"action": "placed"})[0] is True
    assert order_was_accepted({"action": "placed", "order_id": 4512662,
                               "fills": []})[0] is True


def test_a_fill_or_an_id_is_acceptance_on_its_own():
    """So an order the venue clearly acted on is never misreported as
    refused just because its envelope used an unfamiliar word."""
    assert order_was_accepted({"order_id": 4512662})[0] is True
    assert order_was_accepted({"fills": [{"size": 10}]})[0] is True


def test_an_unrecognised_body_is_refused_not_assumed():
    verdict, detail = order_verdict({"weather": "fine"})
    assert verdict == "unknown", (
        "an unfamiliar body was reported as a refusal; not recognising it "
        "is a fact about us, not about what the venue did")
    assert "unrecognised" in detail


def test_a_stated_rejection_still_wins_over_an_id():
    """A body carrying both an id and a refusal is a refusal."""
    assert order_was_accepted(
        {"order_id": 1, "rejection_reason": "reduce-only would increase"}
    )[0] is False


def test_a_market_rounded_below_one_lot_is_named_not_dropped():
    """[review] An unannotated `continue` removed a live market from the
    confirmation. The operator approves what the dialog lists, so a
    position that never appears is a position they believe is closing."""
    legs = plan_close({"A": 100.0, "B": 1.0}, 0.25, {"A": 1.0, "B": 1.0})
    assert [leg["market"] for leg in legs] == ["A"]
    assert legs.rounded_out and "B" in legs.rounded_out[0]
    text = describe(legs, {"A": 0.07})
    assert "NOT included" in text and "B" in text, text


def test_everything_rounding_out_is_not_reported_as_a_flat_account():
    """The worst wording available: "the venue reports no open positions"
    is a claim about the ACCOUNT, and the account is not flat -- the
    fraction was just too small to reach one lot."""
    legs = plan_close({"B": 1.0}, 0.25, {"B": 1.0})
    assert list(legs) == []
    text = describe(legs)
    assert "no open positions" not in text, text
    assert "still open" in text and "one lot" in text, text


def test_a_partial_fill_is_reported_as_partial():
    """[review] partially_filled was accepted as a plain success and no
    quantity was ever compared, so 40 of 100 read exactly like 100."""
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    c.order_response = {"action": "partially_filled", "order_id": 7,
                        "fills": [{"size": 40}]}
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    res = send_close(c, 0.0, legs)
    assert res["ok"] is True                      # the leg WAS accepted
    assert res["partial"], "a 40/100 fill was reported as a full close"
    assert "40" in res["note"] and "100" in res["note"], res["note"]


def test_a_full_fill_is_not_flagged_partial():
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    c.order_response = {"action": "filled", "order_id": 7,
                        "fills": [{"size": 100}]}
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    assert not send_close(c, 0.0, legs)["partial"]


def test_a_venue_that_does_not_state_fills_is_not_guessed_at():
    """-1.0, not 0.0: "it did not say" and "nothing filled" are different
    facts, and rendering the first as a quantity would invent one."""
    assert filled_size({"action": "placed"}) == -1.0
    assert filled_size({"fills": "lots"}) == -1.0
    assert filled_size(None) == -1.0
    assert filled_size({"fills": [{"size": 10}, {"size": 5}]}) == 15.0
    assert filled_size({"filled_size": 12}) == 12.0
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    c.order_response = {"action": "placed", "order_id": 7}
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    assert not send_close(c, 0.0, legs)["partial"]


def test_a_total_missing_a_price_is_labelled_partial():
    """[review] The total summed only the priced legs and still called
    itself the total, understating what the operator was approving -- on
    the confirmation dialog, which exists so the number can be checked
    before anything is sent."""
    legs = plan_close({"A": 100.0, "B": 100.0}, 1.0, {"A": 1.0, "B": 1.0})
    text = describe(legs, {"A": 0.07})            # B has no price
    assert "PARTIAL total" in text, text
    assert "1 leg(s) unpriced" in text, text
    # Both legs are still LISTED -- an unpriced leg is being closed too.
    assert "A" in text and "B" in text


def test_a_fully_priced_total_is_not_labelled_partial():
    legs = plan_close({"A": 100.0, "B": 100.0}, 1.0, {"A": 1.0, "B": 1.0})
    text = describe(legs, {"A": 0.07, "B": 0.07})
    assert "PARTIAL" not in text, text
    assert "total" in text


def test_no_price_at_all_says_so_rather_than_showing_nothing():
    legs = plan_close({"A": 100.0}, 1.0, {"A": 1.0})
    text = describe(legs, {})
    assert "no notional available" in text, text


def test_a_transport_error_is_unresolved_not_a_refusal():
    """[review] "Failed" reads as "nothing happened", and invites a second
    press that doubles the close.

    _request() can raise AFTER urlopen() succeeded, while reading the
    response, so the venue may well have taken the order. A refusal
    leaves the position untouched; this does not, and the two must not
    render alike.
    """
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]}, fail=True)
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    res = send_close(c, 0.0, legs)

    assert res["ok"] is False
    assert res["unknown"], "a transport error was not recorded as unresolved"
    assert res["sent"] == 0, "an unresolved leg was counted as sent"
    note = res["note"]
    assert "UNRESOLVED" in note, note
    assert "EXECUTED" in note, note
    assert "Check the position" in note, note
    # ...and it must NOT be described as a plain refusal.
    assert not note.startswith("partial:"), note


def test_every_leg_refused_is_not_called_partial():
    """[review] "partial" claims something got through.

    When every leg was refused, nothing did -- the position is exactly
    where it was. That is a different instruction to the operator than
    "some of your close landed", and on this control the difference
    decides whether they press it again.
    """
    c = _Client({"positions": [_pos("QQQ-VOL-PERP", "sell", 100)]})
    c.order_response = {"status": "rejected"}
    legs = plan_close({"QQQ-VOL-PERP": -100.0}, 1.0)
    res = send_close(c, 0.0, legs)

    assert res["ok"] is False
    assert res["sent"] == 0
    assert not res["note"].startswith("partial"), res["note"]
    assert "refused" in res["note"], res["note"]


def test_a_malformed_fills_field_is_not_acceptance():
    """[review] order_verdict() and filled_size() disagreed about the same
    field. filled_size({"fills": "lots"}) already refuses to read it, but
    order_verdict() counted any truthy `fills` as acceptance -- so an
    unrecognised body was reported as a sent leg instead of an unresolved
    one."""
    assert order_verdict({"fills": "lots"})[0] == "unknown"
    assert order_verdict({"fills": {}})[0] == "unknown"
    assert order_verdict({"fills": []})[0] == "unknown"
    # A real collection still is.
    assert order_verdict({"fills": [{"size": 10}]})[0] == "accepted"
    # ...and the two functions now agree about the malformed case.
    assert filled_size({"fills": "lots"}) == -1.0
