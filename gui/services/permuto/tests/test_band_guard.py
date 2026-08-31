"""[BANDGUARD] Legs must be inside the venue's oracle band ON ARRIVAL.

Encodes the measured contest-open failure: quotes priced off a
grace-aged oracle read during a ~0.25%/s collapse overshot the +/-5%
band (15s x 0.25 + 2% ring = 5.75%), and one out-of-band leg 400'd the
whole batch -- minutes of 100% rejection with clean minutes in between.
"""

from __future__ import annotations

from gui.services.permuto.band_guard import (
    SAFETY_PCT,
    VENUE_BAND_PCT,
    BandGuard,
    clamp_offset_window,
)


# --------------------------------------------------------------------------- #
# The window arithmetic
# --------------------------------------------------------------------------- #

def test_calm_fresh_oracle_leaves_the_full_ring_available():
    # band 5 - safety 1 - 0 drift = 4%: comfortably outside the 2% ring,
    # so the guard is a no-op exactly when the venue would accept all.
    assert clamp_offset_window(5.0, 1.0, 0.0, 0.0) == 4.0


def test_the_contest_open_case_closes_the_window():
    # The measured collapse: 15s-old read at 0.25%/s -> 3.75% projected
    # drift; 5 - 1 - 3.75 = 0.25% window. The 2%-ring ask that 400'd the
    # batch is clamped to within 0.25% of the current read instead.
    w = clamp_offset_window(5.0, 1.0, 15.0, 0.25)
    assert abs(w - 0.25) < 1e-9


def test_fast_enough_movement_means_skip_not_clamp():
    assert clamp_offset_window(5.0, 1.0, 15.0, 0.5) < 0.0
    assert clamp_offset_window(0.0, 1.0, 0.0, 0.0) == 0.0


def test_negative_inputs_do_not_widen_the_window():
    assert clamp_offset_window(5.0, 1.0, -10.0, 0.25) == 4.0
    assert clamp_offset_window(5.0, 1.0, 10.0, -0.25) == 4.0


# --------------------------------------------------------------------------- #
# Velocity tracking
# --------------------------------------------------------------------------- #

def test_velocity_measures_percent_per_second():
    g = BandGuard(alpha=1.0)                     # no smoothing: raw sample
    g.observe(0.0, {"M": 0.100})
    g.observe(10.0, {"M": 0.095})                # -5% over 10s = 0.5%/s
    assert abs(g.velocity("M") - 0.5) < 1e-9


def test_a_graced_repeat_sample_holds_the_estimate():
    # A re-served (graced) read repeats the same price; dt>0 but dP=0
    # pairs are skipped, so a stall neither zeroes nor inflates velocity.
    g = BandGuard(alpha=1.0)
    g.observe(0.0, {"M": 0.100})
    g.observe(5.0, {"M": 0.090})                 # 2%/s
    g.observe(10.0, {"M": 0.090})                # graced repeat
    g.observe(15.0, {"M": 0.090})                # graced repeat
    assert abs(g.velocity("M") - 2.0) < 1e-9


def test_regime_change_is_picked_up_within_two_samples():
    # The danger is frozen -> collapsing. alpha=0.5 must not linger on
    # the calm estimate while the band rejects everything.
    g = BandGuard()                              # default alpha 0.5
    g.observe(0.0, {"M": 0.100})
    for i in range(1, 6):                        # frozen: identical prices
        g.observe(i * 5.0, {"M": 0.100})
    assert g.velocity("M") == 0.0
    g.observe(30.0, {"M": 0.090})                # -10%/5s = 2%/s sample
    g.observe(35.0, {"M": 0.081})                # another 2%/s
    assert g.velocity("M") > 1.4                 # mostly converged


def test_junk_prices_are_ignored():
    g = BandGuard(alpha=1.0)
    g.observe(0.0, {"M": 0.1})
    g.observe(5.0, {"M": 0.0})
    g.observe(6.0, {"M": float("nan")})
    g.observe(7.0, {"M": "0.2"})
    assert g.velocity("M") == 0.0


# --------------------------------------------------------------------------- #
# Clamping
# --------------------------------------------------------------------------- #

def test_a_calm_market_leaves_ring_prices_untouched():
    g = BandGuard()
    g.observe(0.0, {"M": 0.100})
    for i in range(1, 4):
        g.observe(i * 5.0, {"M": 0.100})
    ask = 0.102                                  # +2%: ring edge
    assert g.clamp_price("M", 0.100, ask, 2.0) == ask


def test_the_open_collapse_clamps_the_stale_ask_inside_the_band():
    g = BandGuard(alpha=1.0)
    g.observe(0.0, {"M": 0.100})
    g.observe(5.0, {"M": 0.09875})               # 0.25%/s decay
    # Priced off a 15s-old read, the ask sits ~5.75% above the current
    # oracle -- the exact 400 from the open. Window is 0.25%.
    oracle_now = 0.09875
    stale_ask = 0.09875 * 1.0575
    clamped = g.clamp_price("M", oracle_now, stale_ask, 15.0)
    assert 0.0 < clamped <= oracle_now * 1.0026
    assert clamped >= oracle_now * 0.9974


def test_no_window_means_skip():
    g = BandGuard(alpha=1.0)
    g.observe(0.0, {"M": 0.100})
    g.observe(5.0, {"M": 0.0975})                # 0.5%/s
    assert g.clamp_price("M", 0.0975, 0.0995, 15.0) == 0.0


def test_junk_oracle_or_price_means_skip():
    g = BandGuard()
    assert g.clamp_price("M", 0.0, 0.1, 1.0) == 0.0
    assert g.clamp_price("M", 0.1, 0.0, 1.0) == 0.0


def test_an_unknown_market_is_treated_as_calm():
    # No observations yet: velocity 0, window band-safety. A brand-new
    # market must not be frozen out of quoting by an absent estimate.
    g = BandGuard()
    assert g.clamp_price("NEW", 0.100, 0.102, 3.0) == 0.102
