"""Tests for the emergency consolidation planner.

The planner is the part of this feature that can lose money silently, so
these pin behaviour rather than implementation: what gets taken, in what
order, and -- most importantly -- what gets refused.
"""

from __future__ import annotations

import pytest

from gui.services.consolidate.planner import (
    denomination,
    Anchor,
    OfferCandidate,
    PlanError,
    build_plan,
    effective_rate,
    rate_deviation_frac,
)

XCH = "xch"
BYC = "byc"
DBX = "dbx"


def offer(oid: str, give: int, recv: int, *, give_asset=BYC, recv_asset=XCH, status=0):
    """Build an offer from DISPLAY amounts, scaled to raw like the wallet.

    These used to pass raw amounts straight through, which quietly assumed
    both assets share a denomination.  They do not -- XCH is 10^12 raw units
    per display unit and a CAT is 10^3 -- so every rate here was out by 10^9
    against a display-denominated anchor, and the tests agreed with the bug
    because they created the offers the same wrong way.  Scaling here keeps
    every expectation below readable in display terms while exercising the
    real raw-integer path.
    """
    return OfferCandidate(
        offer_id=oid,
        give_asset=give_asset,
        receive_asset=recv_asset,
        give_amount=give * denomination(give_asset),
        receive_amount=recv * denomination(recv_asset),
        status=status,
    )


# A round anchor: 2 BYC per 1 XCH.
ANCHOR = Anchor(rate=2.0, source="test")


# ---------------------------------------------------------------------------
# Rate arithmetic
# ---------------------------------------------------------------------------

def test_effective_rate_is_give_per_receive():
    assert effective_rate(offer("a", 200, 100)) == 2.0


def test_degenerate_offer_rates_as_infinite_rather_than_raising():
    # A zero-receive offer must sort last and be filtered by any finite cap,
    # not blow up the sort or sneak through as "free".
    assert effective_rate(offer("a", 200, 0)) == float("inf")


def test_deviation_is_signed_so_better_than_anchor_is_negative():
    # A better-than-reference offer is the whole point of running this; it
    # must not be mistaken for an outlier by a caller filtering on
    # magnitude.
    assert rate_deviation_frac(1.0, ANCHOR) == pytest.approx(-0.5)
    assert rate_deviation_frac(3.0, ANCHOR) == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# The cap decides where to STOP, never what to take first
# ---------------------------------------------------------------------------

def test_offers_are_taken_best_price_first():
    offers = [
        offer("worst", 300, 100),   # 3.0
        offer("best", 190, 100),    # 1.9
        offer("mid", 240, 100),     # 2.4
    ]
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000 * denomination(BYC),
        max_slippage_frac=10.0, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert [o.offer_id for o in plan.legs[0].offers] == ["best", "mid", "worst"]


def test_a_wide_cap_and_a_tight_cap_agree_when_good_offers_cover_the_budget():
    """The property that makes a 99% cap safe.

    Widening the cap may add worse fills at the TAIL; it must never displace
    a better fill.  So when the good offers alone cover the position, the
    two caps must produce identical execution -- which is what lets an
    operator who believes an asset is worthless set 99% without also
    accepting bad prices on the part that could have gone out at a fair one.
    """
    offers = [
        offer("good1", 200, 100),
        offer("good2", 202, 100),
        offer("junk", 20_000, 100),   # 100x -- the shape seen live on XCH/BYC
    ]
    tight = build_plan(
        source_asset=BYC, target_asset=XCH, budget=402 * denomination(BYC),
        max_slippage_frac=0.05, direct_offers=offers, direct_anchor=ANCHOR,
    )
    wide = build_plan(
        source_asset=BYC, target_asset=XCH, budget=402 * denomination(BYC),
        max_slippage_frac=0.99, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert [o.offer_id for o in tight.legs[0].offers] == ["good1", "good2"]
    assert [o.offer_id for o in wide.legs[0].offers] == ["good1", "good2"]


def test_the_hundred_x_outlier_is_refused_even_at_a_99_percent_cap():
    """99% slippage still is not 'any price'.

    The operator's case for a wide cap is 'this asset may be worthless'.
    That justifies accepting a bad rate, not an absurd one -- and the junk
    offer the engine flagged live (price 0.013810 against a mid near 1.5) is
    ~100x away, far outside even a 99% cap.
    """
    offers = [offer("junk", 20_000, 100)]  # rate 200 vs anchor 2.0 -> +9900%
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=1_000_000 * denomination(BYC),
        max_slippage_frac=0.99, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert plan.is_empty
    assert plan.skipped_worse_than_cap == 1


def test_cap_boundary_is_inclusive():
    # Exactly at the cap is accepted; a hair beyond is not.  Pinned because
    # an off-by-one here silently changes what the operator's number means.
    at_cap = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000 * denomination(BYC),
        max_slippage_frac=0.5, direct_offers=[offer("x", 300, 100)],
        direct_anchor=ANCHOR,
    )
    assert at_cap.take_count == 1

    past_cap = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000 * denomination(BYC),
        max_slippage_frac=0.5, direct_offers=[offer("x", 301, 100)],
        direct_anchor=ANCHOR,
    )
    assert past_cap.take_count == 0


def test_tail_beyond_the_cap_is_counted_not_walked():
    offers = [offer("ok", 200, 100)] + [offer(f"bad{i}", 900, 100) for i in range(5)]
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000 * denomination(BYC),
        max_slippage_frac=0.1, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert plan.take_count == 1
    assert plan.skipped_worse_than_cap == 5


# ---------------------------------------------------------------------------
# All-or-nothing takes
# ---------------------------------------------------------------------------

def test_an_offer_larger_than_the_remaining_budget_is_skipped_not_trimmed():
    """take_offer has no partial fill (cpp/src/engine.cpp:9921).

    An oversized offer must be skipped and the search continue -- stopping
    at it would strand spendable balance that a later, smaller offer could
    have used.
    """
    offers = [
        offer("too_big", 5_000, 2_500),   # best rate, but larger than budget
        offer("fits", 200, 100),
    ]
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=1_000 * denomination(BYC),
        max_slippage_frac=1.0, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert [o.offer_id for o in plan.legs[0].offers] == ["fits"]
    assert plan.skipped_too_large == 1


def test_planner_never_spends_more_than_the_budget():
    offers = [offer(f"o{i}", 300, 100) for i in range(20)]
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=1_000 * denomination(BYC),
        max_slippage_frac=1.0, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert plan.give_total <= 1_000 * denomination(BYC)


# ---------------------------------------------------------------------------
# Shape validation happens before pricing
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "bad",
    [
        offer("wrong_give", 200, 100, give_asset=DBX),
        offer("wrong_recv", 200, 100, recv_asset=DBX),
        offer("zero_recv", 200, 0),
        offer("zero_give", 0, 100),
        offer("not_takeable", 200, 100, status=3),
    ],
)
def test_malformed_offers_never_reach_the_ranking(bad):
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000 * denomination(BYC),
        max_slippage_frac=10.0, direct_offers=[bad], direct_anchor=ANCHOR,
    )
    assert plan.is_empty
    assert plan.skipped_malformed == 1
    assert plan.skipped_worse_than_cap == 0, "rejected on shape, not on price"


# ---------------------------------------------------------------------------
# Routing
# ---------------------------------------------------------------------------

def test_direct_route_wins_when_it_fills():
    """Two hops mean two all-or-nothing takes, two fees, two windows for the
    book to move.  A direct route that fills at all must beat that."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000 * denomination(BYC),
        max_slippage_frac=1.0,
        direct_offers=[offer("direct", 200, 100, recv_asset=DBX)],
        direct_anchor=ANCHOR,
        hop_asset=XCH,
        first_hop_offers=[offer("h1", 200, 100)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("h2", 100, 100, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 1
    assert [o.offer_id for o in plan.legs[0].offers] == ["direct"]


def test_two_hop_is_used_when_no_direct_book_exists():
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000 * denomination(BYC),
        max_slippage_frac=1.0,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("h1", 200, 100)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("h2", 100, 300, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 2
    assert plan.give_total == 200 * denomination(BYC)      # BYC spent
    assert plan.receive_total == 300 * denomination(DBX)   # DBX received


def test_second_hop_can_only_spend_what_the_first_actually_yields():
    """If hop one underfills, hop two must shrink with it.

    Budgeting hop two from anything other than hop one's realised output
    would promise a quantity that does not exist when the plan runs.
    """
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000 * denomination(BYC),
        max_slippage_frac=1.0,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        # Only 100 XCH obtainable on hop one.
        first_hop_offers=[offer("h1", 200, 100)],
        first_hop_anchor=ANCHOR,
        # Hop two offers want far more XCH than hop one produces.
        second_hop_offers=[
            offer("h2_big", 500, 1_500, give_asset=XCH, recv_asset=DBX),
            offer("h2_fits", 100, 300, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert [o.offer_id for o in plan.legs[1].offers] == ["h2_fits"]
    assert plan.legs[1].give_total <= plan.legs[0].receive_total


def test_two_hop_yields_nothing_when_the_first_hop_finds_nothing():
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000 * denomination(BYC),
        max_slippage_frac=0.01,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("h1", 900, 100)],  # far beyond the cap
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("h2", 100, 300, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert plan.is_empty
    assert plan.unspent_source == 10_000 * denomination(BYC)


# ---------------------------------------------------------------------------
# Incoherent requests
# ---------------------------------------------------------------------------

def test_anchor_must_be_finite_positive_and_carry_provenance():
    with pytest.raises(PlanError):
        Anchor(rate=0.0, source="x")
    with pytest.raises(PlanError):
        Anchor(rate=float("inf"), source="x")
    with pytest.raises(PlanError):
        Anchor(rate=float("nan"), source="x")
    with pytest.raises(PlanError):
        Anchor(rate=2.0, source="")


@pytest.mark.parametrize(
    "kwargs",
    [
        dict(source_asset=XCH, target_asset=XCH, budget=10 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH, budget=0 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH, budget=-5 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH, budget=10 * denomination(BYC), max_slippage_frac=-0.1),
    ],
)
def test_incoherent_requests_raise_rather_than_returning_an_empty_plan(kwargs):
    # An empty plan means "nothing was cheap enough", which is a valid
    # answer.  A malformed request is a different thing and must not be
    # reported the same way.
    with pytest.raises(PlanError):
        build_plan(direct_offers=[], direct_anchor=ANCHOR, **kwargs)


def test_two_hop_without_both_anchors_raises():
    with pytest.raises(PlanError):
        build_plan(
            source_asset=BYC, target_asset=DBX, budget=10 * denomination(BYC),
            max_slippage_frac=0.1, direct_offers=[], direct_anchor=None,
            hop_asset=XCH,
            first_hop_offers=[offer("h1", 2, 1)], first_hop_anchor=None,
            second_hop_offers=[], second_hop_anchor=None,
        )


def test_no_route_at_all_returns_an_empty_plan_not_an_error():
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000 * denomination(BYC),
        max_slippage_frac=1.0, direct_offers=[], direct_anchor=None,
    )
    assert plan.is_empty
    assert plan.unspent_source == 10_000 * denomination(BYC)


# ---------------------------------------------------------------------------
# Review round 1: seven findings, each pinned
# ---------------------------------------------------------------------------

def test_rates_are_denominated_so_they_compare_with_a_display_anchor():
    """XCH is 10^12 raw units per display unit, a CAT is 10^3.

    Forming give/receive on raw amounts left a 2-BYC-per-XCH offer reading as
    2e-9 against a 2.0 anchor -- a factor of 10^9, which either passes every
    offer or fails every offer depending on direction.
    """
    o = OfferCandidate(
        offer_id="x", give_asset=BYC, receive_asset=XCH,
        give_amount=2 * denomination(BYC),      # 2 BYC
        receive_amount=1 * denomination(XCH),   # 1 XCH
    )
    assert effective_rate(o) == pytest.approx(2.0)


def test_the_cap_boundary_survives_binary_float_error():
    """give/receive = 7/100 against anchor 0.05 computes 0.4000000000000001,
    so an offer sitting exactly ON a 0.4 cap was rejected by a bare `>`."""
    o = OfferCandidate(
        offer_id="edge", give_asset=BYC, receive_asset=XCH,
        give_amount=7 * denomination(BYC),
        receive_amount=100 * denomination(XCH),
    )
    anchor = Anchor(rate=0.05, source="test")
    deviation = rate_deviation_frac(effective_rate(o), anchor)
    assert deviation > 0.4            # the raw float really is over
    plan = build_plan(
        source_asset=BYC, target_asset=XCH,
        budget=1_000 * denomination(BYC), max_slippage_frac=0.4,
        direct_offers=[o], direct_anchor=anchor,
    )
    assert plan.take_count == 1       # ...and is still accepted


def test_an_offer_without_an_id_is_malformed():
    """Compact dexie responses need the id to fetch the payload, so such an
    offer cannot be executed however good its price looks."""
    plan = build_plan(
        source_asset=BYC, target_asset=XCH,
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("", 100, 100)], direct_anchor=ANCHOR,
    )
    assert plan.is_empty
    assert plan.skipped_malformed == 1


@pytest.mark.parametrize("bad", [float("nan"), float("inf"), float("-inf")])
def test_a_non_finite_cap_is_refused_rather_than_disabling_the_cap(bad):
    """NaN fails every comparison, so it passed `< 0.0` AND then made every
    `deviation > cap` false -- silently removing the slippage limit."""
    with pytest.raises(PlanError, match="finite"):
        build_plan(
            source_asset=BYC, target_asset=XCH,
            budget=10_000 * denomination(BYC), max_slippage_frac=bad,
            direct_offers=[offer("a", 100, 1)], direct_anchor=ANCHOR,
        )


def test_oversized_only_direct_route_reports_its_oversized_count():
    """"Nothing within your cap" and "nothing small enough" are different
    problems with different fixes; the counter was dropped on this path."""
    plan = build_plan(
        source_asset=BYC, target_asset=XCH,
        budget=10 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("big", 5_000, 2_500)], direct_anchor=ANCHOR,
    )
    assert plan.is_empty
    assert plan.skipped_too_large == 1


def test_oversized_first_hop_reports_its_oversized_count():
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=10 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("big", 5_000, 2_500)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("s", 1, 1, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert plan.is_empty
    assert plan.skipped_too_large == 1


def test_a_second_hop_that_cannot_sell_is_not_a_route():
    """The first leg would spend source to buy an intermediate the second leg
    cannot sell -- receiving no target while paying in full.  is_empty was
    false (the first leg has offers), so it would have executed."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("f", 200, 100)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[],                       # nothing to sell into
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert plan.is_empty
    assert plan.legs == []
    assert plan.unspent_source == 10_000 * denomination(BYC)


def test_an_underfilled_second_hop_reports_the_stranded_residual():
    """The difference is left in an asset the operator never asked to hold.
    Surfaced so the dialog can say so before anything runs."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("f", 200, 100)],     # yields 100 XCH
        first_hop_anchor=ANCHOR,
        second_hop_offers=[                          # sells only 60 of them
            offer("s", 60, 60, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert plan.take_count == 2
    assert plan.hop_residual == 40 * denomination(XCH)


def test_realised_rate_is_denominated_like_effective_rate():
    """The blended leg rate had the same 10^9 error as the per-offer rate,
    and survived the first fix.  It is shown beside the operator's anchor in
    the confirmation dialog, so a mismatch invites a meaningless comparison."""
    plan = build_plan(
        source_asset=BYC, target_asset=XCH,
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("a", 200, 100), offer("b", 220, 100)],
        direct_anchor=ANCHOR,
    )
    leg = plan.legs[0]
    # 420 BYC given for 200 XCH received -> 2.1, not 2.1e-9.
    assert leg.realised_rate == pytest.approx(2.1)


# ---------------------------------------------------------------------------
# Review round 2
# ---------------------------------------------------------------------------

def test_a_dust_direct_fill_no_longer_hides_a_complete_two_hop_route():
    """Returning on ANY direct offer let 1 unit of dust liquidity strand 999
    of 1,000 source units while a complete two-hop route sat unused."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=1_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("dust", 1, 1, give_asset=BYC, recv_asset=DBX)],
        direct_anchor=Anchor(rate=1.0, source="test"),
        hop_asset=XCH,
        first_hop_offers=[offer("f", 1_000, 500)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[
            offer("s", 500, 500, give_asset=XCH, recv_asset=DBX)
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 2
    assert plan.give_total == 1_000 * denomination(BYC)


def test_direct_still_wins_when_its_coverage_matches():
    """Each extra hop is another all-or-nothing take with its own fee and its
    own window for the book to move, so direct keeps ties."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=1_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("d", 1_000, 1_000, give_asset=BYC, recv_asset=DBX)],
        direct_anchor=Anchor(rate=1.0, source="test"),
        hop_asset=XCH,
        first_hop_offers=[offer("f", 1_000, 500)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("s", 500, 500, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 1
    assert [o.offer_id for o in plan.legs[0].offers] == ["d"]


def test_the_route_cap_is_not_applied_twice():
    """Applying the same cap per leg let two 10% hops deliver a composite 21%
    worse than the anchors -- while the operator's limit said 10%."""
    from gui.services.consolidate.planner import per_leg_cap

    assert per_leg_cap(0.10, 1) == pytest.approx(0.10)
    leg = per_leg_cap(0.10, 2)
    assert leg == pytest.approx(0.0488, abs=1e-4)
    # The whole point: compounding the per-leg bound reproduces the route cap.
    assert (1 + leg) ** 2 - 1 == pytest.approx(0.10)


def test_two_near_cap_legs_are_refused_by_the_route_cap():
    """Both legs sit at ~9%, inside the old per-leg reading and outside the
    operator's actual 10% route cap."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=10_000 * denomination(BYC), max_slippage_frac=0.10,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("f", 218, 100)],          # 2.18 vs 2.0 = +9%
        first_hop_anchor=ANCHOR,
        second_hop_offers=[
            offer("s", 109, 100, give_asset=XCH, recv_asset=DBX)  # +9%
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert plan.is_empty


def test_a_repeated_offer_id_is_planned_once():
    """The first take consumes the offer and the second fails -- after the
    plan has already partially executed."""
    dupe = offer("same", 200, 100)
    plan = build_plan(
        source_asset=BYC, target_asset=XCH,
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[dupe, offer("same", 210, 100), dupe],
        direct_anchor=ANCHOR,
    )
    assert plan.take_count == 1
    assert plan.skipped_duplicate == 2


def test_whitespace_provenance_is_refused():
    """It passes `not source` and shows the operator nothing at all."""
    with pytest.raises(PlanError, match="provenance"):
        Anchor(rate=1.0, source="   ")
