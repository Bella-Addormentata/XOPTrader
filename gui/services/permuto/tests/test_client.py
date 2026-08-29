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


def test_a_non_hex_nonce_is_a_permuto_error_not_a_raw_valueerror(monkeypatch):
    """ensure_session() records backoff only for PermutoAuthError.

    A raw ValueError from bytes.fromhex bypassed renewal accounting entirely
    and came back every tick as an unexpected runner error.
    """
    _wire(monkeypatch, {
        "/exchange/wallet_link_challenge": {
            "challenge_token": "ct", "nonce": "zzzz",
        },
    })
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1_000.0)
    with pytest.raises(PermutoAuthError, match="not hex"):
        c.reauth(0.0)


def test_a_bad_nonce_during_renewal_still_records_the_backoff(monkeypatch):
    _wire(monkeypatch, {
        "/exchange/wallet_link_challenge": {
            "challenge_token": "ct", "nonce": None,
        },
    })
    c = PermutoClient(_Identity(), session_token="tok", expires_at_s=1_000.0)
    with pytest.raises(PermutoAuthError):
        c.ensure_session(900.0)
    assert c.session.consecutive_failures == 1


# --------------------------------------------------------------------------- #
# [review] Every route to reauth must be counted, or the backoff is decorative
# --------------------------------------------------------------------------- #

def _client(monkeypatch, responder):
    from gui.services.permuto.client import PermutoClient
    c = PermutoClient(_Identity(), session_token="tok")
    c.session.expires_at_s = 1e12
    monkeypatch.setattr(c, "_request", responder)
    return c


def test_a_failing_retry_reauth_is_charged_to_the_backoff(monkeypatch):
    """_retry_once_on_401 called reauth() DIRECTLY, so a failing renewal left
    consecutive_failures at 0 with `forced` set -- which renew_action answers
    with RENEW immediately, on every 5s tick, for the whole contest."""
    from gui.services.permuto.auth import PermutoAuthError
    from gui.services.permuto.client import PermutoSessionExpired

    def responder(method, path, payload=None):
        raise PermutoSessionExpired("401")

    c = _client(monkeypatch, responder)
    monkeypatch.setattr(
        c, "reauth",
        lambda now_s: (_ for _ in ()).throw(PermutoAuthError("auth down")))

    before = c.session.consecutive_failures
    with pytest.raises(PermutoAuthError):
        c.cancel_all(1000.0)
    assert c.session.consecutive_failures > before, (
        "a failed renewal on the retry path was not charged")
    assert c.session.last_attempt_s == 1000.0


def test_a_401_that_survives_a_fresh_token_escalates_the_backoff(monkeypatch):
    """reauth() zeroes the failure count on success, so a second rejection
    re-set `forced` against a zero count and the next tick renewed at once."""
    from gui.services.permuto.client import PermutoSessionExpired

    def responder(method, path, payload=None):
        raise PermutoSessionExpired("401 again")

    c = _client(monkeypatch, responder)
    c.session.consecutive_failures = 3
    monkeypatch.setattr(c, "reauth", lambda now_s: None)   # succeeds

    with pytest.raises(PermutoSessionExpired):
        c.cancel_all(2000.0)
    assert c.session.consecutive_failures == 4, (
        "the prior count was reset, so the backoff restarts every 5s")


def test_a_keystore_failure_while_signing_becomes_an_auth_error(monkeypatch):
    """identity.sign() reaches DPAPI and can raise something that is not a
    PermutoAuthError -- which escapes ensure_session's accounting entirely, so
    a permanently broken keystore fetches a challenge every tick forever."""
    from gui.services.permuto.auth import PermutoAuthError
    from gui.services.permuto.client import PermutoClient

    class _Broken(_Identity):
        def sign(self, message):
            raise OSError("DPAPI: keyset does not exist")

    c = PermutoClient(_Broken(), session_token="")
    monkeypatch.setattr(
        c, "_request",
        lambda m, p, payload=None, **kw: {"challenge_token": "t",
                                          "nonce": "00" * 32})
    with pytest.raises(PermutoAuthError):
        c.reauth(1.0)


def test_a_timeout_during_the_body_read_is_still_an_auth_error(monkeypatch):
    """[review] urlopen() returning does not end the I/O. resp.read() can
    raise a bare TimeoutError, which is not a URLError -- so it escaped the
    PermutoAuthError contract, bypassed the runner's withdraw path, and was
    never charged to the renewal backoff."""
    import urllib.request

    from gui.services.permuto.auth import PermutoAuthError
    from gui.services.permuto.client import PermutoClient

    class _HangsOnRead:
        def read(self):
            raise TimeoutError("timed out mid-body")

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    monkeypatch.setattr(urllib.request, "urlopen",
                        lambda *a, **k: _HangsOnRead())
    c = PermutoClient(_Identity(), session_token="tok")
    with pytest.raises(PermutoAuthError):
        c.open_orders(1.0)


def test_the_placement_fence_refuses_batches_but_not_cancels(monkeypatch):
    """[review round 9] A tick in flight cannot see the stop flag until it
    returns, so it could resume AFTER join()'s last-resort cancel and place a
    fresh batch -- orders left live while the process exits claiming an empty
    book. The fence sits at the placement chokepoint both threads share, and
    cancels stay open because shutting down IS cancelling."""
    from gui.services.permuto.batch import BatchError
    from gui.services.permuto.client import PermutoClient

    sent = []
    c = PermutoClient(_Identity(), session_token="tok")
    c.session.expires_at_s = 1e12
    monkeypatch.setattr(
        c, "_request",
        lambda m, p, payload=None, **kw: sent.append((m, p)) or {})

    c.halt_placements()
    with pytest.raises(BatchError):
        c.batch_upsert([{"market": "X"}], 1.0)
    assert not sent, "the halted batch still reached the wire"

    c.cancel_all(2.0)
    assert sent and sent[-1][1] == "/exchange/cancel_all", (
        "the fence must not block the cancel it exists to protect")


def test_cold_start_bootstrap_promotes_no_session_to_a_real_reauth(monkeypatch):
    """[release review] The ONE line between the shipped GUI path (PermutoLive
    built with session_token="") and the documented never-places-an-order
    failure is the NO_SESSION -> RENEW promotion. Every other ensure_session
    test constructs a client with a token; nothing pinned the cold start."""
    from gui.services.permuto.client import PermutoClient
    from gui.services.permuto.session import RenewAction

    c = PermutoClient(_Identity(), session_token="")
    reauths = []
    monkeypatch.setattr(c, "reauth", lambda now_s: reauths.append(now_s))
    action = c.ensure_session(1.0)
    assert reauths, "an empty token was treated as terminal again"
    assert action is RenewAction.RENEW


def test_the_fence_also_blocks_the_401_retry_resend(monkeypatch):
    """[review round 10] The fence was checked before the FIRST request only,
    so a shutdown beginning while that request was in flight let the retry
    reauthenticate and place AFTER the last-resort cancel."""
    from gui.services.permuto.batch import BatchError
    from gui.services.permuto.client import (PermutoClient,
                                             PermutoSessionExpired)

    c = PermutoClient(_Identity(), session_token="tok")
    c.session.expires_at_s = 1e12
    sent = []

    def responder(method, path, payload=None, **kw):
        sent.append(path)
        # First send 401s; the shutdown lands while it is in flight.
        c.halt_placements()
        raise PermutoSessionExpired("401")

    monkeypatch.setattr(c, "_request", responder)
    monkeypatch.setattr(c, "reauth", lambda now_s: None)
    with pytest.raises(BatchError):
        c.batch_upsert([{"market": "X"}], 1.0)
    assert sent.count("/exchange/batch_upsert") == 1, (
        "the retry re-placed through the fence")


def test_schedule_cancel_extends_and_clear_omits_time(monkeypatch):
    """Extend-never-rearm: a fresh arm spends one of ten daily triggers,
    re-scheduling while armed is free, and clearing omits `time` per the
    API reference."""
    from gui.services.permuto.client import PermutoClient

    c = PermutoClient(_Identity(), session_token="tok")
    c.session.expires_at_s = 1e12
    sent = []
    monkeypatch.setattr(
        c, "_request",
        lambda m, p, payload=None, **kw: sent.append((p, payload)) or {})

    c.schedule_cancel(1.0, 123_456_000)
    c.clear_schedule_cancel(2.0)
    assert sent[0] == ("/exchange/schedule_cancel", {"time": 123_456_000})
    assert sent[1] == ("/exchange/schedule_cancel", {})


def test_authed_bodies_carry_the_user_id(monkeypatch):
    """[v0.10.4 field report] The first live test order was rejected with
    422 "missing field user_id" on open_orders: the session token
    authenticates the CALL, but the body still names the account. Injected
    at the one door every authenticated request leaves through."""
    from gui.services.permuto.client import PermutoClient

    sent = []
    c = PermutoClient(_Identity(), session_token="tok", user_id="u-123")
    c.session.expires_at_s = 1e12
    monkeypatch.setattr(
        c, "_request",
        lambda m, p, payload=None, **kw: sent.append((p, payload)) or {})

    # _request is stubbed, so drive the injection through the real one by
    # calling the routes and asserting what the stub received... the stub
    # replaces the injector itself, so instead test the injector directly:
    # restore the real _request and capture at the wire.
    import urllib.request

    wire = []

    class _Resp:
        def read(self):
            import json
            return json.dumps({}).encode()

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def fake_urlopen(req, timeout=None):
        import json
        wire.append((req.full_url, json.loads(req.data or b"{}")))
        return _Resp()

    c2 = PermutoClient(_Identity(), session_token="tok", user_id="u-123")
    c2.session.expires_at_s = 1e12
    monkeypatch.setattr(urllib.request, "urlopen", fake_urlopen)

    c2.open_orders(1.0)
    c2.account(2.0)
    c2.cancel_all(3.0)
    c2.schedule_cancel(4.0, 999_000)

    for url, body in wire:
        assert body.get("user_id") == "u-123", (url, body)
    # And the explicit field wins over the injection where one is set.
    assert wire[-1][1]["time"] == 999_000


def test_an_empty_user_id_injects_nothing(monkeypatch):
    """An unregistered identity has no user id; sending an empty one would
    turn a clear 422 into a mystery 4xx."""
    import json
    import urllib.request

    from gui.services.permuto.client import PermutoClient

    wire = []

    class _Resp:
        def read(self):
            return json.dumps({}).encode()

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    monkeypatch.setattr(
        urllib.request, "urlopen",
        lambda req, timeout=None: wire.append(
            json.loads(req.data or b"{}")) or _Resp())

    c = PermutoClient(_Identity(), session_token="tok")
    c.session.expires_at_s = 1e12
    c.open_orders(1.0)
    assert "user_id" not in wire[0]

