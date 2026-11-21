"""The Circuit page's reads: which ones, in what order, and how a round stops."""

from __future__ import annotations

import http.server
import json
import threading
from typing import Any

import pytest

from gui.services.circuit import api, poll
from gui.services.circuit.api import CircuitApi, CircuitApiError


class Transport:
    """A scripted transport: (status, body) by path, or an exception to raise."""

    def __init__(self, answers: dict[str, Any]) -> None:
        self.answers = answers
        self.calls: list[tuple[str, str, Any, dict[str, str]]] = []

    def __call__(self, method, url, body, headers, timeout_s):
        path = url.split("://", 1)[1].split("/", 1)[1]
        self.calls.append((method, "/" + path, json.loads(body) if body else None, headers))
        answer = self.answers.get("/" + path, (404, {"detail": "Not Found"}))
        if isinstance(answer, Exception):
            raise answer
        status, payload = answer
        text = payload if isinstance(payload, str) else json.dumps(payload)
        return status, {"content-type": "application/json"}, text.encode()


def _client(transport: Transport) -> CircuitApi:
    return CircuitApi("https://api.example.test", min_interval_s=0, transport=transport)


def test_the_page_makes_eight_of_the_probe_s_reads_with_the_same_bodies() -> None:
    assert [read.name for read in poll.PAGE_READS] == list(poll.PAGE_READ_NAMES)
    assert len(poll.PAGE_READS) == 8
    probe = {read.name: read for read in api.READS}
    for read in poll.PAGE_READS:
        assert read is probe[read.name]
        assert not api._wallet_fields(read.body), read.name


def test_a_round_asks_each_read_in_turn_and_reports_each() -> None:
    answers = {read.path: (200, {"read": read.name}) for read in poll.PAGE_READS}
    transport = Transport(answers)
    results: list[poll.Result] = []
    answered = poll.fetch(_client(transport), on_result=results.append, wall=lambda: 7.0)
    assert answered == 8
    assert [call[1] for call in transport.calls] == [read.path for read in poll.PAGE_READS]
    assert [result.name for result in results] == list(poll.PAGE_READ_NAMES)
    assert all(result.ok and result.at == 7.0 for result in results)
    assert results[0].data == {"read": "oracle"}
    # The bodies the probe sent to the live server.
    sent = {call[1]: call[2] for call in transport.calls}
    assert sent["/statutes"] == {"full": True}
    assert sent["/bills"] == {"synthetic_pks": [], "include_spent_coins": False}


def test_a_read_that_fails_is_reported_and_the_round_goes_on() -> None:
    answers = {read.path: (200, []) for read in poll.PAGE_READS}
    answers["/statutes"] = (503, {"detail": "maintenance"})
    answers["/vaults"] = (200, "<html>not json</html>")
    answers["/treasury"] = TimeoutError("timed out")
    results: list[poll.Result] = []
    answered = poll.fetch(_client(Transport(answers)), on_result=results.append)
    assert answered == 5
    errors = {result.name: result.error for result in results if not result.ok}
    assert errors["statutes"].startswith("POST /statutes: HTTP 503")
    assert "not JSON" in errors["vaults"]
    assert errors["treasury"].startswith("POST /treasury: no answer (TimeoutError")
    assert len(results) == 8


def test_a_round_ends_between_reads_when_told_to() -> None:
    answers = {read.path: (200, []) for read in poll.PAGE_READS}
    transport = Transport(answers)
    results: list[poll.Result] = []
    poll.fetch(_client(transport), on_result=results.append, should_stop=lambda: len(results) >= 2)
    assert len(transport.calls) == 2 and len(results) == 2


def test_the_page_s_client_stops_waiting_the_moment_it_is_told() -> None:
    stop = threading.Event()
    transport = Transport({"/oracle": (200, {}), "/statutes": (200, {})})
    client = poll.page_client(stop, transport=transport)
    assert client.base_url == api.DEFAULT_BASE_URL
    client.post("/oracle")
    stop.set()
    # The next request would wait 3.2 s for its turn; it is not sent at all.
    with pytest.raises(poll.Stopped):
        client.post("/statutes")
    assert len(transport.calls) == 1
    assert transport.calls[0][3]["User-Agent"] == poll.PAGE_USER_AGENT


class _Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802 -- http.server's name
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length)
        if self.path == "/moved":
            self.send_response(302)
            self.send_header("Location", "http://example.invalid/elsewhere")
            self.end_headers()
            return
        status = 503 if self.path == "/down" else 200
        payload = json.dumps({"path": self.path, "body": json.loads(body or b"null")}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args: Any) -> None:
        pass


@pytest.fixture
def local_server(monkeypatch):
    for name in (
        "HTTP_PROXY",
        "http_proxy",
        "HTTPS_PROXY",
        "https_proxy",
        "ALL_PROXY",
        "all_proxy",
    ):
        monkeypatch.delenv(name, raising=False)
    monkeypatch.setenv("NO_PROXY", "127.0.0.1,localhost")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        server.server_close()


def test_the_requests_transport_returns_every_status_and_follows_no_redirect(local_server) -> None:
    pytest.importorskip("requests")
    client = CircuitApi(local_server, min_interval_s=0, transport=poll.requests_transport)
    assert client.post("/oracle", {"x": 1}) == {"path": "/oracle", "body": {"x": 1}}
    down = client.request("POST", "/down", {})
    assert down.status == 503 and json.loads(down.text)["path"] == "/down"
    moved = client.request("POST", "/moved", {})
    assert moved.status == 302
    with pytest.raises(CircuitApiError, match="HTTP 302"):
        client.post("/moved")
