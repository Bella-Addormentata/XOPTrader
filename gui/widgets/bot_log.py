"""Real-time log viewer with level/module filtering and syntax highlighting.

Provides a QPlainTextEdit-based log display with a custom
QSyntaxHighlighter that colours timestamps, log levels, numbers,
and quoted strings according to the CHIA colour palette.
"""

from __future__ import annotations

import re
from collections import deque
from typing import Optional

from PySide6.QtCore import Qt, QTimer, Signal
from PySide6.QtGui import (
    QColor,
    QFont,
    QSyntaxHighlighter,
    QTextCharFormat,
    QTextDocument,
)
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Log levels in severity order (index used for filtering).
_LOG_LEVELS: list[str] = ["All", "Debug", "Info", "Warning", "Error", "Critical"]

# Module names that appear in structured log lines.
_MODULES: list[str] = [
    "All", "Engine", "Strategy", "Risk", "Execution", "Monitoring", "Data",
]

# Default maximum number of lines retained in the log buffer.
_DEFAULT_MAX_LINES: int = 10_000


# ---------------------------------------------------------------------------
# Syntax highlighter
# ---------------------------------------------------------------------------

class _LogHighlighter(QSyntaxHighlighter):
    """Syntax highlighter for structured log lines.

    Applies colour rules in the following priority order:
      1. ``[ERROR]`` / ``[CRITICAL]`` -- red background highlight.
      2. ``[WARNING]``                 -- yellow foreground.
      3. ``[INFO]``                    -- default text colour.
      4. ``[DEBUG]``                   -- secondary (dim) text.
      5. Timestamps (ISO-8601 prefix)  -- secondary colour.
      6. Numeric literals              -- info-blue.
      7. Quoted strings                -- light-green.
    """

    def __init__(self, document: QTextDocument) -> None:
        super().__init__(document)
        self._rules: list[tuple[re.Pattern, QTextCharFormat]] = []
        self._build_rules()

    def _build_rules(self) -> None:
        """Compile regex patterns and their corresponding formats."""

        # -- Timestamps: ISO-8601 prefix such as "2025-03-24 12:34:56" --
        fmt_ts = QTextCharFormat()
        fmt_ts.setForeground(QColor(COLORS.TEXT_SECONDARY))
        self._rules.append((
            re.compile(r"\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?"),
            fmt_ts,
        ))

        # -- [CRITICAL] -- red background, white text --
        fmt_crit = QTextCharFormat()
        fmt_crit.setBackground(QColor(COLORS.LOSS_RED))
        fmt_crit.setForeground(QColor("#FFFFFF"))
        fmt_crit.setFontWeight(QFont.Weight.Bold)
        self._rules.append((re.compile(r"\[CRITICAL\]"), fmt_crit))

        # -- [ERROR] -- red background, white text --
        fmt_err = QTextCharFormat()
        fmt_err.setBackground(QColor(COLORS.LOSS_RED))
        fmt_err.setForeground(QColor("#FFFFFF"))
        fmt_err.setFontWeight(QFont.Weight.Bold)
        self._rules.append((re.compile(r"\[ERROR\]"), fmt_err))

        # -- [WARNING] -- yellow foreground --
        fmt_warn = QTextCharFormat()
        fmt_warn.setForeground(QColor(COLORS.WARNING_YELLOW))
        fmt_warn.setFontWeight(QFont.Weight.Bold)
        self._rules.append((re.compile(r"\[WARNING\]"), fmt_warn))

        # -- [INFO] -- primary text colour --
        fmt_info = QTextCharFormat()
        fmt_info.setForeground(QColor(COLORS.TEXT_PRIMARY))
        self._rules.append((re.compile(r"\[INFO\]"), fmt_info))

        # -- [DEBUG] -- secondary (dim) text --
        fmt_debug = QTextCharFormat()
        fmt_debug.setForeground(QColor(COLORS.TEXT_SECONDARY))
        self._rules.append((re.compile(r"\[DEBUG\]"), fmt_debug))

        # -- Numeric literals (integers and decimals) --
        fmt_num = QTextCharFormat()
        fmt_num.setForeground(QColor(COLORS.INFO_BLUE))
        self._rules.append((
            re.compile(r"(?<!\w)-?\d+(?:\.\d+)?(?!\w)"),
            fmt_num,
        ))

        # -- Quoted strings (single or double) --
        fmt_str = QTextCharFormat()
        fmt_str.setForeground(QColor(COLORS.LIGHT_GREEN))
        self._rules.append((re.compile(r'"[^"]*"'), fmt_str))
        self._rules.append((re.compile(r"'[^']*'"), fmt_str))

    def highlightBlock(self, text: str) -> None:
        """Apply all highlighting rules to a single text block.

        Parameters
        ----------
        text:
            The plain text of the current block (line).
        """
        for pattern, fmt in self._rules:
            for match in pattern.finditer(text):
                start = match.start()
                length = match.end() - start
                self.setFormat(start, length, fmt)


# ---------------------------------------------------------------------------
# BotLogWidget
# ---------------------------------------------------------------------------

class BotLogWidget(QWidget):
    """Real-time log viewer with filtering and syntax highlighting.

    Signals
    -------
    error_detected(str):
        Emitted whenever an ERROR or CRITICAL line is appended.
        Payload is the raw message text.
    """

    error_detected = Signal(str)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        # Internal log buffer: bounded deque for O(1) eviction of old entries.
        self._log_buffer: deque[tuple[str, str, str, str]] = deque(
            maxlen=_DEFAULT_MAX_LINES,
        )

        # Active filter state.
        self._level_filter: str = "all"
        self._module_filter: str = "all"
        self._search_pattern: Optional[re.Pattern] = None

        # Debounce timer for search field changes (250 ms).
        # Prevents re-filtering the entire buffer on every keystroke.
        self._search_debounce: QTimer = QTimer(self)
        self._search_debounce.setSingleShot(True)
        self._search_debounce.setInterval(250)
        self._search_debounce.timeout.connect(self._refilter)

        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        """Assemble the complete widget layout."""
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(4)

        root.addLayout(self._build_toolbar())
        self._log_view = self._build_log_view()
        root.addWidget(self._log_view, stretch=1)
        root.addLayout(self._build_status_bar())

    def _build_toolbar(self) -> QHBoxLayout:
        """Create the top toolbar with filter controls.

        Contains level and module combo-boxes, a regex search field,
        clear/copy buttons, an auto-scroll toggle, and a max-lines
        spin box.
        """
        bar = QHBoxLayout()
        bar.setSpacing(6)

        # Log level filter
        lbl_level = QLabel("Level:")
        self._combo_level = QComboBox()
        self._combo_level.addItems(_LOG_LEVELS)
        self._combo_level.currentTextChanged.connect(self._on_level_changed)
        bar.addWidget(lbl_level)
        bar.addWidget(self._combo_level)

        # Module filter
        lbl_module = QLabel("Module:")
        self._combo_module = QComboBox()
        self._combo_module.addItems(_MODULES)
        self._combo_module.currentTextChanged.connect(self._on_module_changed)
        bar.addWidget(lbl_module)
        bar.addWidget(self._combo_module)

        # Regex search / grep field
        lbl_search = QLabel("Search:")
        self._search = QLineEdit()
        self._search.setPlaceholderText("regex pattern...")
        self._search.setMinimumWidth(180)
        self._search.textChanged.connect(self._on_search_changed)
        bar.addWidget(lbl_search)
        bar.addWidget(self._search)

        bar.addStretch()

        # Auto-scroll toggle (default: enabled)
        self._chk_autoscroll = QCheckBox("Auto-scroll")
        self._chk_autoscroll.setChecked(True)
        bar.addWidget(self._chk_autoscroll)

        # Max lines spin box
        lbl_max = QLabel("Max lines:")
        self._spin_max = QSpinBox()
        self._spin_max.setRange(100, 100_000)
        self._spin_max.setValue(_DEFAULT_MAX_LINES)
        self._spin_max.setSingleStep(1000)
        bar.addWidget(lbl_max)
        bar.addWidget(self._spin_max)

        # Copy Selected button
        btn_copy = QPushButton("Copy Selected")
        btn_copy.clicked.connect(self._on_copy_selected)
        bar.addWidget(btn_copy)

        # Clear button
        btn_clear = QPushButton("Clear")
        btn_clear.setObjectName("dangerButton")
        btn_clear.clicked.connect(self.clear)
        bar.addWidget(btn_clear)

        return bar

    def _build_log_view(self) -> QPlainTextEdit:
        """Create the read-only log display with monospaced font.

        Installs the :class:`_LogHighlighter` for syntax colouring.
        """
        view = QPlainTextEdit()
        view.setReadOnly(True)
        view.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        view.setMaximumBlockCount(_DEFAULT_MAX_LINES)

        # Monospaced font: prefer JetBrains Mono, fall back to Consolas.
        font = QFont("JetBrains Mono", 10)
        font.setStyleHint(QFont.StyleHint.Monospace)
        view.setFont(font)

        # Dark background with subtle border.
        view.setStyleSheet(
            f"QPlainTextEdit {{"
            f"  background-color: {COLORS.PANEL_BG};"
            f"  color: {COLORS.TEXT_PRIMARY};"
            f"  border: 1px solid {COLORS.BORDER};"
            f"  selection-background-color: {COLORS.PRIMARY_GREEN};"
            f"  selection-color: {COLORS.DARK_BG};"
            f"}}"
        )

        # Attach the syntax highlighter to the underlying document.
        self._highlighter = _LogHighlighter(view.document())

        return view

    def _build_status_bar(self) -> QHBoxLayout:
        """Create the bottom status bar showing line/buffer info."""
        bar = QHBoxLayout()
        bar.setSpacing(16)

        self._lbl_line_count = QLabel("Lines: 0")
        self._lbl_filtered = QLabel("Filtered: 0/0")
        self._lbl_buffer = QLabel("Buffer: 0%")

        for lbl in (self._lbl_line_count, self._lbl_filtered, self._lbl_buffer):
            lbl.setStyleSheet(f"color: {COLORS.TEXT_SECONDARY}; font-size: 9pt;")
            bar.addWidget(lbl)

        bar.addStretch()
        return bar

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def append_log(
        self,
        timestamp: str,
        level: str,
        module: str,
        message: str,
    ) -> None:
        """Append a single log entry to the viewer.

        The entry is stored in the internal buffer and, if it passes
        the active filters, displayed immediately in the text view.

        Parameters
        ----------
        timestamp:
            ISO-8601 timestamp string (e.g. ``"2025-03-24 12:34:56.789"``).
        level:
            Log level name (``DEBUG``, ``INFO``, ``WARNING``, ``ERROR``,
            ``CRITICAL``).
        module:
            Originating module name (``Engine``, ``Strategy``, etc.).
        message:
            The log message body.
        """
        # Update the deque's maxlen when the user changes the spin box.
        max_lines = self._spin_max.value()
        if self._log_buffer.maxlen != max_lines:
            self._log_buffer = deque(self._log_buffer, maxlen=max_lines)

        self._log_buffer.append((timestamp, level, module, message))

        # Only render if the entry passes the current filters.
        if self._passes_filter(timestamp, level, module, message):
            formatted = self._format_line(timestamp, level, module, message)
            self._log_view.appendPlainText(formatted)

            # Enforce the display limit on the QPlainTextEdit widget.
            self._log_view.setMaximumBlockCount(max_lines)

            # Auto-scroll to the bottom if the toggle is enabled.
            if self._chk_autoscroll.isChecked():
                scrollbar = self._log_view.verticalScrollBar()
                scrollbar.setValue(scrollbar.maximum())

        # Emit signal for ERROR / CRITICAL entries.
        if level.upper() in ("ERROR", "CRITICAL"):
            self.error_detected.emit(message)

        self._update_status()

    def set_level_filter(self, level: str) -> None:
        """Programmatically set the log-level filter.

        Parameters
        ----------
        level:
            One of the level names from :data:`_LOG_LEVELS`.
        """
        idx = self._combo_level.findText(level, Qt.MatchFlag.MatchFixedString)
        if idx >= 0:
            self._combo_level.setCurrentIndex(idx)

    def set_module_filter(self, module: str) -> None:
        """Programmatically set the module filter.

        Parameters
        ----------
        module:
            One of the module names from :data:`_MODULES`.
        """
        idx = self._combo_module.findText(module, Qt.MatchFlag.MatchFixedString)
        if idx >= 0:
            self._combo_module.setCurrentIndex(idx)

    def set_search_pattern(self, pattern: str) -> None:
        """Programmatically set the search / grep regex.

        Parameters
        ----------
        pattern:
            A regular expression string.  An empty string clears
            the search filter.
        """
        self._search.setText(pattern)

    def clear(self) -> None:
        """Clear both the display and the internal log buffer."""
        self._log_buffer.clear()
        self._log_view.clear()
        self._update_status()

    # ------------------------------------------------------------------
    # Internal: filter logic
    # ------------------------------------------------------------------

    def _passes_filter(
        self,
        timestamp: str,
        level: str,
        module: str,
        message: str,
    ) -> bool:
        """Determine whether a log entry should be displayed.

        Checks level severity threshold, module match, and regex
        search pattern in sequence (short-circuit on first failure).

        Parameters
        ----------
        timestamp:
            ISO-8601 timestamp string.
        level:
            Log level name.
        module:
            Originating module name.
        message:
            Log message body.

        Returns
        -------
        True if the entry should be rendered, False otherwise.
        """
        # Level filter: entries below the selected severity are hidden.
        if self._level_filter != "all":
            level_order = {
                "debug": 0, "info": 1, "warning": 2, "error": 3, "critical": 4,
            }
            threshold = level_order.get(self._level_filter, 0)
            entry_level = level_order.get(level.lower(), 0)
            if entry_level < threshold:
                return False

        # Module filter: only matching module names pass.
        if self._module_filter != "all":
            if module.lower() != self._module_filter:
                return False

        # Regex search pattern: must match somewhere in the full line.
        if self._search_pattern is not None:
            full_line = f"{timestamp} [{level}] [{module}] {message}"
            if not self._search_pattern.search(full_line):
                return False

        return True

    @staticmethod
    def _format_line(
        timestamp: str,
        level: str,
        module: str,
        message: str,
    ) -> str:
        """Format a log entry into a single display line.

        Parameters
        ----------
        timestamp:
            ISO-8601 timestamp string.
        level:
            Log level name.
        module:
            Originating module name.
        message:
            Log message body.

        Returns
        -------
        A formatted string such as
        ``"2025-03-24 12:34:56 [INFO] [Engine] Bot started"``.
        """
        return f"{timestamp} [{level.upper():>8s}] [{module}] {message}"

    def _refilter(self) -> None:
        """Re-apply all filters to the entire buffer and redisplay.

        Called when a filter control changes so that previously
        hidden entries can appear (or visible ones can be removed).

        Bulk text operations are wrapped in ``setUpdatesEnabled(False)``
        to suppress per-line repaints and syntax-highlight passes until
        the full re-filter is complete.
        """
        self._log_view.setUpdatesEnabled(False)
        try:
            self._log_view.clear()
            max_lines = self._spin_max.value()
            self._log_view.setMaximumBlockCount(max_lines)

            for timestamp, level, module, message in self._log_buffer:
                if self._passes_filter(timestamp, level, module, message):
                    formatted = self._format_line(timestamp, level, module, message)
                    self._log_view.appendPlainText(formatted)
        finally:
            self._log_view.setUpdatesEnabled(True)

        # Scroll to the end after a full re-filter.
        if self._chk_autoscroll.isChecked():
            scrollbar = self._log_view.verticalScrollBar()
            scrollbar.setValue(scrollbar.maximum())

        self._update_status()

    # ------------------------------------------------------------------
    # Internal: status bar
    # ------------------------------------------------------------------

    def _update_status(self) -> None:
        """Refresh the bottom status bar labels."""
        total = len(self._log_buffer)
        displayed = self._log_view.document().blockCount()
        # QPlainTextEdit always has at least one block (even if empty).
        if displayed == 1 and self._log_view.toPlainText() == "":
            displayed = 0

        max_lines = self._spin_max.value()
        usage_pct = (total / max_lines * 100.0) if max_lines > 0 else 0.0

        self._lbl_line_count.setText(f"Lines: {total}")
        self._lbl_filtered.setText(f"Filtered: {displayed}/{total}")
        self._lbl_buffer.setText(f"Buffer: {usage_pct:.0f}%")

    # ------------------------------------------------------------------
    # Slots for filter controls
    # ------------------------------------------------------------------

    def _on_level_changed(self, text: str) -> None:
        """Handle level combo-box change."""
        self._level_filter = text.lower()
        self._refilter()

    def _on_module_changed(self, text: str) -> None:
        """Handle module combo-box change."""
        self._module_filter = text.lower()
        self._refilter()

    def _on_search_changed(self, text: str) -> None:
        """Handle search field text change.

        Compiles the text as a regex pattern.  If the pattern is
        invalid, it falls back to a plain substring match via
        ``re.escape``.

        Re-filtering is debounced (250 ms) so that rapid keystrokes
        do not trigger expensive full-buffer scans on each character.
        """
        if not text.strip():
            self._search_pattern = None
        else:
            try:
                self._search_pattern = re.compile(text, re.IGNORECASE)
            except re.error:
                # Invalid regex -- escape and treat as literal text.
                self._search_pattern = re.compile(
                    re.escape(text), re.IGNORECASE
                )
        # Cancel any pending debounce and restart the timer.
        self._search_debounce.start()

    def _on_copy_selected(self) -> None:
        """Copy the currently selected text to the system clipboard."""
        from PySide6.QtWidgets import QApplication

        cursor = self._log_view.textCursor()
        selected = cursor.selectedText()
        if selected:
            clipboard = QApplication.clipboard()
            if clipboard is not None:
                # QPlainTextEdit uses Unicode paragraph separators
                # internally; replace them with newlines for clipboard.
                clipboard.setText(selected.replace("\u2029", "\n"))
