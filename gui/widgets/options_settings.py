"""The Settings page's Options tab: the options book set up without editing YAML.

What it covers is what a new book needs: switching it on, the wallet it signs
from (the trading wallet, shared, or a dedicated one), the options address,
the engine fence, the top-up target, the grid (contract size, expiries,
strikes) and the limits. Everything else in ``options:`` (the quote asset,
the node, pricing, fees) keeps its config.yaml value: a save merges these
keys onto the file as it was loaded, as every Settings tab does.

The options book's own parser (:func:`gui.services.options.config.parse`) is
the one judge of what is valid, so the tab never accepts something the book
would then refuse.

Defaults, as the book reads them (0.10.53): sharing the trading wallet takes
its host, port, certificates and key from the Connection tab, the fence
follows the book, and the target comes from the limits. [AUTO-SIZE] So does
the contract size, unless a number is written (``contract_xch: auto``).
"""

from __future__ import annotations

import copy
import datetime
import math
from collections.abc import Callable
from typing import Any

from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QGroupBox,
    QLabel,
    QLineEdit,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

__all__ = ["FENCE_MODES", "SIZE_MODES", "TARGET_MODES", "OptionsSettingsSection"]

#: (label, the value written as options.wallet.engine_fence)
FENCE_MODES: tuple[tuple[str, Any], ...] = (
    ("Follows the book (recommended)", "auto"),
    ("Always on", True),
    ("Off: the engine stops while the book acts", False),
)
#: Labels for options.target: from the limits (unset), written, or off (false).
TARGET_MODES: tuple[str, ...] = ("From my limits", "Custom", "Off")
_TARGET_LIMITS, _TARGET_CUSTOM, _TARGET_OFF = range(3)
#: [ROLLING] Labels for options.expiries: a rule that rolls, a list of dates, or
#: none (no new contracts: winding down).
EXPIRY_MODES: tuple[str, ...] = (
    "Every Friday, 08:00 UTC (rolling)",
    "Fixed dates",
    "None: no new contracts",
)
_EXPIRY_ROLLING, _EXPIRY_FIXED, _EXPIRY_NONE = range(3)
#: [AUTO-SIZE] Labels for options.contract_xch: ``auto`` (worked out from the
#: limits), or a number.
SIZE_MODES: tuple[str, ...] = ("Automatic, from my limits", "Fixed")
_SIZE_AUTO, _SIZE_FIXED = range(2)

#: The dedicated wallet's own connection. Its secrets (dotted under options)
#: are the ones a save must take from this tab rather than from the files.
_DEDICATED_KEYS: tuple[str, ...] = ("host", "port", "fingerprint", "cert_path", "key_path")
WALLET_SECRETS: tuple[str, ...] = (
    "options.wallet.cert_path",
    "options.wallet.key_path",
    "options.wallet.fingerprint",
)


def _number_list(text: str) -> list[int]:
    """``"110, 120"`` as ``[110, 120]``; an item that is not a whole number raises ValueError."""
    out: list[int] = []
    for item in text.replace(";", ",").split(","):
        item = item.strip()
        if item:
            value = float(item)
            if not math.isfinite(value) or value != int(value):
                raise ValueError(item)
            out.append(int(value))
    return out


def _text_list(text: str) -> list[str]:
    return [item.strip() for item in text.replace(";", ",").split(",") if item.strip()]


def _limit_value(text: str) -> Any:
    """A limit field: a number, ``unlimited``, or None for blank (unset)."""
    text = text.strip()
    if not text:
        return None
    if text.lower() == "unlimited":
        return "unlimited"
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(text)
    return int(value) if value == int(value) else value


def _expiry_text(value: Any) -> str:
    """An expiry as the tab shows it: UTC ISO, also for a Unix time the book accepts."""
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        try:
            moment = datetime.datetime.fromtimestamp(float(value), tz=datetime.UTC)
        except (OverflowError, OSError, ValueError):
            return str(value)  # the book's parser says what is wrong with it
        return moment.strftime("%Y-%m-%dT%H:%M:%SZ")
    return str(value)


def _number(value: Any, default: float) -> float:
    """[review] A number from config.yaml for a box to show, or ``default``.

    A malformed value must not stop the Settings page loading: the book's own
    parser says what is wrong with it when the tab is judged.
    """
    if isinstance(value, bool):
        return default
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    return number if math.isfinite(number) else default


def _show_number(box: QDoubleSpinBox | QSpinBox, value: float) -> None:
    """Set ``box`` to ``value``, held within the box's range."""
    value = min(max(value, box.minimum()), box.maximum())
    box.setValue(int(value) if isinstance(box, QSpinBox) else value)


def _list_text(value: Any) -> str:
    """A list from config.yaml as the tab shows it; anything else as it is written."""
    if value is None:
        return ""
    if isinstance(value, (list, tuple)):
        return ", ".join(str(item) for item in value)
    return str(value)


def _rule_counts(value: Any) -> tuple[int, int] | None:
    """``options.expiries`` as (weekly, monthly) counts when it is a rule, else None."""
    if isinstance(value, str):
        word = value.strip().lower()
        return {"weekly": (2, 0), "monthly": (0, 2)}.get(word)
    if isinstance(value, dict):
        try:
            return int(value.get("weekly", 0) or 0), int(value.get("monthly", 0) or 0)
        except (TypeError, ValueError, OverflowError):
            return None
    return None


def _limit_text(value: Any) -> str:
    if value is None or value == "":
        return ""
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, float) and math.isfinite(value) and value == int(value):
        return str(int(value))
    return str(value)


class OptionsSettingsSection:
    """The widgets of the Options tab and the dict they read and write.

    ``on_change`` is called on every edit (the page's dirty marking).
    """

    def __init__(self, on_change: Callable[[], None]) -> None:
        self._on_change = on_change
        #: Edited since the last load: only then does a save write ``options:``,
        #: so an untouched tab leaves the section exactly as config.yaml has it.
        self._edited = False
        #: The dedicated connection as loaded, to tell which secrets were edited.
        self._shown: dict[str, str] = {}
        #: Shared at load: switching sharing ON here clears the dedicated
        #: connection, which would otherwise outlive it unseen.
        self._shared_at_load = False
        #: [review] Numbers as config.yaml has them. A box shows a few decimals
        #: and only numbers: until the operator edits it, a save writes the
        #: number back as it was (the book's parser judges a malformed one),
        #: never the box's rounding or fallback.
        self._loaded_numbers: dict[str, Any] = {}
        #: The number boxes edited since the last load.
        self._touched: set[str] = set()
        self.widget = self._build()

    # -- building ----------------------------------------------------------------

    def _build(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(16, 14, 16, 14)
        layout.setSpacing(10)

        intro = QLabel(
            "The options book writes covered calls on xchoptions.com. Switch it Off, to "
            "Rehearse or Live on the Options page; this tab sets it up. Settings not shown "
            "here (the quote asset, pricing, fees) keep their config.yaml values."
        )
        intro.setWordWrap(True)
        intro.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        layout.addWidget(intro)

        # -- the book --
        book = QGroupBox("Options book")
        book_form = QFormLayout(book)
        self.enabled = QCheckBox("Enable the options book")
        self.enabled.setToolTip("options.enabled. Off, the Options page cannot be switched on.")
        book_form.addRow("", self.enabled)
        self.network = QComboBox()
        self.network.addItems(["mainnet", "testnet11"])
        book_form.addRow("Network:", self.network)
        layout.addWidget(book)

        # -- the wallet --
        wallet = QGroupBox("Wallet")
        wallet_form = QFormLayout(wallet)
        self.share = QCheckBox("Sign from the trading wallet (shared with the engine)")
        self.share.setToolTip(
            "options.wallet.share_trading_wallet. The book then signs with the engine's own "
            "wallet and key, from the Connection tab, and the engine fence keeps the two "
            "apart. Off, the book needs a dedicated wallet daemon with a key of its own."
        )
        wallet_form.addRow("", self.share)
        self.address = QLineEdit()
        self.address.setPlaceholderText("xch1... -- the address the book mints from and is paid to")
        self.address.setToolTip(
            "options.wallet.address. Only coins at this address are the book's. With a shared "
            "wallet, pick an address of the trading key that the engine does not send to "
            "itself (one a Dexie trade paid into)."
        )
        wallet_form.addRow("Options address:", self.address)
        self.fence = QComboBox()
        for label, _value in FENCE_MODES:
            self.fence.addItem(label)
        self.fence.setToolTip(
            "options.wallet.engine_fence. Follows the book: the engine leaves the options "
            "address's coins alone while the book is on, has a contract open or a "
            "transaction unsettled, and trades with them otherwise."
        )
        self._fence_label = QLabel("Engine fence:")
        wallet_form.addRow(self._fence_label, self.fence)

        self.dedicated_host = QLineEdit()
        self.dedicated_host.setPlaceholderText("localhost")
        self.dedicated_port = QSpinBox()
        self.dedicated_port.setRange(0, 65535)
        self.dedicated_port.setSpecialValueText("(not set)")
        self.dedicated_fingerprint = QLineEdit()
        self.dedicated_fingerprint.setPlaceholderText("the dedicated wallet's key")
        self.dedicated_cert = QLineEdit()
        self.dedicated_cert.setPlaceholderText(".../config/ssl/wallet/private_wallet.crt")
        self.dedicated_key = QLineEdit()
        self.dedicated_key.setPlaceholderText(".../config/ssl/wallet/private_wallet.key")
        self._dedicated_rows: list[tuple[QLabel, QWidget]] = []
        for label, field in (
            ("Wallet host:", self.dedicated_host),
            ("Wallet RPC port:", self.dedicated_port),
            ("Key fingerprint:", self.dedicated_fingerprint),
            ("Certificate:", self.dedicated_cert),
            ("Certificate key:", self.dedicated_key),
        ):
            row_label = QLabel(label)
            wallet_form.addRow(row_label, field)
            self._dedicated_rows.append((row_label, field))
        layout.addWidget(wallet)

        # -- the top-up target --
        target = QGroupBox("Top-ups")
        target_form = QFormLayout(target)
        self.target_mode = QComboBox()
        self.target_mode.addItems(list(TARGET_MODES))
        self.target_mode.setToolTip(
            "options.target: what a Live step keeps at the options address, sending from the "
            "trading funds when it falls more than the tolerance below. Used with the engine "
            "fence. From my limits: max locked XCH plus one contract, and 2 of the quote asset "
            "(plus max locked quote with puts); with no XCH cap, nothing is topped up."
        )
        target_form.addRow("Keep the address at:", self.target_mode)
        self.target_hint = QLabel("")
        self.target_hint.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        target_form.addRow("", self.target_hint)
        self.target_xch = QDoubleSpinBox()
        self.target_xch.setRange(0.0, 1_000_000.0)
        self.target_xch.setDecimals(4)
        self.target_xch.setSuffix(" XCH")
        self.target_quote = QDoubleSpinBox()
        self.target_quote.setRange(0.0, 100_000_000.0)
        self.target_quote.setDecimals(3)
        self.target_tolerance = QSpinBox()
        self.target_tolerance.setRange(0, 99)
        self.target_tolerance.setValue(20)
        self.target_tolerance.setSuffix(" %")
        self._custom_rows: list[tuple[QLabel, QWidget]] = []
        for label, field in (
            ("XCH:", self.target_xch),
            ("Quote asset:", self.target_quote),
            ("Tolerance:", self.target_tolerance),
        ):
            row_label = QLabel(label)
            target_form.addRow(row_label, field)
            self._custom_rows.append((row_label, field))
        layout.addWidget(target)

        # -- the grid --
        grid = QGroupBox("Grid")
        grid_form = QFormLayout(grid)
        self.size_mode = QComboBox()
        self.size_mode.addItems(list(SIZE_MODES))
        self.size_mode.setToolTip(
            "options.contract_xch. Automatic (auto): the largest of 1, 10, 100, ... XCH at "
            "which every call slot (strikes x expiries, within Max contracts and Max per "
            "expiry) still fits inside Max XCH locked, and never under 1 XCH. More strikes or "
            "dates, or a lower cap, mean smaller contracts. Fixed: the size you write."
        )
        grid_form.addRow("Contract size:", self.size_mode)
        self.size_hint = QLabel("")
        self.size_hint.setWordWrap(True)
        self.size_hint.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        grid_form.addRow("", self.size_hint)
        self.contract_xch = QDoubleSpinBox()
        self.contract_xch.setRange(0.0, 1_000_000.0)
        self.contract_xch.setDecimals(4)
        # [review] A size config.yaml has malformed shows as not set, not as a
        # number the book would mint with.
        self.contract_xch.setSpecialValueText("not set")
        self.contract_xch.setValue(1.0)
        self.contract_xch.setSuffix(" XCH")
        self._size_label = QLabel("XCH per contract:")
        grid_form.addRow(self._size_label, self.contract_xch)
        self.expiry_mode = QComboBox()
        self.expiry_mode.addItems(list(EXPIRY_MODES))
        self.expiry_mode.setToolTip(
            "options.expiries. Rolling: the book keeps the next Fridays at 08:00 UTC (the "
            "crypto options market's time; xchoptions.com has no standard dates) and moves on "
            "by itself, so it never runs out of expiries. Fixed: the dates you list."
        )
        grid_form.addRow("Expiries:", self.expiry_mode)
        self.expiry_weeks = QSpinBox()
        self.expiry_weeks.setRange(0, 12)
        self.expiry_weeks.setValue(2)
        self.expiry_weeks.setToolTip("How many weekly Fridays ahead to keep.")
        self.expiry_months = QSpinBox()
        self.expiry_months.setRange(0, 6)
        self.expiry_months.setValue(0)
        self.expiry_months.setToolTip(
            "And how many month-ends (the last Friday of a month) ahead, for longer contracts."
        )
        self.expiries = QLineEdit()
        self.expiries.setPlaceholderText("2026-12-04T08:00:00Z, 2026-12-11T08:00:00Z")
        self.expiries.setToolTip("options.expiries: UTC times, comma-separated.")
        self._rolling_rows: list[tuple[QLabel, QWidget]] = []
        self._fixed_rows: list[tuple[QLabel, QWidget]] = []
        for label, field, rows in (
            ("Weekly Fridays ahead:", self.expiry_weeks, self._rolling_rows),
            ("Month-ends ahead:", self.expiry_months, self._rolling_rows),
            ("Dates (UTC):", self.expiries, self._fixed_rows),
        ):
            row_label = QLabel(label)
            grid_form.addRow(row_label, field)
            rows.append((row_label, field))
        self.call_strikes = QLineEdit()
        self.call_strikes.setPlaceholderText("none")
        self.call_strikes.setToolTip(
            "options.call_strikes_pct: percent of spot; blank for none. With neither list "
            "written in config.yaml: 110, 120."
        )
        grid_form.addRow("Call strikes (% of spot):", self.call_strikes)
        self.put_strikes = QLineEdit()
        self.put_strikes.setPlaceholderText("none")
        self.put_strikes.setToolTip(
            "options.put_strikes_pct: percent of spot; blank for none. With neither list "
            "written in config.yaml: 90, 80."
        )
        grid_form.addRow("Put strikes (% of spot):", self.put_strikes)
        layout.addWidget(grid)

        # -- the limits --
        limits = QGroupBox("Limits")
        limits_form = QFormLayout(limits)
        self.limit_fields: dict[str, QLineEdit] = {}
        for key, label, hint in (
            ("max_locked_xch", "Max XCH locked:", "e.g. 1.0, or unlimited"),
            ("max_locked_quote", "Max quote locked:", "e.g. 100, or unlimited"),
            ("max_contracts", "Max contracts:", "e.g. 2"),
            ("max_contracts_per_expiry", "Max per expiry:", "e.g. 2"),
        ):
            field = QLineEdit()
            field.setPlaceholderText(hint)
            field.setToolTip(f"options.limits.{key}: a number, 'unlimited', or blank (unset).")
            limits_form.addRow(label, field)
            self.limit_fields[key] = field
        self.limits_hint = QLabel("")
        self.limits_hint.setWordWrap(True)
        self.limits_hint.setStyleSheet(f"color: {_C.TEXT_SECONDARY};")
        limits_form.addRow("", self.limits_hint)
        layout.addWidget(limits)
        layout.addStretch(1)

        for box in (self.enabled, self.share):
            box.toggled.connect(self._changed)
        for combo in (
            self.network,
            self.fence,
            self.target_mode,
            self.size_mode,
            self.expiry_mode,
        ):
            combo.currentIndexChanged.connect(self._changed)
        for edit in (
            self.address,
            self.dedicated_host,
            self.dedicated_fingerprint,
            self.dedicated_cert,
            self.dedicated_key,
            self.expiries,
            self.call_strikes,
            self.put_strikes,
            *self.limit_fields.values(),
        ):
            edit.textChanged.connect(self._changed)
        for spin in (
            self.dedicated_port,
            self.target_xch,
            self.target_quote,
            self.target_tolerance,
            self.contract_xch,
            self.expiry_weeks,
            self.expiry_months,
        ):
            spin.valueChanged.connect(self._changed)
        for key, box in self._number_boxes().items():
            box.valueChanged.connect(lambda _value, key=key: self._touched.add(key))
        self._sync_rows()
        return page

    def _number_boxes(self) -> dict[str, QDoubleSpinBox | QSpinBox]:
        """The boxes whose numbers are written back as loaded until edited."""
        return {
            "contract_xch": self.contract_xch,
            "target_xch": self.target_xch,
            "target_quote": self.target_quote,
            "target_tolerance": self.target_tolerance,
        }

    def inputs(self) -> list[QWidget]:
        """Every input, for the page's signal blocking while it populates."""
        return [
            self.enabled,
            self.network,
            self.share,
            self.address,
            self.fence,
            self.dedicated_host,
            self.dedicated_port,
            self.dedicated_fingerprint,
            self.dedicated_cert,
            self.dedicated_key,
            self.target_mode,
            self.target_xch,
            self.target_quote,
            self.target_tolerance,
            self.size_mode,
            self.contract_xch,
            self.expiry_mode,
            self.expiry_weeks,
            self.expiry_months,
            self.expiries,
            self.call_strikes,
            self.put_strikes,
            *self.limit_fields.values(),
        ]

    def _changed(self, *_args: Any) -> None:
        self._sync_rows()
        if any(widget.signalsBlocked() for widget in (self.enabled, self.address)):
            return  # the page is populating
        self._edited = True
        self._on_change()

    def _sync_rows(self) -> None:
        shared = self.share.isChecked()
        self.fence.setEnabled(shared)
        self._fence_label.setEnabled(shared)
        for label, field in self._dedicated_rows:
            label.setVisible(not shared)
            field.setVisible(not shared)
        custom = self.target_mode.currentIndex() == _TARGET_CUSTOM
        for label, field in self._custom_rows:
            label.setVisible(custom)
            field.setVisible(custom)
        rolling = self.expiry_mode.currentIndex() == _EXPIRY_ROLLING
        for label, field in self._rolling_rows:
            label.setVisible(rolling)
            field.setVisible(rolling)
        for label, field in self._fixed_rows:
            label.setVisible(not rolling)
            field.setVisible(not rolling)
        fixed_size = self.size_mode.currentIndex() == _SIZE_FIXED
        self._size_label.setVisible(fixed_size)
        self.contract_xch.setVisible(fixed_size)
        self.size_hint.setVisible(not fixed_size)
        self.size_hint.setText(self._size_hint())
        self.target_hint.setText(self._target_hint())
        self.limits_hint.setText(self._limits_hint())

    def _auto_size(self) -> tuple[int, int, float | None]:
        """[AUTO-SIZE] What Automatic comes to with the fields as they are.

        (XCH a contract, the call slots the grid can hold, Max XCH locked or
        None when it is not a number) -- by the book's own rule
        (:func:`gui.services.options.config.auto_contract_xch`), so the tab
        shows the size the book would mint with. A field the book would
        refuse counts as unset here; Save says what is wrong with it.
        """
        from gui.services.options import config as options_config

        def cap(key: str) -> float | None:
            try:
                value = _limit_value(self.limit_fields[key].text())
            except ValueError:
                return None
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
                return None
            return float(value)

        def count(key: str) -> int:
            try:
                value = _limit_value(self.limit_fields[key].text())
            except ValueError:
                return options_config.UNSET
            if value == "unlimited":
                return options_config.UNLIMITED
            if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 1:
                return options_config.UNSET
            return int(value)

        xch_cap = cap("max_locked_xch")
        limits = options_config.Limits(
            max_locked_xch=(
                options_config.UNSET
                if xch_cap is None
                else int(math.floor(xch_cap * options_config.XCH_MOJOS))
            ),
            max_locked_quote=options_config.UNSET,
            max_contracts=count("max_contracts"),
            max_contracts_per_expiry=count("max_contracts_per_expiry"),
            max_fee_mojos=0,
        )
        try:
            calls = len(_number_list(self.call_strikes.text()))
        except ValueError:
            calls = 0
        mode = self.expiry_mode.currentIndex()
        if mode == _EXPIRY_ROLLING:
            expiries = self.expiry_weeks.value() + self.expiry_months.value()
        elif mode == _EXPIRY_FIXED:
            dates = _text_list(self.expiries.text())
            try:
                # [review #254] As the book counts them: the same moment
                # written twice, or in two UTC offsets, is one expiry.
                expiries = options_config.distinct_expiries(dates)
            except options_config.OptionsConfigError:
                expiries = len(set(dates))  # Save says what is wrong with it
        else:
            expiries = 0
        size = options_config.auto_contract_xch(limits, calls, expiries)
        return size, options_config.held_call_slots(limits, calls, expiries), xch_cap

    def _contract_size(self) -> float:
        """The contract size the book would use: the box's, or what Automatic comes to."""
        if self.size_mode.currentIndex() == _SIZE_FIXED:
            return self.contract_xch.value()
        return float(self._auto_size()[0])

    def _size_hint(self) -> str:
        """[AUTO-SIZE] What Automatic comes to, and why."""
        if self.size_mode.currentIndex() == _SIZE_FIXED:
            return ""
        size, slots, cap = self._auto_size()
        if cap is None:
            return f"{size} XCH a contract: Max XCH locked below has no number to size it from."
        if slots <= 0:
            return f"{size} XCH a contract: there are no call slots to size it from."
        covered = min(slots, int(cap // size))
        if covered < slots:
            return (
                f"{size} XCH a contract, the smallest it goes: Max XCH locked covers "
                f"{covered} of the {slots} call slots, nearest expiry first."
            )
        return f"{size} XCH a contract: all {slots} call slots fit inside Max XCH locked."

    def _limits_hint(self) -> str:
        """[review] Why nothing would be minted with these limits, or ""."""
        values: dict[str, Any] = {}
        for key, field in self.limit_fields.items():
            try:
                values[key] = _limit_value(field.text())
            except ValueError:
                return ""  # Save says what is wrong with it
        if any(value is None for value in values.values()):
            return "Nothing is minted until all four limits are set: a number each, or unlimited."
        cap = values["max_locked_xch"]
        if isinstance(cap, (int, float)) and cap < self._contract_size():
            return "Max XCH locked is less than one contract: nothing is minted."
        return ""

    def _target_hint(self) -> str:
        mode = self.target_mode.currentIndex()
        if mode == _TARGET_OFF:
            return "Nothing is topped up: fund the options address yourself."
        if mode == _TARGET_CUSTOM:
            return ""
        try:
            cap = _limit_value(self.limit_fields["max_locked_xch"].text())
        except ValueError:
            return ""
        if not isinstance(cap, (int, float)):
            return "No XCH cap set below, so nothing is topped up."
        quote = 2.0
        try:
            puts = _number_list(self.put_strikes.text())
            quote_cap = _limit_value(self.limit_fields["max_locked_quote"].text())
        except ValueError:
            puts, quote_cap = [], None
        if puts and isinstance(quote_cap, (int, float)):
            quote += float(quote_cap)
        xch = float(cap) + self._contract_size()
        return f"From your limits: {xch:g} XCH and {quote:g} of the quote asset."

    # -- config <-> widgets --------------------------------------------------------

    def populate(self, options: Any) -> None:
        """Show ``options:`` (a mapping, or None when config.yaml has none)."""
        self._edited = False
        self._loaded_numbers = {}
        section = options if isinstance(options, dict) else {}
        wallet = section.get("wallet") if isinstance(section.get("wallet"), dict) else {}
        self.enabled.setChecked(section.get("enabled") is True)
        network = str(section.get("network") or "mainnet").strip().lower()
        self.network.setCurrentIndex(1 if network == "testnet11" else 0)
        self.share.setChecked(wallet.get("share_trading_wallet") is True)
        self.address.setText(str(wallet.get("address") or ""))
        fence = wallet.get("engine_fence")
        if fence is None or (isinstance(fence, str) and fence.strip().lower() == "auto"):
            self.fence.setCurrentIndex(0)
        else:
            self.fence.setCurrentIndex(1 if fence is True else 2)
        self.dedicated_host.setText(str(wallet.get("host") or ""))
        try:
            self.dedicated_port.setValue(int(wallet.get("port") or 0))
        except (TypeError, ValueError):
            self.dedicated_port.setValue(0)
        fingerprint = wallet.get("fingerprint")
        self.dedicated_fingerprint.setText("" if fingerprint in (None, "") else str(fingerprint))
        self.dedicated_cert.setText(str(wallet.get("cert_path") or ""))
        self.dedicated_key.setText(str(wallet.get("key_path") or ""))
        self._shared_at_load = self.share.isChecked()
        self._shown = self._dedicated_texts()

        target = section.get("target")
        if target is False:
            self.target_mode.setCurrentIndex(_TARGET_OFF)
        elif isinstance(target, dict):
            self.target_mode.setCurrentIndex(_TARGET_CUSTOM)
            tolerance = target.get("tolerance_pct")
            for key, box, raw, default in (
                ("target_xch", self.target_xch, target.get("xch"), 0.0),
                ("target_quote", self.target_quote, target.get("quote"), 0.0),
                ("target_tolerance", self.target_tolerance, tolerance, 20.0),
            ):
                _show_number(box, _number(default if raw is None else raw, default))
                self._loaded_numbers[key] = raw
        else:
            self.target_mode.setCurrentIndex(_TARGET_LIMITS)

        size = section.get("contract_xch")
        if size is None or (isinstance(size, str) and size.strip().lower() == "auto"):
            # [AUTO-SIZE] `auto`, or not written (the book reads that as auto
            # too): Automatic. The box holds what Fixed would start from, and
            # nothing loaded, so switching to Fixed writes the box's number.
            self.size_mode.setCurrentIndex(_SIZE_AUTO)
            _show_number(self.contract_xch, 1.0)
            self._loaded_numbers["contract_xch"] = None
        else:
            # A number. Malformed, or not above 0: "not set" (0), and
            # written back as it is for the book's parser to refuse on Save,
            # with its reason.
            self.size_mode.setCurrentIndex(_SIZE_FIXED)
            _show_number(self.contract_xch, max(_number(size, 0.0), 0.0))
            self._loaded_numbers["contract_xch"] = size
        expiries = section.get("expiries")
        rule = _rule_counts(expiries)
        listed = expiries if isinstance(expiries, (list, tuple)) else []
        self.expiries.setText(", ".join(_expiry_text(item) for item in listed))
        if isinstance(expiries, str) and expiries.strip().lower() == "none":
            self.expiry_mode.setCurrentIndex(_EXPIRY_NONE)
        elif listed:
            self.expiry_mode.setCurrentIndex(_EXPIRY_FIXED)
        else:
            # A rule, or nothing written: the book reads that as weekly too.
            weeks, months = rule if rule is not None else (2, 0)
            self.expiry_mode.setCurrentIndex(_EXPIRY_ROLLING)
            _show_number(self.expiry_weeks, weeks)
            _show_number(self.expiry_months, months)
        calls = section.get("call_strikes_pct")
        puts = section.get("put_strikes_pct")
        if "call_strikes_pct" not in section and "put_strikes_pct" not in section:
            # [DEFAULTS] What the book writes with no strikes written: shown, so
            # a save keeps both sides instead of writing none.
            from gui.services.options import config as options_config

            calls = list(options_config.DEFAULT_CALL_STRIKES_PCT)
            puts = list(options_config.DEFAULT_PUT_STRIKES_PCT)
        self.call_strikes.setText(_list_text(calls))
        self.put_strikes.setText(_list_text(puts))
        limits = section.get("limits") if isinstance(section.get("limits"), dict) else {}
        for key, field in self.limit_fields.items():
            field.setText(_limit_text(limits.get(key)))
        self._sync_rows()
        self._touched = set()  # what populating set is not an edit

    def _number_out(self, key: str, box: QDoubleSpinBox | QSpinBox, digits: int) -> Any:
        """[review] What a number box writes: config.yaml's own value until it is edited."""
        raw = self._loaded_numbers.get(key)
        if key not in self._touched and raw is not None:
            return raw
        value = box.value()
        return value if isinstance(box, QSpinBox) else round(value, digits)

    def load_from_editor(self, options: Any, on_disk: Any) -> None:
        """[review] Load from Editor: show the editor's ``options``, told from ``on_disk``.

        The baselines stay those of the config on disk (the last load or
        save), so the secrets are told from what the files hold, and the tab
        counts as edited when the editor's section differs, so a save writes
        what it shows.
        """
        self.populate(on_disk)
        shown, shared_at_load = self._shown, self._shared_at_load
        self.populate(options)
        self._shown, self._shared_at_load = shown, shared_at_load
        mine = options if isinstance(options, dict) else {}
        theirs = on_disk if isinstance(on_disk, dict) else {}
        self._edited = mine != theirs

    def rebase(self, options: Any) -> None:
        """[review] After a save: show ``options`` as saved and make it the baseline.

        Edits are told from the baseline. Kept at the load, a secret changed
        and saved, then put back, would count as no edit and the newer value
        would stay on disk; and switching to sharing and back across two saves
        would leave the dedicated connection's secrets deleted.
        """
        widgets = self.inputs()
        was = [widget.blockSignals(True) for widget in widgets]
        try:
            self.populate(options)
        finally:
            for widget, blocked in zip(widgets, was, strict=True):
                widget.blockSignals(blocked)

    def _collect_expiries(self) -> Any:
        mode = self.expiry_mode.currentIndex()
        if mode == _EXPIRY_NONE:
            return "none"
        if mode == _EXPIRY_FIXED:
            dates = _text_list(self.expiries.text())
            if not dates:
                # An empty list reads as weekly: never save one for "fixed".
                raise ValueError("fixed expiries need at least one date")
            return dates
        weeks, months = self.expiry_weeks.value(), self.expiry_months.value()
        # The two named rules are written by name, as config.example.yaml has them.
        named = {(2, 0): "weekly", (0, 2): "monthly"}.get((weeks, months))
        return named or {"weekly": weeks, "monthly": months}

    def wants_writing(self) -> bool:
        """Whether a save writes ``options:``: only once this tab has been edited."""
        return self._edited

    def collect(self) -> dict[str, Any]:
        """The keys this tab owns, as YAML. Raises ValueError on a malformed list or limit."""
        shared = self.share.isChecked()
        wallet: dict[str, Any] = {
            "share_trading_wallet": shared,
            "address": self.address.text().strip(),
            # [review] A wallet of its own has no fence: its disabled control
            # must not save one the book would refuse.
            "engine_fence": FENCE_MODES[self.fence.currentIndex()][1] if shared else "auto",
        }
        if shared and not self._shared_at_load:
            # [review] Switched to sharing here: the dedicated connection goes,
            # so the Connection tab's applies (finish() removes the Nones).
            wallet.update({key: None for key in _DEDICATED_KEYS})
        if not shared:
            fingerprint = self.dedicated_fingerprint.text().strip()
            wallet.update(
                host=self.dedicated_host.text().strip() or "localhost",
                port=self.dedicated_port.value() or None,
                fingerprint=int(fingerprint) if fingerprint else None,
                cert_path=self.dedicated_cert.text().strip(),
                key_path=self.dedicated_key.text().strip(),
            )
        options: dict[str, Any] = {
            "enabled": self.enabled.isChecked(),
            "network": self.network.currentText(),
            "wallet": wallet,
            "contract_xch": (
                "auto"
                if self.size_mode.currentIndex() == _SIZE_AUTO
                else self._number_out("contract_xch", self.contract_xch, 4)
            ),
            "expiries": self._collect_expiries(),
            "call_strikes_pct": _number_list(self.call_strikes.text()),
            "put_strikes_pct": _number_list(self.put_strikes.text()),
            "limits": {key: _limit_value(field.text()) for key, field in self.limit_fields.items()},
        }
        mode = self.target_mode.currentIndex()
        if mode == _TARGET_OFF:
            options["target"] = False
        elif mode == _TARGET_CUSTOM:
            options["target"] = {
                "xch": self._number_out("target_xch", self.target_xch, 4),
                "quote": self._number_out("target_quote", self.target_quote, 3),
                "tolerance_pct": self._number_out("target_tolerance", self.target_tolerance, 0),
            }
        else:
            options["target"] = None  # removed by finish(): the limits decide
        return options

    @staticmethod
    def finish(options: dict[str, Any]) -> None:
        """After the merge onto config.yaml: remove what this tab leaves to the defaults.

        A target from the limits and a blank limit are absences, not values;
        a merge cannot remove a key, so they are taken out here. On a shared
        wallet, empty connection fields are left to the Connection tab.
        """
        if options.get("target") is None:
            options.pop("target", None)
        limits = options.get("limits")
        if isinstance(limits, dict):
            for key in [key for key, value in limits.items() if value is None]:
                limits.pop(key)
        wallet = options.get("wallet")
        if isinstance(wallet, dict):
            for key in _DEDICATED_KEYS:
                if key in wallet and wallet[key] in (None, ""):
                    wallet.pop(key)

    def _dedicated_texts(self) -> dict[str, str]:
        return {
            "options.wallet.cert_path": self.dedicated_cert.text().strip(),
            "options.wallet.key_path": self.dedicated_key.text().strip(),
            "options.wallet.fingerprint": self.dedicated_fingerprint.text().strip(),
        }

    def edited_secrets(self) -> frozenset[str]:
        """[review] The wallet secrets this tab changed, for the save to take from it.

        config_split keeps these from the files unless told otherwise, since
        nothing else in the app edits them. Switching to sharing here clears
        them all; on a dedicated wallet, the fields changed since the load.
        """
        if not self._edited:
            return frozenset()
        if self.share.isChecked():
            return frozenset() if self._shared_at_load else frozenset(WALLET_SECRETS)
        now = self._dedicated_texts()
        return frozenset(key for key in WALLET_SECRETS if now[key] != self._shown.get(key, ""))

    @staticmethod
    def validate_untouched(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
        """[review] An untouched tab: what a save breaks for the book, if anything.

        Only a book that parsed before and would not after: a problem already
        in config.yaml is not this save's to block.
        """
        from gui.services.options import config as options_config

        try:
            options_config.parse(copy.deepcopy(before))
        except options_config.OptionsConfigError:
            return []
        try:
            options_config.parse(copy.deepcopy(after))
        except options_config.OptionsConfigError as exc:
            return [f"Options: this change breaks the options book: {exc}"]
        return []

    def validate(self, merged_root: dict[str, Any]) -> list[str]:
        """What the options book itself would refuse in ``merged_root`` (empty when fine)."""
        if self.expiry_mode.currentIndex() == _EXPIRY_FIXED and not _text_list(
            self.expiries.text()
        ):
            return [
                "Options: fixed expiries need at least one date (or choose Every Friday, or None)"
            ]
        try:
            options = self.collect()
        except ValueError as exc:
            return [f"Options: a strike, fingerprint or limit is not a number ({exc})"]
        root = copy.deepcopy(merged_root)
        section = root.setdefault("options", {})
        if not isinstance(section, dict):
            return ["Options: options: must be a mapping"]
        from gui.services.config_split import deep_merge
        from gui.services.options import config as options_config

        deep_merge(section, options)
        self.finish(section)
        try:
            options_config.parse(root)
        except options_config.OptionsConfigError as exc:
            return [f"Options: {exc}"]
        return []
