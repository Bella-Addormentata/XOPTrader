"""Pure planning logic for emergency consolidation.

No network, no wallet, no Qt -- everything here is a function of its
arguments, so the part where a mistake costs money is the part that can be
tested exhaustively.

THE ANCHOR PROBLEM
------------------
The obvious reference for "is this offer a fair price" is the median of the
book being swept.  On these books that is fatal.  From one block of
``logs/xop_trader.1.log`` on 2026-08-25 (05:18:24-27), counting offers the
engine's own outlier filter rejected against those it kept:

    wmilliETH.b/XCH   8 rejected vs   3 kept    (~73% junk)
    BYC/wUSDC.b       6 rejected vs   7 kept    (~46% junk, and one-sided)
    XCH/wUSDC.b       9 rejected vs  13 kept
    XCH/BYC           6 rejected vs  29 kept

A median tolerates contamination below 50%.  Two of those books are at or
past it -- and they are exactly the books an operator escaping an impaired
asset would be sweeping.  So the anchor MUST come from outside the book
being swept.  ``Anchor`` carries its own provenance for that reason: the UI
shows where the number came from, so a nonsense reference can be spotted
before execution rather than after.

DIRECTION CONVENTION
--------------------
Every rate in this module is expressed as **units of the asset we give per
unit of the asset we receive** -- the price of what we are buying, in what
we are spending.  Lower is better, always.  Fixing one convention here and
converting at the edges avoids the inverted-rate class of bug entirely.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Optional, Sequence

__all__ = [
    "Anchor",
    "denomination",
    "ConsolidationPlan",
    "Leg",
    "OfferCandidate",
    "PlanError",
    "build_plan",
    "effective_rate",
    "rate_deviation_frac",
]


#: Raw units per display unit.  XCH is 10^12 mojos; every CAT is 10^3.  The
#: same split the engine applies (``rec.asset_id == "xch" ? 1e12 : 1e3`` in
#: compute_portfolio_equity_usd) and the config parser enforces.
XCH_UNITS = 10 ** 12
CAT_UNITS = 10 ** 3


def denomination(asset_id: str) -> int:
    """Raw units per display unit of ``asset_id``."""
    return XCH_UNITS if asset_id.lower() == "xch" else CAT_UNITS


class PlanError(ValueError):
    """Raised when a plan cannot be built at all (as opposed to being empty)."""


@dataclass(frozen=True)
class Anchor:
    """A reference rate that did NOT come from the book being swept.

    Parameters
    ----------
    rate:
        Give-per-receive, same convention as everything else here.
    source:
        Human-readable provenance, shown verbatim in the confirmation
        dialog.  "dexie price_last", "engine snapshot mid", "implied via
        XCH".  The operator is the last line of defence against a bad
        anchor, and they can only exercise that if they can see it.
    """

    rate: float
    source: str

    def __post_init__(self) -> None:
        if not (self.rate > 0.0) or self.rate != self.rate or self.rate == float("inf"):
            raise PlanError(f"anchor rate must be finite and positive, got {self.rate!r}")
        if not self.source.strip():
            # The operator is the last line of defence against a bad
            # reference and can only exercise that if the dialog shows them
            # something. "   " passes `not self.source` and displays as
            # nothing at all.
            raise PlanError("anchor must carry its provenance")


@dataclass(frozen=True)
class OfferCandidate:
    """One dexie offer, reduced to what the planner needs.

    ``give_amount`` / ``receive_amount`` are in the raw integer units of
    their respective assets (mojos for XCH, CAT mojos otherwise), exactly as
    the wallet reports them.

    An earlier version of this docstring claimed the planner could form
    ratios directly because "unit scaling cancels".  **It does not cancel
    across two different assets.**  XCH carries 10^12 raw units per display
    unit and a CAT carries 10^3, so a 2 BYC-per-XCH offer expressed raw is
    2000 / 10^12 = 2e-9 -- while a dexie or engine anchor for the same offer
    is 2.0, a factor of 10^9 apart.  Depending on direction that makes every
    offer pass the cap or every offer fail it, and neither failure looks
    like a bug from the outside.  :func:`effective_rate` normalises both
    sides by :func:`denomination` so rates are display-denominated and
    directly comparable with the anchor.
    """

    offer_id: str
    give_asset: str
    receive_asset: str
    give_amount: int
    receive_amount: int
    status: int = 0  # 0 == PENDING_ACCEPT, i.e. takeable


@dataclass(frozen=True)
class Leg:
    """One hop of a plan: a set of offers taken to convert one asset to another."""

    give_asset: str
    receive_asset: str
    anchor: Anchor
    offers: tuple[OfferCandidate, ...]
    give_total: int
    receive_total: int

    @property
    def realised_rate(self) -> float:
        """Blended give-per-receive for the whole leg, in DISPLAY units.

        Same denomination trap as effective_rate, and it survived the fix
        there: dividing the raw totals reports a 2-BYC-per-XCH leg as 2e-9.
        This is the number a confirmation dialog shows next to the anchor the
        operator chose, so the two must be in the same units or the comparison
        it invites is meaningless.
        """
        if self.receive_total <= 0:
            return float("inf")
        give = self.give_total / denomination(self.give_asset)
        receive = self.receive_total / denomination(self.receive_asset)
        if receive <= 0.0:
            return float("inf")
        return give / receive


@dataclass
class ConsolidationPlan:
    """What the button will do, in enough detail to be shown before it runs."""

    source_asset: str
    target_asset: str
    legs: list[Leg] = field(default_factory=list)
    skipped_worse_than_cap: int = 0
    skipped_malformed: int = 0
    skipped_too_large: int = 0
    skipped_duplicate: int = 0
    unspent_source: int = 0
    hop_residual: int = 0
    """Hop-asset units the second leg could not spend.

    A two-hop plan buys the intermediate asset with the FIRST take and sells
    it with the second.  When the second leg cannot absorb everything the
    first yields -- its offers are too large, too expensive, or too few --
    the difference is left sitting in an asset the operator did not want and
    did not ask to hold.  Silently, previously.  It is surfaced so the
    confirmation dialog can state it before anything executes.
    """

    @property
    def is_empty(self) -> bool:
        return not any(leg.offers for leg in self.legs)

    @property
    def take_count(self) -> int:
        return sum(len(leg.offers) for leg in self.legs)

    @property
    def receive_total(self) -> int:
        return self.legs[-1].receive_total if self.legs else 0

    @property
    def give_total(self) -> int:
        return self.legs[0].give_total if self.legs else 0


def effective_rate(offer: OfferCandidate) -> float:
    """Give-per-receive for one offer.  Lower is better.

    ``inf`` for a degenerate offer, so it sorts last and is filtered out by
    any finite cap rather than needing a special case at every call site.
    """
    if offer.receive_amount <= 0 or offer.give_amount < 0:
        return float("inf")
    # Normalise to DISPLAY units on both sides.  Raw ratios do not cancel
    # across assets with different denominations -- see OfferCandidate.
    give = offer.give_amount / denomination(offer.give_asset)
    receive = offer.receive_amount / denomination(offer.receive_asset)
    if receive <= 0.0:
        return float("inf")
    return give / receive


#: The cap is documented as inclusive, but binary floats do not honour that
#: at the boundary: give/receive = 7/100 against an anchor of 0.05 computes a
#: deviation of 0.4000000000000001, so an offer sitting exactly ON a 0.4 cap
#: is rejected.  A relative tolerance restores the documented behaviour.  The
#: direction is deliberate -- at the boundary we ACCEPT, because rejecting an
#: at-cap offer is the surprising half and the operator chose that number.
_CAP_EPSILON = 1e-9


def within_cap(deviation: float, max_slippage_frac: float) -> bool:
    """Is ``deviation`` inside an inclusive cap, tolerant of float error?"""
    if deviation != deviation:                      # NaN deviation
        return False
    tolerance = _CAP_EPSILON * (1.0 + abs(max_slippage_frac))
    return deviation <= max_slippage_frac + tolerance


def rate_deviation_frac(rate: float, anchor: Anchor) -> float:
    """How much worse than the anchor a rate is, as a fraction.

    Positive means worse (we give more per unit received).  Negative means
    better than the reference, which is not suspicious on its own -- a
    genuinely good offer is why anyone runs this -- so callers must not
    filter on the absolute value.
    """
    if rate == float("inf"):
        return float("inf")
    return (rate - anchor.rate) / anchor.rate


def _usable(offer: OfferCandidate, give_asset: str, receive_asset: str) -> bool:
    """Shape validation, applied before any pricing is considered.

    A malformed offer is rejected on structure rather than on price so that
    a junk entry cannot reach the ranking at all -- it never gets the chance
    to look attractive.
    """
    if not offer.offer_id:
        # Compact dexie responses need this id to fetch the live offer
        # payload before it can be taken, so an offer without one cannot be
        # executed no matter how good its price.  Structural, not a price
        # judgement -- it never reaches the ranking.
        return False
    if offer.status != 0:
        return False
    if offer.give_asset != give_asset or offer.receive_asset != receive_asset:
        return False
    if offer.give_amount <= 0 or offer.receive_amount <= 0:
        return False
    return True


def _plan_leg(
    *,
    give_asset: str,
    receive_asset: str,
    budget: int,
    offers: Iterable[OfferCandidate],
    anchor: Anchor,
    max_slippage_frac: float,
    counters: dict[str, int],
    seen_ids: set[str],
) -> Leg:
    """Select offers for one hop, best price first, until the budget is spent.

    Best-first ordering is what makes a wide cap safe.  The cap decides only
    where to STOP, never what to take first, so widening it can add worse
    fills at the tail but can never displace a better one.  A 99% cap and a
    5% cap execute identically whenever the good offers cover the position --
    which is the property that lets an operator who believes an asset is
    worthless set the cap accordingly without also accepting bad execution
    on the part of the position that could have gone out at a fair price.
    """
    usable: list[OfferCandidate] = []
    for offer in offers:
        if not _usable(offer, give_asset, receive_asset):
            counters["malformed"] = counters.get("malformed", 0) + 1
            continue
        # A repeated id is the SAME on-chain offer arriving twice. Planning
        # it twice means the first take consumes it and the second fails --
        # after the plan has already partially executed, which is the worst
        # moment to discover it. Deduped across the whole plan, not just
        # this leg, because the same offer can appear in both hops' books.
        if offer.offer_id in seen_ids:
            counters["duplicate"] = counters.get("duplicate", 0) + 1
            continue
        seen_ids.add(offer.offer_id)
        usable.append(offer)

    usable.sort(key=effective_rate)

    chosen: list[OfferCandidate] = []
    give_total = 0
    receive_total = 0
    remaining = budget

    for index, offer in enumerate(usable):
        rate = effective_rate(offer)
        if not within_cap(rate_deviation_frac(rate, anchor), max_slippage_frac):
            # The list is sorted best-first, so every remaining offer is at
            # least this bad.  Count the whole tail from here and stop,
            # rather than walking it to re-derive the same verdict.
            counters["worse_than_cap"] = (
                counters.get("worse_than_cap", 0) + (len(usable) - index)
            )
            break
        if offer.give_amount > remaining:
            # take_offer is ALL-OR-NOTHING (see the comment at
            # cpp/src/engine.cpp:9921) -- there is no partial fill, so an
            # offer larger than the remaining budget cannot be trimmed.
            # Skip it and keep looking: a later, smaller offer may still
            # fit, and stopping here would strand spendable balance.
            counters["too_large"] = counters.get("too_large", 0) + 1
            continue
        chosen.append(offer)
        give_total += offer.give_amount
        receive_total += offer.receive_amount
        remaining -= offer.give_amount
        if remaining <= 0:
            break

    return Leg(
        give_asset=give_asset,
        receive_asset=receive_asset,
        anchor=anchor,
        offers=tuple(chosen),
        give_total=give_total,
        receive_total=receive_total,
    )


def per_leg_cap(route_cap: float, hops: int) -> float:
    """Split an operator's ROUTE slippage cap across ``hops`` legs.

    [review round 2] The same cap used to be applied independently to each
    hop, so a two-hop plan could accept 10% on the first leg and 10% on the
    second and deliver a composite 21% worse than the product of the anchors
    -- while telling the operator their limit was 10%. The bound on a route
    of n equally-capped legs is (1+s)^n - 1, not s.

    Inverting that gives each leg (1+s)^(1/n) - 1, so the COMPOSITE honours
    the number the operator actually chose. A 10% route cap becomes ~4.88%
    per leg over two hops. That is tighter and will sometimes find nothing --
    which is the correct answer to "keep me within 10%", and far better than
    quietly spending 21%.
    """
    if hops <= 1:
        return route_cap
    return (1.0 + route_cap) ** (1.0 / hops) - 1.0


def _single_leg_plan(
    source_asset: str, target_asset: str, leg: "Leg", budget: int,
    counters: dict[str, int]
) -> "ConsolidationPlan":
    return ConsolidationPlan(
        source_asset=source_asset,
        target_asset=target_asset,
        legs=[leg],
        skipped_worse_than_cap=counters.get("worse_than_cap", 0),
        skipped_malformed=counters.get("malformed", 0),
        skipped_too_large=counters.get("too_large", 0),
        skipped_duplicate=counters.get("duplicate", 0),
        unspent_source=budget - leg.give_total,
    )


def _empty_plan(
    source_asset: str, target_asset: str, budget: int, counters: dict[str, int]
) -> "ConsolidationPlan":
    """A plan that does nothing, carrying EVERY diagnostic counter.

    Factored out because the three early returns each dropped a different
    subset by hand, and the diagnostics are how an operator tells "nothing
    was cheap enough" from "nothing was small enough".
    """
    return ConsolidationPlan(
        source_asset=source_asset,
        target_asset=target_asset,
        legs=[],
        skipped_worse_than_cap=counters.get("worse_than_cap", 0),
        skipped_malformed=counters.get("malformed", 0),
        skipped_too_large=counters.get("too_large", 0),
        skipped_duplicate=counters.get("duplicate", 0),
        unspent_source=budget,
    )


def build_plan(
    *,
    source_asset: str,
    target_asset: str,
    budget: int,
    max_slippage_frac: float,
    direct_offers: Sequence[OfferCandidate],
    direct_anchor: Optional[Anchor],
    hop_asset: Optional[str] = None,
    first_hop_offers: Sequence[OfferCandidate] = (),
    first_hop_anchor: Optional[Anchor] = None,
    second_hop_offers: Sequence[OfferCandidate] = (),
    second_hop_anchor: Optional[Anchor] = None,
) -> ConsolidationPlan:
    """Build a one- or two-hop consolidation plan.

    A direct route is preferred whenever one exists and produces any fill:
    each hop is a separate all-or-nothing take with its own fee, its own
    slippage, and its own window in which the book can move, so two hops are
    strictly more dangerous than one and are only worth it when there is no
    direct book at all.

    The two-hop path exists because not every holding has a direct pair --
    with XCH as the target every asset does, but consolidating into DBX
    would leave BYC and wUSDC.b needing a hop through XCH.

    Raises
    ------
    PlanError
        If the request is incoherent (same source and target, non-positive
        budget, negative cap, or a hop requested without its anchors).  An
        empty plan -- nothing was cheap enough -- is NOT an error; it is a
        valid answer that the dialog reports as "no offers within your cap".
    """
    if source_asset == target_asset:
        raise PlanError("source and target are the same asset")
    if budget <= 0:
        raise PlanError(f"budget must be positive, got {budget}")
    # NaN fails EVERY comparison, so `< 0.0` waves it through -- and then
    # every `deviation > cap` test downstream is also false, silently
    # disabling the cap entirely and selecting arbitrarily bad offers.
    # Infinity removes the advertised finite bound the same way.  Both are
    # rejected here rather than defended against at each comparison.
    if max_slippage_frac != max_slippage_frac or max_slippage_frac in (
        float("inf"), float("-inf")
    ):
        raise PlanError(
            f"max slippage must be a finite number, got {max_slippage_frac!r}"
        )
    if max_slippage_frac < 0.0:
        raise PlanError(f"max slippage must be non-negative, got {max_slippage_frac}")

    counters: dict[str, int] = {}
    seen_ids: set[str] = set()

    direct_leg = None
    if direct_anchor is not None:
        direct_leg = _plan_leg(
            give_asset=source_asset,
            receive_asset=target_asset,
            budget=budget,
            offers=direct_offers,
            anchor=direct_anchor,
            max_slippage_frac=max_slippage_frac,
            counters=counters,
            seen_ids=seen_ids,
        )
        # [review round 2] This used to return the moment the direct route
        # selected ANY offer. A reproduced case took a single 1-unit direct
        # offer and left 999 of 1,000 source units untouched while a complete
        # two-hop route sat unused -- dust liquidity making the only viable
        # consolidation unreachable, in a tool whose whole promise is moving
        # as much as possible. Direct is still PREFERRED, because each extra
        # hop is another all-or-nothing take with its own fee and its own
        # window for the book to move; it is just no longer preferred at any
        # coverage whatsoever.
        if direct_leg.offers and hop_asset is None:
            return _single_leg_plan(
                source_asset, target_asset, direct_leg, budget, counters)

    if hop_asset is None and direct_leg is not None and direct_leg.offers:
        return _single_leg_plan(
            source_asset, target_asset, direct_leg, budget, counters)

    if hop_asset is None:
        # skipped_too_large was omitted here and below, so a direct route
        # that failed ONLY because every offer was bigger than the budget
        # reported zero oversized offers -- telling the operator "nothing
        # within your cap" when the truth was "nothing small enough".  Those
        # are different problems with different fixes.
        return _empty_plan(source_asset, target_asset, budget, counters)

    if first_hop_anchor is None or second_hop_anchor is None:
        raise PlanError("a two-hop plan needs an anchor for each hop")

    # Both hops share the operator's ROUTE cap, split so the composite
    # honours it -- see per_leg_cap.
    leg_cap = per_leg_cap(max_slippage_frac, 2)

    first = _plan_leg(
        give_asset=source_asset,
        receive_asset=hop_asset,
        budget=budget,
        offers=first_hop_offers,
        anchor=first_hop_anchor,
        max_slippage_frac=leg_cap,
        counters=counters,
        seen_ids=seen_ids,
    )
    if not first.offers:
        if direct_leg is not None and direct_leg.offers:
            return _single_leg_plan(
                source_asset, target_asset, direct_leg, budget, counters)
        return _empty_plan(source_asset, target_asset, budget, counters)

    # The second hop can only spend what the first actually yields.  Budget
    # it from first.receive_total rather than from any projection: if hop
    # one underfills, hop two must shrink with it or the plan promises a
    # quantity that will not exist when it runs.
    second = _plan_leg(
        give_asset=hop_asset,
        receive_asset=target_asset,
        budget=first.receive_total,
        offers=second_hop_offers,
        anchor=second_hop_anchor,
        max_slippage_frac=leg_cap,
        counters=counters,
        seen_ids=seen_ids,
    )

    # Direct wins ties and near-ties; the two-hop route has to be materially
    # better to justify a second all-or-nothing take. "Materially" is coverage
    # of the source budget -- but measured as source that actually REACHES THE
    # TARGET, not source spent on the first hop.
    #
    # [review round 3] Comparing against first.give_total was wrong and could
    # lose money: a first hop spending 1,000 units followed by a second hop
    # absorbing only 1 of them "beat" a 999-unit direct fill, delivered almost
    # no target, and stranded the rest as hop_residual in an asset the
    # operator never asked to hold. Only the pro-rata share of the first leg
    # whose output the second leg actually consumed counts as delivered.
    if second.offers and direct_leg is not None and direct_leg.offers:
        delivered_source = 0.0
        if first.receive_total > 0:
            delivered_source = (
                first.give_total * (second.give_total / first.receive_total)
            )
        if direct_leg.give_total >= delivered_source:
            return _single_leg_plan(
                source_asset, target_asset, direct_leg, budget, counters)

    if not second.offers:
        # A partial direct fill beats a two-hop route that cannot complete.
        if direct_leg is not None and direct_leg.offers:
            return _single_leg_plan(
                source_asset, target_asset, direct_leg, budget, counters)
        # The first hop would spend source to buy an intermediate asset the
        # second hop cannot sell -- so the plan receives NO target at all
        # while still costing the whole first leg.  is_empty was false in
        # this state (the first leg has offers), so it would have executed.
        # A route that cannot deliver the target is not a route.
        return _empty_plan(source_asset, target_asset, budget, counters)

    return ConsolidationPlan(
        source_asset=source_asset,
        target_asset=target_asset,
        legs=[first, second],
        skipped_worse_than_cap=counters.get("worse_than_cap", 0),
        skipped_malformed=counters.get("malformed", 0),
        skipped_too_large=counters.get("too_large", 0),
        skipped_duplicate=counters.get("duplicate", 0),
        unspent_source=budget - first.give_total,
        hop_residual=first.receive_total - second.give_total,
    )
