"""Comprehensive settings/configuration panel for XOPTrader.

Provides a tabbed interface for editing every section of the
``config.yaml`` file used by the market-making engine, plus
GUI-only appearance preferences persisted via ``QSettings``.

Layout
------
::

    QVBoxLayout
      +-- Header: "Settings" title + Save / Reset / Apply buttons
      +-- QTabWidget
      |     +-- Connection
      |     +-- Trading Pairs
      |     +-- Strategy
      |     +-- Risk Management
      |     +-- Monitoring
      |     +-- Appearance
      |     +-- Advanced
      +-- Status line: config path + last-saved timestamp

Key behaviours
--------------
- ``load_config(path)`` populates every widget from YAML.
- ``save_config(path)`` serialises back to YAML.
- Dirty tracking: unsaved changes enable *Save* and show ``*`` on
  modified tab titles.
- Validation on save: red borders on invalid fields with tooltip
  explanations.
- ``config_changed(dict)`` emitted on any edit.
- ``config_saved(str)``  emitted after a successful write.

Compliant with:
    - ISO/IEC 5055  (bounded resource usage, no unreachable code)
    - ISO/IEC 25000 (usability: tooltips, keyboard navigation)
    - ISO/IEC 27001 (no secrets logged, password echo mode)
"""

from __future__ import annotations

import copy
import datetime
import logging
import re
from pathlib import Path
from typing import Any, Final, Optional

import yaml
from PySide6.QtCore import QSettings, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QFont,
    QFontDatabase,
    QSyntaxHighlighter,
    QTextCharFormat,
)
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QTabWidget,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Theme colour shorthand
# ---------------------------------------------------------------------------
_C = COLORS

# QSS fragment applied to input fields that fail validation.
_INVALID_BORDER: Final[str] = f"border: 2px solid {_C.LOSS_RED};"
# QSS fragment that clears the error state.
_VALID_BORDER: Final[str] = f"border: 1px solid {_C.BORDER};"

# Default config.yaml file name when none is provided.
_DEFAULT_CONFIG_FILENAME: Final[str] = "config.yaml"


# ===================================================================
# YAML syntax highlighter for the Advanced tab raw editor
# ===================================================================

class _YamlHighlighter(QSyntaxHighlighter):
    """Minimal YAML syntax highlighter for QPlainTextEdit.

    Highlights keys, string values, comments, and numeric literals
    using the CHIA colour palette.
    """

    def __init__(self, parent: Any = None) -> None:
        super().__init__(parent)

        # -- Key format (text before the first colon on a line) --
        self._key_fmt = QTextCharFormat()
        self._key_fmt.setForeground(QColor(_C.PRIMARY_GREEN))
        self._key_fmt.setFontWeight(QFont.Weight.Bold)

        # -- String values (single- or double-quoted) --
        self._str_fmt = QTextCharFormat()
        self._str_fmt.setForeground(QColor("#CE9178"))  # warm orange

        # -- Comments --
        self._comment_fmt = QTextCharFormat()
        self._comment_fmt.setForeground(QColor(_C.TEXT_SECONDARY))
        self._comment_fmt.setFontItalic(True)

        # -- Numeric literals --
        self._num_fmt = QTextCharFormat()
        self._num_fmt.setForeground(QColor("#B5CEA8"))  # soft green

        # -- Boolean / null --
        self._bool_fmt = QTextCharFormat()
        self._bool_fmt.setForeground(QColor(_C.INFO_BLUE))

        # Pre-compiled patterns (order matters -- first match wins).
        self._rules: list[tuple[re.Pattern[str], QTextCharFormat]] = [
            # Full-line and inline comments.
            (re.compile(r"#.*$"), self._comment_fmt),
            # YAML key (word characters before a colon).
            (re.compile(r"^\s*[\w_.-]+(?=\s*:)"), self._key_fmt),
            # Double-quoted strings.
            (re.compile(r'"[^"]*"'), self._str_fmt),
            # Single-quoted strings.
            (re.compile(r"'[^']*'"), self._str_fmt),
            # Booleans and null.
            (re.compile(r"\b(?:true|false|null|yes|no)\b", re.IGNORECASE), self._bool_fmt),
            # Numeric literals (int, float, negative).
            (re.compile(r"(?<![\"'\w])-?\d+(?:\.\d+)?(?![\"'\w])"), self._num_fmt),
        ]

    def highlightBlock(self, text: str) -> None:
        """Apply highlighting rules to a single block of text."""
        for pattern, fmt in self._rules:
            for match in pattern.finditer(text):
                start = match.start()
                length = match.end() - start
                self.setFormat(start, length, fmt)


# ===================================================================
# Add-Pair dialog
# ===================================================================

class _AddPairDialog(QDialog):
    """Modal dialog for adding a new trading pair.

    Collects pair name, base/quote asset IDs, and enabled flag.
    """

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self.setWindowTitle("Add Trading Pair")
        self.setMinimumWidth(460)

        layout = QFormLayout(self)
        layout.setContentsMargins(16, 16, 16, 16)
        layout.setSpacing(10)

        # -- Name --
        self.name_edit = QLineEdit()
        self.name_edit.setPlaceholderText("e.g. XCH/wUSDC")
        layout.addRow("Pair Name:", self.name_edit)

        # -- Base asset ID --
        self.base_edit = QLineEdit()
        self.base_edit.setPlaceholderText("xch")
        self.base_edit.setText("xch")
        layout.addRow("Base Asset ID:", self.base_edit)

        # -- Quote asset ID --
        self.quote_edit = QLineEdit()
        self.quote_edit.setPlaceholderText("64-character hex CAT asset ID")
        layout.addRow("Quote Asset ID:", self.quote_edit)

        # -- Enabled --
        self.enabled_cb = QCheckBox("Enabled")
        self.enabled_cb.setChecked(True)
        layout.addRow("", self.enabled_cb)

        # -- Dialog buttons --
        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok
            | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.accepted.connect(self._validate_and_accept)
        buttons.rejected.connect(self.reject)
        layout.addRow(buttons)

    # -- internal -----------------------------------------------------------

    def _validate_and_accept(self) -> None:
        """Accept only when required fields are populated."""
        if not self.name_edit.text().strip():
            QMessageBox.warning(self, "Validation", "Pair name is required.")
            return
        if not self.quote_edit.text().strip():
            QMessageBox.warning(self, "Validation", "Quote asset ID is required.")
            return
        self.accept()

    # -- public API ---------------------------------------------------------

    def pair_data(self) -> dict[str, Any]:
        """Return the entered pair as a config-compatible dict."""
        return {
            "name": self.name_edit.text().strip(),
            "base_asset_id": self.base_edit.text().strip() or "xch",
            "quote_asset_id": self.quote_edit.text().strip(),
            "enabled": self.enabled_cb.isChecked(),
        }


# ===================================================================
# Main SettingsWidget
# ===================================================================

class SettingsWidget(QWidget):
    """Full-featured settings panel with categorised tabs.

    Parameters
    ----------
    parent : QWidget | None
        Parent widget.

    Signals
    -------
    config_changed(dict)
        Emitted whenever the user modifies any field.  The payload
        is the full configuration dict *before* writing to disk.
    config_saved(str)
        Emitted after ``save_config()`` succeeds.  Payload is the
        absolute path that was written.
    """

    # -- Qt signals ---------------------------------------------------------
    config_changed = Signal(dict)
    config_saved = Signal(str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        # Internal state for dirty-tracking and reset.
        self._config_path: Optional[str] = None
        self._last_saved_time: Optional[str] = None
        self._clean_snapshot: dict[str, Any] = {}
        self._dirty: bool = False
        self._tab_dirty: dict[int, bool] = {}

        # Mapping of tab index -> base (clean) title for dirty markers.
        self._tab_titles: dict[int, str] = {}

        # Registry of all input widgets keyed by config path for
        # validation and bulk operations.
        self._field_widgets: dict[str, QWidget] = {}

        self._build_ui()

    # ===================================================================
    # UI construction
    # ===================================================================

    def _build_ui(self) -> None:
        """Assemble the top-level layout: header, tabs, status bar."""
        root = QVBoxLayout(self)
        root.setContentsMargins(12, 10, 12, 8)
        root.setSpacing(8)

        # -- Header row: title + action buttons --
        header = QHBoxLayout()
        title = QLabel("Settings")
        title.setStyleSheet(
            f"font-size: 18px; font-weight: bold; color: {_C.TEXT_PRIMARY};"
        )
        header.addWidget(title)
        header.addStretch(1)

        self._save_btn = QPushButton("Save")
        self._save_btn.setObjectName("primaryButton")
        self._save_btn.setEnabled(False)
        self._save_btn.setToolTip("Write current settings to config.yaml")
        self._save_btn.clicked.connect(self._on_save_clicked)

        self._reset_btn = QPushButton("Reset")
        self._reset_btn.setToolTip("Discard unsaved changes")
        self._reset_btn.setEnabled(False)
        self._reset_btn.clicked.connect(self._on_reset_clicked)

        self._apply_btn = QPushButton("Apply")
        self._apply_btn.setToolTip("Save and signal the engine to reload")
        self._apply_btn.setEnabled(False)
        self._apply_btn.clicked.connect(self._on_apply_clicked)

        for btn in (self._save_btn, self._reset_btn, self._apply_btn):
            btn.setFixedHeight(30)
            btn.setMinimumWidth(70)
            header.addWidget(btn)

        root.addLayout(header)

        # -- Tab widget --
        self._tabs = QTabWidget()
        self._tabs.setDocumentMode(True)

        # Build each tab and register its title.
        tab_builders: list[tuple[str, callable]] = [
            ("Connection", self._build_connection_tab),
            ("Trading Pairs", self._build_pairs_tab),
            ("Strategy", self._build_strategy_tab),
            ("Risk Management", self._build_risk_tab),
            ("Monitoring", self._build_monitoring_tab),
            ("Appearance", self._build_appearance_tab),
            ("Advanced", self._build_advanced_tab),
        ]
        for idx, (title_text, builder) in enumerate(tab_builders):
            widget = builder()
            self._tabs.addTab(widget, title_text)
            self._tab_titles[idx] = title_text
            self._tab_dirty[idx] = False

        root.addWidget(self._tabs, stretch=1)

        # -- Status bar --
        status_row = QHBoxLayout()
        self._config_path_label = QLabel("Config: (none loaded)")
        self._config_path_label.setStyleSheet(
            f"color: {_C.TEXT_SECONDARY}; font-size: 9pt;"
        )
        self._last_saved_label = QLabel("")
        self._last_saved_label.setStyleSheet(
            f"color: {_C.TEXT_SECONDARY}; font-size: 9pt;"
        )
        status_row.addWidget(self._config_path_label)
        status_row.addStretch(1)
        status_row.addWidget(self._last_saved_label)
        root.addLayout(status_row)

    # -------------------------------------------------------------------
    # Connection tab
    # -------------------------------------------------------------------

    def _build_connection_tab(self) -> QWidget:
        """Build the Connection tab: Chia node, wallet, and Dexie API."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # -- Chia Full Node --
        fn_group = QGroupBox("Chia Full Node")
        fn_form = QFormLayout(fn_group)
        fn_form.setSpacing(8)

        self._fn_host = QLineEdit()
        self._fn_host.setPlaceholderText("localhost")
        self._fn_host.setToolTip("Hostname or IP of the Chia full-node RPC")
        fn_form.addRow("Host:", self._fn_host)

        self._fn_port = QSpinBox()
        self._fn_port.setRange(1, 65535)
        self._fn_port.setValue(8555)
        self._fn_port.setToolTip("TCP port of the Chia full-node RPC (default 8555)")
        fn_form.addRow("Port:", self._fn_port)

        self._fn_cert = self._make_path_row(
            fn_form, "SSL Cert:", "ssl_cert_path",
            "Path to full-node SSL certificate (.crt)",
            "Certificate Files (*.crt *.pem);;All Files (*)",
        )
        self._fn_key = self._make_path_row(
            fn_form, "SSL Key:", "ssl_key_path",
            "Path to full-node SSL private key (.key)",
            "Key Files (*.key *.pem);;All Files (*)",
        )

        self._fn_test_btn = QPushButton("Test Connection")
        self._fn_test_btn.setToolTip("Attempt to reach the full-node RPC")
        self._fn_test_btn.clicked.connect(
            lambda: self._test_connection("full_node")
        )
        self._fn_test_result = QLabel("")
        test_row = QHBoxLayout()
        test_row.addWidget(self._fn_test_btn)
        test_row.addWidget(self._fn_test_result)
        test_row.addStretch(1)
        fn_form.addRow("", test_row)

        layout.addWidget(fn_group)

        # -- Wallet --
        wl_group = QGroupBox("Wallet")
        wl_form = QFormLayout(wl_group)
        wl_form.setSpacing(8)

        self._wl_host = QLineEdit()
        self._wl_host.setPlaceholderText("localhost")
        self._wl_host.setToolTip("Hostname or IP of the Chia wallet RPC")
        wl_form.addRow("Host:", self._wl_host)

        self._wl_port = QSpinBox()
        self._wl_port.setRange(1, 65535)
        self._wl_port.setValue(9256)
        self._wl_port.setToolTip("TCP port of the Chia wallet RPC (default 9256)")
        wl_form.addRow("Port:", self._wl_port)

        self._wl_cert = self._make_path_row(
            wl_form, "SSL Cert:", "wallet_cert_path",
            "Path to wallet SSL certificate (.crt)",
            "Certificate Files (*.crt *.pem);;All Files (*)",
        )
        self._wl_key = self._make_path_row(
            wl_form, "SSL Key:", "wallet_key_path",
            "Path to wallet SSL private key (.key)",
            "Key Files (*.key *.pem);;All Files (*)",
        )

        self._wl_fingerprint = QSpinBox()
        self._wl_fingerprint.setRange(0, 2_147_483_647)
        self._wl_fingerprint.setValue(0)
        self._wl_fingerprint.setToolTip("Wallet fingerprint (integer)")
        wl_form.addRow("Fingerprint:", self._wl_fingerprint)

        self._wl_test_btn = QPushButton("Test Connection")
        self._wl_test_btn.setToolTip("Attempt to reach the wallet RPC")
        self._wl_test_btn.clicked.connect(
            lambda: self._test_connection("wallet")
        )
        self._wl_test_result = QLabel("")
        wl_test_row = QHBoxLayout()
        wl_test_row.addWidget(self._wl_test_btn)
        wl_test_row.addWidget(self._wl_test_result)
        wl_test_row.addStretch(1)
        wl_form.addRow("", wl_test_row)

        layout.addWidget(wl_group)

        # -- Dexie --
        dx_group = QGroupBox("Dexie API")
        dx_form = QFormLayout(dx_group)
        dx_form.setSpacing(8)

        self._dx_api_base = QLineEdit()
        self._dx_api_base.setPlaceholderText("https://api.dexie.space/v1")
        self._dx_api_base.setToolTip("Base URL of the Dexie REST API")
        dx_form.addRow("API Base URL:", self._dx_api_base)

        self._dx_rate_limit = QSpinBox()
        self._dx_rate_limit.setRange(1, 1000)
        self._dx_rate_limit.setValue(50)
        self._dx_rate_limit.setToolTip("Maximum API requests per 10-second window")
        dx_form.addRow("Rate Limit (per 10s):", self._dx_rate_limit)

        layout.addWidget(dx_group)
        layout.addStretch(1)

        # Wire change signals for dirty tracking (tab index 0).
        for widget in (
            self._fn_host, self._fn_cert, self._fn_key,
            self._wl_host, self._wl_cert, self._wl_key,
            self._dx_api_base,
        ):
            widget.textChanged.connect(lambda _t, ti=0: self._mark_dirty(ti))

        for widget in (
            self._fn_port, self._wl_port, self._wl_fingerprint,
            self._dx_rate_limit,
        ):
            widget.valueChanged.connect(lambda _v, ti=0: self._mark_dirty(ti))

        return page

    # -------------------------------------------------------------------
    # Trading Pairs tab
    # -------------------------------------------------------------------

    def _build_pairs_tab(self) -> QWidget:
        """Build the Trading Pairs tab with an editable table."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(8)

        # -- Toolbar --
        toolbar = QHBoxLayout()
        add_btn = QPushButton("+ Add Pair")
        add_btn.setObjectName("primaryButton")
        add_btn.setToolTip("Add a new trading pair")
        add_btn.clicked.connect(self._on_add_pair)
        toolbar.addWidget(add_btn)
        toolbar.addStretch(1)
        layout.addLayout(toolbar)

        # -- Pairs table --
        self._pairs_table = QTableWidget(0, 5)
        self._pairs_table.setHorizontalHeaderLabels(
            ["Enabled", "Name", "Base Asset", "Quote Asset", "Actions"]
        )
        header = self._pairs_table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(4, QHeaderView.ResizeMode.ResizeToContents)
        self._pairs_table.setAlternatingRowColors(True)
        self._pairs_table.setSelectionBehavior(
            QTableWidget.SelectionBehavior.SelectRows
        )
        self._pairs_table.verticalHeader().setVisible(False)
        layout.addWidget(self._pairs_table, stretch=1)

        return page

    # -------------------------------------------------------------------
    # Strategy tab
    # -------------------------------------------------------------------

    def _build_strategy_tab(self) -> QWidget:
        """Build the Strategy tab: AS/GLFT params, spread, tiers."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # -- Avellaneda-Stoikov / GLFT section --
        as_group = QGroupBox("Avellaneda-Stoikov / GLFT")
        as_form = QFormLayout(as_group)
        as_form.setSpacing(8)

        self._gamma = QDoubleSpinBox()
        self._gamma.setRange(0.0, 10.0)
        self._gamma.setSingleStep(0.001)
        self._gamma.setDecimals(4)
        self._gamma.setToolTip(
            "Risk aversion coefficient (\u03b3).  Higher values "
            "produce wider spreads and lower inventory risk."
        )
        as_form.addRow("\u03b3  Gamma:", self._gamma)

        self._kappa = QDoubleSpinBox()
        self._kappa.setRange(0.1, 10.0)
        self._kappa.setSingleStep(0.1)
        self._kappa.setDecimals(2)
        self._kappa.setToolTip(
            "Fill intensity decay (\u03ba).  Controls how rapidly "
            "fill probability drops as the spread widens."
        )
        as_form.addRow("\u03ba  Kappa:", self._kappa)

        self._phi = QDoubleSpinBox()
        self._phi.setRange(0.0, 2.0)
        self._phi.setSingleStep(0.1)
        self._phi.setDecimals(2)
        self._phi.setToolTip(
            "Inventory skew strength (\u03c6) from GLFT extension.  "
            "0 = no skew; higher = more aggressive rebalancing."
        )
        as_form.addRow("\u03c6  Phi:", self._phi)

        self._q_max = QSpinBox()
        self._q_max.setRange(1, 100_000)
        self._q_max.setToolTip(
            "Maximum inventory in base units.  The strategy scales "
            "quotes relative to this ceiling."
        )
        as_form.addRow("q_max:", self._q_max)

        layout.addWidget(as_group)

        # -- Spread & Offers section --
        so_group = QGroupBox("Spread && Offers")
        so_form = QFormLayout(so_group)
        so_form.setSpacing(8)

        self._min_profit_bps = QSpinBox()
        self._min_profit_bps.setRange(1, 1000)
        self._min_profit_bps.setSuffix(" bps")
        self._min_profit_bps.setToolTip(
            "Minimum profit margin in basis points.  The engine "
            "will never post an ask below cost_basis + this margin."
        )
        so_form.addRow("Min Profit Margin:", self._min_profit_bps)

        self._offer_ttl = QSpinBox()
        self._offer_ttl.setRange(1, 1000)
        self._offer_ttl.setSuffix(" blocks")
        self._offer_ttl.setToolTip(
            "Cancel stale offers after N blocks (~52 seconds each)."
        )
        so_form.addRow("Offer TTL:", self._offer_ttl)

        self._num_tiers = QSpinBox()
        self._num_tiers.setRange(1, 10)
        self._num_tiers.setToolTip("Number of tiers in the offer ladder.")
        self._num_tiers.valueChanged.connect(self._on_num_tiers_changed)
        so_form.addRow("Num Tiers:", self._num_tiers)

        layout.addWidget(so_group)

        # -- Tier Configuration section --
        tier_group = QGroupBox("Tier Configuration")
        tier_layout = QVBoxLayout(tier_group)
        tier_layout.setSpacing(6)

        self._tier_table = QTableWidget(0, 3)
        self._tier_table.setHorizontalHeaderLabels(
            ["Tier #", "Spread (bps)", "Size (%)"]
        )
        th = self._tier_table.horizontalHeader()
        th.setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        th.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        th.setSectionResizeMode(2, QHeaderView.ResizeMode.Stretch)
        self._tier_table.verticalHeader().setVisible(False)
        self._tier_table.setAlternatingRowColors(True)
        self._tier_table.cellChanged.connect(
            lambda _r, _c: self._mark_dirty(2)
        )
        tier_layout.addWidget(self._tier_table)

        self._tier_sum_label = QLabel("Size total: 0.00  (must equal 1.00)")
        self._tier_sum_label.setStyleSheet(
            f"color: {_C.TEXT_SECONDARY}; font-size: 9pt;"
        )
        tier_layout.addWidget(self._tier_sum_label)

        layout.addWidget(tier_group)
        layout.addStretch(1)

        # Wire dirty tracking (tab index 2).
        for widget in (self._gamma, self._kappa, self._phi):
            widget.valueChanged.connect(lambda _v, ti=2: self._mark_dirty(ti))
        for widget in (
            self._q_max, self._min_profit_bps, self._offer_ttl,
            self._num_tiers,
        ):
            widget.valueChanged.connect(lambda _v, ti=2: self._mark_dirty(ti))

        return page

    # -------------------------------------------------------------------
    # Risk Management tab
    # -------------------------------------------------------------------

    def _build_risk_tab(self) -> QWidget:
        """Build the Risk Management tab."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # -- Inventory Limits --
        inv_group = QGroupBox("Inventory Limits")
        inv_layout = QVBoxLayout(inv_group)
        inv_form = QFormLayout()
        inv_form.setSpacing(8)

        self._soft_limit = QDoubleSpinBox()
        self._soft_limit.setRange(0.0, 1.0)
        self._soft_limit.setSingleStep(0.05)
        self._soft_limit.setDecimals(2)
        self._soft_limit.setToolTip(
            "Soft inventory limit (fraction of q_max).  Beyond this "
            "the strategy begins aggressive quote skewing."
        )
        inv_form.addRow("Soft Limit (%):", self._soft_limit)

        # Visual progress bar for soft limit.
        self._soft_limit_bar = QProgressBar()
        self._soft_limit_bar.setRange(0, 100)
        self._soft_limit_bar.setFixedHeight(14)
        self._soft_limit_bar.setTextVisible(True)
        self._soft_limit.valueChanged.connect(self._update_soft_limit_bar)
        inv_form.addRow("", self._soft_limit_bar)

        self._hard_limit = QDoubleSpinBox()
        self._hard_limit.setRange(0.0, 1.0)
        self._hard_limit.setSingleStep(0.05)
        self._hard_limit.setDecimals(2)
        self._hard_limit.setToolTip(
            "Hard inventory limit.  Beyond this the strategy pulls "
            "quotes entirely on the overweight side.  Must exceed soft limit."
        )
        inv_form.addRow("Hard Limit (%):", self._hard_limit)

        self._single_cat_cap = QDoubleSpinBox()
        self._single_cat_cap.setRange(0.0, 1.0)
        self._single_cat_cap.setSingleStep(0.01)
        self._single_cat_cap.setDecimals(2)
        self._single_cat_cap.setToolTip(
            "Maximum portfolio fraction allocated to any single CAT token."
        )
        inv_form.addRow("Single CAT Cap (%):", self._single_cat_cap)

        inv_layout.addLayout(inv_form)
        layout.addWidget(inv_group)

        # -- Position Sizing --
        ps_group = QGroupBox("Position Sizing")
        ps_form = QFormLayout(ps_group)
        ps_form.setSpacing(8)

        self._kelly_fraction = QDoubleSpinBox()
        self._kelly_fraction.setRange(0.0, 1.0)
        self._kelly_fraction.setSingleStep(0.05)
        self._kelly_fraction.setDecimals(2)
        self._kelly_fraction.setToolTip(
            "Fraction of full Kelly criterion to use.  "
            "0.5 = Half-Kelly (recommended)."
        )
        ps_form.addRow("Kelly Fraction:", self._kelly_fraction)

        self._max_capital_per_pair = QDoubleSpinBox()
        self._max_capital_per_pair.setRange(0.0, 1.0)
        self._max_capital_per_pair.setSingleStep(0.05)
        self._max_capital_per_pair.setDecimals(2)
        self._max_capital_per_pair.setToolTip(
            "Maximum fraction of total capital deployed to a single pair."
        )
        ps_form.addRow("Max Capital/Pair (%):", self._max_capital_per_pair)

        layout.addWidget(ps_group)
        layout.addStretch(1)

        # Wire dirty tracking (tab index 3).
        for widget in (
            self._soft_limit, self._hard_limit,
            self._single_cat_cap, self._kelly_fraction,
            self._max_capital_per_pair,
        ):
            widget.valueChanged.connect(lambda _v, ti=3: self._mark_dirty(ti))

        return page

    # -------------------------------------------------------------------
    # Monitoring tab
    # -------------------------------------------------------------------

    def _build_monitoring_tab(self) -> QWidget:
        """Build the Monitoring tab: Prometheus and Telegram alerts."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # -- Prometheus --
        prom_group = QGroupBox("Prometheus")
        prom_form = QFormLayout(prom_group)
        prom_form.setSpacing(8)

        self._prom_port = QSpinBox()
        self._prom_port.setRange(1, 65535)
        self._prom_port.setValue(9090)
        self._prom_port.setToolTip("TCP port to expose Prometheus /metrics endpoint")
        prom_form.addRow("Metrics Port:", self._prom_port)

        layout.addWidget(prom_group)

        # -- Telegram --
        tg_group = QGroupBox("Telegram Alerts")
        tg_form = QFormLayout(tg_group)
        tg_form.setSpacing(8)

        # Bot token with show/hide toggle.
        token_row = QHBoxLayout()
        self._tg_token = QLineEdit()
        self._tg_token.setEchoMode(QLineEdit.EchoMode.Password)
        self._tg_token.setPlaceholderText("bot123456:ABC-DEF...")
        self._tg_token.setToolTip("Telegram Bot API token (sensitive)")
        token_row.addWidget(self._tg_token)

        self._tg_token_toggle = QPushButton("Show")
        self._tg_token_toggle.setFixedWidth(56)
        self._tg_token_toggle.setToolTip("Toggle token visibility")
        self._tg_token_toggle.clicked.connect(self._toggle_token_visibility)
        token_row.addWidget(self._tg_token_toggle)
        tg_form.addRow("Bot Token:", token_row)

        self._tg_chat_id = QLineEdit()
        self._tg_chat_id.setPlaceholderText("-100123456789")
        self._tg_chat_id.setToolTip("Telegram chat/channel ID for alerts")
        tg_form.addRow("Chat ID:", self._tg_chat_id)

        self._tg_test_btn = QPushButton("Send Test Alert")
        self._tg_test_btn.setToolTip(
            "Send a test message via Telegram to verify configuration"
        )
        self._tg_test_btn.clicked.connect(self._on_test_telegram)
        tg_form.addRow("", self._tg_test_btn)

        layout.addWidget(tg_group)
        layout.addStretch(1)

        # Wire dirty tracking (tab index 4).
        self._prom_port.valueChanged.connect(
            lambda _v, ti=4: self._mark_dirty(ti)
        )
        for widget in (self._tg_token, self._tg_chat_id):
            widget.textChanged.connect(lambda _t, ti=4: self._mark_dirty(ti))

        return page

    # -------------------------------------------------------------------
    # Appearance tab (QSettings-persisted, GUI-only)
    # -------------------------------------------------------------------

    def _build_appearance_tab(self) -> QWidget:
        """Build the Appearance tab for GUI-only preferences."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        grp = QGroupBox("Display Preferences")
        form = QFormLayout(grp)
        form.setSpacing(8)

        # -- Theme (currently only one) --
        self._theme_combo = QComboBox()
        self._theme_combo.addItem("CHIA Dark")
        self._theme_combo.setToolTip(
            "Visual theme.  Additional themes coming in future releases."
        )
        form.addRow("Theme:", self._theme_combo)

        # -- Font size --
        self._font_size_spin = QSpinBox()
        self._font_size_spin.setRange(8, 24)
        self._font_size_spin.setValue(12)
        self._font_size_spin.setSuffix(" pt")
        self._font_size_spin.setToolTip("Base font size for the GUI (points)")
        form.addRow("Font Size:", self._font_size_spin)

        # -- Chart update interval --
        self._chart_interval = QSpinBox()
        self._chart_interval.setRange(1, 30)
        self._chart_interval.setValue(5)
        self._chart_interval.setSuffix(" s")
        self._chart_interval.setToolTip(
            "How often chart data refreshes (seconds)"
        )
        form.addRow("Chart Update:", self._chart_interval)

        # -- Dashboard refresh interval --
        self._dash_interval = QSpinBox()
        self._dash_interval.setRange(1, 60)
        self._dash_interval.setValue(10)
        self._dash_interval.setSuffix(" s")
        self._dash_interval.setToolTip(
            "How often the dashboard summary refreshes (seconds)"
        )
        form.addRow("Dashboard Refresh:", self._dash_interval)

        # -- Show grid lines --
        self._show_grid = QCheckBox("Show grid lines on charts")
        self._show_grid.setChecked(True)
        form.addRow("", self._show_grid)

        # -- Monospace font picker --
        self._mono_font_combo = QComboBox()
        self._mono_font_combo.setToolTip(
            "Monospaced font used for numeric / data display"
        )
        self._populate_monospace_fonts()
        form.addRow("Mono Font:", self._mono_font_combo)

        layout.addWidget(grp)
        layout.addStretch(1)

        # Load saved appearance preferences from QSettings.
        self._load_appearance_settings()

        # Wire dirty tracking (tab index 5).
        self._theme_combo.currentIndexChanged.connect(
            lambda _i, ti=5: self._mark_dirty(ti)
        )
        self._font_size_spin.valueChanged.connect(
            lambda _v, ti=5: self._mark_dirty(ti)
        )
        self._chart_interval.valueChanged.connect(
            lambda _v, ti=5: self._mark_dirty(ti)
        )
        self._dash_interval.valueChanged.connect(
            lambda _v, ti=5: self._mark_dirty(ti)
        )
        self._show_grid.stateChanged.connect(
            lambda _s, ti=5: self._mark_dirty(ti)
        )
        self._mono_font_combo.currentIndexChanged.connect(
            lambda _i, ti=5: self._mark_dirty(ti)
        )

        return page

    # -------------------------------------------------------------------
    # Advanced tab
    # -------------------------------------------------------------------

    def _build_advanced_tab(self) -> QWidget:
        """Build the Advanced tab: volatility, DB path, raw YAML editor."""
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        # -- Volatility section --
        vol_group = QGroupBox("Volatility (Yang-Zhang)")
        vol_form = QFormLayout(vol_group)
        vol_form.setSpacing(8)

        self._lookback_blocks = QSpinBox()
        self._lookback_blocks.setRange(10, 10_000)
        self._lookback_blocks.setToolTip(
            "Number of blocks to look back for volatility estimation "
            "(~52 s each; 200 ~ 2.9 hours)"
        )
        vol_form.addRow("Lookback Blocks:", self._lookback_blocks)

        self._yz_alpha = QDoubleSpinBox()
        self._yz_alpha.setRange(0.0, 1.0)
        self._yz_alpha.setSingleStep(0.01)
        self._yz_alpha.setDecimals(2)
        self._yz_alpha.setToolTip(
            "Yang-Zhang optimal weight (\u03b1).  Default 0.34."
        )
        vol_form.addRow("YZ Alpha:", self._yz_alpha)

        layout.addWidget(vol_group)

        # -- Database path --
        db_group = QGroupBox("Database")
        db_form = QFormLayout(db_group)
        db_form.setSpacing(8)

        self._db_path = self._make_path_row(
            db_form, "DB Path:", "database_path",
            "Path to the SQLite database file",
            "SQLite Files (*.db *.sqlite *.sqlite3);;All Files (*)",
        )
        layout.addWidget(db_group)

        # -- Raw YAML editor --
        yaml_group = QGroupBox("Raw YAML Editor")
        yaml_layout = QVBoxLayout(yaml_group)
        yaml_layout.setSpacing(6)

        self._yaml_editor = QPlainTextEdit()
        self._yaml_editor.setPlaceholderText(
            "# Paste or edit raw YAML configuration here..."
        )
        # Apply a monospaced font for the editor.
        editor_font = QFont("JetBrains Mono", 10)
        editor_font.setStyleHint(QFont.StyleHint.Monospace)
        self._yaml_editor.setFont(editor_font)
        self._yaml_editor.setTabStopDistance(28.0)
        self._yaml_editor.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        # Attach syntax highlighter.
        self._yaml_highlighter = _YamlHighlighter(
            self._yaml_editor.document()
        )
        yaml_layout.addWidget(self._yaml_editor, stretch=1)

        # Validate button and status.
        validate_row = QHBoxLayout()
        self._yaml_validate_btn = QPushButton("Validate YAML")
        self._yaml_validate_btn.setToolTip(
            "Parse the editor content and report any YAML syntax errors"
        )
        self._yaml_validate_btn.clicked.connect(self._on_validate_yaml)
        validate_row.addWidget(self._yaml_validate_btn)

        self._yaml_load_btn = QPushButton("Load from Editor")
        self._yaml_load_btn.setToolTip(
            "Replace all field values with the YAML in the editor"
        )
        self._yaml_load_btn.clicked.connect(self._on_load_from_editor)
        validate_row.addWidget(self._yaml_load_btn)

        self._yaml_status_label = QLabel("")
        self._yaml_status_label.setStyleSheet(f"font-size: 9pt;")
        validate_row.addWidget(self._yaml_status_label)
        validate_row.addStretch(1)
        yaml_layout.addLayout(validate_row)

        layout.addWidget(yaml_group, stretch=1)

        # Wire dirty tracking (tab index 6).
        self._lookback_blocks.valueChanged.connect(
            lambda _v, ti=6: self._mark_dirty(ti)
        )
        self._yz_alpha.valueChanged.connect(
            lambda _v, ti=6: self._mark_dirty(ti)
        )
        self._db_path.textChanged.connect(
            lambda _t, ti=6: self._mark_dirty(ti)
        )
        self._yaml_editor.textChanged.connect(
            lambda ti=6: self._mark_dirty(ti)
        )

        return page

    # ===================================================================
    # Helper: file-path row with Browse button
    # ===================================================================

    def _make_path_row(
        self,
        form: QFormLayout,
        label: str,
        key: str,
        tooltip: str,
        file_filter: str,
    ) -> QLineEdit:
        """Create a QLineEdit + Browse button row and add it to *form*.

        Parameters
        ----------
        form : QFormLayout
            The parent form layout to add the row to.
        label : str
            Left-side label text.
        key : str
            Internal registry key for dirty tracking.
        tooltip : str
            Tooltip shown on the line edit.
        file_filter : str
            Filter string passed to QFileDialog.

        Returns
        -------
        QLineEdit
            The line-edit widget so the caller can read/write its text.
        """
        row = QHBoxLayout()
        edit = QLineEdit()
        edit.setToolTip(tooltip)
        row.addWidget(edit, stretch=1)

        browse = QPushButton("Browse...")
        browse.setFixedWidth(80)
        browse.setToolTip(f"Open file browser for {label.rstrip(':').strip()}")

        def _browse() -> None:
            """Open a file dialog and write the result to the edit."""
            path, _ = QFileDialog.getOpenFileName(
                self, f"Select {label.rstrip(':').strip()}", "", file_filter,
            )
            if path:
                edit.setText(path)

        browse.clicked.connect(_browse)
        row.addWidget(browse)

        form.addRow(label, row)
        self._field_widgets[key] = edit
        return edit

    # ===================================================================
    # Monospace font enumeration
    # ===================================================================

    def _populate_monospace_fonts(self) -> None:
        """Detect available monospaced fonts and populate the combo box."""
        preferred = [
            "JetBrains Mono", "Fira Code", "Cascadia Code",
            "Source Code Pro", "Consolas", "Courier New",
        ]
        available: list[str] = []
        all_families = QFontDatabase.families()
        for name in preferred:
            if name in all_families:
                available.append(name)
        # Also scan system fonts for anything with "Mono" in the name.
        for family in sorted(all_families):
            if QFontDatabase.isFixedPitch(family) and family not in available:
                available.append(family)
        if not available:
            available.append("monospace")
        self._mono_font_combo.addItems(available)

    # ===================================================================
    # QSettings-based appearance load / save
    # ===================================================================

    def _load_appearance_settings(self) -> None:
        """Read GUI-only preferences from QSettings."""
        settings = QSettings("XOP", "XOPTrader")
        settings.beginGroup("appearance")
        self._font_size_spin.setValue(
            int(settings.value("font_size", 12))
        )
        self._chart_interval.setValue(
            int(settings.value("chart_interval", 5))
        )
        self._dash_interval.setValue(
            int(settings.value("dash_interval", 10))
        )
        self._show_grid.setChecked(
            settings.value("show_grid", True) in (True, "true")
        )
        saved_mono = settings.value("mono_font", "")
        if saved_mono:
            idx = self._mono_font_combo.findText(str(saved_mono))
            if idx >= 0:
                self._mono_font_combo.setCurrentIndex(idx)
        settings.endGroup()

    def _save_appearance_settings(self) -> None:
        """Persist GUI-only preferences to QSettings."""
        settings = QSettings("XOP", "XOPTrader")
        settings.beginGroup("appearance")
        settings.setValue("font_size", self._font_size_spin.value())
        settings.setValue("chart_interval", self._chart_interval.value())
        settings.setValue("dash_interval", self._dash_interval.value())
        settings.setValue("show_grid", self._show_grid.isChecked())
        settings.setValue(
            "mono_font", self._mono_font_combo.currentText()
        )
        settings.endGroup()

    # ===================================================================
    # Dirty tracking
    # ===================================================================

    def _mark_dirty(self, tab_index: int) -> None:
        """Flag *tab_index* as having unsaved changes.

        Enables the Save/Reset/Apply buttons and appends ``*`` to
        the tab title as a visual cue.
        """
        if not self._tab_dirty.get(tab_index, False):
            self._tab_dirty[tab_index] = True
            base = self._tab_titles.get(tab_index, "")
            self._tabs.setTabText(tab_index, f"{base} *")

        if not self._dirty:
            self._dirty = True
            self._save_btn.setEnabled(True)
            self._reset_btn.setEnabled(True)
            self._apply_btn.setEnabled(True)

        # Emit real-time change notification.
        self.config_changed.emit(self._collect_config_dict())

    def _clear_dirty(self) -> None:
        """Clear all dirty flags and restore clean tab titles."""
        self._dirty = False
        self._save_btn.setEnabled(False)
        self._reset_btn.setEnabled(False)
        self._apply_btn.setEnabled(False)
        for idx, base_title in self._tab_titles.items():
            self._tab_dirty[idx] = False
            self._tabs.setTabText(idx, base_title)

    # ===================================================================
    # Config collection (widgets -> dict)
    # ===================================================================

    def _collect_config_dict(self) -> dict[str, Any]:
        """Read every widget value and return a config-compatible dict.

        The returned dict mirrors the YAML structure so it can be
        written directly with ``yaml.safe_dump()``.

        Returns
        -------
        dict[str, Any]
            Nested configuration dictionary.
        """
        cfg: dict[str, Any] = {}

        # -- chia --
        cfg["chia"] = {
            "full_node_host": self._fn_host.text(),
            "full_node_port": self._fn_port.value(),
            "wallet_host": self._wl_host.text(),
            "wallet_port": self._wl_port.value(),
            "ssl_cert_path": self._fn_cert.text(),
            "ssl_key_path": self._fn_key.text(),
            "wallet_cert_path": self._wl_cert.text(),
            "wallet_key_path": self._wl_key.text(),
            "wallet_fingerprint": self._wl_fingerprint.value(),
        }

        # -- dexie --
        cfg["dexie"] = {
            "api_base": self._dx_api_base.text(),
            "max_requests_per_10s": self._dx_rate_limit.value(),
        }

        # -- pairs --
        pairs_list: list[dict[str, Any]] = []
        for row in range(self._pairs_table.rowCount()):
            cb_widget = self._pairs_table.cellWidget(row, 0)
            enabled = cb_widget.isChecked() if cb_widget else True
            name_item = self._pairs_table.item(row, 1)
            base_item = self._pairs_table.item(row, 2)
            quote_item = self._pairs_table.item(row, 3)
            pairs_list.append({
                "name": name_item.text() if name_item else "",
                "base_asset_id": base_item.text() if base_item else "xch",
                "quote_asset_id": quote_item.text() if quote_item else "",
                "enabled": enabled,
            })
        cfg["pairs"] = pairs_list

        # -- strategy --
        tier_spacing: list[float] = []
        tier_size: list[float] = []
        for row in range(self._tier_table.rowCount()):
            sp_item = self._tier_table.item(row, 1)
            sz_item = self._tier_table.item(row, 2)
            tier_spacing.append(
                float(sp_item.text()) if sp_item and sp_item.text() else 0.0
            )
            tier_size.append(
                float(sz_item.text()) if sz_item and sz_item.text() else 0.0
            )

        cfg["strategy"] = {
            "gamma": self._gamma.value(),
            "kappa": self._kappa.value(),
            "phi": self._phi.value(),
            "q_max": self._q_max.value(),
            "min_profit_margin_bps": self._min_profit_bps.value(),
            "offer_ttl_blocks": self._offer_ttl.value(),
            "num_tiers": self._num_tiers.value(),
            "tier_spacing_bps": tier_spacing,
            "tier_size_pct": tier_size,
        }

        # -- risk --
        cfg["risk"] = {
            "soft_limit_pct": self._soft_limit.value(),
            "hard_limit_pct": self._hard_limit.value(),
            "single_cat_cap_pct": self._single_cat_cap.value(),
            "kelly_fraction": self._kelly_fraction.value(),
            "max_capital_per_pair_pct": self._max_capital_per_pair.value(),
        }

        # -- volatility --
        cfg["volatility"] = {
            "lookback_blocks": self._lookback_blocks.value(),
            "yz_alpha": self._yz_alpha.value(),
        }

        # -- monitoring --
        cfg["monitoring"] = {
            "prometheus_port": self._prom_port.value(),
            "telegram_bot_token": self._tg_token.text(),
            "telegram_chat_id": self._tg_chat_id.text(),
        }

        # -- database --
        cfg["database"] = {
            "path": self._db_path.text(),
        }

        return cfg

    # ===================================================================
    # Config population (dict -> widgets)
    # ===================================================================

    def _populate_from_dict(self, cfg: dict[str, Any]) -> None:
        """Write a config dict into every widget, suppressing dirty signals.

        Parameters
        ----------
        cfg : dict[str, Any]
            Nested configuration dictionary matching the YAML schema.
        """
        # Block signals during bulk population to avoid false dirty marks.
        self._block_all_signals(True)

        try:
            chia = cfg.get("chia", {})
            self._fn_host.setText(str(chia.get("full_node_host", "localhost")))
            self._fn_port.setValue(int(chia.get("full_node_port", 8555)))
            self._fn_cert.setText(str(chia.get("ssl_cert_path", "")))
            self._fn_key.setText(str(chia.get("ssl_key_path", "")))
            self._wl_host.setText(str(chia.get("wallet_host", "localhost")))
            self._wl_port.setValue(int(chia.get("wallet_port", 9256)))
            self._wl_cert.setText(str(chia.get("wallet_cert_path", "")))
            self._wl_key.setText(str(chia.get("wallet_key_path", "")))
            self._wl_fingerprint.setValue(
                int(chia.get("wallet_fingerprint", 0))
            )

            dexie = cfg.get("dexie", {})
            self._dx_api_base.setText(
                str(dexie.get("api_base", "https://api.dexie.space/v1"))
            )
            self._dx_rate_limit.setValue(
                int(dexie.get("max_requests_per_10s", 50))
            )

            # -- pairs --
            pairs = cfg.get("pairs", [])
            self._pairs_table.setRowCount(0)
            for pair in pairs:
                self._insert_pair_row(pair)

            # -- strategy --
            strat = cfg.get("strategy", {})
            self._gamma.setValue(float(strat.get("gamma", 0.01)))
            self._kappa.setValue(float(strat.get("kappa", 1.5)))
            self._phi.setValue(float(strat.get("phi", 0.5)))
            self._q_max.setValue(int(strat.get("q_max", 1000)))
            self._min_profit_bps.setValue(
                int(strat.get("min_profit_margin_bps", 35))
            )
            self._offer_ttl.setValue(int(strat.get("offer_ttl_blocks", 60)))
            self._num_tiers.setValue(int(strat.get("num_tiers", 4)))

            spacing = strat.get("tier_spacing_bps", [])
            sizes = strat.get("tier_size_pct", [])
            self._populate_tier_table(spacing, sizes)

            # -- risk --
            risk = cfg.get("risk", {})
            self._soft_limit.setValue(float(risk.get("soft_limit_pct", 0.6)))
            self._hard_limit.setValue(float(risk.get("hard_limit_pct", 0.8)))
            self._single_cat_cap.setValue(
                float(risk.get("single_cat_cap_pct", 0.12))
            )
            self._kelly_fraction.setValue(
                float(risk.get("kelly_fraction", 0.5))
            )
            self._max_capital_per_pair.setValue(
                float(risk.get("max_capital_per_pair_pct", 0.20))
            )

            # -- volatility --
            vol = cfg.get("volatility", {})
            self._lookback_blocks.setValue(
                int(vol.get("lookback_blocks", 200))
            )
            self._yz_alpha.setValue(float(vol.get("yz_alpha", 0.34)))

            # -- monitoring --
            mon = cfg.get("monitoring", {})
            self._prom_port.setValue(int(mon.get("prometheus_port", 9090)))
            self._tg_token.setText(str(mon.get("telegram_bot_token", "")))
            self._tg_chat_id.setText(str(mon.get("telegram_chat_id", "")))

            # -- database --
            db = cfg.get("database", {})
            self._db_path.setText(str(db.get("path", "data/xop_trader.db")))

            # -- raw YAML editor --
            self._yaml_editor.setPlainText(
                yaml.safe_dump(cfg, default_flow_style=False, sort_keys=False)
            )

            # Update visual helpers.
            self._update_soft_limit_bar(self._soft_limit.value())
            self._update_tier_sum_label()

        finally:
            self._block_all_signals(False)

    def _block_all_signals(self, block: bool) -> None:
        """Block or unblock signals on all tracked input widgets.

        Parameters
        ----------
        block : bool
            True to block, False to unblock.
        """
        widgets_to_block = [
            self._fn_host, self._fn_port, self._fn_cert, self._fn_key,
            self._wl_host, self._wl_port, self._wl_cert, self._wl_key,
            self._wl_fingerprint,
            self._dx_api_base, self._dx_rate_limit,
            self._gamma, self._kappa, self._phi, self._q_max,
            self._min_profit_bps, self._offer_ttl, self._num_tiers,
            self._tier_table,
            self._soft_limit, self._hard_limit, self._single_cat_cap,
            self._kelly_fraction, self._max_capital_per_pair,
            self._prom_port, self._tg_token, self._tg_chat_id,
            self._lookback_blocks, self._yz_alpha, self._db_path,
            self._yaml_editor,
            self._theme_combo, self._font_size_spin,
            self._chart_interval, self._dash_interval,
            self._show_grid, self._mono_font_combo,
        ]
        for w in widgets_to_block:
            w.blockSignals(block)

    # ===================================================================
    # Pairs table helpers
    # ===================================================================

    def _insert_pair_row(self, pair: dict[str, Any]) -> None:
        """Append a single pair to the pairs table.

        Parameters
        ----------
        pair : dict[str, Any]
            Must contain keys: name, base_asset_id, quote_asset_id, enabled.
        """
        row = self._pairs_table.rowCount()
        self._pairs_table.insertRow(row)

        # Enabled checkbox -- centred in cell.
        cb = QCheckBox()
        cb.setChecked(bool(pair.get("enabled", True)))
        cb.stateChanged.connect(lambda _s, ti=1: self._mark_dirty(ti))
        cb_container = QWidget()
        cb_layout = QHBoxLayout(cb_container)
        cb_layout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        cb_layout.setContentsMargins(0, 0, 0, 0)
        cb_layout.addWidget(cb)
        self._pairs_table.setCellWidget(row, 0, cb_container)

        # Name.
        name_item = QTableWidgetItem(str(pair.get("name", "")))
        name_item.setFlags(
            name_item.flags() | Qt.ItemFlag.ItemIsEditable
        )
        self._pairs_table.setItem(row, 1, name_item)

        # Base asset.
        base_item = QTableWidgetItem(str(pair.get("base_asset_id", "xch")))
        base_item.setFlags(
            base_item.flags() | Qt.ItemFlag.ItemIsEditable
        )
        self._pairs_table.setItem(row, 2, base_item)

        # Quote asset.
        quote_item = QTableWidgetItem(str(pair.get("quote_asset_id", "")))
        quote_item.setFlags(
            quote_item.flags() | Qt.ItemFlag.ItemIsEditable
        )
        self._pairs_table.setItem(row, 3, quote_item)

        # Action buttons.
        actions = QWidget()
        actions_layout = QHBoxLayout(actions)
        actions_layout.setContentsMargins(4, 2, 4, 2)
        actions_layout.setSpacing(4)

        remove_btn = QPushButton("Remove")
        remove_btn.setObjectName("dangerButton")
        remove_btn.setFixedHeight(24)
        remove_btn.setToolTip("Remove this trading pair")
        # Resolve the button's current row at click time rather than
        # capturing a row index at insert time.  Captured indices go
        # stale when earlier rows are deleted.
        remove_btn.clicked.connect(
            lambda _checked, btn=remove_btn: self._on_remove_pair(
                self._pairs_table.indexAt(btn.pos()).row()
            )
        )
        actions_layout.addWidget(remove_btn)
        self._pairs_table.setCellWidget(row, 4, actions)

    def _on_add_pair(self) -> None:
        """Open the Add Pair dialog and append the result to the table."""
        dialog = _AddPairDialog(self)
        if dialog.exec() == QDialog.DialogCode.Accepted:
            self._insert_pair_row(dialog.pair_data())
            self._mark_dirty(1)

    def _on_remove_pair(self, row: int) -> None:
        """Remove *row* from the pairs table after confirmation.

        Parameters
        ----------
        row : int
            Zero-based row index to remove.
        """
        if row < 0 or row >= self._pairs_table.rowCount():
            return
        name_item = self._pairs_table.item(row, 1)
        pair_name = name_item.text() if name_item else f"row {row}"
        reply = QMessageBox.question(
            self,
            "Remove Pair",
            f"Remove trading pair '{pair_name}'?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if reply == QMessageBox.StandardButton.Yes:
            self._pairs_table.removeRow(row)
            self._mark_dirty(1)

    # ===================================================================
    # Tier table helpers
    # ===================================================================

    def _populate_tier_table(
        self,
        spacing: list[float],
        sizes: list[float],
    ) -> None:
        """Fill the tier configuration table from lists.

        Parameters
        ----------
        spacing : list[float]
            Spread in basis points for each tier.
        sizes : list[float]
            Capital fraction for each tier (should sum to 1.0).
        """
        num = max(len(spacing), len(sizes), self._num_tiers.value())
        self._tier_table.setRowCount(0)
        self._tier_table.blockSignals(True)
        for i in range(num):
            row = self._tier_table.rowCount()
            self._tier_table.insertRow(row)

            # Tier number (read-only).
            tier_item = QTableWidgetItem(str(i + 1))
            tier_item.setFlags(
                tier_item.flags() & ~Qt.ItemFlag.ItemIsEditable
            )
            tier_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self._tier_table.setItem(row, 0, tier_item)

            # Spread bps (editable).
            sp_val = spacing[i] if i < len(spacing) else 0.0
            sp_item = QTableWidgetItem(f"{sp_val:.0f}")
            sp_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self._tier_table.setItem(row, 1, sp_item)

            # Size pct (editable).
            sz_val = sizes[i] if i < len(sizes) else 0.0
            sz_item = QTableWidgetItem(f"{sz_val:.2f}")
            sz_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
            self._tier_table.setItem(row, 2, sz_item)

        self._tier_table.blockSignals(False)
        self._update_tier_sum_label()

    def _on_num_tiers_changed(self, value: int) -> None:
        """Adjust the tier table row count when num_tiers changes.

        Parameters
        ----------
        value : int
            New tier count.
        """
        current = self._tier_table.rowCount()
        if value > current:
            # Add rows with default zero values.
            self._tier_table.blockSignals(True)
            for i in range(current, value):
                row = self._tier_table.rowCount()
                self._tier_table.insertRow(row)
                tier_item = QTableWidgetItem(str(i + 1))
                tier_item.setFlags(
                    tier_item.flags() & ~Qt.ItemFlag.ItemIsEditable
                )
                tier_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                self._tier_table.setItem(row, 0, tier_item)

                sp_item = QTableWidgetItem("0")
                sp_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                self._tier_table.setItem(row, 1, sp_item)

                sz_item = QTableWidgetItem("0.00")
                sz_item.setTextAlignment(Qt.AlignmentFlag.AlignCenter)
                self._tier_table.setItem(row, 2, sz_item)
            self._tier_table.blockSignals(False)
        elif value < current:
            for _ in range(current - value):
                self._tier_table.removeRow(self._tier_table.rowCount() - 1)
        self._update_tier_sum_label()

    def _update_tier_sum_label(self) -> None:
        """Recalculate and display the sum of tier size percentages."""
        total = 0.0
        for row in range(self._tier_table.rowCount()):
            item = self._tier_table.item(row, 2)
            if item and item.text():
                try:
                    total += float(item.text())
                except ValueError:
                    pass
        ok = abs(total - 1.0) < 0.005
        colour = _C.PROFIT_GREEN if ok else _C.LOSS_RED
        self._tier_sum_label.setText(
            f"Size total: {total:.2f}  "
            f"{'(valid)' if ok else '(must equal 1.00)'}"
        )
        self._tier_sum_label.setStyleSheet(
            f"color: {colour}; font-size: 9pt;"
        )

    # ===================================================================
    # Soft-limit progress bar
    # ===================================================================

    def _update_soft_limit_bar(self, value: float) -> None:
        """Refresh the soft-limit progress bar visualisation.

        Parameters
        ----------
        value : float
            Current soft-limit fraction (0.0 - 1.0).
        """
        pct = int(value * 100)
        self._soft_limit_bar.setValue(pct)
        self._soft_limit_bar.setFormat(f"{pct}%")

    # ===================================================================
    # Token visibility toggle
    # ===================================================================

    def _toggle_token_visibility(self) -> None:
        """Toggle the Telegram bot token between hidden and visible."""
        if self._tg_token.echoMode() == QLineEdit.EchoMode.Password:
            self._tg_token.setEchoMode(QLineEdit.EchoMode.Normal)
            self._tg_token_toggle.setText("Hide")
        else:
            self._tg_token.setEchoMode(QLineEdit.EchoMode.Password)
            self._tg_token_toggle.setText("Show")

    # ===================================================================
    # Validation
    # ===================================================================

    def _validate(self) -> list[str]:
        """Validate all fields and return a list of error messages.

        Fields with errors receive a red border via inline QSS.
        Fields that pass validation have their border reset.

        Returns
        -------
        list[str]
            Human-readable validation error messages.  Empty if valid.
        """
        errors: list[str] = []

        # Helper to mark / clear a widget.
        def _set_valid(widget: QWidget, valid: bool, msg: str = "") -> None:
            if hasattr(widget, "setStyleSheet"):
                border = _VALID_BORDER if valid else _INVALID_BORDER
                widget.setStyleSheet(border)
            if not valid and msg:
                widget.setToolTip(msg)
                errors.append(msg)

        # -- Connection: hosts must not be blank --
        _set_valid(
            self._fn_host,
            bool(self._fn_host.text().strip()),
            "Full-node host is required.",
        )
        _set_valid(
            self._wl_host,
            bool(self._wl_host.text().strip()),
            "Wallet host is required.",
        )
        _set_valid(
            self._dx_api_base,
            bool(self._dx_api_base.text().strip()),
            "Dexie API base URL is required.",
        )

        # -- Risk: soft < hard --
        soft_ok = self._soft_limit.value() < self._hard_limit.value()
        _set_valid(
            self._soft_limit,
            soft_ok,
            "Soft limit must be less than hard limit.",
        )
        _set_valid(
            self._hard_limit,
            soft_ok,
            "Hard limit must exceed soft limit.",
        )

        # -- Tier sizes must sum to 1.0 --
        tier_sum = 0.0
        for row in range(self._tier_table.rowCount()):
            item = self._tier_table.item(row, 2)
            if item and item.text():
                try:
                    tier_sum += float(item.text())
                except ValueError:
                    errors.append(
                        f"Tier {row + 1} size is not a valid number."
                    )
        if self._tier_table.rowCount() > 0 and abs(tier_sum - 1.0) >= 0.005:
            errors.append(
                f"Tier size percentages sum to {tier_sum:.4f}, "
                "but must equal 1.0."
            )

        return errors

    # ===================================================================
    # Public API: load_config / save_config
    # ===================================================================

    def load_config(self, path: str) -> None:
        """Load a YAML configuration file and populate all widgets.

        Parameters
        ----------
        path : str
            Filesystem path to a YAML config file.
        """
        resolved = Path(path).expanduser().resolve()
        if not resolved.is_file():
            QMessageBox.warning(
                self,
                "Load Error",
                f"Configuration file not found:\n{resolved}",
            )
            return

        try:
            with open(resolved, encoding="utf-8") as fh:
                raw: dict[str, Any] = yaml.safe_load(fh) or {}
        except yaml.YAMLError as exc:
            QMessageBox.critical(
                self,
                "YAML Parse Error",
                f"Failed to parse {resolved}:\n\n{exc}",
            )
            return

        self._config_path = str(resolved)
        self._populate_from_dict(raw)

        # Snapshot for reset.
        self._clean_snapshot = copy.deepcopy(raw)
        self._clear_dirty()

        self._config_path_label.setText(f"Config: {resolved}")
        self._last_saved_label.setText("")
        log.info("Settings loaded from %s", resolved)

    def save_config(self, path: Optional[str] = None) -> bool:
        """Validate and write the current settings to a YAML file.

        Parameters
        ----------
        path : str | None
            Destination path.  When *None*, re-uses the path from the
            most recent ``load_config()`` call.

        Returns
        -------
        bool
            True if the file was written successfully.
        """
        dest = path or self._config_path
        if not dest:
            dest, _ = QFileDialog.getSaveFileName(
                self,
                "Save Configuration",
                _DEFAULT_CONFIG_FILENAME,
                "YAML Files (*.yaml *.yml);;All Files (*)",
            )
            if not dest:
                return False

        # Run validation.
        errors = self._validate()
        if errors:
            QMessageBox.warning(
                self,
                "Validation Errors",
                "Please fix the following before saving:\n\n"
                + "\n".join(f"  - {e}" for e in errors),
            )
            return False

        cfg = self._collect_config_dict()

        resolved = Path(dest).expanduser().resolve()
        try:
            # Ensure parent directory exists.
            resolved.parent.mkdir(parents=True, exist_ok=True)
            with open(resolved, "w", encoding="utf-8") as fh:
                yaml.safe_dump(
                    cfg,
                    fh,
                    default_flow_style=False,
                    sort_keys=False,
                    allow_unicode=True,
                )
        except OSError as exc:
            QMessageBox.critical(
                self,
                "Save Error",
                f"Failed to write {resolved}:\n\n{exc}",
            )
            return False

        # Persist GUI-only appearance settings separately.
        self._save_appearance_settings()

        # Update state.
        self._config_path = str(resolved)
        self._clean_snapshot = copy.deepcopy(cfg)
        now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        self._last_saved_time = now
        self._last_saved_label.setText(f"Last saved: {now}")
        self._config_path_label.setText(f"Config: {resolved}")
        self._clear_dirty()

        self.config_saved.emit(str(resolved))
        log.info("Settings saved to %s", resolved)
        return True

    # ===================================================================
    # Button slots
    # ===================================================================

    def _on_save_clicked(self) -> None:
        """Handle the Save button click."""
        self.save_config()

    def _on_reset_clicked(self) -> None:
        """Discard unsaved changes and restore the last-saved snapshot."""
        if not self._clean_snapshot:
            return
        reply = QMessageBox.question(
            self,
            "Reset Settings",
            "Discard all unsaved changes and revert to last saved values?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if reply == QMessageBox.StandardButton.Yes:
            self._populate_from_dict(self._clean_snapshot)
            self._clear_dirty()

    def _on_apply_clicked(self) -> None:
        """Save config and emit config_changed so the engine reloads."""
        if self.save_config():
            # Re-emit with the freshly saved dict so listeners reload.
            self.config_changed.emit(self._collect_config_dict())

    # ===================================================================
    # Connection test stubs
    # ===================================================================

    def _test_connection(self, target: str) -> None:
        """Attempt a connection test against the specified target.

        This is a UI stub that sets the result label.  The actual
        RPC call will be wired through ``gui.services`` once the
        async infrastructure is in place.

        Parameters
        ----------
        target : str
            Either ``"full_node"`` or ``"wallet"``.
        """
        if target == "full_node":
            label = self._fn_test_result
            host = self._fn_host.text()
            port = self._fn_port.value()
        else:
            label = self._wl_test_result
            host = self._wl_host.text()
            port = self._wl_port.value()

        if not host.strip():
            label.setText("Host is empty")
            label.setStyleSheet(f"color: {_C.LOSS_RED};")
            return

        # TODO: Perform async RPC health-check via gui.services.
        label.setText(f"Connecting to {host}:{port}...")
        label.setStyleSheet(f"color: {_C.WARNING_YELLOW};")
        log.info("Connection test requested: %s -> %s:%d", target, host, port)

    # ===================================================================
    # Telegram test stub
    # ===================================================================

    def _on_test_telegram(self) -> None:
        """Send a test message through the Telegram bot API.

        This is a UI stub.  The actual HTTP request will be performed
        asynchronously via ``gui.services`` once wired.
        """
        token = self._tg_token.text().strip()
        chat_id = self._tg_chat_id.text().strip()
        if not token or not chat_id:
            QMessageBox.information(
                self,
                "Telegram Test",
                "Both Bot Token and Chat ID are required.",
            )
            return
        # TODO: Async HTTP POST to Telegram sendMessage.
        QMessageBox.information(
            self,
            "Telegram Test",
            "Test alert queued.  (Async send not yet wired.)",
        )
        log.info("Telegram test alert requested for chat %s", chat_id)

    # ===================================================================
    # YAML editor actions
    # ===================================================================

    def _on_validate_yaml(self) -> None:
        """Parse the raw YAML editor content and report errors."""
        text = self._yaml_editor.toPlainText()
        if not text.strip():
            self._yaml_status_label.setText("Editor is empty.")
            self._yaml_status_label.setStyleSheet(
                f"color: {_C.WARNING_YELLOW}; font-size: 9pt;"
            )
            return
        try:
            yaml.safe_load(text)
            self._yaml_status_label.setText("Valid YAML.")
            self._yaml_status_label.setStyleSheet(
                f"color: {_C.PROFIT_GREEN}; font-size: 9pt;"
            )
        except yaml.YAMLError as exc:
            self._yaml_status_label.setText(f"Error: {exc}")
            self._yaml_status_label.setStyleSheet(
                f"color: {_C.LOSS_RED}; font-size: 9pt;"
            )

    def _on_load_from_editor(self) -> None:
        """Parse the YAML editor and push values into all widgets."""
        text = self._yaml_editor.toPlainText()
        if not text.strip():
            QMessageBox.information(
                self, "Load from Editor", "The editor is empty."
            )
            return
        try:
            parsed: dict[str, Any] = yaml.safe_load(text) or {}
        except yaml.YAMLError as exc:
            QMessageBox.critical(
                self,
                "YAML Parse Error",
                f"Cannot load from editor:\n\n{exc}",
            )
            return

        if not isinstance(parsed, dict):
            QMessageBox.warning(
                self,
                "Invalid Structure",
                "Top-level YAML must be a mapping (dict), not a list or scalar.",
            )
            return

        reply = QMessageBox.question(
            self,
            "Load from Editor",
            "Replace all field values with the YAML in the editor?",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
        )
        if reply == QMessageBox.StandardButton.Yes:
            self._populate_from_dict(parsed)
            # Mark all tabs dirty since we cannot diff selectively.
            for idx in self._tab_titles:
                self._mark_dirty(idx)
