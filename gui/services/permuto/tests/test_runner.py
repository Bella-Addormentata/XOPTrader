"""The sequencer. Fake client, real policy modules underneath."""

from __future__ import annotations

import pytest

from gui.services.permuto.auth import PermutoAuthError
from gui.services.permuto.client import PermutoNotLinked
from gui.services.permuto.quoting import RestingQuote
from gui.services.permuto.risk import FLATTEN_MARGIN_UTILISATION
from gui.services.permuto.runner import RECANCEL_INTERVAL_S, QuoteRunner, _margin_state
from gui.services.permuto.session import RenewAction

_MKT = "QQQ-VOL-PERP"
_ORACLE = {_MKT: 0.07}


class _Client:
    def __init__(self, **kw):
        self.calls = []
        self.session_action = kw.get("session", RenewAction.OK)
        self.account_payload = kw.get("account", {
            "equity_usd": 100_000.0, "used_margin_usd": 0.0, "positions": {},
        })
        self.open_payload = kw.get("open_orders", {"orders": []})
        self.fail_on = kw.get("fail_on")
        self.cancelled = []

    def _maybe_fail(self, name):
        if self.fail_on == name:
            raise PermutoAuthError("%s exploded" % name)

    def ensure_session(self, now_s):
        self.calls.append("ensure_session")
        self._maybe_fail("ensure_session")
        return self.session_action

    def open_orders(self, now_s):
        self.calls.append("open_orders")
        self._maybe_fail("open_orders")
        return self.open_payload

    def account(self, now_s):
        self.calls.append("account")
        self._maybe_fail("account")
        return self.account_payload

    def batch_upsert(self, legs, now_s):
        self.calls.append("batch_upsert")
        self.last_batch = legs
        self._maybe_fail("batch_upsert")
        return {}

    def cancel_all(self, now_s, markets=None):
        self.calls.append("cancel_all")
        self.cancelled.append(list(markets) if markets else None)
        self._maybe_fail("cancel_all")
        return {}


def _runner(client, **kw):
    # [CURFEW] These tests tick at epoch 1.0, which the real session table
    # reads as CLOSED -- correctly, but it would floor the position cap and
    # quietly turn quoting tests into curfew tests. The curfew has its own
    # suite (test_curfew.py) and its own runner-integration tests below.
    kw.setdefault("curfew_enabled", False)
    return QuoteRunner(client, [_MKT], **kw)


# --------------------------------------------------------------------------- #
# C-11: pause, un-pause, and the belief that must not outlive the orders
# --------------------------------------------------------------------------- #
def test_a_pause_withdraws_and_forgets_the_book():
    c = _Client()
    r = _runner(c)
    r._resting[_MKT] = RestingQuote(0.069, 0.071)

    result = r.tick(1.0, _ORACLE, {"trading_paused": True})

    assert result.action == "withdraw"
    assert "cancel_all" in c.calls
    assert r._resting[_MKT].empty, "belief must not outlive the cancel"


def test_a_pause_KEEPS_the_session_warm():
    """[discord 2026-08-27] This asserted the opposite, and the contest
    begins with the case that breaks it.

    The old reasoning was "nothing can be placed while paused, so renewing is
    a wasted round trip". But the sponsor resets balances on Sunday evening
    during a trading pause that un-pauses at the 09:30 ET open -- fourteen-odd
    hours. Letting the session lapse through that means the first tick after
    the open spends a full challenge/sign/auth round trip before it can place
    anything, at the moment every entrant reconnects at once, on a metric
    that only accrues while quoting. A failed reauth there backs off through
    the open.

    It is not even a per-tick cost: ensure_session() is a policy check that
    only reaches the network when a renewal is actually due.
    """
    c = _Client()
    _runner(c).tick(1.0, _ORACLE, {"trading_paused": True})
    assert "ensure_session" in c.calls


def test_a_sustained_pause_does_not_hammer_cancel_all():
    """A pause is a SUSTAINED withdraw. At a 5s tick the Sunday pause is
    ~10,000 identical authenticated cancels against a venue that is paused --
    a good way to be rate-limited at the open."""
    c = _Client()
    r = _runner(c)
    r.tick(1.0, _ORACLE, {"trading_paused": True})
    first = c.calls.count("cancel_all")
    assert first == 1, "the first withdraw must always cancel"

    # Ten more ticks inside the re-assert window.
    for i in range(10):
        r.tick(2.0 + i, _ORACLE, {"trading_paused": True})
    assert c.calls.count("cancel_all") == first, "re-cancelled a known-empty book"


def test_a_sustained_pause_still_re_asserts_the_cancel_periodically():
    """Belief can be stale -- the venue cancels everything at carried->live
    and reconcile() does not run while paused -- so an empty belief must
    still re-send, just not every tick."""
    c = _Client()
    r = _runner(c)
    r.tick(1.0, _ORACLE, {"trading_paused": True})
    r.tick(1.0 + RECANCEL_INTERVAL_S + 1, _ORACLE, {"trading_paused": True})
    assert c.calls.count("cancel_all") == 2


def test_the_tick_after_an_unpause_rebuilds_rather_than_holds():
    """The venue cancels every resting order at the open, silently."""
    c = _Client()
    r = _runner(c)
    r.tick(1.0, _ORACLE, {"trading_paused": True})
    r._resting[_MKT] = RestingQuote(0.069, 0.071)  # a belief that is now false

    result = r.tick(2.0, _ORACLE, {"trading_paused": False})
    assert result.action == "quote"
    assert "batch_upsert" in c.calls


def test_the_reopen_debt_survives_a_failed_tick():
    """A latch, not a flag consumed by whichever tick happens to see it."""
    c = _Client(fail_on="account")
    r = _runner(c)
    r.tick(1.0, _ORACLE, {"trading_paused": True})

    # The failing tick now WITHDRAWS rather than aborting -- which discharges
    # the debt honestly rather than dropping it, because the full cancel also
    # forgets the book, and a belief that no longer exists cannot be stale.
    # The property that matters is the outcome, not the latch: the book is
    # rebuilt once the venue is readable again.
    assert r.tick(2.0, _ORACLE, {"trading_paused": False}).action == "withdraw"
    assert all(q.empty for q in r._resting.values())

    c.fail_on = None
    assert r.tick(3.0, _ORACLE, {"trading_paused": False}).action == "quote"


def test_a_partial_withdrawal_does_not_discharge_the_rebuild_debt():
    """[review] Clearing the latch on ANY withdrawal was wrong.

    The partial branch deliberately leaves the non-withdrawing markets'
    beliefs intact -- and those are exactly the ones that can still be stale
    after a carried->live cancel, because the venue cancelled orders we
    still think are resting. Discharging the debt there dropped the rebuild
    owed to markets that were never touched.

    Reaching it needs a market that withdraws beside one that decides to
    QUOTE but places nothing: `just_reopened` forces QUOTE, so with a healthy
    account the rebuild really does happen and clearing the latch is right.
    Exhausted margin is what separates the two -- risk leaves no legs, so
    nothing was rebuilt and the debt must survive.
    """
    c = _Client(account={"equity_usd": 1000.0, "used_margin_usd": 995.0,
                         "positions": {}})
    r = _runner2(c)
    r.tick(1.0, {_MKT: 0.07, _MKT2: 0.07}, {"trading_paused": True})

    # _MKT loses its oracle -> 1 of 2 withdraws -> the PARTIAL branch.
    result = r.tick(2.0, {_MKT2: 0.07}, {"trading_paused": False})

    # risk_blocked, not hold: risk refused every leg and the book may be
    # empty -- reporting hold painted the switch ON over nothing resting.
    assert result.action == "risk_blocked"
    assert "batch_upsert" not in c.calls, "nothing was rebuilt"
    assert r._reopen_pending, (
        "withdrawing one market discharged the rebuild owed to the other")


def test_a_partial_withdrawal_beside_a_real_rebuild_does_discharge_it():
    """The other half: when the surviving market actually re-quotes, the
    debt IS paid and the latch must clear -- otherwise every later tick
    force-quotes a book that is already correct."""
    c = _Client()
    r = _runner2(c)
    r.tick(1.0, {_MKT: 0.07, _MKT2: 0.07}, {"trading_paused": True})
    assert r.tick(2.0, {_MKT2: 0.07},
                  {"trading_paused": False}).action == "quote"
    assert not r._reopen_pending



def test_a_second_pause_clears_a_pending_reopen():
    c = _Client()
    r = _runner(c)
    r.tick(1.0, _ORACLE, {"trading_paused": True})
    r.tick(2.0, _ORACLE, {"trading_paused": False})   # reopen latched+consumed
    r.tick(3.0, _ORACLE, {"trading_paused": True})    # paused again
    assert r._reopen_pending is False


def test_a_steady_pause_is_not_a_reopen_every_tick():
    c = _Client()
    r = _runner(c)
    for t in range(1, 5):
        r.tick(float(t), _ORACLE, {"trading_paused": True})
    assert r._reopen_pending is False


# --------------------------------------------------------------------------- #
# Reconciliation
# --------------------------------------------------------------------------- #
def test_reconcile_overwrites_rather_than_merges():
    """The entries the venue omits are the ones that must be dropped."""
    r = _runner(_Client())
    r._resting[_MKT] = RestingQuote(0.069, 0.071)
    r.reconcile({"orders": [{"market": _MKT, "side": "BUY", "price": 0.069}]})
    assert r._resting[_MKT].bid_price == 0.069
    assert r._resting[_MKT].ask_price is None


def test_reconcile_ignores_markets_we_do_not_quote():
    r = _runner(_Client())
    r.reconcile({"orders": [
        {"market": "TSLA-VOL-PERP", "side": "BUY", "price": 0.5},
    ]})
    assert r._resting[_MKT].empty


@pytest.mark.parametrize("junk", [
    {"orders": [{"market": _MKT, "side": "BUY", "price": None}]},
    {"orders": [{"market": _MKT, "side": "BUY", "price": "abc"}]},
    {"orders": ["not a dict"]},
    {"orders": None},
    {},
    [],
])
def test_reconcile_survives_a_malformed_payload(junk):
    r = _runner(_Client())
    r.reconcile(junk)
    assert r._resting[_MKT].empty


def test_a_one_sided_book_is_requoted_because_min_bid_ask_is_zero():
    c = _Client(open_orders={
        "orders": [{"market": _MKT, "side": "BUY", "price": 0.0698}],
    })
    r = _runner(c)
    r._resting[_MKT] = RestingQuote(0.0698, None)
    assert r.tick(1.0, _ORACLE, {}).action == "quote"


# --------------------------------------------------------------------------- #
# Risk wiring
# --------------------------------------------------------------------------- #
def test_a_flattening_account_places_nothing():
    c = _Client(account={
        "equity_usd": 1_000.0,
        "used_margin_usd": 1_000.0 * FLATTEN_MARGIN_UTILISATION,
        "positions": {},
    })
    r = _runner(c)
    result = r.tick(1.0, _ORACLE, {})
    # risk_blocked, not hold: the flatten line left nothing to place, and
    # hold reads to the switch as "trading normally".
    assert result.action == "risk_blocked"
    assert "batch_upsert" not in c.calls


def test_reduce_only_keeps_only_the_shrinking_side():
    c = _Client(account={
        "equity_usd": 100_000.0, "used_margin_usd": 0.0,
        "positions": {_MKT: 100.0},
    })
    r = _runner(c, max_position_usd=7.0)
    assert r.tick(1.0, _ORACLE, {}).action == "quote"
    assert [leg["side"] for leg in c.last_batch] == ["sell"]
    assert all(leg["reduce_only"] for leg in c.last_batch)


def test_a_long_book_is_quoted_below_the_oracle():
    """Price skew, not size skew -- both legs move, sizes stay equal."""
    flat = _Client()
    _runner(flat).tick(1.0, _ORACLE, {})
    long_book = _Client(account={
        "equity_usd": 100_000.0, "used_margin_usd": 0.0,
        "positions": {_MKT: 50.0},
    })
    _runner(long_book, max_position_usd=7.0).tick(1.0, _ORACLE, {})

    def _leg(client, side):
        return [x for x in client.last_batch if x["side"] == side][0]

    # The pair slid down...
    assert _leg(long_book, "buy")["price"] < _leg(flat, "buy")["price"]
    assert _leg(long_book, "sell")["price"] < _leg(flat, "sell")["price"]
    def _notional(client, side):
        leg = _leg(client, side)
        return leg["price"] * leg["size"]

    # ...with the two sides carrying equal NOTIONAL, which is the unit depth
    # credit is measured in, so min(bid_usd, ask_usd) is not truncated. The
    # ladder equalises notional rather than contract count, and notional is
    # the one that has to match.
    assert _notional(long_book, "buy") == pytest.approx(
        _notional(long_book, "sell"), rel=0.001
    )
    # ...and the skew cost nothing in standing notional.
    assert _notional(long_book, "buy") == pytest.approx(
        _notional(flat, "buy"), rel=0.001
    )


def test_a_carried_session_actually_quotes_the_smaller_size():
    """risk.assess() computes an eighth; the ladder must use it."""
    live, carried = _Client(), _Client()
    _runner(live).tick(1.0, _ORACLE, {})
    _runner(carried).tick(1.0, _ORACLE, {"carried": True})

    def _size(client):
        return [x for x in client.last_batch if x["side"] == "buy"][0]["size"]

    assert _size(carried) == pytest.approx(_size(live) / 8.0, rel=0.01)


def test_a_missing_oracle_places_nothing_for_that_market():
    c = _Client()
    result = _runner(c).tick(1.0, {}, {})
    assert result.action == "withdraw"


def test_a_stale_oracle_withdraws():
    c = _Client()
    result = _runner(c).tick(1.0, _ORACLE, {"oracle_age_s": 60.0})
    assert result.action == "withdraw"
    assert "cancel_all" in c.calls


# --------------------------------------------------------------------------- #
# Session
# --------------------------------------------------------------------------- #
def test_a_backing_off_session_waits_rather_than_withdrawing():
    """The book may still be legitimately resting."""
    c = _Client(session=RenewAction.WAIT)
    result = _runner(c).tick(1.0, _ORACLE, {})
    assert result.action == "wait"
    assert "cancel_all" not in c.calls


def test_no_session_at_all_withdraws():
    c = _Client(session=RenewAction.NO_SESSION)
    assert _runner(c).tick(1.0, _ORACLE, {}).action == "withdraw"


# --------------------------------------------------------------------------- #
# The loop outlives its failures
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize(
    "where", ["ensure_session", "open_orders", "account", "batch_upsert"]
)
def test_a_failure_anywhere_returns_rather_than_raises(where):
    """The loop must outlive its own failures for ~102 unattended hours."""
    result = _runner(_Client(fail_on=where)).tick(1.0, _ORACLE, {})
    assert result is not None


@pytest.mark.parametrize("where", ["ensure_session", "open_orders", "account"])
def test_a_failure_reaching_the_venue_RETRACTS_the_book(where):
    """[review] Not merely "returns" -- retracts.

    These three calls sat outside any handler, so a failure aborted the tick
    BEFORE the withdraw path. decide()'s branch for a dead session was
    therefore unreachable by exception, and the previous version of this test
    asserted only that nothing propagated -- testing around the hole rather
    than covering it. Orders stayed resting, priced against a venue we could
    no longer see, for the whole outage.

    A view of the venue we cannot obtain is not a reason to stop managing
    what we already placed.
    """
    c = _Client(fail_on=where)
    result = _runner(c).tick(1.0, _ORACLE, {})
    assert result.action == "withdraw", (
        "%s failed and the book was left resting" % where)
    assert "cancel_all" in c.calls, "nothing was actually retracted"


def test_a_batch_failure_is_still_an_error_not_a_false_all_clear():
    """The batch runs AFTER the withdraw decision, so a failure there is a
    genuine error rather than a degradation -- it must not be reported as a
    successful pass."""
    c = _Client(fail_on="batch_upsert")
    result = _runner(c).tick(1.0, _ORACLE, {})
    assert not result.ok
    assert "exploded" in result.error


def test_an_unexpected_exception_is_still_caught():
    class _Broken(_Client):
        def ensure_session(self, now_s):
            raise ZeroDivisionError("bug")

    result = _runner(_Broken()).tick(1.0, _ORACLE, {})
    assert not result.ok
    assert "ZeroDivisionError" in result.error


def test_never_linked_is_reported_as_blocked_not_a_transient_error():
    class _Unlinked(_Client):
        def ensure_session(self, now_s):
            raise PermutoNotLinked("no session held")

    result = _runner(_Unlinked()).tick(1.0, _ORACLE, {})
    assert result.action == "blocked"


def test_a_rejected_batch_does_not_update_the_belief():
    """Believe only what the venue accepted."""
    c = _Client(fail_on="batch_upsert")
    r = _runner(c)
    assert not r.tick(1.0, _ORACLE, {}).ok
    assert r._resting[_MKT].empty


# --------------------------------------------------------------------------- #
# Account parsing
# --------------------------------------------------------------------------- #
def test_a_malformed_account_reads_as_fully_utilised_not_empty():
    """0 equity means "stop adding risk", which is the safe reading."""
    assert _margin_state(None, False).utilisation() == 1.0
    assert _margin_state({}, False).utilisation() == 1.0


def test_positions_parse_from_a_dict_or_a_list():
    as_dict = _margin_state({"positions": {_MKT: 5.0}}, False)
    as_list = _margin_state(
        {"positions": [{"market": _MKT, "size": 5.0}]}, False
    )
    assert as_dict.positions[_MKT] == as_list.positions[_MKT] == 5.0


def test_numeric_strings_are_accepted_and_booleans_are_not():
    assert _margin_state({"equity_usd": "1500.5"}, False).equity_usd == 1500.5
    assert _margin_state({"equity_usd": True}, False).equity_usd == 0.0


_MKT2 = "NVDA-VOL-PERP"
_BOTH = {_MKT: 0.07, _MKT2: 0.09}


def _runner2(client, **kw):
    kw.setdefault("curfew_enabled", False)   # see _runner above
    return QuoteRunner(client, [_MKT, _MKT2], **kw)


# --------------------------------------------------------------------------- #
# [review] A market that must leave has to leave, whatever its neighbours do
# --------------------------------------------------------------------------- #
def test_one_markets_withdrawal_is_not_skipped_because_another_quotes():
    """The guard used to be `withdrawing and not any_quoted`.

    A stale oracle on QQQ while NVDA merely needed a refresh skipped the
    cancel entirely: QQQ's unsafe orders stayed live and the batch touched
    only NVDA.
    """
    c = _Client()
    r = _runner2(c)
    r._resting[_MKT] = RestingQuote(0.069, 0.071)

    # QQQ has no oracle -> WITHDRAW. NVDA has one and nothing resting -> QUOTE.
    result = r.tick(1.0, {_MKT2: 0.09}, {})

    assert "cancel_all" in c.calls, "the withdrawing market was left live"
    assert c.cancelled[0] == [_MKT], "cancel must be scoped, not global"
    assert result.action == "quote"
    assert {leg["market"] for leg in c.last_batch} == {_MKT2}


def test_every_market_withdrawing_still_cancels_globally():
    c = _Client()
    r = _runner2(c)
    r.tick(1.0, _BOTH, {"trading_paused": True})
    assert c.cancelled[-1] is None, "a full withdrawal should not be scoped"


# --------------------------------------------------------------------------- #
# [review] HOLD must not stop the runner looking at the venue
# --------------------------------------------------------------------------- #
def test_holding_still_polls_orders_and_account():
    """Once _resting looked two-sided the loop stopped asking.

    A filled side was never discovered -- and depth credit is min(bid, ask),
    so that market earns zero -- while margin could cross the reduce and
    flatten lines with the old quotes still live.
    """
    c = _Client(open_orders={"orders": [
        {"market": _MKT, "side": "buy", "price": 0.0698},
        {"market": _MKT, "side": "sell", "price": 0.0702},
    ]})
    r = _runner(c)
    assert r.tick(1.0, _ORACLE, {}).action == "hold"
    assert "open_orders" in c.calls
    assert "account" in c.calls


def test_a_fill_discovered_while_holding_triggers_a_requote():
    # The venue says one side is gone; belief says two-sided.
    c = _Client(open_orders={"orders": [
        {"market": _MKT, "side": "buy", "price": 0.0698},
    ]})
    r = _runner(c)
    r._resting[_MKT] = RestingQuote(0.0698, 0.0702)
    assert r.tick(1.0, _ORACLE, {}).action == "quote"


# --------------------------------------------------------------------------- #
# [review] FLATTEN and REDUCE_ONLY have to retract what is resting
# --------------------------------------------------------------------------- #
def test_flatten_cancels_rather_than_only_relabelling():
    c = _Client(
        open_orders={"orders": [
            {"market": _MKT, "side": "buy", "price": 0.0698},
        ]},
        account={
            "equity_usd": 1_000.0,
            "used_margin_usd": 1_000.0 * FLATTEN_MARGIN_UTILISATION,
            "positions": {},
        },
    )
    r = _runner(c)
    r.tick(1.0, _ORACLE, {})
    assert "cancel_all" in c.calls, "flatten left the two-sided quote live"
    assert c.cancelled[-1] == [_MKT]


def test_reduce_only_cancels_the_market_before_upserting_one_side():
    """batch_upsert is keyed on (market, side).

    Omitting the risk-increasing side does not remove it, so the old quote
    would stay live beside the new reduce-only one. Needs something resting
    for there to be anything to retract.
    """
    c = _Client(
        open_orders={"orders": [
            {"market": _MKT, "side": "buy", "price": 0.0698},
        ]},
        account={
            "equity_usd": 100_000.0, "used_margin_usd": 0.0,
            "positions": {_MKT: 100.0},
        },
    )
    r = _runner(c, max_position_usd=7.0)
    r.tick(1.0, _ORACLE, {})
    assert c.calls.index("cancel_all") < c.calls.index("batch_upsert")
    assert [leg["side"] for leg in c.last_batch] == ["sell"]


# --------------------------------------------------------------------------- #
# [review] A partly-readable account must fail CLOSED
# --------------------------------------------------------------------------- #
def test_equity_without_margin_reads_as_fully_utilised():
    """0.0 read as maximum headroom, so the runner added risk against an
    account it could not actually read."""
    st = _margin_state({"equity_usd": 1_000.0}, False)
    assert st.utilisation() == 1.0


def test_a_readable_zero_margin_is_still_zero():
    st = _margin_state({"equity_usd": 1_000.0, "used_margin_usd": 0.0}, False)
    assert st.utilisation() == 0.0


# --------------------------------------------------------------------------- #
# [live 2026-08-29] The venue's REAL account payload
# --------------------------------------------------------------------------- #
_LIVE_ACCOUNT_PAYLOAD = {
    # Verbatim from POST /exchange/account on the first authenticated tick
    # ever. None of the field names we guessed exist; every number is a
    # STRING. Guessing wrong read as equity 0 -> utilisation 1.0 -> the
    # runner flattened all three markets on a $500k, zero-position account
    # and the supervised test order never quoted.
    "balance": "500000",
    "locked_margin": "0",
    "locked_order_margin": "0",
    "open_order_count": 0,
    "pending_trigger_count": 0,
    "positions": [],
    "pricing_incomplete": False,
    "total_realized_pnl": "0",
    "total_unrealized_pnl": "0",
    "user_id": "b3edfaa83da8cdbfe9258d54776409f1deec0bdb537086617294e6bad1929001",
}


def test_the_live_account_payload_reads_as_healthy():
    st = _margin_state(dict(_LIVE_ACCOUNT_PAYLOAD), False)
    assert st.equity_usd == 500_000.0
    assert st.used_margin_usd == 0.0
    assert st.utilisation() == 0.0
    assert st.positions_readable
    assert st.positions == {}


def test_live_payload_locked_margins_sum_into_used():
    payload = dict(_LIVE_ACCOUNT_PAYLOAD,
                   locked_margin="1200.5", locked_order_margin="300")
    st = _margin_state(payload, False)
    assert st.used_margin_usd == 1500.5


def test_live_payload_unrealized_pnl_moves_equity():
    payload = dict(_LIVE_ACCOUNT_PAYLOAD, total_unrealized_pnl="-2500")
    st = _margin_state(payload, False)
    assert st.equity_usd == 497_500.0


def test_half_a_locked_margin_pair_still_fails_closed():
    """locked_margin without locked_order_margin is a partly-readable
    account -- unknown margin must mean no room, never lots of room."""
    payload = dict(_LIVE_ACCOUNT_PAYLOAD)
    del payload["locked_order_margin"]
    st = _margin_state(payload, False)
    assert st.utilisation() == 1.0


@pytest.mark.parametrize("bad", ["nan", "inf", "-inf"])
def test_a_non_finite_resting_price_is_not_a_present_side(bad):
    """float("nan") converts happily and would count as a live quote.

    RestingQuote.two_sided would then be true while every drift comparison
    against it is false -- the loop HOLDs on a book it cannot evaluate.
    """
    r = _runner(_Client())
    r.reconcile({"orders": [
        {"market": _MKT, "side": "buy", "price": bad},
        {"market": _MKT, "side": "sell", "price": 0.0702},
    ]})
    assert r._resting[_MKT].bid_price is None
    assert not r._resting[_MKT].two_sided


# --------------------------------------------------------------------------- #
# [review] Risk must be evaluated for every LIVE market, not only for the ones
# already deciding to quote.
# --------------------------------------------------------------------------- #

def test_a_held_two_sided_book_is_retracted_when_margin_crosses_the_line():
    """The state decide() is happiest about is the one this missed.

    A two-sided in-ring book returns HOLD, so any_quoted stayed false and
    assess() was never called -- leaving the quote live while the account
    crossed the flatten line. FLATTEN was unreachable in exactly the
    situation it exists for.
    """
    c = _Client(
        open_orders={"orders": [
            {"market": _MKT, "side": "buy", "price": 0.0698},
            {"market": _MKT, "side": "sell", "price": 0.0702},
        ]},
        account={
            "equity_usd": 1_000.0,
            "used_margin_usd": 1_000.0 * FLATTEN_MARGIN_UTILISATION,
            "positions": {},
        },
    )
    r = _runner(c)
    result = r.tick(1.0, _ORACLE, {})

    assert result.action == "withdraw", "the book was left live"
    assert "cancel_all" in c.calls
    assert c.cancelled[-1] == [_MKT]
    assert r._resting[_MKT].empty


def test_a_held_book_at_the_position_limit_is_also_acted_on():
    c = _Client(
        open_orders={"orders": [
            {"market": _MKT, "side": "buy", "price": 0.0698},
            {"market": _MKT, "side": "sell", "price": 0.0702},
        ]},
        account={
            "equity_usd": 100_000.0, "used_margin_usd": 0.0,
            "positions": {_MKT: 100.0},
        },
    )
    r = _runner(c, max_position_usd=7.0)
    assert r.tick(1.0, _ORACLE, {}).action == "withdraw"
    assert "cancel_all" in c.calls


def test_a_healthy_held_book_is_still_left_alone():
    """The guard must not turn every HOLD into a cancel."""
    c = _Client(open_orders={"orders": [
        {"market": _MKT, "side": "buy", "price": 0.0698},
        {"market": _MKT, "side": "sell", "price": 0.0702},
    ]})
    r = _runner(c)
    assert r.tick(1.0, _ORACLE, {}).action == "hold"
    assert "cancel_all" not in c.calls


def test_nothing_resting_means_nothing_to_retract():
    """A flatten-level account with no live quote needs no cancel."""
    c = _Client(account={
        "equity_usd": 1_000.0,
        "used_margin_usd": 1_000.0 * FLATTEN_MARGIN_UTILISATION,
        "positions": {},
    })
    r = _runner(c)
    r.tick(1.0, _ORACLE, {})
    assert "cancel_all" not in c.calls


def test_a_session_backoff_does_not_cancel_a_resting_book():
    """[review] WAIT exists to PRESERVE a legitimately resting book.

    During a backoff no account is fetched, so `state` is the default
    MarginState -- which utilisation() reports as fully used, by design,
    because unreadable must mean no room. Feeding that to the risk pass
    cancelled the book on every WAIT tick.
    """
    c = _Client(session=RenewAction.WAIT, open_orders={"orders": [
        {"market": _MKT, "side": "buy", "price": 0.0698},
        {"market": _MKT, "side": "sell", "price": 0.0702},
    ]})
    r = _runner(c)
    r._resting[_MKT] = RestingQuote(0.0698, 0.0702)

    result = r.tick(1.0, _ORACLE, {})
    assert result.action == "wait"
    assert "cancel_all" not in c.calls, "the resting book was cancelled"
    assert not r._resting[_MKT].empty


def test_an_unreadable_account_still_flattens():
    """Not fetching is not the same as fetching and failing.

    _margin_state() fails closed on a payload it cannot read, and that must
    still reach the risk pass.
    """
    c = _Client(
        open_orders={"orders": [
            {"market": _MKT, "side": "buy", "price": 0.0698},
            {"market": _MKT, "side": "sell", "price": 0.0702},
        ]},
        account={"equity_usd": 1_000.0},      # no used_margin: unreadable
    )
    r = _runner(c)
    assert r.tick(1.0, _ORACLE, {}).action == "withdraw"
    assert "cancel_all" in c.calls


def test_one_markets_risk_retraction_is_not_skipped_because_another_quotes():
    """[sweep] The same mistake as the withdraw path, one commit later.

    A market past its position limit was left holding a live two-sided book
    because a NEIGHBOUR merely needed a refresh. Risk is per-market and so is
    the retraction.
    """
    c = _Client(
        open_orders={"orders": [
            {"market": _MKT, "side": "buy", "price": 0.0698},
            {"market": _MKT, "side": "sell", "price": 0.0702},
        ]},
        account={
            "equity_usd": 1_000.0,
            "used_margin_usd": 1_000.0 * FLATTEN_MARGIN_UTILISATION,
            "positions": {},
        },
    )
    r = _runner2(c)                     # QQQ holds a book; NVDA has none
    r._resting[_MKT] = RestingQuote(0.0698, 0.0702)

    result = r.tick(1.0, _BOTH, {})
    assert "cancel_all" in c.calls, "the at-risk market was left live"
    assert _MKT in (c.cancelled[0] or [])
    assert result.action in ("withdraw", "hold", "quote", "risk_blocked")
    assert r._resting[_MKT].empty


# --------------------------------------------------------------------------- #
# [review] Unknown inventory is not flat inventory
# --------------------------------------------------------------------------- #

def test_an_unreadable_position_is_not_read_as_flat():
    """Dropping it removed the market from the dict, and assess() reads a
    missing market as 0.0 -- so one unreadable position let that market take
    normal risk-increasing quotes against inventory we could not see."""
    import math
    st = _margin_state({"equity_usd": 100_000.0, "used_margin_usd": 0.0,
                        "positions": {_MKT: "not-a-number"}}, False)
    assert math.isnan(st.positions[_MKT]), "the market was dropped, not flagged"


def test_an_unreadable_position_in_a_list_payload_too():
    import math
    st = _margin_state({"equity_usd": 100_000.0, "used_margin_usd": 0.0,
                        "positions": [{"market": _MKT, "size": "junk"}]}, False)
    assert math.isnan(st.positions[_MKT])


def test_an_unreadable_position_stops_the_market_adding_risk():
    """The end-to-end property: assess() already refuses a non-finite
    position; the sentinel is what lets it see one."""
    from gui.services.permuto.risk import RiskAction, assess
    st = _margin_state({"equity_usd": 100_000.0, "used_margin_usd": 0.0,
                        "positions": {_MKT: "junk"}}, False)
    d = assess(st, _MKT, base_size=100.0, max_position=100.0,
               ring_pct=2.0, half_spread_pct=0.5)
    assert d.action is not RiskAction.NORMAL


def test_every_session_holding_tick_extends_the_venue_dead_mans_switch():
    """[release review] The one retraction that survives a crash, a reboot
    or a power loss -- everything in-process needs this process alive, and
    the contest is ~102 unattended hours."""
    class _DMS(_Client):
        def __init__(self, **kw):
            super().__init__(**kw)
            self.armed = []

        def schedule_cancel(self, now_s, deadline_ms):
            self.armed.append(deadline_ms)
            return {}

    c = _DMS()
    _runner(c).tick(100.0, _ORACLE, {})
    assert c.armed, "no venue-side dead man's switch was armed"
    assert c.armed[0] >= int(100.0 * 1000), "deadline is not in the future"


def test_a_failing_dead_mans_switch_does_not_affect_the_tick():
    """A net under the loop, not a gate in it."""
    class _DMSBroken(_Client):
        def schedule_cancel(self, now_s, deadline_ms):
            raise RuntimeError("dms route down")

    result = _runner(_DMSBroken()).tick(1.0, _ORACLE, {})
    assert result.action == "quote", result.reason


def test_a_market_past_its_limit_is_retracted_even_while_a_pause_withdraws():
    """[review round 10] The early return after a withdraw skipped the risk
    pass, so a market HOLDing a two-sided book past its position limit kept
    it for as long as a neighbour was withdrawing -- and a venue pause
    withdraws every tick."""
    c = _Client(account={"equity_usd": 1_000.0,
                         "used_margin_usd": 900.0,      # past the 75% line
                         "positions": {}})
    r = _runner2(c)
    r._resting[_MKT] = RestingQuote(0.0698, 0.0702)   # a live book
    r._resting[_MKT2] = RestingQuote()

    # _MKT2 has no oracle -> withdraws; _MKT would HOLD but is past risk.
    result = r.tick(1.0, {_MKT: 0.07}, {})
    # risk_blocked, because risk had the last word -- what matters is that
    # the tick did NOT return early on the neighbour's withdraw and the
    # at-risk book came down.
    assert result.action in ("withdraw", "risk_blocked")
    assert "cancel_all" in c.calls
    assert r._resting[_MKT].empty, (
        "the at-risk book survived because a neighbour was withdrawing")


def test_a_missing_positions_payload_is_not_a_flat_account():
    """[review round 11] {} and [] are genuinely flat; a MISSING key or a
    wrong-typed value is inventory we cannot see, and both used to collapse
    into the same {} -- so valid equity with no readable positions let every
    market add risk against unknown inventory."""
    from gui.services.permuto.risk import RiskAction, assess

    for bad in ({"equity_usd": 100_000.0, "used_margin_usd": 0.0},
                {"equity_usd": 100_000.0, "used_margin_usd": 0.0,
                 "positions": "nonsense"}):
        st = _margin_state(bad, False)
        assert not st.positions_readable
        d = assess(st, _MKT, base_size=100.0, max_position=1_000.0,
                   ring_pct=2.0, half_spread_pct=0.5)
        assert d.action is RiskAction.FLATTEN, bad

    # And the genuinely flat shapes still quote.
    for flat in ({"equity_usd": 100_000.0, "used_margin_usd": 0.0,
                  "positions": {}},
                 {"equity_usd": 100_000.0, "used_margin_usd": 0.0,
                  "positions": []}):
        st = _margin_state(flat, False)
        assert st.positions_readable
        d = assess(st, _MKT, base_size=100.0, max_position=1_000.0,
                   ring_pct=2.0, half_spread_pct=0.5)
        assert d.action is RiskAction.NORMAL, flat


def test_a_malformed_open_orders_payload_keeps_the_previous_belief():
    """[review round 11] Normalising garbage to [] declared the book empty,
    and an empty belief SKIPS the safety cancel on the withdraw and flatten
    paths -- one mangled response could leave live orders resting precisely
    when risk wanted them gone."""
    r = _runner(_Client())
    r._resting[_MKT] = RestingQuote(0.0698, 0.0702)

    for garbage in ({"unexpected": "shape"}, "nonsense", None, 42):
        r.reconcile(garbage)
        assert not r._resting[_MKT].empty, garbage

    # A WELL-FORMED empty list is still authoritative.
    r.reconcile({"orders": []})
    assert r._resting[_MKT].empty


def test_a_non_clean_batch_status_is_a_failed_tick_not_a_quote():
    """[review round 11] Recording every leg as resting and returning ok on
    "partial" kept the toolbar ON while one side never rested."""
    class _Partial(_Client):
        def batch_upsert(self, legs, now_s):
            self.calls.append("batch_upsert")
            return {"status": "partial", "results": []}

    r = _runner(_Partial())
    result = r.tick(1.0, _ORACLE, {})
    assert not result.ok
    assert "partial" in (result.error or "")
    assert r._resting[_MKT].empty, (
        "legs were recorded as resting off a non-clean status")


# --------------------------------------------------------------------------- #
# [live 2026-08-29] The venue's real batch vocabulary
# --------------------------------------------------------------------------- #

_LIVE_PARTIAL = {
    "status": "batch_partial",
    "note": ("Batch upsert is best-effort; each leg is modify-or-place "
             "independently after the shared mutate token is consumed."),
    "order_count": 2,
    "results": [
        {"action": "modified", "market": _MKT, "order_id": 4512562,
         "price": "0.0995", "remaining_size": "5253", "size": "5253",
         "status": "modified"},
        {"action": "placed", "fills": [], "order_id": 4512662,
         "position": None, "market": _MKT,
         "rejection_reason": "post-only order would cross"},
    ],
}


def test_a_live_batch_partial_is_a_quoting_tick_not_an_error():
    """Captured live: an ALO ask rejected because a bid rests above it is
    the add-liquidity-only guard working -- the tick quotes, the leg
    retries, and nothing is recorded as resting off the response."""
    import copy

    class _Partial(_Client):
        def batch_upsert(self, legs, now_s):
            self.calls.append("batch_upsert")
            return copy.deepcopy(_LIVE_PARTIAL)

    r = _runner(_Partial())
    result = r.tick(1.0, _ORACLE, {})
    assert result.ok, "a benign per-leg rejection must not fail the tick"
    assert "post-only" in result.reason
    assert r._resting[_MKT].empty, (
        "legs must still not be recorded as resting off a partial -- "
        "reconcile() from open_orders owns that belief")


def test_every_leg_rejected_is_still_an_error():
    class _AllRejected(_Client):
        def batch_upsert(self, legs, now_s):
            self.calls.append("batch_upsert")
            return {"status": "batch_partial", "results": [
                {"action": "placed", "market": _MKT,
                 "rejection_reason": "margin"},
                {"action": "placed", "market": _MKT,
                 "rejection_reason": "margin"},
            ]}

    r = _runner(_AllRejected())
    result = r.tick(1.0, _ORACLE, {})
    assert not result.ok
    assert "rejected" in (result.error or "")


def test_batch_partial_without_detail_stays_conservative():
    """The round-11 rule survives the new vocabulary: 'partial' with no
    per-leg rows is unverifiable and must not record legs as resting."""
    class _Empty(_Client):
        def batch_upsert(self, legs, now_s):
            self.calls.append("batch_upsert")
            return {"status": "batch_partial", "results": []}

    r = _runner(_Empty())
    result = r.tick(1.0, _ORACLE, {})
    assert not result.ok
    assert r._resting[_MKT].empty


def test_batch_ok_with_clean_rows_records_resting():
    class _Ok(_Client):
        def batch_upsert(self, legs, now_s):
            self.calls.append("batch_upsert")
            return {"status": "batch_ok", "results": [
                {"action": "placed", "market": _MKT, "order_id": 1},
                {"action": "placed", "market": _MKT, "order_id": 2},
            ]}

    r = _runner(_Ok())
    result = r.tick(1.0, _ORACLE, {})
    assert result.ok
    assert r._resting[_MKT].two_sided, (
        "an all-clean batch_ok is a full acceptance and records the legs")


# --------------------------------------------------------------------------- #
# [WATCH] Contest telemetry: depth accrual, fills, stall detection
# --------------------------------------------------------------------------- #

def _lb_runner(monkeypatch, rows):
    """Runner whose client carries a user id and whose leaderboard reads
    pop from `rows` (a list mutated by the test)."""
    import gui.services.permuto.auth as auth_mod

    c = _Client()
    c._user_id = "u1"
    monkeypatch.setattr(auth_mod, "leaderboard_entry",
                        lambda uid: rows.pop(0) if rows else None)
    return _runner(c)


def test_a_fill_is_logged_loudly_and_not_on_the_baseline(monkeypatch, caplog):
    rows = [
        {"depth_seconds_5d": 100.0, "trade_count": 0, "total_pnl": "0"},
        {"depth_seconds_5d": 200.0, "trade_count": 1, "total_pnl": "3"},
    ]
    r = _lb_runner(monkeypatch, rows)
    import logging
    with caplog.at_level(logging.INFO, logger="gui.services.permuto.runner"):
        r.tick(1.0, _ORACLE, {})
        assert not [m for m in caplog.messages if "FILL LANDED" in m], (
            "the first observation is a baseline, not a fill")
        r.tick(1.0 + 301.0, _ORACLE, {})
    assert [m for m in caplog.messages if "FILL LANDED" in m], (
        "a trade_count increase is the qualifying-fill signal and must "
        "be loud")


def test_stalled_depth_while_quoting_warns_after_two_samples(
        monkeypatch, caplog):
    from gui.services.permuto.quoting import RestingQuote

    rows = [
        {"depth_seconds_5d": 500.0, "trade_count": 0, "total_pnl": "0"},
        {"depth_seconds_5d": 500.0, "trade_count": 0, "total_pnl": "0"},
        {"depth_seconds_5d": 500.0, "trade_count": 0, "total_pnl": "0"},
    ]
    r = _lb_runner(monkeypatch, rows)
    r._resting[_MKT] = RestingQuote(0.099, 0.101)
    import logging
    with caplog.at_level(logging.INFO, logger="gui.services.permuto.runner"):
        r.tick(1.0, _ORACLE, {})
        r.tick(302.0, _ORACLE, {})
        assert not [m for m in caplog.messages if "STALLED" in m], (
            "one flat sample is noise, not a stall")
        r.tick(603.0, _ORACLE, {})
    assert [m for m in caplog.messages if "STALLED" in m]


def test_leaderboard_watch_is_throttled(monkeypatch):
    calls = []
    import gui.services.permuto.auth as auth_mod

    c = _Client()
    c._user_id = "u1"
    monkeypatch.setattr(auth_mod, "leaderboard_entry",
                        lambda uid: calls.append(uid) or None)
    r = _runner(c)
    r.tick(1.0, _ORACLE, {})
    r.tick(2.0, _ORACLE, {})
    r.tick(100.0, _ORACLE, {})
    assert len(calls) == 1, "a full paged leaderboard read every tick "        "would hammer a public endpoint"


# --------------------------------------------------------------------------- #
# [CURFEW] Runner integration. The helpers above opt OUT of the curfew so the
# quoting tests keep testing quoting; these construct it explicitly and prove
# the cap actually reaches risk.assess() through the runner.
# --------------------------------------------------------------------------- #

from gui.services.permuto.curfew import (            # noqa: E402
    CLOSES_UTC, OPENS_UTC, SETTLE_AFTER_OPEN_S,
)

_MID_SESSION = OPENS_UTC[0] + SETTLE_AFTER_OPEN_S + 3_600.0
_OVERNIGHT = CLOSES_UTC[0] + 4 * 3_600.0


def _account(position):
    return {"equity_usd": 100_000.0, "used_margin_usd": 0.0,
            "positions": {_MKT: position}}


def test_curfew_mid_session_quotes_both_sides_at_the_full_cap():
    # 100 contracts * 0.07 = $7 notional, far inside the $1200 cap.
    c = _Client(account=_account(100.0))
    r = _runner(c, curfew_enabled=True)
    assert r.tick(_MID_SESSION, _ORACLE, {}).action == "quote"
    assert sorted(leg["side"] for leg in c.last_batch) == ["buy", "sell"]


def test_curfew_overnight_floors_the_cap_into_reduce_only():
    # Same position, same oracle -- only the clock differs. Overnight the
    # cap is the floor ($150), and $7 of inventory is still inside it, so
    # push the position past the FLOOR to show the curfew biting.
    c = _Client(account=_account(100_000.0))   # $7,000 notional
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert [leg["side"] for leg in c.last_batch] == ["sell"]
    assert all(leg["reduce_only"] for leg in c.last_batch)


def test_the_same_position_is_unrestricted_mid_session():
    # The mirror of the test above: $7,000 exceeds the $150 overnight floor
    # but sits inside the $12,000 configured limit, so mid-session it is
    # quoted two-sided. The ONLY difference is the time of day.
    c = _Client(account=_account(100_000.0))
    r = _runner(c, curfew_enabled=True, max_position_usd=12_000.0)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert sorted(leg["side"] for leg in c.last_batch) == ["buy", "sell"]


def test_a_frozen_oracle_floors_the_cap_even_mid_session():
    # The clock wrong in the direction that costs money: the table says
    # mid-session, the oracle has stopped moving. The freeze wins.
    c = _Client(account=_account(100_000.0))
    r = _runner(c, curfew_enabled=True, max_position_usd=12_000.0)
    t = _MID_SESSION
    for _ in range(6):                     # hold the oracle still
        r.tick(t, _ORACLE, {})
        t += 60.0
    assert [leg["side"] for leg in c.last_batch] == ["sell"]


def test_curfew_disabled_ignores_the_clock_entirely():
    c = _Client(account=_account(100_000.0))
    r = _runner(c, curfew_enabled=False, max_position_usd=12_000.0)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert sorted(leg["side"] for leg in c.last_batch) == ["buy", "sell"]
