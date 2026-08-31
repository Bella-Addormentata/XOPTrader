"""Operator-driven position close. One deliberate action, never a loop.

WHY THIS EXISTS. The quoting loop can only shed inventory as a MAKER, and
`risk.py` deliberately reserves crossing the spread to close as an operator
decision. Until now there was no way to make that decision: the Permuto page
offered Create/Restore/Register/Check/Recover/Discard/Start polling/Start
quoting and nothing else, so an operator who wanted out had no control that
could do it. A doctrine that reserves a decision for a human, in software
that gives the human no button, is not a safeguard -- it is a dead end.

WHAT IT WILL NOT DO, BY CONSTRUCTION.

* Every leg is ``reduce_only``. The venue will refuse anything that would
  grow or flip a position, so the worst case of a bug here is that we close
  less than intended -- never that we open something new.
* Size is clamped to the position actually reported by the venue, read
  fresh in the same call. It cannot act on a stale belief about what we
  hold.
* It runs once per press. There is no retry, no schedule and no state; if
  it half-fills, the operator sees the result and decides again.

TIME IN FORCE. ``ioc`` crosses the spread and is the point: a maker-side
close is what the loop already attempts and what leaves inventory sitting
through a reopen gap. The operator pressing this button has decided the
spread is cheaper than the exposure. ``alo`` is offered for a patient close
when there is time.
"""

from __future__ import annotations

import logging
from typing import Any, Optional

_log = logging.getLogger(__name__)

#: Fractions the UI offers. 1.0 is a full flatten.
CLOSE_FRACTIONS = (0.25, 0.50, 1.00)


def read_positions(client: Any, now_s: float) -> dict:
    """``{market: signed_contracts}`` from the venue, right now.

    Signed the same way ``runner._margin_state`` signs it, and for the same
    reason: a row whose side we cannot read is worse than useless, because
    an unsigned size makes a short look like a long and "reduce" then means
    "sell more". Unreadable rows are dropped rather than guessed.
    """
    payload = client.account(now_s) or {}
    rows = payload.get("positions")
    out: dict = {}
    if not isinstance(rows, list):
        return out
    for row in rows:
        if not isinstance(row, dict):
            continue
        market = str(row.get("market") or "").strip()
        if not market:
            continue
        try:
            size = abs(float(row.get("size", row.get("position", 0.0)) or 0.0))
        except (TypeError, ValueError):
            continue
        if size <= 0.0:
            continue
        side = str(row.get("side", "")).strip().lower()
        if side in ("sell", "short", "s", "ask"):
            out[market] = -size
        elif side in ("buy", "long", "b", "bid"):
            out[market] = size
        else:
            _log.warning(
                "permuto: position row for %s has side %r, which we cannot "
                "read -- skipping it rather than guessing its direction",
                market, row.get("side"))
    return out


def plan_close(positions: dict, fraction: float,
               lot_sizes: Optional[dict] = None) -> list:
    """Legs that would close ``fraction`` of each position.

    Returns ``[{market, side, size, reduce_only}]``. A short is closed by
    BUYING and a long by SELLING -- getting that backwards is the single
    most damaging bug available here, so the sign is derived from the
    position rather than passed in.
    """
    if not (0.0 < fraction <= 1.0):
        raise ValueError("fraction must be in (0, 1], got %r" % (fraction,))
    lots = lot_sizes or {}
    legs = []
    for market, signed in sorted(positions.items()):
        if not signed:
            continue
        lot = float(lots.get(market, 1.0) or 1.0)
        size = abs(signed) * fraction
        if lot > 0.0:
            size = int(size / lot) * lot
        if size <= 0.0:
            continue
        legs.append({
            "market": market,
            # Short -> buy to close. Long -> sell to close.
            "side": "buy" if signed < 0 else "sell",
            "size": size,
            "reduce_only": True,
        })
    return legs


def describe(legs: list, prices: Optional[dict] = None) -> str:
    """Human-readable summary for the confirmation dialog.

    The operator must see contracts AND notional before committing: a
    contract count alone is meaningless when one market trades at 0.15 and
    another at 0.46.
    """
    if not legs:
        return "Nothing to close -- the venue reports no open positions."
    px = prices or {}
    lines, total = [], 0.0
    for leg in legs:
        key = leg["market"].replace("-PERP", "")
        price = float(px.get(key, px.get(leg["market"], 0.0)) or 0.0)
        notional = leg["size"] * price
        total += notional
        lines.append("  %-15s %-4s %12.0f contracts%s"
                     % (leg["market"], leg["side"].upper(), leg["size"],
                        ("  ~$%,.0f".replace(",", "") % notional)
                        if price > 0 else ""))
    if total > 0.0:
        lines.append("  %-15s %-4s %12s   ~$%.0f total"
                     % ("", "", "", total))
    return "\n".join(lines)


def close_positions(client: Any, now_s: float, fraction: float, *,
                    tif: str = "ioc", lot_sizes: Optional[dict] = None,
                    prices: Optional[dict] = None) -> dict:
    """Read, plan, send. Returns a result dict for the UI to display.

    Never raises for a venue-side refusal: a refusal is information the
    operator needs on screen, not a traceback in a log they are not
    reading.
    """
    positions = read_positions(client, now_s)
    if not positions:
        return {"ok": True, "sent": 0, "note": "no open positions to close",
                "legs": []}

    legs = plan_close(positions, fraction, lot_sizes)
    if not legs:
        return {"ok": True, "sent": 0,
                "note": "every close rounded below one lot", "legs": []}

    payload = [{"market": leg["market"], "side": leg["side"],
                "size": leg["size"], "tif": tif, "reduce_only": True}
               for leg in legs]
    _log.warning(
        "permuto: OPERATOR CLOSE -- %d leg(s), fraction %.0f%%, tif=%s:\n%s",
        len(payload), fraction * 100.0, tif, describe(legs, prices))
    try:
        response = client.batch_upsert(payload, now_s)
    except Exception as exc:  # noqa: BLE001 - shown, not raised
        _log.error("permuto: operator close failed: %s", exc)
        return {"ok": False, "sent": 0, "note": str(exc), "legs": legs}
    return {"ok": True, "sent": len(payload), "note": "",
            "legs": legs, "response": response}
