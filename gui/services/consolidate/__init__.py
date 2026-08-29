"""Emergency consolidation: move as much of the balance as possible into one asset.

Lives in the GUI process on purpose.  ``Engine::check_pause_flag()`` has a
single call site (``cpp/src/engine.cpp:1597``) inside ``on_new_block_coro``,
which is only reached after a successful ``get_block_height()``.  During the
2026-08-25 full-node outage that call failed 529 times in a row, so for two and
a half hours the engine read no flag and ran no step -- an emergency control
hosted there would have been unavailable for the whole incident.  The wallet
RPC stayed healthy throughout.  So this talks to the wallet directly and needs
nothing from the engine.
"""

from gui.services.consolidate.planner import (
    Anchor,
    ConsolidationPlan,
    Leg,
    OfferCandidate,
    PlanError,
    build_plan,
    denomination,
    effective_rate,
    rate_deviation_frac,
)

__all__ = [
    "Anchor",
    "denomination",
    "ConsolidationPlan",
    "Leg",
    "OfferCandidate",
    "PlanError",
    "build_plan",
    "effective_rate",
    "rate_deviation_frac",
]
