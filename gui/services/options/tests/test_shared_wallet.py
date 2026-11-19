"""``options.wallet.share_trading_wallet``: the opt-in and the engine check behind it."""

from __future__ import annotations

import subprocess
import types
import urllib.error

import pytest

from gui.services.options import config, engine_guard
from gui.services.options.tests.test_config import _section

TRADING = {"wallet_host": "localhost", "wallet_port": 9256, "wallet_fingerprint": 481655774}
ADDRESS = "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2"


def _shared(**wallet) -> dict:
    return {
        "port": 9256,
        "cert_path": "c",
        "key_path": "k",
        "fingerprint": 481655774,
        "share_trading_wallet": True,
        "address": ADDRESS,
        # The unfenced shared mode this module is about; the fence is the
        # default now (test_fence.py).
        "engine_fence": False,
        **wallet,
    }


def _parse(wallet: dict, chia: dict | None = None, **root) -> config.OptionsConfig:
    parsed = config.parse({"options": _section(wallet=wallet), "chia": chia or TRADING, **root})
    assert parsed is not None
    return parsed


# -- config ------------------------------------------------------------------


def test_it_is_off_by_default_and_the_trading_wallet_is_still_refused() -> None:
    plain = {"port": 9256, "cert_path": "c", "key_path": "k"}
    with pytest.raises(config.OptionsConfigError, match="trading wallet's RPC port"):
        _parse(plain)
    with pytest.raises(config.OptionsConfigError, match="the trading wallet's key"):
        _parse({**plain, "port": 9356, "fingerprint": 481655774})
    with pytest.raises(config.OptionsConfigError, match="trading wallet's RPC port"):
        _parse({**plain, "share_trading_wallet": False})
    dedicated = _parse({**plain, "port": 9356})
    assert not dedicated.wallet.share_trading_wallet and dedicated.wallet.engine_metrics_url == ""


def test_switched_on_it_allows_the_trading_wallet_s_daemon_and_key() -> None:
    parsed = _parse(_shared())
    assert parsed.wallet.share_trading_wallet
    assert (parsed.wallet.port, parsed.wallet.fingerprint) == (9256, 481655774)
    # And the default wallet port, with chia.wallet_port unset.
    assert _parse(_shared(), chia={"wallet_fingerprint": 481655774}).wallet.port == 9256


@pytest.mark.parametrize("address", [None, "", "   "])
def test_switched_on_it_needs_an_address_of_its_own(address) -> None:
    # Blank takes the wallet's current address: where the engine's self-sends land.
    with pytest.raises(config.OptionsConfigError, match="needs options.wallet.address"):
        _parse(_shared(address=address))
    assert _parse(_shared()).wallet.address == ADDRESS


def test_it_does_not_make_trading_mode_acceptable() -> None:
    with pytest.raises(config.OptionsConfigError, match="mode: trading is not supported"):
        _parse(_shared(mode="trading"))


@pytest.mark.parametrize(
    "wallet",
    [
        _shared(fingerprint=None),  # whichever key is open
        _shared(fingerprint=""),
        _shared(fingerprint=12345),  # another key, switched in under the engine
    ],
)
def test_on_the_engine_s_daemon_it_must_name_the_engine_s_key(wallet) -> None:
    with pytest.raises(config.OptionsConfigError, match="must be the engine's key"):
        _parse(wallet)


def test_with_no_trading_key_configured_sharing_is_refused() -> None:
    # Without chia.wallet_fingerprint nothing says which key the engine uses,
    # so any key named here could be the wrong one and be logged in under it.
    chia = {"wallet_port": 9256}
    for fingerprint in (None, 99):
        with pytest.raises(config.OptionsConfigError, match="which must be set too"):
            _parse(_shared(fingerprint=fingerprint), chia=chia)


@pytest.mark.parametrize("port", [9256, 9357])
def test_the_key_must_match_whatever_the_host_and_port_say(port) -> None:
    # One daemon can be named by more than one host or port forward: a key
    # check that ran only when the host and port matched could be stepped round,
    # and a dry-run step, which logs in, would switch the engine's key.
    with pytest.raises(config.OptionsConfigError, match="must be the engine's key"):
        _parse(_shared(fingerprint=12345, host="127.0.0.1", port=port))
    assert _parse(_shared(host="127.0.0.1", port=port)).wallet.port == port


@pytest.mark.parametrize(
    ("wallet_host", "trading_host"),
    [("10.0.0.5", "localhost"), ("localhost", "10.0.0.5"), ("wallet-box", "wallet-box")],
)
def test_sharing_needs_the_wallet_on_this_machine(wallet_host, trading_host) -> None:
    # The engine check sees this machine's metrics port and process list only;
    # an engine elsewhere would read as not running.
    chia = {**TRADING, "wallet_host": trading_host}
    with pytest.raises(config.OptionsConfigError, match="needs the wallet on this machine"):
        _parse(_shared(host=wallet_host), chia=chia)
    for local in ("localhost", "127.0.0.1", "::1", "[::1]"):
        assert _parse(_shared(host=local), chia={**TRADING, "wallet_host": "localhost"})


@pytest.mark.parametrize("raw", ["true", "yes", 1])
def test_only_a_real_true_switches_it_on(raw) -> None:
    with pytest.raises(
        config.OptionsConfigError, match="share_trading_wallet must be true or false"
    ):
        _parse(_shared(share_trading_wallet=raw))


# -- options.wallet.engine_beside_open_contracts -------------------------------------


def test_the_engine_beside_open_contracts_is_off_by_default() -> None:
    assert not _parse(_shared()).wallet.engine_beside_open_contracts
    assert not _parse(_shared(fund_from_wallet=True)).wallet.engine_beside_open_contracts


def test_the_engine_beside_open_contracts_needs_funding_from_the_whole_wallet() -> None:
    # A running engine can spend the coins at the options address; the book's
    # next cancel or clawback must be able to pay from elsewhere.
    with pytest.raises(config.OptionsConfigError, match="needs options.wallet.fund_from_wallet"):
        _parse(_shared(engine_beside_open_contracts=True))
    with pytest.raises(config.OptionsConfigError, match="needs options.wallet.fund_from_wallet"):
        _parse(_shared(engine_beside_open_contracts=True, fund_from_wallet=False))
    parsed = _parse(_shared(engine_beside_open_contracts=True, fund_from_wallet=True))
    assert parsed.wallet.engine_beside_open_contracts and parsed.wallet.fund_from_wallet


def test_the_engine_beside_open_contracts_means_nothing_on_a_wallet_of_its_own() -> None:
    # A dedicated wallet never holds the engine, so there is nothing to relax
    # and nothing to refuse.
    dedicated = _parse(
        {"port": 9356, "cert_path": "c", "key_path": "k", "engine_beside_open_contracts": True}
    )
    assert not dedicated.wallet.share_trading_wallet


@pytest.mark.parametrize("raw", ["true", "yes", 1])
def test_only_a_real_true_lets_the_engine_beside_open_contracts(raw) -> None:
    with pytest.raises(
        config.OptionsConfigError, match="engine_beside_open_contracts must be true or false"
    ):
        _parse(_shared(engine_beside_open_contracts=raw, fund_from_wallet=True))


def test_the_command_line_says_which_rule_holds_the_engine(monkeypatch, capsys) -> None:
    cli = pytest.importorskip("gui.services.options.__main__")
    up = engine_guard.EngineState(running=True, certain=True, detail="xop_trader.exe process 1")
    down = engine_guard.EngineState(running=False, certain=True, detail="nothing there")
    monkeypatch.setattr(cli.engine_guard, "engine_state", lambda url: up)
    strict = _parse(_shared(fund_from_wallet=True))
    beside = _parse(_shared(engine_beside_open_contracts=True, fund_from_wallet=True))

    class Book:  # one 0.5 XCH call open
        def contracts(self, statuses):
            return [types.SimpleNamespace(underlying_amount=500_000_000_000, underlying_asset=None)]

    cli._shared_wallet_banner(strict, Book())
    out = capsys.readouterr().out
    assert "share_trading_wallet is ON" in out
    assert "must stay STOPPED while any contract is open" in out
    assert "WARNING: 1 contract(s) open with 0.5 XCH locked" in out

    cli._shared_wallet_banner(beside, Book())
    out = capsys.readouterr().out
    assert "share_trading_wallet is ON" in out
    assert "may run while contracts sit open" in out and "STOPPED while this book acts" in out
    assert "note: 1 contract(s) open with 0.5 XCH locked beside the engine" in out
    assert "WARNING: 1 contract(s)" not in out

    # A live step is refused either way; only the advice differs.
    assert "keep it stopped while contracts are open" in cli._engine_refusal(up, "a step", strict)
    refusal = cli._engine_refusal(up, "a step", beside)
    assert refusal.startswith("refused, trading engine up") and "while the book acts" in refusal
    assert cli._engine_refusal(down, "a step", beside) is None


def test_the_engine_is_looked_for_at_its_metrics_port() -> None:
    assert _parse(_shared()).wallet.engine_metrics_url == "http://localhost:9090/metrics"
    moved = _parse(_shared(), monitoring={"prometheus_port": 19090})
    assert moved.wallet.engine_metrics_url == "http://localhost:19090/metrics"
    named = _parse(_shared(engine_metrics_url="http://127.0.0.1:7/m"))
    assert named.wallet.engine_metrics_url == "http://127.0.0.1:7/m"
    with pytest.raises(config.OptionsConfigError, match="prometheus_port must be a number"):
        _parse(_shared(), monitoring={"prometheus_port": "x"})


# -- engine_guard ------------------------------------------------------------


def _ran(stdout: str = "", code: int = 0):
    calls: list[list[str]] = []

    def run(argv: list[str]) -> subprocess.CompletedProcess:
        calls.append(argv)
        return subprocess.CompletedProcess(argv, code, stdout=stdout, stderr="")

    return run, calls


def _http(found: bool | None):
    return lambda url: (found, f"http says {found}")


_TASKLIST_HIT = '"xop_trader.exe","19956","Console","1","412,000 K"\r\n'
_TASKLIST_NONE = "INFO: No tasks are running which match the specified criteria.\r\n"


def test_windows_finds_the_engine_but_not_the_gui() -> None:
    run, calls = _ran(_TASKLIST_HIT)
    state = engine_guard.engine_state("u", http=_http(False), run=run, platform="win32")
    assert state.running and state.certain and not state.clear
    assert "19956" in state.describe() and state.describe().startswith("RUNNING")
    assert calls == [["tasklist", "/FI", "IMAGENAME eq xop_trader.exe", "/FO", "CSV", "/NH"]]

    gui = '"xop_trader_gui.exe","16720","Console","1","90,000 K"\r\n'
    for listing in (_TASKLIST_NONE, gui):
        run, _ = _ran(listing)
        state = engine_guard.engine_state("u", http=_http(False), run=run, platform="win32")
        assert state.clear and state.describe().startswith("not running")


def test_elsewhere_pgrep_is_asked_for_the_exact_name() -> None:
    run, calls = _ran("4242\n", 0)
    assert engine_guard.engine_state("u", http=_http(False), run=run, platform="linux").running
    assert calls == [["pgrep", "-x", "xop_trader"]]
    run, _ = _ran("", 1)
    assert engine_guard.engine_state("u", http=_http(False), run=run, platform="linux").clear


def test_an_answer_at_the_metrics_port_is_an_engine_whatever_the_process_list_says() -> None:
    run, _ = _ran(_TASKLIST_NONE)
    state = engine_guard.engine_state("u", http=_http(True), run=run, platform="win32")
    assert state.running and not state.clear


@pytest.mark.parametrize(
    ("http", "run"),
    [
        (_http(None), _ran(_TASKLIST_NONE)[0]),  # the port neither answered nor refused
        (_http(False), _ran("", 5)[0]),  # tasklist failed
    ],
)
def test_it_fails_closed_when_it_cannot_tell(http, run) -> None:
    state = engine_guard.engine_state("u", http=http, run=run, platform="win32")
    assert not state.running and not state.certain and not state.clear
    assert state.describe().startswith("CANNOT TELL")


def test_a_process_list_that_cannot_be_run_cannot_tell() -> None:
    def missing(argv):
        raise FileNotFoundError("pgrep")

    state = engine_guard.engine_state("u", http=_http(False), run=missing, platform="linux")
    assert not state.clear and "could not be read" in state.detail


def test_the_http_probe_reads_refused_as_absent_and_any_page_as_present(monkeypatch) -> None:
    def refused(url, timeout):
        raise urllib.error.URLError(ConnectionRefusedError(10061, "refused"))

    def error_page(url, timeout):
        raise urllib.error.HTTPError(url, 500, "boom", {}, None)

    def timed_out(url, timeout):
        raise urllib.error.URLError(TimeoutError("timed out"))

    for opener, found in ((refused, False), (error_page, True), (timed_out, None)):
        monkeypatch.setattr(engine_guard.urllib.request, "urlopen", opener)
        assert engine_guard._probe_http("http://localhost:1/metrics")[0] is found


def test_the_http_probe_against_a_real_closed_port() -> None:
    import socket

    with socket.socket() as sock:  # bind, learn the port, close: nothing listens there now
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    found, detail = engine_guard._probe_http(f"http://127.0.0.1:{port}/metrics")
    assert found is False, detail


# -- EngineCheckedWallet: asked at every signature and push ----------------------

_DOWN = engine_guard.EngineState(running=False, certain=True, detail="nothing there")
_UP = engine_guard.EngineState(running=True, certain=True, detail="xop_trader.exe process 1")
_UNSURE = engine_guard.EngineState(running=False, certain=False, detail="tasklist failed")


class _Wallet:
    def __init__(self) -> None:
        self.calls: list[str] = []

    def sign(self, coin_spends):
        self.calls.append("sign")
        return b"sig"

    def push(self, coin_spends, signature):
        self.calls.append("push")
        return b"id"

    def not_ready(self, *, select_key: bool = True):
        self.calls.append("not_ready")
        return None


def test_the_checked_wallet_asks_before_every_sign_and_push(monkeypatch) -> None:
    states = iter([_DOWN, _DOWN, _UP, _DOWN])
    asked: list[str] = []

    def state(url):
        asked.append(url)
        return next(states)

    monkeypatch.setattr(engine_guard, "engine_state", state)
    inner = _Wallet()
    wallet = engine_guard.EngineCheckedWallet(inner, "http://localhost:19090/metrics")

    assert wallet.sign([]) == b"sig" and wallet.push([], b"sig") == b"id"
    # The engine came up between two operations of one step: the next is refused.
    with pytest.raises(engine_guard.EngineRunningError, match="refused, trading engine up"):
        wallet.sign([])
    assert wallet.push([], b"sig") == b"id"  # and allowed again once it is down
    assert inner.calls == ["sign", "push", "push"]
    assert asked == ["http://localhost:19090/metrics"] * 4


@pytest.mark.parametrize("state", [_UP, _UNSURE])
def test_the_checked_wallet_refuses_while_the_engine_may_be_up(monkeypatch, state) -> None:
    monkeypatch.setattr(engine_guard, "engine_state", lambda url: state)
    inner = _Wallet()
    wallet = engine_guard.EngineCheckedWallet(inner, "u")
    for call in (lambda: wallet.sign([]), lambda: wallet.push([], b"s")):
        with pytest.raises(engine_guard.EngineRunningError):
            call()
    assert inner.calls == []
    # Reads pass straight through: they spend nothing.
    assert wallet.not_ready() is None and inner.calls == ["not_ready"]


def test_a_refusal_is_a_signing_error_so_the_book_reports_it_per_action() -> None:
    from gui.services.options import signing

    assert issubclass(engine_guard.EngineRunningError, signing.SigningError)
    assert issubclass(engine_guard.EngineRunningError, RuntimeError)  # the CLI's catch-all
