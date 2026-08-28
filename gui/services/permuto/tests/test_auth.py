"""Protocol-boundary tests for the link/auth client.

The review was right that this had none. Everything here is a venue-response
shape, and the failure mode they share is the dangerous one: a malformed or
changed 200 that produces a confident "Registered" while nothing was actually
linked or saved. So the assertions are mostly about REFUSING, not succeeding.

``_request`` is monkeypatched throughout -- these tests never touch the
network and must never register anything.
"""

from __future__ import annotations

import urllib.error

import pytest

from gui.services.permuto import auth
from gui.services.permuto.auth import PermutoAuthError, PermutoLinkIndeterminate


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


class RecordingIdentity(FakeIdentity):
    """Adds the durable link-attempt marker the real identity carries."""

    def __init__(self, pubkey="ab" * 48, mark_raises=None):
        super().__init__(pubkey)
        self.link_attempted = False
        self.marks = 0
        self._mark_raises = mark_raises

    def mark_link_attempt(self):
        self.marks += 1
        if self._mark_raises is not None:
            raise self._mark_raises
        self.link_attempted = True

    def clear_link_attempt(self):
        self.link_attempted = False


def _transport_failure(message, cause):
    """A PermutoAuthError shaped exactly as ``_request`` raises one.

    ``_request`` uses ``raise ... from exc``, so the transport error survives
    on ``__cause__`` -- which is the only thing that distinguishes "the venue
    answered and said no" from "we never found out".
    """
    err = PermutoAuthError(message)
    err.__cause__ = cause
    return err


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


# --------------------------------------------------------------------------- #
# The commit boundary -- "we do not know" is not the same as "it did not happen"
# --------------------------------------------------------------------------- #

def test_a_timeout_on_the_commit_call_is_not_reported_as_not_linked(monkeypatch):
    """119-4. A socket timeout AFTER the venue committed raises the identical
    exception as a connection refused BEFORE it. Reported as an ordinary
    failure, the page re-enabled Register for a key that may already be an
    account -- and a second permanent link is the one action with no undo."""
    table = _happy()
    table[("POST", "/exchange/wallet_auth")] = _transport_failure(
        "POST /exchange/wallet_auth unreachable: timed out",
        urllib.error.URLError("timed out"),
    )
    _routes(monkeypatch, table)
    ident = RecordingIdentity()
    with pytest.raises(PermutoLinkIndeterminate, match="MAY already be linked"):
        auth.register(ident)
    # The marker outlives the process, which is the point: a crash here must
    # not leave the next launch believing the key is free.
    assert ident.link_attempted is True


def test_a_5xx_on_the_commit_call_is_indeterminate_too(monkeypatch):
    """A 500 can be raised after the write. Only a 4xx is an answer."""
    table = _happy()
    table[("POST", "/exchange/wallet_auth")] = _transport_failure(
        "POST /exchange/wallet_auth -> HTTP 502",
        urllib.error.HTTPError("u", 502, "bad gateway", {}, None),
    )
    _routes(monkeypatch, table)
    ident = RecordingIdentity()
    with pytest.raises(PermutoLinkIndeterminate):
        auth.register(ident)
    assert ident.link_attempted is True


def test_a_4xx_refusal_stays_an_ordinary_failure(monkeypatch):
    """The venue understood the request and turned it down, so nothing was
    linked and the marker must not strand the page in recovery mode."""
    table = _happy()
    table[("POST", "/exchange/wallet_auth")] = _transport_failure(
        "POST /exchange/wallet_auth -> HTTP 403 signature rejected",
        urllib.error.HTTPError("u", 403, "forbidden", {}, None),
    )
    _routes(monkeypatch, table)
    ident = RecordingIdentity()
    with pytest.raises(PermutoAuthError) as caught:
        auth.register(ident)
    assert not isinstance(caught.value, PermutoLinkIndeterminate)
    assert ident.link_attempted is False


def test_the_attempt_is_recorded_before_the_commit_call(monkeypatch):
    """Recorded after would leave exactly the window this marker closes."""
    calls = _routes(monkeypatch, _happy())

    class Ordered(RecordingIdentity):
        marked_after = None

        def mark_link_attempt(self):
            self.marked_after = list(calls)
            super().mark_link_attempt()

    ident = Ordered()
    auth.register(ident)
    assert ident.marks == 1
    assert not any(c[1].startswith("/exchange/wallet_auth")
                   for c in ident.marked_after)
    assert any(c[1].startswith("/exchange/wallet_auth") for c in calls)


def test_an_unwritable_secrets_file_stops_the_link_before_it_commits(monkeypatch):
    """Fail closed. Linking succeeds and saving it does not is the one
    outcome with no recovery path, so the write is tested BEFORE the
    irreversible request rather than after it."""
    calls = _routes(monkeypatch, _happy())
    ident = RecordingIdentity(mark_raises=OSError("secrets.yaml is read-only"))
    with pytest.raises(OSError):
        auth.register(ident)
    assert not any(c[1].startswith("/exchange/wallet_auth") for c in calls)


def test_a_malformed_200_leaves_the_attempt_marked(monkeypatch):
    """A 200 means the venue committed. If its body is then unusable we have
    a linked key and no identifiers -- the marker is all that says so."""
    table = _happy()
    table[("POST", "/exchange/wallet_auth")] = {"session_token": ""}
    _routes(monkeypatch, table)
    ident = RecordingIdentity()
    with pytest.raises(PermutoAuthError, match="session token"):
        auth.register(ident)
    assert ident.link_attempted is True


# --------------------------------------------------------------------------- #
# Reconciliation -- read the venue back WITHOUT linking anything
# --------------------------------------------------------------------------- #

def test_reconcile_confirms_a_key_that_is_on_the_leaderboard(monkeypatch):
    calls = _routes(monkeypatch, {
        ("GET", "/info/wallet_bls_trading_address"): RESOLVED,
        ("GET", "/exchange/leaderboard"): {
            "market_makers": [{"user_id": "c" * 64}], "traders": [],
        },
    })
    assert auth.reconcile_registration(FakeIdentity()) == (
        "c" * 64, "xch1example"
    )
    paths = [c[1].split("?")[0] for c in calls]
    # Both routes are reads. Reconciling must never be a way of linking.
    assert paths == ["/info/wallet_bls_trading_address",
                     "/exchange/leaderboard"]


def test_a_derived_user_id_alone_is_not_proof_of_a_link(monkeypatch):
    """MEASURED 2026-08-28, and it is the whole reason this is two calls.

    /info/wallet_bls_trading_address DERIVES wallet_user_id from the pubkey
    rather than looking up a registration: a freshly generated key that has
    never been sent to the venue comes back with a populated wallet_user_id
    and wallet_address. Treating that as "already linked" reports EVERY valid
    key as registered -- and the caller then records a registration that
    never happened and disables Register for an account that does not exist.
    """
    _routes(monkeypatch, {
        ("GET", "/info/wallet_bls_trading_address"): RESOLVED,
        ("GET", "/exchange/leaderboard"): {"market_makers": [], "traders": []},
    })
    assert auth.reconcile_registration(FakeIdentity()) is None


def test_reconcile_says_none_rather_than_inventing_a_registration(monkeypatch):
    """Recording a link that never happened would permanently disable
    Register for an account that does not exist, so the unknown answer is
    'not linked'."""
    _routes(monkeypatch, {
        ("GET", "/info/wallet_bls_trading_address"): {
            "wallet_address": "xch1example", "wallet_pubkey": "ab" * 48,
        },
    })
    assert auth.reconcile_registration(FakeIdentity()) is None


@pytest.mark.parametrize("tok", [12345, "", "   ", None, {"t": 1}])
def test_an_unusable_session_token_is_refused(monkeypatch, tok):
    """A numeric or blank token is truthy, would be str()'d, and would record
    a PERMANENT link as successful while holding nothing that can
    authenticate. Same strictness as the identifiers."""
    table = _happy()
    table[("POST", "/exchange/wallet_auth")] = {"session_token": tok}
    _routes(monkeypatch, table)
    with pytest.raises(PermutoAuthError, match="session token"):
        auth.register(FakeIdentity())
