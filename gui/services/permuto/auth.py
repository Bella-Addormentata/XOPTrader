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


def _request(method: str, path: str, payload: Optional[dict] = None) -> Any:
    body = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(
        BASE_URL + path, data=body, headers=_HEADERS, method=method
    )
    try:
        with urllib.request.urlopen(req, timeout=_TIMEOUT) as resp:
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
    flags = (_request("GET", "/info/meta") or {}).get("flags") or {}
    return (not flags.get("signup_closed", False)), flags


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
    while offset <= 2000:
        page = _request(
            "GET", "/exchange/leaderboard?limit=100&offset=%d" % offset
        )
        for bucket in ("market_makers", "traders"):
            for row in page.get(bucket, []):
                if row.get("user_id") == user_id:
                    return {"category": bucket, **row}
        seen = offset + max(
            len(page.get("market_makers", [])), len(page.get("traders", []))
        )
        total = max(
            page.get("market_makers_total", 0), page.get("traders_total", 0)
        )
        if seen >= total:
            return None
        offset += 100
    return None


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

    nonce = bytes.fromhex(nonce_hex)
    if len(nonce) != 32:
        raise PermutoAuthError(
            "expected a 32-byte nonce, got %d bytes -- refusing to sign an "
            "unexpected message" % len(nonce)
        )

    signature = identity.sign(nonce)

    auth = _request(
        "POST",
        "/exchange/wallet_auth",
        {
            "challenge_token": token,
            "wallet_pubkey": pubkey,
            "signature": signature.hex(),
        },
    )
    session = auth.get("session_token") or auth.get("token")
    if not session:
        raise PermutoAuthError("auth response carried no session token: %r" % auth)

    return Registration(
        user_id=str(resolved.get("wallet_user_id") or auth.get("user_id") or ""),
        trading_address=str(resolved.get("wallet_address") or ""),
        pubkey=pubkey,
        session_token=str(session),
    )
