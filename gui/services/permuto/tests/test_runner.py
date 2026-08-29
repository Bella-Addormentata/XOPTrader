"""The sequencer. Fake client, real policy modules underneath."""

from __future__ import annotations

import pytest

from gui.services.permuto.auth import PermutoAuthError
from gui.services.permuto.client import PermutoNotLinked
from gui.services.permuto.quoting import RestingQuote
from gui.services.permuto.risk import FLATTEN_MARGIN_UTILISATION
from gui.services.permuto.runner import QuoteRunner, _margin_state
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


def test_a_pause_does_not_spend_a_session_renewal():
    """Nothing can be placed while paused; renewing is a wasted round trip."""
    c = _Client()
    _runner(c).tick(1.0, _ORACLE, {"trading_paused": True})
    assert "ensure_session" not in c.calls


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
    assert not r.tick(2.0, _ORACLE, {"trading_paused": False}).ok
    assert r._reopen_pending, "the rebuild we owe the book was dropped"

    c.fail_on = None
    assert r.tick(3.0, _ORACLE, {"trading_paused": False}).action == "quote"


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
    assert result.action == "hold"
    assert "batch_upsert" not in c.calls


def test_reduce_only_keeps_only_the_shrinking_side():
    c = _Client(account={
        "equity_usd": 100_000.0, "used_margin_usd": 0.0,
        "positions": {_MKT: 100.0},
    })
    r = _runner(c, max_position=100.0)
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
    _runner(long_book, max_position=100.0).tick(1.0, _ORACLE, {})

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
    c = _Client(fail_on=where)
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
    r = _runner(c, max_position=100.0)
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
    r = _runner(c, max_position=100.0)
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
