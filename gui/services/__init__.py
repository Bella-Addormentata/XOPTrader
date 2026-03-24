"""Service sub-package for background data providers and connectors.

Public API
----------
ConfigService   -- YAML configuration loader / validator / writer.
MetricsService  -- Prometheus endpoint poller with connection tracking.
DatabaseService -- Read-only SQLite query runner on a background thread.
EngineBridge    -- Unified facade aggregating all three services.
"""

from gui.services.config_service import ConfigService
from gui.services.database_service import DatabaseService
from gui.services.engine_bridge import EngineBridge
from gui.services.metrics_service import MetricsService

__all__: list[str] = [
    "ConfigService",
    "DatabaseService",
    "EngineBridge",
    "MetricsService",
]
