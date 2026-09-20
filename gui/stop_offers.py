"""[S74 2026-09-20] What a stop does with the resting offers: the GUI half.

The engine's graceful stop used to have one behaviour -- cancel the whole
book -- so whenever offers had to survive a restart the GUI and the engine were
hard-killed instead. A stop now carries a POLICY, ``cancel`` or ``keep``
(``cpp/include/xop/util/stop_offers_policy.hpp``):

* the GUI ASKS on Stop Trading and on window close, and writes the answer into
  the addressed stop request as an ``offers=`` line (``gui/shutdown_flag.py``);
* every stop where nobody can answer -- an OS session end, a signal, any other
  close the operator did not start -- writes NO such line, and the engine's own
  ``engine.shutdown_offers`` decides. The GUI never substitutes its copy of the
  config for the engine's.

This module is the decision and the wording, with no Qt in it, so every branch
is testable without a window: the policy spelling (a mirror of the engine's
``parse_stop_offers_policy``), who gets asked and when, what the prompt says
about the offers that would be left resting, and the capability check that
keeps "Keep" away from an engine build that would cancel anyway.

Standard library only. ``gui.widgets.stop_engine_dialog`` is the Qt part.
"""

from __future__ import annotations

import logging
import sqlite3
import time
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from enum import Enum
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Optional, Sequence

_log = logging.getLogger(__name__)

POLICY_CANCEL = "cancel"
POLICY_KEEP = "keep"
POLICIES = (POLICY_CANCEL, POLICY_KEEP)

#: Where the default lives in config.yaml.
CONFIG_SECTION = "engine"
CONFIG_KEY = "shutdown_offers"

#: Printed by ``xop_trader --help`` (``kStopPolicyHelpToken`` in
#: stop_offers_policy.hpp, same bytes -- tests/test_stop_offers.py holds the two
#: together). An engine built before the policy existed ignores ``offers=keep``
#: and CANCELS the book, so "Keep" is offered only for a binary that prints it.
ENGINE_HELP_TOKEN = "shutdown.flag offers=cancel|keep"


# ---------------------------------------------------------------------------
# The spelling of a policy
# ---------------------------------------------------------------------------

def parse_policy(text: Any) -> Optional[str]:
    """``"cancel"``/``"keep"`` for a readable policy, else None. Never a guess.

    The engine's rule, line for line: spaces and tabs around the word are
    dropped and ASCII case is ignored. ``str.lower()`` alone would ALSO fold
    U+212A KELVIN SIGN to ``k``, which the engine's byte comparison does not,
    so a non-ASCII value is refused before it is lowered.
    """
    if not isinstance(text, str):
        return None
    value = text.strip(" \t")
    if not value.isascii():
        return None
    value = value.lower()
    return value if value in POLICIES else None


def configured_default(config: Optional[Mapping[str, Any]]) -> str:
    """``engine.shutdown_offers`` from a loaded config mapping.

    ``cancel`` when the key is absent -- the engine's own default -- and also
    when it is unreadable: the engine refuses to START on an unreadable value,
    so no running engine has one, and the prompt has to preselect something.
    """
    if not isinstance(config, Mapping):
        return POLICY_CANCEL
    section = config.get(CONFIG_SECTION)
    if not isinstance(section, Mapping):
        return POLICY_CANCEL
    return parse_policy(section.get(CONFIG_KEY)) or POLICY_CANCEL


def merge_shutdown_offers(
    base_engine: Any,
    *,
    selected: str,
    populated: Optional[str],
) -> Optional[dict[str, Any]]:
    """The ``engine`` section a Settings save writes, or None for "no section".

    PATCH, never rebuild -- the rule the ratio map follows (gui/ratio_targets).
    *base_engine* is the section as it is on disk NOW (the loaded snapshot only
    when the file cannot be re-read); *populated* is the policy the dropdown was
    loaded with, None when the key was absent; *selected* is what it shows.

    * An UNTOUCHED dropdown writes nothing: a key absent before the save is
      absent after it (an upgrade changes nothing, and the YAML stays clean),
      and a value edited on disk after this page loaded survives the save.
    * A CHANGED dropdown sets the one key. Every other key in the section, and
      their order, is kept.
    * A value on disk that NOBODY can read ("kep") is replaced by what the
      page shows: the engine refuses to start on it, so leaving it is not
      "changing nothing", it is leaving the engine unable to boot.
    """
    section: dict[str, Any] = (
        dict(base_engine) if isinstance(base_engine, Mapping) else {})
    chosen = parse_policy(selected)
    if chosen is None:
        return section or None
    baseline = parse_policy(populated) if populated is not None else None
    unreadable_on_disk = (
        CONFIG_KEY in section and parse_policy(section[CONFIG_KEY]) is None)
    if unreadable_on_disk or chosen != (baseline or POLICY_CANCEL):
        section[CONFIG_KEY] = chosen
    return section or None


def engine_help_advertises_stop_policy(help_text: Any) -> bool:
    """True when ``xop_trader --help`` output carries :data:`ENGINE_HELP_TOKEN`."""
    return isinstance(help_text, str) and ENGINE_HELP_TOKEN in help_text


# ---------------------------------------------------------------------------
# Who is asked, and what the answer means
# ---------------------------------------------------------------------------

class StopChoice(Enum):
    """The three buttons of the prompt."""

    KEEP = "keep"
    CANCEL = "cancel"
    DONT_STOP = "dont-stop"

    @property
    def policy(self) -> Optional[str]:
        if self is StopChoice.KEEP:
            return POLICY_KEEP
        if self is StopChoice.CANCEL:
            return POLICY_CANCEL
        return None


@dataclass(frozen=True)
class StopDecision:
    """``proceed`` False means the stop (or the close) is called off.

    ``policy`` None means "write no offers line": the engine applies its own
    ``engine.shutdown_offers``.
    """

    proceed: bool
    policy: Optional[str] = None
    asked: bool = False


def decide_stop(
    *,
    engine_running: bool,
    interactive: bool,
    ask: Callable[[], StopChoice],
) -> StopDecision:
    """Decide one stop. *ask* shows the modal prompt; it is called at most once.

    * No engine running: nothing to ask about.
    * Not interactive (session end, signal): NEVER a dialog. A modal box with
      nobody at the machine blocks the shutdown for ever, which is the one
      thing this must not do. No policy is written; the engine's config
      default decides.
    * Otherwise the operator answers, and nothing is remembered: the prompt
      preselects the config default every time.
    * A prompt that RAISES is treated as unanswered -- proceed, no policy --
      rather than trapping the operator in a window that cannot close.
    """
    if not engine_running or not interactive:
        return StopDecision(proceed=True)
    try:
        choice = ask()
    except Exception:  # noqa: BLE001 -- a broken dialog must not block a close
        _log.exception(
            "The stop prompt failed; stopping with no offers policy, so the "
            "engine applies engine.shutdown_offers.")
        return StopDecision(proceed=True)
    if choice is StopChoice.DONT_STOP or not isinstance(choice, StopChoice):
        return StopDecision(proceed=False, asked=True)
    return StopDecision(proceed=True, policy=choice.policy, asked=True)


def policy_to_send(requested: Optional[str], *, keep_supported: bool) -> Optional[str]:
    """The policy that may actually be written into the stop request.

    ``keep`` reaches only an engine that advertises the stop policy. For one
    that does not, the line is withheld ENTIRELY rather than downgraded to
    ``cancel``: that engine ignores the line either way and cancels, and the
    caller is told (None for a requested keep) so it can say so instead of
    letting the operator believe the offers were kept.
    """
    policy = parse_policy(requested) if requested is not None else None
    if policy == POLICY_KEEP and not keep_supported:
        return None
    return policy


# -- a close nobody at the machine started ----------------------------------

#: How long a non-interactive mark stands. A quit that really happens ends the
#: process within seconds. One that does NOT -- Windows lets the user or another
#: application cancel a log-off after commitDataRequest has fired -- leaves the
#: GUI running, and a mark that never expired would then silence the stop prompt
#: for the rest of the session: every later Stop Trading would quietly use the
#: config default. [review #165]
NONINTERACTIVE_MARK_S = 120.0

_noninteractive_quit_reason: Optional[str] = None
_noninteractive_quit_at: float = 0.0


def mark_noninteractive_quit(reason: str, *, now: Optional[float] = None) -> None:
    """Record that the application is quitting with nobody to answer a prompt
    (SIGINT/SIGTERM, an OS session end). Stands for NONINTERACTIVE_MARK_S; the
    first reason is kept while it stands."""
    global _noninteractive_quit_reason, _noninteractive_quit_at
    moment = time.monotonic() if now is None else now
    if noninteractive_quit_reason(now=moment) is None:
        _noninteractive_quit_reason = reason
        _noninteractive_quit_at = moment
        _log.info("Non-interactive quit (%s): the engine stop will not prompt; "
                  "engine.shutdown_offers decides what happens to the offers.",
                  reason)


def noninteractive_quit_reason(*, now: Optional[float] = None) -> Optional[str]:
    """The reason while the mark stands, else None."""
    if _noninteractive_quit_reason is None:
        return None
    moment = time.monotonic() if now is None else now
    if moment - _noninteractive_quit_at > NONINTERACTIVE_MARK_S:
        return None
    return _noninteractive_quit_reason


def reset_noninteractive_quit() -> None:
    """Tests only."""
    global _noninteractive_quit_reason, _noninteractive_quit_at
    _noninteractive_quit_reason = None
    _noninteractive_quit_at = 0.0


# ---------------------------------------------------------------------------
# What would be left resting
# ---------------------------------------------------------------------------

#: offer_log statuses of an offer that still exists on the book.
_STATUS_RESTING = "pending"
_STATUS_CANCEL_PENDING = "cancel_pending"


def effective_expiry_secs(config: Optional[Mapping[str, Any]], pair_name: str) -> int:
    """On-chain expiry for offers on *pair_name*: the pair's
    ``offer_expiry_secs_override`` when present (0 binds: "never expire"),
    else ``strategy.offer_expiry_secs``, else 0. The engine's
    ``effective_offer_expiry_secs``."""
    if not isinstance(config, Mapping):
        return 0

    def _secs(value: Any) -> Optional[int]:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        return int(value) if value >= 0 else None

    pairs = config.get("pairs")
    if isinstance(pairs, Sequence) and not isinstance(pairs, (str, bytes)):
        for pair in pairs:
            if isinstance(pair, Mapping) and pair.get("name") == pair_name:
                override = _secs(pair.get("offer_expiry_secs_override"))
                if override is not None:
                    return override
                break
    strategy = config.get("strategy")
    if isinstance(strategy, Mapping):
        return _secs(strategy.get("offer_expiry_secs")) or 0
    return 0


@dataclass(frozen=True)
class RestingSummary:
    """What offer_log says is on the book right now."""

    resting: int = 0
    cancel_in_flight: int = 0
    per_pair: tuple[tuple[str, int], ...] = ()
    #: Latest ``created_at + expiry`` over resting offers on a pair WITH an
    #: expiry. offer_log's created_at is the row's insert time -- at or after
    #: the posting -- so this is never earlier than the real expiry of an offer
    #: that carries the configured one. None when no such offer exists.
    latest_expiry: Optional[datetime] = None
    #: Resting offers on a pair whose expiry is 0: they never expire on chain.
    without_expiry: int = 0
    #: Resting offers whose created_at could not be read.
    expiry_unknown: int = 0


def _parse_created_at(text: Any) -> Optional[datetime]:
    """SQLite CURRENT_TIMESTAMP: ``YYYY-MM-DD HH:MM:SS`` in UTC."""
    if not isinstance(text, str):
        return None
    try:
        return datetime.strptime(text.strip(), "%Y-%m-%d %H:%M:%S").replace(
            tzinfo=timezone.utc)
    except ValueError:
        return None


def summarise_resting_offers(
    rows: Iterable[Sequence[Any]],
    config: Optional[Mapping[str, Any]],
) -> RestingSummary:
    """Summarise ``(pair_name, status, created_at)`` rows of offer_log."""
    resting = 0
    cancel_in_flight = 0
    without_expiry = 0
    expiry_unknown = 0
    latest: Optional[datetime] = None
    per_pair: dict[str, int] = {}
    for row in rows:
        pair_name, status, created_at = str(row[0]), row[1], row[2]
        if status == _STATUS_CANCEL_PENDING:
            cancel_in_flight += 1
            continue
        if status != _STATUS_RESTING:
            continue
        resting += 1
        per_pair[pair_name] = per_pair.get(pair_name, 0) + 1
        secs = effective_expiry_secs(config, pair_name)
        if secs <= 0:
            without_expiry += 1
            continue
        created = _parse_created_at(created_at)
        if created is None:
            expiry_unknown += 1
            continue
        expires = created + timedelta(seconds=secs)
        if latest is None or expires > latest:
            latest = expires
    return RestingSummary(
        resting=resting,
        cancel_in_flight=cancel_in_flight,
        per_pair=tuple(sorted(per_pair.items())),
        latest_expiry=latest,
        without_expiry=without_expiry,
        expiry_unknown=expiry_unknown,
    )


def read_resting_rows(db_path: Optional[Path]) -> Optional[list[tuple[Any, ...]]]:
    """The rows :func:`summarise_resting_offers` needs, or None when the
    database cannot be read. Opened READ-ONLY (``mode=ro``): the engine owns
    this file, and a prompt must never be able to write to it or create it."""
    if db_path is None:
        return None
    path = Path(db_path)
    if not path.is_file():
        return None
    try:
        conn = sqlite3.connect(f"{path.resolve().as_uri()}?mode=ro", uri=True,
                               timeout=1.0)
    except sqlite3.Error as exc:
        _log.warning("Stop prompt: could not open %s read-only: %s", path, exc)
        return None
    try:
        return list(conn.execute(
            "SELECT pair_name, status, created_at FROM offer_log "
            "WHERE status IN (?, ?)", (_STATUS_RESTING, _STATUS_CANCEL_PENDING)))
    except sqlite3.Error as exc:
        _log.warning("Stop prompt: could not read offer_log in %s: %s", path, exc)
        return None
    finally:
        conn.close()


# ---------------------------------------------------------------------------
# The words of the prompt
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class PromptText:
    title: str
    text: str
    informative: str


def _local(moment: datetime) -> str:
    return moment.astimezone().strftime("%Y-%m-%d %H:%M %Z").strip()


def build_prompt_text(
    summary: Optional[RestingSummary],
    *,
    default_policy: str,
    keep_supported: bool,
    closing: bool,
) -> PromptText:
    """Everything the prompt says. *summary* None means offer_log could not be
    read, and the prompt says so instead of showing a zero."""
    title = "Close XOPTrader" if closing else "Stop Trading"
    lead = ("Closing the window stops the engine. " if closing
            else "Stop the engine? ")

    if summary is None:
        text = lead + ("The number of resting offers could not be read; assume "
                       "there are some.")
    elif summary.resting == 0:
        text = lead + "No offers are resting on the book."
    else:
        pairs = ", ".join(f"{name} {count}" for name, count in summary.per_pair)
        text = lead + f"{summary.resting} offer(s) are resting on the book ({pairs})."
    if summary is not None and summary.cancel_in_flight:
        text += (f" {summary.cancel_in_flight} more already have a cancel in "
                 "flight; neither choice changes those.")

    lines: list[str] = []
    if summary is not None and summary.resting:
        if summary.latest_expiry is not None:
            lines.append(
                "On-chain expiry is on: a kept offer posted under the current "
                "setting stops being takeable by about "
                f"{_local(summary.latest_expiry)}. An offer posted before the "
                "expiry was turned on carries none.")
        if summary.without_expiry:
            lines.append(
                f"{summary.without_expiry} of them are on a pair with NO "
                "on-chain expiry (offer_expiry_secs is 0): kept, they stay "
                "takeable until an engine or you cancel them.")
        if summary.expiry_unknown:
            lines.append(
                f"The posting time of {summary.expiry_unknown} of them could "
                "not be read, so no expiry is shown for them.")
    lines.append(
        "Keep offers on the book: nothing is cancelled. The offers stay "
        "takeable with NO engine behind them -- no repricing, no TTL, no dead "
        "man's switch -- until the next start re-adopts them.")
    lines.append(
        "Cancel all offers: the engine cancels the whole book on the way down; "
        "with full blocks the cancel spends can take a while to confirm, and "
        "the next start finishes any that did not.")
    lines.append("Don't stop: "
                 + ("the window stays open and the engine keeps running."
                    if closing else "the engine keeps running."))
    if not keep_supported:
        lines.append(
            "KEEP IS NOT AVAILABLE: this engine build predates the stop policy "
            "and would cancel the book whatever the request says.")
    lines.append(
        f"Preselected: the config default, engine.shutdown_offers = "
        f"{default_policy}. Stops with nobody to ask (log-off, shutdown, a "
        "signal) use that default without this prompt.")
    return PromptText(title=title, text=text, informative="\n\n".join(lines))


def default_choice(default_policy: str, *, keep_supported: bool) -> StopChoice:
    """The button the prompt preselects: the config default -- or Cancel when
    the default is keep but this engine cannot honour it."""
    if parse_policy(default_policy) == POLICY_KEEP and keep_supported:
        return StopChoice.KEEP
    return StopChoice.CANCEL
