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
from .risk import (PORTFOLIO_MAX_EXPOSURE_FRACTION, MarginState,
                   RiskAction, assess, portfolio_cap_usd,
                   skewed_reference)
from .band_guard import VENUE_BAND_PCT, BandGuard
from .bbo import (placement_prices as bbo_placement_prices,
                  required_ladder_offset_pct as bbo_required_offset_pct,
                  rests_and_earns as bbo_rests_and_earns,
                  _quantise as bbo_quantise)
from .cross_backoff import CrossBackoff, headroom_pct
from .preflight import (latest_oracle, preflight_leg_price,
                        quantise_toward, rescaled_size)
from .curfew import (OPENS_UTC, OracleFreeze, Stage, assess_curfew,
                     permitted_leg_size)
from .modes import Profile, profile_for, uncurfewed_profile
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
                          skew_frac_abs: float, tick_frac: float) -> float:
    """The most backoff this leg can carry and still rest.

    [review] Bounded by BOTH constraints, because satisfying one and
    failing the other still costs the quote:

      * the credit RING -- outside it the leg earns nothing, so retreating
        past it turns a rejected leg into a resting worthless one;
      * the RE-QUOTE TRIGGER -- decide() replaces any leg further than
        ring * REQUOTE_AT_RING_FRACTION from the oracle, so a leg born
        past it is cancelled and replaced every tick, forever.

    Computed from the CURRENT skew rather than whatever held when the
    offset was learned. An offset learned while flat is not legal after a
    fill: 1.607% at oracle 0.07 becomes a 2.857% ask once skew reaches
    0.95%, outside the ring entirely.
    """
    ring_room = headroom_pct(ring_pct, half_spread_pct, skew_frac_abs,
                             tick_frac)
    trigger_budget = ring_pct * REQUOTE_AT_RING_FRACTION * 0.8
    trigger_room = (trigger_budget - abs(half_spread_pct)
                    - abs(skew_frac_abs) * 100.0)
    return max(0.0, min(ring_room, trigger_room))


def _effective_tick(raw, default: float = 0.0001) -> float:
    """The tick quote_ladder will actually use, decided once.

    [review] Two callers derived this independently and disagreed on junk:
    `float(raw or default)` passes NaN straight through, because NaN is
    truthy, while quote_ladder validates and falls back. So a non-finite
    tick_size disabled the crossing backoff -- headroom of zero clears the
    learned offset -- while the ladder priced on 0.0001 as if nothing were
    wrong. Same value to both, or they will drift again.
    """
    try:
        tick = float(raw)
    except (TypeError, ValueError):
        return default
    if not math.isfinite(tick) or tick <= 0.0:
        return default
    return tick


def _is_cross_refusal(reason: str) -> bool:
    """True when the venue refused this leg for crossing the book.

    [review] Matched on the STABLE markers, not the full sentence. The live
    venue sends "Post-only order would cross the book. Switch to GTC or
    adjust price." but this repo's own fixtures carry the shorter
    "post-only order would cross", and an exact-phrase match on "cross the
    book" silently skipped those -- so a refusal took the CLEAN path and
    decayed the backoff instead of widening it, which is precisely
    backwards.
    """
    return "post-only" in reason and "cross" in reason


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
        # [DEPTH 2026-09-02] TRIED 0.05 AND REVERTED -- recorded so it is not
        # retried. Cutting this does buy ring headroom (1.700% -> 1.900% at
        # zero skew), but one tick is 0.14-0.28% of oracle on these markets,
        # which is COARSER than the spread itself: at 0.05 the session and
        # overnight placements both quantise to the same single tick and
        # test_the_session_quotes_wider_than_the_overnight_window fails --
        # correctly, because the 1.6x session widening stops existing. The
        # 0.2% of headroom it buys does not close a ~1.5% gap anyway; skew is
        # the term that matters. Width now comes from the ring-edge rule in
        # _bbo_offset_pct instead, which is quantisation-proof.
        half_spread_pct: float = 0.25,
        quote_when_carried: bool = True,
        oracle_fetch: Any = None,
        #: [BBO] Optional ``market -> bbo.Book | None``. When absent the
        #: runner behaves exactly as before, learning the resting price from
        #: refusals alone. When supplied, the book is CONSULTED instead of
        #: guessed: see the note at the placement site for why that matters.
        bbo_fetch: Any = None,
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
        #: Tick size as a fraction of the oracle, per market, so headroom_pct
        #: can reserve the one-tick rounding margin ask/ceil adds.
        self._last_tick_frac: dict = {}
        #: A quote forced wide by the observed BBO must not be born past the
        #: ordinary drift trigger and replaced every tick. Recorded only
        #: after the venue accepts the pair; bounded by the scoring ring.
        self._requote_at_pct: dict = {}
        #: Markets currently one-sided because a side's room ran out.
        #: Distinct from a curfew-closed side: the cap is positive,
        #: the inventory has simply consumed all of it.
        self._pinned_markets: set = set()
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
        self._bbo_fetch = bbo_fetch
        #: Measured duration of the last pre-send fetch: the best estimate
        #: of how long the NEXT request will take.
        self._send_latency_s = 0.25
        self._curfew_retract_pending = False
        self._curfew_enabled = curfew_enabled
        self._freeze = OracleFreeze()
        self._curfew_stage = None
        #: (cap stage, schedule stage). Both, because with no cap
        #: configured the first never changes while the second drives
        #: the quoting profile.
        self._curfew_stage_key = None
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
    def _bbo_offset_pct(self, market: str, oracle: Any, reference: Any,
                        tick_size: float, eff_half_spread: float,
                        learned_backoff: float, ring_backoff: float):
        """``(status, (bid_price, ask_price), book)`` from the live book.

        ``status`` is one of:

        * ``"ok"``      -- exact per-side prices exist inside the ring.
        * ``"shut"``    -- no tick-aligned price rests and earns on at least
                           one side. Depth credit is ``min(bid, ask)``, so a
                           market that cannot quote both sides earns nothing
                           whatever we send: skip it rather than spend a
                           rate-limit token on a refusal.
        * ``"reset"``   -- our own resting order is the BBO that closes the
                   other side. Cancel it once so the next tick sees
                   the external book and can rebuild both sides.
        * ``"unknown"`` -- the book could not be read. Fall back to the
                           learned backoff; a monitoring failure must never
                           be the reason a quote stops going out.

        Prices are independent because inventory skew moves the ladder center.
        A single symmetric offset can cross on one side or leave the scoring
        ring on the other even when both side-specific windows are open.
        """
        try:
            oracle_f = float(oracle or 0.0)
            reference_f = float(reference or 0.0)
        except (TypeError, ValueError):
            return "unknown", 0.0, None
        if not (oracle_f > 0.0 and reference_f > 0.0 and tick_size > 0.0):
            return "unknown", 0.0, None
        try:
            book = self._bbo_fetch(market)
        except Exception as exc:  # noqa: BLE001 - never fail the tick on this
            _log.debug("permuto: %s BBO fetch raised: %s", market, exc)
            return "unknown", 0.0, None
        if book is None:
            return "unknown", 0.0, None

        resting = self._resting.get(market, RestingQuote())
        needed = 0.0
        for side in ("ask", "bid"):
            off = bbo_required_offset_pct(
                side, oracle_f, reference_f, book,
                ring_pct=self._ring_pct, tick_size=tick_size,
                allow_subtick=True)
            if off is None:
                blocker = (book.best_bid if side == "ask"
                           else book.best_ask)
                own_blocker = (resting.bid_price if side == "ask"
                               else resting.ask_price)
                tolerance = tick_size * 1e-4
                own_is_blocker = (
                    blocker is not None
                    and own_blocker is not None
                    and (own_blocker >= blocker - tolerance
                         if side == "ask"
                         else own_blocker <= blocker + tolerance)
                )
                if own_is_blocker:
                    _log.info(
                        "permuto: %s %s side is blocked by our own resting "
                        "%s at %.6f -- cancelling the one-sided book once "
                        "so the next tick can rebuild against the external "
                        "BBO.", market, side,
                        "bid" if side == "ask" else "ask", blocker)
                    return "reset", 0.0, book
                _log.info(
                    "permuto: %s %s side has no placeable price -- best bid "
                    "%s / best ask %s against a %.1f%% ring. Skipping: depth "
                    "is min(bid, ask), so one side alone banks nothing.",
                    market, side, book.best_bid, book.best_ask,
                    self._ring_pct)
                return "shut", 0.0, book
            needed = max(needed, off)

        if needed <= eff_half_spread:
            # The configured spread already clears the book -- so go WIDE
            # rather than resting where we happen to be.
            #
            # [DEPTH 2026-09-02] depth_credit_usd counts a leg's full notional
            # anywhere inside the ring and gives nothing extra for being tight,
            # so a leg at the ring edge earns exactly what a leg at 0.05%
            # earns. What differs is the fill rate, and a fill is not neutral:
            # it flattens one side, and credit is min(bid, ask), so the market
            # banks ZERO until the restore lands. Width is therefore free
            # eligibility bought with fewer holes in the book -- and less of
            # the inventory that consumed the ring headroom to begin with.
            preferred = eff_half_spread + max(ring_backoff, 0.0)
        else:
            preferred = max(
                needed, eff_half_spread + max(learned_backoff, 0.0))
        prices = bbo_placement_prices(
            oracle_f, reference_f, book,
            preferred_offset_pct=preferred,
            ring_pct=self._ring_pct, tick_size=tick_size)
        if prices is None:
            _log.info(
                "permuto: %s has open side windows but no valid two-sided "
                "grid around reference %.6f; skipping rather than banking "
                "zero.", market, reference_f)
            return "shut", 0.0, book
        return "ok", prices, book

    def _forget_book(self) -> None:
        self._resting = {m: RestingQuote() for m in self._markets}
        self._requote_at_pct.clear()

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
                prev_bid = seen[market]["bid"]
                seen[market]["bid"] = price if prev_bid is None else max(prev_bid, price)
            elif side in ("SELL", "ASK", "S"):
                prev_ask = seen[market]["ask"]
                seen[market]["ask"] = price if prev_ask is None else min(prev_ask, price)

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
        # Freeze observation and schedule assessment are unconditional to
        # provide ground truth for carried-session detection and single-market
        # quietness regardless of whether inventory caps are enabled.
        self._freeze.observe(now_s, oracles)
        schedule_curfew = assess_curfew(
            now_s, self._max_position_usd if self._curfew_enabled else 0.0,
            frozen_oracle=self._freeze.frozen(now_s))

        if self._curfew_enabled:
            curfew = schedule_curfew
            stage_key = (curfew.stage, curfew.schedule_stage)
            if stage_key != self._curfew_stage_key:
                _log.warning("permuto: inventory curfew %s -> %s: %s "
                             "(long $%.0f / short $%.0f of $%.0f)",
                             getattr(self._curfew_stage, "value", "none"),
                             curfew.stage.value, curfew.reason,
                             curfew.long_cap_usd, curfew.short_cap_usd,
                             self._max_position_usd)
                # [review] RETRACT THE BOOK THE NEW STAGE NO LONGER ALLOWS.
                self._curfew_retract_pending = True
            self._curfew = curfew
        else:
            self._curfew = None

        paused = bool(flags.get("trading_paused"))
        carried = bool(flags.get("carried") or flags.get("carried_session"))
        curfew_stages = {schedule_curfew.stage, schedule_curfew.schedule_stage}
        if Stage.CLOSED in curfew_stages or Stage.PREOPEN in curfew_stages:
            # /info/meta keeps VOL markets "active" while their equity
            # oracle is carried, and /info/oracle publishes no carried bit.
            # The schedule/freeze state computed above is therefore the only
            # production signal for the venue's 8x stressed-margin regime.
            carried = True

        venue_ring = flags.get("ring_pct")
        if venue_ring is not None:
            try:
                v_ring = float(venue_ring)
                if math.isfinite(v_ring) and 0.0 < v_ring <= VENUE_BAND_PCT:
                    self._ring_pct = v_ring
                else:
                    _log.debug("permuto: ring_pct %r outside (0, %.1f]; retaining %.1f",
                               venue_ring, VENUE_BAND_PCT, self._ring_pct)
            except (TypeError, ValueError) as exc:
                # Invalid ring_pct flag; retain current self._ring_pct
                _log.debug("permuto: invalid ring_pct flag: %r, retaining %.1f (%s)",
                           venue_ring, self._ring_pct, exc, exc_info=True)

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
        # exactly the case that breaks: the venue pauses before the contest
        # open on Sunday evening and un-pauses at the 09:30 ET open --
        # fourteen-odd hours in which the session would have expired
        # unrenewed. The first tick after the open would then spend a full
        # challenge/sign/auth round trip before it could place anything,
        # at the exact moment every entrant reconnects at once, on a metric
        # that only accrues while quoting. If that reauth failed we would
        # be backing off through the open.
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
            # [audit] ...and a side pinned by EXHAUSTED ROOM is just as
            # closed as one pinned by a zero cap. Overnight flow is
            # one-directional -- buyers lift the ask, nothing sells back
            # against a frozen oracle -- so the short room drains and
            # the ask is dropped while short_cap_usd is still POSITIVE.
            # This test only saw the zero-cap case, so decide() read the
            # missing ask as a repairable gap and returned QUOTE every
            # tick: measured at ~12,600 authenticated cancel+upsert
            # pairs across one overnight session, each replacing a quote
            # with an identical copy of itself, on a rate-limited route.
            one_sided_ok = bool(
                self._curfew is not None
                and (self._curfew.short_cap_usd <= 0.0
                     or self._curfew.long_cap_usd <= 0.0
                     or market in self._pinned_markets))
            call = decide(
                view, self._resting.get(market, RestingQuote()),
                ring_pct=self._ring_pct,
                quote_when_carried=self._quote_when_carried,
                one_sided_ok=one_sided_ok,
                requote_at_pct=self._requote_at_pct.get(market),
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
        retracted = False
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
                retracted = True
                # [review] AND EVERY HOLD IS NOW STALE. `results` was
                # decided against the book that existed a moment ago,
                # and HOLD means "what is resting is fine" -- but
                # nothing is resting any more. The earlier fix only
                # covered the case where EVERY market held, because it
                # hung off `not any_quoted`; with one market QUOTE and
                # another HOLD the loop rebuilt the first, left the
                # second empty for a tick, and still reported "quote".
                #
                # Rebuilding immediately beats waiting a tick: the
                # book is empty either way, and a tick of it is a tick
                # of no depth on every market at once.
                for market, (action, _reason) in list(results.items()):
                    if action == LoopAction.HOLD.value:
                        results[market] = (
                            LoopAction.QUOTE.value,
                            "book retracted for a curfew stage change; "
                            "rebuilding under the new posture")
                        any_quoted = True
                if self._curfew is not None:
                    self._curfew_stage = self._curfew.stage
                    self._curfew_stage_key = (
                        self._curfew.stage, self._curfew.schedule_stage)

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
        else:
            scheduled = schedule_curfew.schedule_stage
            posture_stage = (scheduled
                             if scheduled not in (None, Stage.UNSCHEDULED)
                             else schedule_curfew.stage)
        # Stage-level default; market-specific posture can tighten from here.
        oracle_fresh_agg = not self._freeze.frozen(now_s)
        default_profile = (
            profile_for(
                posture_stage,
                oracle_fresh=oracle_fresh_agg,
            )
            if (self._curfew_enabled or not oracle_fresh_agg)
            else uncurfewed_profile()
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
                # [audit] BOTH questions, not just the first. "Printed
                # since the bell" alone is satisfied forever by a single
                # print: a market that ticks once at 09:31 and then
                # freezes stayed quotable for the whole settle window,
                # which is the same stale-price trap the gate exists to
                # close, just arriving fifteen minutes later.
                opened = [o for o in OPENS_UTC if o <= now_s]
                fresh = (bool(opened)
                         and self._freeze.changed_since(market, max(opened))
                         and not self._freeze.market_gone_quiet(market, now_s))
            else:
                # [review] gone_quiet, not market_frozen. The gate must
                # outlive SETTLING -- the aggregate detector keeps the
                # curfew in SESSION while ANY market prints, so a
                # neighbour that stops is quoted against its own stale
                # price indefinitely -- but an UNSEEN market must not be
                # treated as stale, or the loop refuses to quote anything
                # until it has watched a second distinct value arrive.
                fresh = not self._freeze.market_gone_quiet(market, now_s)
            if not fresh:
                market_profile = profile_for(posture_stage, oracle_fresh=False)
            else:
                market_profile = (
                    profile_for(posture_stage, oracle_fresh=True)
                    if self._curfew_enabled else uncurfewed_profile()
                )
            profile_by_market[market] = market_profile
            eff_half_spread_by_market[market] = (
                self._half_spread_pct * market_profile.spread_mult
            )
        self._eff_half_spread_by_market = eff_half_spread_by_market

        risk_by_market: dict = {}
        #: What a risk-INCREASING leg may still add, book-wide.
        #:
        #: [review] Handing the budget to assess() as max_position bounds
        #: the POSITION limit, not new exposure: with the budget spent by
        #: neighbours and this market FLAT, abs(0) >= 1e-9 is false, so
        #: assess() answers NORMAL and both risk-increasing legs go out at
        #: full size. A budget that only binds once you already hold
        #: something is not a budget.
        #:
        #: Headroom is a property of the BOOK, so it is the total against
        #: the budget -- NOT the per-market cap. Clamping a leg to the
        #: per-market cap instead would re-apply the position limit as an
        #: order-size limit and break the equal-notional pairing that
        #: min(bid, ask) depends on.
        portfolio_headroom_usd = None
        #: Markets that cannot be two-sided this tick -- reduce-only, or
        #: a side whose room ran out. Either way the market earns zero.
        newly_pinned: set = set()
        # [audit] Notional per market, for the PORTFOLIO cap below.
        # max_position_usd is per-market and nothing aggregated it, so
        # three markets at the shipped 250,000 authorised 750,000 of
        # exposure on a 500,000 account. No single market has to breach
        # its own limit for the book to breach the account.
        positions_usd: dict = {}
        # [review] EVERY position the account reports, not just the
        # markets we quote. MarginState.positions comes from
        # /exchange/account and QuoteRunner can be built with a SUBSET,
        # so exposure in an unconfigured market -- or one held manually
        # -- was invisible to the budget and it authorised that much
        # again. A budget that only sees its own book is not a budget.
        for market in set(self._markets) | set(state.positions or {}):
            oracle = oracles.get(market)
            try:
                contracts = abs(float(
                    state.positions.get(market, 0.0) or 0.0))
            except (TypeError, ValueError):
                contracts = float('nan')
            # [review] A FLAT market is worth zero at ANY price, so it
            # must not need an oracle to be valued. Marking it NaN made
            # every unpriced market -- including the ones we simply are
            # not quoting this tick -- fail the whole budget closed and
            # drop every risk-increasing leg in the book. Only a
            # NON-ZERO position we cannot price is genuinely unreadable.
            if contracts == 0.0:
                positions_usd[market] = 0.0
            else:
                positions_usd[market] = (
                    contracts * float(oracle)
                    if oracle and oracle > 0.0 else float('nan'))

        _total = 0.0
        for _value in positions_usd.values():
            if _value != _value:            # NaN: cannot value the book
                _total = float('nan')
                break
            _total += abs(_value)
        if (_total == _total and math.isfinite(state.equity_usd)
                and state.equity_usd > 0.0):
            portfolio_headroom_usd = max(
                0.0, state.equity_usd * PORTFOLIO_MAX_EXPOSURE_FRACTION
                - _total)
        else:
            # Unreadable book or equity: no room at all, which is the
            # same direction every other guard here fails in.
            portfolio_headroom_usd = 0.0
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
            # [audit] ...and then by the PORTFOLIO budget, which can only
            # tighten it further. A market is allowed its own cap only
            # to the extent the rest of the book has left room; an
            # unreadable equity or a neighbour's unreadable position
            # yields zero, because authorising exposure against a
            # number nobody can see is how the last account died.
            # min() only when there IS a per-market cap: with the
            # "no limit" sentinel (<= 0) min() would keep the zero and
            # cap the market at nothing. The budget replaces it there.
            _budgeted = portfolio_cap_usd(
                state.equity_usd, market, cap_usd, positions_usd)
            cap_usd = (min(cap_usd, _budgeted) if cap_usd > 0.0
                       else _budgeted)
            # Clamped strictly positive: the overnight SHORT cap is zero,
            # and assess() reads a non-positive max_position as "no limit".
            # The prohibition is enforced by the per-leg veto below; what
            # assess() needs here is a number that keeps an existing short
            # in REDUCE_ONLY rather than switching the limit off.
            # [review] ...but NOT when there is no configured limit at
            # all. assess() reads a non-positive max_position as "no
            # limit", which is precisely what the sentinel means, and
            # the 1e-9 clamp turned it into a limit of almost zero --
            # so every nonzero position became REDUCE_ONLY. The clamp
            # exists for a curfew-FLOORED cap, where zero would read as
            # "unlimited" and undo the curfew; an unset cap is the
            # opposite case and wants exactly that reading.
            if self._max_position_usd > 0.0:
                max_position = (max(cap_usd, 1e-9) / oracle
                                if oracle and oracle > 0.0 else 0.0)
            else:
                max_position = 0.0          # the sentinel: no limit
            # [audit] REDUCE_ONLY IS A PIN. It quotes one side only, and
            # depth credit is min(bid, ask) -- so the market earns
            # EXACTLY ZERO while it lasts. Overnight that is not a
            # passing state: recovery needs a fill on the other side,
            # and the flow that put us here is one-directional against
            # a frozen oracle. The per-tick 'banked ZERO depth' line
            # does fire, but it repeats every 5s all night and the
            # tick still reports action='quote', so the GUI shows a
            # live book. Announce the TRANSITION instead.
            _verdict = assess(
                state, market,
                base_size=base_size,
                max_position=max_position,
                ring_pct=self._ring_pct,
                half_spread_pct=eff_half_spread_by_market[market],
                # [review] The skew budget must reserve the rounding tick
                # as well, or spread + skew + ceil() lands past the
                # re-quote trigger before any backoff is even considered.
                tick_frac=(_effective_tick(
                    (flags.get("specs") or {}).get(market, {}).get(
                        "tick_size") if isinstance(flags.get("specs"), dict)
                    else None)
                    / max(float(oracle or 1e-12), 1e-12)),
            )
            risk_by_market[market] = _verdict
            if _verdict.action is RiskAction.REDUCE_ONLY:
                newly_pinned.add(market)

        # [review] RECONCILED HERE, above the not-any_quoted early
        # returns. A pinned market makes decide() answer HOLD, so the
        # tick returns before the tail -- and the pin could never be
        # CLEARED. An operator who reduced the position would find the
        # market still latched and its missing side never rebuilt, which
        # is a worse stuck state than the one this was added to report.
        #
        # [audit] Announce a market that has just run out of room, and
        # forget one that has recovered. This is the state that cost
        # ~40%% of a simulated night in silence: the book goes
        # one-sided, depth credit is min(bid, ask) so the market earns
        # exactly zero, and nothing else says so -- assess() reports
        # NORMAL because the floor leaves |position| a hair under the
        # cap. Overnight the recovery needs a BID fill, which
        # one-directional flow will not provide, so an operator who is
        # not told will find out in the morning.
        for market in sorted(newly_pinned - self._pinned_markets):
            _log.critical(
                "permuto: %s IS PINNED ONE-SIDED -- inventory has "
                "consumed the whole side cap, so this market now earns "
                "ZERO depth (credit is min(bid, ask)). It cannot "
                "recover without a fill on the other side. Reduce the "
                "position to resume earning.", market)
        for market in sorted(self._pinned_markets - newly_pinned):
            # [review] "depth resumes" is a claim about the BOOK, and
            # leaving the pin only means risk stopped forbidding a side.
            # _resting can still be empty here, and the batch that
            # rebuilds it can still be rejected -- so announcing
            # recovery now can tell an operator depth is back while the
            # market earns zero. Same mistake as measuring depth credit
            # before the venue answered: report the ACHIEVEMENT, not the
            # intention. Two-sidedness is confirmed by reconcile() or by
            # an accepted upsert, both of which land before the next
            # tick reads this.
            if self._resting.get(market, RestingQuote()).two_sided:
                _log.warning("permuto: %s is two-sided again -- depth "
                             "resumes", market)
            else:
                _log.warning("permuto: %s is no longer pinned, but no "
                             "two-sided book is resting yet -- it earns "
                             "nothing until both sides are back", market)
        self._pinned_markets = newly_pinned

        # A market holding a live quote that risk wants shrunk or gone must
        # act now, whatever decide() thought of the quote itself.
        # Only markets the quoting loop below will NOT reach. One that is
        # re-quoting already handles its own risk action there -- cancel then
        # place the shrinking side -- and pre-empting it here would cancel
        # the book and then skip the replacement, leaving the market flat
        # when it should have been reduced.
        # [review] NO EXEMPTION HERE, and the attempt is instructive.
        # A pinned market whose lone resting leg is already the
        # reduce-only shape gets cancelled and re-placed every other
        # tick, and exempting it looked free. It is not: reconcile()
        # records bid/ask PRICES and discards each order's
        # reduce_only flag and size, so nothing here can tell that
        # lone quote from an ordinary one -- and if risk later turns
        # REDUCE_ONLY, the un-replaced order can fill THROUGH flat
        # into opposite exposure. The exemption also caught FLATTEN,
        # which must retract everything by definition.
        #
        # The churn is a rate-limit problem; those two are position
        # and margin problems. Correct needs RestingQuote to carry
        # reduce_only and size -- the same rework the RAMP cap
        # revalidation needs, and it belongs with it.
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
                    "error", "quote still resting; withdrawal failed: %s"
                    % exc, results)
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
            if retracted:
                # [audit] The sibling of the `shut` case above, and the one
                # it does NOT cover. The stage-change retraction cancels
                # every order AFTER decide() has answered and after
                # any_quoted was frozen, so on a frozen oracle -- where
                # decide() says HOLD because the resting quote has not
                # drifted -- results still reads "hold" for markets whose
                # orders were cancelled seconds earlier in this same tick.
                # `shut` misses it because the posture that follows the
                # change need not be a withdrawing one: CLOSED -> PREOPEN
                # 30 minutes before each open is quote=True, so nothing
                # else rewrote the row.
                #
                # Reachable roughly once a trading day at that boundary,
                # and on the first tick after a GUI restart while a book
                # is still resting at the venue. One tick, then the next
                # sees an empty _resting and re-quotes -- but for that tick
                # the GUI is told a two-sided book is live over zero
                # orders, which is the direction this status line must
                # never be wrong in.
                reason = ("the book was retracted for a curfew stage "
                          "change; re-quoting next tick")
                for market in self._markets:
                    results[market] = ("withdraw", reason)
                return TickResult("withdraw", reason, results)
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
        bbo_resets: list = []
        bbo_placements: set = set()
        bbo_initial_prices: dict = {}
        observed_books: dict = {}
        bbo_reduce_placements: set = set()
        start_bbo_tick = getattr(self._bbo_fetch, "start_tick", None)
        if callable(start_bbo_tick):
            start_bbo_tick()

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
            # say we must. headroom_pct bounds the backoff so that the
            # quantised trailing leg stays inside the credit ring against the
            # TRUE oracle (skew and tick rounding both accounted for).
            # quote_ladder's own ring_pct*0.9 clamp is relative to the SKEWED
            # reference, not the true oracle, so it is NOT the true ceiling --
            # that is headroom_pct's job.
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
            # [review] Reserve the worst-case ask-ceil rounding margin so
            # headroom_pct accounts for the one tick quote_ladder adds --
            # from the EFFECTIVE tick, the same one the ladder will use.
            #
            # `float(spec.get(...) or 0.0001)` looked like a fallback and is
            # not one: NaN is truthy, so a non-finite tick_size sailed
            # through as NaN, made _last_tick_frac NaN, and headroom_pct
            # then returned zero -- which makes observe_cross CLEAR the
            # learned backoff. Meanwhile quote_ladder validates and quietly
            # uses 0.0001, so bad venue metadata disabled the crossing
            # defence while the quote itself carried on regardless.
            _tick = _effective_tick(spec.get("tick_size"))
            self._last_tick_frac[market] = (
                _tick / max(float(oracle or 1e-12), 1e-12))
            # [review] Capped against CURRENT headroom and the re-quote
            # trigger, not applied raw. See _requote_safe_backoff: an offset
            # learned under wider headroom is illegal after a fill raises
            # skew, and one inside the ring but past the trigger is replaced
            # on the next tick -- the churn the budget exists to stop.
            _safe_backoff = _requote_safe_backoff(
                self._ring_pct, eff_half_spread,
                self._last_skew.get(market, 0.0),
                self._last_tick_frac.get(market, 0.0))
            _ring_backoff = headroom_pct(
                self._ring_pct, eff_half_spread,
                self._last_skew.get(market, 0.0),
                self._last_tick_frac.get(market, 0.0))
            _extra_offset = min(self._cross_backoff.offset_pct(market),
                                _safe_backoff)
            _bbo_prices = None
            _observed_book = None
            # [BBO 2026-09-02] Ask the book rather than guessing at it.
            #
            # cross_backoff.py states the venue "publishes no L2/orderbook/
            # ticker route". It does -- GET /info/l2/{market} -- and the
            # consequence of assuming otherwise was total: with bids parked
            # at exactly +2.00% (the ring ceiling), the 0.25%-per-refusal
            # controller saturated at its headroom and every ask was refused
            # post-only, tick after tick, for the whole session. Zero
            # depth-seconds banked on 2026-09-02 against a 300M gate.
            #
            # One observation answers what the controller could only
            # approach, and -- more importantly -- distinguishes "retreat
            # further" from "no placeable price exists", which a blind
            # controller cannot do and so retries forever.
            if self._bbo_fetch is not None:
                _status, _observed, _observed_book = self._bbo_offset_pct(
                    market, oracle, reference, _tick, eff_half_spread,
                    _extra_offset, _ring_backoff)
                if _observed_book is not None:
                    observed_books[market] = _observed_book
                if (_status == "reset"
                        and risk.action is not RiskAction.REDUCE_ONLY):
                    to_cancel.append(market)
                    bbo_resets.append(market)
                    results[market] = (
                        "withdraw",
                        "our own one-sided quote is blocking the missing "
                        "side at the public BBO; cancelling once and "
                        "rebuilding next tick")
                    continue
                if _status in ("shut", "reset"):
                    # [review] SKIP ONLY WHEN THERE IS NOTHING ELSE TO DO.
                    #
                    # The first version of this returned unconditionally,
                    # which deadlocked the account it was written to help.
                    # A market that cannot earn depth can still need its
                    # inventory worked off, and the reduce-only branch below
                    # rests exactly one leg for that. Reducing a SHORT means
                    # resting a BID -- the side the ring wall does not touch
                    # -- so the leg that unpins us is placeable precisely
                    # when the earning leg is not.
                    #
                    # Skipping both meant no fills, so inventory never fell,
                    # so the skew never released the ring headroom, so the
                    # ask stayed unreachable: a loop that could not exit
                    # itself. Depth is min(bid, ask) and stays zero here
                    # either way; the difference is whether we are getting
                    # closer to earning again or standing still.
                    if risk.action is not RiskAction.REDUCE_ONLY:
                        if not self._resting.get(market, RestingQuote()).empty:
                            to_cancel.append(market)
                        results[market] = (
                            "skip",
                            "no tick-aligned price rests inside the %.1f%% "
                            "ring on both sides, and no inventory to work "
                            "off -- a leg here would be refused or earn "
                            "nothing" % (self._ring_pct,))
                        continue
                    _log.info(
                        "permuto: %s cannot earn depth (ring shut) but is "
                        "REDUCE_ONLY -- resting the reducing leg anyway so "
                        "inventory falls and the skew stops eating ring "
                        "headroom.", market)
                elif _status == "ok":
                    _bbo_prices = _observed
                    bbo_placements.add(market)
                    bbo_initial_prices[market] = _observed
            try:
                _lot = float(spec.get("lot_size", 1.0) or 1.0)
            except (TypeError, ValueError):
                _lot = 1.0
            if not math.isfinite(_lot) or _lot <= 0.0:
                _lot = 1.0
            ladder = quote_ladder(
                market, reference, depth_usd,
                levels=1,
                first_offset_pct=eff_half_spread + _extra_offset,
                ring_pct=self._ring_pct,
                tick_size=_tick,
                lot_size=_lot,
            )
            if _bbo_prices is not None:
                bid_price, ask_price = _bbo_prices
                placed = []
                for leg in ladder:
                    price = (bid_price if leg.side is Side.BUY else ask_price)
                    size = rescaled_size(
                        leg.size, leg.price, price, _lot)
                    if size > 0.0:
                        placed.append(type(leg)(
                            leg.market, leg.side, price, size,
                            leg.reduce_only))
                ladder = placed
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
                    leg_price = leg.price
                    min_passive = None
                    max_passive = None
                    if _bbo_prices is None:
                        book = _observed_book
                        if book is None and self._bbo_fetch is not None:
                            try:
                                book = self._bbo_fetch(market)
                            except Exception as exc:  # noqa: BLE001
                                _log.debug("permuto: BBO fetch for reduce-only leg failed for %s: %s", market, exc)
                                book = None
                        if book is not None:
                            observed_books[market] = book
                            if leg.side is Side.SELL and book.best_bid is not None:
                                min_passive = bbo_quantise(book.best_bid + _tick, _tick, up=True)
                                if leg_price < min_passive:
                                    leg_price = min_passive
                            elif leg.side is Side.BUY and book.best_ask is not None:
                                max_passive = bbo_quantise(book.best_ask - _tick, _tick, up=False)
                                if leg_price > max_passive:
                                    leg_price = max_passive
                    if oracle and oracle > 0.0:
                        band_lo = oracle * (1.0 - VENUE_BAND_PCT / 100.0)
                        band_hi = oracle * (1.0 + VENUE_BAND_PCT / 100.0)
                        if leg_price < band_lo - 1e-9 or leg_price > band_hi + 1e-9:
                            results[market] = (
                                "skip",
                                "reduce-only leg outside legal venue band")
                            continue
                    leg_size = (rescaled_size(leg.size, leg.price, leg_price, _lot)
                                if leg_price != leg.price else leg.size)
                    if leg_size <= 0.0:
                        continue
                    leg = type(leg)(leg.market, leg.side, leg_price,
                                    leg_size, True)
                    if market in observed_books:
                        bbo_reduce_placements.add(market)
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
                    if leg.reduce_only:
                        if leg.side is Side.SELL and min_passive is not None and clamped < min_passive:
                            if min_passive <= oracle * (1.0 + VENUE_BAND_PCT / 100.0) + 1e-9:
                                clamped = min_passive
                            else:
                                results[market] = (
                                    "skip",
                                    "reduce-only ask cannot clear book within venue band")
                                continue
                        elif leg.side is Side.BUY and max_passive is not None and clamped > max_passive:
                            if max_passive >= oracle * (1.0 - VENUE_BAND_PCT / 100.0) - 1e-9:
                                clamped = max_passive
                            else:
                                results[market] = (
                                    "skip",
                                    "reduce-only bid cannot clear book within venue band")
                                continue
                    if clamped != leg.price:
                        leg = type(leg)(leg.market, leg.side, clamped,
                                        leg.size, leg.reduce_only)
                # [review] ...and only when a per-market cap EXISTS. With
                # the no-limit sentinel assess_curfew() leaves both side
                # caps at zero -- correctly, a curfew cannot be a
                # fraction of a number nobody set -- so this clamp
                # dropped BOTH legs and the tick reported risk_blocked.
                # Fixing the sentinel for assess() alone was half a fix:
                # a flat account with the cap disabled quoted nothing at
                # all. The portfolio budget is the limit in that
                # configuration, and it is applied above.
                if (self._curfew is not None and oracle and oracle > 0.0
                        and self._max_position_usd > 0.0):
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
                        # The room-floor branch is a one-contract-wide
                        # window (short_cap - 1 < |pos| < short_cap);
                        # REDUCE_ONLY fires first in every reachable
                        # case and is where the pin is recorded.
                        continue
                    if allowed < leg.size:
                        leg = type(leg)(leg.market, leg.side, leg.price,
                                        allowed, leg.reduce_only)
                # [review] A risk-increasing leg may not exceed the
                # portfolio room. A REDUCING leg always may: shrinking
                # exposure is what the budget wants, and blocking it
                # would trap the book at the very moment it is trying
                # to get back inside.
                room_usd = portfolio_headroom_usd
                if (room_usd is not None and oracle and oracle > 0.0
                        and self._increases_exposure(leg, float(
                            state.positions.get(market, 0.0) or 0.0))):
                    room_contracts = math.floor(
                        max(0.0, room_usd) / oracle)
                    if room_contracts < 1.0:
                        continue
                    if room_contracts < leg.size:
                        leg = type(leg)(leg.market, leg.side, leg.price,
                                        room_contracts, leg.reduce_only)
                legs.append(leg)


        if to_cancel:
            # Before the upsert, so the reduce-only leg is not racing the
            # risk-increasing one it replaces.
            self._client.cancel_all(now_s, to_cancel)
            for market in to_cancel:
                self._resting[market] = RestingQuote()
                self._requote_at_pct.pop(market, None)

        if not legs:
            holding = [
                market for market, (action, _reason) in results.items()
                if action == LoopAction.HOLD.value
                and not self._resting.get(market, RestingQuote()).empty
            ]
            if holding:
                return TickResult(
                    "hold",
                    "%d market(s) resting; no replacement legs needed"
                    % len(holding),
                    results)
            if bbo_resets:
                return TickResult(
                    "withdraw",
                    "cancelled a self-blocking one-sided quote; rebuilding "
                    "against the external BBO next tick",
                    results)
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
            # [review] EVERY market, not every skipped one. Comparing
            # against the skip/withdraw rows alone ignored markets left
            # marked "quote" that produced no legs -- an invalid ladder,
            # a curfew clamp to zero size -- because those sit in neither
            # list. The equality held anyway, so a MIXED failure was
            # reported as a clean mode withdrawal and the real cause
            # never surfaced. Blaming the posture is only honest when
            # the posture is the whole story.
            if mode_skips and len(mode_skips) == len(results):
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
                # [review] The SAME normalisation as the ladder. This
                # parsed the raw value, so "decided once" was not true of
                # the pre-send path: with NaN metadata and a fresh-oracle
                # reprice, quantise_toward received NaN and handed back the
                # changed price UNSNAPPED, so an off-grid order still went
                # out despite the fallback added for the ladder. Two places
                # deriving one number is how it drifted the first time.
                m_tick = _effective_tick(mspec.get("tick_size"))
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
                if leg.reduce_only and leg.market in observed_books:
                    book = observed_books[leg.market]
                    if leg.side is Side.SELL and book.best_bid is not None:
                        min_p = bbo_quantise(book.best_bid + m_tick, m_tick, up=True)
                        if price < min_p:
                            if min_p <= ref * (1.0 + VENUE_BAND_PCT / 100.0) + 1e-9:
                                price = min_p
                            else:
                                results[leg.market] = (
                                    "skip",
                                    "reduce-only ask cannot clear book within venue band")
                                dropped.add(leg.market)
                                continue
                    elif leg.side is Side.BUY and book.best_ask is not None:
                        max_p = bbo_quantise(book.best_ask - m_tick, m_tick, up=False)
                        if price > max_p:
                            if max_p >= ref * (1.0 - VENUE_BAND_PCT / 100.0) - 1e-9:
                                price = max_p
                            else:
                                results[leg.market] = (
                                    "skip",
                                    "reduce-only bid cannot clear book within venue band")
                                dropped.add(leg.market)
                                continue

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
            if dropped:
                # Depth is min(bid, ask) per market. Once either side cannot
                # be sent, its sibling is pure exposure earning zero; retain
                # only legs from unaffected markets.
                kept_legs = [leg for leg in kept_legs
                             if leg.market not in dropped]
            return kept_legs, dropped

        def _revalidate_bbo(pending, sources):
            """Drop BBO-shaped markets invalidated by price adjustments or newer oracle."""
            dropped = set()
            for market in bbo_placements:
                original = latest_oracle(oracles, None, market)
                current = 0.0
                for source in sources:
                    current = latest_oracle(source, None, market)
                    if current > 0.0:
                        break
                if current <= 0.0:
                    current = original

                market_legs = [leg for leg in pending
                               if leg.market == market]
                if not market_legs:
                    continue

                initial_prices = bbo_initial_prices.get(market)
                prices_unchanged = (
                    initial_prices is not None
                    and all(
                        leg.price == (initial_prices[0] if leg.side is Side.BUY else initial_prices[1])
                        for leg in market_legs
                    )
                )

                if current == original and prices_unchanged:
                    continue

                book = None
                if current == original and market in observed_books:
                    book = observed_books[market]
                elif self._bbo_fetch is not None:
                    try:
                        book = self._bbo_fetch(market)
                    except Exception as exc:  # noqa: BLE001
                        _log.debug("permuto: final BBO fetch failed for %s: %s",
                                   market, exc)
                        book = None

                if book is None:
                    dropped.add(market)
                    results[market] = (
                        "skip",
                        "BBO price altered or oracle moved after read, and the "
                        "final book could not be read inside the tick budget")
                    continue
                raw_specs = flags.get("specs")
                spec = (raw_specs.get(market, {})
                        if isinstance(raw_specs, dict) else {})
                tick_size = _effective_tick(spec.get("tick_size"))
                if all(
                    bbo_rests_and_earns(
                        "bid" if leg.side is Side.BUY else "ask",
                        leg.price, current, book,
                        ring_pct=self._ring_pct, tick_size=tick_size)
                    for leg in market_legs
                ):
                    continue
                dropped.add(market)
                results[market] = (
                    "skip",
                    "adjusted BBO leg or refreshed oracle no longer admits "
                    "both legs inside the depth ring")
                _log.info(
                    "permuto: %s BBO placement invalidated (oracle %.6f -> %.6f); "
                    "dropping stale pair and rebuilding next tick",
                    market, original, current)

            for market in bbo_reduce_placements:
                if market in dropped:
                    continue
                original = latest_oracle(oracles, None, market)
                current = 0.0
                for source in sources:
                    current = latest_oracle(source, None, market)
                    if current > 0.0:
                        break
                if current <= 0.0:
                    current = original

                market_legs = [leg for leg in pending
                               if leg.market == market]
                if not market_legs:
                    continue

                book = None
                if current == original and market in observed_books:
                    book = observed_books[market]
                elif self._bbo_fetch is not None:
                    try:
                        book = self._bbo_fetch(market)
                    except Exception as exc:  # noqa: BLE001
                        _log.debug("permuto: final BBO fetch failed for reduce-only %s: %s",
                                   market, exc)
                        book = None
                if book is None and market in observed_books:
                    book = observed_books[market]

                if book is None:
                    dropped.add(market)
                    results[market] = (
                        "skip",
                        "final book could not be read for reduce-only leg revalidation")
                    continue

                raw_specs = flags.get("specs")
                spec = (raw_specs.get(market, {})
                        if isinstance(raw_specs, dict) else {})
                tick_size = _effective_tick(spec.get("tick_size"))
                eps = tick_size * 1e-4
                valid = True
                for leg in market_legs:
                    if leg.side is Side.SELL:
                        if book.best_bid is not None and leg.price <= book.best_bid + eps:
                            valid = False
                            break
                        if leg.price > current * (1.0 + VENUE_BAND_PCT / 100.0) + 1e-9:
                            valid = False
                            break
                    elif leg.side is Side.BUY:
                        if book.best_ask is not None and leg.price >= book.best_ask - eps:
                            valid = False
                            break
                        if leg.price < current * (1.0 - VENUE_BAND_PCT / 100.0) - 1e-9:
                            valid = False
                            break

                if not valid:
                    dropped.add(market)
                    results[market] = (
                        "skip",
                        "reduce-only leg would cross the refreshed book or exceed venue band")
                    _log.info(
                        "permuto: %s reduce-only leg invalidated against refreshed book or oracle; dropping leg",
                        market)

            if not dropped:
                return list(pending), dropped
            return ([leg for leg in pending if leg.market not in dropped],
                    dropped)

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
            legs, stale_bbo = _revalidate_bbo(
                legs, (fresh_oracles, oracles))
            preflight_cancel.update(stale_bbo)

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
                    legs, stale_bbo2 = _revalidate_bbo(
                        legs, (again, fresh_oracles, oracles))
                    dropped2.update(stale_bbo2)
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
        try:
            response = self._client.batch_upsert(payload, now_s)
        except (PermutoAuthError, BatchError) as exc:
            if "Carried-session stress margin" in str(exc):
                reduce_legs = [l for l in payload if l.get("reduce_only")]
                if reduce_legs:
                    _log.warning(
                        "permuto: batch_upsert stress margin blocked (%s); "
                        "sending %d reduce_only leg(s) via /exchange/order",
                        exc, len(reduce_legs))
                    single_sent = 0
                    for rleg in reduce_legs:
                        try:
                            self._client.place_order({
                                "market": rleg["market"],
                                "side": rleg["side"],
                                "price": rleg["price"],
                                "size": rleg["size"],
                                "order_type": "limit",
                                "tif": rleg.get("tif", "gtc"),
                                "reduce_only": True,
                            }, now_s)
                            single_sent += 1
                        except Exception as sub_exc:
                            _log.warning("permuto: single reduce_only order failed for %s: %s",
                                         rleg["market"], sub_exc)
                    if single_sent > 0:
                        return TickResult(
                            "quote", "placed reduce_only single orders under stress margin",
                            {m: ("quote", "reduce_only posted") for m in self._markets})
            raise

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
                crossed, seen, dirty = set(), set(), set()
                # [review] A COUNT MISMATCH POISONS THE WHOLE MAPPING, not
                # just the tail. These rows are paired with legs by
                # POSITION, so a missing row in the MIDDLE shifts every
                # later pairing by one -- a market then reads another
                # market's result, and can come back "seen" and clean while
                # its own leg was refused. Marking only the unpaired suffix
                # dirty, as the previous fix did, addresses the one case
                # where the shift happens to be at the end.
                #
                # There is no way to re-align without an identifier the
                # rows do not carry, so the honest response is to trust
                # none of it: no crossings inferred, every market dirty, and
                # the backoff left exactly where it was until a response
                # arrives that can be read.
                if len(leg_rows) != len(legs):
                    _log.warning(
                        "permuto: batch response had %d row(s) for %d leg(s)"
                        " -- positional mapping is ambiguous, so no backoff "
                        "is inferred this tick", len(leg_rows), len(legs))
                    for leg in legs:
                        dirty.add(leg.market)
                    leg_rows = []
                for leg, row in zip(legs, leg_rows):
                    if not isinstance(row, dict):
                        # A row we cannot parse tells us nothing about this
                        # market, so it must not count as a clean tick.
                        dirty.add(leg.market)
                        continue
                    seen.add(leg.market)
                    reason = str(row.get("rejection_reason") or "").lower()
                    action = str(row.get("action") or "").lower()
                    if _is_cross_refusal(reason):
                        crossed.add(leg.market)
                    elif reason or action not in ("placed", "modified",
                                                  "unchanged"):
                        # [review] A NON-crossing refusal (margin/band) or an
                        # action that does not prove the order is resting is
                        # not evidence we stopped crossing. "rejected",
                        # "cancelled", an empty action, or any unknown value
                        # all count as dirty.
                        dirty.add(leg.market)
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
                                         self._last_skew.get(mkt, 0.0),
                                         self._last_tick_frac.get(mkt, 0.0)))
                    elif accepted and mkt in seen and mkt not in dirty:
                        # Only a market whose every row came back present and
                        # ACCEPTED has actually demonstrated it rests.
                        #
                        # [review] `accepted` gates this, and it has to: a
                        # known batch_failed envelope can still report every
                        # row as "placed", so without the gate an explicit
                        # venue refusal decayed the learned offset as though
                        # the batch had rested. The rows describe legs; the
                        # envelope describes whether the venue took them,
                        # and only the envelope can answer that.
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

        default_trigger = self._ring_pct * REQUOTE_AT_RING_FRACTION
        for market in {leg.market for leg in legs}:
            if market not in bbo_placements:
                self._requote_at_pct.pop(market, None)
                continue
            ref = float(send_oracles.get(market) or 0.0)
            market_legs = [leg for leg in legs if leg.market == market]
            if ref <= 0.0 or not market_legs:
                self._requote_at_pct.pop(market, None)
                continue
            widest = max(abs(leg.price / ref - 1.0) * 100.0
                         for leg in market_legs)
            if (default_trigger < widest
                    and widest <= self._ring_pct + 1e-9):
                widest = min(widest, self._ring_pct)
                self._requote_at_pct[market] = min(
                    self._ring_pct,
                    widest + (self._ring_pct - widest) * 0.5,
                )
            else:
                self._requote_at_pct.pop(market, None)

        self._reopen_pending = False
        return TickResult("quote", "%d legs" % len(legs), results)

    @staticmethod
    def _increases_exposure(leg, position: float) -> bool:
        """True when filling this leg would make |position| larger.

        A buy grows a long and shrinks a short; a sell does the reverse.
        From FLAT either side grows exposure, which is the case the
        portfolio budget most needs to catch -- a market with nothing on
        it looks harmless to every per-market check.
        """
        if position != position:            # NaN: assume the worse case
            return True
        if leg.side is Side.BUY:
            return position >= 0.0
        return position <= 0.0

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
