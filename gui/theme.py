"""QSS theme system with CHIA blockchain branding.

Provides a dark-mode colour palette, a complete Qt Style Sheet
covering every standard widget class, and helper functions to
apply the theme to any QApplication instance.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from PySide6.QtGui import QFont, QFontDatabase
from PySide6.QtWidgets import QApplication, QCheckBox


# ---------------------------------------------------------------------------
# Colour palette -- official CHIA branding + trading-UI accents
# ---------------------------------------------------------------------------

@dataclass(frozen=True, slots=True)
class ChiaColors:
    """Immutable container for every colour token used by the theme.

    Colour values are CSS hex strings suitable for direct
    interpolation into QSS rules.
    """

    # Brand greens (from Chia Color.ts: Green[500], Green[400])
    PRIMARY_GREEN: str = "#FF9900"  # Amber for Bloomberg feel
    LIGHT_GREEN: str = "#FFB347"

    # Surface / background hierarchy -- Chia Neutral palette (teal-tinted)
    DARK_BG: str = "#000000"       # Pure Black
    PANEL_BG: str = "#000000"      # Pure Black
    ELEVATED_BG: str = "#111111"   # Very Dark Gray

    # Sidebar surfaces
    SIDEBAR_BG: str = "#000000"    # Pure Black
    SIDEBAR_SELECTED: str = "#221100"  # Amber Tint

    # Borders
    BORDER: str = "#333333"        # Dark Gray
    BORDER_LIGHT: str = "#444444"  

    # Typography (from Chia Color.Text.Dark)
    TEXT_PRIMARY: str = "#FF9900"   # Amber
    TEXT_SECONDARY: str = "#CC7700" # Dim Amber
    TEXT_DISABLED: str = "#663300"  

    # Semantic / trading
    PROFIT_GREEN: str = "#00FF00"   # Neon Green
    LOSS_RED: str = "#FF0000"       # Neon Red
    WARNING_YELLOW: str = "#FFFF00" 
    INFO_BLUE: str = "#00FFFF"     

    # Accent palette (from Chia)
    AQUA: str = "#3EC3C1"           # Aqua[400]
    PURPLE: str = "#CD48ED"         # Purple[500]
    ORANGE: str = "#FF9B20"         # Orange[400]


# Module-level singleton so callers can import colours directly.
COLORS = ChiaColors()


# ---------------------------------------------------------------------------
# Font configuration
# ---------------------------------------------------------------------------

# Preferred monospaced font for numeric / data display.
MONO_FONT_FAMILY: str = "Consolas"

# Preferred proportional font for general UI text.
UI_FONT_FAMILY: str = "Consolas"

# Fallback stack when preferred fonts are unavailable.
_MONO_FALLBACK: str = "'Consolas', 'Courier New', monospace"
_UI_FALLBACK: str = "'Consolas', 'Courier New', monospace"

# Base sizes (points).  Adjusted by *font_size_delta* at runtime.
# Increased from 10pt to match Chia GUI's more generous typography.
_BASE_UI_FONT_SIZE: int = 12
_BASE_MONO_FONT_SIZE: int = 12


def _resolve_font(family: str, fallback: str, size_pt: int) -> QFont:
    """Build a QFont, falling back through the stack when *family* is absent.

    Parameters
    ----------
    family:
        First-choice font family name.
    fallback:
        Comma-separated CSS-style fallback list (used only for QSS;
        QFont itself resolves via Qt font matching).
    size_pt:
        Point size.

    Returns
    -------
    QFont configured with the best available family.
    """
    font = QFont(family, size_pt)
    # If Qt substituted because *family* is missing, try each fallback.
    if not QFontDatabase.hasFamily(family):
        for candidate in fallback.replace("'", "").split(","):
            candidate = candidate.strip()
            if QFontDatabase.hasFamily(candidate):
                font.setFamily(candidate)
                break
    return font


# ---------------------------------------------------------------------------
# QSS stylesheet generation
# ---------------------------------------------------------------------------

def get_stylesheet(
    colors: ChiaColors = COLORS,
    font_size_delta: int = 0,
) -> str:
    """Return the complete QSS stylesheet as a single string.

    Parameters
    ----------
    colors:
        Colour palette to interpolate.  Defaults to the CHIA palette.
    font_size_delta:
        Integer offset (positive or negative) added to every
        font-size value for accessibility / user preference.

    Returns
    -------
    A multi-thousand-character QSS string ready for
    ``QApplication.setStyleSheet()``.
    """
    # Shorthand aliases keep the template readable.
    c = colors
    # Effective font sizes.
    fs = max(7, _BASE_UI_FONT_SIZE + font_size_delta)
    mfs = max(7, _BASE_MONO_FONT_SIZE + font_size_delta)
    # Slightly smaller secondary size for captions, hints, etc.
    sfs = max(7, fs - 1)
    # Check/radio indicator box.  Was a hard-coded 20px with a 2px border --
    # a 24px box beside 16px text, half again the height of the label it
    # marks, and unmoved by the operator's font-size setting.  Scale it with
    # the UI font so it stays proportionate at every size.
    cbi = max(11, 13 + font_size_delta)
    # Round UP to half the BORDERED box (box + 1px border each side):
    # rounding down leaves a half-pixel of square on the radio.  Qt
    # clamps an over-large radius to half, so the ceiling is safe.
    cbi_r = (cbi + 3) // 2

    return f"""
/* ===================================================================
   XOPTrader -- CHIA DEX Market-Maker Theme
   Generated by gui.theme.get_stylesheet()
   =================================================================== */

/* ----- Base window & widget backgrounds ----- */
QMainWindow {{
    background-color: {c.DARK_BG};
    color: {c.TEXT_PRIMARY};
    font-family: {_UI_FALLBACK};
    font-size: {fs}pt;
}}

QWidget {{
    background-color: {c.DARK_BG};
    color: {c.TEXT_PRIMARY};
    font-family: {_UI_FALLBACK};
    font-size: {fs}pt;
}}

/* ----- Menu bar ----- */
QMenuBar {{
    background-color: {c.PANEL_BG};
    color: {c.TEXT_PRIMARY};
    border-bottom: 1px solid {c.BORDER};
    padding: 4px 0px;
    font-size: {fs}pt;
    min-height: 32px;
}}

QMenuBar::item {{
    background: transparent;
    padding: 6px 14px;
}}

QMenuBar::item:selected {{
    background-color: {c.PRIMARY_GREEN};
    color: {c.DARK_BG};
    border-radius: 6px;
}}

/* ----- Drop-down menus ----- */
QMenu {{
    background-color: {c.PANEL_BG};
    color: {c.TEXT_PRIMARY};
    border: 1px solid {c.BORDER};
    padding: 4px 0px;
    font-size: {fs}pt;
}}

QMenu::item {{
    padding: 8px 28px 8px 20px;
}}

QMenu::item:selected {{
    background-color: {c.PRIMARY_GREEN};
    color: {c.DARK_BG};
}}

QMenu::separator {{
    height: 1px;
    background: {c.BORDER};
    margin: 4px 8px;
}}

/* ----- Toolbar ----- */
QToolBar {{
    background-color: {c.PANEL_BG};
    border: none;
    spacing: 8px;
    padding: 6px 8px;
    min-height: 44px;
}}

QToolBar::separator {{
    width: 1px;
    background: {c.BORDER};
    margin: 4px 6px;
}}

/* ----- Push buttons ----- */
/* Default (dark) variant */
QPushButton {{
    background-color: {c.ELEVATED_BG};
    color: {c.TEXT_PRIMARY};
    border: 1px solid {c.BORDER};
    border-radius: 8px;
    padding: 8px 20px;
    font-size: {fs}pt;
    min-height: 28px;
}}

QPushButton:hover {{
    background-color: {c.PANEL_BG};
    border-color: {c.TEXT_SECONDARY};
}}

QPushButton:pressed {{
    background-color: {c.DARK_BG};
}}

QPushButton:disabled {{
    color: {c.TEXT_SECONDARY};
    background-color: {c.DARK_BG};
    border-color: {c.BORDER};
}}

/* Primary (green) variant -- assign objectName "primaryButton" */
QPushButton#primaryButton {{
    background-color: {c.PRIMARY_GREEN};
    color: {c.DARK_BG};
    border: 1px solid {c.LIGHT_GREEN};
    font-weight: 600;
}}

QPushButton#primaryButton:hover {{
    background-color: {c.LIGHT_GREEN};
}}

QPushButton#primaryButton:pressed {{
    background-color: {c.PRIMARY_GREEN};
}}

/* Danger (red) variant -- assign objectName "dangerButton" */
QPushButton#dangerButton {{
    background-color: {c.LOSS_RED};
    color: #ffffff;
    border: 1px solid {c.LOSS_RED};
    font-weight: 600;
}}

QPushButton#dangerButton:hover {{
    background-color: #FF6659;
}}

QPushButton#dangerButton:pressed {{
    background-color: #D32F2F;
}}

/* Compact variant for buttons that live INSIDE a table row.

   The base QPushButton rule above sets min-height 28px and 8px/20px padding,
   which is taller than a table row -- and a widget-level setFixedHeight()
   cannot shrink it, because the stylesheet's min-height wins over the size
   hint.  That is why the in-row Cancel button rendered taller than the row it
   sat on.  Opt out with the dynamic property compact=true.

   An attribute selector outranks the plain type selector above, and the
   #dangerButton ID rules outrank both.  Those ID rules set the palette AND
   font-weight: 600, so the weight below is the one property here that a
   danger button does NOT get; padding, min-height, border-radius and
   font-size all apply.  (An earlier version of this comment claimed the ID
   rules set only colours, which is wrong and would mislead the next person
   sizing a control against them.) */
QPushButton[compact="true"] {{
    padding: 1px 8px;
    min-height: 0px;
    border-radius: 4px;
    font-size: {sfs}pt;
    font-weight: 500;
}}

/* ----- Tab widget & tab bar ----- */
QTabWidget::pane {{
    background-color: {c.PANEL_BG};
    border: 1px solid {c.BORDER};
    border-top: none;
}}

QTabBar {{
    background: transparent;
}}

QTabBar::tab {{
    background-color: {c.DARK_BG};
    color: {c.TEXT_SECONDARY};
    border: 1px solid {c.BORDER};
    border-bottom: none;
    padding: 12px 28px;
    margin-right: 2px;
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
    font-size: {fs}pt;
    min-width: 90px;
}}

QTabBar::tab:selected {{
    background-color: {c.PANEL_BG};
    color: {c.TEXT_PRIMARY};
    border-bottom: 2px solid {c.PRIMARY_GREEN};
}}

QTabBar::tab:hover:!selected {{
    background-color: {c.ELEVATED_BG};
    color: {c.TEXT_PRIMARY};
}}

/* ----- Splitter handles ----- */
QSplitter::handle {{
    background-color: {c.BORDER};
}}

QSplitter::handle:horizontal {{
    width: 2px;
}}

QSplitter::handle:vertical {{
    height: 2px;
}}

/* ----- Tree / Table views ----- */
QTreeView, QTableView {{
    background-color: {c.PANEL_BG};
    alternate-background-color: {c.ELEVATED_BG};
    color: {c.TEXT_PRIMARY};
    border: 1px solid {c.BORDER};
    gridline-color: {c.BORDER};
    selection-background-color: {c.PRIMARY_GREEN};
    selection-color: {c.DARK_BG};
    font-family: {_MONO_FALLBACK};
    font-size: {mfs}pt;
}}

QTreeView::item, QTableView::item {{
    /* Vertical padding is subtracted from the CELL RECT before Qt places a
       cell widget, so 10px top+bottom left 9px of a 30px row for controls:
       the in-row Cancel/Remove buttons rendered as 7px slivers.  Text
       rendering is unaffected either way (fixed-height rows centre it), so
       only controls ever saw the difference.  Keep the horizontal inset. */
    padding: 4px 14px;
    min-height: 32px;
}}

QTreeView::item:hover, QTableView::item:hover {{
    background-color: {c.ELEVATED_BG};
}}

QHeaderView {{
    background-color: {c.DARK_BG};
}}

QHeaderView::section {{
    background-color: {c.DARK_BG};
    color: {c.TEXT_SECONDARY};
    border: none;
    border-right: 1px solid {c.BORDER};
    border-bottom: 1px solid {c.BORDER};
    padding: 10px 16px;
    font-weight: 600;
    font-size: {sfs}pt;
    min-height: 32px;
}}

QHeaderView::section:hover {{
    color: {c.TEXT_PRIMARY};
}}

/* ----- Scroll bars (thin, unobtrusive) ----- */
QScrollBar:vertical {{
    background: {c.DARK_BG};
    width: 10px;
    margin: 2px;
}}

QScrollBar::handle:vertical {{
    background: {c.BORDER};
    min-height: 40px;
    border-radius: 5px;
}}

QScrollBar::handle:vertical:hover {{
    background: {c.TEXT_SECONDARY};
}}

QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
    height: 0;
}}

QScrollBar:horizontal {{
    background: {c.DARK_BG};
    height: 10px;
    margin: 2px;
}}

QScrollBar::handle:horizontal {{
    background: {c.BORDER};
    min-width: 40px;
    border-radius: 5px;
}}

QScrollBar::handle:horizontal:hover {{
    background: {c.TEXT_SECONDARY};
}}

QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {{
    width: 0;
}}

/* ----- Input fields ----- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {{
    background-color: {c.ELEVATED_BG};
    color: {c.TEXT_PRIMARY};
    border: 1px solid {c.BORDER};
    border-radius: 8px;
    padding: 10px 14px;
    font-family: {_MONO_FALLBACK};
    font-size: {mfs}pt;
    min-height: 24px;
    selection-background-color: {c.PRIMARY_GREEN};
    selection-color: {c.DARK_BG};
}}

QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {{
    border-color: {c.PRIMARY_GREEN};
}}

QComboBox::drop-down {{
    border: none;
    width: 20px;
}}

QComboBox QAbstractItemView {{
    background-color: {c.PANEL_BG};
    color: {c.TEXT_PRIMARY};
    border: 1px solid {c.BORDER};
    selection-background-color: {c.PRIMARY_GREEN};
    selection-color: {c.DARK_BG};
}}

/* ----- Group box ----- */
QGroupBox {{
    border: 1px solid {c.BORDER};
    border-radius: 12px;
    margin-top: 16px;
    padding: 24px 16px 16px 16px;
    font-size: {fs}pt;
}}

QGroupBox::title {{
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 0 6px;
    color: {c.PRIMARY_GREEN};
    font-weight: 600;
}}

/* ----- Labels ----- */
QLabel {{
    color: {c.TEXT_PRIMARY};
    background: transparent;
}}

/* ----- Status bar ----- */
QStatusBar {{
    background-color: {c.PANEL_BG};
    color: {c.TEXT_SECONDARY};
    border-top: 1px solid {c.BORDER};
    font-size: {sfs}pt;
}}

QStatusBar::item {{
    border: none;
}}

/* ----- Tooltips ----- */
QToolTip {{
    background-color: {c.PANEL_BG};
    color: {c.TEXT_PRIMARY};
    border: 1px solid {c.PRIMARY_GREEN};
    border-radius: 8px;
    padding: 8px 12px;
    font-size: {sfs}pt;
}}

/* ----- Progress bar ----- */
QProgressBar {{
    background-color: {c.ELEVATED_BG};
    border: 1px solid {c.BORDER};
    border-radius: 6px;
    text-align: center;
    color: {c.TEXT_PRIMARY};
    font-size: {sfs}pt;
    min-height: 20px;
}}

QProgressBar::chunk {{
    background-color: {c.PRIMARY_GREEN};
    border-radius: 5px;
}}

/* ----- Check box & radio button ----- */
QCheckBox, QRadioButton {{
    color: {c.TEXT_PRIMARY};
    spacing: 6px;
    font-size: {fs}pt;
}}

QCheckBox::indicator, QRadioButton::indicator {{
    width: {cbi}px;
    height: {cbi}px;
    border: 1px solid {c.BORDER};
    background-color: {c.ELEVATED_BG};
}}

QCheckBox::indicator {{
    border-radius: 4px;
}}

QRadioButton::indicator {{
    border-radius: {cbi_r}px;
}}

QCheckBox::indicator:checked, QRadioButton::indicator:checked {{
    background-color: {c.PRIMARY_GREEN};
    border-color: {c.LIGHT_GREEN};
}}

QCheckBox::indicator:hover, QRadioButton::indicator:hover {{
    border-color: {c.PRIMARY_GREEN};
}}

/* ----- Slider ----- */
QSlider::groove:horizontal {{
    background: {c.ELEVATED_BG};
    height: 4px;
    border-radius: 2px;
}}

QSlider::handle:horizontal {{
    background: {c.PRIMARY_GREEN};
    width: 14px;
    height: 14px;
    margin: -5px 0;
    border-radius: 7px;
}}

QSlider::handle:horizontal:hover {{
    background: {c.LIGHT_GREEN};
}}

QSlider::groove:vertical {{
    background: {c.ELEVATED_BG};
    width: 4px;
    border-radius: 2px;
}}

QSlider::handle:vertical {{
    background: {c.PRIMARY_GREEN};
    width: 14px;
    height: 14px;
    margin: 0 -5px;
    border-radius: 7px;
}}

QSlider::handle:vertical:hover {{
    background: {c.LIGHT_GREEN};
}}
"""


# ---------------------------------------------------------------------------
# Theme application helper
# ---------------------------------------------------------------------------

#: Minimum comfortable pointer target, in px, for a control with no label.
MIN_HIT_TARGET_PX = 24


class HitTargetCheckBox(QCheckBox):
    """An unlabeled checkbox whose WHOLE rect toggles it.

    A checkbox with a label is easy to hit -- the text is part of the button.
    An unlabeled one is exactly as big as its indicator, so making the
    indicator proportionate to the text shrank the pointer target with it:
    21x15, down from 24x24.

    Enlarging the widget is not enough on its own.  QCheckBox does NOT hit
    test against its whole rect; it uses ``SE_CheckBoxClickRect``, which
    tracks the indicator.  Verified with QTest.mouseClick: with only a 24x24
    minimum size, a click at the centre toggled while clicks at the right
    edge and the bottom-right corner did nothing.  So the minimum size sets
    the area and ``hitButton`` makes that area actually respond.
    """

    def __init__(self, size: int = MIN_HIT_TARGET_PX, parent=None) -> None:
        super().__init__(parent)
        self.setMinimumSize(size, size)

    def hitButton(self, pos) -> bool:      # noqa: N802 (Qt naming)
        """Treat the entire widget as the button, label or no label."""
        return self.rect().contains(pos)


def fit_row_height(table) -> None:
    """Size *table*'s rows to the current UI font.

    Qt's default section size is a constant (30px here) that does not move
    when the operator changes the font size, while the text and any control
    inside a row does.  At the +6 delta a compact button needs 31px on Linux,
    more once a cell layout adds margins, so the row -- not the control -- is
    what has to give.

    Deriving a height from font metrics does NOT work: QWidget.font() reports
    the widget's own font, not the size a stylesheet applies, so the metrics
    stay at the base size whatever delta is in force.  Measure a real styled
    control instead.  Rows keep their 30px look at the default size and grow
    only where a large font genuinely needs it -- on Linux at +6 the compact
    button alone is 31px, which is what turned CI red.
    """
    from PySide6.QtWidgets import QPushButton

    # Measure a real compact button under the CURRENT stylesheet rather than
    # guessing.  ResizeToContents would also fit, but it honours the
    # QTableView::item min-height (32px) and padding, inflating every row to
    # ~53px -- chunkier rows to solve a too-tall control, which is backwards.
    # Measure the SAME style the production controls use.  Both in-row
    # buttons are #dangerButton, whose ID rule wins on font-weight (600 vs
    # the compact rule's 500).  Both weights happen to yield identical
    # heights with the current font -- verified across every delta -- but
    # measuring a style the real control does not use leaves the row height
    # depending on that coincidence.
    probe = QPushButton("X")
    probe.setObjectName("dangerButton")
    probe.setProperty("compact", True)
    probe.ensurePolished()
    # The row must hold: the control, the cell layout margins (1px top and
    # bottom), the QTableView::item vertical padding (4px top and bottom --
    # Qt subtracts it from the cell rect BEFORE placing a cell widget), and
    # the grid line.  Measuring only the control is how the buttons ended up
    # rendered at 7px inside rows that looked tall enough.
    needed = probe.sizeHint().height() + 2 + 8 + 1
    probe.deleteLater()
    # Never shrink below the existing 30px look; only grow where a large font
    # actually demands it.
    table.verticalHeader().setDefaultSectionSize(
        max(30, needed)
    )


def apply_theme(
    app: QApplication,
    font_size_delta: int = 0,
    colors: Optional[ChiaColors] = None,
) -> None:
    """Configure *app* with the CHIA dark theme.

    This sets the Fusion base style (consistent cross-platform look),
    applies the full QSS stylesheet, and installs the preferred fonts.

    Parameters
    ----------
    app:
        The running QApplication instance.
    font_size_delta:
        Point-size adjustment for accessibility.  ``+2`` makes all
        text two points larger; ``-1`` makes it one point smaller.
    colors:
        Optional custom colour palette.  Falls back to the default
        CHIA palette when *None*.
    """
    palette = colors if colors is not None else COLORS

    # Use Fusion style as the base -- it renders identically on every
    # platform and responds well to QSS overrides.
    app.setStyle("Fusion")

    # Resolve and apply fonts before the stylesheet so that QSS
    # font-family references match what the system actually provides.
    ui_font = _resolve_font(
        UI_FONT_FAMILY,
        _UI_FALLBACK,
        _BASE_UI_FONT_SIZE + font_size_delta,
    )
    app.setFont(ui_font)

    # Apply the complete stylesheet.
    app.setStyleSheet(get_stylesheet(palette, font_size_delta))
