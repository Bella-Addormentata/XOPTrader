"""First-run setup wizard dialog for XOPTrader.

Shown automatically when a fresh ``config.yaml`` is bootstrapped.
Summarises what was auto-detected and collects the few credentials
(Telegram alerts) that cannot be discovered automatically.
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import yaml
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)


class FirstRunSetupDialog(QDialog):
    """Setup wizard shown on first launch.

    Displays a summary of values that were auto-detected from the local
    Chia installation, then collects optional credentials (Telegram
    bot token and chat ID) that cannot be discovered automatically.

    Parameters
    ----------
    config_path:
        Path to the bootstrapped ``config.yaml`` that will be patched
        when the user clicks *Save*.
    chia_detected:
        Whether Chia connection details were successfully auto-detected.
    parent:
        Qt parent widget (typically the MainWindow).
    """

    def __init__(
        self,
        config_path: Path,
        chia_detected: bool,
        parent: Optional[QWidget] = None,
    ) -> None:
        super().__init__(parent)
        self._config_path = config_path
        self._chia_detected = chia_detected
        self.setWindowTitle("XOPTrader — First-Time Setup")
        self.setMinimumWidth(520)
        self.setWindowFlag(Qt.WindowContextHelpButtonHint, False)
        self._build_ui()

    # ------------------------------------------------------------------
    # UI construction
    # ------------------------------------------------------------------

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setSpacing(4)
        root.setContentsMargins(8, 8, 8, 8)

        # --- header ---
        title = QLabel("Welcome to XOPTrader")
        title.setStyleSheet("font-size: 16px; font-weight: bold;")
        root.addWidget(title)

        sub = QLabel(
            "A starter <b>config.yaml</b> has been created for you. "
            "Below is a summary of what was detected automatically and "
            "the optional items you may want to configure now."
        )
        sub.setWordWrap(True)
        root.addWidget(sub)

        # --- auto-detected summary ---
        detected_box = QGroupBox("Auto-Detected")
        det_layout = QVBoxLayout(detected_box)
        det_layout.setSpacing(4)

        items = self._detected_items()
        for label_text, ok in items:
            icon = "✔" if ok else "✘"
            colour = "#4caf50" if ok else "#f44336"
            lbl = QLabel(f'<span style="color:{colour}; font-weight:bold;">{icon}</span>  {label_text}')
            lbl.setWordWrap(True)
            det_layout.addWidget(lbl)

        root.addWidget(detected_box)

        # --- optional: telegram ---
        tg_box = QGroupBox("Telegram Alerts  (optional)")
        tg_form = QFormLayout(tg_box)
        tg_form.setContentsMargins(8, 8, 8, 8)
        tg_form.setSpacing(4)

        tg_note = QLabel(
            "Receive trade alerts and error notifications via Telegram. "
            "Leave blank to skip — you can add these later in Settings → Monitoring."
        )
        tg_note.setWordWrap(True)
        tg_note.setStyleSheet("color: #aaa; font-size: 11px;")
        tg_form.addRow(tg_note)

        self._tg_token = QLineEdit()
        self._tg_token.setPlaceholderText("123456789:AAAA…")
        self._tg_token.setEchoMode(QLineEdit.EchoMode.Password)
        tg_form.addRow("Bot Token:", self._tg_token)

        self._tg_chat = QLineEdit()
        self._tg_chat.setPlaceholderText("-1001234567890")
        tg_form.addRow("Chat ID:", self._tg_chat)

        # Show/hide token toggle
        toggle_btn = QPushButton("Show")
        toggle_btn.setCheckable(True)
        toggle_btn.setFixedWidth(56)
        toggle_btn.toggled.connect(self._on_toggle_token)
        tg_form.addRow("", toggle_btn)

        root.addWidget(tg_box)

        # --- hints for remaining manual items ---
        hint_box = QGroupBox("Review in Settings")
        hint_layout = QVBoxLayout(hint_box)
        hint_layout.setSpacing(4)
        for hint in [
            "Trading Pairs — which pairs to enable",
            "Strategy — risk aversion (γ) and ladder depth",
            "Risk — drawdown limits and position sizing",
            "Start in <b>Dry Run</b> mode first to verify connectivity",
        ]:
            hl = QLabel(f"• {hint}")
            hl.setWordWrap(True)
            hint_layout.addWidget(hl)
        root.addWidget(hint_box)

        # --- buttons ---
        btns = QDialogButtonBox()
        save_btn = btns.addButton("Save &amp; Continue", QDialogButtonBox.ButtonRole.AcceptRole)
        skip_btn = btns.addButton("Skip for Now", QDialogButtonBox.ButtonRole.RejectRole)
        save_btn.setDefault(True)
        btns.accepted.connect(self._on_save)
        btns.rejected.connect(self.reject)
        root.addWidget(btns)

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _detected_items(self) -> list[tuple[str, bool]]:
        """Build the list of (description, was_detected) rows."""
        items: list[tuple[str, bool]] = [
            ("Chia SSL cert paths", self._chia_detected),
            ("Wallet fingerprint", self._chia_detected),
            ("RPC ports (full node / wallet)", self._chia_detected),
            ("verify_ssl set to false for localhost", self._chia_detected),
        ]
        # Check if Telegram is already filled in the config.
        tg_present = self._read_telegram_from_config() is not None
        items.append(("Telegram bot token", tg_present))
        return items

    def _read_telegram_from_config(self) -> Optional[str]:
        """Return the telegram_bot_token if it is non-empty in the config."""
        try:
            with open(self._config_path, "r", encoding="utf-8") as fh:
                data = yaml.safe_load(fh) or {}
            token = data.get("monitoring", {}).get("telegram_bot_token", "")
            return token if token else None
        except Exception:
            return None

    # ------------------------------------------------------------------
    # Slots
    # ------------------------------------------------------------------

    def _on_toggle_token(self, checked: bool) -> None:
        mode = QLineEdit.EchoMode.Normal if checked else QLineEdit.EchoMode.Password
        self._tg_token.setEchoMode(mode)
        btn = self.sender()
        if btn:
            btn.setText("Hide" if checked else "Show")

    def _on_save(self) -> None:
        """Patch optional Telegram values into config.yaml if provided."""
        token = self._tg_token.text().strip()
        chat_id = self._tg_chat.text().strip()

        if token or chat_id:
            try:
                with open(self._config_path, "r", encoding="utf-8") as fh:
                    data = yaml.safe_load(fh) or {}
                mon = data.setdefault("monitoring", {})
                if token:
                    mon["telegram_bot_token"] = token
                if chat_id:
                    mon["telegram_chat_id"] = chat_id
                with open(self._config_path, "w", encoding="utf-8") as fh:
                    yaml.dump(
                        data,
                        fh,
                        default_flow_style=False,
                        allow_unicode=True,
                        sort_keys=False,
                    )
            except Exception as exc:
                from PySide6.QtWidgets import QMessageBox  # noqa: WPS433

                QMessageBox.warning(
                    self,
                    "Save Failed",
                    f"Could not write Telegram settings to config:\n{exc}",
                )
                return

        self.accept()
