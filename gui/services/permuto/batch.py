"""Building a `batch_upsert` payload the venue will actually accept.

`batch_upsert` is the endpoint that matters for a market maker, and it is
shaped by one constraint that is easy to read past:

    up to 12 GTC/ALO quotes, one mutate token.
    **Modify-or-place per (market, side), at most one leg each.**

So it is NOT a ladder endpoint. It maintains exactly one quote per side per
market — three markets is six legs — and refreshes them atomically. A
multi-level ladder needs `batch_place`, which is insert-only and cannot
restore a lifted side in one call.

That constraint is a good fit rather than a limitation, because of how depth
credit works. Credit is ``min(bid, ask)`` inside the ±2% ring, so a lifted side
earns ZERO for that market until restored — and the documented restore path is
"one call, not a cancel/place pair". `batch_upsert` is that one call. Size at a
single good price inside the ring, replaced atomically after every fill, is
exactly what the scoring function pays for; extra levels mostly add notional
that the minimum never counts.

Everything here is pure. Validation happens before a token is spent, because a
rejected batch costs a rate-limit token AND leaves the previous quotes resting
at prices we have already decided are wrong.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Sequence

from .orders import OrderIntent, Placement, Side, classify_placement

__all__ = [
    "BatchError",
    "MAX_LEGS",
    "build_upsert_batch",
]

#: Hard venue limit. A 13th leg is a 400 for the whole batch, not a truncation.
MAX_LEGS = 12


class BatchError(ValueError):
    """The batch is invalid and must not be sent.

    Raised rather than returned, because every case here means the caller's
    model of its own book is wrong -- and sending anyway spends a mutate token
    to be told the same thing by the venue, while the stale quotes it meant to
    replace keep resting.
    """


@dataclass(frozen=True)
class _Leg:
    market: str
    side: str
    price: float
    size: float
    tif: str
    reduce_only: bool


def build_upsert_batch(
    intents: Sequence[OrderIntent],
    oracles: dict[str, float],
    *,
    tif: str = "ALO",
    band_pct: float = 5.0,
    ring_pct: float = 2.0,
) -> list[dict]:
    """Validate and serialise legs for ``POST /exchange/batch_upsert``.

    ``oracles`` is keyed by the same market SYMBOL the intents carry, and must
    be the venue's current values. A stale local oracle produces legs that
    validate perfectly here and are rejected or purged there, with nothing
    locally to show for it.

    ALO by default: an add-liquidity-only quote either rests as a maker or is
    refused, and a maker rest is the entire point -- a quote that crosses and
    takes has paid the spread instead of earning it, and taken on inventory we
    then have to manage.
    """
    if not intents:
        return []
    if len(intents) > MAX_LEGS:
        raise BatchError(
            "batch_upsert accepts at most %d legs, got %d; the venue rejects "
            "the WHOLE batch rather than truncating"
            % (MAX_LEGS, len(intents))
        )

    seen: set[tuple[str, str]] = set()
    out: list[dict] = []

    for leg in intents:
        key = (leg.market, leg.side.value)
        if key in seen:
            # Upsert is keyed on (market, side); two legs for one key is not
            # a ladder, it is one silently overwriting the other -- and which
            # one survives is the venue's business, not ours.
            raise BatchError(
                "two legs for %s %s: batch_upsert holds at most one quote per "
                "(market, side). Use batch_place for a multi-level ladder."
                % (leg.market, leg.side.value)
            )
        seen.add(key)

        if not leg.market.endswith("-PERP"):
            # /info/meta carries symbol AND oracle_ticker and they differ by
            # suffix. Order routes want the symbol; the ticker is a 400 on
            # every leg.
            raise BatchError(
                "market %r is not a tradeable symbol (expected the -PERP "
                "form, e.g. QQQ-VOL-PERP, not the oracle ticker)" % leg.market
            )

        oracle = oracles.get(leg.market)
        if oracle is None or not (oracle > 0.0):
            raise BatchError(
                "no fresh oracle for %s; refusing to price against a missing "
                "or stale reference" % leg.market
            )

        placement = classify_placement(
            leg.side, leg.price, oracle, band_pct=band_pct, ring_pct=ring_pct
        )
        if placement is Placement.ILLEGAL:
            raise BatchError(
                "%s %s @ %.6f is outside the +/-%.1f%% legal band around "
                "%.6f -- the venue answers 400 and there is no order"
                % (leg.market, leg.side.value, leg.price, band_pct, oracle)
            )
        if placement is Placement.PURGE_RISK:
            raise BatchError(
                "%s %s @ %.6f is aggressive OUTSIDE the +/-%.1f%% ring around "
                "%.6f. The venue refuses it at place, and any such order that "
                "does rest is PURGED on the next oracle move -- the book "
                "empties with nothing local to show for it"
                % (leg.market, leg.side.value, leg.price, ring_pct, oracle)
            )

        if not (leg.size > 0.0) or leg.size != leg.size:
            raise BatchError("%s %s has non-positive or non-finite size"
                             % (leg.market, leg.side.value))

        out.append({
            "market": leg.market,
            "side": leg.side.value,
            "price": leg.price,
            "size": leg.size,
            "tif": tif,
            "reduce_only": leg.reduce_only,
        })

    return out
