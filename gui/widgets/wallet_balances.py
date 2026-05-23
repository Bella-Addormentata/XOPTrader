"""Wallet balances page widget for XOPTrader GUI.

Displays per-wallet balance information (spendable, confirmed, pending,
unconfirmed) fetched from the Chia wallet RPC.  Data is pushed via the
``update_balances`` slot which the ``MainWindow`` calls on every bridge
refresh tick.

ISO/IEC 27001:2022 -- no credentials stored or displayed.
ISO/IEC 5055      -- all public APIs carry type hints and docstrings.
ISO/IEC 25000     -- degrades gracefully on empty or partial data.
"""

from __future__ import annotations

import json
import logging
from typing import Any, Final, Optional

from PySide6.QtCore import QSettings, Qt, Signal, Slot
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

# ---------------------------------------------------------------------------
# Palette aliases
# ---------------------------------------------------------------------------
PRIMARY_GREEN: Final[str] = _C.PRIMARY_GREEN
LIGHT_GREEN: Final[str] = _C.LIGHT_GREEN
DARK_BG: Final[str] = _C.DARK_BG
PANEL_BG: Final[str] = _C.PANEL_BG
ELEVATED_BG: Final[str] = _C.ELEVATED_BG
BORDER: Final[str] = _C.BORDER
TEXT_PRIMARY: Final[str] = _C.TEXT_PRIMARY
TEXT_SECONDARY: Final[str] = _C.TEXT_SECONDARY
PROFIT_GREEN: Final[str] = _C.PROFIT_GREEN
LOSS_RED: Final[str] = _C.LOSS_RED
WARNING: Final[str] = _C.WARNING_YELLOW
INFO: Final[str] = _C.INFO_BLUE

_MONO: Final[str] = "Consolas, 'Courier New', monospace"

# Table column headers.
_COLUMNS: Final[list[str]] = [
    "Wallet",
    "Spendable",
    "Confirmed",
    "Pending",
    "Unconfirmed",
    "Reserve %",
]

# Well-known wallet type names.
_WALLET_TYPES: Final[dict[int, str]] = {
    0: "XCH",
    6: "CAT",
}


def _is_stablecoin_symbol(symbol: str) -> bool:
    normalized = symbol.strip().upper()
    return normalized in {"WUSDC.B", "WUSDC", "USDC", "USDS", "USDT"}


def _split_pair(pair_name: str) -> tuple[str, str]:
    if "/" not in pair_name:
        return pair_name.strip(), ""
    base, quote = pair_name.split("/", 1)
    return base.strip(), quote.strip()


# QSettings identity for persisted GUI state.  Must match the
# (org, app) tuple used elsewhere in the GUI so all preferences live
# in one backing store.
_QSETTINGS_ORG: Final[str] = "XOP"
_QSETTINGS_APP: Final[str] = "XOPTrader"
_ALLOC_TARGETS_KEY: Final[str] = "wallet/allocation_targets"
_ALLOC_TOLERANCES_KEY: Final[str] = "wallet/allocation_tolerances"

_log = logging.getLogger(__name__)


class WalletBalancesWidget(QWidget):
    """Full-page widget showing wallet balances in a styled table.

    Parameters
    ----------
    parent : QWidget | None
        Optional parent widget.
    """

    allocation_targets_applied = Signal(dict, dict, dict, dict)

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._reserve: dict[str, float] = {}
        self._stuck_offers: int = 0
        self._last_balances: dict[str, dict[str, float]] = {}
        self._last_market_data: dict[str, dict[str, float]] = {}
        # Pair configs from config.yaml (each with at least "name" and
        # "enabled" keys).  Used to restrict the Target Allocation table
        # to assets that participate in at least one enabled pair.
        self._last_pairs_cfg: list[dict[str, Any]] = []
        self._target_allocations: dict[str, float] = {}
        self._target_tolerances: dict[str, float] = {}
        self._alloc_updating = False
        # Restore persisted targets before the UI is built so the first
        # render already has them.
        self._load_target_allocations()
        self._load_target_tolerances()
        self._build_ui()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(16, 16, 16, 16)
        root.setSpacing(12)

        # -- Header --
        header = QLabel("Wallet Balances")
        header.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 18px; font-weight: bold;"
        )
        root.addWidget(header)

        desc = QLabel(
            "Live balances queried from the Chia wallet RPC.  "
            "Updates every ~30 seconds."
        )
        desc.setWordWrap(True)
        desc.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 12px;")
        root.addWidget(desc)

        # -- Summary cards row --
        self._summary_frame = QFrame()
        self._summary_frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 12px; }}"
        )
        summary_layout = QHBoxLayout(self._summary_frame)
        summary_layout.setSpacing(24)

        self._total_spendable_label = self._make_summary_card(
            "Total Spendable", "—", summary_layout
        )
        self._total_confirmed_label = self._make_summary_card(
            "Total Confirmed", "—", summary_layout
        )
        self._total_pending_label = self._make_summary_card(
            "Total Pending", "—", summary_layout
        )
        self._wallet_count_label = self._make_summary_card(
            "Wallets", "0", summary_layout
        )
        root.addWidget(self._summary_frame)

        # -- Stuck offers warning --
        self._stuck_label = QLabel()
        self._stuck_label.setStyleSheet(
            f"color: {LOSS_RED}; font-size: 12px; font-weight: bold;"
        )
        self._stuck_label.setVisible(False)
        root.addWidget(self._stuck_label)

        # -- Balance table --
        self._table = QTableWidget(0, len(_COLUMNS))
        self._table.setHorizontalHeaderLabels(_COLUMNS)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.setAlternatingRowColors(True)
        self._table.verticalHeader().setVisible(False)
        self._table.horizontalHeader().setStretchLastSection(True)
        self._table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )

        self._table.setStyleSheet(
            f"""
            QTableWidget {{
                background-color: {DARK_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 4px;
                gridline-color: {BORDER};
                font-family: {_MONO};
                font-size: 12px;
            }}
            QTableWidget::item {{
                padding: 6px 10px;
            }}
            QTableWidget::item:alternate {{
                background-color: {PANEL_BG};
            }}
            QHeaderView::section {{
                background-color: {ELEVATED_BG};
                color: {TEXT_SECONDARY};
                border: 1px solid {BORDER};
                padding: 6px 10px;
                font-size: 11px;
                font-weight: bold;
            }}
            """
        )
        root.addWidget(self._table, stretch=1)

        # -- Target allocation panel --
        self._alloc_frame = QFrame()
        self._alloc_frame.setStyleSheet(
            f"QFrame {{ background: {PANEL_BG}; border: 1px solid {BORDER}; "
            f"border-radius: 8px; padding: 10px; }}"
        )
        alloc_layout = QVBoxLayout(self._alloc_frame)
        alloc_layout.setSpacing(8)

        alloc_header = QLabel("Target Portfolio Allocation")
        alloc_header.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 14px; font-weight: bold;"
        )
        alloc_layout.addWidget(alloc_header)

        alloc_desc = QLabel(
            "Set target percentages by asset (confirmed value basis). "
            "Targets should sum to 100%."
        )
        alloc_desc.setWordWrap(True)
        alloc_desc.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 11px;")
        alloc_layout.addWidget(alloc_desc)

        self._alloc_table = QTableWidget(0, 5)
        self._alloc_table.setHorizontalHeaderLabels(
            ["Asset", "Current %", "Target %", "Target % +/-", "Delta %"]
        )
        self._alloc_table.setSelectionBehavior(
            QTableWidget.SelectionBehavior.SelectRows
        )
        self._alloc_table.verticalHeader().setVisible(False)
        self._alloc_table.horizontalHeader().setStretchLastSection(True)
        self._alloc_table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch
        )
        self._alloc_table.setStyleSheet(
            f"""
            QTableWidget {{
                background-color: {DARK_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 4px;
                gridline-color: {BORDER};
                font-family: {_MONO};
                font-size: 12px;
            }}
            QHeaderView::section {{
                background-color: {ELEVATED_BG};
                color: {TEXT_SECONDARY};
                border: 1px solid {BORDER};
                padding: 4px 8px;
                font-size: 11px;
                font-weight: bold;
            }}
            """
        )
        self._alloc_table.cellChanged.connect(self._on_alloc_cell_changed)
        alloc_layout.addWidget(self._alloc_table)

        alloc_controls = QHBoxLayout()
        self._alloc_sum_label = QLabel("Target sum: —")
        self._alloc_sum_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        self._alloc_hint_label = QLabel("")
        self._alloc_hint_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        normalize_btn = QPushButton("Normalize Targets to 100%")
        normalize_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {ELEVATED_BG};
                color: {TEXT_PRIMARY};
                border: 1px solid {BORDER};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 11px;
            }}
            QPushButton:hover {{
                border-color: {PRIMARY_GREEN};
            }}
            """
        )
        normalize_btn.clicked.connect(self._normalize_targets)
        self._apply_alloc_btn = QPushButton("Apply Targets To Strategy")
        self._apply_alloc_btn.setStyleSheet(
            f"""
            QPushButton {{
                background-color: {PRIMARY_GREEN};
                color: {DARK_BG};
                border: 1px solid {PRIMARY_GREEN};
                border-radius: 4px;
                padding: 4px 8px;
                font-size: 11px;
                font-weight: bold;
            }}
            QPushButton:hover {{
                background-color: {LIGHT_GREEN};
                border-color: {LIGHT_GREEN};
            }}
            """
        )
        self._apply_alloc_btn.clicked.connect(self._apply_targets)
        alloc_controls.addWidget(self._alloc_sum_label)
        alloc_controls.addWidget(self._alloc_hint_label, stretch=1)
        alloc_controls.addWidget(normalize_btn)
        alloc_controls.addWidget(self._apply_alloc_btn)
        alloc_layout.addLayout(alloc_controls)

        root.addWidget(self._alloc_frame)

        # -- Status label --
        self._status_label = QLabel("Waiting for wallet data…")
        self._status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px;"
        )
        root.addWidget(self._status_label)

    def _make_summary_card(
        self, title: str, value: str, layout: QHBoxLayout
    ) -> QLabel:
        """Create a small summary card with a title and value label."""
        card = QVBoxLayout()
        card.setSpacing(2)
        title_lbl = QLabel(title)
        title_lbl.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 10px; border: none;"
        )
        title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        card.addWidget(title_lbl)
        value_lbl = QLabel(value)
        value_lbl.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 16px; font-weight: bold; "
            f"font-family: {_MONO}; border: none;"
        )
        value_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        card.addWidget(value_lbl)
        layout.addLayout(card)
        return value_lbl

    @Slot(dict)
    def update_balances(
        self,
        balances: dict[str, dict[str, float]],
        reserve: dict[str, float] | None = None,
        market_data: dict[str, dict[str, float]] | None = None,
        stuck_offers: int = 0,
        pairs: list[dict[str, Any]] | None = None,
    ) -> None:
        """Update the widget with fresh wallet balance data.

        Parameters
        ----------
        balances:
            Mapping of wallet name to balance dict with keys
            ``spendable``, ``confirmed``, ``pending_change``,
            ``unconfirmed``, ``wallet_type``.
        reserve:
            Optional mapping of wallet name to spendable reserve ratio (0–1).
        stuck_offers:
            Number of stuck offers beyond TTL.
        """
        self._reserve = reserve or {}
        self._stuck_offers = stuck_offers
        self._last_balances = dict(balances or {})
        self._last_market_data = dict(market_data or {})
        if pairs is not None:
            self._last_pairs_cfg = list(pairs)

        if not balances:
            self._status_label.setText(
                "No wallet data — check Chia wallet connection"
            )
            return

        self._table.setRowCount(len(balances))

        total_spendable = 0.0
        total_confirmed = 0.0
        total_pending = 0.0

        for row, (wallet_name, bal) in enumerate(sorted(balances.items())):
            spendable = bal.get("spendable", 0.0)
            confirmed = bal.get("confirmed", 0.0)
            pending = bal.get("pending_change", 0.0)
            unconfirmed = bal.get("unconfirmed", 0.0)
            wallet_type = int(bal.get("wallet_type", 0))
            res_pct = self._reserve.get(wallet_name, -1.0)

            # Determine display suffix.
            type_name = _WALLET_TYPES.get(wallet_type, "Token")
            display_name = f"{wallet_name}  ({type_name})"

            # Reserve colour coding.
            if res_pct >= 0:
                res_display = f"{res_pct * 100:.1f}%"
                if res_pct < 0.10:
                    res_color = QColor(LOSS_RED)
                elif res_pct < 0.25:
                    res_color = QColor(WARNING)
                else:
                    res_color = QColor(PROFIT_GREEN)
            else:
                res_display = "—"
                res_color = QColor(TEXT_SECONDARY)

            # Only accumulate XCH for totals (avoid mixing units).
            if wallet_type == 0:
                total_spendable += spendable
                total_confirmed += confirmed
                total_pending += pending

            # Populate row.
            items: list[tuple[str, QColor | None]] = [
                (display_name, None),
                (self._fmt(spendable), None),
                (self._fmt(confirmed), None),
                (self._fmt(pending), QColor(WARNING) if pending != 0 else None),
                (self._fmt(unconfirmed), None),
                (res_display, res_color),
            ]

            for col, (text, color) in enumerate(items):
                item = QTableWidgetItem(text)
                item.setTextAlignment(
                    Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
                    if col > 0
                    else Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
                )
                if color:
                    item.setForeground(color)
                self._table.setItem(row, col, item)

        # Update summary cards.
        self._total_spendable_label.setText(f"{total_spendable:,.6f} XCH")
        self._total_confirmed_label.setText(f"{total_confirmed:,.6f} XCH")
        self._total_pending_label.setText(f"{total_pending:,.6f} XCH")
        self._wallet_count_label.setText(str(len(balances)))

        # Stuck offers warning.
        if stuck_offers > 0:
            self._stuck_label.setText(
                f"\u26a0 {stuck_offers} stuck offer(s) beyond TTL — "
                f"check fee levels"
            )
            self._stuck_label.setVisible(True)
        else:
            self._stuck_label.setVisible(False)

        self._status_label.setText(
            f"Last update: {len(balances)} wallet(s) loaded"
        )
        self._refresh_allocation_table()

    def _asset_prices_usdc(self) -> dict[str, float]:
        prices: dict[str, float] = {}

        # Seed stablecoin anchors at 1 USDC.  Pull from BOTH pair names
        # and wallet names so a stablecoin wallet still gets priced even
        # if no pair references it directly.
        for pair_name in self._last_market_data:
            base_asset, quote_asset = _split_pair(pair_name)
            if _is_stablecoin_symbol(base_asset):
                prices[base_asset.upper()] = 1.0
            if _is_stablecoin_symbol(quote_asset):
                prices[quote_asset.upper()] = 1.0
        for wallet_name in self._last_balances:
            normalized = wallet_name.strip().upper()
            compact = normalized.replace(" ", "")
            if _is_stablecoin_symbol(normalized):
                prices.setdefault(normalized, 1.0)
            elif _is_stablecoin_symbol(compact):
                prices.setdefault(compact, 1.0)

        # Iteratively propagate price graph from pair mids.
        for _ in range(4):
            changed = False
            for pair_name, md in self._last_market_data.items():
                mid_price = float(md.get("mid_price", 0.0) or 0.0)
                if mid_price <= 0.0:
                    continue
                base_asset, quote_asset = _split_pair(pair_name)
                base_key = base_asset.upper()
                quote_key = quote_asset.upper()

                # The engine emits `mid_price` as `display_price * 1e12`
                # for every pair regardless of which side is XCH (see
                # cpp/src/execution/market_data.cpp publish_snapshot —
                # to_mojos(price) := round(price * kMojosPerXch)).  The
                # "display price" is quote-units per base-unit, so
                # dividing by 1e12 always yields the human-readable
                # ratio.  Using the quote asset's mojo divisor here (as
                # earlier code did) was correct for raw on-chain
                # `quote_mojos/base_mojo` prices but is wrong for the
                # engine's already-scaled metric and inflates non-XCH
                # asset prices by ~1e9.
                price_in_quote_units = mid_price / 1_000_000_000_000.0
                if price_in_quote_units <= 0.0:
                    continue

                if quote_key in prices and base_key not in prices:
                    prices[base_key] = price_in_quote_units * prices[quote_key]
                    changed = True
                if base_key in prices and quote_key not in prices:
                    prices[quote_key] = prices[base_key] / price_in_quote_units
                    changed = True
            if not changed:
                break

        return prices

    def _asset_id_symbol_map(self) -> dict[str, str]:
        """Build a mapping of CAT asset_id (hex) -> base symbol from pairs.

        Uses ``base_asset_id`` / ``quote_asset_id`` paired with the
        ``name`` field (``BASE/QUOTE``) of every configured pair so we
        can resolve a wallet's symbol from its on-chain asset id, even
        when the user has left the wallet's display name as Chia's
        default ``CAT abcd...`` placeholder.
        """
        mapping: dict[str, str] = {}
        for pair_cfg in self._last_pairs_cfg:
            pair_name = str(pair_cfg.get("name", ""))
            if not pair_name:
                continue
            base_sym, quote_sym = _split_pair(pair_name)
            base_id = str(pair_cfg.get("base_asset_id", "") or "").lower()
            quote_id = str(pair_cfg.get("quote_asset_id", "") or "").lower()
            if base_id and base_id != "xch" and base_sym:
                mapping[base_id] = base_sym.upper()
            if quote_id and quote_id != "xch" and quote_sym:
                mapping[quote_id] = quote_sym.upper()
        return mapping

    def _wallet_asset_symbol(
        self,
        wallet_name: str,
        wallet_data: dict[str, float],
        asset_prices: dict[str, float],
        asset_id_map: dict[str, str] | None = None,
    ) -> Optional[str]:
        if int(wallet_data.get("wallet_type", -1)) == 0:
            return "XCH"

        # Prefer on-chain asset_id lookup so the user's wallet display
        # name (e.g. Chia's default "CAT abcd..." placeholder) doesn't
        # matter.  Falls through to name-based matching if the wallet
        # has no asset_id or the id isn't in any configured pair.
        if asset_id_map:
            wallet_asset_id = str(wallet_data.get("asset_id", "") or "").lower()
            if wallet_asset_id and wallet_asset_id in asset_id_map:
                return asset_id_map[wallet_asset_id]

        normalized = wallet_name.strip().upper()
        compact = normalized.replace(" ", "")

        # Each stablecoin is its own asset — never merge wUSDC.b with
        # wUSDC, USDS, etc.  Return the wallet's own normalized symbol
        # so multi-stablecoin wallets stay in separate allocation rows.
        if _is_stablecoin_symbol(normalized):
            return normalized
        if _is_stablecoin_symbol(compact):
            return compact

        if normalized in asset_prices:
            return normalized
        if compact in asset_prices:
            return compact
        for candidate in asset_prices:
            # Skip stablecoin candidates in the substring fallback to
            # avoid pulling unrelated USDC-named wallets into one bucket.
            if _is_stablecoin_symbol(candidate):
                continue
            if candidate in compact or compact in candidate:
                return candidate
        return None

    def _refresh_allocation_table(self) -> None:
        asset_prices = self._asset_prices_usdc()
        current_values: dict[str, float] = {}
        # One-shot diagnostic so we can see in the GUI log why the
        # Target Allocation panel chose the symbols it did.  Logged at
        # INFO only when the resolved set materially changes between
        # refreshes (avoids per-tick spam).
        _diag_snapshot: dict[str, str] = {}
        # Track every recognized-asset wallet so we can still list assets
        # that have no priced market data yet (e.g. XCH right after engine
        # restart, before XCH/wUSDC.b has published a mid_price).  Without
        # this, XCH would silently vanish from the Target Allocation table
        # whenever its price graph hadn't resolved.
        all_assets: set[str] = set()

        # Build the set of assets that participate in at least one
        # enabled trading pair.  Assets only present in disabled pairs
        # (or in wallets not traded at all) are hidden from the
        # allocation table so the user only manages targets for assets
        # the bot is actually trading.  When pairs config hasn't
        # arrived yet (empty set), fall through to the legacy
        # "show everything" behaviour so startup isn't blank.
        enabled_assets: set[str] = set()
        for pair_cfg in self._last_pairs_cfg:
            if not pair_cfg.get("enabled", True):
                continue
            pair_name = str(pair_cfg.get("name", ""))
            if not pair_name:
                continue
            base_asset, quote_asset = _split_pair(pair_name)
            if base_asset:
                enabled_assets.add(base_asset.upper())
            if quote_asset:
                enabled_assets.add(quote_asset.upper())

        asset_id_map = self._asset_id_symbol_map()

        for wallet_name, wallet_data in self._last_balances.items():
            symbol = self._wallet_asset_symbol(
                wallet_name, wallet_data, asset_prices, asset_id_map
            )
            _diag_snapshot[wallet_name] = (
                f"type={int(wallet_data.get('wallet_type', -1))} "
                f"aid={str(wallet_data.get('asset_id','') or '')[:10]} "
                f"sym={symbol} "
                f"price={asset_prices.get((symbol or '').upper(), 0.0)}"
            )
            if not symbol:
                _log.debug(
                    "Allocation: skipping unmatched wallet %r "
                    "(type=%s, asset_id=%r)",
                    wallet_name,
                    wallet_data.get("wallet_type"),
                    wallet_data.get("asset_id", ""),
                )
                continue
            asset_key = symbol.upper()
            if enabled_assets and asset_key not in enabled_assets:
                _log.debug(
                    "Allocation: wallet %r resolved to %s but not in "
                    "enabled pairs %s",
                    wallet_name, asset_key, sorted(enabled_assets),
                )
                continue
            all_assets.add(asset_key)
            price = asset_prices.get(asset_key, asset_prices.get(symbol, 0.0))
            if price <= 0.0:
                _log.debug(
                    "Allocation: wallet %r (%s) has no price yet "
                    "(known prices: %s)",
                    wallet_name, asset_key, sorted(asset_prices),
                )
                continue
            confirmed = float(wallet_data.get("confirmed", 0.0) or 0.0)
            current_values[asset_key] = (
                current_values.get(asset_key, 0.0) + confirmed * price
            )

        # Always surface every enabled-pair asset, even if the user has
        # no wallet for it yet.  This lets them pre-set a target % for
        # an asset they intend to start holding (or for one whose wallet
        # hasn't been auto-created yet).  Rows with no balance show 0%
        # current and "—" delta until a wallet appears.
        for asset_key in enabled_assets:
            all_assets.add(asset_key)

        # One-shot diagnostic emission.  Logged at INFO only when the
        # resolved snapshot changes so we get a single visible line per
        # state change, instead of one per refresh tick.
        diag_key = (
            tuple(sorted(_diag_snapshot.items())),
            tuple(sorted(enabled_assets)),
            tuple(sorted(asset_id_map.items())),
            tuple(sorted((k, round(v, 4)) for k, v in current_values.items())),
        )
        if diag_key != getattr(self, "_alloc_diag_last", None):
            self._alloc_diag_last = diag_key
            _log.info(
                "Allocation diag | enabled=%s | asset_id_map=%s | "
                "wallets=%s | current_values=%s",
                sorted(enabled_assets),
                asset_id_map,
                _diag_snapshot,
                {k: round(v, 4) for k, v in current_values.items()},
            )

        if not all_assets:
            self._alloc_table.setRowCount(0)
            self._alloc_sum_label.setText("Target sum: —")
            self._alloc_hint_label.setText("No wallets detected yet")
            return

        total_value = sum(current_values.values())

        self._alloc_updating = True
        assets = sorted(all_assets)
        self._alloc_table.setRowCount(len(assets))

        for row, asset in enumerate(assets):
            priced = asset in current_values and total_value > 0.0
            if priced:
                current_pct = (current_values[asset] / total_value) * 100.0
                current_text = f"{current_pct:.2f}%"
            else:
                current_pct = 0.0
                current_text = "—"

            if asset not in self._target_allocations:
                # Seed target from current % when priced, otherwise 0 so
                # the user can dial it in manually.
                self._target_allocations[asset] = current_pct if priced else 0.0
            target_pct = self._target_allocations.get(asset, 0.0)
            tolerance_pct = self._target_tolerances.get(asset, 0.0)

            if priced:
                delta_pct = target_pct - current_pct
                # Mark the delta as "within tolerance" when |delta| <= tolerance.
                if tolerance_pct > 0.0 and abs(delta_pct) <= tolerance_pct:
                    delta_text = f"{delta_pct:+.2f}% (in band)"
                else:
                    delta_text = f"{delta_pct:+.2f}%"
            else:
                delta_pct = 0.0
                delta_text = "—"

            cols = [
                QTableWidgetItem(asset),
                QTableWidgetItem(current_text),
                QTableWidgetItem(f"{target_pct:.2f}"),
                QTableWidgetItem(f"{tolerance_pct:.2f}"),
                QTableWidgetItem(delta_text),
            ]

            cols[0].setFlags(cols[0].flags() & ~Qt.ItemFlag.ItemIsEditable)
            cols[1].setFlags(cols[1].flags() & ~Qt.ItemFlag.ItemIsEditable)
            cols[4].setFlags(cols[4].flags() & ~Qt.ItemFlag.ItemIsEditable)

            cols[0].setTextAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            for idx in (1, 2, 3, 4):
                cols[idx].setTextAlignment(
                    Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
                )

            if not priced:
                cols[4].setForeground(QColor(TEXT_SECONDARY))
            elif tolerance_pct > 0.0 and abs(delta_pct) <= tolerance_pct:
                cols[4].setForeground(QColor(PROFIT_GREEN))
            elif abs(delta_pct) > 5.0:
                cols[4].setForeground(QColor(WARNING))
            else:
                cols[4].setForeground(QColor(PROFIT_GREEN))

            for col, item in enumerate(cols):
                self._alloc_table.setItem(row, col, item)

        self._alloc_updating = False
        self._update_allocation_sum_status()
        if total_value <= 0.0:
            self._alloc_hint_label.setText(
                "Waiting for market data — set targets now; current % will "
                "fill in once mid prices arrive"
            )

        # Surface the diagnostic snapshot directly in the panel so it's
        # visible without the GUI log file (pythonw.exe drops stderr).
        # Tooltip on the hint label shows the full per-wallet resolution
        # for easy copy/paste when debugging Target Allocation issues.
        diag_lines = [
            f"enabled_pairs={sorted(enabled_assets)}",
            f"asset_id_map={ {k[:8]+'..': v for k, v in asset_id_map.items()} }",
            "wallets:",
        ]
        for wname, info in _diag_snapshot.items():
            diag_lines.append(f"  {wname!r}: {info}")
        diag_lines.append(
            f"current_values={ {k: round(v, 2) for k, v in current_values.items()} }"
        )
        self._alloc_hint_label.setToolTip("\n".join(diag_lines))

    @Slot(int, int)
    def _on_alloc_cell_changed(self, row: int, col: int) -> None:
        if self._alloc_updating or col not in (2, 3):
            return
        asset_item = self._alloc_table.item(row, 0)
        edited_item = self._alloc_table.item(row, col)
        if asset_item is None or edited_item is None:
            return

        asset = asset_item.text().strip().upper()
        raw_text = edited_item.text().replace("%", "").strip()
        if col == 2:
            try:
                value = float(raw_text)
            except ValueError:
                value = self._target_allocations.get(asset, 0.0)
            value = max(0.0, min(100.0, value))
            self._target_allocations[asset] = value
            self._save_target_allocations()
        else:  # col == 3 -- tolerance
            try:
                value = float(raw_text)
            except ValueError:
                value = self._target_tolerances.get(asset, 0.0)
            # Tolerance is a percentage-point half-window; clamp to [0, 50].
            value = max(0.0, min(50.0, value))
            self._target_tolerances[asset] = value
            self._save_target_tolerances()
        self._refresh_allocation_table()

    def _update_allocation_sum_status(self) -> None:
        target_sum = sum(self._target_allocations.values())
        self._alloc_sum_label.setText(f"Target sum: {target_sum:.2f}%")
        if abs(target_sum - 100.0) <= 0.5:
            self._alloc_sum_label.setStyleSheet(
                f"color: {PROFIT_GREEN}; font-size: 11px; font-weight: bold;"
            )
            self._alloc_hint_label.setText("Allocation is balanced")
            self._alloc_hint_label.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 11px;"
            )
        else:
            self._alloc_sum_label.setStyleSheet(
                f"color: {WARNING}; font-size: 11px; font-weight: bold;"
            )
            diff = 100.0 - target_sum
            if diff > 0:
                self._alloc_hint_label.setText(f"Add {diff:.2f}% to reach 100%")
            else:
                self._alloc_hint_label.setText(f"Reduce {-diff:.2f}% to reach 100%")
            self._alloc_hint_label.setStyleSheet(
                f"color: {WARNING}; font-size: 11px;"
            )

    @Slot()
    def _normalize_targets(self) -> None:
        target_sum = sum(self._target_allocations.values())
        if target_sum <= 0.0:
            return
        scale = 100.0 / target_sum
        for asset in list(self._target_allocations.keys()):
            self._target_allocations[asset] = max(
                0.0, min(100.0, self._target_allocations[asset] * scale)
            )
        self._refresh_allocation_table()

    def _pair_ratio_targets_from_allocations(self) -> dict[str, float]:
        pair_targets: dict[str, float] = {}
        for pair_name in self._last_market_data:
            base_asset, quote_asset = _split_pair(pair_name)
            base_key = base_asset.upper()
            quote_key = quote_asset.upper()
            base_pct = self._target_allocations.get(base_key)
            quote_pct = self._target_allocations.get(quote_key)
            if base_pct is None or quote_pct is None:
                continue
            denom = base_pct + quote_pct
            if denom <= 0.0:
                continue
            pair_targets[pair_name] = base_pct / denom
        return pair_targets

    def _pair_band_enter_from_tolerances(self) -> dict[str, float]:
        """Map asset-level "Target % +/-" inputs to a per-pair ratio band.

        For each pair, project the asset tolerances into ratio space by
        evaluating the pair's target ratio at (base+tol)/(quote-tol) and
        (base-tol)/(quote+tol), then taking the widest deviation from
        the nominal target.  The resulting fraction is clamped to (0, 0.49)
        so the C++ side accepts it as a valid enter band.
        """
        bands: dict[str, float] = {}
        for pair_name in self._last_market_data:
            base_asset, quote_asset = _split_pair(pair_name)
            base_key = base_asset.upper()
            quote_key = quote_asset.upper()
            base_pct = self._target_allocations.get(base_key)
            quote_pct = self._target_allocations.get(quote_key)
            if base_pct is None or quote_pct is None:
                continue
            denom = base_pct + quote_pct
            if denom <= 0.0:
                continue
            base_tol = self._target_tolerances.get(base_key, 0.0)
            quote_tol = self._target_tolerances.get(quote_key, 0.0)
            if base_tol <= 0.0 and quote_tol <= 0.0:
                continue
            target_ratio = base_pct / denom

            def _ratio(b: float, q: float) -> float:
                b = max(0.0, b)
                q = max(0.0, q)
                d = b + q
                return (b / d) if d > 0.0 else target_ratio

            upper_r = _ratio(base_pct + base_tol, quote_pct - quote_tol)
            lower_r = _ratio(base_pct - base_tol, quote_pct + quote_tol)
            band = max(abs(upper_r - target_ratio),
                       abs(target_ratio - lower_r))
            band = max(0.0, min(0.49, band))
            if band > 0.0:
                bands[pair_name] = band
        return bands

    @Slot()
    def _apply_targets(self) -> None:
        if not self._target_allocations:
            self._alloc_hint_label.setText("No target allocations to apply")
            self._alloc_hint_label.setStyleSheet(
                f"color: {WARNING}; font-size: 11px;"
            )
            return
        asset_targets = {
            key: float(value) / 100.0
            for key, value in self._target_allocations.items()
        }
        pair_targets = self._pair_ratio_targets_from_allocations()
        asset_tolerances = {
            key: float(value) / 100.0
            for key, value in self._target_tolerances.items()
            if float(value) > 0.0
        }
        pair_band_enters = self._pair_band_enter_from_tolerances()
        self.allocation_targets_applied.emit(
            asset_targets, pair_targets, asset_tolerances, pair_band_enters
        )
        self._alloc_hint_label.setText(
            f"Applied {len(asset_targets)} asset target(s), "
            f"{len(pair_targets)} pair ratio target(s), "
            f"{len(pair_band_enters)} pair band override(s)"
        )
        self._alloc_hint_label.setStyleSheet(
            f"color: {INFO}; font-size: 11px;"
        )

    @staticmethod
    def _fmt(value: float) -> str:
        """Format a balance value for display."""
        if abs(value) < 0.000001:
            return "0"
        return f"{value:,.6f}"

    # ------------------------------------------------------------------
    # Persistence helpers
    # ------------------------------------------------------------------

    def _load_target_allocations(self) -> None:
        """Restore previously persisted target allocations from QSettings."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            raw = settings.value(_ALLOC_TARGETS_KEY, "", type=str)
            if not raw:
                return
            data = json.loads(raw)
            if not isinstance(data, dict):
                return
            restored: dict[str, float] = {}
            for key, value in data.items():
                try:
                    restored[str(key).upper()] = max(
                        0.0, min(100.0, float(value))
                    )
                except (TypeError, ValueError):
                    continue
            self._target_allocations.update(restored)
        except (ValueError, OSError) as exc:
            _log.warning("Failed to load allocation targets: %s", exc)

    def _save_target_allocations(self) -> None:
        """Persist target allocations to QSettings as a JSON blob."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            payload = json.dumps(
                {k: float(v) for k, v in self._target_allocations.items()},
                sort_keys=True,
            )
            settings.setValue(_ALLOC_TARGETS_KEY, payload)
        except (TypeError, ValueError, OSError) as exc:
            _log.warning("Failed to save allocation targets: %s", exc)

    def _load_target_tolerances(self) -> None:
        """Restore previously persisted target tolerances from QSettings."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            raw = settings.value(_ALLOC_TOLERANCES_KEY, "", type=str)
            if not raw:
                return
            data = json.loads(raw)
            if not isinstance(data, dict):
                return
            restored: dict[str, float] = {}
            for key, value in data.items():
                try:
                    restored[str(key).upper()] = max(
                        0.0, min(50.0, float(value))
                    )
                except (TypeError, ValueError):
                    continue
            self._target_tolerances.update(restored)
        except (ValueError, OSError) as exc:
            _log.warning("Failed to load allocation tolerances: %s", exc)

    def _save_target_tolerances(self) -> None:
        """Persist target tolerances to QSettings as a JSON blob."""
        try:
            settings = QSettings(_QSETTINGS_ORG, _QSETTINGS_APP)
            payload = json.dumps(
                {k: float(v) for k, v in self._target_tolerances.items()},
                sort_keys=True,
            )
            settings.setValue(_ALLOC_TOLERANCES_KEY, payload)
        except (TypeError, ValueError, OSError) as exc:
            _log.warning("Failed to save allocation tolerances: %s", exc)

    def clear(self) -> None:
        """Reset the widget to its initial empty state."""
        self._table.setRowCount(0)
        self._alloc_table.setRowCount(0)
        self._target_allocations.clear()
        self._target_tolerances.clear()
        self._last_balances = {}
        self._last_market_data = {}
        self._alloc_sum_label.setText("Target sum: —")
        self._alloc_hint_label.setText("")
        self._total_spendable_label.setText("—")
        self._total_confirmed_label.setText("—")
        self._total_pending_label.setText("—")
        self._wallet_count_label.setText("0")
        self._stuck_label.setVisible(False)
        self._status_label.setText("Waiting for wallet data…")
