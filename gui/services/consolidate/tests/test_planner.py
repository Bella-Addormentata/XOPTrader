"""Tests for the emergency consolidation planner.

The planner is the part of this feature that can lose money silently, so
these pin behaviour rather than implementation: what gets taken, in what
order, and -- most importantly -- what gets refused.
"""

from __future__ import annotations

import pytest

from gui.services.consolidate.planner import (
    Anchor,
    OfferCandidate,
    PlanError,
    build_plan,
    denomination,
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
        dict(source_asset=XCH, target_asset=XCH,
             budget=10 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH,
             budget=0 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH,
             budget=-5 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH,
             budget=10 * denomination(BYC), max_slippage_frac=-0.1),
        dict(source_asset="   ", target_asset=XCH,
             budget=10 * denomination(BYC), max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset="",
             budget=10 * denomination(BYC), max_slippage_frac=0.1),
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


def test_an_underfilled_second_hop_does_not_beat_a_good_direct_fill():
    """[round 3] Comparing against first.give_total counted source SPENT on
    hop one, not source DELIVERED to the target. A first hop spending 1,000
    followed by a second absorbing only 1 of them beat a 999-unit direct
    fill -- delivering almost nothing and stranding the rest in an asset the
    operator never asked to hold."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=1_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("d", 999, 999, give_asset=BYC, recv_asset=DBX)],
        direct_anchor=Anchor(rate=1.0, source="test"),
        hop_asset=XCH,
        first_hop_offers=[offer("f", 1_000, 1_000)],   # spends all 1,000 BYC
        first_hop_anchor=Anchor(rate=1.0, source="test"),
        second_hop_offers=[                            # ...but sells 1 XCH
            offer("s", 1, 1, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 1
    assert [o.offer_id for o in plan.legs[0].offers] == ["d"]


def test_a_fully_absorbed_two_hop_still_wins_on_real_coverage():
    """The counterpart: when the second hop actually absorbs the first, the
    two-hop route delivers more and should still be chosen."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=1_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("d", 100, 100, give_asset=BYC, recv_asset=DBX)],
        direct_anchor=Anchor(rate=1.0, source="test"),
        hop_asset=XCH,
        first_hop_offers=[offer("f", 1_000, 1_000)],
        first_hop_anchor=Anchor(rate=1.0, source="test"),
        second_hop_offers=[
            offer("s", 1_000, 1_000, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 2
    assert plan.hop_residual == 0


def test_asset_ids_match_case_insensitively():
    """Wallet ids are lowercased while settings can carry uppercase, so exact
    matching classified a good offer as malformed and returned an empty plan --
    which the operator would have read as "nothing was cheap enough"."""
    plan = build_plan(
        source_asset="BYC", target_asset="XCH",
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("a", 200, 100)],   # built with lowercase ids
        direct_anchor=ANCHOR,
    )
    assert plan.take_count == 1
    assert plan.skipped_malformed == 0


def test_same_asset_is_caught_regardless_of_case():
    with pytest.raises(PlanError, match="same asset"):
        build_plan(
            source_asset="XCH", target_asset="xch",
            budget=10 * denomination(BYC), max_slippage_frac=0.1,
            direct_offers=[], direct_anchor=None,
        )


def test_a_whitespace_only_offer_id_is_malformed():
    plan = build_plan(
        source_asset=BYC, target_asset=XCH,
        budget=10_000 * denomination(BYC), max_slippage_frac=10.0,
        direct_offers=[offer("   ", 100, 100)], direct_anchor=ANCHOR,
    )
    assert plan.is_empty
    assert plan.skipped_malformed == 1


@pytest.mark.parametrize("hop", [BYC, DBX, " byc ", "DBX"])
def test_a_hop_that_is_an_endpoint_is_refused(hop):
    """hop == source or target is a self-conversion leg, not a route. Only
    the anchors were validated, so it produced a degenerate plan.

    [review] The third case used to be "XCH".lower(), which is not an
    endpoint of this BYC-to-DBX request, guarded by an early return -- so
    that parameter ran no assertion at all. Replaced with case and whitespace
    variants of the real endpoints, which is what the guard was standing in
    for, and the guard is gone so every case now exercises the refusal.
    """
    with pytest.raises(PlanError, match="third asset"):
        build_plan(
            source_asset=BYC, target_asset=DBX,
            budget=10 * denomination(BYC), max_slippage_frac=0.1,
            direct_offers=[], direct_anchor=None,
            hop_asset=hop,
            first_hop_offers=[], first_hop_anchor=ANCHOR,
            second_hop_offers=[], second_hop_anchor=ANCHOR,
        )


def test_a_blank_hop_asset_is_refused():
    """"   " folds to empty and passed the endpoint check, letting the planner
    build legs through an unnamed asset."""
    with pytest.raises(PlanError, match="blank"):
        build_plan(
            source_asset=BYC, target_asset=DBX,
            budget=10 * denomination(BYC), max_slippage_frac=0.1,
            direct_offers=[], direct_anchor=None,
            hop_asset="   ",
            first_hop_offers=[], first_hop_anchor=ANCHOR,
            second_hop_offers=[], second_hop_anchor=ANCHOR,
        )


# ---------------------------------------------------------------------------
# Review round 4
# ---------------------------------------------------------------------------

def test_a_padded_asset_id_is_still_denominated_as_xch():
    """`denomination` lowercased where everything else strips AND lowercases.

    `" XCH ".lower()` is `" xch "`, which is not `"xch"`, so an id that
    `_usable` had just accepted as XCH fell through to the CAT branch -- the
    10^9 error again, from the one place left folding ids differently.

    Asserted against the chain's own numbers rather than against this
    module's constants: the tests in this file have already once agreed with
    a denomination bug by deriving their expectations from the code.
    """
    assert denomination(" XCH ") == 10 ** 12
    assert denomination("\tXCH\n") == 10 ** 12
    assert denomination(" byc ") == 10 ** 3


def test_a_padded_asset_id_does_not_skew_an_offers_rate():
    """Built by hand rather than through `offer()`: that helper scales with
    `denomination` too, so it would have cancelled the very bug this pins."""
    padded = OfferCandidate(
        offer_id="p", give_asset=" XCH ", receive_asset=BYC,
        give_amount=100 * 10 ** 12,     # 100 XCH, in mojos
        receive_amount=200 * 10 ** 3,   # 200 BYC, in CAT mojos
    )
    assert effective_rate(padded) == pytest.approx(0.5)


def test_a_padded_source_asset_does_not_skew_the_realised_rate():
    """`Leg.give_asset` is the caller's raw settings string, untrimmed, and
    the realised rate built from it is what the confirmation dialog shows
    beside the operator's anchor."""
    plan = build_plan(
        source_asset=" XCH ", target_asset=BYC,
        budget=100 * (10 ** 12), max_slippage_frac=0.1,
        direct_offers=[offer("a", 100, 200, give_asset=XCH, recv_asset=BYC)],
        direct_anchor=Anchor(rate=0.5, source="test"),
    )
    assert plan.take_count == 1
    # 100 XCH given for 200 BYC received -> 0.5, not 0.5e9.
    assert plan.legs[0].realised_rate == pytest.approx(0.5)


def test_a_bargain_first_hop_pays_for_a_dearer_second_hop():
    """The equal nth-root split is sufficient for the route cap, not
    equivalent to it.

    A first hop 10% BETTER than its anchor and a second 10% worse composite
    to -1%, inside a 10% ROUTE cap by any reading -- and the static 4.88%
    per-leg number threw the route away, in exactly the thin-book case this
    feature exists for.
    """
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=3_600 * denomination(BYC), max_slippage_frac=0.10,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("f", 3_600, 2_000)],      # 1.8 vs 2.0 = -10%
        first_hop_anchor=ANCHOR,
        second_hop_offers=[                               # 1.1 vs 1.0 = +10%
            offer("s", 1_100, 1_000, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 2
    first, second = plan.legs
    # From first principles: what a ROUTE cap bounds is the composite, and
    # this route's is -1%.  Derived from the anchors the plan carries, not
    # from any number the planner reports about its own cap arithmetic.
    composite = (
        (first.realised_rate / first.anchor.rate)
        * (second.realised_rate / second.anchor.rate)
    ) - 1.0
    assert composite == pytest.approx(-0.01)
    assert composite <= 0.10


def test_a_slightly_dear_first_hop_still_leaves_the_second_its_headroom():
    """+1% then +5.5% composites to +6.55%, well inside a 10% route cap, yet
    the flat 4.88% split refused the second leg."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=2_020 * denomination(BYC), max_slippage_frac=0.10,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("f", 2_020, 1_000)],      # 2.02 vs 2.0 = +1%
        first_hop_anchor=ANCHOR,
        second_hop_offers=[                            # 1.055 vs 1.0 = +5.5%
            offer("s", 844, 800, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 2
    assert (1.01 * 1.055) - 1.0 == pytest.approx(0.06555)   # inside 10%


def test_a_dear_first_hop_shrinks_what_the_second_may_spend():
    """The other half of the same rule.  +4% then +6% composites to +10.24%,
    which is OUTSIDE the operator's 10%, so widening the second leg to the
    full route cap would be just as wrong as pinning it at 4.88%."""
    plan = build_plan(
        source_asset=BYC, target_asset=DBX,
        budget=2_080 * denomination(BYC), max_slippage_frac=0.10,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("f", 2_080, 1_000)],      # 2.08 vs 2.0 = +4%
        first_hop_anchor=ANCHOR,
        second_hop_offers=[                               # 1.06 vs 1.0 = +6%
            offer("s", 530, 500, give_asset=XCH, recv_asset=DBX),
        ],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert (1.04 * 1.06) - 1.0 > 0.10       # first principles: past the cap
    assert plan.is_empty


def test_the_second_hop_allowance_multiplies_back_out_to_the_route_cap():
    from gui.services.consolidate.planner import per_leg_cap, remaining_route_cap

    for d1 in (0.0, 0.005, 0.01, 0.02, 0.0488):
        allowance = remaining_route_cap(0.10, d1)
        assert (1.0 + d1) * (1.0 + allowance) - 1.0 == pytest.approx(0.10)
    # A first hop better than its anchor buys headroom, but never more than
    # the headline number: the "bargain" is itself only an anchor estimate,
    # and no single take should be executed further from its own reference
    # than the operator asked for.
    assert remaining_route_cap(0.10, -0.10) == pytest.approx(0.10)
    assert remaining_route_cap(0.10, -0.90) == pytest.approx(0.10)
    # A degenerate deviation must not divide by zero; fall back to the split.
    assert remaining_route_cap(0.10, -1.0) == pytest.approx(per_leg_cap(0.10, 2))


def test_route_coverage_is_compared_exactly_above_two_to_the_fifty_third():
    """Coverage was `first.give_total * (second.give_total /
    first.receive_total)` in binary floats.  XCH carries 10^12 mojos per
    display unit, so 2^53 mojos is only ~9,007 XCH -- an ordinary position,
    not a pathological one.

    Here the two-hop route delivers EXACTLY the direct fill (10,010 * 9/11 =
    8,190 XCH), a tie direct is documented to win because a second
    all-or-nothing take is strictly more dangerous.  In floats the product
    lands one mojo high and the riskier route is chosen instead.
    """
    g1, r1, g2 = 10_010 * 10 ** 12, 11 * 10 ** 3, 9 * 10 ** 3
    assert g1 > 2 ** 53
    assert g1 * g2 % r1 == 0                        # exact: a dead tie...
    assert g1 * g2 // r1 == 8_190 * 10 ** 12
    assert g1 * (g2 / r1) > 8_190 * 10 ** 12        # ...that floats lose

    plan = build_plan(
        source_asset=XCH, target_asset=DBX,
        budget=10_010 * denomination(XCH), max_slippage_frac=0.10,
        direct_offers=[offer("d", 8_190, 8_190, give_asset=XCH, recv_asset=DBX)],
        direct_anchor=Anchor(rate=1.0, source="test"),
        hop_asset=BYC,
        first_hop_offers=[offer("f", 10_010, 11, give_asset=XCH, recv_asset=BYC)],
        first_hop_anchor=Anchor(rate=910.0, source="test"),
        second_hop_offers=[offer("s", 9, 9, give_asset=BYC, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 1
    assert [o.offer_id for o in plan.legs[0].offers] == ["d"]


def test_a_blank_endpoint_is_refused():
    """A blank source folds to "" -- not equal to the target, so planning
    went ahead: `_usable` then matched any offer whose own give_asset was
    blank, and `denomination("   ")` quietly called it a CAT.  The same guard
    already existed for the hop and was never applied to the endpoints."""
    with pytest.raises(PlanError, match="must be named"):
        build_plan(
            source_asset="   ", target_asset=XCH,
            budget=10 * denomination(BYC), max_slippage_frac=0.1,
            direct_offers=[], direct_anchor=ANCHOR,
        )
    with pytest.raises(PlanError, match="must be named"):
        build_plan(
            source_asset=BYC, target_asset="",
            budget=10 * denomination(BYC), max_slippage_frac=0.1,
            direct_offers=[], direct_anchor=ANCHOR,
        )


# ---------------------------------------------------------------------------
# [review round 5] Ordering must be exact, and "materially better" must mean
# something.
# ---------------------------------------------------------------------------

def test_the_sort_key_separates_rates_that_collide_as_floats():
    """2**53 + 1 is not representable as a double, so it equals 2**53.

    XCH carries 10^12 raw units per display unit, which puts ~9,007 XCH at
    that boundary -- an ordinary position, not a pathological one. Two
    distinct prices then sort as a tie, input order decides, and if only one
    offer fits the planner takes the worse of them.
    """
    from gui.services.consolidate.planner import _exact_rate_key

    recv = 2 ** 52
    better = OfferCandidate(offer_id="better", give_asset=XCH,
                            receive_asset=BYC,
                            give_amount=2 ** 53, receive_amount=recv)
    worse = OfferCandidate(offer_id="worse", give_asset=XCH,
                           receive_asset=BYC,
                           give_amount=2 ** 53 + 1, receive_amount=recv)

    # Indistinguishable in floating point...
    assert float(worse.give_amount) == float(better.give_amount)
    # ...and correctly ordered exactly.
    assert _exact_rate_key(better) < _exact_rate_key(worse)
    assert not _exact_rate_key(worse) < _exact_rate_key(better)


def test_the_sort_key_puts_unusable_offers_last():
    from gui.services.consolidate.planner import _exact_rate_key

    good = OfferCandidate(offer_id="g", give_asset=XCH, receive_asset=BYC,
                          give_amount=100, receive_amount=50)
    dud = OfferCandidate(offer_id="d", give_asset=XCH, receive_asset=BYC,
                         give_amount=100, receive_amount=0)
    assert _exact_rate_key(good) < _exact_rate_key(dud)
    assert not _exact_rate_key(dud) < _exact_rate_key(good)


def test_a_marginally_better_two_hop_route_loses_to_direct():
    """A second leg is a second fee and a second all-or-nothing window.

    [round 3] The first version of this test gave both routes identical
    source coverage, so it passed under the OLD source-scored comparison too
    and pinned nothing about materiality. The two-hop route here delivers
    strictly MORE target -- 1,010 against 1,000 -- but only 1%, under the 2%
    bar, so direct must still win.
    """
    direct = offer("direct", 1_000, 1_000, give_asset=BYC, recv_asset=XCH)
    hop1 = offer("h1", 1_000, 1_000, give_asset=BYC, recv_asset="usds")
    hop2 = offer("h2", 1_000, 1_010, give_asset="usds", recv_asset=XCH)
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, hop_asset="usds",
        budget=1_000 * denomination(BYC),
        direct_offers=[direct],
        direct_anchor=Anchor(rate=1.0, source="test"),
        first_hop_offers=[hop1], first_hop_anchor=Anchor(rate=1.0, source="t"),
        second_hop_offers=[hop2], second_hop_anchor=Anchor(rate=1.0, source="t"),
        max_slippage_frac=0.10,
    )
    assert len(plan.legs) == 1, "1% more target is not materially better"
    # And the two-hop route really did offer more, so the bar is what refused
    # it rather than the route being worse.
    assert hop2.receive_amount > direct.receive_amount


def test_the_materiality_bar_is_driven_by_its_named_constant():
    """[round 3] The comparison used a hand-written 102/100 beside a named
    MIN_TWO_HOP_ADVANTAGE, so changing the constant moved nothing."""
    from gui.services.consolidate import planner as P

    assert P._ADVANTAGE_NUM / P._ADVANTAGE_DEN == pytest.approx(
        1.0 + P.MIN_TWO_HOP_ADVANTAGE
    )


def test_the_package_facade_exports_denomination():
    import gui.services.consolidate as pkg

    assert hasattr(pkg, "denomination")
    assert "denomination" in pkg.__all__


def test_a_two_hop_route_delivering_less_target_never_wins():
    """[audit] The comparison used to score SOURCE delivered, not target.

    So a permitted-but-expensive second hop could win while handing the
    operator strictly less of the thing they asked for.
    """
    direct = offer("direct", 1_000, 900, give_asset=BYC, recv_asset=XCH)
    hop1 = offer("h1", 1_000, 1_000, give_asset=BYC, recv_asset="usds")
    # Consumes the whole first hop but returns far less target.
    hop2 = offer("h2", 1_000, 400, give_asset="usds", recv_asset=XCH)
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, hop_asset="usds",
        budget=1_000 * denomination(BYC),
        direct_offers=[direct],
        direct_anchor=Anchor(rate=1.111, source="test"),
        first_hop_offers=[hop1], first_hop_anchor=Anchor(rate=1.0, source="t"),
        second_hop_offers=[hop2], second_hop_anchor=Anchor(rate=2.5, source="t"),
        max_slippage_frac=0.10,
    )
    assert len(plan.legs) == 1
    assert plan.receive_total == direct.receive_amount


def test_a_two_hop_route_delivering_materially_more_target_wins():
    direct = offer("direct", 1_000, 400, give_asset=BYC, recv_asset=XCH)
    hop1 = offer("h1", 1_000, 1_000, give_asset=BYC, recv_asset="usds")
    hop2 = offer("h2", 1_000, 900, give_asset="usds", recv_asset=XCH)
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, hop_asset="usds",
        budget=1_000 * denomination(BYC),
        direct_offers=[direct],
        direct_anchor=Anchor(rate=2.5, source="test"),
        first_hop_offers=[hop1], first_hop_anchor=Anchor(rate=1.0, source="t"),
        second_hop_offers=[hop2], second_hop_anchor=Anchor(rate=1.111, source="t"),
        max_slippage_frac=0.10,
    )
    assert len(plan.legs) == 2
    assert plan.receive_total > direct.receive_amount


def test_an_infinite_first_leg_deviation_falls_back_rather_than_refusing_all():
    """[audit] inf is the only degenerate value that reaches this.

    realised_rate returns inf when receive_total is zero, so
    `1.0 + inf > 0.0` was True and the guard never fired: the division by
    infinity produced -1.0, a NEGATIVE cap that rejects every offer.
    """
    from gui.services.consolidate.planner import (
        per_leg_cap,
        remaining_route_cap,
    )

    assert remaining_route_cap(0.10, float("inf")) == per_leg_cap(0.10, 2)
    assert remaining_route_cap(0.10, float("nan")) == per_leg_cap(0.10, 2)
    assert remaining_route_cap(0.10, float("inf")) > 0.0
