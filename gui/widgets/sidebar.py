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
    QLabel,
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
SIDEBAR_BG: Final[str] = _C.SIDEBAR_BG
SIDEBAR_SELECTED: Final[str] = _C.SIDEBAR_SELECTED

# Sidebar geometry -- matches Chia GUI drawer width of 72px
RAIL_WIDTH: Final[int] = 72
EXPANDED_WIDTH: Final[int] = 240
ANIMATION_DURATION_MS: Final[int] = 200

# Icon box dimensions (Chia: spacing(6) = 48px)
ICON_BOX_SIZE: Final[int] = 48
ICON_FONT_SIZE: Final[int] = 22

# Navigation entries: (label, unicode icon)
# Using clearer, more recognisable glyphs that render well at 22px
_NAV_ITEMS: Final[list[tuple[str, str]]] = [
    ("Dashboard",  "\U0001F4CA"),   # 📊  bar chart
    ("Charts",     "\U0001F4C8"),   # 📈  chart increasing
    ("Orders",     "\U0001F4CB"),   # 📋  clipboard
    ("Order Book", "\U0001F4D6"),   # 📖  open book
    ("Analysis",   "\U0001F50D"),   # 🔍  magnifying glass
    ("Wallet",     "\U0001F4B0"),   # 💰  money bag
    ("Reports",    "\U0001F4C4"),   # 📄  document (reports)
    ("Settings",   "\u2699\uFE0F"), # ⚙️  gear
]


class SidebarButton(QPushButton):
    """Chia-style navigation button with icon box and label.

    Renders as a vertical icon-in-box above a small label, matching the
    Chia blockchain GUI sidebar pattern (48px icon box, 12px border-radius,
    green highlight border and glow on selection).

    Parameters
    ----------
    icon_char : str
        Unicode character used as the icon.
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
        self.setMinimumHeight(68)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 8, 0, 8)
        self._layout.setSpacing(2)

        self._icon_label = QLabel(self._icon_char)
        self._icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._icon_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._layout.addWidget(self._icon_label)

        self._text_label = QLabel(self._label_text)
        self._text_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._text_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self._layout.addWidget(self._text_label)

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
            self._text_label.show()
        else:
            self._text_label.hide()

    def _apply_stylesheet(self) -> None:
        """Apply Chia-styled icon-box navigation with green glow on active."""
        if self._active:
            border_color = PRIMARY_GREEN
            bg_color = SIDEBAR_SELECTED
            text_color = LIGHT_GREEN
            icon_size = ICON_FONT_SIZE + 1
            font_weight = "bold"
        else:
            border_color = BORDER
            bg_color = "transparent"
            text_color = TEXT_SECONDARY
            icon_size = ICON_FONT_SIZE
            font_weight = "normal"
            
        self._icon_label.setStyleSheet(f"font-size: {icon_size}px; font-weight: {font_weight}; background-color: transparent; border: none;")
        self._text_label.setStyleSheet(f"font-size: 11px; font-weight: normal; background-color: transparent; border: none;")

        self.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {bg_color};
                color: {text_color};
                border: 1px solid {border_color};
                border-radius: 0px;
                margin: 0px;
            }}
            QPushButton:hover {{
                border-color: {PRIMARY_GREEN};
                color: {TEXT_PRIMARY};
                background-color: {ELEVATED_BG};
            }}
            QPushButton:focus {{
                border: 1px solid {PRIMARY_GREEN};
            }}
            """
        )


class Sidebar(QWidget):
    """Collapsible left-hand navigation sidebar.

    Emits *page_changed(int)* when the user selects a different page.
    Supports animated expand/collapse between a 72 px icon rail and
    240 px full panel (matching Chia GUI drawer width).

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

        # Select the first page by default.  The page_changed signal is
        # NOT emitted here because the signal is typically not yet connected
        # at construction time.  Callers should rely on QStackedWidget
        # defaulting to index 0, or call select_page(0) after connecting.
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
        layout.setContentsMargins(0, 12, 0, 12)
        layout.setSpacing(4)

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
        separator.setContentsMargins(12, 0, 12, 0)
        layout.addWidget(separator)

        # Collapse / Expand toggle
        self._toggle_btn = QPushButton(self)
        self._toggle_btn.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._toggle_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._toggle_btn.setMinimumHeight(44)
        self._toggle_btn.clicked.connect(self.toggle)
        self._update_toggle_label()
        self._toggle_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: transparent;
                color: {TEXT_SECONDARY};
                border: 1px solid transparent;
                border-radius: 0px;
                text-align: left;
                font-size: 14px;
                padding: 4px;
                margin: 0px;
            }}
            QPushButton:hover {{
                color: {TEXT_PRIMARY};
                background-color: {ELEVATED_BG};
                border-color: transparent;
            }}
            """
        )
        layout.addWidget(self._toggle_btn)

    def _apply_stylesheet(self) -> None:
        """Apply sidebar container styling with Chia-themed background."""
        self.setStyleSheet(
            f"""
            Sidebar {{
                background-color: {SIDEBAR_BG};
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
            Zero-based page index (0 = Dashboard .. 5 = Settings).
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
