"""The GUI side of the live strategy gauges: two defects, both fail-quiet.

[2026-09-02, review]

1. THE COMPETITIVENESS-GATE LABEL LOST ITS SIGN SEPARATOR AT OFFSET 0.
   ``sign = "+" if offset > 0 else ""`` then ``f"{base}{sign}{offset}"``
   concatenates at the neutral offset: base 3 / offset 0 renders "30 -> 3",
   and the operator reads the gate base as thirty on a 0-10 scale.  Offset 0
   is the COMMON state (warm-up window; any block where ema_fill_rate is at
   target).  It survived review because the live controller was railed at -3,
   which renders correctly -- the bug is invisible in exactly the state the
   feature was built and tested against.

2. A GAUGE THAT IS MISSING MUST NOT READ AS HEALTHY.
   ``effective_spread_mult`` defaulted to 1.0, i.e. "quoting at baseline" --
   the most reassuring value in the range -- for an engine that does not
   publish the family at all.  The gauge has since been renamed to what it
   actually is (``analysis_x_pid_mult``: two of Step 5's ten mutation sites)
   and the real applied multiplier is published separately.  Every one of
   them now defaults to 0.0, which the widget renders as "--".

MUTATION CHECK, run before this file was accepted:
  * restoring ``sign = "+" if offset > 0 else ""`` -> the offset-0 tests fail
    ("30 -> 3" vs "3+0 -> 3").
  * restoring ``default=1.0`` on any of the four multiplier keys -> the
    corresponding absence test fails.
Both were confirmed to fail before the fixes and pass after.
"""

from __future__ import annotations

import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from gui.widgets.market_analysis import format_comp_gate  # noqa: E402
from gui.services.metrics_service import MetricsService  # noqa: E402


# ---------------------------------------------------------------------------
# 1. The competitiveness-gate label.
# ---------------------------------------------------------------------------

def test_zero_offset_keeps_the_separator():
    # The regression itself.  Without an explicit sign this rendered "30 -> 3".
    assert format_comp_gate(3, 0, 3) == "3+0 -> 3"
    assert format_comp_gate(1, 0, 1) == "1+0 -> 1"


def test_zero_offset_never_concatenates_the_two_numbers():
    # Stated as the property rather than the string, so a future reformat that
    # keeps the numbers adjacent still fails.
    for base in (1, 3):
        rendered = format_comp_gate(base, 0, base)
        assert f"{base}0" not in rendered, rendered


def test_negative_offset_reads_as_subtraction():
    # The live railed state: offset -3 drives the non-stablecoin gate open.
    assert format_comp_gate(3, -3, 0) == "3-3 -> 0 (open)"
    assert format_comp_gate(1, -1, 0) == "1-1 -> 0 (open)"


def test_positive_offset_reads_as_addition():
    assert format_comp_gate(3, 2, 5) == "3+2 -> 5"
    assert format_comp_gate(1, 4, 5) == "1+4 -> 5"


def test_open_marker_appears_only_at_an_effective_gate_of_zero():
    assert format_comp_gate(3, -3, 0).endswith(" (open)")
    assert not format_comp_gate(3, 0, 3).endswith(" (open)")
    assert not format_comp_gate(3, 2, 5).endswith(" (open)")


def test_every_offset_in_range_is_unambiguous():
    # Exactly one sign character, and it always separates the two numbers.
    for base in (1, 3):
        for offset in range(-15, 16):
            effective = max(0, min(10, base + offset))
            rendered = format_comp_gate(base, offset, effective)
            head = rendered.split(" -> ")[0]
            assert head.count("+") + head.count("-") == 1, rendered
            assert head[0] == str(base), rendered
            assert head[1] in "+-", rendered


# ---------------------------------------------------------------------------
# 2. Absent gauges must read as "no reading", never as 1.0x.
# ---------------------------------------------------------------------------

def test_an_engine_without_the_family_reports_no_multiplier_not_one():
    # An engine binary that predates these gauges publishes no xop_strategy
    # samples at all.  A 1.0 default would render "1.000x" -- quoting at
    # baseline -- for a bot whose spread state is entirely unknown.
    live = MetricsService._strategy_for({}, "XCH/DBX")

    assert live["live_available"] is False
    assert live["analysis_x_pid_mult"] == 0.0
    assert live["spread_applied_mult"] == 0.0
    assert live["spread_base_bps"] == 0.0
    assert live["spread_applied_bps"] == 0.0


def test_a_present_family_is_read_through_verbatim():
    m = {
        "xop_strategy": {
            (("pair_name", "XCH/DBX"), ("metric", "spread_pid_active")): 1.0,
            (("pair_name", "XCH/DBX"), ("metric", "spread_pid_mult")): 0.820,
            (("pair_name", "XCH/DBX"), ("metric", "analysis_x_pid_mult")): 1.23,
            (("pair_name", "XCH/DBX"), ("metric", "spread_base_bps")): 100.0,
            (("pair_name", "XCH/DBX"), ("metric", "spread_applied_bps")): 500.0,
            (("pair_name", "XCH/DBX"), ("metric", "spread_applied_mult")): 5.0,
            (("pair_name", "XCH/DBX"), ("metric", "comp_pid_offset")): -3.0,
            (("pair_name", "XCH/DBX"), ("metric", "comp_gate_base")): 3.0,
            (("pair_name", "XCH/DBX"), ("metric", "comp_gate_effective")): 0.0,
        }
    }
    live = MetricsService._strategy_for(m, "XCH/DBX")

    assert live["live_available"] is True
    assert live["spread_pid_mult"] == 0.820

    # THE FINDING, end to end: the two controller factors multiply to 1.23x
    # while the spread actually posted is 5.0x baseline, because the global
    # half-spread cap assigned 500 bps and discarded the chain.  The GUI must
    # be able to show both, and must not present the first as the second.
    assert live["analysis_x_pid_mult"] == 1.23
    assert live["spread_applied_mult"] == 5.0
    assert live["spread_applied_mult"] != live["analysis_x_pid_mult"]
    assert live["spread_base_bps"] == 100.0
    assert live["spread_applied_bps"] == 500.0


def test_another_pairs_samples_are_not_read_for_this_pair():
    m = {
        "xop_strategy": {
            (("pair_name", "XCH/BYC"), ("metric", "spread_applied_mult")): 5.0,
            (("pair_name", "XCH/BYC"), ("metric", "spread_pid_active")): 1.0,
        }
    }
    live = MetricsService._strategy_for(m, "XCH/DBX")

    assert live["live_available"] is False
    assert live["spread_applied_mult"] == 0.0
