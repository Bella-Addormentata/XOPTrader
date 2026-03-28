"""Prometheus metrics poller for XOPTrader GUI.

Periodically scrapes the C++ engine's ``/metrics`` HTTP endpoint,
parses the Prometheus text exposition format into structured Python
dicts, and emits Qt signals so GUI widgets can bind directly.

All HTTP I/O runs in a dedicated ``QThread`` worker so the GUI event
loop is never blocked.  The service tracks connection state and uses
exponential back-off (1 s -> 2 s -> 4 s -> 8 s -> ... -> 30 s cap)
when the endpoint is unreachable.

Compliant with:
    - ISO/IEC 27001:2022  (TLS-ready URL; no credentials in logs)
    - ISO/IEC 5055       (bounded buffers, no unbounded allocations)
    - ISO/IEC 7498 / 27033  (network I/O isolated in worker thread)
"""

from __future__ import annotations

import logging
import re
import time
from collections import deque
from typing import Any, Final, Optional

from PySide6.QtCore import (
    QMutex,
    QMutexLocker,
    QObject,
    QThread,
    QTimer,
    Signal,
    Slot,
)

# ---------------------------------------------------------------------------
# Module-level logger and constants
# ---------------------------------------------------------------------------
_log: logging.Logger = logging.getLogger(__name__)

# Default scrape target.
_DEFAULT_URL: Final[str] = "http://localhost:9090/metrics"

# Polling interval (milliseconds).
_DEFAULT_POLL_MS: Final[int] = 5_000

# Exponential back-off parameters (seconds).
_BACKOFF_INITIAL_S: Final[float] = 1.0
_BACKOFF_MULTIPLIER: Final[float] = 2.0
_BACKOFF_MAX_S: Final[float] = 30.0

# Maximum metric snapshots retained for sparkline / mini-chart history.
_HISTORY_MAX_LEN: Final[int] = 1_000

# HTTP read timeout (seconds) -- keeps the worker from blocking forever.
_HTTP_TIMEOUT_S: Final[float] = 5.0

# ---------------------------------------------------------------------------
# Prometheus text-format parser (lightweight, no external dependency)
# ---------------------------------------------------------------------------

# Matches a single Prometheus sample line.  Captures:
#   group(1) = metric name
#   group(2) = optional label block including braces (or None)
#   group(3) = numeric value
_SAMPLE_RE: re.Pattern[str] = re.compile(
    r"^([a-zA-Z_:][a-zA-Z0-9_:]*)"  # metric name
    r"(\{[^}]*\})?"                  # optional {labels}
    r"\s+([-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?)"  # value
)

# Matches individual label key="value" pairs inside braces.
_LABEL_RE: re.Pattern[str] = re.compile(
    r'([a-zA-Z_][a-zA-Z0-9_]*)="([^"]*)"'
)


def _parse_prometheus_text(text: str) -> dict[str, dict[tuple[tuple[str, str], ...], float]]:
    """Parse Prometheus text exposition format into a nested dict.

    Parameters
    ----------
    text : str
        Raw HTTP response body in Prometheus text format.

    Returns
    -------
    dict
        Mapping of ``metric_name`` to a dict whose keys are frozen
        label-tuples ``(("label_a", "val"), ...)`` and values are
        floats.  Metrics with no labels use an empty tuple key.
    """
    metrics: dict[str, dict[tuple[tuple[str, str], ...], float]] = {}

    for line in text.splitlines():
        # Skip comments and TYPE / HELP metadata lines.
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue

        match = _SAMPLE_RE.match(stripped)
        if match is None:
            continue

        name: str = match.group(1)
        labels_block: str | None = match.group(2)
        value: float = float(match.group(3))

        # Parse labels into a hashable tuple of pairs.
        if labels_block:
            label_pairs = tuple(sorted(_LABEL_RE.findall(labels_block)))
        else:
            label_pairs = ()

        metrics.setdefault(name, {})[label_pairs] = value

    return metrics


def _scalar(
    metrics: dict[str, dict[tuple[tuple[str, str], ...], float]],
    name: str,
    default: float = 0.0,
) -> float:
    """Extract a single scalar (no labels) from the parsed metrics dict.

    Parameters
    ----------
    metrics : dict
        Output of :func:`_parse_prometheus_text`.
    name : str
        Prometheus metric name.
    default : float
        Returned when the metric is missing.

    Returns
    -------
    float
    """
    inner = metrics.get(name)
    if inner is None:
        return default
    return inner.get((), default)


def _labelled(
    metrics: dict[str, dict[tuple[tuple[str, str], ...], float]],
    name: str,
    label_key: str,
    label_value: str,
    default: float = 0.0,
) -> float:
    """Extract a labelled metric value.

    Parameters
    ----------
    metrics : dict
        Output of :func:`_parse_prometheus_text`.
    name : str
        Prometheus metric name.
    label_key : str
        Label name to match (e.g. ``"pair"``).
    label_value : str
        Label value to match (e.g. ``"XCH/wUSDC"``).
    default : float
        Returned when the metric is missing.

    Returns
    -------
    float
    """
    inner = metrics.get(name)
    if inner is None:
        return default
    for labels, value in inner.items():
        for key, val in labels:
            if key == label_key and val == label_value:
                return value
    return default


# ===================================================================
# Worker -- runs HTTP fetch in a background QThread
# ===================================================================

class _MetricsWorker(QObject):
    """Background worker that performs a single HTTP GET and emits the
    parsed result (or an error) via signals.

    This object is *moved* to a ``QThread`` and communicates with the
    main-thread ``MetricsService`` exclusively through Qt signals.
    """

    # Emitted on successful scrape with the parsed metrics dict.
    result_ready = Signal(dict)

    # Emitted when the HTTP request fails for any reason.
    request_failed = Signal(str)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._url: str = _DEFAULT_URL

    @Slot(str)
    def set_url(self, url: str) -> None:
        """Update the scrape URL.

        Thread-safe: designed to be called via a queued signal from the
        main thread so the mutation occurs on the worker thread.

        Parameters
        ----------
        url : str
            Full URL including path (e.g. ``http://host:9090/metrics``).
        """
        self._url = url

    @Slot()
    def fetch(self) -> None:
        """Perform a blocking HTTP GET, parse the response, and emit
        either ``result_ready`` or ``request_failed``.

        This slot is invoked from the main-thread timer via a
        queued connection, so it executes on the worker thread.
        """
        # Import requests lazily so the module loads even if the library
        # is somehow missing (error surfaces at fetch time instead).
        try:
            import requests  # type: ignore[import-untyped]
        except ImportError as exc:
            self.request_failed.emit(f"Missing dependency: {exc}")
            return

        try:
            response = requests.get(self._url, timeout=_HTTP_TIMEOUT_S)
            response.raise_for_status()
        except requests.RequestException as exc:
            self.request_failed.emit(str(exc))
            return

        try:
            parsed = _parse_prometheus_text(response.text)
        except Exception as exc:  # noqa: BLE001 -- defensive parse guard
            self.request_failed.emit(f"Parse error: {exc}")
            return

        self.result_ready.emit(parsed)


# ===================================================================
# Main service -- lives on the GUI thread
# ===================================================================

class MetricsService(QObject):
    """Prometheus metrics poller with connection tracking and history.

    Parameters
    ----------
    url : str
        Scrape endpoint URL.
    poll_interval_ms : int
        Polling period in milliseconds (default 5 000).
    parent : QObject | None
        Optional Qt parent.

    Signals
    -------
    metrics_updated(dict)
        Emitted after every successful scrape with the full parsed dict.
    connection_lost()
        Emitted on the *first* consecutive failure after a good scrape.
    connection_restored()
        Emitted on the *first* success after one or more failures.
    """

    # -- Qt signals ---------------------------------------------------------
    metrics_updated = Signal(dict)
    connection_lost = Signal()
    connection_restored = Signal()

    # -- Internal trigger signals (queued connections to worker thread) -------
    _trigger_fetch = Signal()
    _trigger_set_url = Signal(str)

    def __init__(
        self,
        url: str = _DEFAULT_URL,
        poll_interval_ms: int = _DEFAULT_POLL_MS,
        parent: Optional[QObject] = None,
    ) -> None:
        super().__init__(parent)

        self._url: str = url
        self._poll_interval_ms: int = max(500, poll_interval_ms)

        # -- Connection state -----------------------------------------------
        self._connected: bool = False
        self._backoff_s: float = _BACKOFF_INITIAL_S

        # -- Latest parsed metrics (guarded by mutex) -----------------------
        self._mutex: QMutex = QMutex()
        self._latest: dict[str, dict[tuple[tuple[str, str], ...], float]] = {}

        # -- History ring buffer for sparkline charts -----------------------
        self._history: deque[dict[str, dict[tuple[tuple[str, str], ...], float]]] = deque(
            maxlen=_HISTORY_MAX_LEN,
        )

        # -- Worker thread --------------------------------------------------
        self._thread: QThread = QThread(self)
        self._thread.setObjectName("MetricsWorkerThread")

        self._worker: _MetricsWorker = _MetricsWorker()
        self._worker.moveToThread(self._thread)

        # Wire worker signals -> main-thread slots.
        self._worker.result_ready.connect(self._on_result)
        self._worker.request_failed.connect(self._on_failure)

        # Queued connections: emit trigger signals to invoke worker slots
        # on the worker thread rather than blocking the GUI thread.
        self._trigger_fetch.connect(self._worker.fetch)
        self._trigger_set_url.connect(self._worker.set_url)

        # -- Poll timer (fires on the main thread) --------------------------
        self._timer: QTimer = QTimer(self)
        self._timer.setTimerType(self._timer.TimerType.CoarseTimer)
        self._timer.timeout.connect(self._request_fetch)

        # Dispatch the initial URL to the worker via queued signal so
        # it is set on the worker thread (thread-safe).
        self._trigger_set_url.emit(self._url)

        _log.info(
            "MetricsService created: url=%s, interval=%d ms",
            self._url,
            self._poll_interval_ms,
        )

    # ===================================================================
    # Lifecycle
    # ===================================================================

    def start(self) -> None:
        """Start the background thread and begin periodic polling."""
        if self._thread.isRunning():
            _log.warning("MetricsService.start() called but thread already running.")
            return
        _log.info("Starting MetricsService polling.")
        self._thread.start()
        self._timer.start(self._poll_interval_ms)
        # Fire the first fetch immediately.
        self._request_fetch()

    def stop(self) -> None:
        """Stop polling and cleanly shut down the worker thread.

        Safe to call even if the service was never started -- the thread
        quit/wait is skipped when the thread is not running.
        """
        _log.info("Stopping MetricsService.")
        self._timer.stop()
        if self._thread.isRunning():
            self._thread.quit()
            # Wait up to 5 seconds for the thread to finish.
            if not self._thread.wait(5_000):
                _log.warning("MetricsService worker thread did not exit in time.")

    # ===================================================================
    # Convenience getters (main-thread, non-blocking)
    # ===================================================================

    def get_pnl(self) -> dict[str, float]:
        """Return a dict of PnL metrics in mojos.

        Keys: ``total``, ``realized``, ``unrealized``, ``spread``,
        ``inventory``.

        Returns
        -------
        dict[str, float]
        """
        with QMutexLocker(self._mutex):
            m = self._latest
        return {
            "total": _scalar(m, "xop_pnl_total_mojos"),
            "realized": _scalar(m, "xop_pnl_realized_mojos"),
            "unrealized": _scalar(m, "xop_pnl_unrealized_mojos"),
            "spread": _scalar(m, "xop_pnl_spread_mojos"),
            "inventory": _scalar(m, "xop_pnl_inventory_mojos"),
        }

    def get_inventory(self, asset_id: str) -> dict[str, float]:
        """Return inventory data for a single asset.

        Parameters
        ----------
        asset_id : str
            Chia asset ID (hex string or ``"xch"``).

        Returns
        -------
        dict[str, float]
            Keys: ``balance``, ``cost_basis``, ``underwater``.
        """
        with QMutexLocker(self._mutex):
            m = self._latest
        return {
            "balance": _labelled(m, "xop_inventory_balance_mojos", "asset_id", asset_id),
            "cost_basis": _labelled(m, "xop_inventory_cost_basis_mojos", "asset_id", asset_id),
            "underwater": _labelled(m, "xop_inventory_underwater", "asset_id", asset_id),
        }

    def get_market_data(self, pair: str) -> dict[str, float]:
        """Return live market data for a trading pair.

        Parameters
        ----------
        pair : str
            Pair name (e.g. ``"XCH/wUSDC"``).

        Returns
        -------
        dict[str, float]
            Keys: ``mid_price``, ``spread_bps``, ``volume_24h``.
        """
        with QMutexLocker(self._mutex):
            m = self._latest
        return {
            "mid_price": _labelled(m, "xop_market_mid_price_mojos", "pair", pair),
            "spread_bps": _labelled(m, "xop_market_spread_bps", "pair", pair),
            "volume_24h": _labelled(m, "xop_market_volume_24h", "pair", pair),
        }

    def get_health(self) -> dict[str, float]:
        """Return node / wallet health indicators.

        Returns
        -------
        dict[str, float]
            Keys: ``block_height``, ``node_synced``, ``wallet_connected``.
            Boolean metrics are represented as ``1.0`` (true) / ``0.0``
            (false).
        """
        with QMutexLocker(self._mutex):
            m = self._latest
        return {
            "block_height": _scalar(m, "xop_health_block_height"),
            "node_synced": _scalar(m, "xop_health_node_synced"),
            "wallet_connected": _scalar(m, "xop_health_wallet_connected"),
        }

    def get_offers_summary(self) -> dict[str, float]:
        """Return aggregated offer counters.

        Returns
        -------
        dict[str, float]
            Keys: ``pending``, ``filled``, ``cancelled``, ``expired``.
        """
        with QMutexLocker(self._mutex):
            m = self._latest
        return {
            "pending": _scalar(m, "xop_offers_pending"),
            "filled": _scalar(m, "xop_offers_filled_total"),
            "cancelled": _scalar(m, "xop_offers_cancelled_total"),
            "expired": _scalar(m, "xop_offers_expired_total"),
        }

    def get_risk(self) -> dict[str, Any]:
        """Return risk metrics.

        Returns
        -------
        dict[str, Any]
            Keys: ``var_95``, ``max_drawdown``, ``concentration`` (dict
            of asset_id -> float).
        """
        with QMutexLocker(self._mutex):
            m = self._latest

        # Concentration is a labelled metric -- collect all asset_ids.
        concentration: dict[str, float] = {}
        conc_inner = m.get("xop_risk_concentration", {})
        for labels, value in conc_inner.items():
            for key, val in labels:
                if key == "asset_id":
                    concentration[val] = value

        return {
            "var_95": _scalar(m, "xop_risk_var_95_mojos"),
            "max_drawdown": _scalar(m, "xop_risk_max_drawdown_mojos"),
            "concentration": concentration,
        }

    def get_analysis(self, pairs: list[str]) -> dict[str, dict[str, Any]]:
        """Return startup market analysis metrics for all specified pairs.

        Reads the ``xop_analysis`` (global) and ``xop_analysis_pair``
        (per-pair) metrics published by the C++ engine during the
        ``Analyzing`` phase.

        Parameters
        ----------
        pairs : list[str]
            Pair names to query (e.g. ``["XCH/wUSDC"]``).

        Returns
        -------
        dict[str, dict[str, Any]]
            Outer key: pair name.
            Inner keys: ``blocks_collected`` (float), ``blocks_target`` (float),
            ``vol_annual``, ``mean_spread_bps``, ``spread_cv``,
            ``variance_ratio``, ``book_imbalance``, ``momentum``,
            ``regime_code``, ``agg_code`` (all float), ``complete`` (bool),
            ``spread_multiplier`` (float).
        """
        with QMutexLocker(self._mutex):
            m = self._latest

        blocks_target = _labelled(
            m, "xop_analysis", "metric", "blocks_target"
        )

        result: dict[str, dict[str, Any]] = {}
        for pair in pairs:
            def _pair_metric(metric_name: str, default: float = 0.0, _p: str = pair) -> float:
                inner = m.get("xop_analysis_pair", {})
                for labels, value in inner.items():
                    label_dict = dict(labels)
                    if (label_dict.get("pair_name") == _p and
                            label_dict.get("metric") == metric_name):
                        return value
                return default

            collected = _pair_metric("blocks_collected")
            complete: bool = _pair_metric("complete") >= 1.0

            result[pair] = {
                "blocks_collected": collected,
                "blocks_target":    blocks_target,
                "vol_annual":       _pair_metric("volatility_annual"),
                "mean_spread_bps":  _pair_metric("mean_spread_bps"),
                "spread_cv":        _pair_metric("spread_cv"),
                "variance_ratio":   _pair_metric("variance_ratio", default=1.0),
                "book_imbalance":   _pair_metric("book_imbalance", default=0.5),
                "momentum":         _pair_metric("momentum"),
                "regime_code":      _pair_metric("regime", default=1.0),
                "agg_code":         _pair_metric("aggressiveness", default=1.0),
                "complete":         complete,
            }

        # Attach the global recommended_spread_multiplier to every pair entry.
        spread_mult = _labelled(
            m, "xop_analysis", "metric", "recommended_spread_multiplier",
            default=1.0,
        )
        for pair in result:
            result[pair]["spread_multiplier"] = spread_mult

        return result

    def is_analysis_active(self) -> bool:
        """True when startup analysis metrics indicate an in-progress analysis.

        Returns ``True`` when the engine has published a non-zero
        ``blocks_target`` and at least one pair has not yet reached
        ``complete = 1``.  Returns ``False`` when no analysis metrics are
        present or all pairs are complete.

        Returns
        -------
        bool
        """
        with QMutexLocker(self._mutex):
            m = self._latest

        blocks_target = _labelled(m, "xop_analysis", "metric", "blocks_target")
        if blocks_target <= 0:
            return False

        inner = m.get("xop_analysis_pair", {})
        for labels, value in inner.items():
            label_dict = dict(labels)
            if label_dict.get("metric") == "complete" and value < 1.0:
                return True

        return False

    def get_history(self) -> list[dict[str, dict[tuple[tuple[str, str], ...], float]]]:
        """Return a copy of the metrics history buffer.

        Returns
        -------
        list[dict]
            Up to 1 000 most recent parsed metric snapshots, oldest first.
        """
        with QMutexLocker(self._mutex):
            return list(self._history)

    @property
    def is_connected(self) -> bool:
        """Whether the last scrape was successful."""
        return self._connected

    # ===================================================================
    # Internal slots
    # ===================================================================

    def set_url(self, url: str) -> None:
        """Update the scrape endpoint URL on the worker.

        This is the public API for changing the metrics URL; callers
        should use this instead of accessing the worker directly.
        The URL is dispatched to the worker via a queued signal so the
        mutation occurs on the worker thread (no cross-thread data race).

        Parameters
        ----------
        url : str
            Full URL including path (e.g. ``http://host:9090/metrics``).
        """
        self._url = url
        self._trigger_set_url.emit(url)

    @Slot()
    def _request_fetch(self) -> None:
        """Ask the worker to perform a fetch.

        Emits ``_trigger_fetch`` which is connected to the worker's
        ``fetch`` slot via a queued connection, ensuring the HTTP I/O
        executes on the worker thread rather than blocking the GUI.
        """
        # Invoke across thread boundary via queued signal.
        self._trigger_fetch.emit()

    @Slot(dict)
    def _on_result(
        self,
        parsed: dict[str, dict[tuple[tuple[str, str], ...], float]],
    ) -> None:
        """Handle a successful scrape result from the worker.

        Parameters
        ----------
        parsed : dict
            Prometheus metrics parsed into structured form.
        """
        with QMutexLocker(self._mutex):
            self._latest = parsed
            self._history.append(parsed)

        # Reset back-off on success.
        self._backoff_s = _BACKOFF_INITIAL_S

        # Connection-state transitions.
        if not self._connected:
            self._connected = True
            _log.info("Metrics endpoint connected: %s", self._url)
            self.connection_restored.emit()

        # Resume normal polling interval.
        if self._timer.interval() != self._poll_interval_ms:
            self._timer.setInterval(self._poll_interval_ms)

        self.metrics_updated.emit(parsed)

    @Slot(str)
    def _on_failure(self, error_msg: str) -> None:
        """Handle a failed scrape.

        Applies exponential back-off to the timer interval and emits
        ``connection_lost`` on the first failure.

        Parameters
        ----------
        error_msg : str
            Human-readable error description from the worker.
        """
        _log.warning("Metrics fetch failed: %s (backoff=%.1fs)", error_msg, self._backoff_s)

        if self._connected:
            self._connected = False
            self.connection_lost.emit()

        # Increase timer interval using exponential back-off.
        backoff_ms = int(self._backoff_s * 1_000)
        self._timer.setInterval(backoff_ms)

        # Advance the back-off for the next failure, capped.
        self._backoff_s = min(
            self._backoff_s * _BACKOFF_MULTIPLIER,
            _BACKOFF_MAX_S,
        )
