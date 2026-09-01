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
import time
from dataclasses import dataclass, field
from typing import Any, Optional

from .auth import PermutoAuthError
from .batch import BatchError, build_upsert_batch
from .client import PermutoNotLinked
from .orders import Side, depth_credit_usd, quote_ladder
from .quoting import (REQUOTE_AT_RING_FRACTION, LoopAction,
                      RestingQuote, VenueView, decide)
from .risk import MarginState, RiskAction, assess, skewed_reference
from .band_guard import VENUE_BAND_PCT, BandGuard
from .cross_backoff import CrossBackoff, headroom_pct
from .preflight import (latest_oracle, preflight_leg_price,
                        quantise_toward, rescaled_size)
from .curfew import (OPENS_UTC, OracleFreeze, Stage, assess_curfew,
                     permitted_leg_size)
from .modes import Profile, profile_for
from .session import RenewAction

#: How often a SUSTAINED full withdrawal re-asserts the cancel when we
#: already believe the book is empty. Covers a stale belief without turning
#: a fourteen-hour pause into ten thousand identical requests.
RECANCEL_INTERVAL_S = 60.0

#: How far ahead the venue-side scheduled cancel-all is pushed each tick.
#: 24 missed ticks of headroom -- generous against transients, small against
#: the hours an unnoticed crash would otherwise leave quotes resting.
DMS_EXTEND_S = 120.0

# [WATCH 2026-08-29] Contest telemetry cadences. The tick-budget warning
# threshold is half the 5s tick so drift is visible long before ticks
# start overlapping; the leaderboard watch is throttled hard because it is
# a full paged read of a public endpoint.
TICK_BUDGET_WARN_S = 2.5
LEADERBOARD_WATCH_S = 300.0

_log = logging.getLogger(__name__)

__all__ = ["QuoteRunner", "TickResult"]

#: Batch statuses that mean "the venue took it".
#:
#: [live 2026-08-31, contest open] 'batch_upserted' was the venue's ACTUAL
#: success status and we had never seen it. The set was
#: ("batch_ok", "batch_partial") -- and the comment beside it admitted
#: batch_ok was inferred "by symmetry" from the one status a capture had
#: caught. So every successful batch at the contest open was logged as an
#: error, and the breaker added below would have throttled a perfectly
#: healthy loop after five of them.
BATCH_ACCEPTED = ("batch_ok", "batch_partial", "batch_upserted")

#: Statuses where the venue is explicitly telling us it did NOT take the
#: batch. These are believed over the legs.
#:
#: [review] The leg-trusting fallback below must NOT extend to these. On
#: 2026-08-30 the venue answered 'batch_failed' during its pre-competition
#: reset while STILL reporting legs 'placed' -- best-effort placement
#: inside a failing envelope. Trusting legs there would silently
#: reclassify a genuine refusal as success and record orders the venue may
#: have rolled back. Legs are evidence only when the envelope is UNKNOWN.
BATCH_REJECTED = ("batch_failed", "batch_rejected", "error")


def _requote_safe_backoff(ring_pct: float, half_spread_pct: float,
                          skew_frac_abs: float) -> float:
    """The most backoff this leg can carry and still rest.

    [review] Bounded by BOTH constraints, because satisfying one and
    failing the other still costs the quote:

      * the credit RING -- outside it the leg earns nothing, so retreating
        past it turns a rejected leg into a resting worthless one;
      * the RE-QUOTE TRIGGER -- decide() replaces any leg further than
        ring * REQUOTE_AT_RING_FRACTION from the oracle, so a leg born
        past it is cancelled and replaced every tick, forever.

    Computed from the CURRENT skew rather than whatever applied when the
    offset was learned, so an offset earned under wider headroom is
    clamped instead of reapplied blind.
    """
    ring_room = headroom_pct(ring_pct, half_spread_pct, skew_frac_abs)
    trigger_budget = ring_pct * REQUOTE_AT_RING_FRACTION * 0.8
    trigger_room = (trigger_budget - abs(half_spread_pct)
                    - abs(skew_frac_abs) * 100.0)
    return max(0.0, min(ring_room, trigger_room))


def _legs_all_accepted(leg_rows) -> bool:
    """True when every leg reports the venue acted on it.

    THE STATUS STRING IS NOT THE EVIDENCE, the legs are. Guessing at a
    closed vocabulary has now failed twice in two days -- once by treating
    a real success as an error, once by treating a venue outage as our
    bug. A leg that says 'placed' or 'modified' was accepted whatever the
    envelope around it is called, so an unknown status with clean legs is
    trusted, logged once, and does not trip the breaker.
    """
    if not isinstance(leg_rows, list) or not leg_rows:
        return False
    for row in leg_rows:
        if not isinstance(row, dict):
            return False
        if row.get("rejection_reason"):
            return False
        if str(row.get("action", "")).lower() not in ("placed", "modified",
                                                      "cancelled", "unchanged"):
            return False
    return True


#: [BATCHBREAKER] Consecutive unrecognised batch statuses before the loop
#: stops re-sending every tick. Five is ~25s at a 5s tick: long enough that
#: a genuine transient heals itself, short enough that a systematic
#: rejection cannot place hundreds of unacknowledged orders overnight.
BATCH_FAIL_STREAK_LIMIT = 5

#: [DEPTHSIGNAL] Seconds between positive depth-credit INFO lines. Zero
#: credit is never throttled -- that one is the failure, and it should be
#: as loud as it is often.
DEPTH_LOG_INTERVAL_S = 60.0

#: How often to probe once the breaker is open. Still self-heals, at 1/min
#: instead of 12/min.
BATCH_PROBE_INTERVAL_S = 60.0


@dataclass
class TickResult:
    """What one tick did. Returned rather than logged only, so the GUI can
    show the current state without re-deriving it."""

    action: str = "idle"
    reason: str = ""
    markets: dict = field(default_factory=dict)
    error: str = ""
    #: [CURFEW] The inventory-curfew stage this tick ran under, or "" when
    #: the curfew is off. Carried so the GUI can say WHY the book is
    #: one-sided or small: without it an operator glancing at the window at
    #: 22:00 sees bid-only quoting and no reason for it.
    curfew: str = ""

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
        curfew_enabled: bool = True,
        ring_pct: float = 2.0,
        half_spread_pct: float = 0.25,
        quote_when_carried: bool = True,
        oracle_fetch: Any = None,
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
        # [CURFEW 2026-08-30] The venue does NOT pause when the underlying
        # shuts -- measured: trading_paused False and every market active
        # while all three oracles sat frozen to sixteen digits. It keeps
        # matching orders against a stale price, which is exactly the
        # window the competing long-carry trade exploits against MMs. So
        # inventory is capped on a clock instead: the cap ramps down before
        # each close and stays at the floor all night, which turns an
        # oversized position into maker-side REDUCE_ONLY quotes through the
        # machinery risk.assess() already has. Nothing here crosses the
        # spread.
        # [BATCHBREAKER 2026-08-30] Consecutive batch statuses we do not
        # recognise as acceptance. A repeating rejection used to re-send
        # the same batch every tick forever -- measured live at ~12 sends
        # a minute for 5+ minutes, each one placing orders at the venue
        # that the failure path then declined to record.
        self._batch_fail_streak = 0
        self._batch_muted_until_s = 0.0
        #: [DEPTHSIGNAL] When the positive depth credit was last logged.
        #: The figure goes to gui.log, which is configured INFO-only, so a
        #: DEBUG line would be invisible exactly when it is being relied on.
        #: But a tick is ~5s and an unthrottled INFO would be 720 lines an
        #: hour, so it is rate limited: a healthy book still says so once a
        #: minute, and silence then means something is actually wrong
        #: rather than merely un-logged.
        self._depth_logged_at_s = 0.0
        #: [MODES] The half-spread actually in force this tick, after the
        #: stage profile. Read by the crossing-backoff headroom, which must
        #: budget the spread we USED and not the one we configured.
        self._eff_half_spread = half_spread_pct
        self._eff_half_spread_by_market: dict = {}
        #: [ANTICROSS] Learned per-market retreat from the book. The venue
        #: refused 51 legs for crossing on 2026-08-31 and publishes no L2, so
        #: its own refusals are the only signal available. Depth credit is
        #: flat inside the ring, so retreating costs no eligibility.
        self._cross_backoff = CrossBackoff()
        #: Skew last applied per market, so the backoff's ring headroom can
        #: account for it at response time (skew and backoff push the
        #: trailing leg the same way, and the ring does not care which).
        self._last_skew: dict = {}
        #: Unknown-but-accepted statuses already reported, so the "add it
        #: to BATCH_ACCEPTED" nudge is logged once rather than every tick.
        self._unknown_ok_statuses: set = set()
        # [BANDGUARD] Oracle velocity per market, so leg prices are clamped
        # inside the venue's +/-5% oracle band ON ARRIVAL. At the contest
        # open, whole minutes of batches 400'd because grace-aged reads
        # plus the 2% ring overshot the band while the oracle collapsed.
        self._band_guard = BandGuard()
        # [PREFLIGHT] Optional callable returning {market: price}, invoked
        # immediately BEFORE the batch is sent. band_guard anchors to the
        # tick's read and measures staleness as fetch age -- but age is not
        # divergence: a 2s-old read is 5% behind when vol collapses 20% a
        # minute, which is how "Price 0.047 outside band (+/-5% of 0.04423)"
        # happened with a fresh fetch. Re-reading here shrinks the exposure
        # to one request's flight time, measured rather than assumed.
        self._oracle_fetch = oracle_fetch
        #: Measured duration of the last pre-send fetch: the best estimate
        #: of how long the NEXT request will take.
        self._send_latency_s = 0.25
        self._curfew_retract_pending = False
        self._curfew_enabled = curfew_enabled
        self._freeze = OracleFreeze()
        self._curfew_stage = None
        self._curfew = None
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
        # [WATCH] Contest flags whose TRANSITIONS change the plan (C-09):
        # untraded_purge_at flipping non-null starts the qualifying-fill
        # clock; signup_closed flipping true ends re-registration forever.
        self._watched_flags: dict = {}
        # [WATCH] Telemetry state: slow-tick latch, leaderboard throttle,
        # last (time, depth) sample, last trade count, stall strikes.
        self._tick_slow: bool = False
        self._lb_next_s: float = 0.0
        self._lb_prev: tuple = (0.0, 0.0)
        self._lb_trades: int = -1
        self._lb_stall_strikes: int = 0
        # [live 2026-08-29] Last leg-rejection note, so a persistent benign
        # rejection (ALO-cross) logs once per CHANGE rather than per tick.
        self._last_leg_rejections: str = ""

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
        # [review round 11] A MALFORMED payload keeps the old belief; only a
        # well-formed one may declare the book empty. Normalising garbage to
        # [] overwrote the belief with "empty", and an empty belief SKIPS the
        # safety cancel on the withdraw and flatten paths -- so a venue
        # hiccup that mangled one open_orders response could leave live
        # orders resting precisely when risk wanted them gone. Keeping the
        # stale belief errs the other way: at worst a cancel is sent for
        # orders already gone, which changes nothing.
        if isinstance(open_orders, dict):
            rows = open_orders.get("orders", open_orders.get("open"))
            if not isinstance(rows, list):
                _log.warning("permuto: open_orders carried no order list "
                             "(%r) -- keeping the previous belief",
                             str(open_orders)[:200])
                return
        elif isinstance(open_orders, list):
            rows = open_orders
        else:
            _log.warning("permuto: open_orders was %s, not an object -- "
                         "keeping the previous belief",
                         type(open_orders).__name__)
            return

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
        started = time.perf_counter()
        try:
            result = self._tick(now_s, oracles or {}, flags or {})
        except PermutoNotLinked as exc:
            # Not transient and not recoverable by retrying: nothing is
            # resting to withdraw and nothing can be placed.
            result = TickResult("blocked", str(exc), error=str(exc))
        except (PermutoAuthError, BatchError) as exc:
            _log.warning("permuto: tick failed: %s", exc)
            result = TickResult("error", str(exc), error=str(exc))
        except Exception as exc:  # noqa: BLE001
            # The loop must outlive its own bugs for ~102 unattended hours.
            _log.exception("permuto: unexpected tick failure")
            result = TickResult("error", repr(exc), error=repr(exc))
        # [CURFEW] Stamped here rather than at each of the dozen
        # TickResult sites, so no return path can forget it -- including
        # the exception paths above, where knowing the curfew state is
        # exactly as useful.
        if self._curfew is not None:
            result.curfew = self._curfew.stage.value
        # [WATCH] Wall-clock per tick, so a slowing venue or a regressing
        # loop is visible AS it degrades rather than inferred from missing
        # depth two hours later. DEBUG when healthy; WARN past half the
        # cadence, throttled to state CHANGES so a persistently slow venue
        # does not write the same line forever.
        elapsed = time.perf_counter() - started
        if elapsed >= TICK_BUDGET_WARN_S:
            if not self._tick_slow:
                self._tick_slow = True
                _log.warning("permuto: tick took %.2fs (budget warn at "
                             "%.1fs; action=%s) -- watching for overlap",
                             elapsed, TICK_BUDGET_WARN_S, result.action)
        else:
            if self._tick_slow:
                self._tick_slow = False
                _log.info("permuto: tick time recovered (%.2fs)", elapsed)
            _log.debug("permuto: tick %.2fs action=%s",
                       elapsed, result.action)
        return result

    def _tick(self, now_s: float, oracles: dict, flags: dict) -> TickResult:
        # [CURFEW] Ground truth for "the underlying is shut" before anything
        # else reads the clock: a frozen oracle can only ever TIGHTEN the
        # cap, so observing early costs nothing and a missed observation
        # would loosen it.
        self._band_guard.observe(now_s, oracles)
        if self._curfew_enabled:
            self._freeze.observe(now_s, oracles)
            curfew = assess_curfew(
                now_s, self._max_position_usd,
                frozen_oracle=self._freeze.frozen(now_s))
            if curfew.stage is not self._curfew_stage:
                _log.warning("permuto: inventory curfew %s -> %s: %s "
                             "(long $%.0f / short $%.0f of $%.0f)",
                             getattr(self._curfew_stage, "value", "none"),
                             curfew.stage.value, curfew.reason,
                             curfew.long_cap_usd, curfew.short_cap_usd,
                             self._max_position_usd)
                # [review] RETRACT THE BOOK THE NEW STAGE NO LONGER ALLOWS.
                # The size clamp below only shapes legs we are about to
                # place; an ask resting from before the close stays live and
                # takeable, and decide() answers HOLD for a quote that is
                # still fresh and in-ring -- so without this the short
                # prohibition never reached the order that mattered. Only
                # latch the new stage once the retraction actually
                # succeeded, so a failed cancel is retried next tick rather
                # than silently skipped.
                # [live 2026-08-30] DEFERRED, not done here: this runs
                # before ensure_session(), so on the first tick after a
                # restart the cancel always failed with "needs a session
                # and none is held". It self-healed a tick later, but the
                # retraction belongs after the session exists.
                #
                # [review] EXCEPT INTO EXIT. That transition tightens the
                # CAPS but does not forbid anything already resting: the
                # danger is the OPEN, not the close, and a two-sided pair
                # keeps earning credit right up to the bell. Blanket
                # retraction here cancelled the book EXIT is supposed to
                # leave alone -- and did it before the profile was ever
                # consulted, so `withdraw=False` could never take effect.
                # The caps still bind; nothing that would GROW inventory
                # gets placed. This only spares what is already there.
                # [review] AND THAT INCLUDES EXIT. An exemption was tried
                # here so EXIT could keep earning credit to the bell, and
                # it does not survive the cap invariant: a pair placed
                # early in RAMP can be ~$420 a side, while EXIT permits
                # ~$300 long / $120 short, and RestingQuote records PRICES
                # but not quantities -- so nothing here can prove a
                # retained order fits the tightened caps. A fill during
                # EXIT would then breach the cap and carry the excess into
                # the overnight window, which is the exact failure the
                # curfew exists to prevent. Fifteen minutes of forgone
                # credit is the cheaper side of that trade.
                self._curfew_retract_pending = True
            self._curfew = curfew

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

        # [WATCH C-09] Contest-flag transitions, from the same flags dict
        # the tick already receives -- no extra request. Logged only on
        # CHANGE: these flip at most a handful of times all week, and each
        # flip changes what the operator should be doing.
        for key in ("untraded_purge_at", "signup_closed", "CONTEST_START",
                    "contest_start"):
            if key in flags and flags.get(key) != self._watched_flags.get(key):
                _log.warning("permuto: contest flag %s changed: %r -> %r",
                             key, self._watched_flags.get(key),
                             flags.get(key))
                self._watched_flags[key] = flags.get(key)

        # [WATCH 2026-08-29] Depth-accrual and fill telemetry, every
        # LEADERBOARD_WATCH_S. Net under the loop like the dead man's
        # switch: a failure here must never affect the tick. This is the
        # contest's outcome-side check -- quotes can rest while credit
        # silently stops (venue pause, ring drift), and depth is the number
        # being ranked, so watch the number itself. A trade_count increase
        # is logged loudly: Monday morning it doubles as the qualifying
        # fill confirmation (C-08).
        if now_s >= self._lb_next_s:
            self._lb_next_s = now_s + LEADERBOARD_WATCH_S
            try:
                from .auth import leaderboard_entry
                uid = getattr(self._client, "_user_id", "") or ""
                row = leaderboard_entry(uid) if uid else None
                if row:
                    depth = float(row.get("depth_seconds_5d") or 0.0)
                    trades = int(row.get("trade_count") or 0)
                    pnl = row.get("total_pnl")
                    prev_t, prev_depth = self._lb_prev
                    self._lb_prev = (now_s, depth)
                    if self._lb_trades >= 0 and trades > self._lb_trades:
                        _log.warning(
                            "permuto: FILL LANDED -- trade_count %d -> %d, "
                            "pnl %s", self._lb_trades, trades, pnl)
                    self._lb_trades = trades
                    if prev_t > 0.0:
                        rate = (depth - prev_depth) / max(1.0, now_s - prev_t)
                        _log.info("permuto: depth %.0f (+%.0f/s), trades "
                                  "%d, pnl %s", depth, rate, trades, pnl)
                        quoting = any(not rq.empty
                                      for rq in self._resting.values())
                        if rate <= 0.0 and quoting:
                            self._lb_stall_strikes += 1
                            # Warn on the TRANSITION only (== 2, not >= 2):
                            # a stall lasting all day must not re-warn
                            # every 5-minute sample. Recovery resets the
                            # strikes, so the next episode warns again.
                            if self._lb_stall_strikes == 2:
                                _log.warning(
                                    "permuto: depth accrual STALLED for "
                                    "%.0f min while quotes rest -- one-"
                                    "sided books earn zero (min(bid,ask)); "
                                    "check ring placement and venue state",
                                    self._lb_stall_strikes
                                    * LEADERBOARD_WATCH_S / 60.0)
                        else:
                            self._lb_stall_strikes = 0
                    else:
                        _log.info("permuto: depth %.0f, trades %d, pnl %s",
                                  depth, trades, pnl)
            except Exception as exc:  # noqa: BLE001
                _log.debug("permuto: leaderboard watch failed: %s", exc)

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
            # [CURFEW] A side the curfew has closed cannot be restored, so
            # its absence must not read as a book to repair -- otherwise the
            # loop re-upserts an identical quote on every tick, all night.
            one_sided_ok = bool(
                self._curfew is not None
                and (self._curfew.short_cap_usd <= 0.0
                     or self._curfew.long_cap_usd <= 0.0))
            call = decide(
                view, self._resting.get(market, RestingQuote()),
                ring_pct=self._ring_pct,
                quote_when_carried=self._quote_when_carried,
                one_sided_ok=one_sided_ok,
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
        # [CURFEW] The deferred stage-change retraction, now that a
        # session exists. Latched only on success, so a failed cancel is
        # retried next tick rather than leaving the old book resting under
        # caps that no longer permit it.
        if getattr(self, "_curfew_retract_pending", False) and session_ok:
            try:
                self._client.cancel_all(now_s, list(self._markets))
            except Exception as exc:  # noqa: BLE001
                _log.error("permuto: curfew stage change could not retract "
                           "the book (%s); retrying next tick", exc)
            else:
                for market in self._markets:
                    self._resting[market] = RestingQuote()
                self._curfew_retract_pending = False
                if self._curfew is not None:
                    self._curfew_stage = self._curfew.stage

        # [MODES] Computed ONCE, above the risk loop, because the stage is a
        # property of the tick and not of a market.
        #
        # [review] The effective half-spread has to be the SAME number
        # everywhere. The first version widened only the ladder while
        # risk.assess() and the crossing headroom still budgeted the
        # configured 0.25%. At defaults that is a real failure and not a
        # tidiness point: a session position near half its cap produces
        # ~0.48% skew, and 0.48 + 0.75 puts a leg past the 1.2% re-quote
        # trigger the moment it is born -- so every tick would cancel and
        # replace a quote that was never wrong, and the backoff would budget
        # ring headroom against a spread nobody was using.
        # [review] POSTURE reads the SCHEDULE stage, not the effective one.
        # The effective stage is deliberately lossy -- a frozen oracle maps a
        # scheduled SETTLING back to PREOPEN so the short cap stays shut --
        # and that made the (SETTLING, stale) branch unreachable: after the
        # bell, a still-frozen oracle arrived as PREOPEN, which quotes. So
        # the guard against quoting a stale post-open price was dead code,
        # killed by the other fix I made hours earlier.
        # [review] `schedule_stage or stage` masks the effective stage,
        # because Stage.UNSCHEDULED is TRUTHY. Past the end of the session
        # table assess_curfew returns effective CLOSED for a frozen oracle
        # while recording schedule_stage=UNSCHEDULED -- so the `or` picked
        # UNSCHEDULED and applied the wide, half-size SESSION profile
        # instead of the full-size CLOSED earning profile, throwing away
        # the overnight window precisely when the schedule has run out and
        # the freeze detector is the only thing that knows the truth.
        #
        # UNSCHEDULED means "the clock abstains", which is the same as
        # having no schedule at all: fall through to the effective stage.
        posture_stage = Stage.UNSCHEDULED
        if self._curfew is not None:
            scheduled = self._curfew.schedule_stage
            posture_stage = (scheduled
                             if scheduled not in (None, Stage.UNSCHEDULED)
                             else self._curfew.stage)
        # Stage-level default; market-specific posture can tighten from here.
        default_profile = profile_for(
            posture_stage,
            oracle_fresh=not self._freeze.frozen(now_s),
        )
        self._eff_half_spread = (self._half_spread_pct
                                 * default_profile.spread_mult)
        profile_by_market: dict[str, Profile] = {}
        eff_half_spread_by_market: dict[str, float] = {}
        for market in self._markets:
            # [review] After the bell, "fresh" has to mean PRINTED SINCE
            # THE BELL, not merely "moved in the last 180 seconds". An
            # oracle that ticks at 13:29 and then stops is still inside the
            # confirmation window at the 13:30 open, so market_frozen()
            # reports it live and the runner quotes against a price that
            # predates the session -- the exact stale-price case this gate
            # exists to close, walking straight through it.
            #
            # Outside SETTLING there is no boundary to be after, so the
            # ordinary freshness test governs.
            if posture_stage is Stage.SETTLING:
                opened = [o for o in OPENS_UTC if o <= now_s]
                fresh = bool(opened) and self._freeze.changed_since(
                    market, max(opened))
            else:
                # [review] gone_quiet, not market_frozen. The gate must
                # outlive SETTLING -- the aggregate detector keeps the
                # curfew in SESSION while ANY market prints, so a
                # neighbour that stops is quoted against its own stale
                # price indefinitely -- but an UNSEEN market must not be
                # treated as stale, or the loop refuses to quote anything
                # until it has watched a second distinct value arrive.
                fresh = not self._freeze.market_gone_quiet(market, now_s)
            market_profile = profile_for(
                posture_stage,
                oracle_fresh=fresh,
            )
            profile_by_market[market] = market_profile
            eff_half_spread_by_market[market] = (
                self._half_spread_pct * market_profile.spread_mult
            )
        self._eff_half_spread_by_market = eff_half_spread_by_market

        risk_by_market: dict = {}
        for market in (self._markets if account_seen else []):
            oracle = oracles.get(market)
            base_size = self._base_size(oracle)
            # Contracts equivalent of the dollar limit at THIS oracle. Zero
            # oracle means zero base_size too, and assess() treats a
            # non-positive limit as "no limit" -- but base_size 0 places
            # nothing, so nothing is sized off the degenerate value.
            # [CURFEW] The curfew cap, not the raw configured limit. Its
            # floor is deliberately non-zero: assess() reads a
            # non-positive max_position as "no limit", so ramping to zero
            # would restore UNLIMITED inventory at the exact moment the
            # curfew meant to forbid it.
            # Side-aware: overnight the SHORT cap is the tight one, because
            # a short carried through the reopen is the position that gets
            # liquidated (see curfew.OVERNIGHT_LONG_FRACTION).
            cap_usd = self._max_position_usd
            if self._curfew is not None:
                cap_usd = self._curfew.cap_for(
                    float(state.positions.get(market, 0.0) or 0.0))
            # Clamped strictly positive: the overnight SHORT cap is zero,
            # and assess() reads a non-positive max_position as "no limit".
            # The prohibition is enforced by the per-leg veto below; what
            # assess() needs here is a number that keeps an existing short
            # in REDUCE_ONLY rather than switching the limit off.
            max_position = (max(cap_usd, 1e-9) / oracle
                            if oracle and oracle > 0.0 else 0.0)
            risk_by_market[market] = assess(
                state, market,
                base_size=base_size,
                max_position=max_position,
                ring_pct=self._ring_pct,
                half_spread_pct=eff_half_spread_by_market[market],
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

        # [MODES review] A withdrawing posture must RETRACT, not merely
        # decline to place -- and it has to happen HERE, above the
        # not-any_quoted early returns.
        #
        # The case that exposed it: the clock enters SETTLING while the
        # oracle is still frozen. The effective curfew stage stays PREOPEN,
        # so no stage-change cancellation fires; decide() says HOLD because
        # the resting quote is fine on its own terms; the tick returns
        # "all markets resting and in ring" -- and the stale order sits
        # there waiting for the opening gap to fill it. A guard further
        # down was never reached, which is the whole reason this is a
        # separate pass over EVERY market rather than a branch inside the
        # quoting loop.
        #
        # Only `withdraw` profiles do this -- which is currently BOTH
        # non-quoting stages, EXIT and stale SETTLING. (An earlier draft
        # exempted EXIT so it could earn to the bell; that did not survive
        # the cap invariant and was reverted. This comment used to still
        # describe the exemption, which is how it would come back.)
        pull = [m for m in self._markets
                if (profile_by_market.get(m) is not None
                    and profile_by_market[m].withdraw
                    and not self._resting.get(m, RestingQuote()).empty)]
        shut = [m for m in self._markets
                if (profile_by_market.get(m) is not None
                    and profile_by_market[m].withdraw)]
        for market in shut:
            results[market] = ("withdraw", profile_by_market[market].reason)
        if pull:
            reason = profile_by_market[pull[0]].reason
            _log.warning("permuto: withdrawing %s -- %s",
                         ", ".join(sorted(pull)), reason)
            try:
                self._client.cancel_all(now_s, sorted(pull))
                for market in pull:
                    self._resting[market] = RestingQuote()
            except Exception as exc:  # noqa: BLE001 - reported, not raised
                _log.error("permuto: could not withdraw %s: %s", pull, exc)
                return TickResult(
                    "error", "stale quote still resting; withdrawal "
                    "failed: %s" % exc, results)
            if not any_quoted:
                return TickResult("withdraw", reason, results)

        if not any_quoted:
            # [review] `shut`, not `pull`. `pull` is only the markets that
            # still had something resting to cancel, and `withdrawing` is a
            # snapshot of `results` taken far above -- BEFORE the loop just
            # now rewrote those entries. Enter EXIT with a stage change that
            # already cancelled the book and all three of those are empty
            # while every per-market result says "withdraw", so this fell
            # through to the "hold" below and told the GUI the book was live
            # over nothing at all. The posture is what makes this a
            # withdrawal; whether there happened to be an order left to pull
            # is incidental.
            if shut:
                return TickResult(
                    "withdraw", profile_by_market[shut[0]].reason, results)
            if withdrawing:
                return TickResult("withdraw", withdraw_reason, results)
            wait = LoopAction.WAIT.value
            waiting = [m for m, (a, _) in results.items() if a == wait]
            if waiting:
                return TickResult("wait", results[waiting[0]][1], results)
            # [CURFEW] Not necessarily two-sided any more: overnight the
            # ask is closed by design, and claiming otherwise would tell an
            # operator the book is whole when half of it is deliberately
            # absent.
            return TickResult("hold", "all markets resting and in ring",
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
                pos = state.positions.get(market, 0.0)
                has_pos = pos != 0.0  # NaN compares unequal: unreadable
                                      # inventory is treated as a position.
                if has_pos:
                    results[market] = (
                        "flatten",
                        risk.reason + " -- quotes retracted; the POSITION "
                        "is still open and needs closing by hand")
                    _log.critical(
                        "permuto: %s margin past the flatten line -- "
                        "resting quotes retracted, but the position "
                        "remains OPEN and exposed. Close it manually.",
                        market)
                else:
                    results[market] = (
                        "flatten",
                        risk.reason + " -- refusing to quote (no open "
                        "position)")
                    _log.critical(
                        "permuto: %s margin past the flatten line "
                        "(utilisation %.0f%%, equity $%.0f) -- refusing "
                        "to quote. No open position.",
                        market, state.utilisation() * 100.0,
                        state.equity_usd)
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
            # [ANTICROSS] Sit as far from the book as the venue's refusals
            # say we must. quote_ladder still clamps every level inside the
            # ring, so this can widen the placement but never the credit
            # footprint -- a leg outside the ring earns nothing.
            self._last_skew[market] = risk.skew
            # [MODES] The stage decides the POSTURE, the curfew and risk
            # decide the LIMITS -- the profile is applied first precisely so
            # both can still only reduce it, never the other way round.
            market_profile = profile_by_market[market]
            eff_half_spread = eff_half_spread_by_market[market]
            if not market_profile.quote:
                results[market] = ("skip", market_profile.reason)
                continue
            depth_usd *= market_profile.depth_mult
            if depth_usd <= 0.0:
                results[market] = ("skip", market_profile.reason)
                continue
            ladder = quote_ladder(
                market, reference, depth_usd,
                levels=1,
                # [review] CAP THE LEARNED OFFSET AGAINST CURRENT HEADROOM,
                # and against the RE-QUOTE TRIGGER, not merely the ring.
                #
                # Two ways the raw learned value goes wrong. It was learned
                # under whatever skew and oracle applied then, so a fill
                # that raises skew leaves an offset legal at the time and
                # not now -- 1.607% learned while flat becomes a 2.857% ask
                # once skew reaches 0.95%, outside the 2% ring entirely.
                # And even inside the ring, spread + skew + backoff past
                # the 1.2% trigger means decide() replaces the quote on the
                # very next tick: the churn this budget exists to stop.
                first_offset_pct=(
                    eff_half_spread
                    + min(self._cross_backoff.offset_pct(market),
                          _requote_safe_backoff(
                              self._ring_pct, eff_half_spread,
                              self._last_skew.get(market, 0.0)))),
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
                # [CURFEW] Clamp the leg to the room left under its side's
                # cap. A yes/no veto was not enough: the ladder is sized to
                # target_depth_usd, so a leg permitted merely because it
                # "reduces" could still overshoot flat and open the very
                # short the curfew forbids -- and at the EXIT cap one leg
                # was eight times the position it was trying to hold, so
                # the ramp could never converge.
                # [BANDGUARD] Pull the price inside the venue band as it
                # will be judged on ARRIVAL, or drop the leg when the
                # projected in-flight drift leaves no window. One out-of-
                # band leg 400s the whole batch -- every sibling dies with
                # it -- so a dropped leg here is strictly better than the
                # rejection it would have caused.
                if oracle and oracle > 0.0:
                    oracle_age = 0.0
                    try:
                        oracle_age = float(flags.get("oracle_age_s") or 0.0)
                    except (TypeError, ValueError):
                        oracle_age = 0.0
                    clamped = self._band_guard.clamp_price(
                        market, oracle, leg.price, oracle_age)
                    if clamped <= 0.0:
                        results[market] = (
                            "skip",
                            "oracle moving too fast for the venue band "
                            "(%.2f%%/s, read %.1fs old)"
                            % (self._band_guard.velocity(market),
                               oracle_age))
                        continue
                    if clamped != leg.price:
                        leg = type(leg)(leg.market, leg.side, clamped,
                                        leg.size, leg.reduce_only)
                if self._curfew is not None and oracle and oracle > 0.0:
                    allowed = permitted_leg_size(
                        leg.side is Side.BUY,
                        float(state.positions.get(market, 0.0) or 0.0),
                        leg.size,
                        self._curfew.long_cap_usd / oracle,
                        self._curfew.short_cap_usd / oracle)
                    # Whole contracts: lot_size is 1 on every market, and a
                    # fractional remainder is a rejected batch.
                    allowed = math.floor(allowed)
                    if allowed < 1.0:
                        continue
                    if allowed < leg.size:
                        leg = type(leg)(leg.market, leg.side, leg.price,
                                        allowed, leg.reduce_only)
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
            # [review] But say WHICH refusal it was. `any_quoted` is set
            # before the profile filter runs, so a market that decide()
            # marked QUOTE and the MODE then skipped still arrives here --
            # and reporting "risk left nothing to place" for a deliberate
            # EXIT or stale-oracle decision blames risk for a posture
            # choice, which is the wrong thing to go looking at when the
            # book is empty and nobody knows why.
            mode_skips = [m for m, (a, r) in results.items()
                          if a in ("skip", "withdraw")
                          and profile_by_market.get(m) is not None
                          and not profile_by_market[m].quote]
            if mode_skips and len(mode_skips) == len(
                    [m for m, (a, _) in results.items()
                     if a in ("skip", "withdraw")]):
                return TickResult("withdraw",
                                  results[mode_skips[0]][1], results)
            return TickResult("risk_blocked",
                              "risk left nothing to place", results)

        # [BATCHBREAKER] A batch that keeps coming back rejected is not a
        # transient. Re-sending it every 5s does not fix it, and on this
        # venue each attempt still PLACES legs best-effort -- so the loop
        # accumulates orders it has decided not to believe in. Past the
        # streak limit, probe at a bounded rate instead and say so loudly.
        if self._batch_fail_streak >= BATCH_FAIL_STREAK_LIMIT:
            if now_s < self._batch_muted_until_s:
                return TickResult(
                    "error",
                    "batch rejected %d times running; probing once per %.0fs "
                    "instead of re-sending every tick"
                    % (self._batch_fail_streak, BATCH_PROBE_INTERVAL_S),
                    results,
                    error="batch breaker open")
            self._batch_muted_until_s = now_s + BATCH_PROBE_INTERVAL_S
            _log.critical(
                "permuto: batch has been rejected %d times running -- "
                "probing once now. The venue may be refusing the SHAPE of "
                "this batch (it accepts legs best-effort even when the "
                "batch status is a failure), so every attempt can leave "
                "orders behind. Check open orders at the venue.",
                self._batch_fail_streak)

        # [PREFLIGHT] Re-anchor to the oracle as it is RIGHT NOW, not as
        # it was when this tick started pricing. One out-of-band leg 400s
        # the WHOLE batch, so a leg we decline to send is strictly cheaper
        # than the rejection it would have caused.
        #
        # [review round 3] The first pass and the post-cancel second pass
        # now share ONE helper. They were separate code and the copy
        # drifted immediately: the second pass quantised at the default
        # tick instead of the market published one, and kept the original
        # size after re-pricing. Two passes that must agree should not be
        # two pieces of code.
        send_refs = {}

        def _fetch_refs():
            # (prices, elapsed). {} on failure -- and the elapsed time is
            # folded into the latency estimate either way.
            #
            # [review] A failed fetch can burn the whole request timeout.
            # The earlier version left _send_latency_s untouched on that
            # path and then priced off the OLDER tick oracle, so preflight
            # ignored a delay it had just incurred -- exactly when the
            # market is moving fast enough to make the fetch slow.
            t0 = time.perf_counter()
            try:
                got = self._oracle_fetch()
            except Exception as exc:  # noqa: BLE001
                _log.debug("permuto: pre-send oracle fetch failed (%s)", exc)
                got = None
            elapsed = time.perf_counter() - t0
            self._send_latency_s = (0.7 * self._send_latency_s
                                    + 0.3 * elapsed)
            return (got if isinstance(got, dict) and got else {}), elapsed

        def _prepare(pending, sources):
            # Re-anchor legs against `sources`, newest reference first.
            # Returns (kept, dropped_markets).
            #
            # EVERY retained leg is quantised on its own market grid, not
            # only the ones this pass re-priced: an earlier band-guard
            # clamp can already have left an off-grid boundary, and
            # build_upsert_batch does not validate tick alignment.
            kept_legs, dropped = [], set()
            for leg in pending:
                ref = 0.0
                for src in sources:
                    ref = latest_oracle(src, None, leg.market)
                    if ref > 0.0:
                        break
                if ref <= 0.0:
                    kept_legs.append(leg)
                    continue

                raw_specs = flags.get("specs")
                mspec = (raw_specs.get(leg.market, {})
                         if isinstance(raw_specs, dict) else {})
                m_tick = float(mspec.get("tick_size", 0.0001) or 0.0001)
                m_lot = float(mspec.get("lot_size", 1.0) or 1.0)

                out = preflight_leg_price(
                    leg.price, ref,
                    band_pct=VENUE_BAND_PCT,
                    latency_s=self._send_latency_s,
                    velocity_pct_per_s=self._band_guard.velocity(leg.market),
                    is_buy=leg.side is Side.BUY,
                    ring_pct=self._ring_pct)
                if out.dropped:
                    results[leg.market] = ("skip", out.reason)
                    dropped.add(leg.market)
                    continue

                price = quantise_toward(
                    out.price if out.changed else leg.price, ref, m_tick)
                if price <= 0.0:
                    results[leg.market] = (
                        "skip", "no on-grid price inside the band")
                    dropped.add(leg.market)
                    continue
                size = (rescaled_size(leg.size, leg.price, price, m_lot)
                        if price != leg.price else leg.size)
                if size <= 0.0:
                    results[leg.market] = ("skip", "size rounds to nothing")
                    dropped.add(leg.market)
                    continue
                if price != leg.price or size != leg.size:
                    leg = type(leg)(leg.market, leg.side, price, size,
                                    leg.reduce_only)
                send_refs[leg.market] = ref
                kept_legs.append(leg)
            return kept_legs, dropped

        def _retract(markets, note):
            # True when the book is safe to build on. A failed retraction
            # leaves an unsafe quote resting, and sending siblings beside
            # it is the state stand-down exists to prevent.
            try:
                self._client.cancel_all(now_s, sorted(markets))
            except Exception as exc:  # noqa: BLE001
                _log.warning("permuto: could not retract %s (%s): %s",
                             sorted(markets), note, exc)
                return False
            for market in markets:
                self._resting[market] = RestingQuote()
            return True

        fresh_oracles = {}
        if self._oracle_fetch is not None and legs:
            fresh_oracles, _ = _fetch_refs()
            legs, preflight_cancel = _prepare(legs, (fresh_oracles, oracles))

            if preflight_cancel:
                if not _retract(preflight_cancel, "first pass"):
                    return TickResult(
                        "skip",
                        "unsafe quote still resting; retraction failed",
                        results)

                # That cancel was an authenticated round trip, so the
                # survivors have aged. Re-read and re-price them. If the
                # read is unavailable we do NOT fall back to pre-cancel
                # references -- that is the very ageing this exists to
                # avoid.
                if legs:
                    again, _ = _fetch_refs()
                    if not again:
                        return TickResult(
                            "skip",
                            "post-cancel oracle read unavailable; not "
                            "sending on pre-cancel prices",
                            results)
                    # Newest first: second read, first pre-send read, tick.
                    legs, dropped2 = _prepare(
                        legs, (again, fresh_oracles, oracles))
                    if dropped2 and not _retract(dropped2, "second pass"):
                        return TickResult(
                            "skip",
                            "unsafe quote still resting after the second "
                            "pass", results)

            if not legs:
                return TickResult(
                    "skip",
                    "every leg would have been refused by the oracle band",
                    results)

        send_oracles = dict(oracles or {})
        # Only the per-leg references preflight actually validated, never a
        # raw merge: a fetch carrying a junk value for one market would
        # otherwise poison the validator for that market.
        try:
            send_oracles.update(send_refs)
        except NameError:                       # no legs -> no preflight pass
            pass
        # [DEPTHSIGNAL] What this batch is worth in eligibility terms,
        # computed locally, BEFORE it is sent.
        #
        # The venue's own depth_seconds counter cannot serve as the feedback
        # signal: sampled every 60s on 2026-08-31 during the cash session it
        # was byte-identical for ALL 39 market makers across three minutes,
        # while total_pnl moved on every sample. It is published on a
        # coarse, batched cadence, so "did that tick earn anything?" is
        # unanswerable from the leaderboard at tick resolution.
        #
        # depth_credit_usd() is the same min(bid, ask)-inside-the-ring rule
        # the venue credits, so a zero here means this tick banks nothing --
        # which is precisely the one-sided-book failure the note below
        # describes and which was, until now, completely silent.
        # [review 2026-08-31, live] The FIRST version of this measured the
        # outgoing batch, before the send. That was worse than no signal at
        # all: it logged "$1200/s" on ticks whose batch then 400'd in its
        # entirety and rested nothing, so the instrument reported intent as
        # achievement and disagreed with the venue for 46 minutes straight
        # while the leaderboard sat flat at 4,093.892. Measured after the
        # fact: 38 whole-batch 400s in ~14 minutes, every one of them
        # banking exactly zero while the log claimed otherwise.
        #
        # So credit is computed from what the venue ACCEPTED, below, and a
        # leg the venue refused contributes nothing here -- because it
        # contributes nothing there.
        def _credit_for(kept):
            total = 0.0
            for mkt in {leg.market for leg in kept}:
                ref = send_oracles.get(mkt) or 0.0
                if ref <= 0.0:
                    continue
                try:
                    total += depth_credit_usd(
                        [l for l in kept if l.market == mkt
                         and l.side is Side.BUY],
                        [l for l in kept if l.market == mkt
                         and l.side is Side.SELL],
                        ref, ring_pct=self._ring_pct)
                except ValueError:
                    # A leg whose side disagrees with its book is a builder
                    # bug, not a reason to lose the whole tick's send.
                    _log.debug("permuto: depth credit skipped for %s", mkt)
            return total

        payload = build_upsert_batch(legs, send_oracles,
                                     ring_pct=self._ring_pct)
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
        if isinstance(response, dict):
            status = str(response.get("status", "")).lower()
            if (status in BATCH_ACCEPTED
                    or (status not in BATCH_REJECTED
                        and _legs_all_accepted(response.get("results")))):
                if self._batch_fail_streak:
                    _log.info("permuto: batch accepted again after %d "
                              "rejection(s)", self._batch_fail_streak)
                self._batch_fail_streak = 0
                self._batch_muted_until_s = 0.0
            else:
                self._batch_fail_streak += 1
            # [live 2026-08-29] The venue's REAL vocabulary, captured on the
            # first accepted batch: 'batch_partial' (and by symmetry
            # 'batch_ok') with a per-leg results list, plus the note "Batch
            # upsert is best-effort; each leg is modify-or-place
            # independently". A partial is NORMAL -- tonight's live case is
            # an ALO ask rejected because a bid rests 2% above the oracle,
            # which is the add-liquidity-only guard doing its job and worth
            # a retry every tick, not a failed tick. Accepted legs are NOT
            # recorded from the response (rows do not echo the side);
            # reconcile() heals _resting from open_orders next tick, which
            # also keeps re-sending the rejected leg until it rests.
            leg_rows = response.get("results")
            accepted = status in BATCH_ACCEPTED
            if (not accepted and status not in BATCH_REJECTED
                    and _legs_all_accepted(leg_rows)):
                # Unknown envelope, clean legs: believe the legs.
                if status not in self._unknown_ok_statuses:
                    self._unknown_ok_statuses.add(status)
                    _log.warning(
                        "permuto: batch status %r is not in the known "
                        "accepted set %r, but every leg reports the venue "
                        "acted on it -- treating as success. Add it to "
                        "BATCH_ACCEPTED.", status, BATCH_ACCEPTED)
                accepted = True

            # [DEPTHSIGNAL] Only the legs the venue did not refuse. Rows are
            # positional against `legs` (order_count matches the results
            # length on every capture so far); if that ever stops holding,
            # the zip truncates rather than mispairing, which understates
            # the credit -- the safe direction for a number whose whole job
            # is to stop us believing we are earning when we are not.
            if not accepted:
                rested = []
            elif isinstance(leg_rows, list) and leg_rows:
                rested = [leg for leg, row in zip(legs, leg_rows)
                          if isinstance(row, dict)
                          and not row.get("rejection_reason")
                          and str(row.get("action", "")).lower()
                          in ("placed", "modified", "unchanged")]
            else:
                rested = list(legs)
            # [ANTICROSS] Feed the venue's own refusals back into the
            # placement. Rows do not echo the side, so this is per-market:
            # if ANY leg here crossed, the market retreats; if none did, it
            # relaxes back toward the configured spread.
            if isinstance(leg_rows, list) and leg_rows:
                crossed = set()
                for leg, row in zip(legs, leg_rows):
                    if not isinstance(row, dict):
                        continue
                    if "cross the book" in str(
                            row.get("rejection_reason") or "").lower():
                        crossed.add(leg.market)
                for mkt in {l.market for l in legs}:
                    if mkt in crossed:
                        self._cross_backoff.observe_cross(
                            mkt,
                            # [review] The spread we USED this tick, not the
                            # configured one: budgeting ring headroom against
                            # a narrower spread than the ladder actually
                            # placed lets the backoff hand back room the
                            # widened quote has already spent.
                            headroom_pct(self._ring_pct,
                                         self._eff_half_spread_by_market.get(
                                             mkt, self._eff_half_spread),
                                         self._last_skew.get(mkt, 0.0)))
                    else:
                        self._cross_backoff.observe_clean(mkt)

            credit_usd = _credit_for(rested)
            if legs and credit_usd <= 0.0:
                _log.warning(
                    "permuto: this tick banked ZERO depth -- %d leg(s) sent, "
                    "%d accepted, no market two-sided inside the %.1f%% "
                    "ring. Eligibility accrues on min(bid, ask), so a "
                    "one-sided or rejected book banks nothing however large.",
                    len(legs), len(rested), self._ring_pct)
            elif now_s - self._depth_logged_at_s >= DEPTH_LOG_INTERVAL_S:
                self._depth_logged_at_s = now_s
                _log.info("permuto: RESTED depth credit $%.0f/s across %d "
                          "market(s) (%d of %d legs accepted)", credit_usd,
                          len({l.market for l in rested}), len(rested),
                          len(legs))

            if accepted and isinstance(leg_rows, list):
                if status == "batch_partial" and not leg_rows:
                    # "partial" with no per-leg detail is unverifiable --
                    # the round-11 rule stands: never record legs as
                    # resting off a status that admits some failed.
                    _log.warning(
                        "permuto: batch_partial with no results detail; "
                        "believing open_orders over our own send")
                    return TickResult(
                        "error",
                        "batch_partial with no per-leg results",
                        results,
                        error="batch status 'batch_partial'")
                rejected = []
                for row in leg_rows:
                    if not isinstance(row, dict):
                        continue
                    reason = row.get("rejection_reason")
                    if reason or str(row.get("action", "")).lower() == "rejected":
                        # [live] Rejected rows do not echo the market.
                        rejected.append("%s: %s"
                                        % (row.get("market") or "leg",
                                           reason))
                if rejected and len(rejected) >= len(leg_rows):
                    return TickResult(
                        "error",
                        "every batch leg rejected: " + "; ".join(rejected),
                        results,
                        error="all legs rejected")
                if rejected:
                    note = "; ".join(rejected)
                    if note != self._last_leg_rejections:
                        self._last_leg_rejections = note
                        _log.info(
                            "permuto: batch accepted with rejected leg(s) "
                            "-- %s -- retrying each tick", note)
                    return TickResult(
                        "quote",
                        "quoting (leg(s) pending: %s)" % note,
                        results)
                self._last_leg_rejections = ""
                # All legs clean: fall through to record them as resting.
            elif status and status not in ("ok", "success", "accepted"):
                # [review round 11] A non-clean status is a FAILED tick, not
                # an annotated success. Recording every requested leg as
                # resting and returning ok meant a venue answering "partial"
                # forever kept the toolbar ON while one side never rested.
                # _resting is left untouched; the next open_orders
                # reconciliation establishes what was actually accepted.
                _log.warning(
                    "permuto: batch_upsert returned status %r (body %r); "
                    "believing open_orders over our own send",
                    # [diagnostic 2026-08-31] 400 chars truncated the body
                    # before the per-leg rejection_reason fields, which is
                    # exactly where the explanation for a repeating
                    # 'batch_failed' lives. Widened to capture one whole
                    # response; narrow again once the cause is known.
                    status, str(response)[:4000])
                return TickResult(
                    "error",
                    "batch status %r -- legs not recorded as resting; "
                    "reconciling from open_orders next tick" % status,
                    results,
                    error="batch status %r" % status)

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
                size = float(row.get("size", row.get("position", 0.0)))
            except (TypeError, ValueError):
                positions[market] = float("nan")
                continue
            # [live 2026-08-31] SIGN THE SIZE. The venue reports a position
            # as {"side": "sell", "size": "812520"} -- an UNSIGNED magnitude
            # plus a direction. Reading `size` alone recorded an 812,520
            # contract SHORT as a +812,520 LONG, and every risk control
            # downstream then ran inverted:
            #
            #   * assess() saw a huge long and returned REDUCE_ONLY;
            #   * REDUCE_ONLY keeps the leg that shrinks a LONG -- the ASK;
            #   * each of those asks GREW the real short, and the skew for a
            #     phantom long priced them aggressively below the oracle,
            #     which is why 156 consecutive rejections were "Aggressive
            #     ask" and not one was a bid.
            #
            # The loop spent the session enlarging the position it believed
            # it was unwinding. Absent or unrecognised side keeps the raw
            # magnitude rather than guessing a direction.
            side = str(row.get("side", "")).strip().lower()
            if side in ("sell", "short", "s", "ask"):
                size = -abs(size)
            elif side in ("buy", "long", "b", "bid"):
                size = abs(size)
            elif size != 0.0:
                # [review] FAIL CLOSED on an unrecognised direction. An
                # earlier version kept the raw magnitude, which is
                # "default to long" wearing a modest hat -- the exact
                # assumption that turned an 812,520 short into a phantom
                # long. A non-zero size we cannot orient is unreadable
                # inventory, and assess() already treats that as FLATTEN.
                size = float("nan")
            positions[market] = size

    # [live 2026-08-29] The venue's /exchange/account payload, observed on
    # the first authenticated tick ever (every earlier attempt 422'd on
    # user_id), carries NONE of the names we guessed:
    #   {"balance": "500000", "locked_margin": "0",
    #    "locked_order_margin": "0", "positions": [],
    #    "total_realized_pnl": "0", "total_unrealized_pnl": "0", ...}
    # equity = balance + unrealized PnL; used = locked_margin +
    # locked_order_margin. The guessed names stay first for forward compat.
    equity, equity_present = _num_present(
        account, "equity_usd", "equity", "account_value")
    if not equity_present:
        bal, bal_present = _num_present(account, "balance")
        if bal_present:
            upnl, _ = _num_present(account, "total_unrealized_pnl")
            equity = bal + upnl
            equity_present = True
    used, used_present = _num_present(
        account, "used_margin_usd", "used_margin", "margin_used")
    if not used_present:
        lm, lm_present = _num_present(account, "locked_margin")
        lom, lom_present = _num_present(account, "locked_order_margin")
        if lm_present and lom_present:
            # Both halves or neither: a partly-readable pair falls through
            # to the fail-closed branch below.
            used = lm + lom
            used_present = True
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
