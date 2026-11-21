"""Read-only client for Circuit's public API, and a probe that records its answers.

Circuit's own tools talk to one server, https://api.circuitdao.com:
circuit-cli (Circuit's command line) and CircuitSage (a third-party PowerShell
module). It answers JSON over HTTPS -- a few GETs, mostly POSTs with a JSON
body. Besides reporting protocol state it can build UNSIGNED transactions for
a wallet's synthetic public keys. This module wraps only the reads. It sends no
key and no address, so nothing it does can move money or name a wallet.

THE PROBE. Issue #225 builds a read-only Circuit page first, and the page needs
the real shape of these answers. The cloud sessions this was written in cannot
reach the server, so :func:`probe` makes one call to each read endpoint in
:data:`READS` and saves every answer to a folder, to be run once on a machine
that can::

    python api.py probe                           (this file on its own)
    python -m gui.services.circuit probe          (in the app's environment)

The file on its own needs only Python. The ``-m`` form loads the
``gui.services`` package first, and that needs the app's own packages (Qt,
PyYAML), so it works only where the app runs from source.

It takes about a minute. It writes ``circuit_probe_<UTC time>/`` in the current
folder (``--out`` chooses another): one ``<name>.json`` per endpoint
(``<name>.txt`` when an answer is not JSON) and ``summary.json`` (each answer's
status, time, size and shape, and the server's cache and rate-limit headers),
and prints a line per endpoint. A read that fails is recorded and the probe
goes on; the exit status is 1 only when no read succeeded (no 2xx answer). The
answers are the protocol's public state; nothing in them is yours.

RATE LIMIT. circuit-cli waits a minute after being refused and records the
limit as 20 requests a minute, so requests here are :data:`MIN_INTERVAL_S`
apart (under 19 a minute) and an HTTP 429 is waited out once, for the server's
``Retry-After`` or a minute.

STANDARD LIBRARY ONLY, on purpose: the file runs on its own, downloaded from
GitHub, with any Python 3.10 or later and without the app's virtualenv. It
imports nothing from the rest of XOPTrader (a test checks).
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as _dt
import json
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import Any

DEFAULT_BASE_URL = "https://api.circuitdao.com"
#: Seconds between the starts of two requests: 60 / 3.2 = 18.75 a minute,
#: under the 20 a minute circuit-cli records.
MIN_INTERVAL_S = 3.2
#: Wait after an HTTP 429 that gives no usable Retry-After (circuit-cli waits a minute).
WAIT_ON_429_S = 60.0
#: The longest Retry-After obeyed; a longer one is cut to this.
MAX_RETRY_AFTER_S = 120.0
#: A generic client name: the server learns what made the request, nothing about who.
USER_AGENT = "XOPTrader-circuit-probe/1"
#: Answer headers the probe keeps, to learn how often a page may ask: these,
#: and any whose name holds "ratelimit" (dashes ignored). Nothing in them is
#: about the person asking.
KEPT_HEADERS = frozenset(
    {"content-type", "cache-control", "age", "date", "last-modified", "expires", "retry-after"}
)

#: Body fields that would name a wallet. The read-only client refuses to send
#: any of them with a value, at any depth. ``/bills`` takes an EMPTY
#: ``synthetic_pks`` for the protocol-wide list, which is allowed.
WALLET_FIELDS = frozenset(
    {
        "synthetic_pks",
        "synthetic_pk",
        "public_key",
        "private_key",
        "fingerprint",
        "address",
        "puzzle_hash",
        "target_puzzle_hash",
    }
)

#: ``(method, url, body, headers, timeout_s) -> (status, headers, body)``.
#: An HTTP error status is RETURNED; only getting no answer at all (DNS,
#: refused connection, timeout, TLS) raises.
Transport = Callable[
    [str, str, bytes | None, dict[str, str], float], tuple[int, dict[str, str], bytes]
]


class CircuitApiError(RuntimeError):
    """No usable answer: none came back, the server refused, or it was not JSON."""

    def __init__(self, message: str, *, status: int | None = None, body: str = "") -> None:
        super().__init__(message)
        self.status = status
        self.body = body


@dataclasses.dataclass(frozen=True)
class Answer:
    """One HTTP answer, whatever its status."""

    status: int
    text: str
    elapsed_s: float
    headers: dict[str, str] = dataclasses.field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return 200 <= self.status < 300

    def json(self) -> Any:
        """The body parsed as JSON; ``ValueError`` when it is not JSON."""
        return json.loads(self.text)


@dataclasses.dataclass(frozen=True)
class Read:
    """One read-only request: the probe saves its answer as ``<name>.json``."""

    name: str
    method: str
    path: str
    body: dict[str, Any] | None = None


#: Every read the probe makes, with the bodies circuit-cli sends for them.
#: None carries a key or an address.
READS: tuple[Read, ...] = (
    # FastAPI publishes a schema of every endpoint here unless it is switched
    # off. The server's validation errors have FastAPI's shape, so it may be on.
    Read("openapi", "GET", "/openapi.json"),
    Read("health", "GET", "/health"),
    Read("rpc_version", "GET", "/rpc/version"),
    Read("protocol_invariants", "GET", "/protocol/invariants"),
    # circuit-cli's `upkeep state`: everything at once.
    Read(
        "protocol_state",
        "POST",
        "/protocol/state",
        {
            "vaults": True,
            "surplus_auctions": True,
            "recharge_auctions": True,
            "treasury": True,
            "bills": True,
        },
    ),
    Read("statutes", "POST", "/statutes", {"full": True}),
    Read("statutes_info", "POST", "/statutes/info", {}),
    Read("oracle", "POST", "/oracle", {}),
    Read("treasury", "POST", "/treasury", {}),
    Read("surplus_auctions", "POST", "/surplus_auctions", {}),
    Read("recharge_auctions", "POST", "/recharge_auctions", {}),
    Read("vaults", "POST", "/vaults", {}),
    # Not /savings/list: it works on one wallet's deposit and withdrawal
    # (asked with {}, the server answered 422 for missing "amount" and
    # "treasury_withdraw_amount" on 2026-11-21), so it is no protocol read.
    Read("bills", "POST", "/bills", {"synthetic_pks": [], "include_spent_coins": False}),
    Read("announcers", "POST", "/announcers", {"approved": True, "include_spent_coins": False}),
    Read("registry", "POST", "/registry", {}),
)

#: Text worth pointing out in the answers. STATUTE_BLOCK_ISSUANCE is the
#: emergency statute that switches the protocol's core functions off (Circuit's
#: governance guide): the clearest sign the server can give of whether the
#: protocol behind today's BYC is being wound down.
HIGHLIGHT_TOKENS: tuple[str, ...] = ("BLOCK_ISSUANCE",)


def urllib_transport(
    method: str, url: str, body: bytes | None, headers: dict[str, str], timeout_s: float
) -> tuple[int, dict[str, str], bytes]:
    """The default :data:`Transport`, on ``urllib`` (proxy settings from the environment)."""
    # S310: CircuitApi accepts only an http(s) base URL, so no file: or custom scheme.
    request = urllib.request.Request(url, data=body, method=method, headers=headers)  # noqa: S310
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:  # noqa: S310
            return response.status, dict(response.headers.items()), response.read()
    except urllib.error.HTTPError as exc:
        try:
            payload = exc.read() or b""
        except Exception:  # noqa: BLE001 -- the status is the answer; a body is a bonus
            payload = b""
        answer_headers = dict(exc.headers.items()) if exc.headers is not None else {}
        return exc.code, answer_headers, payload


def _wallet_fields(node: Any) -> list[str]:
    """Names of wallet fields in ``node`` that carry a value, at any depth.

    Tuples are walked like lists, since ``json.dumps`` sends both as arrays.
    """
    found: list[str] = []
    if isinstance(node, dict):
        for key, value in node.items():
            if key in WALLET_FIELDS and value not in (None, "", [], (), {}):
                found.append(str(key))
            found.extend(_wallet_fields(value))
    elif isinstance(node, (list, tuple)):
        for item in node:
            found.extend(_wallet_fields(item))
    return found


def _excerpt(text: str, limit: int = 200) -> str:
    flat = " ".join(text.split())
    return flat if len(flat) <= limit else flat[: limit - 3] + "..."


def kept_headers(headers: dict[str, str]) -> dict[str, str]:
    """The :data:`KEPT_HEADERS` and rate-limit headers of an answer, names lower-cased."""
    kept: dict[str, str] = {}
    for key, value in headers.items():
        name = key.lower()
        if name in KEPT_HEADERS or "ratelimit" in name.replace("-", ""):
            kept[name] = value
    return kept


def _retry_after(headers: dict[str, str]) -> float:
    raw = next((value for key, value in headers.items() if key.lower() == "retry-after"), None)
    try:
        seconds = float(raw) if raw is not None else WAIT_ON_429_S
    except ValueError:  # an HTTP date, or junk: wait the default
        seconds = WAIT_ON_429_S
    if not math.isfinite(seconds):  # "NaN" and "inf" parse as floats; sleep(nan) raises
        seconds = WAIT_ON_429_S
    return min(max(seconds, 1.0), MAX_RETRY_AFTER_S)


class CircuitApi:
    """A read-only, rate-limited client for Circuit's API."""

    def __init__(
        self,
        base_url: str = DEFAULT_BASE_URL,
        *,
        timeout_s: float = 30.0,
        min_interval_s: float = MIN_INTERVAL_S,
        transport: Transport | None = None,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        cleaned = base_url.strip().rstrip("/")
        parts = urllib.parse.urlsplit(cleaned)
        if parts.scheme not in ("https", "http") or not parts.netloc:
            raise ValueError(f"the API's base URL must be http(s)://host, not {base_url!r}")
        if parts.query or parts.fragment:
            raise ValueError(f"the API's base URL takes no query or fragment: {base_url!r}")
        self.base_url = cleaned
        self._timeout_s = timeout_s
        self._min_interval_s = min_interval_s
        self._transport = transport or urllib_transport
        self._clock = clock
        self._sleep = sleep
        self._last_start: float | None = None

    def request(self, method: str, path: str, body: dict[str, Any] | None = None) -> Answer:
        """Send one request and return the answer, whatever its status.

        Raises :class:`CircuitApiError` only when no answer came back, and
        ``ValueError`` -- before anything is sent -- for a method other than
        GET or POST or a body that carries wallet details.
        """
        method = method.upper()
        if method not in ("GET", "POST"):
            raise ValueError(f"the read-only Circuit client sends GET or POST, not {method}")
        named = _wallet_fields(body)
        if named:
            raise ValueError(
                "the read-only Circuit client sends no wallet details; refused: "
                + ", ".join(sorted(set(named)))
            )
        url = f"{self.base_url}/{path.lstrip('/')}"
        payload = None if body is None else json.dumps(body).encode("utf-8")
        headers = {"Accept": "application/json", "User-Agent": USER_AGENT}
        if payload is not None:
            headers["Content-Type"] = "application/json"
        for attempt in (1, 2):
            self._wait_turn()
            started = self._clock()
            self._last_start = started
            try:
                status, answer_headers, raw = self._transport(
                    method, url, payload, headers, self._timeout_s
                )
            except Exception as exc:  # noqa: BLE001 -- every transport failure is "no answer"
                raise CircuitApiError(
                    f"{method} {path}: no answer ({type(exc).__name__}: {exc})"
                ) from exc
            answer = Answer(
                status,
                raw.decode("utf-8", errors="replace"),
                self._clock() - started,
                dict(answer_headers),
            )
            if status == 429 and attempt == 1:
                self._sleep(_retry_after(answer.headers))
                continue
            return answer
        raise AssertionError("unreachable")  # pragma: no cover

    def get(self, path: str) -> Any:
        """GET ``path`` and return its JSON; :class:`CircuitApiError` otherwise."""
        return self._json(self.request("GET", path), "GET", path)

    def post(self, path: str, body: dict[str, Any] | None = None) -> Any:
        """POST ``body`` (default ``{}``) and return the JSON answer."""
        return self._json(self.request("POST", path, {} if body is None else body), "POST", path)

    def _wait_turn(self) -> None:
        if self._last_start is None:
            return
        gap = self._min_interval_s - (self._clock() - self._last_start)
        if gap > 0:
            self._sleep(gap)

    @staticmethod
    def _json(answer: Answer, method: str, path: str) -> Any:
        if not answer.ok:
            raise CircuitApiError(
                f"{method} {path}: HTTP {answer.status}: {_excerpt(answer.text)}",
                status=answer.status,
                body=answer.text,
            )
        try:
            return answer.json()
        except ValueError as exc:
            raise CircuitApiError(
                f"{method} {path}: the answer is not JSON: {_excerpt(answer.text)}",
                status=answer.status,
                body=answer.text,
            ) from exc


# --------------------------------------------------------------------------- #
# The probe.
# --------------------------------------------------------------------------- #


@dataclasses.dataclass(frozen=True)
class ProbeResult:
    """What one read returned. ``status`` is None when no answer came back."""

    name: str
    method: str
    path: str
    status: int | None
    elapsed_s: float | None
    size_bytes: int
    saved_as: str | None
    shape: str
    error: str = ""
    headers: dict[str, str] = dataclasses.field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.status is not None and 200 <= self.status < 300


def describe(data: Any, limit: int = 8) -> str:
    """A one-line picture of a JSON value: its keys, or how long it is."""
    if isinstance(data, dict):
        if not data:
            return "object: empty"
        keys = [str(key) for key in data]
        more = f", +{len(keys) - limit} more" if len(keys) > limit else ""
        return f"object: {', '.join(keys[:limit])}{more}"
    if isinstance(data, list):
        return f"list of {len(data)}"
    return type(data).__name__


def _clip(value: Any, limit: int = 300) -> str:
    text = json.dumps(value, ensure_ascii=False, default=str)
    return text if len(text) <= limit else text[: limit - 3] + "..."


def highlights(
    answers: dict[str, Any], tokens: Sequence[str] = HIGHLIGHT_TOKENS, limit: int = 50
) -> list[dict[str, str]]:
    """Where ``tokens`` appear in the answers -- as a key or inside a string --
    with the value around them, for a person to read first."""
    wanted = [token.upper() for token in tokens]
    found: list[dict[str, str]] = []

    def hit(text: str) -> bool:
        upper = text.upper()
        return any(token in upper for token in wanted)

    def walk(node: Any, path: str) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                here = f"{path}.{key}"
                if hit(str(key)):
                    found.append({"at": here, "value": _clip(value)})
                elif isinstance(value, str) and hit(value):
                    found.append({"at": here, "value": _clip(node)})
                walk(value, here)
        elif isinstance(node, list):
            for index, item in enumerate(node):
                here = f"{path}[{index}]"
                if isinstance(item, str) and hit(item):
                    found.append({"at": here, "value": _clip(node)})
                walk(item, here)

    for name, data in answers.items():
        walk(data, name)
    return found[:limit]


def _utc_now() -> str:
    # timezone.utc, not datetime.UTC (3.11+): this file runs on 3.10.
    return _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")  # noqa: UP017


def _line(result: ProbeResult) -> str:
    status = "---" if result.status is None else str(result.status)
    took = "" if result.elapsed_s is None else f"{result.elapsed_s:5.1f} s"
    size = "" if result.status is None else f"{result.size_bytes / 1024:8.1f} KB"
    detail = result.error or result.shape
    line = f"  {status:>3}  {took:>7}  {size:>11}  {result.name:<20} {detail}"
    # Windows consoles may not take every character a server sends.
    return line[:200].encode("ascii", "replace").decode("ascii")


def probe(
    api: CircuitApi,
    out_dir: Path,
    reads: Sequence[Read] = READS,
    *,
    echo: Callable[[str], None] = print,
) -> list[ProbeResult]:
    """Make each read once and save every answer in ``out_dir``.

    A read that fails is recorded and the probe goes on; ``summary.json``
    lists them all, with :func:`highlights` from the answers that succeeded.

    ``out_dir`` must be new or empty (``FileExistsError`` otherwise, before
    anything is asked): an answer left from an earlier run would sit beside
    a ``summary.json`` that does not describe it.
    """
    if out_dir.exists() and (not out_dir.is_dir() or any(out_dir.iterdir())):
        raise FileExistsError(
            f"{out_dir} is not a new or empty folder; give the probe one, so no"
            " answer from an earlier run sits beside this run's summary"
        )
    out_dir.mkdir(parents=True, exist_ok=True)
    started = _utc_now()
    seconds = round(len(reads) * MIN_INTERVAL_S)
    echo(f"Asking {api.base_url} {len(reads)} read-only questions (about {seconds} s).")
    results: list[ProbeResult] = []
    answers: dict[str, Any] = {}
    for read in reads:
        try:
            answer = api.request(read.method, read.path, read.body)
        except CircuitApiError as exc:
            result = ProbeResult(
                read.name, read.method, read.path, None, None, 0, None, "", str(exc)
            )
        else:
            try:
                data = answer.json()
            except ValueError:
                saved_as = f"{read.name}.txt"
                (out_dir / saved_as).write_text(answer.text, encoding="utf-8")
                shape = "not JSON"
            else:
                saved_as = f"{read.name}.json"
                (out_dir / saved_as).write_text(
                    json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
                )
                shape = describe(data)
                if answer.ok:
                    answers[read.name] = data
            result = ProbeResult(
                read.name,
                read.method,
                read.path,
                answer.status,
                round(answer.elapsed_s, 3),
                len(answer.text.encode("utf-8")),
                saved_as,
                shape,
                "" if answer.ok else f"HTTP {answer.status}: {_excerpt(answer.text)}",
                kept_headers(answer.headers),
            )
        results.append(result)
        echo(_line(result))
    summary = {
        "base_url": api.base_url,
        "client": USER_AGENT,
        "started_utc": started,
        "finished_utc": _utc_now(),
        "succeeded": sum(1 for result in results if result.ok),
        "reached": sum(1 for result in results if result.status is not None),
        "results": [dataclasses.asdict(result) for result in results],
        "highlights": highlights(answers),
    }
    (out_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return results


def main(
    argv: Sequence[str] | None = None,
    *,
    transport: Transport | None = None,
    sleep: Callable[[float], None] = time.sleep,
) -> int:
    """``probe``: save one answer from each read endpoint.

    Returns 1 when no read succeeded (no 2xx answer, though error answers are
    still saved), 2 for bad arguments (from argparse), else 0.
    """
    invoked = os.path.basename(sys.argv[0] or "")
    if invoked in ("__main__.py", ""):
        prog = "python -m gui.services.circuit"
    else:
        prog = f"python {invoked}"
    parser = argparse.ArgumentParser(
        prog=prog, description="Read-only tools for Circuit's public API (issue #225)."
    )
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("probe", help="save one answer from each read endpoint to a folder")
    run.add_argument("--base-url", default=DEFAULT_BASE_URL, help=f"default {DEFAULT_BASE_URL}")
    run.add_argument(
        "--out",
        type=Path,
        default=None,
        help="a new or empty folder for the answers"
        " (default: circuit_probe_<UTC time> in the current folder)",
    )
    run.add_argument(
        "--timeout", type=float, default=30.0, help="seconds to wait for each answer (default 30)"
    )
    args = parser.parse_args(argv)
    try:
        api = CircuitApi(args.base_url, timeout_s=args.timeout, transport=transport, sleep=sleep)
    except ValueError as exc:
        parser.error(str(exc))
    now = _dt.datetime.now(_dt.timezone.utc)  # noqa: UP017 -- see _utc_now
    out = args.out or Path(f"circuit_probe_{now:%Y%m%dT%H%M%SZ}")
    try:
        results = probe(api, out)
    except FileExistsError as exc:
        parser.error(str(exc))
    succeeded = sum(1 for result in results if result.ok)
    reached = sum(1 for result in results if result.status is not None)
    print(
        f"\n{succeeded} of {len(results)} answered OK, {reached} reached the server."
        f" Saved in {out.resolve()}"
    )
    print("Nothing was signed and no wallet detail was sent. Zip that folder to share it.")
    return 0 if succeeded else 1


if __name__ == "__main__":
    raise SystemExit(main())
