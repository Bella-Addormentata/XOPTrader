"""The sequencer. Fake client, real policy modules underneath."""

from __future__ import annotations

import logging
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
        #: Callable(legs) -> dict, or a dict. Default {} means "the venue
        #: confirmed nothing", which the runner now scores as zero depth.
        self.batch_response = kw.get("batch_response")

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
        if self.batch_response is None:
            return {}
        if callable(self.batch_response):
            return self.batch_response(legs)
        return self.batch_response

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
    # [2026-08-31] This test used a list row carrying `size` and NO `side`,
    # and asserted it equalled the signed dict form. That is precisely the
    # assumption that hid the phantom-long bug for a whole session: the
    # dict form is SIGNED, while the venue's list form is an unsigned
    # magnitude plus a direction. The two agree only when the direction is
    # actually given.
    as_dict = _margin_state({"positions": {_MKT: 5.0}}, False)
    as_list = _margin_state(
        {"positions": [{"market": _MKT, "side": "buy", "size": 5.0}]}, False
    )
    assert as_dict.positions[_MKT] == as_list.positions[_MKT] == 5.0

    short = _margin_state(
        {"positions": [{"market": _MKT, "side": "sell", "size": 5.0}]}, False
    )
    assert short.positions[_MKT] == -5.0


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


def test_overnight_from_flat_the_runner_places_only_the_bid():
    """End to end: the frozen-oracle window opens no new shorts.

    Flat inventory breaches no position limit and neither side shrinks, so
    every symmetric control in the stack says "quote both". Only the
    per-leg curfew veto stops the ask going out against a stale oracle.
    """
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert c.last_batch, "expected a bid, got nothing"
    assert {leg["side"] for leg in c.last_batch} == {"buy"}


def test_overnight_an_existing_short_is_still_worked_off():
    # The prohibition is on GROWING a short, never on reducing one carried
    # in from the session.
    c = _Client(account=_account(-100.0))
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert {leg["side"] for leg in c.last_batch} == {"buy"}
    assert all(leg["reduce_only"] for leg in c.last_batch)


def test_mid_session_from_flat_both_sides_still_go_out():
    # The asymmetry belongs to the closed window, not to the strategy.
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert sorted({leg["side"] for leg in c.last_batch}) == ["buy", "sell"]


# --------------------------------------------------------------------------- #
# [review] Regressions for the two blockers the adversarial pass found in the
# first curfew implementation. Both were verified by execution before the fix.
# --------------------------------------------------------------------------- #

def _legs(client):
    return [(l["side"], l["size"], round(l["size"] * float(l["price"]), 2))
            for l in (client.last_batch or [])]


def test_overnight_a_long_permits_an_ask_of_exactly_that_long():
    """BLOCKER 1. The veto used to be size-blind: it waved through any ask
    once position > 0 because "selling reduces a long". True only up to the
    SIZE of the long -- and the ladder leg is sized to target_depth_usd, so
    ONE contract of inventory bought a $1,199.99 ask. Filling it left us
    ~$1,196 SHORT overnight: four times the long cap, and precisely the
    position the curfew exists to forbid."""
    c = _Client(account=_account(1.0))
    _runner(c, curfew_enabled=True).tick(_OVERNIGHT, _ORACLE, {})
    sells = [leg for leg in _legs(c) if leg[0] == "sell"]
    assert len(sells) == 1
    assert sells[0][1] == 1.0, "the ask must close the long, never exceed it"


def test_overnight_a_larger_long_scales_the_ask_with_it():
    c = _Client(account=_account(2_000.0))
    _runner(c, curfew_enabled=True).tick(_OVERNIGHT, _ORACLE, {})
    sells = [leg for leg in _legs(c) if leg[0] == "sell"]
    assert sells and sells[0][1] == 2_000.0


def test_overnight_the_bid_is_sized_to_the_cap_not_to_target_depth():
    """BLOCKER 2. The curfew capped the POSITION but never the LEG, so a
    $1,200 bid rested against a $300 overnight cap -- one fill blew 4x
    through it in a single trade and the ramp could never converge."""
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=True, max_position_usd=1_200.0)
    r.tick(_OVERNIGHT, _ORACLE, {})
    buys = [leg for leg in _legs(c) if leg[0] == "buy"]
    assert len(buys) == 1
    assert buys[0][2] <= 300.0 + 1.0, "bid notional exceeds the long cap"
    assert buys[0][2] > 250.0, "bid collapsed to nothing"


def test_a_curfew_stage_change_retracts_the_resting_book():
    """BLOCKER 3. The clamp only shapes legs about to be placed. An ask
    resting from before the close stayed live and takeable, because
    decide() answers HOLD for a quote that is still fresh and in-ring."""
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert "cancel_all" in c.calls, "the stage change must retract the book"


def test_a_failed_retraction_is_retried_rather_than_latched():
    # Latching the new stage over a failed cancel would leave the old book
    # resting under the new caps for the rest of the night.
    c = _Client(account=_account(0.0), fail_on="cancel_all")
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert r._curfew_stage is None, "stage latched despite a failed cancel"
    c2_calls_before = len([x for x in c.calls if x == "cancel_all"])
    r.tick(_OVERNIGHT + 5.0, _ORACLE, {})
    after = len([x for x in c.calls if x == "cancel_all"])
    assert after > c2_calls_before, "the retraction was not retried"


def test_every_tick_reports_the_curfew_stage_it_ran_under():
    """The GUI shows only the action, so without this an operator sees
    bid-only quoting at 22:00 with no way to tell an intended curfew from a
    half-broken loop."""
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=True)
    assert r.tick(_OVERNIGHT, _ORACLE, {}).curfew == "closed"
    assert r.tick(_MID_SESSION, _ORACLE, {}).curfew == "session"


def test_the_stage_is_reported_on_a_tick_that_could_not_trade():
    # The degraded paths are exactly where knowing the posture matters --
    # and they are the ones most likely to forget it, since each builds its
    # own TickResult. (An unreadable account degrades to withdraw rather
    # than erroring, which is why this asserts the action rather than ok.)
    c = _Client(account=_account(0.0), fail_on="account")
    r = _runner(c, curfew_enabled=True)
    result = r.tick(_OVERNIGHT, _ORACLE, {})
    assert result.action == "withdraw"
    assert result.curfew == "closed"


def test_a_disabled_curfew_reports_nothing_rather_than_guessing():
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    assert r.tick(_OVERNIGHT, _ORACLE, {}).curfew == ""


# --------------------------------------------------------------------------- #
# [BATCHBREAKER] A repeating rejection must not re-send forever.
#
# Live 2026-08-30: the venue began answering 'batch_failed' while still
# reporting every leg 'placed' -- it accepts legs best-effort even when the
# batch status is a failure. The loop treated that as an error, declined to
# record the orders, and re-sent every 5s: ~12 sends a minute, each leaving
# orders behind at the venue.
# --------------------------------------------------------------------------- #

class _RejectingClient(_Client):
    """Answers with an unrecognised batch status, as the venue really did."""

    def batch_upsert(self, legs, now_s):
        self.calls.append("batch_upsert")
        self.last_batch = legs
        return {"status": "batch_failed",
                "results": [{"action": "placed", "order_id": 1}]}


def _sends(client):
    return len([x for x in client.calls if x == "batch_upsert"])


def test_a_repeating_batch_rejection_stops_re_sending_every_tick():
    from gui.services.permuto.runner import BATCH_FAIL_STREAK_LIMIT
    c = _RejectingClient(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    t = 1.0
    for _ in range(20):
        r.tick(t, _ORACLE, {})
        t += 5.0
    # Without the breaker this is 20. With it, the streak limit plus at most
    # one probe per interval across the 100s walked here.
    assert _sends(c) <= BATCH_FAIL_STREAK_LIMIT + 3, _sends(c)
    assert _sends(c) >= BATCH_FAIL_STREAK_LIMIT


def test_the_breaker_still_probes_so_it_can_heal():
    from gui.services.permuto.runner import BATCH_PROBE_INTERVAL_S
    c = _RejectingClient(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    t = 1.0
    for _ in range(10):
        r.tick(t, _ORACLE, {})
        t += 5.0
    before = _sends(c)
    t += BATCH_PROBE_INTERVAL_S + 5.0
    r.tick(t, _ORACLE, {})
    assert _sends(c) == before + 1, "the breaker never probes again"


def test_an_accepted_batch_clears_the_streak():
    c = _RejectingClient(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    t = 1.0
    for _ in range(3):
        r.tick(t, _ORACLE, {})
        t += 5.0
    assert r._batch_fail_streak == 3
    c.batch_upsert = lambda legs, now_s: {"status": "batch_ok", "results": []}
    r.tick(t, _ORACLE, {})
    assert r._batch_fail_streak == 0


def test_the_curfew_retraction_waits_for_a_session():
    """Live: it ran before ensure_session() and failed every restart with
    'cancel_all needs a session and none is held'."""
    c = _Client(account=_account(0.0), session=RenewAction.NO_SESSION)
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    # A cancel may still happen on the withdraw path -- what must never
    # happen again is one being attempted BEFORE the session exists.
    if "cancel_all" in c.calls:
        assert c.calls.index("ensure_session") < c.calls.index("cancel_all")
    # And the retraction is deferred rather than dropped, so it still runs
    # once a session is held.
    assert r._curfew_retract_pending is True, "the retraction was dropped"


def test_the_deferred_retraction_runs_once_a_session_exists():
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=True)
    r.tick(_OVERNIGHT, _ORACLE, {})
    assert r._curfew_retract_pending is False, "the retraction never ran"
    assert "cancel_all" in c.calls
    assert c.calls.index("ensure_session") < c.calls.index("cancel_all")


# --------------------------------------------------------------------------- #
# [live 2026-08-31, contest open] The venue's ACTUAL success status is
# 'batch_upserted', which this code had never seen -- the accepted set was
# ("batch_ok", "batch_partial") and the comment beside it admitted batch_ok
# was inferred "by symmetry". Every successful batch at the open was logged
# as an error, and the breaker would have throttled a healthy loop.
# --------------------------------------------------------------------------- #

class _UpsertedClient(_Client):
    def batch_upsert(self, legs, now_s):
        self.calls.append("batch_upsert")
        self.last_batch = legs
        return {"status": "batch_upserted",
                "results": [{"action": "placed", "order_id": 7},
                            {"action": "modified", "order_id": 8}]}


class _UnknownStatusClient(_Client):
    def batch_upsert(self, legs, now_s):
        self.calls.append("batch_upsert")
        self.last_batch = legs
        return {"status": "batch_teleported",       # never seen before
                "results": [{"action": "placed", "order_id": 9}]}


def test_batch_upserted_is_a_success_not_an_error():
    c = _UpsertedClient(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    result = r.tick(1.0, _ORACLE, {})
    assert result.action != "error", result.reason
    assert r._batch_fail_streak == 0


def test_an_unknown_status_with_clean_legs_is_believed():
    # Guessing at a closed vocabulary has failed twice; the legs are the
    # evidence when the envelope is unfamiliar.
    c = _UnknownStatusClient(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    for _ in range(8):
        r.tick(1.0, _ORACLE, {})
    assert r._batch_fail_streak == 0
    assert len([x for x in c.calls if x == "batch_upsert"]) == 8,         "the breaker throttled a working venue"


def test_an_explicit_rejection_is_believed_over_its_legs():
    # The 2026-08-30 case: 'batch_failed' WITH legs reporting 'placed'.
    # Best-effort placement inside a failing envelope must not read as
    # success, or a genuine refusal is silently reclassified.
    c = _RejectingClient(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    r.tick(1.0, _ORACLE, {})
    assert r._batch_fail_streak == 1


# --------------------------------------------------------------------------- #
# [BANDGUARD] End to end: a collapsing oracle clamps leg prices so the
# batch survives, instead of one stale leg 400ing every sibling.
# --------------------------------------------------------------------------- #

def test_a_collapsing_oracle_still_produces_an_in_band_batch():
    """A fast oracle must not push our legs out of the venue band.

    [review round 3] This test used to guard its assertion behind
    `if c.last_batch:`. Removing the guard exposed that no batch was
    being sent at all -- the third tick HELD, because the quote placed
    on the second tick was still fresh and in-ring, so decide() rightly
    declined to touch it and the assertion loop iterated over nothing.
    The fix is to clear the resting book so a re-quote genuinely happens.
    """
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    # Establish a fast decay: ~0.5%/s across two ticks.
    r.tick(_MID_SESSION, {_MKT: 0.100}, {})
    r.tick(_MID_SESSION + 5.0, {_MKT: 0.0975}, {})
    assert r._band_guard.velocity(_MKT) > 0.0, "no velocity: test is inert"
    c.last_batch = None
    r._resting = {}
    r.tick(_MID_SESSION + 10.0, {_MKT: 0.0975}, {"oracle_age_s": 2.0})
    assert c.last_batch, "no batch was sent; the assertion never ran"
    for leg in c.last_batch:
        dev = abs(float(leg["price"]) / 0.0975 - 1.0) * 100.0
        assert dev <= 5.0, "leg %r is outside the venue band" % (leg,)


def test_a_collapse_faster_than_the_band_stands_the_market_down():
    """When drift exceeds the band there is no safe price -- send nothing.

    [review round 3] The sibling test above was written as though a fast
    oracle is always survivable by clamping. It is not. At 0.5%/s a read
    10s stale has already drifted 5%, which is the WHOLE venue band, so
    the clamp window closes and the only correct action is to stand the
    market down. Asserting that explicitly stops a future change from
    turning a stand-down into a silently-empty batch and calling it a
    pass.
    """
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    r.tick(_MID_SESSION, {_MKT: 0.100}, {})
    r.tick(_MID_SESSION + 5.0, {_MKT: 0.0975}, {})
    c.last_batch = None
    r._resting = {}
    res = r.tick(_MID_SESSION + 10.0, {_MKT: 0.0975},
                 {"oracle_age_s": 10.0})
    assert not c.last_batch, "sent a batch the band cannot accommodate"
    action, reason = res.markets[_MKT]
    assert action == "skip", res.markets
    assert "too fast" in reason, reason


def test_a_calm_oracle_is_untouched_by_the_guard():
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    for i in range(4):
        r.tick(_MID_SESSION + i * 5.0, _ORACLE, {})
    assert c.last_batch, "expected quotes on a calm market"
    # The ladder's own ring is 2%; nothing should be clamped tighter.
    for leg in c.last_batch:
        dev = abs(float(leg["price"]) / _ORACLE[_MKT] - 1.0) * 100.0
        assert dev <= 2.1


# --------------------------------------------------------------------------- #
# [PREFLIGHT] The runner re-reads the oracle immediately before sending.
# --------------------------------------------------------------------------- #

def test_a_moved_oracle_re_anchors_legs_before_they_are_sent():
    """The live case: the tick prices off 0.10, the oracle is 0.09 by send
    time. Every leg must land inside +/-5% of the FRESH value, not the
    stale one it was priced against."""
    c = _Client(account=_account(0.0))
    fresh = {_MKT: 0.09}
    r = _runner(c, curfew_enabled=False, oracle_fetch=lambda: fresh)
    r.tick(_MID_SESSION, {_MKT: 0.10}, {})
    assert c.last_batch, "expected a batch"
    for leg in c.last_batch:
        dev = abs(float(leg["price"]) / 0.09 - 1.0) * 100.0
        assert dev <= 5.0, "leg %r outside the band of the fresh oracle" % (leg,)


def test_a_failed_pre_send_fetch_still_quotes_off_the_tick_read():
    # A hiccup on the extra request must not cost a quoting cycle.
    def boom():
        raise RuntimeError("oracle fetch exploded")

    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False, oracle_fetch=boom)
    result = r.tick(_MID_SESSION, _ORACLE, {})
    assert result.action != "error", result.reason
    assert c.last_batch, "a failed pre-send fetch silenced the loop"


def test_no_fetcher_configured_behaves_exactly_as_before():
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert c.last_batch


def test_a_violently_moving_oracle_sends_nothing_rather_than_a_400():
    # Velocity high enough that projected flight-time drift exceeds the
    # band: no price is safe, so the batch is skipped with a reason.
    c = _Client(account=_account(0.0))
    fresh = {_MKT: 0.070}
    r = _runner(c, curfew_enabled=False, oracle_fetch=lambda: fresh)
    r.tick(_MID_SESSION, {_MKT: 0.100}, {})          # -30% in one step
    r._send_latency_s = 20.0                          # slow link
    c.last_batch = None
    r.tick(_MID_SESSION + 5.0, {_MKT: 0.070}, {})
    assert c.last_batch is None, "sent into a market it cannot price"


def test_a_preflight_drop_retracts_the_quote_it_cannot_replace():
    """[Copilot review] Omitting a leg from an upsert does NOT retract the
    quote already resting for that (market, side) -- the venue keeps it. A
    stand-down that leaves the old book live is the opposite of standing
    down."""
    c = _Client(account=_account(0.0))
    # Violent move: velocity high enough that no price survives the flight.
    r = _runner(c, curfew_enabled=False, oracle_fetch=lambda: {_MKT: 0.05})
    r.tick(_MID_SESSION, {_MKT: 0.100}, {})
    r._send_latency_s = 30.0
    c.calls.clear()
    r.tick(_MID_SESSION + 5.0, {_MKT: 0.050}, {})
    assert "cancel_all" in c.calls, "left an unsafe quote resting"


def test_re_anchored_legs_are_sent_on_the_venue_tick_grid():
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False, oracle_fetch=lambda: {_MKT: 0.09})
    r.tick(_MID_SESSION, {_MKT: 0.10}, {})
    assert c.last_batch
    for leg in c.last_batch:
        ticks = float(leg["price"]) / 0.0001
        assert abs(round(ticks) - ticks) < 1e-6, "off-grid price %r" % leg


# --------------------------------------------------------------------------- #
# [live 2026-08-31] POSITION SIGN. The venue reports a position as
# {"side": "sell", "size": "812520"} -- an unsigned magnitude plus a
# direction. Reading `size` alone recorded a short as a LONG, and every
# risk control downstream ran inverted: the loop "reduced" its phantom
# long by SELLING, which grew the real short all session.
#
# Every pre-existing test used the DICT form of `positions`, which carries
# a signed number, so the list form the venue actually sends was untested.
# --------------------------------------------------------------------------- #

def _account_rows(rows):
    return {"equity_usd": 100_000.0, "used_margin_usd": 0.0,
            "positions": rows}


def _pos(market, side, size):
    return {"market": market, "side": side, "size": str(size)}


def test_a_sell_position_parses_as_a_short():
    from gui.services.permuto.runner import _margin_state
    st = _margin_state(_account_rows([_pos(_MKT, "sell", 812520)]), False)
    assert st.positions[_MKT] == -812520.0


def test_a_buy_position_parses_as_a_long():
    from gui.services.permuto.runner import _margin_state
    st = _margin_state(_account_rows([_pos(_MKT, "buy", 500)]), False)
    assert st.positions[_MKT] == 500.0


def test_an_unorientable_position_fails_closed():
    # "Keep the magnitude" is "default to long" wearing a modest hat --
    # the exact assumption that created the phantom long. Unreadable.
    import math
    from gui.services.permuto.runner import _margin_state
    st = _margin_state(
        _account_rows([{"market": _MKT, "size": "42"}]), False)
    assert math.isnan(st.positions[_MKT])


def test_a_flat_row_without_a_side_is_still_flat():
    from gui.services.permuto.runner import _margin_state
    st = _margin_state(
        _account_rows([{"market": _MKT, "size": "0"}]), False)
    assert st.positions[_MKT] == 0.0


def test_a_short_is_reduced_by_BUYING_not_selling():
    """THE BUG, end to end. A short past the cap must quote the side that
    shrinks it. Before the sign fix this emitted asks, and every ask that
    filled made the short larger."""
    c = _Client(account=_account_rows([_pos(_MKT, "sell", 812520)]))
    r = _runner(c, curfew_enabled=False, max_position_usd=7.0)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert c.last_batch, "expected a reducing quote"
    sides = {leg["side"] for leg in c.last_batch}
    assert sides == {"buy"}, "a short must be reduced by buying, got %s" % sides
    assert all(leg["reduce_only"] for leg in c.last_batch)


def test_a_long_is_still_reduced_by_selling():
    c = _Client(account=_account_rows([_pos(_MKT, "buy", 812520)]))
    r = _runner(c, curfew_enabled=False, max_position_usd=7.0)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert c.last_batch
    assert {leg["side"] for leg in c.last_batch} == {"sell"}


def test_a_market_spec_tick_is_used_when_repricing():
    """[Copilot round 2] Repricing must use the market's PUBLISHED tick,
    the same one quote_ladder built with -- not a hardcoded 0.0001."""
    c = _Client(account=_account(0.0))
    r = _runner(c, curfew_enabled=False, oracle_fetch=lambda: {_MKT: 0.09})
    r.tick(_MID_SESSION, {_MKT: 0.10},
           {"specs": {_MKT: {"tick_size": 0.01, "lot_size": 1.0}}})
    # [review round 3] Unconditional. A `if c.last_batch:` guard
    # lets this pass on an EMPTY batch -- one of the very failures
    # it exists to catch. A test that cannot fail reports coverage
    # that is not there.
    assert c.last_batch, "no batch was sent; the assertion never ran"
    for leg in c.last_batch:
        ticks = float(leg["price"]) / 0.01
        assert abs(round(ticks) - ticks) < 1e-6, (
            "leg %r ignores the market's 0.01 tick" % (leg,))


def test_survivors_are_re_priced_after_a_cancel_round_trip():
    """[Copilot round 2] The cancel is an authenticated round trip between
    the fresh read and the send; survivors must be re-read against a NEW
    oracle rather than ageing through it on a one-request budget."""
    reads = []

    def fetch():
        reads.append(len(reads))
        # Second read differs, so a re-price is observable.
        return {_MKT: 0.09 if len(reads) < 2 else 0.088, _MKT2: 0.05}

    c = _Client(account=_account(0.0))
    r = _runner2(c, curfew_enabled=False, oracle_fetch=fetch)
    # [review round 3] This test used to guard its assertions behind
    # `if "cancel_all" in c.calls:`. With a fresh runner the band guard
    # has ZERO velocity, so no leg was ever dropped, no cancel happened,
    # and the test passed without exercising the path it is named for. A
    # test that cannot fail reports coverage that does not exist.
    #
    # Establish a real velocity first -- two samples, -50% over 5s -- so
    # the stand-down actually triggers, then assert unconditionally.
    r._band_guard.observe(_MID_SESSION, {_MKT: 0.100, _MKT2: 0.200})
    r._band_guard.observe(_MID_SESSION + 5.0, {_MKT: 0.050, _MKT2: 0.200})
    assert r._band_guard.velocity(_MKT) > 0.0, "no velocity: test is inert"
    r._send_latency_s = 30.0          # guarantees stand-down on _MKT

    c.calls.clear()
    reads.clear()
    r.tick(_MID_SESSION + 10.0, {_MKT: 0.050, _MKT2: 0.200}, {})

    assert "cancel_all" in c.calls, "the unsafe market was never retracted"
    assert len(reads) >= 2, "survivors were not re-read after the cancel"

# --------------------------------------------------------------------------- #
# [DEPTHSIGNAL] A one-sided book banks nothing, and must say so.
# --------------------------------------------------------------------------- #

def _venue_ok(reject_idx=()):
    """A realistic accepted batch response, one result row per leg.

    Rows are positional against the legs, which is how the venue has
    replied on every live capture. `reject_idx` marks legs the venue
    refused -- an ALO cross, typically.
    """
    def build(legs):
        return {
            "status": "batch_ok",
            "results": [
                {"action": "placed",
                 "rejection_reason": (
                     "Post-only order would cross the book."
                     if i in reject_idx else None)}
                for i, _ in enumerate(legs)
            ],
        }
    return build


def test_an_accepted_two_sided_batch_reports_the_rested_credit(caplog):
    c = _Client(account=_account(100.0), batch_response=_venue_ok())
    r = _runner(c, curfew_enabled=True)
    with caplog.at_level(logging.DEBUG, logger="gui.services.permuto.runner"):
        assert r.tick(_MID_SESSION, _ORACLE, {}).action == "quote"
    assert sorted(leg["side"] for leg in c.last_batch) == ["buy", "sell"]
    assert not [rec for rec in caplog.records
                if "banked ZERO depth" in rec.getMessage()],         "a healthy accepted two-sided book was reported as earning nothing"
    assert [rec for rec in caplog.records
            if "RESTED depth credit" in rec.getMessage()],         "the rested depth credit was never reported at all"


def test_a_batch_the_venue_refuses_banks_nothing(caplog):
    """The instrument must not report intent as achievement.

    [live 2026-08-31] The first version of this signal was computed from
    the OUTGOING batch, before the send. It logged "$1200/s" on ticks
    whose batch then 400'd in its entirety, so it claimed we were earning
    while the venue's own counter sat flat at 4,093.892 for 46 minutes --
    38 whole-batch rejections in ~14 minutes, every one banking zero. A
    measurement taken before the thing it measures is not a measurement.

    Here the legs are a perfectly good two-sided book and the venue
    refuses BOTH. The credit must be zero.
    """
    c = _Client(account=_account(100.0),
                batch_response=_venue_ok(reject_idx=(0, 1)))
    r = _runner(c, curfew_enabled=True)
    with caplog.at_level(logging.DEBUG, logger="gui.services.permuto.runner"):
        r.tick(_MID_SESSION, _ORACLE, {})
    assert c.last_batch, "no batch was sent; the assertions never ran"
    assert sorted(leg["side"] for leg in c.last_batch) == ["buy", "sell"],         "the OUTGOING book was two-sided -- that is the point of this test"
    assert not [rec for rec in caplog.records
                if "RESTED depth credit" in rec.getMessage()],         "reported credit for a batch the venue refused outright"
    assert [rec for rec in caplog.records
            if "banked ZERO depth" in rec.getMessage()],         "a fully-refused batch did not warn that it banked nothing"


def test_a_reduce_only_batch_warns_that_it_banks_nothing(caplog):
    """Overnight the curfew floors the short cap, leaving one side only.

    `depth_credit_usd` scores that at min(bid, ask) = 0, so the tick banks
    nothing towards the 300,000,000 gate however large the leg is.
    """
    c = _Client(account=_account(-100.0), batch_response=_venue_ok())
    r = _runner(c, curfew_enabled=True)
    with caplog.at_level(logging.DEBUG, logger="gui.services.permuto.runner"):
        r.tick(_OVERNIGHT, _ORACLE, {})
    # Unconditional: measured, this tick DOES send one reduce-only buy leg.
    assert c.last_batch, "no batch was sent; the assertions never ran"
    sides = {leg["side"] for leg in c.last_batch}
    assert len(sides) == 1, "expected a one-sided book, got %r" % (sides,)
    assert all(leg["reduce_only"] for leg in c.last_batch), c.last_batch
    assert [rec for rec in caplog.records
            if "banked ZERO depth" in rec.getMessage()],         "a one-sided book was sent without warning that it earns nothing"

# --------------------------------------------------------------------------- #
# [ANTICROSS] The venue's refusals must actually move our prices.
# --------------------------------------------------------------------------- #

def _venue_cross(reject_idx):
    """A response refusing the given leg indices for crossing the book."""
    def build(legs):
        return {
            "status": "batch_ok",
            "results": [
                {"action": "placed",
                 "rejection_reason": (
                     "Post-only order would cross the book. Switch to GTC "
                     "or adjust price." if i in reject_idx else None)}
                for i, _ in enumerate(legs)
            ],
        }
    return build


def test_a_crossing_refusal_widens_the_next_quote(caplog):
    """The whole point: refused legs must come back further from the book.

    [live 2026-08-31] 51 legs were refused for crossing in one afternoon.
    Each refusal rested nothing and therefore banked zero depth-seconds,
    and the loop re-sent the same crossing price on the next tick forever
    because nothing consumed the venue's answer.
    """
    c = _Client(account=_account(100.0), batch_response=_venue_cross((0, 1)))
    r = _runner(c, curfew_enabled=True)
    with caplog.at_level(logging.INFO, logger="gui.services.permuto"):
        r.tick(_MID_SESSION, _ORACLE, {})
    first = sorted(float(l["price"]) for l in c.last_batch)
    assert r._cross_backoff.offset_pct(_MKT) > 0.0,         "the venue said we crossed and the placement did not move"

    # Second tick: same oracle, but the book must be quoted wider.
    r._resting = {}
    r.tick(_MID_SESSION + 5.0, _ORACLE, {})
    second = sorted(float(l["price"]) for l in c.last_batch)
    assert second[0] < first[0], "the bid did not retreat after crossing"
    assert second[-1] > first[-1], "the ask did not retreat after crossing"


def test_the_widened_quote_still_earns_full_depth_credit():
    """Retreating must not cost eligibility -- that would defeat the point.

    depth_credit_usd counts a leg's whole notional anywhere inside the
    ring, so a wider quote scores exactly the same as a tight one. If a
    backoff ever pushed a leg out of the ring it would rest and score
    nothing, which is the failure this feature exists to remove.
    """
    oracle = _ORACLE[_MKT]
    c = _Client(account=_account(100.0), batch_response=_venue_cross((0, 1)))
    r = _runner(c, curfew_enabled=True)
    for i in range(12):                       # drive the backoff to its cap
        r._resting = {}
        r.tick(_MID_SESSION + i * 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) > 0.0
    for leg in c.last_batch:
        dev = abs(float(leg["price"]) / oracle - 1.0) * 100.0
        assert dev <= 2.0 + 1e-9, (
            "leg %r sits %.3f%% out, outside the 2%% credit ring -- it would "
            "rest and earn nothing" % (leg, dev))


def test_a_clean_market_relaxes_back_toward_its_spread():
    c = _Client(account=_account(100.0), batch_response=_venue_cross((0, 1)))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    widened = r._cross_backoff.offset_pct(_MKT)
    assert widened > 0.0

    c.batch_response = _venue_ok()            # venue stops refusing
    for i in range(1, 4):
        r._resting = {}
        r.tick(_MID_SESSION + i * 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) < widened,         "the backoff never relaxed once the venue stopped refusing us"

# --------------------------------------------------------------------------- #
# [ANTICROSS review] Refusal matching, and what counts as a clean tick.
# --------------------------------------------------------------------------- #

def _venue_reason(reason, idx=(0, 1)):
    def build(legs):
        return {"status": "batch_ok",
                "results": [{"action": "placed",
                             "rejection_reason": reason if i in idx else None}
                            for i, _ in enumerate(legs)]}
    return build


def test_the_short_refusal_spelling_also_widens_the_backoff():
    """The venue's wording is not one fixed sentence.

    Live it sends "Post-only order would cross the book. Switch to GTC or
    adjust price."; this repo's own fixtures carry the shorter "post-only
    order would cross". An exact match on "cross the book" skipped the
    short form, so a refusal took the CLEAN path and DECAYED the backoff
    instead of widening it -- precisely backwards, and invisible because
    both spellings look alike at a glance.
    """
    c = _Client(account=_account(100.0),
                batch_response=_venue_reason("post-only order would cross"))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) > 0.0,         "the short refusal spelling was treated as a clean tick"


def test_a_capitalised_full_sentence_refusal_still_matches():
    c = _Client(account=_account(100.0),
                batch_response=_venue_reason(
                    "Post-only order would cross the book. Switch to GTC "
                    "or adjust price."))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) > 0.0


def test_a_non_crossing_refusal_does_not_decay_the_backoff():
    """A margin or band rejection is not evidence we stopped crossing.

    Those never reach the venue's post-only check at all, so treating them
    as a clean tick walks the learned offset back while the book is exactly
    where it was -- undoing convergence during the other refusal classes.
    """
    c = _Client(account=_account(100.0),
                batch_response=_venue_reason(
                    "Post-only order would cross the book."))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    widened = r._cross_backoff.offset_pct(_MKT)
    assert widened > 0.0

    c.batch_response = _venue_reason(
        "Price 0.0999 is outside the allowed oracle band")
    r._resting = {}
    r.tick(_MID_SESSION + 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) == widened, \
        "a band rejection decayed the crossing backoff"


def test_a_rejected_action_with_no_reason_does_not_decay_the_backoff():
    """action=rejected with no rejection_reason must not count as clean.

    [review] The previous check only tested ``rejection_reason``; a row
    that carries ``action='rejected'`` and an empty reason passed the
    guard and decayed the learned offset even though the order did not
    rest -- exactly backwards.
    """
    c = _Client(account=_account(100.0),
                batch_response=_venue_cross((0, 1)))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    widened = r._cross_backoff.offset_pct(_MKT)
    assert widened > 0.0

    # Venue sends action=rejected with no reason -- the batch succeeded
    # overall but the individual legs did not rest.
    def _rejected_no_reason(legs):
        return {"status": "batch_ok",
                "results": [{"action": "rejected", "rejection_reason": None}
                            for _ in legs]}

    c.batch_response = _rejected_no_reason
    r._resting = {}
    r.tick(_MID_SESSION + 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) == widened, \
        "action=rejected with no reason decayed the crossing backoff"


def test_a_market_with_fewer_rows_than_legs_does_not_decay_the_backoff():
    """zip truncation must not hide an unverifiable leg.

    [review] When leg_rows is shorter than legs, zip silently drops the
    trailing legs.  The previous code called observe_clean because the
    market appeared in ``seen`` and not in ``dirty``, even though half
    the legs had no row at all.  Now the unpaired suffix is marked dirty
    so the clean path is blocked.
    """
    c = _Client(account=_account(100.0),
                batch_response=_venue_cross((0, 1)))
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    widened = r._cross_backoff.offset_pct(_MKT)
    assert widened > 0.0

    # Return only one row for a two-leg batch -- the other leg is missing.
    def _one_row(legs):
        return {"status": "batch_ok",
                "results": [{"action": "placed", "rejection_reason": None}]}

    c.batch_response = _one_row
    r._resting = {}
    r.tick(_MID_SESSION + 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) == widened, \
        "a truncated row list decayed the crossing backoff"


def test_widened_quote_with_max_skew_still_stays_inside_the_ring():
    """'Retreating is free' must hold even when the pair is at max inventory skew.

    [review] ``test_the_widened_quote_still_earns_full_depth_credit`` runs with
    position=100 out of max_position≈17143, giving a skew of ~0.006% --
    effectively zero. That test cannot detect a bound that forgets the skew
    term. This one reproduces the exact condition where the bug lived:
    max-skew + max-backoff, quantised.

    With max_position_usd=7.0 and oracle=0.07, max_position=100 contracts, so
    position=100 is the fully-skewed case (|skew|=0.96%). The backoff is driven
    to its cap by repeated crossing refusals, then the final ask price is checked
    against the 2% credit ring measured from the TRUE oracle (not the skewed
    reference). Before the multiplicative fix the ask landed at 2.0100%;
    before the tick-reservation fix it landed at 2.0467%.
    """
    oracle = _ORACLE[_MKT]
    # [review] A SHORT just BELOW the cap, not a long AT it.
    #
    # The first version used +100 against a 100-contract cap, which hits
    # the limit exactly -> REDUCE_ONLY -> the sell leg alone. And for a
    # LONG the skew is negative, so that lone sell is the LEADING leg,
    # moving toward the oracle. The TRAILING leg -- the one whose skew and
    # offset compound outward, which is the entire failure mode -- was
    # never emitted, so the assertion held even under the old additive
    # bound. It could not have caught what it was written for.
    #
    # A short at 95 of 100 keeps both sides quoting and gives the ASK
    # near-maximum POSITIVE skew, which is the leg that leaves the ring.
    c = _Client(account=_account(-95.0), batch_response=_venue_cross((0, 1)))
    r = _runner(c, curfew_enabled=True, max_position_usd=7.0)
    for i in range(20):                       # drive the backoff to its cap
        r._resting = {}
        r.tick(_MID_SESSION + i * 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) > 0.0, \
        "backoff never fired -- test is not exercising the combined case"
    sides = {leg["side"] for leg in c.last_batch}
    assert sides == {"buy", "sell"}, (
        "expected a two-sided book so the TRAILING leg is exercised, got "
        "%r" % (sides,))
    assert max(float(l["price"]) for l in c.last_batch) > oracle,         "the ask is not the trailing leg -- positive skew was expected"
    for leg in c.last_batch:
        dev = abs(float(leg["price"]) / oracle - 1.0) * 100.0
        assert dev <= 2.0 + 1e-9, (
            "leg %r sits %.4f%% out at max skew+backoff -- outside the 2%% "
            "credit ring, it rests and earns nothing" % (leg, dev))

def test_a_failed_batch_envelope_does_not_decay_the_backoff():
    """[review] The ROWS describe legs; the ENVELOPE says whether the venue
    took them, and only the envelope can answer that.

    A known batch_failed response can still report every row as "placed",
    so without gating on `accepted` an explicit venue refusal decayed the
    learned offset exactly as though the batch had rested.
    """
    def crossed(legs):
        return {"status": "batch_ok",
                "results": [{"action": "placed", "rejection_reason":
                             "Post-only order would cross the book."}
                            for _ in legs]}

    c = _Client(account=_account(100.0), batch_response=crossed)
    r = _runner(c, curfew_enabled=True)
    r.tick(_MID_SESSION, _ORACLE, {})
    widened = r._cross_backoff.offset_pct(_MKT)
    assert widened > 0.0

    # Rows all look clean, but the ENVELOPE is an explicit failure.
    c.batch_response = lambda legs: {
        "status": "batch_failed",
        "results": [{"action": "placed"} for _ in legs]}
    r._resting = {}
    r.tick(_MID_SESSION + 5.0, _ORACLE, {})
    assert r._cross_backoff.offset_pct(_MKT) == widened,         "a batch_failed envelope with clean-looking rows decayed the backoff"


def test_junk_tick_metadata_cannot_disable_the_backoff():
    """[review] `float(raw or default)` is not a fallback: NaN is TRUTHY.

    A non-finite tick_size therefore sailed through, made the reservation
    NaN, and headroom_pct returned zero -- which makes observe_cross CLEAR
    the learned offset. Meanwhile quote_ladder validated and quietly used
    0.0001, so bad venue metadata disabled the crossing defence while the
    quote itself carried on as if nothing were wrong.
    """
    def crossed(legs):
        return {"status": "batch_ok",
                "results": [{"action": "placed", "rejection_reason":
                             "Post-only order would cross the book."}
                            for _ in legs]}

    c = _Client(account=_account(100.0), batch_response=crossed)
    r = _runner(c, curfew_enabled=True)
    flags = {"specs": {_MKT: {"tick_size": float("nan"), "lot_size": 1.0}}}
    r.tick(_MID_SESSION, _ORACLE, flags)
    assert r._cross_backoff.offset_pct(_MKT) > 0.0,         "junk tick metadata cleared the backoff instead of falling back"


def test_the_runner_APPLIES_the_backoff_cap_not_just_computes_it():
    """[review] The helper being correct proves nothing on its own.

    Capping at the LADDER is the part that matters. Coverage that only
    exercises _requote_safe_backoff leaves the call site free to apply the
    raw learned offset -- and removing the cap there kept all 625 tests
    green, which is exactly how this class of gap keeps surviving.
    """
    from gui.services.permuto.quoting import REQUOTE_AT_RING_FRACTION

    oracle = _ORACLE[_MKT]
    c = _Client(account=_account(0.0), batch_response=_venue_ok())
    r = _runner(c, curfew_enabled=False, max_position_usd=250_000.0)
    r._cross_backoff._pct[_MKT] = 5.0        # far past ring and trigger
    r.tick(_MID_SESSION, _ORACLE, {})
    assert c.last_batch, "no batch to inspect"

    trigger = 2.0 * REQUOTE_AT_RING_FRACTION
    for leg in c.last_batch:
        out = abs(float(leg["price"]) / oracle - 1.0) * 100.0
        assert out <= trigger + 1e-6, (
            "leg %r sits %.3f%% from the oracle, past the %.2f%% re-quote "
            "trigger -- the learned backoff was applied uncapped"
            % (leg, out, trigger))


def test_preflight_repricing_snaps_to_the_grid_under_junk_tick_metadata():
    """[review] "Decided once" was not true of the PRE-SEND path.

    _prepare parsed mspec["tick_size"] raw, so with NaN metadata and a
    fresh-oracle reprice quantise_toward received NaN and handed back the
    changed price UNSNAPPED -- an off-grid order going out despite the
    fallback that had been added for the ladder.
    """
    def fetch():
        # A COLLAPSE, not a nudge. preflight only re-prices a leg that has
        # fallen outside the band, so a 0.6% move leaves the ladder's own
        # on-grid prices untouched and the test proves nothing -- which is
        # how the first version of it passed with the raw tick restored.
        return {_MKT: 0.0700, _MKT2: 0.200}

    c = _Client(account=_account(0.0))
    r = _runner2(c, curfew_enabled=False, oracle_fetch=fetch)
    flags = {"specs": {_MKT: {"tick_size": float("nan"), "lot_size": 1.0}}}
    r.tick(_MID_SESSION, {_MKT: 0.0800, _MKT2: 0.200}, flags)
    assert c.last_batch, "no batch was sent"
    for leg in (l for l in c.last_batch if l["market"] == _MKT):
        ticks = float(leg["price"]) / 0.0001
        assert abs(round(ticks) - ticks) < 1e-6, (
            "leg %r is off the 0.0001 grid -- junk metadata bypassed the "
            "normalised tick on the preflight path" % (leg,))
