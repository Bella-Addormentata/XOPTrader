"""The sequencer. Every decision here was made somewhere else.

`quoting.decide()` says whether to be in the market, `risk.assess()` says how
big and how skewed, `orders.quote_ladder()` builds the legs,
`batch.build_upsert_batch()` validates them and `client` sends them. This
module's whole job is to call those in the right order, once per tick, and to
keep the small amount of state that has to survive between ticks.

WHAT THE STATE IS FOR (C-11). The loop's belief about what is resting is not
decoration -- it is the input `quoting.decide()` uses to choose HOLD over
QUOTE, and HOLD means sending nothing. So a belief that outlives the orders it
describes is not a stale cache, it is a silent outage: the loop holds, sends
nothing, and the book stays empty while depth accrues to everyone else.

Three things invalidate the belief and all three are handled here rather than
trusted to memory:

- A **pause** withdraws the book. Resting orders do not survive it, so the
  belief is cleared when we withdraw, not when we next notice.
- An **un-pause** is worse, because it looks like nothing happened. The
  sequencer cancels every resting order and pending trigger at the open, so
  the tick after a reopen must rebuild rather than hold. `just_reopened` is
  computed here from the pause edge, because the venue does not announce it.
- Anything else -- a fill, a purge, an expiry -- is caught by reconciling
  against `open_orders()` rather than by prediction. Prediction is how a book
  that lost one side to a fill sits at HOLD earning nothing, since depth
  credit is `min(bid, ask)` and a one-sided book scores zero.

ERRORS DO NOT STOP THE LOOP. A tick that throws returns a failed TickResult
and the next tick runs. The contest is ~102 unattended hours; a loop that dies
on the first transient 500 has failed at the only thing it had to do.
"""

from __future__ import annotations

import logging
import math
from dataclasses import dataclass, field
from typing import Any, Optional

from .auth import PermutoAuthError
from .batch import BatchError, build_upsert_batch
from .client import PermutoNotLinked
from .orders import Side, quote_ladder
from .quoting import LoopAction, RestingQuote, VenueView, decide
from .risk import MarginState, RiskAction, assess, skewed_reference
from .session import RenewAction

#: How often a SUSTAINED full withdrawal re-asserts the cancel when we
#: already believe the book is empty. Covers a stale belief without turning
#: a fourteen-hour pause into ten thousand identical requests.
RECANCEL_INTERVAL_S = 60.0

#: How far ahead the venue-side scheduled cancel-all is pushed each tick.
#: 24 missed ticks of headroom -- generous against transients, small against
#: the hours an unnoticed crash would otherwise leave quotes resting.
DMS_EXTEND_S = 120.0

_log = logging.getLogger(__name__)

__all__ = ["QuoteRunner", "TickResult"]


@dataclass
class TickResult:
    """What one tick did. Returned rather than logged only, so the GUI can
    show the current state without re-deriving it."""

    action: str = "idle"
    reason: str = ""
    markets: dict = field(default_factory=dict)
    error: str = ""

    @property
    def ok(self) -> bool:
        return not self.error


class QuoteRunner:
    """One account's quoting loop. Owns the belief state; owns no policy."""

    def __init__(
        self,
        client: Any,
        markets: list,
        *,
        target_depth_usd: float = 1_200.0,
        max_position_usd: float = 1_200.0,
        ring_pct: float = 2.0,
        half_spread_pct: float = 0.25,
        quote_when_carried: bool = True,
    ) -> None:
        self._client = client
        self._markets = list(markets)
        self._target_depth_usd = target_depth_usd
        # [release review] USD, not contracts. The old default of 100
        # CONTRACTS was ~$15-32 of notional at live oracles (0.15-0.32)
        # against $1,200 quote legs -- so the first routine fill of ~1.5% of
        # one quote pinned the market REDUCE_ONLY, zeroed its min(bid, ask)
        # depth credit, and the gate margin is only ~1.78x at full health. A
        # dollar limit survives an oracle that moves 10-13% in seconds; a
        # contract limit does not.
        self._max_position_usd = max_position_usd
        self._ring_pct = ring_pct
        self._half_spread_pct = half_spread_pct
        self._quote_when_carried = quote_when_carried

        self._resting: dict = {m: RestingQuote() for m in self._markets}
        self._was_paused = False
        self._reopen_pending = False
        #: When the last full cancel_all went out, for the re-assert below.
        #: Negative infinity so the first withdraw always cancels.
        self._last_full_cancel_s = float("-inf")
        #: Whether the venue-side dead man's switch reported armed, for
        #: log-once state transitions rather than a message per tick.
        self._dms_ok: Optional[bool] = None

    # ------------------------------------------------------------------ #
    # Belief
    # ------------------------------------------------------------------ #
    def _forget_book(self) -> None:
        self._resting = {m: RestingQuote() for m in self._markets}

    def reconcile(self, open_orders: Any) -> None:
        """Replace the belief with what the venue says is actually resting.

        Overwrites rather than merges. A merge would preserve exactly the
        entries the venue no longer lists -- the filled, purged and expired
        ones -- which are precisely the beliefs that cause a silent HOLD on an
        empty book.
        """
        rows = []
        if isinstance(open_orders, dict):
            rows = open_orders.get("orders") or open_orders.get("open") or []
        elif isinstance(open_orders, list):
            rows = open_orders

        seen: dict = {m: {"bid": None, "ask": None} for m in self._markets}
        for row in rows:
            if not isinstance(row, dict):
                continue
            market = row.get("market") or row.get("symbol")
            if market not in seen:
                continue
            side = str(row.get("side", "")).upper()
            try:
                price = float(row.get("price"))
            except (TypeError, ValueError):
                continue
            # float("nan") converts happily and would be recorded as a
            # present side, making RestingQuote.two_sided true while every
            # later drift comparison against it is false -- the loop HOLDs on
            # a book it cannot actually evaluate.
            if not math.isfinite(price):
                continue
            if side in ("BUY", "BID", "B"):
                seen[market]["bid"] = price
            elif side in ("SELL", "ASK", "S"):
                seen[market]["ask"] = price

        self._resting = {
            m: RestingQuote(bid_price=v["bid"], ask_price=v["ask"])
            for m, v in seen.items()
        }

    # ------------------------------------------------------------------ #
    # One tick
    # ------------------------------------------------------------------ #
    def tick(self, now_s: float, oracles: dict, flags: dict) -> TickResult:
        """Run one pass. Never raises; failures come back in the result."""
        try:
            return self._tick(now_s, oracles or {}, flags or {})
        except PermutoNotLinked as exc:
            # Not transient and not recoverable by retrying: nothing is
            # resting to withdraw and nothing can be placed.
            return TickResult("blocked", str(exc), error=str(exc))
        except (PermutoAuthError, BatchError) as exc:
            _log.warning("permuto: tick failed: %s", exc)
            return TickResult("error", str(exc), error=str(exc))
        except Exception as exc:  # noqa: BLE001
            # The loop must outlive its own bugs for ~102 unattended hours.
            _log.exception("permuto: unexpected tick failure")
            return TickResult("error", repr(exc), error=repr(exc))

    def _tick(self, now_s: float, oracles: dict, flags: dict) -> TickResult:
        paused = bool(flags.get("trading_paused"))
        carried = bool(flags.get("carried") or flags.get("carried_session"))

        # The un-pause edge, computed here because the venue does not announce
        # it. Latched rather than consumed immediately: the tick that observes
        # the reopen may still fail on session or oracle, and the rebuild it
        # owes the book must not be lost with it.
        if paused and not self._was_paused:
            self._reopen_pending = False
        if self._was_paused and not paused:
            self._reopen_pending = True
        self._was_paused = paused

        session_ok, session_waiting = True, False
        # [discord 2026-08-27] RENEWED THROUGH A PAUSE, deliberately.
        #
        # This used to be `if not paused`, and the contest begins with
        # exactly the case that breaks: the sponsor resets balances on
        # Sunday evening during a trading pause that un-pauses at the
        # 09:30 ET open -- fourteen-odd hours in which the session would
        # have expired unrenewed. The first tick after the open would
        # then spend a full challenge/sign/auth round trip before it
        # could place anything, at the exact moment every entrant
        # reconnects at once, on a metric that only accrues while
        # quoting. If that reauth failed we would be backing off through
        # the open.
        #
        # Authentication is not a trading route, so it works while
        # trading is paused. The result changes nothing while paused --
        # decide() tests trading_paused first and withdraws regardless --
        # it just means we come out of the pause already holding a
        # usable session.
        #
        # [review] A raise here used to abort the tick BEFORE the
        # withdraw path, so the one branch written for a dead session --
        # decide()'s `if not view.session_ok: WITHDRAW "no usable trading
        # session"` -- was unreachable by exception. session_ok is only
        # ever set from a RETURNED action, and an auth failure returns
        # nothing. The book stayed resting for the whole outage while the
        # code that exists to retract it sat one frame up the stack.
        #
        # Caught and folded into session_ok instead: a session we could
        # not establish is a session we do not have, which is exactly
        # what decide() already knows how to answer.
        try:
            action = self._client.ensure_session(now_s)
            session_waiting = action is RenewAction.WAIT
            session_ok = action in (RenewAction.OK, RenewAction.RENEW)
        except PermutoNotLinked:
            raise
        except PermutoAuthError as exc:
            _log.warning("permuto: session unavailable this tick: %s", exc)
            session_ok, session_waiting = False, False

        # [review] Poll the venue BEFORE deciding, not after deciding to
        # quote. The first version reconciled only on the way to a batch, so
        # once _resting looked two-sided the loop answered HOLD and stopped
        # asking: a filled side was never discovered, the market earned zero
        # -- depth credit is min(bid, ask) -- and margin could cross the
        # reduce and flatten lines with the old quotes still live. This
        # module's own docstring says prediction is the failure mode; the
        # implementation predicted anyway.
        #
        # Two requests per tick is the price of not being wrong about the
        # book. Skipped while paused or session-less, where both would fail.
        # `account_seen` matters as much as `state`: without it, a tick that
        # never fetched an account is indistinguishable from one that fetched
        # an unreadable one, and the risk pass below would act on the
        # fully-utilised default.
        # [release review] EXTEND THE VENUE-SIDE DEAD MAN'S SWITCH before
        # anything else that needs a session. schedule_cancel is the one
        # retraction that survives a crash, reboot or power loss -- the
        # in-process cancels all need this process alive, and the contest is
        # ~102 unattended hours. Extended every session-holding tick to
        # now + 120s (24 missed ticks of headroom); a fresh arm costs one of
        # ten daily triggers, re-extending is free, so extend-never-rearm.
        # A failure here must not affect the tick -- the switch is a net
        # under the loop, not a gate in it.
        if session_ok:
            try:
                self._client.schedule_cancel(
                    now_s, int((now_s + DMS_EXTEND_S) * 1000.0))
                if self._dms_ok is not True:
                    _log.info("permuto: venue-side dead man's switch armed "
                              "(+%.0fs, extended every tick)", DMS_EXTEND_S)
                    self._dms_ok = True
            except AttributeError:
                pass    # a test fake without the method
            except Exception as exc:  # noqa: BLE001
                if self._dms_ok is not False:
                    _log.warning("permuto: could not arm/extend the venue "
                                 "dead man's switch: %s -- a crash would "
                                 "leave the book resting", exc)
                    self._dms_ok = False

        state = MarginState(carried=carried)
        account_seen = False
        if session_ok and not paused:
            # [review] Same reasoning as the session call above: these two
            # were outside any handler, so a failure on either aborted the
            # tick before the withdraw and left the book resting. A view of
            # the venue we could not obtain is not a reason to stop managing
            # what we already placed -- account_seen stays False, which the
            # risk pass below already treats as "do not act on a default".
            try:
                self.reconcile(self._client.open_orders(now_s))
                state = _margin_state(self._client.account(now_s), carried)
                account_seen = True
            except PermutoNotLinked:
                raise
            except (PermutoAuthError, BatchError) as exc:
                _log.warning("permuto: venue view unavailable: %s", exc)
                session_ok = False

        results: dict = {}
        any_quoted = False
        for market in self._markets:
            oracle = oracles.get(market)
            view = VenueView(
                trading_paused=paused,
                oracle=oracle,
                oracle_age_s=float(flags.get("oracle_age_s", 0.0)),
                session_ok=session_ok,
                session_waiting=session_waiting,
                carried=carried,
                just_reopened=self._reopen_pending,
            )
            call = decide(
                view, self._resting.get(market, RestingQuote()),
                ring_pct=self._ring_pct,
                quote_when_carried=self._quote_when_carried,
            )
            results[market] = (call.action.value, call.reason)
            if call.action is LoopAction.QUOTE:
                any_quoted = True

        want = LoopAction.WITHDRAW.value
        withdrawing = [m for m, (a, _) in results.items() if a == want]
        if withdrawing:
            # [review] Unconditionally, NOT only when nothing else wants to
            # quote. The first version guarded this on `not any_quoted`, so a
            # stale oracle on QQQ while NVDA merely needed a refresh skipped
            # the cancel entirely -- QQQ's unsafe orders stayed live and the
            # batch below touched only NVDA. A market that must leave has to
            # leave whatever its neighbours are doing.
            #
            # Scoped to the withdrawing markets rather than cancel_all, so one
            # bad oracle does not empty a book that is earning depth
            # elsewhere. Falls back to a full cancel when every market is
            # withdrawing, which is also the pause and dead-session case.
            reason = results[withdrawing[0]][1]
            if len(withdrawing) == len(self._markets):
                # [discord 2026-08-27] Do not re-cancel a book we have just
                # emptied. A pause is a SUSTAINED withdraw, and the Sunday
                # one runs from the evening to the 09:30 ET open -- at a 5s
                # tick that is ~10,000 authenticated cancel_all calls against
                # a venue that is paused, which is a good way to be
                # rate-limited at the open.
                #
                # Re-asserted rather than skipped outright: belief can be
                # stale (the venue cancels everything at carried->live), and
                # during a pause reconcile() does not run to correct it. So
                # an empty belief still re-sends every kRecancelS, which
                # self-corrects a drifted belief within a minute while
                # cutting the pause traffic by ~98%.
                believed_empty = all(
                    q.empty for q in self._resting.values())
                due = (now_s - self._last_full_cancel_s
                       >= RECANCEL_INTERVAL_S)
                if not believed_empty or due:
                    # Stamped BEFORE the attempt: a cancel that throws during
                    # a venue outage must not retry every 5s tick for the
                    # whole outage -- that is the auth-route hammer again,
                    # through the cancel door. The next attempt waits out the
                    # re-assert interval like any other.
                    self._last_full_cancel_s = now_s
                    self._client.cancel_all(now_s)
                self._forget_book()
                # A full withdrawal discharges the reopen debt, because
                # _forget_book() is what the debt was FOR: the latch exists
                # to stop a stale two-sided belief surviving the venue's
                # carried->live cancel, and there is no belief left to be
                # stale about.
                self._reopen_pending = False
            else:
                self._client.cancel_all(now_s, withdrawing)
                # Clear on withdrawal, not on next observation: between the
                # two sits a belief in orders we just cancelled.
                for market in withdrawing:
                    self._resting[market] = RestingQuote()
                # [review] The latch is NOT cleared here. It used to be
                # cleared for both branches, so withdrawing ONE market
                # discharged the rebuild owed to the others -- whose beliefs
                # this branch deliberately leaves intact, and which are
                # exactly the ones that can still be stale after a reopen.
            # [review round 10] NO early return here. Returning when nothing
            # wanted to quote skipped the risk pass below entirely, so a
            # market HOLDing a two-sided book past its position limit kept it
            # for as long as a NEIGHBOUR was withdrawing -- and a venue pause
            # withdraws every tick. The withdraw result is reported after
            # risk has had its say; the risk_forced branch already returns
            # the same shape when it acts.
            withdraw_reason = reason

        # [review] RISK IS EVALUATED FOR EVERY LIVE MARKET, not only for the
        # ones already deciding to quote.
        #
        # The early HOLD below used to return before assess() ran at all, so
        # a two-sided in-ring book -- the state decide() is happiest about --
        # stayed live while the freshly fetched account crossed the position
        # limit or the 75% flatten line. FLATTEN and REDUCE_ONLY were
        # unreachable in exactly the situation they exist for: the loop was
        # content because the QUOTES were fine, and never asked whether the
        # POSITION was.
        # [review] Only when an account was actually read. During a session
        # backoff nothing is fetched, so `state` is the DEFAULT MarginState --
        # which utilisation() reports as fully used, by design, because
        # unreadable must mean no room. Feeding that to the risk pass would
        # cancel a resting book on every WAIT tick, which is the precise
        # opposite of WAIT's purpose: the book may be legitimately resting and
        # the renewal is a transient.
        #
        # Not fetching is not the same as fetching and failing. The latter
        # still flattens, because _margin_state() fails closed on a payload it
        # cannot read.
        risk_by_market: dict = {}
        for market in (self._markets if account_seen else []):
            oracle = oracles.get(market)
            base_size = self._base_size(oracle)
            # Contracts equivalent of the dollar limit at THIS oracle. Zero
            # oracle means zero base_size too, and assess() treats a
            # non-positive limit as "no limit" -- but base_size 0 places
            # nothing, so nothing is sized off the degenerate value.
            max_position = (self._max_position_usd / oracle
                            if oracle and oracle > 0.0 else 0.0)
            risk_by_market[market] = assess(
                state, market,
                base_size=base_size,
                max_position=max_position,
                ring_pct=self._ring_pct,
                half_spread_pct=self._half_spread_pct,
            )

        # A market holding a live quote that risk wants shrunk or gone must
        # act now, whatever decide() thought of the quote itself.
        # Only markets the quoting loop below will NOT reach. One that is
        # re-quoting already handles its own risk action there -- cancel then
        # place the shrinking side -- and pre-empting it here would cancel
        # the book and then skip the replacement, leaving the market flat
        # when it should have been reduced.
        risk_forced = [
            m for m, r in risk_by_market.items()
            if r.action is not RiskAction.NORMAL
            and not self._resting.get(m, RestingQuote()).empty
            and results.get(m, ("", ""))[0] != LoopAction.QUOTE.value
        ]
        # [sweep] Unconditional, NOT `and not any_quoted`. This is the same
        # mistake as the withdraw path two commits ago: a market past its
        # position or margin limit was left holding a live two-sided book
        # because a NEIGHBOUR happened to need a refresh. Risk is per-market
        # and so is the retraction.
        if risk_forced:
            worst = risk_by_market[risk_forced[0]]
            self._client.cancel_all(now_s, risk_forced)
            for market in risk_forced:
                self._resting[market] = RestingQuote()
                results[market] = (worst.action.value, worst.reason)
            if not any_quoted:
                return TickResult("withdraw", worst.reason, results)

        if not any_quoted:
            if withdrawing:
                return TickResult("withdraw", withdraw_reason, results)
            wait = LoopAction.WAIT.value
            waiting = [m for m, (a, _) in results.items() if a == wait]
            if waiting:
                return TickResult("wait", results[waiting[0]][1], results)
            return TickResult("hold", "all markets two-sided and in ring",
                              results)

        legs = []
        to_cancel: list = []
        for market, (action, _) in results.items():
            if action != LoopAction.QUOTE.value:
                continue
            oracle = oracles.get(market)
            base_size = self._base_size(oracle)
            risk = risk_by_market.get(market)
            if risk is None:
                # No account this tick; quoting proceeds on decide() alone.
                results[market] = ("skip", "no account snapshot this tick")
                continue
            if risk.action is RiskAction.FLATTEN:
                # [review] FLATTEN used to change the label and drop the
                # legs, which left the existing two-sided quote live and sent
                # nothing to reduce -- so at the 75% line the runner did
                # neither of the things the action names. Retract the book
                # for this market at minimum; closing the position itself is
                # a taker order and stays an operator decision.
                results[market] = (
                    "flatten",
                    risk.reason + " -- quotes retracted; the POSITION is "
                    "still open and needs closing by hand")
                _log.critical(
                    "permuto: %s margin past the flatten line -- resting "
                    "quotes retracted, but the position remains OPEN and "
                    "exposed. Close it manually.", market)
                # Only if something is actually resting. _resting was
                # reconciled from the venue at the top of this tick, so an
                # empty entry means there is nothing to retract and a cancel
                # would be a request that changes nothing.
                if not self._resting.get(market, RestingQuote()).empty:
                    to_cancel.append(market)
                continue

            reference = skewed_reference(oracle, risk.skew)
            if reference is None:
                results[market] = ("skip", "no usable reference price")
                continue

            # Carry RISK's reduction into the depth target, rather than
            # rebuilding a notional from the skewed price. Building the
            # ladder from the configured target alone would compute the
            # reduction and discard it -- a carried session would quote eight
            # times what its collateral covers and earn a rejected batch. But
            # reconstructing notional as size*skewed_price is not right
            # either: it silently shaves the skew off the standing notional,
            # so leaning on inventory would quietly cost depth credit, which
            # is the exact trade this design exists to avoid. Scaling the
            # target by risk's own ratio keeps skew free and reductions
            # honoured.
            depth_usd = self._target_depth_usd * (
                risk.size / base_size if base_size > 0.0 else 0.0
            )
            raw_specs = flags.get("specs")
            spec = (raw_specs.get(market, {})
                    if isinstance(raw_specs, dict) else {})
            ladder = quote_ladder(
                market, reference, depth_usd,
                levels=1, first_offset_pct=self._half_spread_pct,
                ring_pct=self._ring_pct,
                tick_size=spec.get("tick_size", 0.0001),
                lot_size=spec.get("lot_size", 1.0),
            )
            if risk.action is RiskAction.REDUCE_ONLY:
                # [review] batch_upsert is keyed on (market, side), so
                # omitting the risk-increasing side does not remove it -- the
                # old quote stays live beside the new reduce-only one, which
                # is the opposite of reducing. Cancel the market first, then
                # place the single shrinking leg.
                if not self._resting.get(market, RestingQuote()).empty:
                    to_cancel.append(market)

            for leg in ladder:
                if risk.action is RiskAction.REDUCE_ONLY:
                    # Keep only the side that shrinks the book, and mark it
                    # so depth_credit_usd stops counting a retreat as depth.
                    position = float(state.positions.get(market, 0.0) or 0.0)
                    shrinking = (
                        (position > 0 and leg.side is Side.SELL)
                        or (position < 0 and leg.side is Side.BUY)
                    )
                    if not shrinking:
                        continue
                    leg = type(leg)(leg.market, leg.side, leg.price,
                                    leg.size, True)
                legs.append(leg)

        if to_cancel:
            # Before the upsert, so the reduce-only leg is not racing the
            # risk-increasing one it replaces.
            self._client.cancel_all(now_s, to_cancel)
            for market in to_cancel:
                self._resting[market] = RestingQuote()

        if not legs:
            # [review round 10] NOT "hold". hold means "the book is resting
            # and correct"; here risk refused every leg, and after a
            # reduce-only cancel the book may be EMPTY. MainWindow treats
            # quote/hold as proof the loop trades, so reporting hold cleared
            # the not_quoting gate and painted PERMUTO ON over nothing
            # resting. risk_blocked gates the switch like any other
            # non-trading outcome.
            return TickResult("risk_blocked",
                              "risk left nothing to place", results)

        payload = build_upsert_batch(legs, oracles, ring_pct=self._ring_pct)
        response = self._client.batch_upsert(payload, now_s)

        # [release review] "partial" IS HTTP success on this venue, and the
        # response was thrown away -- a per-leg ALO refusal (a competitor's
        # aggressive in-ring rest crossing our leg) was invisible: the loop
        # believed both sides rested and re-sent the same crossing price on
        # the next drift check while min(bid, ask) earned zero. The
        # authenticated response SHAPE is not yet pinned by a live capture,
        # so this is deliberately conservative: any status other than a
        # clean acceptance is surfaced loudly and reported in the result,
        # and reconcile() heals the belief from open_orders next tick.
        batch_note = ""
        if isinstance(response, dict):
            status = str(response.get("status", "")).lower()
            if status and status not in ("ok", "success", "accepted"):
                batch_note = (" -- batch status %r; some legs may "
                              "not rest" % status)
                _log.warning(
                    "permuto: batch_upsert returned status %r (body %r); "
                    "believing open_orders over our own send",
                    status, str(response)[:400])

        # Believe only what we just sent, and only after it was accepted.
        for leg in legs:
            current = self._resting.get(leg.market, RestingQuote())
            if leg.side is Side.BUY:
                current = RestingQuote(leg.price, current.ask_price)
            else:
                current = RestingQuote(current.bid_price, leg.price)
            self._resting[leg.market] = current

        self._reopen_pending = False
        return TickResult("quote", "%d legs%s" % (len(legs), batch_note),
                          results)

    def _base_size(self, oracle: Optional[float]) -> float:
        """Contracts that carry ``target_depth_usd`` of notional per side."""
        if not oracle or not (oracle > 0.0):
            return 0.0
        return self._target_depth_usd / oracle


def _margin_state(account: Any, carried: bool) -> MarginState:
    """Read the venue's account payload defensively.

    Missing fields become 0.0, which `MarginState.utilisation()` reports as
    fully utilised -- so a malformed payload stops the loop adding risk rather
    than reading as an empty, healthy account.
    """
    if not isinstance(account, dict):
        return MarginState(carried=carried)

    def _num_present(src, *keys):
        """Value plus whether any of `keys` yielded a usable number."""
        for key in keys:
            value = src.get(key)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                return float(value), True
            if isinstance(value, str):
                try:
                    return float(value), True
                except ValueError:
                    continue
        return 0.0, False

    def _num(*keys):
        for key in keys:
            value = account.get(key)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                return float(value)
            if isinstance(value, str):
                try:
                    return float(value)
                except ValueError:
                    continue
        return 0.0

    # [review] An unreadable position is recorded as NaN, not dropped.
    #
    # Both loops used to `continue`, which removes the market from the dict --
    # and assess() reads a missing market as 0.0, i.e. FLAT. So a valid
    # equity/margin snapshot carrying one unreadable position let that market
    # take normal, risk-increasing quotes against inventory we could not see.
    # Unknown inventory became no inventory, which is the fail-open direction
    # on the one number the position limit exists to bound.
    #
    # assess() already refuses on a non-finite position (risk.py) -- the
    # sentinel simply never reached it.
    positions = {}
    raw = account.get("positions")
    # [review round 11] Present-and-well-typed is a separate fact from
    # empty. {} and [] are genuinely flat accounts; a MISSING key or a
    # wrong-typed value is an account whose inventory we cannot see, and
    # collapsing both into {} let assess() read unknown inventory as flat
    # and add risk against it.
    positions_readable = isinstance(raw, (dict, list))
    if isinstance(raw, dict):
        for market, value in raw.items():
            try:
                positions[market] = float(value)
            except (TypeError, ValueError):
                positions[market] = float("nan")
    elif isinstance(raw, list):
        for row in raw:
            if not isinstance(row, dict):
                continue
            market = row.get("market") or row.get("symbol")
            if market is None:
                continue
            try:
                positions[market] = float(
                    row.get("size", row.get("position", 0.0))
                )
            except (TypeError, ValueError):
                positions[market] = float("nan")

    equity = _num("equity_usd", "equity", "account_value")
    used, used_present = _num_present(
        account, "used_margin_usd", "used_margin", "margin_used")
    if equity > 0.0 and not used_present:
        # [review] Fail CLOSED on a partly-readable payload. Defaulting the
        # missing field to 0.0 made utilisation() report 0% -- maximum
        # headroom -- so the runner went on adding risk against an account it
        # could not actually read, which is the opposite of what the
        # docstring promises. Treat unknown margin as fully used.
        used = equity

    return MarginState(
        equity_usd=equity,
        used_margin_usd=used,
        positions=positions,
        positions_readable=positions_readable,
        carried=carried,
    )
