"""The read-only Circuit API client and its probe, against a scripted server."""

from __future__ import annotations

import ast
import json
from pathlib import Path
from typing import Any

import pytest

from gui.services.circuit import api
from gui.services.circuit.api import (
    MIN_INTERVAL_S,
    READS,
    WAIT_ON_429_S,
    CircuitApi,
    CircuitApiError,
    Read,
)

BASE = "https://api.example.test"


class Clock:
    """Time that moves only when someone sleeps (or a test says so)."""

    def __init__(self) -> None:
        self.now = 1000.0
        self.slept: list[float] = []

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.slept.append(seconds)
        self.now += seconds


class InTurn:
    """Answers served one after another for a path; the last one repeats."""

    def __init__(self, *answers: Any) -> None:
        self.answers = list(answers)

    def next(self) -> Any:
        return self.answers.pop(0) if len(self.answers) > 1 else self.answers[0]


class Server:
    """A scripted transport: answers by path, records every request."""

    def __init__(self, answers: dict[str, Any] | None = None) -> None:
        # path -> (status, body[, headers]), an InTurn of them, or an
        # Exception to raise. A bare body (dict, list, str) is a 200;
        # a str body is sent as it is, anything else as JSON.
        self.answers = dict(answers or {})
        self.calls: list[dict[str, Any]] = []

    def __call__(
        self, method: str, url: str, body: bytes | None, headers: dict[str, str], timeout_s: float
    ) -> tuple[int, dict[str, str], bytes]:
        path = url[len(BASE):]
        self.calls.append(
            {
                "method": method,
                "url": url,
                "path": path,
                "body": None if body is None else json.loads(body),
                "headers": dict(headers),
                "timeout_s": timeout_s,
            }
        )
        answer = self.answers.get(path, (404, '{"detail":"Not Found"}'))
        if isinstance(answer, InTurn):
            answer = answer.next()
        if isinstance(answer, Exception):
            raise answer
        if isinstance(answer, tuple):
            status, payload = answer[0], answer[1]
            extra = answer[2] if len(answer) > 2 else {}
        else:
            status, payload, extra = 200, answer, {}
        text = payload if isinstance(payload, str) else json.dumps(payload)
        return status, dict(extra), text.encode("utf-8")


def client(server: Server, clock: Clock | None = None, **kwargs: Any) -> CircuitApi:
    clock = clock or Clock()
    return CircuitApi(BASE, transport=server, clock=clock, sleep=clock.sleep, **kwargs)


# --------------------------------------------------------------------------- #
# The client.
# --------------------------------------------------------------------------- #


def test_post_sends_json_to_the_joined_url() -> None:
    server = Server({"/treasury": {"balance": 1}})
    api_client = CircuitApi(BASE + "/", transport=server, sleep=lambda _s: None)
    answer = api_client.post("treasury", {"x": 1})
    assert answer == {"balance": 1}
    call = server.calls[0]
    assert call["url"] == BASE + "/treasury"
    assert call["method"] == "POST"
    assert call["body"] == {"x": 1}
    assert call["headers"]["Content-Type"] == "application/json"
    assert call["headers"]["User-Agent"] == api.USER_AGENT


def test_get_sends_no_body() -> None:
    server = Server({"/health": {"status": "ok"}})
    assert client(server).get("/health") == {"status": "ok"}
    assert server.calls[0]["body"] is None
    assert "Content-Type" not in server.calls[0]["headers"]


def test_post_without_a_body_sends_an_empty_object() -> None:
    server = Server({"/oracle": {"price": 1}})
    client(server).post("/oracle")
    assert server.calls[0]["body"] == {}


def test_the_first_request_does_not_wait_and_the_next_is_spaced() -> None:
    clock = Clock()
    server = Server({"/a": {}, "/b": {}})
    api_client = client(server, clock)
    api_client.get("/a")
    assert clock.slept == []
    clock.now += 1.0  # one second passes between the two
    api_client.get("/b")
    assert clock.slept == [pytest.approx(MIN_INTERVAL_S - 1.0)]


def test_no_wait_when_enough_time_has_passed() -> None:
    clock = Clock()
    server = Server({"/a": {}})
    api_client = client(server, clock)
    api_client.get("/a")
    clock.now += MIN_INTERVAL_S + 5
    api_client.get("/a")
    assert clock.slept == []


def test_a_429_is_waited_out_once_then_the_answer_returned() -> None:
    clock = Clock()
    server = Server({"/treasury": InTurn((429, "slow down"), (200, {"ok": True}))})
    assert client(server, clock).post("/treasury") == {"ok": True}
    assert len(server.calls) == 2
    assert WAIT_ON_429_S in clock.slept


def test_retry_after_is_obeyed_and_capped() -> None:
    clock = Clock()
    server = Server({"/a": InTurn((429, "", {"Retry-After": "7"}), (200, {}))})
    client(server, clock).get("/a")
    assert 7.0 in clock.slept

    clock = Clock()
    server = Server({"/a": InTurn((429, "", {"retry-after": "3600"}), (200, {}))})
    client(server, clock).get("/a")
    assert api.MAX_RETRY_AFTER_S in clock.slept

    clock = Clock()
    http_date = {"Retry-After": "Wed, 21 Oct 2026 07:28:00 GMT"}
    server = Server({"/a": InTurn((429, "", http_date), (200, {}))})
    client(server, clock).get("/a")
    assert WAIT_ON_429_S in clock.slept


def test_a_second_429_is_returned_not_retried_forever() -> None:
    server = Server({"/a": InTurn((429, "no"), (429, "still no"))})
    with pytest.raises(CircuitApiError) as caught:
        client(server).get("/a")
    assert caught.value.status == 429
    assert len(server.calls) == 2


def test_an_error_status_raises_with_status_and_body() -> None:
    server = Server({"/vaults": (422, {"detail": [{"type": "missing", "loc": ["body"]}]})})
    with pytest.raises(CircuitApiError) as caught:
        client(server).post("/vaults")
    assert caught.value.status == 422
    assert "missing" in caught.value.body


def test_a_non_json_answer_raises_on_get() -> None:
    server = Server({"/health": (200, "<html>maintenance</html>")})
    with pytest.raises(CircuitApiError, match="not JSON"):
        client(server).get("/health")


def test_no_answer_at_all_is_a_circuit_api_error() -> None:
    server = Server({"/health": TimeoutError("timed out")})
    with pytest.raises(CircuitApiError, match="no answer") as caught:
        client(server).get("/health")
    assert caught.value.status is None


@pytest.mark.parametrize(
    "body",
    [
        {"synthetic_pks": ["a" * 96]},
        {"fingerprint": 123},
        {"operation": "bid", "args": {"target_puzzle_hash": "ab" * 32}},
        {"list": [{"address": "xch1..."}]},
    ],
)
def test_wallet_details_are_refused_before_anything_is_sent(body: dict[str, Any]) -> None:
    server = Server()
    with pytest.raises(ValueError, match="no wallet details"):
        client(server).request("POST", "/vault", body)
    assert server.calls == []


def test_an_empty_key_list_is_allowed() -> None:
    server = Server({"/bills": []})
    assert client(server).post("/bills", {"synthetic_pks": []}) == []


def test_only_get_and_post_are_sent() -> None:
    server = Server()
    with pytest.raises(ValueError):
        client(server).request("DELETE", "/vault")
    assert server.calls == []


@pytest.mark.parametrize("url", ["api.circuitdao.com", "ftp://api.circuitdao.com", "https://", "https://h/?x=1"])
def test_a_bad_base_url_is_refused(url: str) -> None:
    with pytest.raises(ValueError):
        CircuitApi(url)


# --------------------------------------------------------------------------- #
# The reads.
# --------------------------------------------------------------------------- #


def test_reads_have_unique_names_and_carry_no_wallet_details() -> None:
    names = [read.name for read in READS]
    assert len(names) == len(set(names))
    for read in READS:
        assert read.method in ("GET", "POST")
        assert read.path.startswith("/")
        assert api._wallet_fields(read.body) == [], read.name
        if read.method == "GET":
            assert read.body is None, read.name


def test_reads_fit_well_inside_the_rate_limit() -> None:
    # 60 / MIN_INTERVAL_S requests a minute must stay under circuit-cli's 20.
    assert 60.0 / MIN_INTERVAL_S < 20


# --------------------------------------------------------------------------- #
# The probe.
# --------------------------------------------------------------------------- #

STATUTES = {
    "statute_labels": [["STATUTE_BLOCK_ISSUANCE", 43], ["STATUTE_TREASURY_MINIMUM", 20]],
    "full_implemented_statutes": {"STATUTE_BLOCK_ISSUANCE": {"value": 0}},
}


def _probe_server() -> Server:
    return Server(
        {
            "/health": {"status": "ok"},
            "/rpc/version": '"1.2.3"',  # a JSON string
            "/statutes": STATUTES,
            "/treasury": (500, "Internal Server Error"),
            "/oracle": (200, "not json at all"),
            "/vaults": [{"name": "v1"}, {"name": "v2"}],
            "/registry": ConnectionResetError("reset"),
        }
    )


def test_probe_saves_every_answer_and_a_summary(tmp_path: Path) -> None:
    server = _probe_server()
    lines: list[str] = []
    out = tmp_path / "probe"
    results = api.probe(client(server), out, echo=lines.append)

    assert [result.name for result in results] == [read.name for read in READS]
    assert len(server.calls) == len(READS)  # one request each, none skipped
    by_name = {result.name: result for result in results}

    assert by_name["health"].ok and by_name["health"].saved_as == "health.json"
    assert json.loads((out / "health.json").read_text(encoding="utf-8")) == {"status": "ok"}
    assert by_name["vaults"].shape == "list of 2"
    assert by_name["rpc_version"].shape == "str"

    # An error answer is still saved, and says what went wrong.
    assert by_name["treasury"].status == 500
    assert by_name["treasury"].error.startswith("HTTP 500")
    assert (out / "treasury.txt").read_text(encoding="utf-8") == "Internal Server Error"
    # A 200 that is not JSON is kept as text.
    assert by_name["oracle"].ok and by_name["oracle"].shape == "not JSON"
    assert (out / "oracle.txt").exists()
    # No answer at all: recorded, nothing saved, the probe went on.
    assert by_name["registry"].status is None
    assert by_name["registry"].saved_as is None
    assert "no answer" in by_name["registry"].error
    # Unknown endpoints answered 404 and were recorded too.
    assert by_name["openapi"].status == 404

    summary = json.loads((out / "summary.json").read_text(encoding="utf-8"))
    assert summary["base_url"] == BASE
    assert summary["answered"] == sum(1 for result in results if result.ok)
    assert [row["name"] for row in summary["results"]] == [read.name for read in READS]
    assert len(lines) == len(READS) + 1  # a header and one line per read


def test_probe_highlights_the_block_issuance_statute(tmp_path: Path) -> None:
    results = api.probe(client(_probe_server()), tmp_path, echo=lambda _line: None)
    assert results
    summary = json.loads((tmp_path / "summary.json").read_text(encoding="utf-8"))
    places = [item["at"] for item in summary["highlights"]]
    assert "statutes.statute_labels[0][0]" in places
    assert "statutes.full_implemented_statutes.STATUTE_BLOCK_ISSUANCE" in places
    value = next(
        item["value"]
        for item in summary["highlights"]
        if item["at"] == "statutes.full_implemented_statutes.STATUTE_BLOCK_ISSUANCE"
    )
    assert json.loads(value) == {"value": 0}


def test_the_probe_keeps_cache_and_rate_limit_headers_only(tmp_path: Path) -> None:
    headers = {
        "Content-Type": "application/json",
        "Cache-Control": "max-age=30",
        "X-RateLimit-Limit": "20",
        "RateLimit-Remaining": "19",
        "x-rate-limit-reset": "41",
        "Set-Cookie": "session=abc",
        "Server": "uvicorn",
    }
    server = Server({"/health": (200, {"status": "ok"}, headers)})
    api.probe(client(server), tmp_path, [Read("health", "GET", "/health")], echo=lambda _line: None)
    summary = json.loads((tmp_path / "summary.json").read_text(encoding="utf-8"))
    assert summary["results"][0]["headers"] == {
        "content-type": "application/json",
        "cache-control": "max-age=30",
        "x-ratelimit-limit": "20",
        "ratelimit-remaining": "19",
        "x-rate-limit-reset": "41",
    }


def test_highlights_skip_answers_that_failed() -> None:
    assert api.highlights({}) == []
    found = api.highlights({"x": {"note": "BLOCK_ISSUANCE is 1"}})
    assert found == [{"at": "x.note", "value": json.dumps({"note": "BLOCK_ISSUANCE is 1"})}]


def test_probe_lines_are_plain_ascii(tmp_path: Path) -> None:
    server = Server({"/health": {"état": "prêt"}})
    lines: list[str] = []
    api.probe(client(server), tmp_path, [Read("health", "GET", "/health")], echo=lines.append)
    assert all(line.isascii() for line in lines)
    # The saved file keeps the server's own characters.
    assert json.loads((tmp_path / "health.json").read_text(encoding="utf-8")) == {"état": "prêt"}


def test_describe() -> None:
    assert api.describe({}) == "object: empty"
    assert api.describe({"a": 1, "b": 2}) == "object: a, b"
    assert api.describe({str(i): i for i in range(10)}, limit=3) == "object: 0, 1, 2, +7 more"
    assert api.describe([1, 2, 3]) == "list of 3"
    assert api.describe(None) == "NoneType"


# --------------------------------------------------------------------------- #
# The command line.
# --------------------------------------------------------------------------- #


def test_main_probe_writes_the_folder_and_exits_0(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    server = Server({"/health": {"status": "ok"}})
    code = api.main(
        ["probe", "--base-url", BASE, "--out", str(tmp_path / "p")],
        transport=server,
        sleep=lambda _s: None,
    )
    assert code == 0
    assert (tmp_path / "p" / "summary.json").exists()
    assert "1 of" in capsys.readouterr().out


def test_main_exits_1_when_nothing_answered(tmp_path: Path) -> None:
    server = Server({read.path: OSError("down") for read in READS})
    code = api.main(
        ["probe", "--base-url", BASE, "--out", str(tmp_path / "p")],
        transport=server,
        sleep=lambda _s: None,
    )
    assert code == 1


def test_main_refuses_a_bad_base_url(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as caught:
        api.main(["probe", "--base-url", "nope", "--out", str(tmp_path)], transport=Server())
    assert caught.value.code == 2


# --------------------------------------------------------------------------- #
# The file runs on its own.
# --------------------------------------------------------------------------- #


def test_api_module_imports_only_the_standard_library() -> None:
    source = Path(api.__file__).read_text(encoding="utf-8")
    tree = ast.parse(source)
    imported: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            assert node.level == 0, "a relative import would break the file on its own"
            imported.add((node.module or "").split(".")[0])
    imported.discard("__future__")
    import sys

    assert imported <= set(sys.stdlib_module_names), imported - set(sys.stdlib_module_names)
