"""Startup market analysis display widget for XOPTrader GUI.

Shows the real-time progress of the startup market analysis phase and the
collected statistics (volatility, spread distribution, regime signal, order-book
imbalance, momentum, and recommended trading aggressiveness) for each
configured trading pair.

All data flows in via the public ``update_analysis`` method which the
``MainWindow`` calls on every bridge refresh tick whenever the bot is in the
``Analyzing`` state.

Scholarly basis for the displayed metrics
==========================================
- Ho & Stoll (1981) — observation-before-action principle.
- Madhavan & Smidt (1993) — market-open observation period.
- Easley, Lopez de Prado & O'Hara (2012) — VPIN burn-in requirement.
- Lo & MacKinlay (1988) — variance ratio regime test.
- Glosten & Milgrom (1985) — adverse-selection spread component.

ISO/IEC 27001:2022 -- no credentials or secrets are stored or displayed.
ISO/IEC 5055      -- all public APIs carry type hints and docstrings.
ISO/IEC 25000     -- widget degrades gracefully on empty or partial data.
"""

from __future__ import annotations

from typing import Any, Optional

from PySide6.QtCore import Qt, Slot
from PySide6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QProgressBar,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from gui.theme import COLORS as _C

# ---------------------------------------------------------------------------
# Palette aliases
# ---------------------------------------------------------------------------
PRIMARY_GREEN  = _C.PRIMARY_GREEN
LIGHT_GREEN    = _C.LIGHT_GREEN
DARK_BG        = _C.DARK_BG
PANEL_BG       = _C.PANEL_BG
ELEVATED_BG    = _C.ELEVATED_BG
BORDER         = _C.BORDER
TEXT_PRIMARY   = _C.TEXT_PRIMARY
TEXT_SECONDARY = _C.TEXT_SECONDARY
PROFIT_GREEN   = _C.PROFIT_GREEN
LOSS_RED       = _C.LOSS_RED
WARNING        = _C.WARNING_YELLOW
INFO           = _C.INFO_BLUE
# [S42] Used for values that are CONTEXT rather than live state (the frozen
# startup recommendation) and for "no data" placeholders.  Rendering either of
# those in a live status colour is what made a stale, inverted reading look
# authoritative.
TEXT_MUTED     = _C.TEXT_DISABLED

# Regime display names and colours.
_REGIME_LABELS = {0: "Mean-Reverting", 1: "Random Walk", 2: "Momentum"}
_REGIME_COLORS = {0: PROFIT_GREEN, 1: INFO, 2: WARNING}

# Aggressiveness display names and colours.
_AGG_LABELS = {0: "Conservative", 1: "Normal", 2: "Aggressive"}
_AGG_COLORS = {0: WARNING, 1: INFO, 2: PROFIT_GREEN}

# Monospaced font for numeric readouts.
_MONO = "Consolas, 'Courier New', monospace"


def format_comp_gate(base: int, offset: int, effective: int) -> str:
    """Render the competitiveness gate as ``base(+/-)offset -> effective``.

    Module-level and pure so it can be tested without a QApplication.

    THE SEPARATOR MUST NEVER VANISH.  This was written inline as
    ``sign = "+" if offset > 0 else ""`` followed by ``f"{base}{sign}{offset}"``
    which, at the NEUTRAL offset of 0, concatenated the two numbers: base 3 /
    offset 0 rendered as ``"30 -> 3"`` and the operator read the gate base as
    thirty on a 0-10 scale.  Offset 0 is the most common state -- the
    post-creation warm-up window sits there, as does any block where
    ema_fill_rate is at target -- and it was the one case the display could
    not express.  It went unnoticed because the live controller happened to be
    railed at -3 throughout, which renders correctly.
    """
    sign = "+" if offset >= 0 else "-"
    tail = " (open)" if effective == 0 else ""
    return f"{base}{sign}{abs(offset)} -> {effective}{tail}"


# ---------------------------------------------------------------------------
# _StatRow -- a single labelled stat row in the analysis panel
# ---------------------------------------------------------------------------

class _StatRow(QWidget):
    """A horizontal label-value pair for displaying a single statistic."""

    def __init__(self, label: str, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(12)

        self._label = QLabel(label + ":")
        self._label.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 13px;")
        self._label.setFixedWidth(190)

        self._value = QLabel("\u2014")
        self._value.setStyleSheet(
            f"color: {TEXT_PRIMARY}; font-size: 13px; font-family: {_MONO};"
        )
        self._value.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )

        layout.addWidget(self._label)
        layout.addWidget(self._value)
        layout.addStretch(1)

    def set_value(self, text: str, colour: Optional[str] = None) -> None:
        """Update the displayed value."""
        self._value.setText(text)
        if colour:
            self._value.setStyleSheet(
                f"color: {colour}; font-size: 13px; font-family: {_MONO};"
            )
        else:
            self._value.setStyleSheet(
                f"color: {TEXT_PRIMARY}; font-size: 13px; font-family: {_MONO};"
            )


# ---------------------------------------------------------------------------
# PairAnalysisPanel -- stats for a single trading pair
# ---------------------------------------------------------------------------

class PairAnalysisPanel(QFrame):
    """Displays startup analysis statistics for one trading pair.

    Parameters
    ----------
    pair_name:
        The trading pair identifier (e.g. ``"XCH/wUSDC"``).
    parent:
        Optional parent widget.
    """

    def __init__(self, pair_name: str, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)
        self._pair_name = pair_name

        self.setFrameShape(QFrame.Shape.StyledPanel)
        self.setStyleSheet(
            f"PairAnalysisPanel {{"
            f"  background-color: {ELEVATED_BG};"
            f"  border: 1px solid {BORDER};"
            f"  border-radius: 12px;"
            f"}}"
        )

        root = QVBoxLayout(self)
        root.setContentsMargins(18, 16, 18, 16)
        root.setSpacing(8)

        # Title
        title = QLabel(pair_name)
        title.setStyleSheet(
            f"color: {PRIMARY_GREEN}; font-size: 16px; font-weight: bold;"
        )
        root.addWidget(title)

        # Progress bar for this pair's block collection.
        self._progress = QProgressBar()
        self._progress.setRange(0, 100)
        self._progress.setValue(0)
        self._progress.setFixedHeight(18)
        self._progress.setTextVisible(True)
        self._progress.setStyleSheet(
            f"""
            QProgressBar {{
                background-color: {DARK_BG};
                border: 1px solid {BORDER};
                border-radius: 6px;
                text-align: center;
                color: {TEXT_PRIMARY};
                font-size: 11px;
            }}
            QProgressBar::chunk {{
                background-color: {PRIMARY_GREEN};
                border-radius: 5px;
            }}
            """
        )
        root.addWidget(self._progress)

        # Stats grid
        self._vol_row     = _StatRow("Annualised Volatility", self)
        self._spread_row  = _StatRow("Mean Spread", self)
        self._cv_row      = _StatRow("Spread Stability (CV)", self)
        self._vr_row      = _StatRow("Variance Ratio VR(5)", self)
        self._regime_row  = _StatRow("Market Regime", self)
        self._imb_row     = _StatRow("Book Imbalance", self)
        self._mom_row     = _StatRow("Price Momentum", self)
        self._rec_row     = _StatRow("Recommendation", self)

        # [S42 2026-09-02] RELABELLED.  This row used to read "Spread
        # Multiplier", which the operator reasonably took to mean "the
        # multiplier currently applied to my spreads".  It is not: it is the
        # startup-analysis recommendation, frozen for the process lifetime,
        # and on 2026-09-02 it showed 1.50x ("defensively wide") while the
        # live spread PID was railed at 0.820 -- the maximum TIGHTENING.
        # Both multiply the same field, so the display was inverted, not
        # merely stale.  The label now says which one it is, and the live
        # rows below carry the values that actually govern quoting.
        self._mult_row    = _StatRow("Startup Recommendation (x)", self)

        # -- live controller state ------------------------------------------
        self._live_mult_row = _StatRow("Live Spread PID (x)", self)
        # NOT "Effective Multiplier".  This is the product of two of the ten
        # sites that mutate the spread in Step 5, and two of the other eight
        # are assignments that discard the chain outright.  Labelling it
        # "effective" re-created the very confusion these rows were added to
        # end.  The row below it carries the measured, applied value.
        self._eff_mult_row  = _StatRow("Analysis x PID (x)", self)
        self._applied_row   = _StatRow("Applied Spread (bps)", self)
        self._applied_mult_row = _StatRow("Applied Multiplier (x)", self)
        self._rail_row      = _StatRow("PID Rail Status", self)
        self._comp_row      = _StatRow("Competitiveness Gate", self)
        self._take_row      = _StatRow("Crosses Suppressed", self)

        for row in (self._vol_row, self._spread_row, self._cv_row,
                    self._vr_row, self._regime_row, self._imb_row,
                    self._mom_row, self._rec_row, self._mult_row,
                    self._live_mult_row, self._eff_mult_row,
                    self._applied_row, self._applied_mult_row,
                    self._rail_row, self._comp_row, self._take_row):
            root.addWidget(row)

        root.addStretch(1)

    # -- public API -----------------------------------------------------------

    def update_stats(
        self,
        blocks_collected: int,
        blocks_target: int,
        vol_annual: float,
        mean_spread_bps: float,
        spread_cv: float,
        variance_ratio: float,
        book_imbalance: float,
        momentum: float,
        regime_code: int,
        agg_code: int,
        complete: bool,
        spread_multiplier: float = 1.0,
        live: dict | None = None,
    ) -> None:
        """Refresh all displayed statistics.

        Parameters
        ----------
        blocks_collected:
            Number of blocks observed so far.
        blocks_target:
            Total analysis window length.
        vol_annual:
            Annualised volatility fraction (e.g. 0.35 = 35%).
        mean_spread_bps:
            Mean observed spread in basis points.
        spread_cv:
            Spread coefficient of variation (σ/μ).
        variance_ratio:
            Lo-MacKinlay VR(5) estimate.
        book_imbalance:
            Bid fraction of total depth [0, 1]; 0.5 = balanced.
        momentum:
            Cumulative log-return over the analysis window.
        regime_code:
            0 = Mean-Reverting, 1 = Random Walk, 2 = Momentum.
        agg_code:
            0 = Conservative, 1 = Normal, 2 = Aggressive.
        complete:
            True when the analysis window is complete.
        spread_multiplier:
            STARTUP RECOMMENDATION only (1.5=Conservative, 1.0=Normal,
            0.8=Aggressive).  Frozen once the analysis phase ends -- both
            engine call sites are inside run_startup_analysis().  This is
            NOT the multiplier currently applied to quotes; see `live`.
        live:
            Per-pair live controller state from the ``xop_strategy`` gauge
            family, as assembled by ``MetricsService._strategy_for``.  None
            or ``live_available = False`` renders the live rows as "--".
        """
        pct = int(blocks_collected * 100 / blocks_target) if blocks_target > 0 else 0
        self._progress.setValue(pct)
        self._progress.setFormat(
            f"{blocks_collected}/{blocks_target} blocks "
            f"({'complete' if complete else 'collecting…'})"
        )

        # Volatility
        vol_pct = vol_annual * 100.0
        vol_col = WARNING if vol_pct >= 40 else (PROFIT_GREEN if vol_pct < 20 else TEXT_PRIMARY)
        self._vol_row.set_value(f"{vol_pct:.1f}%", vol_col)

        # Spread
        self._spread_row.set_value(f"{mean_spread_bps:.1f} bps")

        # Spread CV (lower is better — stable spread)
        cv_col = WARNING if spread_cv >= 0.8 else (PROFIT_GREEN if spread_cv < 0.3 else TEXT_PRIMARY)
        self._cv_row.set_value(f"{spread_cv:.2f}", cv_col)

        # Variance ratio
        vr_col = (
            PROFIT_GREEN if variance_ratio < 0.85 else
            WARNING      if variance_ratio > 1.15 else
            TEXT_PRIMARY
        )
        self._vr_row.set_value(f"{variance_ratio:.3f}", vr_col)

        # Regime
        regime_label = _REGIME_LABELS.get(regime_code, "Unknown")
        regime_col   = _REGIME_COLORS.get(regime_code, TEXT_PRIMARY)
        self._regime_row.set_value(regime_label, regime_col)

        # Book imbalance
        imb_pct  = book_imbalance * 100.0
        imb_text = f"{imb_pct:.1f}% bids"
        imb_col  = (
            PROFIT_GREEN if 40 <= imb_pct <= 60 else
            WARNING      if imb_pct >= 70 or imb_pct <= 30 else
            TEXT_PRIMARY
        )
        self._imb_row.set_value(imb_text, imb_col)

        # Momentum
        mom_pct  = momentum * 100.0
        mom_col  = PROFIT_GREEN if mom_pct > 0 else (LOSS_RED if mom_pct < 0 else TEXT_PRIMARY)
        mom_sign = "+" if mom_pct > 0 else ""
        self._mom_row.set_value(f"{mom_sign}{mom_pct:.2f}%", mom_col)

        # Recommendation
        agg_label = _AGG_LABELS.get(agg_code, "Unknown")
        agg_col   = _AGG_COLORS.get(agg_code, TEXT_PRIMARY)
        self._rec_row.set_value(agg_label, agg_col)

        # Startup recommendation (derived from the analysis phase, frozen).
        # Deliberately rendered in the muted colour: it is context, not state.
        # Colouring it WARNING/GREEN as if it were live is what made an
        # inverted reading look authoritative.
        self._mult_row.set_value(f"{spread_multiplier:.2f}x", TEXT_MUTED)

        self._update_live(live)

    # -- live controller rows -------------------------------------------------

    def _update_live(self, live: dict | None = None) -> None:
        """Render the live controller rows from an ``xop_strategy`` dict.

        `live` is the per-pair dict produced by
        ``MetricsService._strategy_for``.  When it is None or reports
        ``live_available = False`` -- an engine binary that predates the
        family -- every row shows an explicit em dash.  It must never fall
        back to 1.00x, because "no data" and "neutral multiplier" are
        different facts and conflating them is the original defect.
        """
        if not live or not live.get("live_available", False):
            for row in (self._live_mult_row, self._eff_mult_row,
                        self._rail_row, self._comp_row, self._take_row):
                row.set_value("--", TEXT_MUTED)
            return

        # Live spread PID multiplier.  < 1.0 is TIGHTENING (more aggressive,
        # thinner margin) and is the direction that warrants attention here;
        # > 1.0 is defensive widening.
        if live.get("spread_pid_active", False):
            pid_mult = float(live.get("spread_pid_mult", 1.0))
            pid_col  = (
                WARNING      if pid_mult < 1.0 else
                PROFIT_GREEN if pid_mult > 1.0 else
                TEXT_PRIMARY
            )
            self._live_mult_row.set_value(f"{pid_mult:.3f}x", pid_col)
        else:
            self._live_mult_row.set_value("disabled", TEXT_MUTED)

        # The two controller factors, multiplied.  Explicitly NOT the applied
        # multiplier: Step 5 mutates the spread at ten sites and two of them
        # are assignments that discard everything before them.  0.0 is the
        # engine's "no reading" sentinel and must not render as 1.000x.
        eff = float(live.get("analysis_x_pid_mult", 0.0))
        if eff > 0.0:
            eff_col = (
                WARNING      if eff < 1.0 else
                PROFIT_GREEN if eff > 1.0 else
                TEXT_PRIMARY
            )
            self._eff_mult_row.set_value(f"{eff:.3f}x", eff_col)
        else:
            self._eff_mult_row.set_value("--", TEXT_MUTED)

        # What was ACTUALLY posted, and the multiplier that measures it.
        # base -> applied, so the operator can see the chain's endpoints
        # rather than a subset of its factors.
        base_bps    = float(live.get("spread_base_bps", 0.0))
        applied_bps = float(live.get("spread_applied_bps", 0.0))
        applied_x   = float(live.get("spread_applied_mult", 0.0))
        if base_bps > 0.0 and applied_bps > 0.0:
            self._applied_row.set_value(
                f"{base_bps:.1f} -> {applied_bps:.1f}", TEXT_PRIMARY)
        else:
            self._applied_row.set_value("--", TEXT_MUTED)
        if applied_x > 0.0:
            ax_col = (
                WARNING      if applied_x < 1.0 else
                PROFIT_GREEN if applied_x > 1.0 else
                TEXT_PRIMARY
            )
            self._applied_mult_row.set_value(f"{applied_x:.3f}x", ax_col)
        else:
            self._applied_mult_row.set_value("--", TEXT_MUTED)

        # Rail latch.  A railed controller is not controlling: it is emitting
        # a constant that happens to be recomputed every heartbeat.
        if live.get("rail_latched", False):
            blocks = int(live.get("rail_latched_blocks", 0))
            self._rail_row.set_value(f"RAILED ({blocks} blocks)", WARNING)
        else:
            self._rail_row.set_value("controlling", PROFIT_GREEN)

        # Competitiveness gate: base score, PID offset, effective score.
        # An effective score of 0 means the gate is fully open.
        if live.get("comp_pid_active", False):
            base   = int(live.get("comp_gate_base", 0))
            offset = int(live.get("comp_pid_offset", 0))
            effe   = int(live.get("comp_gate_effective", 0))
            comp_col = WARNING if effe == 0 else TEXT_PRIMARY
            self._comp_row.set_value(
                format_comp_gate(base, offset, effe), comp_col)
        else:
            self._comp_row.set_value("disabled", TEXT_MUTED)

        # Crossed-book takes currently being skipped.  Not derivable from the
        # log: a suppression announces itself once and then heartbeats.
        supp    = int(live.get("take_suppressed", 0))
        tracked = int(live.get("take_tracked", 0))
        self._take_row.set_value(
            f"{supp} of {tracked}",
            WARNING if supp > 0 else TEXT_PRIMARY,
        )


# ---------------------------------------------------------------------------
# MarketAnalysisWidget -- top-level widget
# ---------------------------------------------------------------------------

class MarketAnalysisWidget(QWidget):
    """Startup market analysis display panel.

    Displays analysis progress and collected market statistics for all
    enabled trading pairs during the ``Analyzing`` phase of the engine.
    When analysis is complete (or the engine is in ``Running`` state),
    this widget shows the completed analysis results and indicates that
    trading has started.

    Parameters
    ----------
    parent:
        Optional parent widget.
    """

    def __init__(self, parent: Optional[QWidget] = None) -> None:
        super().__init__(parent)

        self._pair_panels: dict[str, PairAnalysisPanel] = {}
        self._analysis_data: dict[str, Any] = {}
        self._engine_status: str = ""

        self._build_ui()

    # -- UI construction ------------------------------------------------------

    def _build_ui(self) -> None:
        """Build the widget layout."""
        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(6)

        # Header
        hdr_layout = QHBoxLayout()
        hdr_layout.setSpacing(16)

        title = QLabel("Startup Market Analysis")
        title.setStyleSheet(
            f"color: {PRIMARY_GREEN}; font-size: 22px; font-weight: bold;"
        )
        hdr_layout.addWidget(title)
        hdr_layout.addStretch(1)

        self._status_label = QLabel("Waiting for data\u2026")
        self._status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 14px;"
        )
        hdr_layout.addWidget(self._status_label)

        root.addLayout(hdr_layout)

        # Overall progress bar (minimum blocks across all pairs)
        prog_lbl = QLabel("Overall Progress")
        prog_lbl.setStyleSheet(f"color: {TEXT_SECONDARY}; font-size: 12px;")
        root.addWidget(prog_lbl)

        self._overall_progress = QProgressBar()
        self._overall_progress.setRange(0, 100)
        self._overall_progress.setValue(0)
        self._overall_progress.setFixedHeight(18)
        self._overall_progress.setStyleSheet(
            f"""
            QProgressBar {{
                background-color: {DARK_BG};
                border: 1px solid {BORDER};
                border-radius: 4px;
                text-align: center;
                color: {TEXT_PRIMARY};
                font-size: 11px;
            }}
            QProgressBar::chunk {{
                background-color: {PRIMARY_GREEN};
                border-radius: 3px;
            }}
            """
        )
        root.addWidget(self._overall_progress)

        # Separator
        sep = QWidget()
        sep.setFixedHeight(1)
        sep.setStyleSheet(f"background-color: {BORDER};")
        root.addWidget(sep)

        # Description text (scholarly basis note)
        desc = QLabel(
            "The engine observes market conditions for the configured number of "
            "blocks before placing any orders. This burn-in period collects "
            "volatility, spread, regime, and order-book data to inform the "
            "initial quoting strategy — following Ho & Stoll (1981), Madhavan & "
            "Smidt (1993), and Lo & MacKinlay (1988)."
        )
        desc.setWordWrap(True)
        desc.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 11px; font-style: italic;"
        )
        root.addWidget(desc)

        # Scrollable area for per-pair panels
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setStyleSheet(
            f"QScrollArea {{ border: none; background: {DARK_BG}; }}"
        )

        self._pairs_container = QWidget()
        self._pairs_layout = QVBoxLayout(self._pairs_container)
        self._pairs_layout.setContentsMargins(0, 0, 0, 0)
        self._pairs_layout.setSpacing(8)
        self._pairs_layout.addStretch(1)

        scroll.setWidget(self._pairs_container)
        root.addWidget(scroll, stretch=1)

    # -- Public API -----------------------------------------------------------

    @Slot(dict)
    def update_analysis(self, analysis_data: dict[str, Any]) -> None:
        """Update the display with the latest analysis snapshot.

        Parameters
        ----------
        analysis_data:
            Dict keyed by pair name.  Each value is a dict with keys:
            ``blocks_collected``, ``blocks_target``, ``vol_annual``,
            ``mean_spread_bps``, ``spread_cv``, ``variance_ratio``,
            ``book_imbalance``, ``momentum``, ``regime_code``,
            ``agg_code``, ``complete``, ``spread_multiplier``.
        """
        if not analysis_data:
            return

        self._analysis_data = analysis_data

        # Drop panels for pairs no longer present in the incoming analysis
        # payload (for example, pairs disabled in config).
        stale_pairs = set(self._pair_panels) - set(analysis_data)
        for pair_name in stale_pairs:
            panel = self._pair_panels.pop(pair_name)
            panel.deleteLater()

        # Compute overall progress (minimum blocks across pairs).
        blocks_target    = 0
        min_collected    = None
        all_complete     = True

        for pair_name, data in analysis_data.items():
            target    = data.get("blocks_target", 0)
            collected = data.get("blocks_collected", 0)
            complete  = data.get("complete", False)

            if target > blocks_target:
                blocks_target = target
            if min_collected is None or collected < min_collected:
                min_collected = collected
            if not complete:
                all_complete = False

            # Create panel if this pair is new.
            if pair_name not in self._pair_panels:
                panel = PairAnalysisPanel(pair_name, self._pairs_container)
                self._pair_panels[pair_name] = panel
                # Insert before the stretch.
                idx = self._pairs_layout.count() - 1
                self._pairs_layout.insertWidget(idx, panel)

            # Update the panel.
            self._pair_panels[pair_name].update_stats(
                blocks_collected = collected,
                blocks_target    = target,
                vol_annual       = data.get("vol_annual", 0.0),
                mean_spread_bps  = data.get("mean_spread_bps", 0.0),
                spread_cv        = data.get("spread_cv", 0.0),
                variance_ratio   = data.get("variance_ratio", 1.0),
                book_imbalance   = data.get("book_imbalance", 0.5),
                momentum         = data.get("momentum", 0.0),
                regime_code      = data.get("regime_code", 1),
                agg_code         = data.get("agg_code", 1),
                complete         = complete,
                spread_multiplier = data.get("spread_multiplier", 1.0),
                # The whole per-pair dict doubles as the live-state carrier;
                # MetricsService merges the xop_strategy gauges into it.
                live             = data,
            )

        # Update overall progress bar.
        if blocks_target > 0 and min_collected is not None:
            pct = int(min_collected * 100 / blocks_target)
            self._overall_progress.setValue(pct)
            self._overall_progress.setFormat(
                f"{min_collected}/{blocks_target} blocks"
            )

        # Update status label.
        if blocks_target == 0:
            if self._engine_status == "Analyzing":
                self._overall_progress.setValue(0)
                self._overall_progress.setFormat("Initializing…")
                self._status_label.setText(
                    "Analysis initializing — waiting for first metrics"
                )
                self._status_label.setStyleSheet(
                    f"color: {WARNING}; font-size: 13px;"
                )
                return

            self._overall_progress.setValue(100)
            self._overall_progress.setFormat("N/A")
            self._status_label.setText(
                "Analysis disabled — trading immediately"
            )
            self._status_label.setStyleSheet(
                f"color: {TEXT_SECONDARY}; font-size: 13px;"
            )
        elif all_complete:
            self._status_label.setText("✔ Analysis complete — trading active")
            self._status_label.setStyleSheet(
                f"color: {PROFIT_GREEN}; font-size: 13px; font-weight: bold;"
            )
        else:
            self._status_label.setText(
                f"Collecting data… {min_collected or 0}/{blocks_target} blocks"
            )
            self._status_label.setStyleSheet(
                f"color: {WARNING}; font-size: 13px;"
            )

    @Slot(str)
    def set_engine_status(self, status: str) -> None:
        """Inform the widget of the current engine status.

        This allows the UI to avoid showing a false "analysis disabled"
        state while analysis metrics are still initializing.
        """
        self._engine_status = status

        if status != "Analyzing":
            return

        # During analysis startup there can be a short window where
        # xop_analysis metrics have not yet published blocks_target.
        if not self._analysis_data:
            self._overall_progress.setValue(0)
            self._overall_progress.setFormat("Initializing…")
            self._status_label.setText(
                "Analysis initializing — waiting for first metrics"
            )
            self._status_label.setStyleSheet(
                f"color: {WARNING}; font-size: 13px;"
            )
            return

        blocks_target = 0
        for data in self._analysis_data.values():
            target = data.get("blocks_target", 0)
            if target > blocks_target:
                blocks_target = target

        if blocks_target == 0:
            self._overall_progress.setValue(0)
            self._overall_progress.setFormat("Initializing…")
            self._status_label.setText(
                "Analysis initializing — waiting for first metrics"
            )
            self._status_label.setStyleSheet(
                f"color: {WARNING}; font-size: 13px;"
            )

    def clear(self) -> None:
        """Remove all pair panels and reset to the initial state."""
        for panel in self._pair_panels.values():
            panel.deleteLater()
        self._pair_panels.clear()
        self._analysis_data.clear()
        self._overall_progress.setValue(0)
        self._overall_progress.setFormat("")
        self._status_label.setText("Waiting for data…")
        self._status_label.setStyleSheet(
            f"color: {TEXT_SECONDARY}; font-size: 13px;"
        )
