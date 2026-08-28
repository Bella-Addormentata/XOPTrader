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
        max_position: float = 100.0,
        ring_pct: float = 2.0,
        half_spread_pct: float = 0.25,
        quote_when_carried: bool = True,
    ) -> None:
        self._client = client
        self._markets = list(markets)
        self._target_depth_usd = target_depth_usd
        self._max_position = max_position
        self._ring_pct = ring_pct
        self._half_spread_pct = half_spread_pct
        self._quote_when_carried = quote_when_carried

        self._resting: dict = {m: RestingQuote() for m in self._markets}
        self._was_paused = False
        self._reopen_pending = False

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
        if not paused:
            action = self._client.ensure_session(now_s)
            session_waiting = action is RenewAction.WAIT
            session_ok = action in (RenewAction.OK, RenewAction.RENEW)

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
        state = MarginState(carried=carried)
        if session_ok and not paused:
            self.reconcile(self._client.open_orders(now_s))
            state = _margin_state(self._client.account(now_s), carried)

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
                self._client.cancel_all(now_s)
                self._forget_book()
            else:
                self._client.cancel_all(now_s, withdrawing)
                # Clear on withdrawal, not on next observation: between the
                # two sits a belief in orders we just cancelled.
                for market in withdrawing:
                    self._resting[market] = RestingQuote()
            self._reopen_pending = False
            if not any_quoted:
                return TickResult("withdraw", reason, results)

        if not any_quoted:
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
            risk = assess(
                state, market,
                base_size=base_size,
                max_position=self._max_position,
                ring_pct=self._ring_pct,
                half_spread_pct=self._half_spread_pct,
            )
            if risk.action is RiskAction.FLATTEN:
                # [review] FLATTEN used to change the label and drop the
                # legs, which left the existing two-sided quote live and sent
                # nothing to reduce -- so at the 75% line the runner did
                # neither of the things the action names. Retract the book
                # for this market at minimum; closing the position itself is
                # a taker order and stays an operator decision.
                results[market] = ("flatten", risk.reason)
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
            ladder = quote_ladder(
                market, reference, depth_usd,
                levels=1, first_offset_pct=self._half_spread_pct,
                ring_pct=self._ring_pct,
            )
            if risk.action is RiskAction.REDUCE_ONLY:
                # [review] batch_upsert is keyed on (market, side), so
                # omitting the risk-increasing side does not remove it -- the
                # old quote stays live beside the new reduce-only one, which
                # is the opposite of reducing. Cancel the market first, then
                # place the single shrinking leg.
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
            return TickResult("hold", "risk left nothing to place", results)

        payload = build_upsert_batch(legs, oracles, ring_pct=self._ring_pct)
        self._client.batch_upsert(payload, now_s)

        # Believe only what we just sent, and only after it was accepted.
        for leg in legs:
            current = self._resting.get(leg.market, RestingQuote())
            if leg.side is Side.BUY:
                current = RestingQuote(leg.price, current.ask_price)
            else:
                current = RestingQuote(current.bid_price, leg.price)
            self._resting[leg.market] = current

        self._reopen_pending = False
        return TickResult("quote", "%d legs" % len(legs), results)

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

    positions = {}
    raw = account.get("positions")
    if isinstance(raw, dict):
        for market, value in raw.items():
            try:
                positions[market] = float(value)
            except (TypeError, ValueError):
                continue
    elif isinstance(raw, list):
        for row in raw:
            if not isinstance(row, dict):
                continue
            market = row.get("market") or row.get("symbol")
            try:
                positions[market] = float(
                    row.get("size", row.get("position", 0.0))
                )
            except (TypeError, ValueError):
                continue

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
        carried=carried,
    )
