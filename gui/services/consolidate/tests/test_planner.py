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
    effective_rate,
    rate_deviation_frac,
)

XCH = "xch"
BYC = "byc"
DBX = "dbx"


def offer(oid: str, give: int, recv: int, *, give_asset=BYC, recv_asset=XCH, status=0):
    return OfferCandidate(
        offer_id=oid,
        give_asset=give_asset,
        receive_asset=recv_asset,
        give_amount=give,
        receive_amount=recv,
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
        source_asset=BYC, target_asset=XCH, budget=10_000,
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
        source_asset=BYC, target_asset=XCH, budget=402,
        max_slippage_frac=0.05, direct_offers=offers, direct_anchor=ANCHOR,
    )
    wide = build_plan(
        source_asset=BYC, target_asset=XCH, budget=402,
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
        source_asset=BYC, target_asset=XCH, budget=1_000_000,
        max_slippage_frac=0.99, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert plan.is_empty
    assert plan.skipped_worse_than_cap == 1


def test_cap_boundary_is_inclusive():
    # Exactly at the cap is accepted; a hair beyond is not.  Pinned because
    # an off-by-one here silently changes what the operator's number means.
    at_cap = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000,
        max_slippage_frac=0.5, direct_offers=[offer("x", 300, 100)],
        direct_anchor=ANCHOR,
    )
    assert at_cap.take_count == 1

    past_cap = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000,
        max_slippage_frac=0.5, direct_offers=[offer("x", 301, 100)],
        direct_anchor=ANCHOR,
    )
    assert past_cap.take_count == 0


def test_tail_beyond_the_cap_is_counted_not_walked():
    offers = [offer("ok", 200, 100)] + [offer(f"bad{i}", 900, 100) for i in range(5)]
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=10_000,
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
        source_asset=BYC, target_asset=XCH, budget=1_000,
        max_slippage_frac=1.0, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert [o.offer_id for o in plan.legs[0].offers] == ["fits"]
    assert plan.skipped_too_large == 1


def test_planner_never_spends_more_than_the_budget():
    offers = [offer(f"o{i}", 300, 100) for i in range(20)]
    plan = build_plan(
        source_asset=BYC, target_asset=XCH, budget=1_000,
        max_slippage_frac=1.0, direct_offers=offers, direct_anchor=ANCHOR,
    )
    assert plan.give_total <= 1_000


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
        source_asset=BYC, target_asset=XCH, budget=10_000,
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
        source_asset=BYC, target_asset=DBX, budget=10_000,
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
        source_asset=BYC, target_asset=DBX, budget=10_000,
        max_slippage_frac=1.0,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("h1", 200, 100)],
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("h2", 100, 300, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert len(plan.legs) == 2
    assert plan.give_total == 200      # BYC spent
    assert plan.receive_total == 300   # DBX received


def test_second_hop_can_only_spend_what_the_first_actually_yields():
    """If hop one underfills, hop two must shrink with it.

    Budgeting hop two from anything other than hop one's realised output
    would promise a quantity that does not exist when the plan runs.
    """
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000,
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
        source_asset=BYC, target_asset=DBX, budget=10_000,
        max_slippage_frac=0.01,
        direct_offers=[], direct_anchor=None,
        hop_asset=XCH,
        first_hop_offers=[offer("h1", 900, 100)],  # far beyond the cap
        first_hop_anchor=ANCHOR,
        second_hop_offers=[offer("h2", 100, 300, give_asset=XCH, recv_asset=DBX)],
        second_hop_anchor=Anchor(rate=1.0, source="test"),
    )
    assert plan.is_empty
    assert plan.unspent_source == 10_000


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
        dict(source_asset=XCH, target_asset=XCH, budget=10, max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH, budget=0, max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH, budget=-5, max_slippage_frac=0.1),
        dict(source_asset=BYC, target_asset=XCH, budget=10, max_slippage_frac=-0.1),
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
            source_asset=BYC, target_asset=DBX, budget=10,
            max_slippage_frac=0.1, direct_offers=[], direct_anchor=None,
            hop_asset=XCH,
            first_hop_offers=[offer("h1", 2, 1)], first_hop_anchor=None,
            second_hop_offers=[], second_hop_anchor=None,
        )


def test_no_route_at_all_returns_an_empty_plan_not_an_error():
    plan = build_plan(
        source_asset=BYC, target_asset=DBX, budget=10_000,
        max_slippage_frac=1.0, direct_offers=[], direct_anchor=None,
    )
    assert plan.is_empty
    assert plan.unspent_source == 10_000
