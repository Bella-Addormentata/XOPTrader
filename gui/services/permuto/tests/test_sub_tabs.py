"""The in-page section switcher.

Two behaviours carry the whole reason this replaced QTabWidget, and both are
asserted rather than described: the bar WRAPS instead of hiding sections, and
the dirty marker does not move the buttons.
"""

from __future__ import annotations

import pytest

pytest.importorskip("PySide6")

from PySide6.QtWidgets import QApplication, QLabel  # noqa: E402

from gui.widgets.sub_tabs import DIRTY_MARK, SubTabBar, SubTabPages  # noqa: E402


@pytest.fixture(scope="module")
def qapp():
    return QApplication.instance() or QApplication([])


@pytest.fixture()
def pages(qapp):
    p = SubTabPages()
    for title in ("Identity", "Markets", "Quoting", "Activity"):
        p.add_page(title, QLabel(title))
    return p


# --------------------------------------------------------------------------- #
# The two reasons this exists
# --------------------------------------------------------------------------- #

def test_the_bar_wraps_rather_than_hiding_sections(qapp):
    """QTabWidget scrolls the overflow behind two chevrons.

    With eleven sections that put Appearance and Advanced off screen with
    nothing to say they existed. A section the operator cannot see is a
    setting they cannot find.
    """
    bar = SubTabBar()
    for title in ("Connection", "Trading Pairs", "Strategy", "Risk Management",
                  "Monitoring", "Fees & Reserves", "Arbitrage",
                  "Depeg & Aging", "CoinGecko", "Appearance", "Advanced"):
        bar.add_section(title)

    layout = bar.layout()
    one_row = layout.heightForWidth(2000)
    narrow = layout.heightForWidth(600)
    assert narrow > one_row, "the bar did not wrap; sections would be hidden"

    # Every button is still a real, laid-out child -- nothing was dropped.
    assert bar.count() == 11
    assert all(b.width() > 0 for b in bar._buttons)


def test_the_dirty_marker_does_not_move_the_buttons(pages):
    """Settings appended " *" to the tab title, which changed its width and
    reflowed every tab after it -- so typing in a field could slide the bar
    out from under the pointer mid-click."""
    before = [b.width() for b in pages.bar._buttons]
    positions = [b.pos().x() for b in pages.bar._buttons]

    pages.set_dirty(0, True)
    pages.set_dirty(1, True)

    assert [b.width() for b in pages.bar._buttons] == before
    assert [b.pos().x() for b in pages.bar._buttons] == positions
    assert pages.bar._buttons[0].text().endswith(DIRTY_MARK)


# --------------------------------------------------------------------------- #
# Switching
# --------------------------------------------------------------------------- #

def test_the_first_section_is_selected_on_build(pages):
    assert pages.current_index() == 0
    assert pages.stack.currentIndex() == 0
    assert pages.bar._buttons[0].isChecked()


def test_switching_moves_the_bar_and_the_stack_together(pages):
    pages.set_current(2)
    assert pages.current_index() == 2
    assert pages.stack.currentIndex() == 2
    assert pages.bar._buttons[2].isChecked()
    assert not pages.bar._buttons[0].isChecked()


def test_only_one_section_is_ever_checked(pages):
    for i in range(pages.bar.count()):
        pages.set_current(i)
        checked = [b.isChecked() for b in pages.bar._buttons]
        assert sum(checked) == 1, "exclusivity broken at index %d" % i


def test_clicking_the_current_section_does_not_deselect_it(pages):
    """A checkable button unchecks itself on a second click, which would
    leave the bar showing no selection at all while a page is displayed."""
    pages.set_current(1)
    pages.bar._buttons[1].click()
    assert pages.bar._buttons[1].isChecked()
    assert pages.current_index() == 1


def test_an_out_of_range_index_is_ignored(pages):
    pages.set_current(1)
    pages.set_current(99)
    pages.set_current(-5)
    assert pages.current_index() == 1


def test_changing_section_emits_once(pages):
    seen = []
    pages.currentChanged.connect(seen.append)
    pages.set_current(3)
    pages.set_current(3)      # no-op, must not re-emit
    assert seen == [3]


# --------------------------------------------------------------------------- #
# Keyboard -- implemented here because a button row does not inherit it
# --------------------------------------------------------------------------- #

def test_arrow_keys_move_between_sections(pages, qapp):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QKeyEvent

    pages.set_current(0)
    right = QKeyEvent(QEvent.KeyPress, Qt.Key_Right, Qt.NoModifier)
    pages.bar.keyPressEvent(right)
    assert pages.current_index() == 1

    left = QKeyEvent(QEvent.KeyPress, Qt.Key_Left, Qt.NoModifier)
    pages.bar.keyPressEvent(left)
    assert pages.current_index() == 0


def test_arrow_keys_stop_at_the_ends_rather_than_wrapping(pages, qapp):
    from PySide6.QtCore import QEvent, Qt
    from PySide6.QtGui import QKeyEvent

    pages.set_current(0)
    pages.bar.keyPressEvent(
        QKeyEvent(QEvent.KeyPress, Qt.Key_Left, Qt.NoModifier))
    assert pages.current_index() == 0

    last = pages.bar.count() - 1
    pages.set_current(last)
    pages.bar.keyPressEvent(
        QKeyEvent(QEvent.KeyPress, Qt.Key_Right, Qt.NoModifier))
    assert pages.current_index() == last


# --------------------------------------------------------------------------- #
# Dirty bookkeeping
# --------------------------------------------------------------------------- #

def test_clear_dirty_clears_every_section(pages):
    for i in range(pages.bar.count()):
        pages.set_dirty(i, True)
    pages.clear_dirty()
    assert not any(pages.bar.is_dirty(i) for i in range(pages.bar.count()))
    assert pages.bar._buttons[0].text() == "Identity"


def test_the_title_survives_a_dirty_round_trip(pages):
    pages.set_dirty(2, True)
    pages.set_dirty(2, False)
    assert pages.bar._buttons[2].text() == "Quoting"
    assert pages.bar.title(2) == "Quoting"
