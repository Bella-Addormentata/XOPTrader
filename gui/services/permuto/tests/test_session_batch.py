"""Session renewal and batch validity.

Both are silent-failure paths. An expired session does not error loudly, it
just stops earning; an invalid batch spends a rate-limit token to be told no
while the stale quotes it meant to replace keep resting.
"""

from __future__ import annotations

import pytest

from gui.services.permuto.batch import MAX_LEGS, BatchError, build_upsert_batch
from gui.services.permuto.orders import OrderIntent, Side, quote_ladder
from gui.services.permuto.session import (
    BACKOFF_CAP_S,
    RENEW_MARGIN_S,
    RenewAction,
    SessionState,
    next_backoff_s,
    renew_action,
)

MKT = "QQQ-VOL-PERP"
ORACLE = {MKT: 0.10}


# --------------------------------------------------------------------------- #
# Session policy
# --------------------------------------------------------------------------- #

def test_a_healthy_session_is_left_alone():
    s = SessionState(token="t", expires_at_s=10_000)
    assert renew_action(s, now_s=1_000) is RenewAction.OK


def test_renewal_is_proactive_not_reactive():
    """Waiting for the first 401 guarantees at least one rejected request per
    cycle -- and during that window a fill can lift a side we cannot restore.
    depth_seconds only accrues while quoting, so that is score, not latency."""
    s = SessionState(token="t", expires_at_s=10_000)
    assert renew_action(s, now_s=10_000 - RENEW_MARGIN_S) is RenewAction.RENEW
    assert renew_action(s, now_s=10_000 - RENEW_MARGIN_S - 1) is RenewAction.OK


def test_a_401_outranks_our_own_clock():
    """A server may invalidate a session before its stated expiry. Trusting
    our copy of the deadline over the server's answer is arguing with reality."""
    s = SessionState(token="t", expires_at_s=10_000, forced=True)
    assert renew_action(s, now_s=0) is RenewAction.RENEW


def test_a_401_also_outranks_the_backoff():
    """The session is known dead; waiting changes nothing except how long the
    book goes unmanaged."""
    s = SessionState(token="t", expires_at_s=10_000, forced=True,
                     consecutive_failures=5, last_attempt_s=1_000)
    assert renew_action(s, now_s=1_001) is RenewAction.RENEW


def test_an_unknown_expiry_renews_rather_than_assumes():
    """An unknown deadline that turns out to be imminent costs a rejected
    batch; a needless renewal costs one request."""
    s = SessionState(token="t", expires_at_s=0.0)
    assert renew_action(s, now_s=1_000) is RenewAction.RENEW


def test_no_token_is_distinct_from_an_expired_one():
    assert renew_action(SessionState(), now_s=0) is RenewAction.NO_SESSION


def test_failures_back_off_but_never_give_up():
    """A session that cannot be renewed means the book is unmanaged. An
    operator notices a bot shouting every minute; not one that went quiet."""
    s = SessionState(token="t", expires_at_s=10_000,
                     consecutive_failures=3, last_attempt_s=1_000)
    assert renew_action(s, now_s=1_001) is RenewAction.WAIT
    assert renew_action(s, now_s=1_000 + next_backoff_s(3)) is RenewAction.RENEW


def test_backoff_grows_then_caps():
    assert next_backoff_s(0) == 0.0
    assert next_backoff_s(1) < next_backoff_s(2) < next_backoff_s(3)
    assert next_backoff_s(50) == BACKOFF_CAP_S


# --------------------------------------------------------------------------- #
# Batch validity
# --------------------------------------------------------------------------- #

def test_a_valid_two_sided_quote_serialises():
    legs = [OrderIntent(MKT, Side.BUY, 0.0995, 10_000),
            OrderIntent(MKT, Side.SELL, 0.1005, 10_000)]
    out = build_upsert_batch(legs, ORACLE)
    assert [o["side"] for o in out] == ["buy", "sell"]
    assert all(o["tif"] == "ALO" for o in out)
    assert all(o["market"] == MKT for o in out)


def test_alo_is_the_default_because_a_crossing_quote_pays_the_spread():
    out = build_upsert_batch([OrderIntent(MKT, Side.BUY, 0.0995, 100)], ORACLE)
    assert out[0]["tif"] == "ALO"


def test_two_legs_on_one_market_side_are_refused():
    """Upsert is keyed on (market, side). A second leg is not a ladder -- one
    silently overwrites the other and the venue picks which."""
    legs = [OrderIntent(MKT, Side.BUY, 0.0995, 100),
            OrderIntent(MKT, Side.BUY, 0.0990, 100)]
    with pytest.raises(BatchError, match="one quote per"):
        build_upsert_batch(legs, ORACLE)


def test_a_three_level_ladder_does_not_fit_this_endpoint():
    """quote_ladder builds levels; batch_upsert holds one per side. Catching
    that here is the difference between a clear error and a mystery 400."""
    legs = quote_ladder(MKT, 0.10, 1000.0, levels=3)
    with pytest.raises(BatchError, match="one quote per"):
        build_upsert_batch(legs, ORACLE)


def test_more_than_twelve_legs_is_refused_whole():
    """The venue rejects the entire batch rather than truncating."""
    legs = [OrderIntent("M%d-VOL-PERP" % i, Side.BUY, 0.0995, 100)
            for i in range(MAX_LEGS + 1)]
    oracles = {leg.market: 0.10 for leg in legs}
    with pytest.raises(BatchError, match="at most"):
        build_upsert_batch(legs, oracles)


def test_the_oracle_ticker_is_refused_in_place_of_the_symbol():
    """A 400 on every single leg, and the two names differ only by suffix."""
    with pytest.raises(BatchError, match="not a tradeable symbol"):
        build_upsert_batch([OrderIntent("QQQ-VOL", Side.BUY, 0.0995, 100)],
                           {"QQQ-VOL": 0.10})


def test_a_missing_oracle_refuses_rather_than_guesses():
    with pytest.raises(BatchError, match="no fresh oracle"):
        build_upsert_batch([OrderIntent(MKT, Side.BUY, 0.0995, 100)], {})


def test_an_illegal_price_is_caught_before_a_token_is_spent():
    with pytest.raises(BatchError, match="legal band"):
        build_upsert_batch([OrderIntent(MKT, Side.BUY, 0.094, 100)], ORACLE)


def test_a_purge_risk_leg_is_refused_with_its_reason():
    """The dangerous one: it can REST and then vanish on an oracle move, so
    the error says so rather than calling it a plain rejection."""
    with pytest.raises(BatchError, match="PURGED"):
        build_upsert_batch([OrderIntent(MKT, Side.BUY, 0.103, 100)], ORACLE)


def test_a_non_finite_size_never_reaches_the_venue():
    with pytest.raises(BatchError, match="size"):
        build_upsert_batch([OrderIntent(MKT, Side.BUY, 0.0995, float("nan"))],
                           ORACLE)


def test_the_restore_path_is_one_call_for_both_sides():
    """A lifted side earns zero for the whole market until restored, and the
    documented restore is one call rather than a cancel/place pair -- so both
    sides must go in the same batch."""
    legs = [OrderIntent(MKT, Side.BUY, 0.0995, 10_000),
            OrderIntent(MKT, Side.SELL, 0.1005, 10_000)]
    assert len(build_upsert_batch(legs, ORACLE)) == 2


def test_three_markets_two_sides_fits_in_one_batch():
    markets = ["QQQ-VOL-PERP", "NVDA-VOL-PERP", "TSLA-VOL-PERP"]
    legs, oracles = [], {}
    for m in markets:
        oracles[m] = 0.10
        legs.append(OrderIntent(m, Side.BUY, 0.0995, 100))
        legs.append(OrderIntent(m, Side.SELL, 0.1005, 100))
    assert len(build_upsert_batch(legs, oracles)) == 6 <= MAX_LEGS
