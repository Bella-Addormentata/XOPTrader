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


def send_close(client: Any, now_s: float, approved_legs: list, *,
               tif: str = "ioc") -> dict:
    """Send ``approved_legs`` via :meth:`~PermutoClient.place_order`, clamped
    against a fresh venue read so the actual order never exceeds what the
    operator approved.

    Each leg is sent as a separate ``/exchange/order`` with ``reduce_only``
    and the requested ``tif``.  ``batch_upsert`` is for GTC/ALO resting
    quotes; IOC and market orders must use the single-order route.

    Clamping rules (per market):
    * If the fresh venue position has the opposite sign from the approved
      leg, the position already flipped -- skip it rather than send a
      new-direction order.
    * If the fresh venue position is smaller than the approved size, use
      the fresh size (never send more than what the venue says is there).
    * If the fresh venue position is zero, skip (already flat).

    Never raises for a venue-side refusal: a refusal is information the
    operator needs on screen, not a traceback in a log they are not
    reading.
    """
    if not approved_legs:
        return {"ok": True, "sent": 0, "note": "no legs to send", "legs": []}

    fresh = read_positions(client, now_s)

    to_send: list = []
    skipped: list = []
    for leg in approved_legs:
        market = leg["market"]
        approved_side = leg["side"]
        approved_size = float(leg["size"])

        fresh_signed = fresh.get(market, 0.0)
        if fresh_signed == 0.0:
            skipped.append(market + "(already flat)")
            continue
        # Check the approved side still matches the fresh sign.
        # A short (negative) needs a BUY; a long (positive) needs a SELL.
        expected_side = "buy" if fresh_signed < 0 else "sell"
        if approved_side != expected_side:
            # The position flipped between plan and send. Sending the
            # original direction would OPEN a new position. Skip it.
            skipped.append(market + "(position flipped)")
            continue
        size = min(approved_size, abs(fresh_signed))
        if size <= 0.0:
            skipped.append(market + "(zero after clamp)")
            continue
        to_send.append({
            "market": market,
            "side": approved_side,
            "size": size,
            "tif": tif,
            "reduce_only": True,
        })

    if skipped:
        _log.warning("permuto: operator close -- skipped legs: %s",
                     ", ".join(skipped))
    if not to_send:
        return {"ok": True, "sent": 0,
                "note": "no legs remain after clamping to fresh positions",
                "legs": approved_legs}

    _log.warning(
        "permuto: OPERATOR CLOSE -- %d leg(s), tif=%s:\n%s",
        len(to_send), tif, describe(to_send))

    responses: list = []
    failed: list = []
    for leg in to_send:
        try:
            resp = client.place_order(leg, now_s)
            responses.append(resp)
        except Exception as exc:  # noqa: BLE001 - shown, not raised
            _log.error("permuto: operator close leg %s failed: %s",
                       leg["market"], exc)
            failed.append("%s: %s" % (leg["market"], exc))

    sent = len(to_send) - len(failed)
    if failed:
        return {"ok": False, "sent": sent,
                "note": "partial: %s" % "; ".join(failed),
                "legs": to_send, "responses": responses}
    return {"ok": True, "sent": sent, "note": "",
            "legs": to_send, "responses": responses}


def close_positions(client: Any, now_s: float, fraction: float, *,
                    tif: str = "ioc", lot_sizes: Optional[dict] = None,
                    prices: Optional[dict] = None) -> dict:
    """Read, plan, send. Returns a result dict for the UI to display.

    .. deprecated::
        Prefer :func:`plan_close` (to build and show the plan) followed by
        :func:`send_close` (to execute it against a fresh position read),
        so the operator approves the concrete plan before anything is sent.
        This combined form is retained for backward compatibility only.
    """
    positions = read_positions(client, now_s)
    if not positions:
        return {"ok": True, "sent": 0, "note": "no open positions to close",
                "legs": []}

    legs = plan_close(positions, fraction, lot_sizes)
    if not legs:
        return {"ok": True, "sent": 0,
                "note": "every close rounded below one lot", "legs": []}

    return send_close(client, now_s, legs, tif=tif)
