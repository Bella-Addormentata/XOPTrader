"""Reading an option back from the chain, and refusing one that is not sound.

The happy paths (live, sold, exercised, expired, clawed back) are also exercised
throughout ``test_builder.py``, which reloads the option after every step. This
module concentrates on the other half: what :func:`chain.load_option` does when
the chain data describes something that only looks like an option.

``Lying`` wraps the simulator and substitutes chosen answers, playing a node
that serves (or a counterparty that arranged) inconsistent data. Each test
breaks exactly one of the things CHIP-0045 says must be verified and expects the
option to be rejected, not priced.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, chain  # noqa: E402
from gui.services.options.chain import OptionStatus  # noqa: E402
from gui.services.options.tests.sim_world import CAT, XCH, World  # noqa: E402

HOUR = 3600


MISSING = object()  # a Lying answer meaning "the node has no such coin"


class Lying:
    """A chain reader that answers from the simulator unless told otherwise."""

    def __init__(self, sim: Any) -> None:
        self._sim = sim
        self.states: dict[bytes, Any] = {}
        self.spends: dict[bytes, Any] = {}
        self.broken: Callable[[], None] | None = None

    def coin_state(self, coin_id: bytes) -> Any:
        if self.broken is not None:
            self.broken()
        if bytes(coin_id) in self.states:
            answer = self.states[bytes(coin_id)]
            return None if answer is MISSING else answer
        return self._sim.coin_state(coin_id)

    def coin_spend(self, coin_id: bytes) -> Any:
        if self.broken is not None:
            self.broken()
        if bytes(coin_id) in self.spends:
            return self.spends[bytes(coin_id)]
        return self._sim.coin_spend(coin_id)


def _mint_call(
    world: World, maker, usd: bytes, *, strike_units: int = 120, expires_at: int | None = None
):
    """Mint a 100 XCH call. Each mint advances the simulated clock, so two options
    that must share an expiry need ``expires_at`` passed explicitly."""
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(usd, strike_units * CAT),
        world.now() + HOUR if expires_at is None else expires_at,
        maker.puzzle_hash,
    )
    minted = builder.build_mint(terms, maker.funding(), maker.custody())
    world.push(minted.coin_spends)
    return terms, minted


def _setup():
    world = World()
    maker, other = world.party(400), world.party(5)
    usd = world.issue_cat(other, 1000)
    return world, maker, other, usd


def _load(reader: Any, world: World, launcher_id: bytes) -> chain.LoadedOption:
    return chain.load_option(reader, launcher_id, now_seconds=world.now())


# ---------------------------------------------------------------------------
# Sound options
# ---------------------------------------------------------------------------


def test_status_follows_the_clock_passed_in() -> None:
    world, maker, _, usd = _setup()
    terms, minted = _mint_call(world, maker, usd)

    before = chain.load_option(
        world.sim, minted.launcher_id, now_seconds=terms.expiration_seconds - 1
    )
    at = chain.load_option(world.sim, minted.launcher_id, now_seconds=terms.expiration_seconds)

    assert before.status is OptionStatus.LIVE
    assert at.status is OptionStatus.EXPIRED
    # Nothing but the status depends on the clock.
    assert before.terms == at.terms == terms


def test_loaded_option_is_usable_as_is() -> None:
    """What comes back is enough to transfer the option with no other record."""
    world, maker, other, usd = _setup()
    _, minted = _mint_call(world, maker, usd)

    loaded = _load(world.sim, world, minted.launcher_id)
    moved = builder.build_transfer(
        loaded.option, other.puzzle_hash, maker.funding(), maker.custody()
    )
    world.push(moved.coin_spends)

    assert _load(world.sim, world, minted.launcher_id).owner_puzzle_hash == other.puzzle_hash


def test_put_reports_the_cat_as_underlying() -> None:
    world = World()
    maker = world.party(5)
    usd = world.issue_cat(maker, 500)
    terms = builder.OptionTerms(
        builder.Asset(usd, 115 * CAT),
        builder.Asset(None, 100 * XCH),
        world.now() + HOUR,
        maker.puzzle_hash,
    )
    minted = builder.build_mint(terms, maker.funding(usd), maker.custody())
    world.push(minted.coin_spends)

    loaded = _load(world.sim, world, minted.launcher_id)

    assert loaded.terms.underlying == builder.Asset(usd, 115 * CAT)
    assert loaded.terms.strike == builder.Asset(None, 100 * XCH)
    assert loaded.locked.cat is not None
    assert bytes(loaded.locked.cat.coin.coin_id()) == bytes(minted.locked.coin.coin_id())


def test_load_cat_rebuilds_a_spendable_cat() -> None:
    world = World()
    holder, other = world.party(5), world.party(1)
    usd = world.issue_cat(holder, 500)
    (coin,) = world.sim.unspent_coins(sdk.cat_puzzle_hash(usd, holder.puzzle_hash), False)

    cat = chain.load_cat(world.sim, coin.coin_id())

    assert bytes(cat.info.asset_id) == usd
    assert bytes(cat.info.p2_puzzle_hash) == holder.puzzle_hash
    assert cat.lineage_proof is not None
    # Spendable: lock it in a put, which needs a valid lineage proof.
    terms = builder.OptionTerms(
        builder.Asset(usd, 100 * CAT),
        builder.Asset(None, XCH),
        world.now() + HOUR,
        holder.puzzle_hash,
    )
    funding = builder.Funding(holder.puzzle_hash, xch=holder.xch_coins(), cats=[cat])
    world.push(builder.build_mint(terms, funding, holder.custody()).coin_spends)
    assert holder.balance(usd) == 400 * CAT
    assert other.balance(usd) == 0


def test_load_cat_refuses_a_plain_coin() -> None:
    world = World()
    holder = world.party(5)
    world.issue_cat(holder, 500)
    (xch_coin,) = holder.xch_coins()
    with pytest.raises(chain.ChainError, match="not a CAT"):
        chain.load_cat(world.sim, xch_coin.coin_id())


# ---------------------------------------------------------------------------
# Not an option at all
# ---------------------------------------------------------------------------


def test_unknown_launcher_id_is_invalid() -> None:
    world, _, _, _ = _setup()
    with pytest.raises(chain.InvalidOptionError, match="no spent launcher"):
        _load(world.sim, world, b"\x42" * 32)


def test_an_ordinary_coin_is_not_a_launcher() -> None:
    world, maker, _, usd = _setup()
    (coin,) = maker.xch_coins()
    _mint_call(world, maker, usd)  # spends ``coin``
    with pytest.raises(chain.InvalidOptionError, match="not a singleton launcher"):
        _load(world.sim, world, coin.coin_id())


def test_another_kind_of_singleton_is_not_an_option() -> None:
    """A DID is launched the same way but carries no option metadata."""
    world, maker, _, _ = _setup()
    clvm = sdk.Clvm()
    spends = sdk.Spends(clvm, maker.puzzle_hash)
    for coin in maker.xch_coins():
        spends.add_xch(coin)
    custody = maker.custody()
    deltas = spends.apply([sdk.Action.create_empty_did()])
    finished = spends.prepare(deltas, sdk.Relation.AssertConcurrent)
    for pending in finished.pending_spends():
        finished.insert(
            pending.coin().coin_id(),
            custody.spend(clvm, pending.p2_puzzle_hash(), pending.conditions()),
        )
    outputs = finished.spend()
    world.push(clvm.coin_spends())
    did = outputs.did(outputs.dids()[0])

    with pytest.raises(chain.InvalidOptionError):
        _load(world.sim, world, did.info.launcher_id)


# ---------------------------------------------------------------------------
# Looks like an option, but a CHIP-0045 check fails
# ---------------------------------------------------------------------------


def test_metadata_with_a_cheaper_strike_is_rejected() -> None:
    """The singleton commits to the real strike; metadata that disagrees is a lie.

    The lock puzzle does not depend on the strike, so this check is the only
    thing standing between a buyer and an option advertised at a strike it will
    never honour.
    """
    world, maker, _, usd = _setup()
    expiry = world.now() + HOUR
    _, real = _mint_call(world, maker, usd, strike_units=120, expires_at=expiry)
    _, bait = _mint_call(world, maker, usd, strike_units=1, expires_at=expiry)
    clvm = sdk.Clvm()
    real_launch = world.sim.coin_spend(real.launcher_id)
    bait_launch = world.sim.coin_spend(bait.launcher_id)
    real_fields = clvm.deserialize(bytes(real_launch.solution)).to_list()
    bait_fields = clvm.deserialize(bytes(bait_launch.solution)).to_list()
    forged = clvm.list([real_fields[0], real_fields[1], bait_fields[2]])
    reader = Lying(world.sim)
    reader.spends[real.launcher_id] = sdk.CoinSpend(
        real_launch.coin, bytes(real_launch.puzzle_reveal), bytes(forged.serialize())
    )

    with pytest.raises(chain.InvalidOptionError, match="does not commit to the strike"):
        _load(reader, world, real.launcher_id)
    # Untampered, the same option is fine.
    assert _load(world.sim, world, real.launcher_id).terms.strike.amount == 120 * CAT


def test_metadata_with_a_later_expiry_is_rejected() -> None:
    """The lock puzzle commits to the expiry, so a false one changes its hash."""
    world, maker, _, usd = _setup()
    _, real = _mint_call(world, maker, usd, expires_at=world.now() + HOUR)
    _, bait = _mint_call(world, maker, usd, expires_at=world.now() + 30 * 24 * HOUR)
    clvm = sdk.Clvm()
    real_launch = world.sim.coin_spend(real.launcher_id)
    bait_launch = world.sim.coin_spend(bait.launcher_id)
    real_fields = clvm.deserialize(bytes(real_launch.solution)).to_list()
    bait_fields = clvm.deserialize(bytes(bait_launch.solution)).to_list()
    forged = clvm.list([real_fields[0], real_fields[1], bait_fields[2]])
    reader = Lying(world.sim)
    reader.spends[real.launcher_id] = sdk.CoinSpend(
        real_launch.coin, bytes(real_launch.puzzle_reveal), bytes(forged.serialize())
    )

    with pytest.raises(chain.InvalidOptionError, match="not locked by the option's terms"):
        _load(reader, world, real.launcher_id)


def test_a_false_creator_hint_is_rejected() -> None:
    """The hint is believed only because the lock puzzle commits to the creator."""
    world, maker, other, usd = _setup()
    _, real = _mint_call(world, maker, usd)
    # A second option minted by somebody else supplies a different creator hint.
    theirs = builder.OptionTerms(
        builder.Asset(None, XCH), builder.Asset(usd, CAT), world.now() + HOUR, other.puzzle_hash
    )
    foreign = builder.build_mint(theirs, other.funding(), other.custody())
    world.push(foreign.coin_spends)
    real_launch = world.sim.coin_spend(real.launcher_id)
    foreign_launch = world.sim.coin_spend(foreign.launcher_id)
    reader = Lying(world.sim)
    reader.spends[bytes(real_launch.coin.parent_coin_info)] = world.sim.coin_spend(
        foreign_launch.coin.parent_coin_info
    )

    with pytest.raises(chain.InvalidOptionError, match="not locked by the option's terms"):
        _load(reader, world, real.launcher_id)


def test_a_launcher_without_a_creator_hint_is_rejected() -> None:
    world, maker, _, usd = _setup()
    _, real = _mint_call(world, maker, usd)
    launch = world.sim.coin_spend(real.launcher_id)
    # Serve some unrelated spend as the launcher's parent: it creates no launcher.
    reader = Lying(world.sim)
    reader.spends[bytes(launch.coin.parent_coin_info)] = launch

    with pytest.raises(chain.InvalidOptionError, match="no creator hint"):
        _load(reader, world, real.launcher_id)


def test_a_missing_underlying_coin_is_rejected() -> None:
    world, maker, _, usd = _setup()
    _, real = _mint_call(world, maker, usd)
    reader = Lying(world.sim)
    reader.states[bytes(real.locked.coin.coin_id())] = MISSING

    with pytest.raises(chain.InvalidOptionError, match="does not exist"):
        _load(reader, world, real.launcher_id)


def test_a_substituted_underlying_coin_is_a_chain_error() -> None:
    """A node answering with a different coin than the id asked for is not believed."""
    world, maker, _, usd = _setup()
    _, real = _mint_call(world, maker, usd)
    _, decoy = _mint_call(world, maker, usd)
    reader = Lying(world.sim)
    reader.states[bytes(real.locked.coin.coin_id())] = world.sim.coin_state(
        decoy.locked.coin.coin_id()
    )

    with pytest.raises(chain.ChainError, match="different coin"):
        _load(reader, world, real.launcher_id)


def test_a_substituted_launcher_is_a_chain_error() -> None:
    world, maker, _, usd = _setup()
    _, real = _mint_call(world, maker, usd)
    _, decoy = _mint_call(world, maker, usd)
    reader = Lying(world.sim)
    reader.spends[real.launcher_id] = world.sim.coin_spend(decoy.launcher_id)

    with pytest.raises(chain.ChainError, match="different coin"):
        _load(reader, world, real.launcher_id)


def test_node_failures_are_chain_errors_not_verdicts() -> None:
    """An unreachable node says nothing about the option; it must not read as invalid."""
    world, maker, _, usd = _setup()
    _, real = _mint_call(world, maker, usd)
    reader = Lying(world.sim)

    def down() -> None:
        raise ConnectionError("node unreachable")

    reader.broken = down
    with pytest.raises(chain.ChainError, match="unreachable"):
        _load(reader, world, real.launcher_id)
    with pytest.raises(chain.ChainError, match="unreachable"):
        chain.load_cat(reader, real.locked.coin.coin_id())


# ---------------------------------------------------------------------------
# NodeChainReader
# ---------------------------------------------------------------------------


class _Record:
    def __init__(self, state: Any) -> None:
        coin = state.coin
        self.coin = type(
            "Coin",
            (),
            {
                "parent_coin_info": bytes(coin.parent_coin_info).hex(),
                "puzzle_hash": "0x" + bytes(coin.puzzle_hash).hex(),
                "amount": int(coin.amount),
            },
        )()
        self.confirmed_block_index = int(state.created_height or 0)
        self.spent_block_index = int(state.spent_height or 0)
        self.spent = state.spent_height is not None


class _NodeClient:
    """A full-node style client (hex in, records out) over the simulator."""

    def __init__(self, sim: Any) -> None:
        self._sim = sim
        self.heights: list[int] = []

    def get_coin_record_by_name(self, name: str) -> Any:
        state = self._sim.coin_state(bytes.fromhex(name))
        return None if state is None else _Record(state)

    def get_puzzle_and_solution(self, coin_id: str, height: int) -> Any:
        self.heights.append(height)
        spend = self._sim.coin_spend(bytes.fromhex(coin_id))
        return type(
            "Solution",
            (),
            {
                "puzzle_reveal": bytes(spend.puzzle_reveal).hex(),
                "solution": "0x" + bytes(spend.solution).hex(),
            },
        )()


def test_node_chain_reader_loads_an_option_through_a_node_client() -> None:
    world, maker, _, usd = _setup()
    terms, minted = _mint_call(world, maker, usd)
    client = _NodeClient(world.sim)

    loaded = _load(chain.NodeChainReader(client), world, minted.launcher_id)

    assert loaded.status is OptionStatus.LIVE
    assert loaded.terms == terms
    assert loaded.owner_puzzle_hash == maker.puzzle_hash
    # Solutions are asked for at the height the coin was spent.
    assert client.heights and all(height > 0 for height in client.heights)


def test_node_chain_reader_reports_unknown_and_unspent_coins() -> None:
    world, maker, _, _ = _setup()
    reader = chain.NodeChainReader(_NodeClient(world.sim))
    (coin,) = maker.xch_coins()

    assert reader.coin_state(b"\x42" * 32) is None
    assert reader.coin_spend(b"\x42" * 32) is None
    state = reader.coin_state(coin.coin_id())
    assert state.spent_height is None
    assert int(state.coin.amount) == int(coin.amount)
    # An unspent coin has no spend to return.
    assert reader.coin_spend(coin.coin_id()) is None


def test_node_chain_reader_wraps_client_failures() -> None:
    class Down:
        def get_coin_record_by_name(self, name: str) -> Any:
            raise TimeoutError("timed out")

    with pytest.raises(chain.ChainError, match="timed out"):
        chain.NodeChainReader(Down()).coin_state(b"\x01" * 32)
