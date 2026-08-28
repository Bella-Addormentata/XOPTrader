"""Protocol-boundary tests for the link/auth client.

The review was right that this had none. Everything here is a venue-response
shape, and the failure mode they share is the dangerous one: a malformed or
changed 200 that produces a confident "Registered" while nothing was actually
linked or saved. So the assertions are mostly about REFUSING, not succeeding.

``_request`` is monkeypatched throughout -- these tests never touch the
network and must never register anything.
"""

from __future__ import annotations

import pytest

from gui.services.permuto import auth
from gui.services.permuto.auth import PermutoAuthError


class FakeIdentity:
    """Signs deterministically; records what it was asked to sign."""

    def __init__(self, pubkey="ab" * 48):
        self._pubkey = pubkey
        self.signed: list[bytes] = []

    def public_key(self):
        return self._pubkey

    def sign(self, message: bytes) -> bytes:
        self.signed.append(message)
        return b"\x11" * 96


def _routes(monkeypatch, table):
    """Route (method, path-prefix) -> response or exception."""
    calls = []

    def fake(method, path, payload=None):
        calls.append((method, path, payload))
        for (m, prefix), value in table.items():
            if method == m and path.startswith(prefix):
                if isinstance(value, Exception):
                    raise value
                return value
        raise AssertionError("unexpected call %s %s" % (method, path))

    monkeypatch.setattr(auth, "_request", fake)
    return calls


OPEN_META = {"flags": {"signup_closed": False}}
RESOLVED = {
    "wallet_user_id": "c" * 64,
    "wallet_address": "xch1example",
    "wallet_pubkey": "ab" * 48,
}


def _happy(nonce_hex="ab" * 32):
    return {
        ("GET", "/info/meta"): OPEN_META,
        ("GET", "/info/wallet_bls_trading_address"): RESOLVED,
        ("POST", "/exchange/wallet_link_challenge"): {
            "challenge_token": "tok", "nonce": nonce_hex,
        },
        ("POST", "/exchange/wallet_auth"): {"session_token": "sess"},
    }


# --------------------------------------------------------------------------- #
# Signup gating -- refuse BEFORE signing
# --------------------------------------------------------------------------- #

def test_closed_signup_refuses_before_signing(monkeypatch):
    ident = FakeIdentity()
    calls = _routes(monkeypatch, {("GET", "/info/meta"):
                                  {"flags": {"signup_closed": True}}})
    with pytest.raises(PermutoAuthError, match="closed"):
        auth.register(ident)
    assert ident.signed == []          # never signed
    assert len(calls) == 1             # never reached the challenge


# --------------------------------------------------------------------------- #
# The nonce
# --------------------------------------------------------------------------- #

def test_the_signed_message_is_the_decoded_nonce_not_its_hex(monkeypatch):
    """The single easiest thing to get wrong here.

    Signing the ASCII of the hex string yields a perfectly valid signature
    over the wrong message, and the venue rejects it with nothing useful.
    """
    nonce = bytes(range(32))
    ident = FakeIdentity()
    _routes(monkeypatch, _happy(nonce.hex()))
    auth.register(ident)
    assert ident.signed == [nonce]
    assert ident.signed[0] != nonce.hex().encode()


def test_a_wrong_length_nonce_is_refused_unsigned(monkeypatch):
    ident = FakeIdentity()
    _routes(monkeypatch, _happy("ab" * 16))       # 16 bytes, not 32
    with pytest.raises(PermutoAuthError, match="32-byte"):
        auth.register(ident)
    assert ident.signed == []


def test_a_missing_nonce_is_refused(monkeypatch):
    ident = FakeIdentity()
    table = _happy()
    table[("POST", "/exchange/wallet_link_challenge")] = {"challenge_token": "t"}
    _routes(monkeypatch, table)
    with pytest.raises(PermutoAuthError, match="challenge_token/nonce"):
        auth.register(ident)
    assert ident.signed == []


# --------------------------------------------------------------------------- #
# Response validation -- the false-success class
# --------------------------------------------------------------------------- #

def test_missing_session_token_is_refused(monkeypatch):
    _routes(monkeypatch, {**_happy(), ("POST", "/exchange/wallet_auth"): {}})
    with pytest.raises(PermutoAuthError, match="session token"):
        auth.register(FakeIdentity())


def test_missing_identifiers_are_refused_rather_than_emptied(monkeypatch):
    """A 200 without identifiers previously became user_id="" and an ok
    result, so the UI said Registered and saved nothing."""
    table = _happy()
    table[("GET", "/info/wallet_bls_trading_address")] = {"wallet_pubkey": "ab" * 48}
    table[("POST", "/exchange/wallet_auth")] = {"session_token": "sess"}
    _routes(monkeypatch, table)
    with pytest.raises(PermutoAuthError, match="identifiers"):
        auth.register(FakeIdentity())


def test_a_rejected_pubkey_fails_before_the_challenge(monkeypatch):
    table = _happy()
    table[("GET", "/info/wallet_bls_trading_address")] = {
        "error": "wallet_pubkey must be 96 hex characters"
    }
    calls = _routes(monkeypatch, table)
    with pytest.raises(PermutoAuthError, match="rejected our pubkey"):
        auth.register(FakeIdentity())
    assert not any("challenge" in c[1] for c in calls)


def test_success_returns_the_venue_identifiers(monkeypatch):
    _routes(monkeypatch, _happy())
    reg = auth.register(FakeIdentity())
    assert reg.user_id == "c" * 64
    assert reg.trading_address == "xch1example"
    assert reg.session_token == "sess"


def test_the_session_token_never_appears_in_repr(monkeypatch):
    """Registration lands in log lines and tracebacks; the token must not."""
    _routes(monkeypatch, _happy())
    reg = auth.register(FakeIdentity())
    assert "sess" not in repr(reg)
    assert "sess" not in str(reg)


# --------------------------------------------------------------------------- #
# Pagination
# --------------------------------------------------------------------------- #

def _board(total, page_rows, offset):
    return {
        "market_makers": page_rows,
        "traders": [],
        "market_makers_total": total,
        "traders_total": 0,
        "offset": offset,
    }


def test_leaderboard_pages_past_the_first(monkeypatch):
    """Reading page one only is how this project misread the field by 35x."""
    target = {"user_id": "z" * 64, "depth_seconds_5d": 1}

    def fake(method, path, payload=None):
        offset = int(path.split("offset=")[1])
        rows = [{"user_id": "%064d" % (offset + i)} for i in range(100)]
        if offset == 200:
            rows[7] = target
        return _board(400, rows, offset)

    monkeypatch.setattr(auth, "_request", fake)
    assert auth.leaderboard_entry("z" * 64) == {"category": "market_makers",
                                                **target}


def test_leaderboard_has_no_silent_result_cap(monkeypatch):
    """An offset ceiling reported a registered account as UNLISTED -- a
    failure that reads as 'you are not registered'."""
    target = {"user_id": "y" * 64}
    seen_offsets = []

    def fake(method, path, payload=None):
        offset = int(path.split("offset=")[1])
        seen_offsets.append(offset)
        rows = [{"user_id": "%064d" % (offset + i)} for i in range(100)]
        if offset == 3000:
            rows[0] = target
        return _board(3200, rows, offset)

    monkeypatch.setattr(auth, "_request", fake)
    assert auth.leaderboard_entry("y" * 64) is not None
    assert max(seen_offsets) >= 3000


def test_leaderboard_absent_returns_none(monkeypatch):
    def fake(method, path, payload=None):
        offset = int(path.split("offset=")[1])
        return _board(100, [{"user_id": "%064d" % i} for i in range(100)], offset)

    monkeypatch.setattr(auth, "_request", fake)
    assert auth.leaderboard_entry("missing") is None


def test_null_totals_do_not_crash_or_truncate(monkeypatch):
    """`.get(key, 0)` returns None for a key present with a JSON null, and
    max(None, 50) raises TypeError -- reporting "Failed" for a registered
    account. Coercing to 0 alone is not enough either: total==0 would end the
    search after page one, failing as "not registered"."""
    target = {"user_id": "n" * 64}

    def fake(method, path, payload=None):
        offset = int(path.split("offset=")[1])
        rows = [{"user_id": "%064d" % (offset + i)} for i in range(100)]
        if offset == 200:
            rows[3] = target
        if offset >= 300:
            rows = []
        return {"market_makers": rows, "traders": [],
                "market_makers_total": None, "traders_total": None}

    monkeypatch.setattr(auth, "_request", fake)
    assert auth.leaderboard_entry("n" * 64) is not None


def test_a_short_page_ends_the_search(monkeypatch):
    """Without a usable total, a page shorter than the batch is the end."""
    calls = []

    def fake(method, path, payload=None):
        calls.append(path)
        return {"market_makers": [], "traders": [],
                "market_makers_total": None, "traders_total": None}

    monkeypatch.setattr(auth, "_request", fake)
    assert auth.leaderboard_entry("absent") is None
    assert len(calls) == 1


@pytest.mark.parametrize("bad_id,bad_addr", [
    (12345, "xch1example"),
    ("c" * 64, 99),
    ("", "xch1example"),
    ("   ", "xch1example"),
])
def test_non_string_identifiers_are_refused(monkeypatch, bad_id, bad_addr):
    """Truthiness alone accepted a numeric id, str()'d it, and persisted it as
    a successful PERMANENT registration. Registration declares strings and the
    venue has always sent strings, so anything else is a changed contract to
    refuse rather than coerce."""
    table = _happy()
    table[("GET", "/info/wallet_bls_trading_address")] = {
        "wallet_user_id": bad_id, "wallet_address": bad_addr,
    }
    _routes(monkeypatch, table)
    with pytest.raises(PermutoAuthError, match="identifiers"):
        auth.register(FakeIdentity())


@pytest.mark.parametrize("meta", [
    {}, {"flags": {}}, {"flags": None},
    {"flags": {"signup_closed": "false"}},
    {"flags": {"signup_closed": 0}},
])
def test_an_unknown_signup_state_fails_closed(monkeypatch, meta):
    """The gate used to read {} and a missing/non-boolean flag as "open", and
    the very next call performs a PERMANENT link."""
    ident = FakeIdentity()
    _routes(monkeypatch, {("GET", "/info/meta"): meta})
    with pytest.raises(PermutoAuthError):
        auth.register(ident)
    assert ident.signed == []
