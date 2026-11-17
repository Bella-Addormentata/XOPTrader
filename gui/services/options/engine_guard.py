"""Is the trading engine running? Asked before signing from a wallet shared with it.

``options.wallet.share_trading_wallet`` lets the book sign from the engine's
own wallet on one condition: the engine is stopped while the book has contracts
open (see :mod:`config`). This module checks that condition, two ways:

* the engine's Prometheus endpoint (``monitoring.prometheus_port``). Any HTTP
  answer at all means an engine is up. A refused connection means none is
  listening there; anything else (a timeout, a reset) cannot tell.
* the process list, for an ``xop_trader`` executable (``tasklist`` on Windows,
  ``pgrep`` elsewhere). This catches an engine started with another config,
  whose endpoint is on another port.

Either one finding the engine means it is running. Either one failing to give
an answer means "cannot tell", which the caller treats as running: the guard
fails closed.

Both checks see this machine only, so :mod:`config` allows sharing only when
the wallet, and the engine's view of it, are on this machine.

:class:`EngineCheckedWallet` asks again before every signature and every push,
not just once a step: an engine started part way through a step, between a
mint and an offer, is caught at the next one.
"""

from __future__ import annotations

import dataclasses
import subprocess
import sys
import urllib.error
import urllib.request
from collections.abc import Callable, Sequence
from typing import Any

from gui.services.options import signing

#: The engine's executable, without ``.exe``. The GUI (``xop_trader_gui``) is
#: not the engine and is not matched.
ENGINE_NAME = "xop_trader"

_HTTP_TIMEOUT = 3.0
_PROCESS_TIMEOUT = 15.0


@dataclasses.dataclass(frozen=True)
class EngineState:
    running: bool
    #: False when a check could not give an answer. Treated as running.
    certain: bool
    #: What each check found, for the operator.
    detail: str

    @property
    def clear(self) -> bool:
        """True only when both checks answered and neither found an engine."""
        return self.certain and not self.running

    def describe(self) -> str:
        if self.running:
            head = "RUNNING"
        elif not self.certain:
            head = "CANNOT TELL (treated as running)"
        else:
            head = "not running"
        return f"{head} ({self.detail})"


def _probe_http(url: str) -> tuple[bool | None, str]:
    """True if something answers at ``url``, False if refused, None if it cannot tell."""
    try:
        with urllib.request.urlopen(url, timeout=_HTTP_TIMEOUT):  # noqa: S310 - fixed local URL
            return True, f"{url} answers"
    except urllib.error.HTTPError:
        return True, f"{url} answers"  # an error page is still an engine
    except urllib.error.URLError as exc:
        reason = exc.reason
        if isinstance(reason, ConnectionRefusedError):
            return False, f"nothing at {url}"
        return None, f"{url}: {reason}"
    except ConnectionRefusedError:
        return False, f"nothing at {url}"
    except OSError as exc:
        return None, f"{url}: {exc}"


def _run(argv: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(  # noqa: S603 - fixed arguments, no shell
        argv, capture_output=True, text=True, timeout=_PROCESS_TIMEOUT, check=False
    )


def _probe_process(
    run: Callable[[list[str]], subprocess.CompletedProcess], platform: str
) -> tuple[bool | None, str]:
    """Whether an ``xop_trader`` process exists, from the system's process list."""
    try:
        if platform.startswith("win"):
            image = f"{ENGINE_NAME}.exe"
            done = run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/FO", "CSV", "/NH"])
            if done.returncode != 0:
                return None, f"tasklist failed ({done.returncode})"
            # One CSV row per match, its first field the quoted image name; with
            # none, tasklist prints an INFO line instead.
            rows = [
                line
                for line in done.stdout.splitlines()
                if line.strip().lower().startswith(f'"{image}"')
            ]
            if rows:
                pids = [row.split('","')[1] for row in rows if '","' in row]
                return True, f"{image} process {', '.join(pids) or 'found'}"
            return False, f"no {image} process"
        done = run(["pgrep", "-x", ENGINE_NAME])
        if done.returncode == 0:
            return True, f"{ENGINE_NAME} process {' '.join(done.stdout.split())}"
        if done.returncode == 1:
            return False, f"no {ENGINE_NAME} process"
        return None, f"pgrep failed ({done.returncode})"
    except (OSError, subprocess.SubprocessError) as exc:
        return None, f"the process list could not be read ({exc})"


def engine_state(
    metrics_url: str,
    *,
    http: Callable[[str], tuple[bool | None, str]] = _probe_http,
    run: Callable[[list[str]], subprocess.CompletedProcess] = _run,
    platform: str = sys.platform,
) -> EngineState:
    """Ask both ways and combine the answers."""
    answers = [http(metrics_url), _probe_process(run, platform)]
    running = any(found is True for found, _ in answers)
    certain = running or all(found is not None for found, _ in answers)
    return EngineState(
        running=running, certain=certain, detail="; ".join(text for _, text in answers)
    )


# -- [OPTIONS-FENCE] the engine's fence, read off its metrics endpoint ------------

#: How old the engine's last refresh of its fence list may be for the book to
#: act beside it. The engine refreshes at most every 30 seconds while it runs.
FENCE_MAX_AGE_SECONDS = 120.0


@dataclasses.dataclass(frozen=True)
class FenceState:
    """What a running engine says about the options fence (``xop_options_fence_*``)."""

    reported: bool  # the endpoint answered with the fence gauges at all
    enabled: bool
    fresh: bool
    coins: int
    age_seconds: float
    detail: str

    @property
    def holds(self) -> bool:
        """True when the book may sign beside the engine: on, fresh, recently refreshed."""
        return (
            self.reported
            and self.enabled
            and self.fresh
            and 0.0 <= self.age_seconds <= FENCE_MAX_AGE_SECONDS
        )


def _gauges(text: str) -> dict[str, float]:
    out: dict[str, float] = {}
    for line in text.splitlines():
        if not line.startswith("xop_options_fence_"):
            continue
        name, _, value = line.partition(" ")
        try:
            out[name.split("{", 1)[0]] = float(value.strip().split(" ")[0])
        except ValueError:
            continue
    return out


def parse_fence(metrics_text: str) -> FenceState:
    """The fence gauges in a Prometheus exposition; an engine without them reports nothing."""
    gauges = _gauges(metrics_text)
    if "xop_options_fence_enabled" not in gauges:
        return FenceState(False, False, False, 0, -1.0, "the engine reports no options fence")
    enabled = gauges.get("xop_options_fence_enabled", 0.0) >= 1.0
    fresh = gauges.get("xop_options_fence_fresh", 0.0) >= 1.0
    coins = int(gauges.get("xop_options_fence_coins", 0.0))
    age = gauges.get("xop_options_fence_age_seconds", -1.0)
    detail = (
        f"fence {'on' if enabled else 'off'}, {'fresh' if fresh else 'NOT fresh'}, "
        f"{coins} coin(s), refreshed {age:.0f}s ago"
    )
    return FenceState(True, enabled, fresh, coins, age, detail)


def fence_state(
    metrics_url: str, *, fetch: Callable[[str], str] | None = None
) -> FenceState:
    """Ask the engine's metrics endpoint about its fence. Unreachable = not reported."""
    try:
        if fetch is not None:
            text = fetch(metrics_url)
        else:
            with urllib.request.urlopen(metrics_url, timeout=_HTTP_TIMEOUT) as resp:  # noqa: S310
                text = resp.read().decode("utf-8", "replace")
    except (OSError, ValueError) as exc:
        return FenceState(False, False, False, 0, -1.0, f"{metrics_url}: {exc}")
    return parse_fence(text)


class EngineRunningError(signing.SigningError):
    """A signature or push refused because the trading engine may be running.

    A SigningError, so every place the book signs or pushes already reports it
    as that action not being taken, and nothing else in the step is held up.
    """


class EngineCheckedWallet:
    """A wallet whose ``sign`` and ``push`` first make sure the engine is down.

    Everything else (log-in, sync status, keys, fee estimates) passes straight
    through: none of it spends, and the book needs it to say what it would do.
    The engine is asked afresh each time; ``engine_state`` is looked up when
    called, so it can be replaced in tests.
    """

    def __init__(self, wallet: Any, metrics_url: str, *, fenced: bool = False) -> None:
        self._wallet = wallet
        self._metrics_url = metrics_url
        #: [OPTIONS-FENCE] ``options.wallet.engine_fence``: a running engine is
        #: fine while it reports its fence on and fresh.
        self._fenced = fenced

    def __getattr__(self, name: str) -> Any:
        return getattr(self._wallet, name)

    def _require_engine_down(self, what: str) -> None:
        state = engine_state(self._metrics_url)
        if state.clear:
            return
        if self._fenced and state.running:
            fence = fence_state(self._metrics_url)
            if fence.holds:
                return
            raise EngineRunningError(
                f"refused, trading engine up without a working fence: {what} from the wallet it "
                f"trades through, and the engine is {state.describe()}; {fence.detail}"
            )
        raise EngineRunningError(
            f"refused, trading engine up: {what} from the wallet it trades through, and "
            f"the engine is {state.describe()}"
        )

    def sign(self, coin_spends: Sequence[Any]) -> bytes:
        self._require_engine_down("signing")
        return self._wallet.sign(coin_spends)

    def push(self, coin_spends: Sequence[Any], aggregated_signature: bytes) -> bytes:
        self._require_engine_down("sending")
        return self._wallet.push(coin_spends, aggregated_signature)
