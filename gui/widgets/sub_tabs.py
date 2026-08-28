"""In-page section switcher, shared by Settings and Permuto.

WHY NOT QTabWidget, WHICH IS WHAT SETTINGS USED.  Two reasons, both of which
show up on the Settings page today at ordinary window sizes.

**Tabs disappear.**  QTabWidget does not wrap.  Past the width it can fit it
puts two small chevrons at the end of the bar and scrolls the rest out of
sight -- so with eleven sections, "Advanced" and "Appearance" are simply not
on screen, and nothing indicates they exist.  A setting the operator cannot
see is a setting they cannot find, and the usual outcome is editing YAML by
hand instead.  The bar here WRAPS: every section is always visible, the page
gets one or two extra rows of buttons when narrow, and nothing hides.

**The dirty marker moved the buttons.**  Settings signalled unsaved edits by
appending `" *"` to the tab title, which changes that tab's width, which
reflows every tab after it.  Typing in a field could therefore slide the tab
bar out from under the pointer mid-click.  Each button here is given a fixed
width at construction, measured from its label PLUS the marker, so the marker
appears and disappears without moving anything.

Keyboard navigation is implemented rather than inherited: QTabWidget gives
Ctrl+Tab and arrow keys for free and a plain button row does not, so losing
them would have been a real regression for anyone who does not use a mouse.
"""

from __future__ import annotations

from typing import Optional

from PySide6.QtCore import QPoint, QRect, QSize, Qt, Signal
from PySide6.QtWidgets import (
    QHBoxLayout,
    QLayout,
    QPushButton,
    QSizePolicy,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

__all__ = ["FlowLayout", "SubTabBar", "SubTabPages"]

#: Appended to a section's label when it has unsaved changes.
DIRTY_MARK = " •"


class FlowLayout(QLayout):
    """Left-to-right layout that wraps to a new row instead of clipping.

    Qt ships no flow layout, and this is the whole reason the bar can promise
    that no section is ever hidden.
    """

    def __init__(self, parent: Optional[QWidget] = None,
                 spacing: int = 6) -> None:
        super().__init__(parent)
        self._items: list = []
        self._spacing = spacing
        self.setContentsMargins(0, 0, 0, 0)

    # -- QLayout plumbing --------------------------------------------------- #
    def addItem(self, item) -> None:          # noqa: N802 - Qt override
        self._items.append(item)

    def count(self) -> int:
        return len(self._items)

    def itemAt(self, index):                  # noqa: N802 - Qt override
        if 0 <= index < len(self._items):
            return self._items[index]
        return None

    def takeAt(self, index):                  # noqa: N802 - Qt override
        if 0 <= index < len(self._items):
            return self._items.pop(index)
        return None

    def expandingDirections(self):            # noqa: N802 - Qt override
        return Qt.Orientations(Qt.Orientation(0))

    def hasHeightForWidth(self) -> bool:      # noqa: N802 - Qt override
        return True

    def heightForWidth(self, width: int) -> int:   # noqa: N802 - Qt override
        return self._arrange(QRect(0, 0, width, 0), apply=False)

    def setGeometry(self, rect: QRect) -> None:    # noqa: N802 - Qt override
        super().setGeometry(rect)
        self._arrange(rect, apply=True)

    def sizeHint(self) -> QSize:              # noqa: N802 - Qt override
        return self.minimumSize()

    def minimumSize(self) -> QSize:           # noqa: N802 - Qt override
        size = QSize()
        for item in self._items:
            size = size.expandedTo(item.minimumSize())
        return size

    # -- the wrap ----------------------------------------------------------- #
    def _arrange(self, rect: QRect, *, apply: bool) -> int:
        x, y, row_height = rect.x(), rect.y(), 0
        for item in self._items:
            hint = item.sizeHint()
            next_x = x + hint.width() + self._spacing
            if next_x - self._spacing > rect.right() and row_height > 0:
                x = rect.x()
                y = y + row_height + self._spacing
                next_x = x + hint.width() + self._spacing
                row_height = 0
            if apply:
                item.setGeometry(QRect(QPoint(x, y), hint))
            x = next_x
            row_height = max(row_height, hint.height())
        return y + row_height - rect.y()


class SubTabBar(QWidget):
    """A wrapping row of mutually exclusive section buttons."""

    currentChanged = Signal(int)  # noqa: N815 - matches Qt naming

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._buttons: list[QPushButton] = []
        self._titles: list[str] = []
        self._dirty: list[bool] = []
        self._current = -1

        self._flow = FlowLayout(self)
        self._flow.setSpacing(6)
        self.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Minimum)

    # -- building ----------------------------------------------------------- #
    def add_section(self, title: str) -> int:
        """Append a section and return its index."""
        index = len(self._buttons)
        btn = QPushButton(title)
        btn.setCheckable(True)
        btn.setCursor(Qt.PointingHandCursor)
        btn.setFocusPolicy(Qt.StrongFocus)

        # Fixed width, measured WITH the dirty marker so showing it later
        # cannot reflow the bar. This is the whole reason the marker is safe
        # to toggle while the operator is aiming at a button.
        metrics = btn.fontMetrics()
        width = metrics.horizontalAdvance(title + DIRTY_MARK) + 28
        btn.setFixedWidth(width)
        btn.setFixedHeight(30)
        btn.setStyleSheet(self._button_style())
        btn.clicked.connect(lambda _=False, i=index: self.set_current(i))

        self._flow.addWidget(btn)
        self._buttons.append(btn)
        self._titles.append(title)
        self._dirty.append(False)
        if self._current < 0:
            self.set_current(0)
        return index

    @staticmethod
    def _button_style() -> str:
        return f"""
            QPushButton {{
                background: transparent;
                color: {_C.TEXT_SECONDARY};
                border: none;
                border-bottom: 2px solid transparent;
                padding: 4px 8px;
                font-size: 12px;
            }}
            QPushButton:hover {{ color: {_C.TEXT_PRIMARY}; }}
            QPushButton:checked {{
                color: {_C.TEXT_PRIMARY};
                border-bottom: 2px solid {_C.TEXT_PRIMARY};
                font-weight: bold;
            }}
        """

    # -- state -------------------------------------------------------------- #
    def count(self) -> int:
        return len(self._buttons)

    def current_index(self) -> int:
        return self._current

    def set_current(self, index: int) -> None:
        if not (0 <= index < len(self._buttons)) or index == self._current:
            # Still enforce the checked state: a user click on the already
            # current button would otherwise uncheck it and leave the bar
            # showing no selection at all.
            self._sync_checked()
            return
        self._current = index
        self._sync_checked()
        self.currentChanged.emit(index)

    def _sync_checked(self) -> None:
        for i, btn in enumerate(self._buttons):
            btn.setChecked(i == self._current)

    def set_dirty(self, index: int, dirty: bool) -> None:
        """Mark a section as having unsaved changes."""
        if not (0 <= index < len(self._buttons)):
            return
        if self._dirty[index] == dirty:
            return
        self._dirty[index] = dirty
        title = self._titles[index]
        self._buttons[index].setText(title + DIRTY_MARK if dirty else title)

    def is_dirty(self, index: int) -> bool:
        return bool(0 <= index < len(self._dirty) and self._dirty[index])

    def clear_dirty(self) -> None:
        for i in range(len(self._buttons)):
            self.set_dirty(i, False)

    def title(self, index: int) -> str:
        return self._titles[index] if 0 <= index < len(self._titles) else ""

    # -- keyboard ----------------------------------------------------------- #
    def keyPressEvent(self, event) -> None:   # noqa: N802 - Qt override
        """Left/Right move between sections.

        Implemented rather than inherited: QTabWidget provides this and a
        plain button row does not, so omitting it would lock out anyone
        navigating without a mouse.
        """
        if event.key() in (Qt.Key_Left, Qt.Key_Up):
            self.set_current(max(0, self._current - 1))
            event.accept()
            return
        if event.key() in (Qt.Key_Right, Qt.Key_Down):
            self.set_current(min(self.count() - 1, self._current + 1))
            event.accept()
            return
        super().keyPressEvent(event)


class SubTabPages(QWidget):
    """A :class:`SubTabBar` above a stack of pages, wired together."""

    currentChanged = Signal(int)  # noqa: N815 - matches Qt naming

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self.bar = SubTabBar()
        bar_row = QHBoxLayout()
        bar_row.setContentsMargins(0, 0, 0, 6)
        bar_row.addWidget(self.bar, stretch=1)
        root.addLayout(bar_row)

        self.stack = QStackedWidget()
        root.addWidget(self.stack, stretch=1)

        self.bar.currentChanged.connect(self._on_bar_changed)

    def _on_bar_changed(self, index: int) -> None:
        self.stack.setCurrentIndex(index)
        self.currentChanged.emit(index)

    def add_page(self, title: str, widget: QWidget) -> int:
        index = self.stack.addWidget(widget)
        bar_index = self.bar.add_section(title)
        # The two must stay in lockstep; anything else silently shows the
        # wrong page for a section, which is worse than failing to build.
        assert bar_index == index, "sub-tab bar and stack fell out of step"
        self.stack.setCurrentIndex(self.bar.current_index())
        return index

    def set_current(self, index: int) -> None:
        self.bar.set_current(index)

    def current_index(self) -> int:
        return self.bar.current_index()

    def set_dirty(self, index: int, dirty: bool) -> None:
        self.bar.set_dirty(index, dirty)

    def clear_dirty(self) -> None:
        self.bar.clear_dirty()
