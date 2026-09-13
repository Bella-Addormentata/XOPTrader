"""The pure ratio-target decisions the Settings page runs on every Save.

[RATIO-SOT 2026-09-13] ``gui.ratio_targets`` holds every decision the
Settings widget makes about ``strategy.ratio_target_by_pair``; these tests call
it directly.  The same decisions end to end, through the real SettingsWidget
and the real writer, are in ``tests/test_ratio_target_save.py``.
"""

from __future__ import annotations

import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402

from gui.ratio_targets import (  # noqa: E402
    RatioRow,
    canonical_ratio_text,
    describe_ratio_target_changes,
    describe_ratio_target_conflicts,
    format_ratio_pct,
    merge_ratio_targets,
)

DBX_TARGET = 0.5263157894736842
BYC_TARGET = 0.9090909090909091


def test_untouched_rows_keep_every_disk_entry_exactly_and_in_order():
    on_disk = {"XCH/DBX": DBX_TARGET, "XCH/BYC": BYC_TARGET, "NOT/SHOWN": 0.3}
    rows = [
        RatioRow(name="XCH/DBX", text="52.63", shown=DBX_TARGET,
                 baseline=DBX_TARGET, source_name="XCH/DBX"),
        RatioRow(name="XCH/BYC", text="90.91", shown=BYC_TARGET,
                 baseline=BYC_TARGET, source_name="XCH/BYC"),
    ]

    result = merge_ratio_targets(on_disk, rows, {"XCH/DBX", "XCH/BYC"})

    assert result == on_disk
    # Dict order only.  The writer keeps existing on-disk key positions
    # (config_split._apply_map), so this does not protect the file layout.
    assert list(result) == list(on_disk)


def test_rename_moves_the_disk_value_to_the_new_name():
    row = RatioRow(name="A/C", text="40.00", shown=0.4, baseline=0.4,
                   source_name="A/B")

    assert merge_ratio_targets({"A/B": 0.4}, [row], {"A/B"}) == {"A/C": 0.4}


def test_entries_the_engine_rejects_are_dropped_not_written_back():
    """Any of these makes config.cpp reject the WHOLE config (NaN aside,
    which the GUI rejects more strictly than the engine does), so writing one
    back would keep a file the engine cannot start on."""
    on_disk = {
        "A/B": 1.5,
        "C/D": "abc",
        "E/F": 0.0,
        "G/H": True,
        "K/L": None,          # a bare `K/L:` in YAML -- TypeError in float()
        "M/N": [0.5],         # TypeError in float()
        "O/P": float("nan"),
        "Q/R": 10 ** 400,     # OverflowError in float()
        "I/J": 0.25,
    }

    assert merge_ratio_targets(on_disk, [], set()) == {"I/J": 0.25}


def test_rewrites_of_the_displayed_value_canonicalise_to_the_display():
    assert format_ratio_pct(DBX_TARGET) == "52.63"
    assert canonical_ratio_text("52.630%") == "52.63"
    assert canonical_ratio_text(" 52.63 ") == "52.63"
    assert canonical_ratio_text("60") == "60.00"
    assert canonical_ratio_text("") == ""
    assert format_ratio_pct(None) == ""


def test_an_edit_uses_the_typed_value_and_keeps_other_entries():
    """Parsing the canonical text would save 0.6012: f"{60.125:.2f}" is "60.12"."""
    row = RatioRow(name="A/B", text="60.125", shown=0.4, baseline=0.4,
                   source_name="A/B")

    result = merge_ratio_targets({"A/B": 0.4, "C/D": 0.7}, [row], {"A/B"})

    assert result == pytest.approx({"A/B": 0.60125, "C/D": 0.7})


def test_a_populated_value_that_differs_from_disk_is_saved_exactly():
    """Load from Editor can populate a value that renders like the disk value."""
    on_disk = {"A/B": DBX_TARGET, "C/D": 0.4, "E/F": 0.3}
    rows = [
        # 0.52631 renders "52.63", exactly like 0.5263157894736842.
        RatioRow(name="A/B", text="52.63", shown=0.52631,
                 baseline=DBX_TARGET, source_name="A/B"),
        # The editor removed C/D's entry: a blank cell over a loaded 0.4.
        RatioRow(name="C/D", text="", shown=None, baseline=0.4,
                 source_name="C/D"),
    ]

    result = merge_ratio_targets(on_disk, rows, {"A/B", "C/D"})

    assert result == {"A/B": 0.52631, "E/F": 0.3}


def test_change_lines_name_every_changed_added_and_removed_entry():
    before = {"A/B": 0.4, "C/D": 0.7, "E/F": 0.2}
    after = {"A/B": 0.4, "C/D": 0.6, "G/H": 0.5}

    assert describe_ratio_target_changes(before, after) == [
        "C/D: 0.7 -> 0.6",
        "E/F: 0.2 -> (removed)",
        "G/H: (absent) -> 0.5",
    ]


def test_conflicts_name_only_edits_that_replace_a_value_changed_on_disk():
    on_disk = {"A/B": 0.55, "C/D": 0.8, "E/F": 0.3, "K/L": 0.25, "I/J": 0.9}
    rows = [
        # Edited, and the file moved 0.5 -> 0.55 since load: the edit wins,
        # and the value it replaces is named.
        RatioRow(name="A/B", text="60", shown=0.5, baseline=0.5,
                 source_name="A/B"),
        # Untouched: the newer file value survives, so nothing is replaced.
        RatioRow(name="C/D", text="70.00", shown=0.7, baseline=0.7,
                 source_name="C/D"),
        # Edited, but the file still holds what the page loaded.
        RatioRow(name="E/F", text="40", shown=0.3, baseline=0.3,
                 source_name="E/F"),
        # Cleared over a value that moved 0.2 -> 0.25.
        RatioRow(name="K/L", text="", shown=0.2, baseline=0.2,
                 source_name="K/L"),
        # Added in the UI: nothing was loaded to compare with.
        RatioRow(name="I/J", text="10", shown=None, baseline=None,
                 source_name=""),
    ]

    assert describe_ratio_target_conflicts(on_disk, rows) == [
        "A/B: edited to 0.6, replacing 0.55, which changed on disk after "
        "this page loaded 0.5",
        "K/L: edited to (removed), replacing 0.25, which changed on disk "
        "after this page loaded 0.2",
    ]


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(pytest.main([__file__, "-v"]))
