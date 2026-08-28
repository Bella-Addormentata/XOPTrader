"""Transport tests. The socket is faked; everything above it is real."""

from __future__ import annotations

import io
import json
import urllib.error

import pytest

from gui.services.permuto import client as client_mod
from gui.services.permuto.auth import PermutoAuthError
from gui.services.permuto.client import (
    DEFAULT_SESSION_TTL_S,
    PermutoClient,
    PermutoNotLinked,
    PermutoSessionExpired,
    _expiry_from,
)
from gui.services.permuto.session import RenewAction

_NONCE = "11" * 32


class _Identity:
    def public_key(self):
        return "aa" * 48

    def sign(self, message):
        assert len(message) == 32
        return b"\x01" * 96


class _Resp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class _Fake:
    """Records requests and replays scripted answers."""

    def __init__(self, routes, errors=None):
        self.routes = routes
        self.errors = errors or {}
        self.calls = []

    def __call__(self, req, timeout=None):
        path = req.full_url.split("permuto.capital", 1)[-1]
        self.calls.append((req.method, path, dict(req.headers)))
        queue = self.errors.get(path)
        if queue:
            code = queue.pop(0)
            raise urllib.error.HTTPError(
                req.full_url, code, "no", {}, io.BytesIO(b"denied")
            )
        body = self.routes.get(path, {})
        return _Resp(json.dumps(body).encode())


def _wire(monkeypatch, routes=None, errors=None):
    routes = dict(routes or {})
    routes.setdefault(
        "/exchange/wallet_link_challenge",
        {"challenge_token": "ct", "nonce": _NONCE},
    )
    routes.setdefault("/exchange/wallet_auth", {"session_token": "fresh"})
    fake = _Fake(routes, errors)
    monkeypatch.setattr(client_mod.urllib.request, "urlopen", fake)
    return fake


# --------------------------------------------------------------------------- #
# The trap: renewal must survive registration closing mid-contest
# --------------------------------------------------------------------------- #
def test_reauth_does_not_consult_the_signup_gate(monkeypatch):
    """Sign-up closes Monday 17:00 ET; the contest runs to Friday.

    If renewal went through register(), every renewal after Monday evening
    would fail and the book would stay drained for four days.
    """
    _wire(monkeypatch, {"/info/meta": {"signup_closed": True}})

    def _boom(*a, **k):  # pragma: no cover - must never run
        raise AssertionError("renewal called the sign-up gate")

    monkeypatch.setattr(client_mod, "PermutoAuthError", PermutoAuthError)
    import gui.services.permuto.auth as auth_mod
    monkeypatch.setattr(auth_mod, "signup_open", _boom)

    c = PermutoClient(_Identity())
    c.reauth(1_000.0)
    assert c.session.token == "fresh"


def test_reauth_clears_forced_and_failures(monkeypatch):
    _wire(monkeypatch)
    c = PermutoClient(_Identity(), session_token="old")
    c.session.forced = True
    c.session.consecutive_failures = 4
    c.reauth(500.0)
    assert (c.session.token, c.session.forced) == ("fresh", False)
    assert c.session.consecutive_failures == 0
    assert c.session.last_attempt_s == 500.0


def test_reauth_refuses_a_nonce_that_is_not_32_bytes(monkeypatch):
    _wire(monkeypatch, {
        "/exchange/wallet_link_challenge": {
            "challenge_token": "ct", "nonce": "aabb"
        },
    })
    with pytest.raises(PermutoAuthError, match="32-byte"):
        PermutoClient(_Identity()).reauth(0.0)


def test_reauth_rejects_a_blank_session_token(monkeypatch):
    _wire(monkeypatch, {"/exchange/wallet_auth": {"session_token": "   "}})
    with pytest.raises(PermutoAuthError, match="no usable session token"):
        PermutoClient(_Identity()).reauth(0.0)


# --------------------------------------------------------------------------- #
# Headers and 401 handling
# --------------------------------------------------------------------------- #
def test_authed_request_carries_bearer_and_challenge_does_not(monkeypatch):
    fake = _wire(monkeypatch, {"/exchange/batch_upsert": {"ok": True}})
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    c.batch_upsert([{"market": "QQQ-VOL-PERP"}], 0.0)
    c.reauth(0.0)

    by_path = {p: h for _, p, h in fake.calls}
    assert by_path["/exchange/batch_upsert"]["Authorization"] == "Bearer tok"
    assert "Authorization" not in by_path["/exchange/wallet_link_challenge"]


def test_holding_no_token_is_not_silently_authenticated(monkeypatch):
    """The retry path must not paper over "never linked".

    quoting.decide() maps no-session to WITHDRAW. If the transport minted a
    token here it would place orders behind a policy that had just decided to
    leave the market.
    """
    fake = _wire(monkeypatch)
    c = PermutoClient(_Identity())
    with pytest.raises(PermutoNotLinked):
        c.batch_upsert([{"m": 1}], 0.0)
    assert fake.calls == []


@pytest.mark.parametrize("code", [401, 403])
def test_rejected_session_sets_forced(monkeypatch, code):
    """Believe the server over our own clock."""
    _wire(monkeypatch, errors={"/exchange/batch_upsert": [code, code]})
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    with pytest.raises(PermutoSessionExpired):
        c.batch_upsert([{"m": 1}], 0.0)
    assert c.session.forced is True


def test_non_auth_http_error_does_not_touch_the_session(monkeypatch):
    _wire(monkeypatch, errors={"/exchange/batch_upsert": [400]})
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    with pytest.raises(PermutoAuthError) as exc:
        c.batch_upsert([{"m": 1}], 0.0)
    assert not isinstance(exc.value, PermutoSessionExpired)
    assert c.session.forced is False


def test_401_triggers_exactly_one_reauth_and_retry(monkeypatch):
    fake = _wire(monkeypatch, {"/exchange/batch_upsert": {"ok": True}},
                 errors={"/exchange/batch_upsert": [401]})
    c = PermutoClient(_Identity(), session_token="stale", expires_at_s=1e12)
    assert c.batch_upsert([{"m": 1}], 7.0) == {"ok": True}

    paths = [p for _, p, _ in fake.calls]
    assert paths.count("/exchange/batch_upsert") == 2
    assert paths.count("/exchange/wallet_auth") == 1
    # The retry must use the NEW token, not the one that was just rejected.
    assert fake.calls[-1][2]["Authorization"] == "Bearer fresh"


def test_a_401_that_survives_reauth_is_not_retried_forever(monkeypatch):
    fake = _wire(monkeypatch,
                 errors={"/exchange/batch_upsert": [401, 401, 401]})
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    with pytest.raises(PermutoSessionExpired):
        c.batch_upsert([{"m": 1}], 0.0)
    assert [p for _, p, _ in fake.calls].count("/exchange/batch_upsert") == 2


# --------------------------------------------------------------------------- #
# Trading routes
# --------------------------------------------------------------------------- #
def test_an_empty_batch_sends_nothing(monkeypatch):
    fake = _wire(monkeypatch)
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    assert c.batch_upsert([], 0.0) == {}
    assert fake.calls == []


def test_cancel_all_omits_markets_when_unscoped(monkeypatch):
    sent = []

    class _Cap(_Fake):
        def __call__(self, req, timeout=None):
            sent.append(json.loads(req.data.decode()))
            return super().__call__(req, timeout)

    fake = _Cap({})
    monkeypatch.setattr(client_mod.urllib.request, "urlopen", fake)
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    c.cancel_all(0.0)
    c.cancel_all(0.0, ["QQQ-VOL-PERP"])
    assert sent == [{}, {"markets": ["QQQ-VOL-PERP"]}]


def test_empty_response_body_is_not_a_json_error(monkeypatch):
    class _Blank(_Fake):
        def __call__(self, req, timeout=None):
            self.calls.append((req.method, "x", {}))
            return _Resp(b"")

    monkeypatch.setattr(client_mod.urllib.request, "urlopen", _Blank({}))
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    assert c.cancel_all(0.0) == {}


def test_spa_fallback_reads_as_a_wrong_path_not_bad_json(monkeypatch):
    """Unknown paths return HTTP 200 and an HTML document.

    Probed live: /exchange/positions, /exchange/balances and /info/account all
    answer 200 with the web app. Only 405-on-GET proves an API route exists.
    """
    class _Html(_Fake):
        def __call__(self, req, timeout=None):
            self.calls.append((req.method, "x", {}))
            return _Resp(b'<!doctype html> <html lang="en"><head>')

    monkeypatch.setattr(client_mod.urllib.request, "urlopen", _Html({}))
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    with pytest.raises(PermutoAuthError, match="not an API route"):
        c.account(0.0)


def test_unparseable_json_is_still_a_permuto_error(monkeypatch):
    class _Junk(_Fake):
        def __call__(self, req, timeout=None):
            self.calls.append((req.method, "x", {}))
            return _Resp(b"{not json")

    monkeypatch.setattr(client_mod.urllib.request, "urlopen", _Junk({}))
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    with pytest.raises(PermutoAuthError, match="unparseable"):
        c.account(0.0)


def test_account_and_open_orders_post_to_confirmed_routes(monkeypatch):
    """Both answer 405 to GET, which is how we know they exist as POST."""
    fake = _wire(monkeypatch, {
        "/exchange/account": {"equity_usd": 1000.0},
        "/exchange/open_orders": {"orders": []},
    })
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1e12)
    assert c.account(0.0) == {"equity_usd": 1000.0}
    assert c.open_orders(0.0) == {"orders": []}
    assert [(m, p) for m, p, _ in fake.calls] == [
        ("POST", "/exchange/account"),
        ("POST", "/exchange/open_orders"),
    ]


# --------------------------------------------------------------------------- #
# ensure_session
# --------------------------------------------------------------------------- #
def test_ensure_session_leaves_a_healthy_session_alone(monkeypatch):
    fake = _wire(monkeypatch)
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=10_000.0)
    assert c.ensure_session(0.0) is RenewAction.OK
    assert fake.calls == []


def test_ensure_session_renews_inside_the_margin(monkeypatch):
    _wire(monkeypatch)
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1_000.0)
    assert c.ensure_session(800.0) is RenewAction.RENEW
    assert c.session.token == "fresh"


def test_ensure_session_reports_wait_without_calling_out(monkeypatch):
    fake = _wire(monkeypatch)
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1_000.0)
    c.session.consecutive_failures = 3
    c.session.last_attempt_s = 100.0
    assert c.ensure_session(101.0) is RenewAction.WAIT
    assert fake.calls == []


def test_a_failed_renewal_records_the_attempt_for_backoff(monkeypatch):
    _wire(monkeypatch, errors={"/exchange/wallet_link_challenge": [500]})
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1_000.0)
    with pytest.raises(PermutoAuthError):
        c.ensure_session(900.0)
    assert c.session.consecutive_failures == 1
    assert c.session.last_attempt_s == 900.0


# --------------------------------------------------------------------------- #
# Expiry parsing
# --------------------------------------------------------------------------- #
def test_absolute_expiry_is_taken_as_given():
    assert _expiry_from({"expires_at": 5_000.0}, 100.0) == 5_000.0


def test_relative_expiry_is_anchored_to_now():
    assert _expiry_from({"expires_in": 60}, 100.0) == 160.0


def test_a_silent_venue_gets_a_conservative_default():
    """0.0 would make the policy renew on EVERY tick of a 5s loop."""
    assert _expiry_from({}, 100.0) == 100.0 + DEFAULT_SESSION_TTL_S


def test_a_boolean_is_not_an_expiry():
    """bool is a subclass of int; True would become an expiry of 1.0."""
    assert _expiry_from({"expires_at": True}, 100.0) == (
        100.0 + DEFAULT_SESSION_TTL_S
    )
