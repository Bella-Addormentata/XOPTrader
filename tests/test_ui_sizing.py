"""UI controls must stay proportionate to the text and their row.

The base ``QPushButton`` rule sets ``min-height: 28px`` and ``padding: 8px
20px``. A widget-level ``setFixedHeight()`` cannot shrink that -- the
stylesheet wins -- so the Cancel button in the orders table rendered 46px tall
inside a 30px row. The ``compact`` property opts a button out of those metrics.

These tests pin the fix at the size level rather than by inspecting source, and
pin that it still holds when the operator changes the UI font size.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtCore import QPoint, Qt  # noqa: E402
from PySide6.QtTest import QTest  # noqa: E402
from PySide6.QtWidgets import (  # noqa: E402
    QApplication, QCheckBox, QHBoxLayout, QLabel, QPushButton,
    QTableWidget, QWidget,
)

import gui.theme as theme  # noqa: E402

NEWLINE = chr(10)


@pytest.fixture(scope="module")
def app():
    """A QApplication on the SAME base style production uses.

    theme.apply_theme() sets Fusion (theme.py) and gui/app.py relies on it,
    but these tests apply only the stylesheet -- so without this they measure
    whatever base style the host platform supplies, and a fixture reused from
    another test module could bring a different one again.  Row defaults,
    size hints and SE_CheckBoxClickRect all come from that style, so the
    geometry asserted here would be geometry no user runs, and could differ
    between a developer machine and CI.

    NOTE there is no runtime assertion that this took effect: once a
    stylesheet is applied Qt wraps the style in a QStyleSheetStyle whose
    objectName is empty, and PySide6 does not expose its baseStyle(), so the
    style actually in force cannot be read back.  Setting it here is the
    guarantee.
    """
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


def _row_height(delta: int = 0) -> int:
    table = QTableWidget(1, 1)
    theme.fit_row_height(table)          # as both production tables do
    table.ensurePolished()
    return table.verticalHeader().defaultSectionSize()


def _wrapped_cell_height() -> int:
    """The button as production actually places it: inside a cell layout.

    settings.py wraps Remove in a QWidget + QHBoxLayout with vertical
    margins, so the bare button height understates what the row must hold.
    Testing the bare widget passed while the real cell still overflowed.
    """
    btn = QPushButton("Remove")
    btn.setObjectName("dangerButton")
    btn.setProperty("compact", True)
    cell = QWidget()
    layout = QHBoxLayout(cell)
    layout.setContentsMargins(4, 1, 4, 1)
    layout.setSpacing(4)
    layout.addWidget(btn)
    cell.ensurePolished()
    return cell.sizeHint().height()


def _button_height(compact: bool, delta: int = 0) -> int:
    btn = QPushButton("Cancel")
    btn.setObjectName("dangerButton")
    if compact:
        btn.setProperty("compact", True)
    btn.ensurePolished()
    return btn.sizeHint().height()


def test_an_uncompact_button_really_does_overflow_the_row(app):
    """The bug, pinned: without the property the button exceeds its row."""
    app.setStyleSheet(theme.get_stylesheet())
    assert _button_height(compact=False) > _row_height()


def test_the_compact_button_fits_its_row(app):
    app.setStyleSheet(theme.get_stylesheet())
    assert _button_height(compact=True) <= _row_height()


@pytest.mark.parametrize("delta", [-2, 0, 2, 4, 6])
def test_it_still_fits_at_every_offered_font_size(app, delta):
    """A hard-coded pixel height would clip here; the style must scale."""
    app.setStyleSheet(theme.get_stylesheet(font_size_delta=delta))
    assert _button_height(compact=True, delta=delta) <= _row_height(delta)


@pytest.mark.parametrize("delta", [-2, 0, 2, 4, 6])
def test_the_button_renders_full_size_inside_a_real_table(app, delta):
    """Measured IN the table, not standalone.

    A standalone wrapper measurement said everything fit while the placed
    button rendered at 7px: QTableView::item vertical padding is subtracted
    from the cell rect before Qt places a cell widget, and only an actual
    setCellWidget placement sees that.
    """
    from PySide6.QtWidgets import QTableWidgetItem

    app.setStyleSheet(theme.get_stylesheet(font_size_delta=delta))
    table = QTableWidget(1, 2)
    theme.fit_row_height(table)
    table.setItem(0, 0, QTableWidgetItem("x"))
    button = QPushButton("Remove")
    button.setObjectName("dangerButton")
    button.setProperty("compact", True)
    cell = QWidget()
    layout = QHBoxLayout(cell)
    layout.setContentsMargins(4, 1, 4, 1)
    layout.addWidget(button)
    table.setCellWidget(0, 1, cell)
    table.resize(420, 220)
    table.show()
    QApplication.processEvents()
    try:
        assert button.height() >= button.sizeHint().height(), (
            f"button rendered {button.height()}px of its "
            f"{button.sizeHint().height()}px inside the row"
        )
    finally:
        table.hide()


def test_rows_are_no_taller_than_the_control_requires(app):
    """Growing every row beyond what the button needs would be a regression.

    Not a pinned pixel count: the previous version asserted == 30, which is
    the WINDOWS answer -- Linux fonts make the same button a few px taller,
    so CI failed at 33 while the behaviour was correct on both. The invariant
    is the formula, not one platform's evaluation of it.
    """
    app.setStyleSheet(theme.get_stylesheet())
    probe = QPushButton("X")
    probe.setObjectName("dangerButton")
    probe.setProperty("compact", True)
    probe.ensurePolished()
    needed = probe.sizeHint().height() + 11    # margins + item padding + grid
    row = _row_height(0)
    assert row == max(30, needed), (
        f"row {row}px vs floor 30 / needed {needed}px"
    )
    assert row >= 30, "rows shrank below the baseline look"


def test_the_compact_button_keeps_its_danger_palette(app):
    """Compact changes metrics only -- the red variant must survive it."""
    app.setStyleSheet(theme.get_stylesheet())
    css = theme.get_stylesheet()
    # The ID rules carry the colours and must not set metrics, or they would
    # outrank the compact attribute selector and re-break the sizing.
    danger = css.split("QPushButton#dangerButton {")[1].split("}")[0]
    assert "background-color" in danger
    assert "min-height" not in danger and "padding" not in danger


# ---------------------------------------------------------------------------
# Check/radio indicators
#
# The indicator was a hard-coded 20px box with a 2px border -- 24px beside 16px
# text, half again the height of the label it marks, and unaffected by the
# operator's font-size setting.
# ---------------------------------------------------------------------------

def _text_height() -> int:
    lbl = QLabel("Xg")
    lbl.ensurePolished()
    return lbl.sizeHint().height()


def _checkbox_height() -> int:
    cb = QCheckBox()
    cb.ensurePolished()
    return cb.sizeHint().height()


def test_the_checkbox_is_not_taller_than_its_own_label(app):
    app.setStyleSheet(theme.get_stylesheet())
    assert _checkbox_height() <= _text_height() + 2


def test_the_indicator_scales_with_the_font(app):
    """It must TRACK the text, not merely stay under it.

    An upper bound alone is not a scaling test: a hard-coded 13px indicator
    would satisfy "no taller than the text" at every delta while having
    stopped responding to the setting entirely.  Compare the measurements
    across deltas so that regression fails here.
    """
    measured = []
    for delta in (-2, 0, 2, 4, 6):
        app.setStyleSheet(theme.get_stylesheet(font_size_delta=delta))
        measured.append((delta, _checkbox_height(), _text_height()))

    for delta, box, text in measured:
        assert box <= text + 2, f"delta={delta}: indicator {box}px vs text {text}px"

    boxes = [box for _d, box, _t in measured]
    assert len(set(boxes)) > 1, (
        f"indicator was {boxes[0]}px at every font size -- it no longer "
        "responds to font_size_delta"
    )
    assert boxes == sorted(boxes), f"indicator did not grow with the font: {boxes}"
    assert boxes[-1] > boxes[0], "largest font gave no larger indicator"


def test_a_checkbox_fits_inside_a_table_row(app):
    """The pairs table puts one in column 0."""
    app.setStyleSheet(theme.get_stylesheet())
    assert _checkbox_height() <= _row_height()


def _rule_value(css, header, prop):
    """Value of *prop* inside the rule whose header line is *header*.

    Anchored on a leading newline: the combined
    "QCheckBox::indicator, QRadioButton::indicator {" rule also CONTAINS
    the substring "QRadioButton::indicator {", so an unanchored split reads
    the wrong rule and silently asserts nothing.
    """
    block = css.split(NEWLINE + header)[1].split("}")[0]
    return int(block.split(prop + ":")[1].split("px")[0].strip())


def test_the_radio_indicator_stays_round(app):
    """Its radius must follow the box, or it renders as a rounded square."""
    for delta in (-2, 0, 2, 4, 6):
        css = theme.get_stylesheet(font_size_delta=delta)
        box = _rule_value(
            css, "QCheckBox::indicator, QRadioButton::indicator {", "width")
        radius = _rule_value(
            css, "QRadioButton::indicator {", "border-radius")
        # A circle needs half the BORDERED box (1px border each side).
        assert radius * 2 >= box + 2, (
            "delta=%d: box %dpx + 2px border, radius %dpx"
            % (delta, box, radius)
        )


# ---------------------------------------------------------------------------
# Hit target
#
# A checkbox WITH a label is easy to hit -- the text is part of the button.
# An unlabeled one is exactly as big as its indicator, so making the indicator
# proportionate shrank the pointer target with it: 21x15, down from 24x24.
# The two pairs-table toggles are unlabeled.
# ---------------------------------------------------------------------------

def _clicks_at(widget, x: int, y: int) -> bool:
    """Whether a real click at (x, y) toggles *widget*.

    Asserted with QTest rather than inferred from sizeHint()/minimumSize().
    Inferring is what hid the original bug: QCheckBox hit tests against
    SE_CheckBoxClickRect, not its own rect, so a widget can be 24x24 while
    only the ~15px indicator responds -- and a size-based test cannot see
    the difference.
    """
    before = widget.isChecked()
    QTest.mouseClick(widget, Qt.MouseButton.LeftButton,
                     Qt.KeyboardModifier.NoModifier, QPoint(x, y))
    QApplication.processEvents()
    return widget.isChecked() != before


@pytest.mark.parametrize("delta", [-2, 0, 2, 4, 6])
def test_every_corner_of_an_unlabeled_checkbox_toggles_it(app, delta):
    app.setStyleSheet(theme.get_stylesheet(font_size_delta=delta))
    size = theme.MIN_HIT_TARGET_PX
    cb = theme.HitTargetCheckBox()
    cb.resize(size, size)
    cb.show()
    app.processEvents()
    for x, y in ((size // 2, size // 2), (size - 2, size // 2),
                 (size - 2, size - 2), (1, 1)):
        assert _clicks_at(cb, x, y), f"click at ({x},{y}) did not toggle"


def test_a_plain_checkbox_shows_why_the_subclass_is_needed(app):
    """Pins the defect: a mere minimum size does NOT widen the target."""
    app.setStyleSheet(theme.get_stylesheet())
    plain = QCheckBox()
    plain.setMinimumSize(theme.MIN_HIT_TARGET_PX, theme.MIN_HIT_TARGET_PX)
    plain.resize(theme.MIN_HIT_TARGET_PX, theme.MIN_HIT_TARGET_PX)
    plain.show()
    app.processEvents()
    assert _clicks_at(plain, 12, 12), "centre should always toggle"
    # The corner is inside the widget but outside SE_CheckBoxClickRect.
    assert not _clicks_at(plain, 22, 22), (
        "a plain QCheckBox toggled at its corner -- if Qt changed this, the "
        "HitTargetCheckBox subclass may no longer be necessary"
    )


def test_the_indicator_stays_proportionate_to_the_text(app):
    """The target grows; the drawn box must not."""
    app.setStyleSheet(theme.get_stylesheet())
    bare = QCheckBox()
    bare.ensurePolished()
    assert bare.sizeHint().height() <= _text_height() + 2


def test_both_pairs_table_toggles_use_the_subclass():
    """A future unlabeled checkbox must not silently miss this."""
    from pathlib import Path
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "settings.py").read_text(encoding="utf-8")
    assert "QCheckBox()" not in source, (
        "a bare unlabeled QCheckBox in settings.py has no enlarged hit target"
    )
    assert source.count("HitTargetCheckBox()") == 2


def test_the_row_probe_measures_the_style_production_uses(app):
    """Both in-row buttons are #dangerButton, whose ID rule wins on weight.

    Measuring a style the real control does not use would leave row height
    depending on the two weights coincidentally sharing vertical metrics.
    """
    app.setStyleSheet(theme.get_stylesheet())
    real = QPushButton("Remove")
    real.setObjectName("dangerButton")
    real.setProperty("compact", True)
    real.ensurePolished()

    table = QTableWidget(1, 1)
    theme.fit_row_height(table)
    row = table.verticalHeader().defaultSectionSize()
    assert row >= real.sizeHint().height(), (
        f"row {row}px cannot hold its own button ({real.sizeHint().height()}px)"
    )


def test_the_danger_rule_is_documented_accurately():
    """The comment claimed the ID rules set only colours; they set weight too."""
    css = theme.get_stylesheet()
    block = css.split("QPushButton#dangerButton {")[1].split("}")[0]
    assert "font-weight" in block, "danger rule no longer sets weight"
    from pathlib import Path
    source = Path(theme.__file__).read_text(encoding="utf-8")
    assert "those set only colours" not in source
