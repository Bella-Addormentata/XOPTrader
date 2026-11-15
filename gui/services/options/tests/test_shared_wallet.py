"""``options.wallet.share_trading_wallet``: the opt-in and the engine check behind it."""

from __future__ import annotations

import subprocess
import urllib.error

import pytest

from gui.services.options import config, engine_guard
from gui.services.options.tests.test_config import _section

TRADING = {"wallet_host": "localhost", "wallet_port": 9256, "wallet_fingerprint": 481655774}


def _shared(**wallet) -> dict:
    return {
        "port": 9256,
        "cert_path": "c",
        "key_path": "k",
        "fingerprint": 481655774,
        "share_trading_wallet": True,
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


def test_with_no_trading_key_configured_a_key_is_still_required() -> None:
    chia = {"wallet_port": 9256}
    with pytest.raises(config.OptionsConfigError, match="fingerprint is required"):
        _parse(_shared(fingerprint=None), chia=chia)
    assert _parse(_shared(fingerprint=99), chia=chia).wallet.fingerprint == 99


@pytest.mark.parametrize("raw", ["true", "yes", 1])
def test_only_a_real_true_switches_it_on(raw) -> None:
    with pytest.raises(
        config.OptionsConfigError, match="share_trading_wallet must be true or false"
    ):
        _parse(_shared(share_trading_wallet=raw))


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
