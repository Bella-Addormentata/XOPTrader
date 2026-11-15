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
fails closed. Nothing here touches the wallet.
"""

from __future__ import annotations

import dataclasses
import subprocess
import sys
import urllib.error
import urllib.request
from collections.abc import Callable

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
