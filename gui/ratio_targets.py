"""Per-pair ratio targets, as the Settings page shows and saves them.

[RATIO-SOT 2026-09-13] ``strategy.ratio_target_by_pair`` is the only per-pair
ratio target the engine reads.  ``cpp/src/config.cpp`` parses it with a (0, 1)
range check, and ``cpp/src/engine.cpp`` consumes it in four places: the
reservation offset, the cross-pair skew (as every OTHER pair's target), and
both ratio-rebalance hysteresis sites.

``pairs[].ratio_target_override`` is a retired GUI-only mirror.  No engine code
has ever read it, and the Wallet tab's Apply rewrites the map without touching
it, so the mirror went stale by construction.  The Settings page used to
display the stale mirror, then REPLACE the whole map on every Save with one
rebuilt from its 2-decimal table: rounding every value, adding entries for
disabled pairs, and reverting whatever an Apply had written since load.

The decisions live here as pure, Qt-free functions so the tests exercise
exactly the code the widget runs.  (Not under ``gui/services``: that package's
``__init__`` eagerly imports PySide6 through EngineBridge.)
"""

from __future__ import annotations

import math
from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any, Optional

#: The engine's per-pair target map, under ``strategy``.
RATIO_TARGETS_KEY = "ratio_target_by_pair"

#: The retired per-pair mirror.  Never read and never written: a Save leaves
#: an existing line exactly as it is, and the operator may delete it.
LEGACY_PAIR_RATIO_KEY = "ratio_target_override"

#: The rest of the Wallet tab Apply's write set
#: (``EngineBridge.apply_wallet_allocation_targets``, which also writes
#: RATIO_TARGETS_KEY).  The Settings page has no widget for any of them, so a
#: Save takes them from the file it saves over, not from its load-time
#: snapshot.
WALLET_OWNED_STRATEGY_KEYS: tuple[str, ...] = (
    "ratio_rebalance_enabled",
    "asset_target_allocations",
    "asset_target_tolerances",
    "ratio_band_enter_by_pair",
)


def valid_ratio(value: Any) -> Optional[float]:
    """Return *value* as a target fraction, or ``None`` if it is not one.

    A target is a finite number strictly inside (0, 1).  That is the engine's
    range check with one difference: the GUI is STRICTER about NaN.  NaN
    fails both comparisons in ``config.cpp``, so the engine lets ``.nan``
    through, while this rejects every non-finite value.  ``bool`` is rejected
    although ``float(True) == 1.0``.  A null, list or mapping (TypeError),
    junk text (ValueError) or an integer too large for a float
    (OverflowError) is rejected rather than raised into a Qt slot.
    """
    if isinstance(value, bool):
        return None
    try:
        fraction = float(value)
    except (TypeError, ValueError, OverflowError):
        return None
    if not math.isfinite(fraction) or not 0.0 < fraction < 1.0:
        return None
    return fraction


def ratio_targets_from(strategy: Any) -> dict[Any, Any]:
    """A shallow copy of ``strategy.ratio_target_by_pair``: raw values, disk order.

    ``{}`` when *strategy* is not a mapping, or the map is missing or is not
    a mapping.
    """
    if not isinstance(strategy, Mapping):
        return {}
    targets = strategy.get(RATIO_TARGETS_KEY)
    if not isinstance(targets, Mapping):
        return {}
    return dict(targets)


def format_ratio_pct(value: Any) -> str:
    """The Ratio Target cell text for *value*: a 2-decimal percent, or blank."""
    fraction = valid_ratio(value)
    if fraction is None:
        return ""
    return f"{fraction * 100.0:.2f}"


def canonical_ratio_text(text: Optional[str]) -> str:
    """*text* as the cell would display it, so a re-render is not an edit.

    ``" 52.630% "`` and ``"52.63"`` both canonicalise to ``"52.63"``.  Text
    that is not a percentage inside (0, 100) comes back stripped, so it never
    matches a displayed value.
    """
    stripped = (text or "").strip().replace("%", "").strip()
    if not stripped:
        return ""
    try:
        pct = float(stripped)
    except ValueError:
        return stripped
    if not math.isfinite(pct) or not 0.0 < pct < 100.0:
        return stripped
    return f"{pct:.2f}"


@dataclass(frozen=True)
class RatioRow:
    """One pairs-table row, as the merge sees it.

    Attributes
    ----------
    name
        The Name cell as it stands now (unstripped, like the saved pairs list).
    text
        The Ratio Target cell text as it stands now.
    shown
        The exact fraction the cell was populated with; ``None`` for blank.
    baseline
        The exact fraction the page last knew to be on disk for this row.
        Equal to *shown* after a file load or a save; the loaded file's value
        after Load from Editor, whose *shown* may differ from it.
    source_name
        The pair name the row was populated under; ``""`` for a row added in
        the UI.  A *name* that differs from it is a rename.
    """

    name: str = ""
    text: str = ""
    shown: Optional[float] = None
    baseline: Optional[float] = None
    source_name: str = ""


# What saving one row asks of its pair's entry (see _row_intent).
_UNTOUCHED = "untouched"
_SET = "set"
_CLEAR = "clear"
_IGNORE = "ignore"


def _row_intent(row: RatioRow) -> tuple[str, Optional[float]]:
    """What saving *row* asks of its pair's entry: ``(kind, fraction)``.

    untouched  The cell shows what it was populated with, and that is what the
               page last knew to be on disk: the entry is not the page's to
               change.
    set        Write *fraction*: an operator edit, or a populated value that
               differs from disk (Load from Editor).
    clear      Remove the entry.
    ignore     Typed text that is not a percentage in (0, 100).  _validate
               blocks such a save, and a collect on every keystroke must not
               raise on it.
    """
    if canonical_ratio_text(row.text) == format_ratio_pct(row.shown):
        # Compare EXACT fractions, never the 2-decimal text: a Load from
        # Editor value of 0.52631 renders "52.63", exactly like a disk value
        # of 0.5263157894736842, and must still be saved.
        if row.shown == row.baseline:
            return _UNTOUCHED, None
        if row.shown is None:
            return _CLEAR, None
        return _SET, row.shown
    raw = (row.text or "").strip().replace("%", "").strip()
    if not raw:
        return _CLEAR, None
    try:
        pct = float(raw)
    except ValueError:
        return _IGNORE, None
    # Parse the RAW typed text, never its canonical form:
    # f"{60.125:.2f}" is "60.12".
    fraction = valid_ratio(pct / 100.0)
    if fraction is None:
        return _IGNORE, None
    return _SET, fraction


def merge_ratio_targets(
    on_disk: Mapping[Any, Any],
    rows: Iterable[RatioRow],
    populated_names: Iterable[str],
) -> dict[Any, Any]:
    """Patch the map *on_disk* with the rows the operator changed.

    1. Start from *on_disk* -- raw values, disk order -- minus the entries the
       engine refuses (see :func:`valid_ratio`).  One such entry makes
       ``config.cpp`` reject the WHOLE config, and a live reload refuse the
       file, so dropping it is a repair; the save logs it as removed.
    2. A name in *populated_names* that no row carries any more was removed or
       renamed away: its entry goes too.
    3. Each named row applies its intent (see :func:`_row_intent`).  An
       untouched row leaves the disk value exactly as it is -- including a
       value written out of band after the page loaded -- except that a
       renamed row carries its old name's value to the new name.
    """
    row_list = list(rows)
    result = {
        name: value
        for name, value in on_disk.items()
        if valid_ratio(value) is not None
    }
    current_names = {row.name for row in row_list}
    for name in populated_names:
        if name not in current_names:
            result.pop(name, None)
    for row in row_list:
        if not row.name:
            continue
        kind, fraction = _row_intent(row)
        if kind == _UNTOUCHED:
            source = row.source_name
            if (
                source
                and source != row.name
                and valid_ratio(on_disk.get(source)) is not None
            ):
                result[row.name] = on_disk[source]
        elif kind == _SET:
            result[row.name] = fraction
        elif kind == _CLEAR:
            result.pop(row.name, None)
    return result


def describe_ratio_target_changes(
    before: Mapping[Any, Any], after: Mapping[Any, Any]
) -> list[str]:
    """One line per entry a save changes, for the audit trail in gui.log.

    ``"N: old -> new"``, ``"N: old -> (removed)"`` and
    ``"N: (absent) -> new"``, with values as ``repr`` so the exact float
    survives.
    """
    lines: list[str] = []
    for name, old in before.items():
        if name not in after:
            lines.append(f"{name}: {old!r} -> (removed)")
        elif after[name] != old:
            lines.append(f"{name}: {old!r} -> {after[name]!r}")
    for name, new in after.items():
        if name not in before:
            lines.append(f"{name}: (absent) -> {new!r}")
    return lines


def describe_ratio_target_conflicts(
    on_disk: Mapping[Any, Any], rows: Iterable[RatioRow]
) -> list[str]:
    """One line per edit that replaces a value changed on disk after load.

    The edit wins -- the operator typed it -- but an out-of-band write (a
    Wallet tab Apply, a hand edit) is never replaced silently: each line names
    the value the edit replaces and the value the page had loaded.  Untouched
    rows are skipped (the disk value survives them), and so are rows added in
    the UI, which have no loaded value to compare.
    """
    lines: list[str] = []
    for row in rows:
        if not row.name or not row.source_name:
            continue
        kind, fraction = _row_intent(row)
        if kind not in (_SET, _CLEAR):
            continue
        now = on_disk.get(row.source_name)
        now_valid = valid_ratio(now)
        if now_valid == row.baseline:
            continue  # the file still holds what the page loaded
        if now_valid == fraction:
            continue  # the file already holds what the edit writes
        saved = "(removed)" if kind == _CLEAR else repr(fraction)
        replaced = repr(now) if row.source_name in on_disk else "(absent)"
        loaded = "(absent)" if row.baseline is None else repr(row.baseline)
        lines.append(
            f"{row.name}: edited to {saved}, replacing {replaced}, which "
            f"changed on disk after this page loaded {loaded}"
        )
    return lines


def legacy_ratio_mirrors(pairs: Any) -> list[tuple[str, Any]]:
    """``(pair name, value)`` for each pair still carrying the retired mirror."""
    if not isinstance(pairs, (list, tuple)):
        return []
    return [
        (str(pair.get("name", "")), pair[LEGACY_PAIR_RATIO_KEY])
        for pair in pairs
        if isinstance(pair, Mapping) and LEGACY_PAIR_RATIO_KEY in pair
    ]
