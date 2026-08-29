"""Keeping a trading session alive for a week without a human present.

Two halves, deliberately separated. The POLICY — when to renew, when to give
up, how long to wait — is pure and exhaustively tested here. The transport is
a thin shell around it.

WHY THIS IS NOT "JUST RE-AUTH". Sessions expire, and depth_seconds only
accrues while quoting: an expired session is not a degraded state, it is zero
score until noticed. The contest runs ~102 hours unattended, so every renewal
between Monday's bell and Friday's close happens with nobody watching. The
failure mode is silence — quotes stop being accepted, the book drains, and
nothing local says anything is wrong.

So renewal is PROACTIVE, on a margin before expiry, rather than reactive on
the first 401. A reactive-only design guarantees at least one rejected request
per cycle, and during that window a fill can lift a side we cannot restore.
The 401 path still exists, because a server may invalidate a session early,
but it is the safety net and not the plan.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

__all__ = [
    "RenewAction",
    "SessionState",
    "next_backoff_s",
    "renew_action",
]

#: Renew this long before the token is due to expire.
#:
#: The OAuth agent path is documented as "renewed every ~40 minutes or on any
#: 401", so five minutes is comfortably inside one renewal period while still
#: leaving room for several retries if the first attempt fails. Too small and
#: a slow renewal races the expiry it was meant to prevent; too large and we
#: mint sessions we never use.
RENEW_MARGIN_S = 300.0

#: Give up re-trying only after this long, then keep trying at the cap.
#:
#: Never stop entirely. A session that cannot be renewed means the book is
#: unmanaged, and an operator is far more likely to notice a bot that is
#: shouting every minute than one that quietly gave up an hour ago.
BACKOFF_BASE_S = 2.0
BACKOFF_CAP_S = 60.0


class RenewAction(str, Enum):
    OK = "ok"
    """Session is valid and not near expiry. Trade."""

    RENEW = "renew"
    """Renew now, before anything is rejected."""

    WAIT = "wait"
    """A renewal failed recently; hold off until the backoff elapses."""

    NO_SESSION = "no_session"
    """Nothing to renew — the identity has never been linked."""


@dataclass
class SessionState:
    """Everything the policy needs. Plain data, owned by the caller."""

    token: str = ""
    expires_at_s: float = 0.0
    """Absolute expiry, same clock as ``now_s``. 0 means unknown."""

    consecutive_failures: int = 0
    last_attempt_s: float = 0.0
    forced: bool = False
    """Set when a 401 arrives, so the next decision renews regardless of the
    clock. A server may invalidate a session before its stated expiry, and
    trusting our own copy of the deadline over the server's answer is how a
    bot argues with reality and loses."""


def next_backoff_s(consecutive_failures: int) -> float:
    """Exponential backoff, capped.

    Capped rather than unbounded because the thing being retried is the only
    route back to managing a live book -- an hour-long sleep would be a
    self-inflicted outage on top of whatever caused the failure.
    """
    if consecutive_failures <= 0:
        return 0.0
    delay = BACKOFF_BASE_S * (2 ** (consecutive_failures - 1))
    return min(delay, BACKOFF_CAP_S)


def renew_action(state: SessionState, now_s: float) -> RenewAction:
    """What to do about the session, right now.

    Total: every input maps to exactly one action, so the caller has no
    judgement left to exercise and the whole policy is testable without a
    socket.
    """
    # [review] BACKOFF FIRST, above both the 401 latch and the empty token.
    #
    # It used to sit below them, and both routes around it end in the same
    # place: a renewal that keeps failing, retried on every 5s tick for the
    # whole contest. `forced` is set by a 401 and cleared only by a
    # SUCCESSFUL reauth, so a venue answering 401 forever pinned it to RENEW;
    # and an empty token short-circuited to NO_SESSION before the failure
    # count was ever consulted, so a failing bootstrap did the same.
    #
    # The "a 401 outranks the clock" intent survives, because on the first
    # 401 `consecutive_failures` is still 0 and this block does not fire --
    # the latch is honoured immediately and only earns a delay once an actual
    # attempt has failed.
    if state.consecutive_failures > 0:
        due = state.last_attempt_s + next_backoff_s(state.consecutive_failures)
        if now_s < due:
            return RenewAction.WAIT
        return RenewAction.RENEW

    if state.forced:
        # A 401 outranks the clock: the session is known dead, and waiting
        # changes nothing except how long the book goes unmanaged.
        return RenewAction.RENEW

    if not state.token:
        # No token AND nothing has failed yet: this is a cold start. The
        # caller decides whether it holds an identity that can mint one --
        # see PermutoClient.ensure_session(), which treats this as "go and
        # bootstrap" rather than as a dead end.
        return RenewAction.NO_SESSION

    if state.expires_at_s <= 0.0:
        # Unknown expiry. Renew rather than assume: an unknown deadline that
        # turns out to be imminent costs a rejected batch, and a needless
        # renewal costs one request.
        return RenewAction.RENEW

    if now_s >= state.expires_at_s - RENEW_MARGIN_S:
        return RenewAction.RENEW

    return RenewAction.OK
