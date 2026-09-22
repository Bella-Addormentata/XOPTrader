"""The GUI half of ``data/shutdown.flag``: an engine stop request addressed to one PID.

[shutdown-flag-race 2026-09-12] At 22:41:07 a closing GUI wrote ``shutdown``
into the flag for engine PID 15916. A newly launched GUI terminated that engine
6.6 s later, before it had read the flag, and the closing GUI then logged
"Engine exited gracefully." six milliseconds after the kill. The next engine,
PID 11616, honoured the leftover because the engine's rule was "any flag
younger than 60 s"; it cancelled 1 of 12 offers and exited, and no engine ran
for 35 minutes.

A request now names its target. The engine honours it only if it names the
engine's own PID and was written at or after that process started; content
with no pid line at all (an operator's hand-written flag) needs only the second
half (``cpp/include/xop/util/shutdown_flag.hpp``). This module writes the
request, reads it back, classifies how a stop really ended, removes a request
whose engine was terminated before it could honour it, and names the stop
marker a closing GUI keeps while its stop is under way.

FORMAT v1 (ASCII, LF line endings, replaced atomically)::

    xop-shutdown-request v1
    pid=<target engine PID, decimal 1..4294967295>
    requested_by_pid=<GUI PID>
    written_at=<local ISO-8601, seconds>

Only a line that starts at column 0 with ``pid=`` addresses the request.

[S74 2026-09-20] One optional line more, still v1::

    offers=<cancel|keep>

says what the stop does with the resting offers (``gui/stop_offers.py``; the
engine's table is ``cpp/include/xop/util/stop_offers_policy.hpp``). It is
written only when the operator answered the stop prompt. Without it the engine
applies its own ``engine.shutdown_offers`` -- which is what every stop with
nobody to ask must get, and what every request written before the line existed
already means. It never changes whose request this is.

Standard library only; PyYAML is imported lazily inside
:func:`resolve_shutdown_flag_path`. Anything under ``gui.services`` imports
``EngineBridge`` and therefore PySide6, and this module must stay importable
before a QApplication exists.
"""

from __future__ import annotations

import os
import time
from dataclasses import dataclass
from datetime import datetime
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Iterable, Optional

from gui.stop_offers import POLICY_CANCEL, POLICY_KEEP, parse_policy

FLAG_NAME = "shutdown.flag"
FORMAT_LINE = "xop-shutdown-request v1"
MAX_PID = 0xFFFF_FFFF
MAX_BYTES = 4096
_REPLACE_ATTEMPTS = 10
_REPLACE_RETRY_S = 0.02

_PID_KEY = "pid="
_REQUESTER_KEY = "requested_by_pid="
_OFFERS_KEY = "offers="


# ---------------------------------------------------------------------------
# The format
# ---------------------------------------------------------------------------

class RequestKind(Enum):
    """What a flag's content says about who it is for."""

    UNADDRESSED = "unaddressed"  # no pid line: pre-fix GUI, hand-written
    ADDRESSED = "addressed"      # exactly one usable pid line
    MALFORMED = "malformed"      # a pid line that names no process, or two


class OffersRequest(Enum):
    """[S74] What a request says about the resting offers -- the engine's
    ``StopOffersRequest``, value for value."""

    UNSPECIFIED = "unspecified"    # no offers line: the engine's config default
    CANCEL = POLICY_CANCEL
    KEEP = POLICY_KEEP
    UNRECOGNISED = "unrecognised"  # a line nobody can read, or two of them


@dataclass(frozen=True)
class ParsedRequest:
    """A parsed flag. ``requester_pid`` is GUI-side information only: the
    engine ignores that line, and a relaunched GUI uses it to find the GUI
    that is still waiting on the stop."""

    kind: RequestKind
    pid: Optional[int] = None
    requester_pid: Optional[int] = None
    offers: OffersRequest = OffersRequest.UNSPECIFIED


def render_shutdown_request(
    target_pid: int,
    *,
    requester_pid: int,
    written_at: str,
    offers_policy: Optional[str] = None,
) -> str:
    """Return the v1 request text for *target_pid*.

    [S74] *offers_policy* ``"cancel"`` or ``"keep"`` appends the ``offers=``
    line; None writes none, so the engine's ``engine.shutdown_offers`` decides
    and the bytes are exactly what they were before the line existed. Anything
    else raises: a request must never carry a policy the engine cannot read,
    because the engine would then fall back to its default while the GUI
    believed it had said something.
    """
    if isinstance(target_pid, bool) or not isinstance(target_pid, int) \
            or not 1 <= target_pid <= MAX_PID:
        raise ValueError(
            f"target_pid must be an integer in 1..{MAX_PID}, got {target_pid!r}")
    text = (
        f"{FORMAT_LINE}\n"
        f"pid={target_pid}\n"
        f"requested_by_pid={requester_pid}\n"
        f"written_at={written_at}\n"
    )
    if offers_policy is not None:
        if offers_policy not in (POLICY_CANCEL, POLICY_KEEP):
            raise ValueError(
                f"offers_policy must be {POLICY_CANCEL!r}, {POLICY_KEEP!r} or "
                f"None, got {offers_policy!r}")
        text += f"{_OFFERS_KEY}{offers_policy}\n"
    return text


def _decimal_pid(value: str) -> Optional[int]:
    """The engine's pid-value rule: trimmed ASCII digits naming 1..MAX_PID."""
    value = value.strip(" \t")
    if not value or not (value.isascii() and value.isdigit()):
        return None
    try:
        pid = int(value)
    except ValueError:  # longer than Python's int-from-str digit limit
        return None
    if not 1 <= pid <= MAX_PID:
        return None
    return pid


def parse_shutdown_request(text: str) -> ParsedRequest:
    """Mirror of the engine's ``parse_shutdown_flag``, line for line."""
    if text.startswith("\ufeff"):  # a UTF-8 BOM, decoded
        text = text[1:]
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()  # the text after a final newline is not a line

    seen_pid = False
    target: Optional[int] = None
    requesters: list[Optional[int]] = []
    offers = OffersRequest.UNSPECIFIED
    for raw in lines:
        line = raw[:-1] if raw.endswith("\r") else raw
        if line.startswith(_REQUESTER_KEY):
            requesters.append(_decimal_pid(line[len(_REQUESTER_KEY):]))

        # [S74] Column 0 only, like "pid=". The first offers line is read; a
        # second one makes the policy UNRECOGNISED whatever either says.
        if line.startswith(_OFFERS_KEY):
            if offers is OffersRequest.UNSPECIFIED:
                policy = parse_policy(line[len(_OFFERS_KEY):])
                offers = (OffersRequest(policy) if policy is not None
                          else OffersRequest.UNRECOGNISED)
            else:
                offers = OffersRequest.UNRECOGNISED
            continue

        # Column 0 only: "requested_by_pid=" must never address a request.
        if not line.startswith(_PID_KEY):
            continue
        value = line[len(_PID_KEY):]

        if seen_pid:
            return ParsedRequest(RequestKind.MALFORMED)
        pid = _decimal_pid(value)
        if pid is None:
            return ParsedRequest(RequestKind.MALFORMED)
        seen_pid = True
        target = pid

    requester = requesters[0] if len(requesters) == 1 else None
    if seen_pid:
        return ParsedRequest(RequestKind.ADDRESSED, target, requester, offers)
    return ParsedRequest(RequestKind.UNADDRESSED, offers=offers)


def read_shutdown_request(flag_path: Path) -> Optional[ParsedRequest]:
    """Parse the flag at *flag_path*; None when it is absent or unreadable."""
    try:
        with open(flag_path, "rb") as handle:
            data = handle.read(MAX_BYTES)
    except OSError:  # FileNotFoundError included
        return None
    return parse_shutdown_request(data.decode("utf-8", errors="replace"))


def _unlink_quietly(path: Path) -> None:
    try:
        path.unlink()
    except OSError:
        pass  # best effort: the engine never reads a leftover temp file


def write_shutdown_request(
    flag_path: Path,
    target_pid: int,
    *,
    requester_pid: Optional[int] = None,
    now: Optional[datetime] = None,
    offers_policy: Optional[str] = None,
) -> None:
    """Write a v1 request for *target_pid* to *flag_path*, atomically.

    [S74] *offers_policy* is the operator's answer to the stop prompt
    (``"cancel"``/``"keep"``); None writes no ``offers=`` line and leaves the
    decision to the engine's ``engine.shutdown_offers``.

    The bytes go to a temporary file beside the target and are moved over it
    with ``os.replace``, so the engine never reads a half-written request.
    The engine's reader holds the flag without FILE_SHARE_DELETE, so a
    replace that lands during a read fails with PermissionError and is
    retried. Raises ValueError for an unaddressable PID and OSError when the
    write cannot be completed. The stop marker (:func:`stop_marker_path`) is
    written the same way.
    """
    flag_path = Path(flag_path)
    content = render_shutdown_request(
        target_pid,
        requester_pid=requester_pid or os.getpid(),
        written_at=(now or datetime.now()).isoformat(timespec="seconds"),
        offers_policy=offers_policy,
    )
    flag_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = flag_path.with_name(f"{flag_path.name}.{os.getpid()}.tmp")
    # Bytes, not text: Windows text mode would turn every LF into CRLF.
    tmp.write_bytes(content.encode("ascii"))
    for attempt in range(1, _REPLACE_ATTEMPTS + 1):
        try:
            os.replace(tmp, flag_path)
            return
        except PermissionError:
            if attempt >= _REPLACE_ATTEMPTS:
                _unlink_quietly(tmp)
                raise
            time.sleep(_REPLACE_RETRY_S)
        except OSError:
            _unlink_quietly(tmp)
            raise


def flag_names_pid(flag_path: Path, pid: int) -> bool:
    """True when the flag exists and is ADDRESSED to *pid*."""
    request = read_shutdown_request(flag_path)
    return (
        request is not None
        and request.kind is RequestKind.ADDRESSED
        and request.pid == pid
    )


def remove_if_addressed_to(flag_path: Path, pid: int) -> bool:
    """Remove the flag only when it is ADDRESSED to *pid*. True if removed.

    Read-then-unlink is not atomic: a request replaced in the microseconds
    between the two would be removed as well. Nothing here claims otherwise.
    """
    if not flag_names_pid(flag_path, pid):
        return False
    try:
        Path(flag_path).unlink()
    except OSError:  # FileNotFoundError included
        return False
    return True


# ---------------------------------------------------------------------------
# How a stop ended
# ---------------------------------------------------------------------------

class StopOutcome(Enum):
    GRACEFUL = "graceful"
    EXITED_WITHOUT_CONSUMING = "exited-without-consuming"
    FLAG_GONE_ABNORMAL_EXIT = "flag-gone-abnormal-exit"
    TERMINATED_BEFORE_CONSUMING = "terminated-before-consuming"
    TERMINATED_AFTER_CONSUMING = "terminated-after-consuming"
    TERMINATED_WITHOUT_REQUEST = "terminated-without-request"
    STILL_RUNNING = "still-running"


def classify_stop_outcome(
    returncode: Optional[int],
    *,
    request_written: bool,
    flag_still_names_target: bool,
    forced: bool,
) -> StopOutcome:
    """Say how a stop ended, from facts the GUI can actually observe.

    "The flag is gone" means the ENGINE removed it -- consumed, or discarded
    as not addressed to it. "The flag still names the target" means the
    engine never removed it, whatever the exit code: on Windows ``os.kill``
    with SIGTERM is TerminateProcess with exit code 15, which is exactly how
    the engine died at 22:41:14 on 2026-09-12.
    """
    if returncode is None:
        return StopOutcome.STILL_RUNNING
    if not request_written:
        return StopOutcome.TERMINATED_WITHOUT_REQUEST
    if forced:
        if flag_still_names_target:
            return StopOutcome.TERMINATED_BEFORE_CONSUMING
        return StopOutcome.TERMINATED_AFTER_CONSUMING
    if flag_still_names_target:
        return StopOutcome.EXITED_WITHOUT_CONSUMING
    if returncode == 0:
        return StopOutcome.GRACEFUL
    return StopOutcome.FLAG_GONE_ABNORMAL_EXIT


def outcome_leaves_undelivered_flag(outcome: StopOutcome) -> bool:
    """True when the engine is gone and never removed its request."""
    return outcome in (
        StopOutcome.EXITED_WITHOUT_CONSUMING,
        StopOutcome.TERMINATED_BEFORE_CONSUMING,
    )


# ---------------------------------------------------------------------------
# A relaunched GUI terminated the engine a request was written for
# ---------------------------------------------------------------------------

def cleanup_after_singleton_kill(
    flag_path: Optional[Path],
    killed_engine_pids: Iterable[int],
) -> Optional[str]:
    """Remove a request whose engine this startup terminated. Never raises.

    Returns the operator-facing message when a flag was removed, else None.
    A request ADDRESSED to an engine that was not killed is left alone; so is
    anything unreadable or MALFORMED (the engine discards those itself).
    """
    try:
        if flag_path is None:
            return None
        killed = sorted({int(pid) for pid in killed_engine_pids})
        request = read_shutdown_request(Path(flag_path))
        if request is None or request.kind is RequestKind.MALFORMED:
            return None
        if request.kind is RequestKind.ADDRESSED:
            if request.pid not in killed:
                return None
            Path(flag_path).unlink()
            return (
                f"removed shutdown.flag addressed to engine PID {request.pid}, "
                "which this startup terminated before it honoured the stop "
                "request -- that stop cancelled nothing; the new engine "
                "reconciles the book"
            )
        if not killed:
            return None
        Path(flag_path).unlink()
        return (
            "removed an unaddressed shutdown.flag after terminating engine "
            f"PID(s) {', '.join(str(pid) for pid in killed)} -- a request "
            "written for an engine this startup terminated must not stop the "
            "next one; that stop cancelled nothing"
        )
    except Exception:  # noqa: BLE001 -- startup cleanup must never raise
        return None


# ---------------------------------------------------------------------------
# A stop under way
# ---------------------------------------------------------------------------

#: [review 2026-09-13] Kept beside shutdown.flag for the whole of a GUI's stop
#: (``EngineBridge._stop_engine_process``). The engine consumes shutdown.flag
#: within one poll and only THEN cancels its book, so the flag alone stops
#: showing a relaunched GUI that a stop is under way; this marker shows it until
#: the stop returns. Same v1 content: ``pid=`` names the engine being stopped,
#: ``requested_by_pid=`` the GUI stopping it. The engine never reads it.
STOP_MARKER_NAME = "gui_stop_in_progress.marker"


def stop_marker_path(flag_path: Path) -> Path:
    """The stop marker that lives beside *flag_path*."""
    return Path(flag_path).with_name(STOP_MARKER_NAME)


# ---------------------------------------------------------------------------
# Where the flag lives, before EngineBridge exists
# ---------------------------------------------------------------------------

def _deep_merge(base: dict, overlay: dict) -> None:
    """``gui.services.config_split.deep_merge``, restated without its imports."""
    for key, value in overlay.items():
        if isinstance(value, dict) and isinstance(base.get(key), dict):
            _deep_merge(base[key], value)
        else:
            base[key] = value


def _configured_database_path(
    config_path: Path,
    validate: Optional[Callable[[dict], Any]],
) -> str:
    """``database.path`` as EngineBridge would apply it, or "" for none."""
    try:
        import yaml  # noqa: WPS433 -- lazy: keep this module stdlib-only
    except ImportError:
        return ""
    try:
        text = config_path.read_text(encoding="utf-8")
    except OSError:  # includes the first launch, before the bootstrap copy
        return ""
    try:
        parsed = yaml.safe_load(text)
    except Exception:  # noqa: BLE001 -- any YAML error means "no database.path"
        return ""
    if not isinstance(parsed, dict):
        return ""

    # ConfigService.load deep-merges secrets.yaml over config.yaml.
    secrets_path = config_path.parent / "secrets.yaml"
    try:
        if secrets_path.is_file():
            secrets = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))
            if isinstance(secrets, dict):
                _deep_merge(parsed, secrets)
    except Exception:  # noqa: BLE001 -- ConfigService also continues without it
        pass

    # EngineBridge applies database.path only when ConfigService.load
    # validated the config (engine_bridge.py initialise()).
    if validate is not None:
        try:
            if validate(parsed):
                return ""
        except Exception:  # noqa: BLE001
            return ""

    section = parsed.get("database")
    if not isinstance(section, dict):
        return ""
    value = section.get("path")
    if not isinstance(value, str):
        return ""
    return value.strip()


def resolve_shutdown_flag_path(
    config_path: Path,
    db_override: Optional[Path],
    *,
    validate: Optional[Callable[[dict], Any]] = None,
) -> Path:
    """Where EngineBridge will put ``shutdown.flag`` for this launch.

    Same precedence as EngineBridge: a non-empty ``database.path`` from the
    merged, validated config (resolved against the config's directory) wins;
    else ``--db``; else ``<config dir>/data/xop_trader.db``. *validate* is
    ``ConfigService._validate`` in the GUI; without it the validation gate is
    not applied. A missing or unreadable config means "no database.path".
    Read-only.
    """
    config_path = Path(config_path)
    raw = _configured_database_path(config_path, validate)
    if raw:
        candidate = Path(raw).expanduser()
        if candidate.is_absolute():
            db = candidate.resolve()
        else:
            db = (config_path.parent / candidate).resolve()
    elif db_override:
        db = Path(db_override).resolve()
    else:
        db = (config_path.parent / "data" / "xop_trader.db").resolve()
    return db.parent / FLAG_NAME
