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
spread is cheaper than the exposure. ``alo`` is NOT offered: these legs carry no
limit price, so a post-only instruction has nothing to rest and the venue
can only reject it. send_close() refuses any tif but ``ioc`` rather than
advertising a patient close that cannot work.
"""

from __future__ import annotations

import logging
import math
from typing import Any, Optional

_log = logging.getLogger(__name__)

#: Fractions the UI offers. 1.0 is a full flatten.
CLOSE_FRACTIONS = (0.25, 0.50, 1.00)


class ClosePayloadError(RuntimeError):
    """The account payload was a shape we do not understand."""


def _raise_if_unreadable(unreadable: list) -> None:
    """One check both position shapes go through.

    [review] Dropping a row silently also drops the FACT that the account
    was incomplete: with every row unreadable the operator is told "no
    open positions", and with one unreadable that exposure vanishes from
    the confirmation plan. Neither is survivable on a control someone
    reaches for to get out of a position.

    Factored out because the dict branch recorded the list and then
    returned without consulting it -- the list branch had the check, the
    dict branch had the bug, and a shared helper is the only version of
    this that cannot drift apart again.
    """
    if unreadable:
        raise ClosePayloadError(
            "could not read: %s -- refusing to plan a close against a "
            "partial view of the account" % ", ".join(sorted(unreadable)))


def read_positions(client: Any, now_s: float) -> dict:
    """``{market: signed_contracts}`` from the venue, right now.

    Signed the same way ``runner._margin_state`` signs it, and for the same
    reason: a row whose side we cannot read is worse than useless, because
    an unsigned size makes a short look like a long and "reduce" then means
    "sell more". An unreadable row REFUSES THE WHOLE PLAN --
    ClosePayloadError, not a quiet omission. Dropping one reports less
    exposure than exists, on the control an operator uses to escape it.

    BOTH DOCUMENTED SHAPES. [review] This accepted only the LIST form, so a
    perfectly valid dict payload -- the signed form `_margin_state` handles
    and `test_positions_parse_from_a_dict_or_a_list` pins -- returned an
    empty mapping. For an emergency control that is the worst failure
    available: it tells the operator "Nothing to close" while the position
    is fully open, at the one moment they are trying to get out.

    An unreadable TOP-LEVEL shape raises rather than reading as empty, for
    the same reason. "I could not understand the account" and "you have no
    positions" must never look alike here.
    """
    # [review] NOT `or {}`. A JSON null coerced to an empty object made an
    # unreadable venue response report "no open positions" -- the same
    # conflation this function fails closed on everywhere else, arriving
    # through the one line that looked like harmless defensiveness.
    payload = client.account(now_s)
    if not isinstance(payload, dict):
        raise ClosePayloadError(
            "account payload was %s, not an object" % type(payload).__name__)
    # [review] A MISSING field is not an empty account. A partial payload
    # -- one that simply did not carry `positions` -- was reading as "the
    # venue reports no open positions", which is the same fail-open this
    # function closes everywhere else. Only an explicitly EMPTY list or
    # object means flat; absence means we could not read it.
    if "positions" not in payload:
        raise ClosePayloadError(
            "account payload carried no `positions` field; refusing to "
            "report a flat account we cannot actually see")
    rows = payload["positions"]
    out: dict = {}
    unreadable: list = []
    if rows == [] or rows == {}:
        return out

    # Dict form: {market: signed_size}. Already signed -- that is the whole
    # difference between the two shapes, and conflating them is what hid the
    # phantom-long bug for a session.
    if isinstance(rows, dict):
        for market, raw in rows.items():
            name = str(market or "").strip()
            if not name:
                # [review] The seventh variant of the same fail-open, and
                # the only survivor of the audit that added the rule: a row
                # may be skipped only when it carries NO EXPOSURE. A blank
                # key with a live size is a malformed but exposed account,
                # and dropping it turned that into "Nothing to close" on
                # the one control an operator uses to escape. The LIST
                # branch already records exactly this as unreadable.
                try:
                    blank = float(raw)
                except (TypeError, ValueError):
                    blank = float("nan")
                if blank == 0.0:
                    continue        # genuinely flat: nothing to misreport
                unreadable.append("(no market) %r" % (raw,))
                continue
            try:
                signed = float(raw)
            except (TypeError, ValueError):
                _log.warning(
                    "permuto: position for %s is %r, which is not a number "
                    "-- skipping it rather than guessing", name, raw)
                unreadable.append(name)
                continue
            if not math.isfinite(signed):
                # [review] NaN and infinity were dropped silently here
                # while the LIST branch raised on the same class of junk.
                # One shape failing closed and the other failing open is
                # worse than either rule applied consistently: the
                # operator cannot tell which they got.
                _log.warning(
                    "permuto: position for %s is %r, which is not finite",
                    name, raw)
                unreadable.append(name)
                continue
            if signed == 0.0:
                continue
            out[name] = signed
        # [review] And CHECK it. This branch recorded `unreadable` and then
        # returned without looking, so {"A": "lots", "B": -5} produced a
        # confirmation showing only B -- a partial view of the account on
        # an emergency close path, which is the fifth variant of the same
        # fail-open in this one function. Both shapes now go through the
        # single check below.
        _raise_if_unreadable(unreadable)
        return out

    if not isinstance(rows, list):
        raise ClosePayloadError(
            "account positions were %s, not a list or an object"
            % type(rows).__name__)

    # [review] THE WHOLE BRANCH, not one more variant of it.
    #
    # This is the sixth fail-open found in this function, and every one was
    # a `continue` that looked harmless in isolation. The rule that should
    # have been applied at the first: a row may be SKIPPED only when it
    # structurally carries no exposure -- a zero size, nothing to act on.
    # Anything we merely could not PARSE might be a live position, and
    # dropping it silently reports less exposure than exists on the one
    # control an operator uses to escape it.
    for index, row in enumerate(rows):
        if not isinstance(row, dict):
            unreadable.append("row %d (%s)" % (index, type(row).__name__))
            continue
        market = str(row.get("market") or "").strip()
        if not market:
            unreadable.append("row %d (no market)" % index)
            continue
        # [review] `or 0.0` made a MISSING or null size indistinguishable
        # from an explicit zero, so {"market": "A", "side": "sell"} -- a
        # truncated row that may well carry the whole position -- vanished
        # from the confirmation as "flat". An absent size is unreadable,
        # not empty; only a size the venue actually stated as zero is flat.
        raw_size = row.get("size")
        if raw_size is None:
            raw_size = row.get("position")
        if raw_size is None:
            unreadable.append("%s (no size field)" % market)
            continue
        try:
            size = abs(float(raw_size))
        except (TypeError, ValueError):
            unreadable.append("%s (size %r)" % (market, raw_size))
            continue
        if not math.isfinite(size):
            unreadable.append("%s (size %r)" % (market, raw_size))
            continue
        if size <= 0.0:
            continue        # genuinely flat: no exposure to misreport
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
            unreadable.append(market)
    _raise_if_unreadable(unreadable)
    return out


#: Words the venue uses to refuse, on either key.
_REFUSED = ("rejected", "failed", "error", "cancelled")

#: ...and to acknowledge. `action` is the one actually observed in
#: captured responses; the status words are accepted too so a differently
#: shaped envelope is not read as a refusal.
_ACCEPTED = ("placed", "modified", "filled", "unchanged", "ok", "success",
             "accepted", "partially_filled")


def order_was_accepted(resp: Any) -> tuple:
    """``(accepted, detail)`` for one single-order response.

    [review] HTTP 200 is not acceptance. The client raises only for HTTP
    errors, and this venue's vocabulary includes application-level refusal
    -- a body carrying ``status: "rejected"`` or a rejection_reason comes
    back 200. Counting those as sent let the UI report a successful close
    the venue had refused, which on an emergency control is worse than
    reporting the failure.
    """
    if not isinstance(resp, dict) or not resp:
        # [review] NO BODY IS NOT AN ACKNOWLEDGEMENT. This used to return
        # True here -- "nothing to contradict, trust the 200" -- but the
        # transport manufactures {} for an empty 200 (client._decode), and
        # a null, a list or a bare number all arrive here as-is. Zero bytes
        # of venue evidence was being counted into `sent` and shown to the
        # operator as "N leg(s) sent".
        return False, ("no order acknowledgement in the venue response "
                       "(%s)" % type(resp).__name__)
    reason = str(resp.get("rejection_reason") or "").strip()
    if reason:
        return False, reason
    status = str(resp.get("status", "")).strip().lower()
    action = str(resp.get("action", "")).strip().lower()
    if status in _REFUSED or action in _REFUSED:
        return False, status or action
    # [review] `action` was never read at all, and it is the vocabulary the
    # venue actually speaks: every acknowledgement this repo has captured
    # is shaped {"action": "placed", "fills": [...], "order_id": N} with
    # "rejection_reason" on refusal. A body saying {"action": "rejected"}
    # and nothing else was therefore reported as a successful close.
    #
    # An id or a fill is acceptance on its own, so an order that fills
    # without a recognised status word is not misreported as refused.
    if (resp.get("order_id") or resp.get("id") or resp.get("fills")
            or action in _ACCEPTED or status in _ACCEPTED):
        return True, ""
    # Deliberately NOT the runner's rule. runner._legs_all_accepted trusts
    # an unknown envelope on purpose, because a false refusal there trips
    # the batch breaker and drains the book. Here the asymmetry runs the
    # other way: a false refusal costs one re-press, and the re-press
    # re-reads the venue and clamps every leg reduce-only against it, so a
    # second attempt on an already-closed position skips as "already
    # flat". Fail-closed is self-correcting; fail-open ends with an
    # operator walking away from a position they think is gone.
    return False, "unrecognised order response: %.200r" % (resp,)


def filled_size(resp) -> float:
    """How much of a leg the venue says actually filled, or -1.0.

    [review] An IOC that fills 40 of 100 is not a completed close, but
    "partially_filled" was accepted as a plain success and no quantity
    was ever compared -- so a 40/100 fill produced the same result and
    the same UI text as a full one. The generic "IOC can part-fill"
    warning is not a substitute for the number: it tells the operator
    the outcome is possible, not that it happened.

    Returns -1.0 when the venue did not say, which is different from 0.0
    (it said nothing filled) and must not be rendered as a quantity.
    """
    if not isinstance(resp, dict):
        return -1.0
    direct = resp.get("filled_size", resp.get("filled"))
    if direct is not None:
        try:
            value = abs(float(direct))
        except (TypeError, ValueError):
            return -1.0
        return value if math.isfinite(value) else -1.0
    fills = resp.get("fills")
    if not isinstance(fills, list):
        return -1.0
    total = 0.0
    for fill in fills:
        if not isinstance(fill, dict):
            return -1.0
        try:
            total += abs(float(fill.get("size", fill.get("qty"))))
        except (TypeError, ValueError):
            return -1.0
    return total if math.isfinite(total) else -1.0


class _Legs(list):
    """The planned legs, plus what rounded away building them.

    A list SUBCLASS rather than a tuple or a dict: every caller already
    treats the return value as the legs themselves -- they index it, pass
    it to describe(), hand it to send_close() and JSON it into the worker
    -- and changing that shape to carry one extra field would touch all of
    them for no gain. The attribute is additive and invisible to code that
    does not ask for it.
    """

    def __new__(cls, legs, rounded_out=()):
        self = super().__new__(cls, legs)
        return self

    def __init__(self, legs, rounded_out=()):
        super().__init__(legs)
        self.rounded_out = list(rounded_out)

    # [review] Equality is the LEGS, stated explicitly rather than
    # inherited by accident. rounded_out is diagnostic -- it records
    # what the plan could not include, not what it will do -- so two
    # plans sending the same orders are the same plan.
    #
    # The alternative the linter suggests, comparing rounded_out when
    # both sides are _Legs and delegating to list otherwise, buys
    # nothing and costs transitivity: _Legs(x, a) != _Legs(x, b)
    # while both still equal list(x).
    def __eq__(self, other):
        return list.__eq__(self, other)

    def __ne__(self, other):
        result = self.__eq__(other)
        return result if result is NotImplemented else not result

    #: Mutable and equality-defining, so unhashable -- the data model's
    #: own rule, and list is unhashable for the same reason.
    __hash__ = None


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
    #: Markets whose share rounds below one lot. Attached to the returned
    #: list so callers can surface them without changing the leg shape.
    rounded_out: list = []
    for market, signed in sorted(positions.items()):
        if not signed:
            continue
        lot = float(lots.get(market, 1.0) or 1.0)
        size = abs(signed) * fraction
        if lot > 0.0:
            size = int(size / lot) * lot
        if size <= 0.0:
            # [review] An unannotated `continue` here removed a live market
            # from the confirmation entirely -- {A: 100, B: 1} at 0.25 lists
            # only A, and the operator is never told B is still open. Worse,
            # if EVERY market rounds out the plan is empty and describe()
            # then reports "the venue reports no open positions", which is
            # the opposite of the truth. Carry the casualties out with the
            # legs so the dialog can say so.
            rounded_out.append("%s (%.4g below one lot of %.4g)"
                               % (market, abs(signed) * fraction, lot))
            continue
        legs.append({
            "market": market,
            # Short -> buy to close. Long -> sell to close.
            "side": "buy" if signed < 0 else "sell",
            "size": size,
            "reduce_only": True,
        })
    return _Legs(legs, rounded_out)


def describe(legs: list, prices: Optional[dict] = None) -> str:
    """Human-readable summary for the confirmation dialog.

    The operator must see contracts AND notional before committing: a
    contract count alone is meaningless when one market trades at 0.15 and
    another at 0.46.
    """
    rounded = list(getattr(legs, "rounded_out", ()) or ())
    if not legs:
        # [review] "No open positions" is a claim about the ACCOUNT.
        # When every market merely rounded below one lot the account
        # is NOT flat, and saying so on the control an operator uses
        # to escape is the worst available wording.
        if rounded:
            return ("Nothing can be closed at this size -- every "
                    "position rounds below one lot: "
                    + "; ".join(rounded)
                    + ". The exposure is still open; try a larger "
                      "fraction.")
        return "Nothing to close -- the venue reports no open positions."
    px = prices or {}
    lines, total = [], 0.0
    unpriced = 0
    for leg in legs:
        key = leg["market"].replace("-PERP", "")
        # [review] The notional is a NICETY; the plan is the point. A junk
        # public oracle value -- "unavailable", NaN, infinity -- used to
        # raise straight out of here and stop the operator ever reaching
        # the confirmation dialog. That is the opposite failure to the
        # account parsing above and needs the opposite rule: the ACCOUNT
        # must fail closed because it is the truth about exposure, while
        # the ORACLE is display data and must degrade quietly.
        try:
            price = float(px.get(key, px.get(leg["market"], 0.0)) or 0.0)
        except (TypeError, ValueError):
            price = 0.0
        if not math.isfinite(price) or price < 0.0:
            price = 0.0
        if price <= 0.0:
            # [review] A leg we could not price must not silently
            # shrink the total. Two legs with one missing price
            # produced a plausible number that UNDERSTATED what the
            # operator was approving -- on a confirmation dialog,
            # which exists so the number can be checked first.
            unpriced += 1
        notional = leg["size"] * price
        total += notional
        lines.append("  %-15s %-4s %12.0f contracts%s"
                     % (leg["market"], leg["side"].upper(), leg["size"],
                        ("  ~$%,.0f".replace(",", "") % notional)
                        if price > 0 else ""))
    if total > 0.0:
        lines.append("  %-15s %-4s %12s   ~$%.0f%s"
                     % ("", "", "", total,
                        " total" if not unpriced else
                        " PARTIAL total -- %d leg(s) unpriced"
                        % unpriced))
    elif unpriced:
        lines.append("  (no notional available -- %d leg(s) "
                     "unpriced)" % unpriced)
    if rounded:
        lines.append("  NOT included -- below one lot at this size:")
        for entry in rounded:
            lines.append("    %s" % entry)
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
    # [review] IOC ONLY. The advertised tif="alo" built exactly the same
    # payload as IOC -- with no limit price in it. ALO is post-only limit,
    # so the venue can only reject a price-less one, and the option read
    # as a supported "patient close" that silently could not work.
    #
    # Not fixed by adding a price: that would need the limit chosen, band
    # checked against a fresh oracle, and SHOWN in the confirmation before
    # approval. That is a feature, and the wrong one for a control whose
    # purpose is crossing the spread to get out now.
    tif = str(tif or "").strip().lower()
    if tif != "ioc":
        raise ValueError(
            "close legs carry no limit price, so only tif='ioc' is valid "
            "here; %r is a resting/post-only variant the venue would "
            "reject" % (tif,))

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
        # Say WHICH markets fell away and why. "No legs remain" alone tells
        # an operator nothing about whether their exposure is gone or their
        # close silently did nothing.
        detail = ("no legs remain after clamping to fresh positions: %s"
                  % "; ".join(skipped)) if skipped else (
                      "no legs remain after clamping to fresh positions")
        return {"ok": True, "sent": 0, "note": detail,
                "legs": approved_legs, "skipped": skipped}

    _log.warning(
        "permuto: OPERATOR CLOSE -- %d leg(s), tif=%s:\n%s",
        len(to_send), tif, describe(to_send))

    responses: list = []
    failed: list = []
    partial: list = []
    #: Legs whose outcome the venue never told us. Distinct from
    #: `failed`, which is the venue saying no.
    unknown: list = []
    for leg in to_send:
        try:
            resp = client.place_order(leg, now_s)
            responses.append(resp)
            # [review] HTTP 200 is not acceptance. The client raises only
            # for HTTP errors, and this venue refuses at the application
            # level with a 200 body -- so counting every non-raising call
            # as sent let the UI report a close the venue had declined.
            ok, detail = order_was_accepted(resp)
            if not ok:
                _log.error("permuto: operator close leg %s refused: %s",
                           leg["market"], detail)
                failed.append("%s: %s" % (leg["market"], detail))
            else:
                got = filled_size(resp)
                if 0.0 <= got < leg["size"] - 1e-9:
                    partial.append("%s filled %.4g of %.4g"
                                   % (leg["market"], got,
                                      leg["size"]))
        except Exception as exc:  # noqa: BLE001 - shown, not raised
            # [review] A TRANSPORT ERROR IS NOT A REFUSAL. _request()
            # can raise after urlopen() has already succeeded -- while
            # reading the response -- so the venue may well have taken
            # this order. Counting it with the refusals told the
            # operator "Failed", which reads as "nothing happened", and
            # the natural next move is to press Close again and double
            # the size. Unknown is its own answer and the only honest
            # one; the remedy is to go and look.
            _log.error("permuto: operator close leg %s did not return a verdict: %s", leg["market"], exc)
            unknown.append("%s: %s" % (leg["market"], exc))

    # Neither refused nor unresolved: only these are known to have gone
    # out and been acknowledged.
    sent = len(to_send) - len(failed) - len(unknown)
    # [review] Skipped markets belong in the OPERATOR's note, not only in a
    # log they are not reading. "2 leg(s) sent" while an approved exposure
    # was quietly dropped -- because it flipped, or went flat, or rounded
    # to nothing -- is precisely the report that gets someone to walk away
    # from a position they believe they closed.
    skipped_note = ("skipped %s" % "; ".join(skipped)) if skipped else ""
    # [review] A part-fill is not a completed close, and the operator
    # cannot see it anywhere else -- the leg was accepted, so it is
    # counted in `sent`. Name the quantities.
    if partial:
        part_note = "PARTIAL -- %s" % "; ".join(partial)
        skipped_note = ("%s (%s)" % (part_note, skipped_note)
                        if skipped_note else part_note)
    # [review] UNKNOWN OUTCOMES LEAD. A leg the venue refused leaves the
    # position untouched; a leg whose answer never arrived may have
    # executed, and the two must not read alike -- the wrong reading
    # invites a second press that doubles the close.
    if unknown:
        note = ("UNRESOLVED -- no verdict for %s. These may have "
                "EXECUTED. Check the position on the venue before "
                "closing again." % "; ".join(unknown))
        if failed:
            note = "%s Refused: %s." % (note, "; ".join(failed))
        if skipped_note:
            note = "%s (%s)" % (note, skipped_note)
        return {"ok": False, "sent": sent, "note": note,
                "legs": to_send, "responses": responses,
                "skipped": skipped, "partial": partial,
                "unknown": unknown}
    if failed:
        note = "partial: %s" % "; ".join(failed)
        if skipped_note:
            note = "%s (%s)" % (note, skipped_note)
        return {"ok": False, "sent": sent, "note": note,
                "legs": to_send, "responses": responses,
                "skipped": skipped, "partial": partial,
                "unknown": unknown}
    return {"ok": True, "sent": sent, "note": skipped_note,
            "legs": to_send, "responses": responses, "skipped": skipped,
            "partial": partial, "unknown": unknown}


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
