"""How often the chain's node is asked.

The node behind the book is usually coinset.org, so requests are counted here
and held to numbers. ``Node`` stands in for it: the state of any number of
coins in one request, one spend per request, and a log of both.

Two things are tested. :class:`chain.CachedChain` on its own: what it keeps,
what it asks for together, and when a kept spend stops being trusted. Then the
book's policy over it: a step with nothing to notice asks nothing, and a sale,
an exercise or an expiry is still noticed at once.

For scale: before the cache, one idle step over six open contracts made 63
requests, every step.
"""

from __future__ import annotations

from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, chain, store  # noqa: E402
from gui.services.options.service import OptionsBook  # noqa: E402
from gui.services.options.tests import test_service as ts  # noqa: E402
from gui.services.options.tests.sim_world import World  # noqa: E402
from gui.services.options.tests.test_chain import _mint_call, _setup  # noqa: E402

DAY = 86400


class Node:
    """A full node as coinset serves one, with every request written down."""

    def __init__(self, sim: Any) -> None:
        self.sim = sim
        self.log: list[tuple[str, int]] = []

    def coin_states(self, coin_ids: Any) -> dict[bytes, Any]:
        wanted = [bytes(coin_id) for coin_id in coin_ids]
        self.log.append(("states", len(wanted)))
        found = {coin_id: self.sim.coin_state(coin_id) for coin_id in wanted}
        return {coin_id: state for coin_id, state in found.items() if state is not None}

    def spend_at(self, coin: Any, spent_height: int) -> Any:
        self.log.append(("spend", 1))
        return self.sim.coin_spend(coin.coin_id())

    def coin_state(self, coin_id: bytes) -> Any:
        raise AssertionError("asked for one coin when several could travel together")

    def coin_spend(self, coin_id: bytes) -> Any:
        raise AssertionError("looked a spend up without the height already known")

    @property
    def requests(self) -> int:
        return len(self.log)


def _reader(world: World, facts: Any = None) -> tuple[Node, chain.CachedChain, list[float]]:
    node, now = Node(world.sim), [1_000.0]
    return node, chain.CachedChain(node, facts, clock=lambda: now[0]), now


def _load(reader: chain.CachedChain, world: World, launcher_id: bytes) -> chain.LoadedOption:
    return chain.load_option(reader, launcher_id, now_seconds=world.now())


# ---------------------------------------------------------------------------
# The cache
# ---------------------------------------------------------------------------


def test_within_one_look_nothing_is_asked_twice() -> None:
    world, maker, _, usd = _setup()
    _, minted = _mint_call(world, maker, usd)
    node, reader, _ = _reader(world)

    first = _load(reader, world, minted.launcher_id)
    asked = node.requests
    again = _load(reader, world, minted.launcher_id)

    assert asked > 1  # reading an option for the first time takes several requests
    assert node.requests == asked
    assert again.terms == first.terms and again.status is first.status
    assert reader.requests == node.requests


def test_a_new_look_asks_for_everything_watched_in_one_request() -> None:
    world, maker, _, usd = _setup()
    _, one = _mint_call(world, maker, usd)
    _, two = _mint_call(world, maker, usd)
    node, reader, _ = _reader(world)
    for minted in (one, two):
        _load(reader, world, minted.launcher_id)
    asked = node.requests

    reader.begin()
    for minted in (one, two):
        _load(reader, world, minted.launcher_id)

    # Two options, one request: the coins of both travel together, and no spend
    # that was already read is read again.
    (request,) = node.log[asked:]
    assert request[0] == "states" and request[1] >= 4


def test_a_look_sees_what_changed() -> None:
    world, maker, taker, usd = _setup()
    _, minted = _mint_call(world, maker, usd)
    node, reader, _ = _reader(world)
    assert _load(reader, world, minted.launcher_id).owner_puzzle_hash == maker.puzzle_hash

    world.push(
        builder.build_transfer(
            minted.option, taker.puzzle_hash, maker.funding(), maker.custody()
        ).coin_spends
    )

    # Without a new look the answer is the last one's, at no cost...
    asked = node.requests
    assert _load(reader, world, minted.launcher_id).owner_puzzle_hash == maker.puzzle_hash
    assert node.requests == asked
    # ...and a new look finds the option moved.
    reader.begin()
    assert _load(reader, world, minted.launcher_id).owner_puzzle_hash == taker.puzzle_hash


def test_settled_spends_are_never_asked_for_again_not_even_by_a_new_process() -> None:
    world, maker, _, usd = _setup()
    _, minted = _mint_call(world, maker, usd)
    facts = store.OptionStore(":memory:")
    node, reader, now = _reader(world, facts)
    _load(reader, world, minted.launcher_id)

    now[0] += 1_000  # seen spent again a quarter of an hour on: beyond a reorg
    reader.begin()
    _load(reader, world, minted.launcher_id)
    asked = node.requests
    reader.begin()
    _load(reader, world, minted.launcher_id)

    # Only the option's own coin and its collateral can still change.
    assert node.log[asked:] == [("states", 2)]

    restarted = Node(world.sim)
    fresh = chain.CachedChain(restarted, facts, clock=lambda: now[0])
    loaded = _load(fresh, world, minted.launcher_id)

    assert loaded.status is chain.OptionStatus.LIVE
    assert {kind for kind, _ in restarted.log} == {"states"}
    assert sum(count for _, count in restarted.log) == 2


def test_a_spend_is_kept_from_its_first_reading_but_trusted_only_once_settled() -> None:
    world, maker, _, usd = _setup()
    _, minted = _mint_call(world, maker, usd)
    facts = store.OptionStore(":memory:")
    node, reader, now = _reader(world, facts)
    _load(reader, world, minted.launcher_id)
    launcher = facts.chain_fact(minted.launcher_id)
    assert launcher is not None and not launcher.final and launcher.puzzle_reveal is not None

    # A new process five minutes later: the spends need not be read again, but
    # that they still stand is checked, because they are not settled yet.
    now[0] += 300
    restarted = Node(world.sim)
    fresh = chain.CachedChain(restarted, facts, clock=lambda: now[0])
    _load(fresh, world, minted.launcher_id)

    assert "spend" not in {kind for kind, _ in restarted.log}
    assert not facts.chain_fact(minted.launcher_id).final
    now[0] += 700
    fresh.begin()
    _load(fresh, world, minted.launcher_id)
    assert facts.chain_fact(minted.launcher_id).final


class Scripted:
    """A node whose answers the test sets by hand."""

    def __init__(self) -> None:
        self.states: dict[bytes, Any] = {}
        self.spends: dict[tuple[bytes, int], Any] = {}
        self.log: list[str] = []

    def coin_states(self, coin_ids: Any) -> dict[bytes, Any]:
        self.log.append("states")
        return {bytes(i): self.states[bytes(i)] for i in coin_ids if bytes(i) in self.states}

    def spend_at(self, coin: Any, spent_height: int) -> Any:
        self.log.append("spend")
        return self.spends[bytes(coin.coin_id()), spent_height]


def test_a_spend_a_reorg_moved_is_read_again_and_its_clock_restarts() -> None:
    coin = sdk.Coin(b"\x01" * 32, b"\x02" * 32, 5)
    coin_id = bytes(coin.coin_id())
    node, now = Scripted(), [1_000.0]
    reader = chain.CachedChain(node, clock=lambda: now[0], final_after_seconds=900)
    node.states[coin_id] = sdk.CoinState(coin, 100, 90)
    node.spends[coin_id, 100] = sdk.CoinSpend(coin, b"\x01", b"\x80")
    assert bytes(reader.coin_spend(coin_id).solution) == b"\x80"

    now[0] += 600
    node.states[coin_id] = sdk.CoinState(coin, 103, 90)  # the same coin, spent in another block
    node.spends[coin_id, 103] = sdk.CoinSpend(coin, b"\x01", b"\x01")
    reader.begin()

    assert reader.coin_state(coin_id).spent_height == 103
    assert bytes(reader.coin_spend(coin_id).solution) == b"\x01"

    # 1,200 seconds after it was first seen spent, but only 600 after it moved:
    # not settled yet, so still looked at.
    now[0] += 600
    reader.begin()
    asked = len(node.log)
    reader.coin_state(coin_id)
    assert node.log[asked:] == ["states"]

    now[0] += 400
    reader.begin()
    reader.coin_state(coin_id)  # this sighting settles it
    reader.begin()
    asked = len(node.log)
    assert reader.coin_state(coin_id).spent_height == 103
    assert bytes(reader.coin_spend(coin_id).solution) == b"\x01"
    assert node.log[asked:] == []


def test_a_spend_a_reorg_undid_is_forgotten() -> None:
    coin = sdk.Coin(b"\x01" * 32, b"\x02" * 32, 5)
    coin_id = bytes(coin.coin_id())
    node = Scripted()
    reader = chain.CachedChain(node)
    node.states[coin_id] = sdk.CoinState(coin, 100, 90)
    node.spends[coin_id, 100] = sdk.CoinSpend(coin, b"\x01", b"\x80")
    assert reader.coin_spend(coin_id) is not None

    node.states[coin_id] = sdk.CoinState(coin, None, 90)
    reader.begin()

    assert reader.coin_state(coin_id).spent_height is None
    assert reader.coin_spend(coin_id) is None


def test_a_coin_not_on_chain_is_none_and_is_looked_for_again_next_time() -> None:
    world, _, _, _ = _setup()
    node, reader, _ = _reader(world)
    missing = b"\x42" * 32

    assert reader.coin_state(missing) is None and reader.coin_spend(missing) is None
    assert node.log == [("states", 1)]
    reader.begin()
    assert reader.coin_state(missing) is None
    assert node.log == [("states", 1), ("states", 1)]


def test_a_bundle_s_coins_are_asked_for_together() -> None:
    world, maker, _, usd = _setup()
    _, minted = _mint_call(world, maker, usd)
    node, reader, _ = _reader(world)

    status = chain.bundle_status(reader, minted.coin_spends)

    assert status is chain.BundleStatus.CONFIRMED
    assert node.log[0] == ("states", len(minted.coin_spends))
    assert node.log.count(("spend", 1)) == len(minted.coin_spends)


def test_a_reader_with_no_way_to_ask_together_is_asked_coin_by_coin() -> None:
    world, maker, _, usd = _setup()
    _, minted = _mint_call(world, maker, usd)
    reader = chain.CachedChain(world.sim)  # the simulator answers one coin at a time

    loaded = chain.load_option(reader, minted.launcher_id, now_seconds=world.now())

    assert loaded.status is chain.OptionStatus.LIVE and reader.requests > 0
    asked = reader.requests
    chain.load_option(reader, minted.launcher_id, now_seconds=world.now())
    assert reader.requests == asked


def test_a_reader_that_already_caches_is_not_wrapped_again() -> None:
    world, _, _, _ = _setup()
    reader = chain.CachedChain(world.sim)
    assert chain.CachedChain.over(reader, None) is reader
    assert isinstance(chain.CachedChain.over(world.sim, None), chain.CachedChain)


# ---------------------------------------------------------------------------
# The book: when it looks
# ---------------------------------------------------------------------------


class Desk(ts.Rig):
    """The service rig with the chain-reading policy at its defaults."""

    def __init__(self, **overrides: Any) -> None:
        settings = {"chain_poll_seconds": 30, "chain_refresh_seconds": 900, **overrides}
        super().__init__(**settings)
        self.node = Node(self.world.sim)
        self.book = self.make_book()

    def make_book(self) -> OptionsBook:
        return OptionsBook(
            self.config,
            book_store=self.store,
            wallet=self.wallet,
            coins=ts.SimCoins(self.maker),
            chain_reader=getattr(self, "node", self.world.sim),
            market=self.read_market,
            desk=self.desk,
            clock=self.world.now,
        )

    def run(self, steps: int, every: int = 60) -> list[Any]:
        reports = []
        for _ in range(steps):
            reports.append(self.book.step())
            self.world.advance(every)
        return reports


def _open_book(contracts: int = 2, **overrides: Any) -> Desk:
    desk = Desk(**overrides)
    desk.run(5)
    assert [item.status for item in desk.contracts()] == [store.OPEN] * contracts
    assert len(desk.store.offers(status="live")) == contracts
    return desk


def test_a_step_with_nothing_to_notice_asks_the_chain_nothing() -> None:
    desk = _open_book()
    asked = desk.node.requests

    reports = desk.run(30)  # half an hour, a step a minute

    assert [report.problems for report in reports] == [[]] * 30
    # Offers were re-posted as they ran out; that needs no chain.
    assert len(desk.store.offers()) > 2
    looks = [report for report in reports if report.chain_looked]
    assert {report.chain_looked for report in looks} == {"routine"} and len(looks) == 2
    # A routine look is one request, however many coins are watched. The first
    # still re-reads the mints' coins; by the second they have settled and only
    # each contract's option coin and collateral are left to watch.
    assert [report.chain_requests for report in looks] == [1, 1]
    first, second = desk.node.log[asked:]
    assert first[0] == second[0] == "states" and first[1] > second[1] == 4
    assert sum(report.chain_requests for report in reports) == 2


def test_a_sale_is_noticed_in_the_next_step_from_the_wallet_alone() -> None:
    desk = _open_book()
    call = ts._by_side(desk, "call", store.OPEN)
    offer = desk.live_offer(call)
    desk.run(3)  # quiet
    desk.world.take(desk.taker, offer.offer_text)

    (report,) = desk.run(1)

    assert report.chain_looked == "the wallet's coins changed"
    assert desk.store.contract(call.launcher_id).status == store.SOLD
    (sold,) = desk.events("sold")
    assert sold.detail["amount"] == offer.price_amount
    assert report.chain_requests <= 6
    # The slot is refilled, confirmed and on offer again within a few steps.
    desk.run(3)
    replacement = ts._by_side(desk, "call", store.OPEN)
    assert replacement.launcher_id != call.launcher_id
    assert desk.store.offers(replacement.launcher_id, "live")


def test_an_exercise_is_noticed_in_the_next_step_from_the_wallet_alone() -> None:
    desk = _open_book()
    call = ts._by_side(desk, "call", store.OPEN)
    desk.world.take(desk.taker, desk.live_offer(call).offer_text)
    desk.run(6)
    loaded = desk.world.load(call.launcher_id)
    desk.world.push(
        builder.build_exercise(
            loaded.option,
            loaded.underlying,
            loaded.locked,
            desk.taker.funding(desk.usd),
            desk.taker.custody(),
        )
    )

    (report,) = desk.run(1)

    assert report.chain_looked == "the wallet's coins changed"  # the strike payment arrived
    assert desk.store.contract(call.launcher_id).status == store.EXERCISED
    assert report.chain_requests <= 4


def test_a_waiting_transaction_is_followed_at_the_poll_interval() -> None:
    desk = Desk(put_strikes_pct=[])
    desk.wallet.push = ts._unheard()  # type: ignore[method-assign]
    desk.run(1, every=10)  # the mint is signed; no node hears of it
    asked = desk.node.requests

    reports = desk.run(9, every=10)  # ninety seconds, a step every ten

    looked = [report.chain_looked for report in reports]
    assert looked.count("a transaction is waiting") == 3  # at 30, 60 and 90 seconds
    assert set(looked) == {"", "a transaction is waiting"}
    # One request for the bundle's coins each time, and one when it was first asked about.
    assert {kind for kind, _ in desk.node.log[asked:]} == {"states"}
    assert desk.node.requests - asked == 4


def test_a_stalled_transaction_is_only_looked_at_on_the_routine_interval() -> None:
    desk = Desk(put_strikes_pct=[], confirm_timeout_seconds=600)
    real_push = desk.wallet.push
    desk.wallet.push = ts._unheard()  # type: ignore[method-assign]
    desk.run(12)  # past the deadline: flagged as stalled
    assert desk.store.broadcasts("stalled")
    asked = desk.node.requests

    reports = desk.run(30)  # half an hour more

    assert {report.chain_looked for report in reports} == {"", "routine"}
    assert desk.node.requests - asked == 2
    # It is still pushed every step, which costs the chain's node nothing, so
    # when a node hears it the wallet's coins change and it is noticed at once.
    desk.wallet.push = real_push  # type: ignore[method-assign]
    desk.run(1)
    (report,) = desk.run(1)
    assert report.chain_looked == "the wallet's coins changed"
    assert [item.status for item in desk.contracts()] == [store.OPEN]


def test_reaching_expiry_is_a_reason_to_look() -> None:
    desk = _open_book(1, put_strikes_pct=[], limits={"max_contracts": 1})
    (call,) = desk.contracts()
    desk.world.take(desk.taker, desk.live_offer(call).offer_text)
    desk.run(2)
    assert desk.store.contract(call.launcher_id).status == store.SOLD
    desk.world.advance(14 * DAY)

    first, second = desk.run(2)

    assert first.chain_looked == "a contract reached expiry"
    assert any("clawback" in action for action in first.actions)
    assert second.chain_looked == "the wallet's coins changed"  # the collateral came home
    assert desk.store.contract(call.launcher_id).status == store.CLOSED


def test_the_wallet_being_away_does_not_stop_the_chain_being_read() -> None:
    desk = _open_book()

    def away(asset_ids: Any) -> Any:
        raise RuntimeError("wallet RPC refused the connection")

    desk.book._coins.held = away  # type: ignore[method-assign]
    desk.world.advance(900)

    (report,) = desk.run(1)

    assert any("could not list the wallet's coins" in problem for problem in report.problems)
    assert report.chain_looked == "routine" and report.chain_requests == 1


def test_refresh_and_the_kill_switch_always_look() -> None:
    desk = _open_book()
    desk.run(2)

    assert desk.book.refresh().chain_looked == "asked to"
    report = desk.book.withdraw_all()
    assert report.chain_looked == "asked to"
    assert sum("withdrawing offers" in action for action in report.actions) == 2


def test_a_restarted_book_reads_the_chain_once_and_carries_on() -> None:
    desk = _open_book()
    desk.run(20)  # long enough for the mints to settle

    desk.node = Node(desk.world.sim)  # a new process: nothing in memory
    desk.book = desk.make_book()
    (first,) = desk.run(1)
    rest = desk.run(5)

    assert first.chain_looked == "first look"
    # What settled is in the database; only what can still change is asked for.
    assert {kind for kind, _ in desk.node.log} == {"states"}
    assert sum(report.chain_requests for report in rest) == 0
    assert len(desk.contracts(store.OPEN)) == 2


def test_six_hours_of_a_book_with_a_sale_cost_a_few_dozen_requests() -> None:
    desk = _open_book()
    asked = desk.node.requests
    desk.run(120)
    call = ts._by_side(desk, "call", store.OPEN)
    desk.world.take(desk.taker, desk.live_offer(call).offer_text)

    reports = desk.run(240)

    assert all(report.problems == [] for report in reports)
    assert desk.store.contract(call.launcher_id).status == store.SOLD
    assert len(desk.contracts(store.OPEN)) == 2  # the slot was refilled
    # 360 steps. Before the cache each took about ten requests per contract.
    assert desk.node.requests - asked <= 60
