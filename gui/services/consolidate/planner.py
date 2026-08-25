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
    "ConsolidationPlan",
    "Leg",
    "OfferCandidate",
    "PlanError",
    "build_plan",
    "effective_rate",
    "rate_deviation_frac",
]


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
        if not self.source:
            raise PlanError("anchor must carry its provenance")


@dataclass(frozen=True)
class OfferCandidate:
    """One dexie offer, reduced to what the planner needs.

    ``give_amount`` / ``receive_amount`` are in the raw integer units of
    their respective assets (mojos for XCH, CAT mojos otherwise), exactly as
    the wallet reports them.  The planner never converts units; it only ever
    forms ratios, so unit scaling cancels as long as a single offer's two
    amounts are each in their own asset's units.
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
        if self.receive_total <= 0:
            return float("inf")
        return self.give_total / self.receive_total


@dataclass
class ConsolidationPlan:
    """What the button will do, in enough detail to be shown before it runs."""

    source_asset: str
    target_asset: str
    legs: list[Leg] = field(default_factory=list)
    skipped_worse_than_cap: int = 0
    skipped_malformed: int = 0
    skipped_too_large: int = 0
    unspent_source: int = 0

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
    return offer.give_amount / offer.receive_amount


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
        if _usable(offer, give_asset, receive_asset):
            usable.append(offer)
        else:
            counters["malformed"] = counters.get("malformed", 0) + 1

    usable.sort(key=effective_rate)

    chosen: list[OfferCandidate] = []
    give_total = 0
    receive_total = 0
    remaining = budget

    for index, offer in enumerate(usable):
        rate = effective_rate(offer)
        if rate_deviation_frac(rate, anchor) > max_slippage_frac:
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
    if max_slippage_frac < 0.0:
        raise PlanError(f"max slippage must be non-negative, got {max_slippage_frac}")

    counters: dict[str, int] = {}

    if direct_anchor is not None:
        leg = _plan_leg(
            give_asset=source_asset,
            receive_asset=target_asset,
            budget=budget,
            offers=direct_offers,
            anchor=direct_anchor,
            max_slippage_frac=max_slippage_frac,
            counters=counters,
        )
        if leg.offers:
            return ConsolidationPlan(
                source_asset=source_asset,
                target_asset=target_asset,
                legs=[leg],
                skipped_worse_than_cap=counters.get("worse_than_cap", 0),
                skipped_malformed=counters.get("malformed", 0),
                skipped_too_large=counters.get("too_large", 0),
                unspent_source=budget - leg.give_total,
            )

    if hop_asset is None:
        return ConsolidationPlan(
            source_asset=source_asset,
            target_asset=target_asset,
            legs=[],
            skipped_worse_than_cap=counters.get("worse_than_cap", 0),
            skipped_malformed=counters.get("malformed", 0),
            unspent_source=budget,
        )

    if first_hop_anchor is None or second_hop_anchor is None:
        raise PlanError("a two-hop plan needs an anchor for each hop")

    first = _plan_leg(
        give_asset=source_asset,
        receive_asset=hop_asset,
        budget=budget,
        offers=first_hop_offers,
        anchor=first_hop_anchor,
        max_slippage_frac=max_slippage_frac,
        counters=counters,
    )
    if not first.offers:
        return ConsolidationPlan(
            source_asset=source_asset,
            target_asset=target_asset,
            legs=[],
            skipped_worse_than_cap=counters.get("worse_than_cap", 0),
            skipped_malformed=counters.get("malformed", 0),
            unspent_source=budget,
        )

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
        max_slippage_frac=max_slippage_frac,
        counters=counters,
    )

    return ConsolidationPlan(
        source_asset=source_asset,
        target_asset=target_asset,
        legs=[first, second],
        skipped_worse_than_cap=counters.get("worse_than_cap", 0),
        skipped_malformed=counters.get("malformed", 0),
        skipped_too_large=counters.get("too_large", 0),
        unspent_source=budget - first.give_total,
    )
