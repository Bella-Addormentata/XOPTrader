"""Collapsible sidebar navigation for XOPTrader main window.

Provides an icon-rail (50 px) / expanded (200 px) sidebar with animated
toggle, exclusive page selection, and CHIA-branded styling.

Compliant with:
    - ISO/IEC 5055  (no unreachable code, bounded resource usage)
    - ISO/IEC 25000 (usability: keyboard-navigable, visible focus states)
"""

from __future__ import annotations

from typing import Final, Optional

from PySide6.QtCore import (
    Property,
    QEasingCurve,
    QPropertyAnimation,
    Qt,
    Signal,
)
from PySide6.QtWidgets import (
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

# ---------------------------------------------------------------------------
# Theme constants -- sourced from the canonical CHIA palette singleton.
# ---------------------------------------------------------------------------
from gui.theme import COLORS as _C

PRIMARY_GREEN: Final[str] = _C.PRIMARY_GREEN
LIGHT_GREEN: Final[str] = _C.LIGHT_GREEN
DARK_BG: Final[str] = _C.DARK_BG
PANEL_BG: Final[str] = _C.PANEL_BG
ELEVATED_BG: Final[str] = _C.ELEVATED_BG
BORDER: Final[str] = _C.BORDER
TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY

# Sidebar geometry
RAIL_WIDTH: Final[int] = 50
EXPANDED_WIDTH: Final[int] = 200
ANIMATION_DURATION_MS: Final[int] = 200

# Navigation entries: (label, unicode icon placeholder)
_NAV_ITEMS: Final[list[tuple[str, str]]] = [
    ("Dashboard", "\u25EB"),   # ◫
    ("Charts", "\u25F0"),      # ◰
    ("Orders", "\u2630"),      # ☰
    ("Order Book", "\u2593"),  # ▓
    ("Settings", "\u2699"),    # ⚙
]


class SidebarButton(QPushButton):
    """Individual navigation button with icon, optional label, and active
    indicator.

    Parameters
    ----------
    icon_char : str
        Single unicode character used as the icon placeholder.
    label_text : str
        Human-readable page label (hidden when sidebar is collapsed).
    parent : QWidget | None
        Parent widget.
    """

    def __init__(
        self,
        icon_char: str,
        label_text: str,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self._icon_char: str = icon_char
        self._label_text: str = label_text
        self._active: bool = False
        self._expanded: bool = True

        self.setCheckable(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        self.setMinimumHeight(42)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

        self._refresh_text()
        self._apply_stylesheet()

    # -- public helpers -----------------------------------------------------

    def set_expanded(self, expanded: bool) -> None:
        """Switch between icon-only and icon+label display."""
        self._expanded = expanded
        self._refresh_text()

    def set_active(self, active: bool) -> None:
        """Mark this button as the active page indicator."""
        self._active = active
        self.setChecked(active)
        self._apply_stylesheet()

    # -- internal -----------------------------------------------------------

    def _refresh_text(self) -> None:
        """Update button text based on expanded/collapsed state."""
        if self._expanded:
            self.setText(f"  {self._icon_char}   {self._label_text}")
        else:
            self.setText(self._icon_char)

    def _apply_stylesheet(self) -> None:
        """Apply CHIA-themed styling with active indicator."""
        active_border = (
            f"border-left: 3px solid {PRIMARY_GREEN};" if self._active else "border-left: 3px solid transparent;"
        )
        active_bg = ELEVATED_BG if self._active else "transparent"
        self.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {active_bg};
                color: {TEXT_PRIMARY};
                {active_border}
                border-top: none;
                border-right: none;
                border-bottom: none;
                text-align: left;
                padding: 8px 6px;
                font-size: 14px;
                border-radius: 0px;
            }}
            QPushButton:hover {{
                background-color: {ELEVATED_BG};
            }}
            QPushButton:focus {{
                outline: 1px solid {PRIMARY_GREEN};
                outline-offset: -1px;
            }}
            """
        )


class Sidebar(QWidget):
    """Collapsible left-hand navigation sidebar.

    Emits *page_changed(int)* when the user selects a different page.
    Supports animated expand/collapse between a 50 px icon rail and
    200 px full panel.

    Parameters
    ----------
    parent : QWidget | None
        Parent widget.
    """

    # Signal emitted when the selected page index changes.
    page_changed = Signal(int)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        self._expanded: bool = True
        self._current_index: int = 0
        self._buttons: list[SidebarButton] = []

        # Sidebar width is animated via the custom Qt property.
        self.setMinimumWidth(RAIL_WIDTH)
        self.setMaximumWidth(EXPANDED_WIDTH)
        self._sidebar_width: int = EXPANDED_WIDTH

        self._build_ui()
        self._apply_stylesheet()

        # Select the first page by default.
        if self._buttons:
            self._buttons[0].set_active(True)

    # -- Qt property for animated width -------------------------------------

    def _get_sidebar_width(self) -> int:
        """Getter for the animated sidebar_width property."""
        return self._sidebar_width

    def _set_sidebar_width(self, value: int) -> None:
        """Setter for the animated sidebar_width property.

        Adjusts both minimum and maximum width so the layout respects
        the animated value.
        """
        self._sidebar_width = value
        self.setFixedWidth(value)

    sidebar_width = Property(int, _get_sidebar_width, _set_sidebar_width)

    # -- UI construction ----------------------------------------------------

    def _build_ui(self) -> None:
        """Construct the sidebar layout: nav buttons + toggle button."""
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 8, 0, 8)
        layout.setSpacing(2)

        # Navigation buttons
        for index, (label, icon) in enumerate(_NAV_ITEMS):
            btn = SidebarButton(icon, label, self)
            btn.clicked.connect(lambda checked, idx=index: self._on_button_clicked(idx))
            self._buttons.append(btn)
            layout.addWidget(btn)

        # Spacer pushes toggle to the bottom
        layout.addStretch(1)

        # Separator line
        separator = QWidget(self)
        separator.setFixedHeight(1)
        separator.setStyleSheet(f"background-color: {BORDER};")
        layout.addWidget(separator)

        # Collapse / Expand toggle
        self._toggle_btn = QPushButton(self)
        self._toggle_btn.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._toggle_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._toggle_btn.setMinimumHeight(36)
        self._toggle_btn.clicked.connect(self.toggle)
        self._update_toggle_label()
        self._toggle_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: transparent;
                color: {TEXT_SECONDARY};
                border: none;
                text-align: center;
                font-size: 13px;
                padding: 6px;
            }}
            QPushButton:hover {{
                color: {TEXT_PRIMARY};
                background-color: {ELEVATED_BG};
            }}
            """
        )
        layout.addWidget(self._toggle_btn)

    def _apply_stylesheet(self) -> None:
        """Apply sidebar container styling."""
        self.setStyleSheet(
            f"""
            Sidebar {{
                background-color: {PANEL_BG};
                border-right: 1px solid {BORDER};
            }}
            """
        )

    # -- public API ---------------------------------------------------------

    def toggle(self) -> None:
        """Animate between collapsed (icon rail) and expanded states."""
        self._expanded = not self._expanded
        target_width = EXPANDED_WIDTH if self._expanded else RAIL_WIDTH

        animation = QPropertyAnimation(self, b"sidebar_width", self)
        animation.setDuration(ANIMATION_DURATION_MS)
        animation.setStartValue(self._sidebar_width)
        animation.setEndValue(target_width)
        animation.setEasingCurve(QEasingCurve.Type.InOutQuad)
        animation.start(QPropertyAnimation.DeletionPolicy.DeleteWhenStopped)

        # Update button labels immediately so they track the animation.
        for btn in self._buttons:
            btn.set_expanded(self._expanded)
        self._update_toggle_label()

    def select_page(self, index: int) -> None:
        """Programmatically select a page by index.

        Parameters
        ----------
        index : int
            Zero-based page index (0 = Dashboard .. 4 = Settings).
        """
        if 0 <= index < len(self._buttons):
            self._on_button_clicked(index)

    @property
    def is_expanded(self) -> bool:
        """Return True when sidebar is in the expanded state."""
        return self._expanded

    # -- internal slots -----------------------------------------------------

    def _on_button_clicked(self, index: int) -> None:
        """Handle navigation button click -- exclusive selection."""
        if index == self._current_index:
            # Re-clicking the active page is a no-op.
            return

        # Deactivate previous, activate new
        if 0 <= self._current_index < len(self._buttons):
            self._buttons[self._current_index].set_active(False)
        self._current_index = index
        self._buttons[index].set_active(True)
        self.page_changed.emit(index)

    def _update_toggle_label(self) -> None:
        """Set the toggle button text based on current state."""
        if self._expanded:
            self._toggle_btn.setText("\u25C0  Collapse")  # ◀ Collapse
        else:
            self._toggle_btn.setText("\u25B6")  # ▶
