"""The authenticated transport. Thin, because the policy lives elsewhere.

`session.py` decides WHEN to renew, `batch.py` decides WHAT a legal batch
looks like, `quoting.py` decides whether to be in the market at all. This
module does none of that. It holds a socket, a token, and the small amount of
state that has to survive between calls.

THE TRAP THIS MODULE EXISTS TO AVOID. Renewal is not registration. The obvious
implementation of "renew the session" is to call `register()` again -- it does
challenge, sign, auth, and returns a token, which is exactly what renewal
needs. But `register()` opens with a `signup_open()` gate and refuses when the
venue reports sign-up closed. **Registration closes Monday 31 Aug at 17:00 ET,
several hours into a contest that runs until Friday.** A renewal path built on
`register()` would work in every test, work on Monday morning, and then fail
on every attempt from Monday evening onwards -- the book would drain and stay
drained for four days, with the logs showing a sign-up error that has nothing
to do with the actual problem. So `reauth()` below deliberately repeats the
challenge/sign/auth sequence WITHOUT the gate: an existing identity proving it
still holds its key is a different operation from a new account asking to be
let in, and only the second one is closed.
"""

from __future__ import annotations

import json
import logging
import urllib.error
import urllib.request
from typing import Any, Optional, Sequence

from .auth import BASE_URL, PermutoAuthError, _HEADERS, _TIMEOUT
from .session import RenewAction, SessionState, renew_action

_log = logging.getLogger(__name__)

__all__ = [
    "DEFAULT_SESSION_TTL_S",
    "PermutoClient",
    "PermutoNotLinked",
    "PermutoSessionExpired",
]

#: Assumed session lifetime when the venue does not state one.
#:
#: The policy in session.py maps "unknown expiry" to RENEW, which is the right
#: POLICY -- an unknown deadline that turns out to be imminent is worse than a
#: spare request. But the loop ticks every few seconds, so taking that
#: literally would mint a new session on every tick and hammer the auth route
#: for the whole contest. The transport is the right place to fix that: adopt
#: a conservative deadline so renewals happen on a sane cadence. 40 minutes
#: matches the documented agent-session cadence, and RENEW_MARGIN_S still
#: pulls the actual renewal 5 minutes earlier.
DEFAULT_SESSION_TTL_S = 2400.0


class PermutoSessionExpired(PermutoAuthError):
    """HTTP 401/403. The session is dead regardless of what our clock says.

    Retryable exactly once, after a fresh token.
    """


class PermutoNotLinked(PermutoAuthError):
    """No session is held LOCALLY. Deliberately not a SessionExpired.

    These look alike and must not be treated alike. A 401 means we had a
    session and the venue refused it, so minting a new one and retrying is
    right. Holding no token at all means the loop has not linked yet -- and
    `quoting.decide()` maps that to WITHDRAW. If the retry path caught this
    too, the transport would quietly authenticate and place orders behind a
    policy that had just decided not to be in the market. The transport does
    not get a vote on that.
    """


class PermutoClient:
    """One identity's authenticated connection to the venue.

    Not thread-safe: one loop owns one client. The session token mutates on
    renewal, and two threads renewing concurrently would race to install
    different tokens and invalidate each other's.
    """

    def __init__(
        self,
        identity: Any,
        *,
        session_token: str = "",
        expires_at_s: float = 0.0,
        base_url: str = BASE_URL,
        timeout: float = _TIMEOUT,
    ) -> None:
        self._identity = identity
        self._base_url = base_url.rstrip("/")
        self._timeout = timeout
        self.session = SessionState(
            token=session_token, expires_at_s=expires_at_s
        )

    # ------------------------------------------------------------------ #
    # Transport
    # ------------------------------------------------------------------ #
    def _request(
        self,
        method: str,
        path: str,
        payload: Optional[dict] = None,
        *,
        authed: bool = True,
    ) -> Any:
        headers = dict(_HEADERS)
        if authed:
            if not self.session.token:
                raise PermutoNotLinked(
                    "%s %s needs a session and none is held; call "
                    "ensure_session() first" % (method, path)
                )
            headers["Authorization"] = "Bearer " + self.session.token

        body = json.dumps(payload).encode() if payload is not None else None
        req = urllib.request.Request(
            self._base_url + path, data=body, headers=headers, method=method
        )
        try:
            with urllib.request.urlopen(req, timeout=self._timeout) as resp:
                return _decode(resp.read().decode(), method, path)
        except urllib.error.HTTPError as exc:
            detail = ""
            try:
                detail = exc.read().decode()[:400]
            except Exception:  # noqa: BLE001
                pass
            if exc.code in (401, 403) and authed:
                # Believe the server over our own clock. Leaving `forced`
                # unset here would let renew_action() keep answering OK from
                # a stale expiry while every request is rejected.
                self.session.forced = True
                raise PermutoSessionExpired(
                    "%s %s -> HTTP %s (session rejected) %s"
                    % (method, path, exc.code, detail)
                ) from exc
            raise PermutoAuthError(
                "%s %s -> HTTP %s %s" % (method, path, exc.code, detail)
            ) from exc
        except urllib.error.URLError as exc:
            raise PermutoAuthError(
                "%s %s unreachable: %s" % (method, path, exc)
            ) from exc
        except (TimeoutError, OSError, ValueError) as exc:
            # [review] The READ boundary, not just the connect. urlopen()
            # returning does not end the I/O: resp.read() can raise a bare
            # TimeoutError or socket OSError, and a mangled body a ValueError
            # -- none of which are URLError, so they escaped the
            # PermutoAuthError contract entirely. An open_orders timeout then
            # bypassed the runner's withdraw path (which catches
            # PermutoAuthError) and the renewal backoff was never charged.
            # The transport promises exactly two exception types; this makes
            # the promise true at both ends of the request.
            raise PermutoAuthError(
                "%s %s failed mid-read: %s" % (method, path, exc)
            ) from exc

    # ------------------------------------------------------------------ #
    # Session
    # ------------------------------------------------------------------ #
    def reauth(self, now_s: float) -> None:
        """Prove we still hold the key, and take a fresh token.

        Deliberately NOT `auth.register()` -- see the module docstring. This
        is the same challenge/sign/auth sequence without the sign-up gate,
        because a registered identity must be able to renew for four days
        after registration closes.
        """
        pubkey = self._identity.public_key()
        challenge = self._request(
            "POST",
            "/exchange/wallet_link_challenge",
            {"wallet_pubkey": pubkey},
            authed=False,
        )
        # _decode returns whatever JSON the venue sent, which need not be an
        # object. A null or an array made this raise AttributeError, and
        # ensure_session() records backoff only for PermutoAuthError -- so a
        # response-shape failure bypassed renewal accounting entirely.
        if not isinstance(challenge, dict):
            raise PermutoAuthError(
                "challenge response was %s, not an object"
                % type(challenge).__name__)
        token = challenge.get("challenge_token")
        nonce_hex = challenge.get("nonce")
        if not token or not nonce_hex:
            raise PermutoAuthError(
                "challenge response missing challenge_token/nonce: %r"
                % (challenge,)
            )
        # bytes.fromhex raises ValueError (or TypeError on a non-string)
        # for a malformed nonce. Left raw those escape the PermutoAuthError
        # contract: ensure_session() records backoff only for that type, so
        # a bad response shape would bypass renewal accounting entirely and
        # be retried on every tick as an unexpected error.
        try:
            nonce = bytes.fromhex(nonce_hex)
        except (TypeError, ValueError) as exc:
            raise PermutoAuthError(
                "challenge nonce is not hex: %r" % (nonce_hex,)) from exc
        if len(nonce) != 32:
            raise PermutoAuthError(
                "expected a 32-byte nonce, got %d -- refusing to sign an "
                "unexpected message" % len(nonce)
            )

        # [review] Signing failures are AUTH failures. identity.sign() reaches
        # the OS keystore (DPAPI on Windows) and can raise something that is
        # not a PermutoAuthError -- which escapes ensure_session()'s renewal
        # accounting entirely, so a permanently broken keystore fetched a
        # fresh challenge on every tick with no backoff at all.
        try:
            signature = self._identity.sign(nonce).hex()
        except PermutoAuthError:
            raise
        except Exception as exc:  # noqa: BLE001
            raise PermutoAuthError(
                "could not sign the auth challenge: %s" % exc) from exc

        auth = self._request(
            "POST",
            "/exchange/wallet_auth",
            {
                "challenge_token": token,
                "wallet_pubkey": pubkey,
                "signature": signature,
            },
            authed=False,
        )
        if not isinstance(auth, dict):
            raise PermutoAuthError(
                "auth response was %s, not an object" % type(auth).__name__)
        session = auth.get("session_token") or auth.get("token")
        if not isinstance(session, str) or not session.strip():
            raise PermutoAuthError(
                "renewal returned no usable session token (got %r)"
                % (session,)
            )

        self.session.token = session
        self.session.expires_at_s = _expiry_from(auth, now_s)
        self.session.consecutive_failures = 0
        self.session.forced = False
        self.session.last_attempt_s = now_s

    def ensure_session(self, now_s: float) -> RenewAction:
        """Bring the session up to date. Returns what the policy decided.

        A WAIT result is returned rather than slept on: the caller owns the
        loop clock, and blocking here would stall the quote loop inside a
        function whose job is bookkeeping.
        """
        action = renew_action(self.session, now_s)
        # [review] NO_SESSION means "cold start", not "give up".
        #
        # This client always holds a signing identity, and reauth() is the
        # full challenge/sign/wallet_auth handshake -- it needs no previous
        # token and passes no signup gate. So an empty token is something we
        # can fix, and treating it as terminal is what left the live session
        # dead: PermutoLive is constructed with session_token="", every
        # ensure_session() returned NO_SESSION, session_ok was False on every
        # tick, and the loop never placed an order while the switch showed
        # PERMUTO ON. Nothing else in the process could mint that first
        # token -- the 401 path that sets `forced` is unreachable, because a
        # request with an empty token raises PermutoNotLinked before it is
        # ever sent.
        #
        # Backoff is not bypassed by this: renew_action() now weighs
        # consecutive_failures ABOVE the empty-token branch, so a bootstrap
        # that keeps failing comes back as WAIT and this promotion never sees
        # it.
        if action is RenewAction.NO_SESSION and self._identity is not None:
            action = RenewAction.RENEW
        if action is not RenewAction.RENEW:
            return action
        self._reauth_counted(now_s)
        return RenewAction.RENEW

    def _reauth_counted(self, now_s: float) -> None:
        """reauth(), with the failure accounting the backoff policy needs.

        [review] Every route to reauth() must come through here. The retry
        path called reauth() DIRECTLY, so a failing renewal left
        consecutive_failures at 0 with `forced` still set -- and
        renew_action() answers that with RENEW immediately, on every 5s tick,
        which is the auth-route hammer the backoff exists to prevent. The
        accounting lived in the one caller that happened to have it.
        """
        try:
            self.reauth(now_s)
        except PermutoAuthError as exc:
            self.session.consecutive_failures += 1
            self.session.last_attempt_s = now_s
            _log.warning(
                "permuto: session renewal failed (attempt %d): %s",
                self.session.consecutive_failures, exc,
            )
            raise

    # ------------------------------------------------------------------ #
    # Trading
    # ------------------------------------------------------------------ #
    def batch_upsert(self, legs: Sequence[dict], now_s: float) -> Any:
        """POST a batch built by :func:`batch.build_upsert_batch`.

        Legs are NOT re-validated here. They were validated against the oracle
        they were priced from, and re-checking against a newer oracle would
        reject a batch the venue would have accepted.
        """
        if not legs:
            # An empty batch is not a cancel -- on an upsert route it is a
            # no-op that still spends a request and can read, in logs, like a
            # withdrawal that never happened.
            return {}
        return self._retry_once_on_401(
            "POST", "/exchange/batch_upsert", {"orders": list(legs)}, now_s
        )

    def account(self, now_s: float) -> Any:
        """Equity, used margin and positions, for :mod:`risk`."""
        return self._retry_once_on_401("POST", "/exchange/account", {}, now_s)

    def open_orders(self, now_s: float) -> Any:
        """What is actually resting, as opposed to what we believe rests."""
        return self._retry_once_on_401(
            "POST", "/exchange/open_orders", {}, now_s
        )

    def cancel_all(
        self, now_s: float, markets: Optional[Sequence[str]] = None
    ) -> Any:
        """Withdraw. Must work when everything else is sick."""
        payload: dict = {}
        if markets:
            payload["markets"] = list(markets)
        return self._retry_once_on_401(
            "POST", "/exchange/cancel_all", payload, now_s
        )

    def _retry_once_on_401(
        self, method: str, path: str, payload: dict, now_s: float
    ) -> Any:
        """One automatic re-auth and retry, then give up.

        Once, not a loop: a 401 that survives a fresh token is not a session
        problem, and retrying it in a tight loop against an auth route is how
        a bot gets rate-limited out of its own contest.
        """
        try:
            return self._request(method, path, payload)
        except PermutoSessionExpired:
            _log.info(
                "permuto: %s rejected the session, re-authing once", path
            )
            # Counted, not direct -- see _reauth_counted().
            prior = self.session.consecutive_failures
            self._reauth_counted(now_s)
            try:
                return self._request(method, path, payload)
            except PermutoSessionExpired:
                # [review] A 401 that SURVIVES a fresh token is the case this
                # method is named for, and it was the quietest failure here:
                # reauth() zeroes consecutive_failures on success, so the
                # second rejection re-set `forced` against a zero count and
                # the next tick renewed again at once. Restoring the prior
                # count and charging this attempt makes the backoff escalate
                # across repeats instead of resetting every five seconds.
                self.session.consecutive_failures = prior + 1
                self.session.last_attempt_s = now_s
                raise


def _decode(raw: str, method: str, path: str) -> Any:
    """Parse a response body, or say clearly why it is not one.

    The venue serves its single-page app on any unrecognised path, with HTTP
    200 and an HTML body -- so a mistyped route does not 404, it succeeds and
    hands back a document. Left to json.loads that surfaces as a bare
    JSONDecodeError from inside the transport, bypassing every PermutoAuthError
    the callers are written to handle, and reading like malformed data rather
    than a wrong address.
    """
    if not raw.strip():
        return {}
    try:
        return json.loads(raw)
    except ValueError as exc:
        head = raw.lstrip()[:40].lower()
        if head.startswith("<!doctype") or head.startswith("<html"):
            raise PermutoAuthError(
                "%s %s returned the web app, not JSON -- that path is not an "
                "API route (unknown paths fall through to the SPA with a 200)"
                % (method, path)
            ) from exc
        raise PermutoAuthError(
            "%s %s returned unparseable JSON: %.120r" % (method, path, raw)
        ) from exc


def _expiry_from(auth: dict, now_s: float) -> float:
    """Absolute expiry on the caller's clock, however the venue phrases it."""
    for key in ("expires_at", "expires_at_s", "expiry"):
        value = auth.get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if value > 0:
                return float(value)
    for key in ("expires_in", "expires_in_s", "ttl"):
        value = auth.get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if value > 0:
                return now_s + float(value)
    return now_s + DEFAULT_SESSION_TTL_S
