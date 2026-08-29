"""Permuto link/auth flow and leaderboard standing.

The unattended path, which is the one a market maker needs:

    GET  /info/meta                      -> refuse early if signup is closed
    POST /exchange/wallet_link_challenge -> {challenge_token, nonce}
    (local) AugSchemeMPL sign the 32 DECODED nonce bytes
    POST /exchange/wallet_auth           -> session token

No WalletConnect, no browser, no third-party wallet. Sage exists to let a
human approve a signature in a browser; a bot running unattended for the whole
contest week cannot use that, because sessions expire and every renewal would
need a person present.

Two details that are easy to get wrong and expensive to debug:

* **The nonce is signed as bytes, not as text.** It arrives hex-encoded and
  must be decoded to exactly 32 bytes first. Signing the ASCII of the hex
  string produces a valid signature over the wrong message, which fails
  verification with no useful error.
* **The signature is RAW AugSchemeMPL**, not CHIP-0002. A wallet signing
  through WalletConnect wraps the message in the "Chia Signed Message"
  envelope; that will not verify here.

A browser ``User-Agent`` is required. Without one the venue answers 403 even
on public routes, which reads exactly like an auth failure and is not one.

THE COMMIT BOUNDARY, AND WHY "FAILED" IS THE WRONG DEFAULT.  The last POST is
irreversible: once the venue commits, this key IS that account, forever, with
no change-my-key flow. But the transport cannot tell us whether it committed
-- a socket timeout arriving *after* the write raises the identical
``URLError`` as a connection refused *before* it. Collapsing that into an
ordinary failure is what invites a second permanent link, so the unknown case
gets :class:`PermutoLinkIndeterminate` and the caller must reconcile rather
than retry. Two supports make that workable:

* the attempt is recorded through the identity BEFORE the request, which is
  also the write test that refuses to link at all when the record could not be
  kept; and
* :func:`reconcile_registration` answers "am I already linked?" over
  ``/info/wallet_bls_trading_address``, the route documented as having no link
  side effects -- so asking the question can never be the act.
"""

from __future__ import annotations

import json
import logging
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Optional

_log = logging.getLogger(__name__)

BASE_URL = "https://perps.permuto.capital"

#: Agent filtering, not authentication. A default urllib agent gets HTTP 403
#: on routes that need no credentials at all.
_HEADERS = {
    "User-Agent": "Mozilla/5.0 (XOPTrader)",
    "Accept": "application/json",
    "Content-Type": "application/json",
}

_TIMEOUT = 30


class PermutoAuthError(RuntimeError):
    """Operator-facing failure during link, auth or standing lookup."""


class PermutoLinkIndeterminate(PermutoAuthError):
    """The committing request failed in a way that does not say what happened.

    ``POST /exchange/wallet_auth`` is the irreversible boundary: once the
    venue commits, this key is that account forever. A socket timeout, a
    connection reset or a 5xx after the commit raises exactly the same
    ``URLError``/``HTTPError`` as a refusal before it, so the transport gives
    us no way to tell "never linked" from "linked, answer lost".

    Collapsing that into an ordinary failure is what re-enables Register for a
    key the venue may already own -- and a second link attempt on an already
    linked key is the one action with no undo. So the unknown case gets its
    own type, and the caller must treat it as *possibly linked*: reconcile
    with :func:`reconcile_registration`, which reads the venue back through a
    route documented as having no link side effects.
    """


def _venue_answered_and_refused(exc: BaseException) -> bool:
    """True when the venue replied to the commit call and turned it down.

    A 4xx is an ANSWER: the request arrived, was understood and was rejected,
    so nothing was committed. Everything else -- a timeout, a reset, a 5xx, a
    200 whose body will not parse -- leaves the outcome unknown. The
    asymmetry is deliberate: a false "indeterminate" costs the operator one
    extra read-back click, while a false "not linked" invites a second
    permanent link.
    """
    cause = exc.__cause__
    return (
        isinstance(cause, urllib.error.HTTPError)
        and 400 <= cause.code < 500
    )


def _request(method: str, path: str, payload: Optional[dict] = None,
             timeout: Optional[float] = None) -> Any:
    body = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        BASE_URL + path, data=body, headers=_HEADERS, method=method
    )
    try:
        with urllib.request.urlopen(
                req, timeout=_TIMEOUT if timeout is None else timeout) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        detail = ""
        try:
            detail = exc.read().decode()[:400]
        except Exception:  # noqa: BLE001
            pass
        raise PermutoAuthError(
            "%s %s -> HTTP %s %s" % (method, path, exc.code, detail)
        ) from exc
    except urllib.error.URLError as exc:
        raise PermutoAuthError("%s %s unreachable: %s" % (method, path, exc)) from exc


# --------------------------------------------------------------------------- #
# Read-only
# --------------------------------------------------------------------------- #

def signup_open() -> tuple[bool, dict]:
    """``(open?, flags)``. Check BEFORE signing anything.

    A new identity is refused at the challenge step with HTTP 403 when signup
    is closed, so there is no point deriving a signature we cannot use.
    """
    meta = _request("GET", "/info/meta") or {}
    flags = meta.get("flags")
    # FAIL CLOSED on an unknown shape. `{}`, a missing `flags`, or a
    # non-boolean all used to read as "signup is open" -- and the very next
    # thing this gates is a PERMANENT link. Only an explicit boolean says the
    # door is open.
    if not isinstance(flags, dict) or "signup_closed" not in flags:
        raise PermutoAuthError(
            "/info/meta did not report signup_closed; refusing to link an "
            "identity against an unknown venue state"
        )
    closed = flags["signup_closed"]
    if not isinstance(closed, bool):
        raise PermutoAuthError(
            "signup_closed was %r, not a boolean; refusing to guess" % (closed,)
        )
    return (not closed), flags


def resolve_trading_address(pubkey_hex: str) -> dict:
    """Map a BLS pubkey to its trading address.

    Documented as having **no link side effects**, so it is safe to call
    before committing to an identity -- which makes it the natural pre-flight
    check that a key is well-formed and the venue agrees with our derivation.
    """
    return _request(
        "GET", "/info/wallet_bls_trading_address?pubkey=" + pubkey_hex
    )


def leaderboard_entry(user_id: str) -> Optional[dict]:
    """Our row on the leaderboard, or ``None``.

    Pages explicitly. The default page size is 20 while the field is far
    larger, and reading only page one is how this project previously misread
    the standings by a factor of 35.
    """
    offset = 0
    while True:
        page = _request(
            "GET", "/exchange/leaderboard?limit=100&offset=%d" % offset
        )
        for bucket in ("market_makers", "traders"):
            for row in page.get(bucket, []):
                if row.get("user_id") == user_id:
                    return {"category": bucket, **row}
        rows_this_page = max(
            len(page.get("market_makers", [])), len(page.get("traders", []))
        )
        seen = offset + rows_this_page

        # `.get(key, 0)` returns None when the key EXISTS with a JSON null,
        # and max(None, 50) raises TypeError -- crashing the lookup and
        # reporting "Failed" for an account that is registered. Coerce, then
        # do not trust the result: totals of 0 from a null response would
        # make `seen >= total` true immediately and silently truncate the
        # search to page one, which fails in the direction that reads as
        # "you are not registered". A short page is the reliable end signal.
        def _count(key):
            value = page.get(key)
            return value if isinstance(value, int) and value >= 0 else 0

        total = max(_count("market_makers_total"), _count("traders_total"))
        if rows_this_page == 0 or (total > 0 and seen >= total):
            return None
        # No fixed ceiling. An offset cap silently reports a registered
        # account as unlisted once the field grows past it, which is the
        # same class of error as reading only page one -- and it fails in
        # the direction that looks like "you are not registered".
        offset += 100


# --------------------------------------------------------------------------- #
# Link + auth
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class Registration:
    """Result of a successful link. ``session_token`` is a live credential."""

    user_id: str
    trading_address: str
    pubkey: str
    session_token: str

    def __repr__(self) -> str:  # pragma: no cover - defensive
        # Never let a session token reach a log line or a traceback.
        return "Registration(user_id=%s..., address=%s...)" % (
            self.user_id[:12],
            self.trading_address[:16],
        )

    __str__ = __repr__


def _note_link_attempt(identity: Any) -> None:
    """Durably record that the commit call is about to be issued.

    Any exception propagates on purpose -- see the call site. ``getattr``
    rather than a hard call because ``identity`` is duck-typed here (the
    protocol tests hand in a signing stub with no storage at all); a stub that
    cannot record an attempt also cannot lose one.
    """
    mark = getattr(identity, "mark_link_attempt", None)
    if callable(mark):
        mark()


def _clear_link_attempt(identity: Any) -> None:
    """Drop the marker after an answer that rules a link out."""
    clear = getattr(identity, "clear_link_attempt", None)
    if not callable(clear):
        return
    try:
        clear()
    except Exception as exc:  # noqa: BLE001 - never mask the real failure
        _log.warning("permuto: could not clear the link-attempt marker: %s", exc)


def reconcile_registration(identity: Any) -> Optional[tuple[str, str]]:
    """``(user_id, trading_address)`` if the venue CONFIRMS this key is linked.

    The read-back for an indeterminate link, and the hard part is not fetching
    anything -- it is finding a signal that actually means "linked".

    WHAT DOES NOT WORK, MEASURED.  The obvious candidate is
    ``/info/wallet_bls_trading_address``, which returns a ``wallet_user_id``
    and is documented as having no link side effects. It is useless as a
    linkage test: that route DERIVES the id from the pubkey rather than
    looking up a registration. Probed on 2026-08-28 with a freshly generated
    key that has never been sent to the venue::

        wallet_user_id = '7232a2d5...'
        wallet_address = 'xch1wge294flzk0tuyf6c4l3c6nzt7llhtel2kq2'

    So keying off that field reports EVERY valid key as already linked -- and
    the caller then records a registration that never happened and disables
    Register for an account that does not exist. That is precisely the failure
    this function exists to avoid, arriving through the function itself.

    WHAT DOES WORK, PARTLY.  The leaderboard lists real accounts, so finding
    our derived id there is positive proof of a link. Not finding it proves
    nothing -- a linked account that has not traded may not be listed yet.

    Hence three outcomes, not two, and the caller must not collapse them:
    a tuple means CONFIRMED linked; ``None`` means UNCONFIRMED, which is not
    the same as unlinked and must not re-enable Register on its own.
    """
    pubkey = identity.public_key()
    resolved = resolve_trading_address(pubkey) or {}
    if resolved.get("error"):
        raise PermutoAuthError("venue rejected our pubkey: %s" % resolved["error"])

    # Derived, not looked up -- see above. Useful only as the KEY for the
    # leaderboard search, never as the answer.
    user_id = resolved.get("wallet_user_id")
    address = resolved.get("wallet_address")
    if not isinstance(user_id, str) or not user_id.strip():
        return None
    if not isinstance(address, str) or not address.strip():
        return None

    if leaderboard_entry(user_id) is None:
        return None
    return user_id, address


def register(identity: Any) -> Registration:
    """Link ``identity`` to Permuto and return the session.

    ``identity`` is a :class:`~gui.services.permuto.identity.PermutoIdentity`.
    The private key never leaves it: this function hands it 32 bytes and gets
    a signature back.
    """
    is_open, flags = signup_open()
    if not is_open:
        raise PermutoAuthError(
            "Permuto sign-up is closed (signup_closed=true); a new identity "
            "would be refused at the challenge step"
        )

    pubkey = identity.public_key()

    # Pre-flight: does the venue derive the same address we expect? Cheap,
    # side-effect-free, and it fails loudly here rather than mid-link.
    resolved = resolve_trading_address(pubkey)
    if resolved.get("error"):
        raise PermutoAuthError("venue rejected our pubkey: %s" % resolved["error"])

    challenge = _request(
        "POST", "/exchange/wallet_link_challenge", {"wallet_pubkey": pubkey}
    )
    token = challenge.get("challenge_token")
    nonce_hex = challenge.get("nonce")
    if not token or not nonce_hex:
        raise PermutoAuthError(
            "challenge response missing challenge_token/nonce: %r" % challenge
        )

    # bytes.fromhex raises ValueError (or TypeError on a non-string). Left
    # raw that escapes this module's operator-facing error contract and
    # surfaces an implementation exception for a venue response that should
    # simply be refused.
    try:
        nonce = bytes.fromhex(nonce_hex)
    except (TypeError, ValueError) as exc:
        raise PermutoAuthError(
            "challenge nonce is not hex: %r" % (nonce_hex,)) from exc
    if len(nonce) != 32:
        raise PermutoAuthError(
            "expected a 32-byte nonce, got %d bytes -- refusing to sign an "
            "unexpected message" % len(nonce)
        )

    signature = identity.sign(nonce)

    # Everything above this line is reversible. The call below is not, so the
    # attempt is recorded FIRST -- and recording it is also the write test
    # that decides whether linking is safe at all. If secrets.yaml cannot be
    # written, the link would succeed and the record of it would not, which
    # is the single outcome with no recovery path: a permanently linked key
    # the app believes is free. Failing here turns that into "not linked".
    _note_link_attempt(identity)
    try:
        auth = _request(
            "POST",
            "/exchange/wallet_auth",
            {
                "challenge_token": token,
                "wallet_pubkey": pubkey,
                "signature": signature.hex(),
            },
        )
    except Exception as exc:  # noqa: BLE001 - re-raised, classified
        if isinstance(exc, PermutoAuthError) and _venue_answered_and_refused(exc):
            _clear_link_attempt(identity)
            raise
        raise PermutoLinkIndeterminate(
            "the Permuto link request did not complete cleanly (%s). The key "
            "MAY already be linked -- do NOT register again. Use Recover "
            "registration, which reads the venue back without linking "
            "anything." % exc
        ) from exc

    # [review] The response to the IRREVERSIBLE POST need not be an object.
    # A null or an array made .get raise AttributeError -- after the venue
    # may already have committed -- which escaped the indeterminate handling
    # entirely and was reported as an ordinary failure, re-enabling Register
    # for a key that may now be an account. A parseable 200 of the wrong
    # SHAPE is exactly the "the venue answered, we cannot tell what it did"
    # case, so it is indeterminate.
    if not isinstance(auth, dict):
        raise PermutoLinkIndeterminate(
            "wallet_auth returned %s, not an object -- the venue answered "
            "but the answer cannot be read, so this key may now be linked"
            % type(auth).__name__)
    session = auth.get("session_token") or auth.get("token")
    # As strictly as the identifiers below. A numeric or whitespace-only
    # token is truthy, would be str()'d, and would record a PERMANENT link as
    # successful while holding nothing that can actually authenticate.
    if not isinstance(session, str) or not session.strip():
        raise PermutoAuthError(
            "auth response carried no usable session token (got %r)"
            % (session,))

    # Validate rather than coerce. Empty strings here would sail through the
    # worker as an "ok" result, the UI would report Registered, and nothing
    # would be saved -- a false durable success from a malformed 200.
    user_id = resolved.get("wallet_user_id") or auth.get("user_id")
    trading_address = resolved.get("wallet_address")
    # Type as well as truthiness. A numeric id is truthy, would be str()'d
    # below, and would then be persisted as a successful PERMANENT
    # registration -- Registration declares these as strings and the venue
    # has always sent strings, so anything else is a changed contract we
    # should refuse rather than coerce.
    if (not isinstance(user_id, str) or not isinstance(trading_address, str)
            or not user_id.strip() or not trading_address.strip()):
        raise PermutoAuthError(
            "link succeeded but the venue did not return both identifiers "
            "(user_id=%r, address=%r); refusing to record a registration we "
            "cannot verify" % (user_id, trading_address)
        )

    return Registration(
        user_id=str(user_id),
        trading_address=str(trading_address),
        pubkey=pubkey,
        session_token=str(session),
    )
