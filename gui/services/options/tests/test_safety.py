"""The net-effect check: what a bundle does, read from the bundle alone."""

from __future__ import annotations

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, safety  # noqa: E402
from gui.services.options.tests.sim_world import CAT, XCH, World  # noqa: E402

HOUR = 3600


def _call(world: World, maker, usd: bytes, *, fee: int = 0):
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(usd, 120 * CAT),
        world.now() + HOUR,
        maker.puzzle_hash,
    )
    return builder.build_mint(terms, maker.funding(), maker.custody(), fee=fee)


def _setup():
    world = World()
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    world.send_cat(taker, maker.puzzle_hash, usd, 5)
    return world, maker, taker, usd


def test_mint_locks_the_collateral_and_pays_only_the_fee() -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd, fee=1000)
    lock = bytes(minted.underlying.puzzle_hash())

    effect = safety.analyse(minted.coin_spends, ours=[maker.puzzle_hash], locks=[lock])

    assert effect.wallet == {None: -(100 * XCH + 1 + 1000)}
    assert effect.locked == {None: 100 * XCH}
    assert effect.other == {None: 1}  # the option coin itself
    assert effect.fee == 1000
    safety.require(
        effect,
        safety.Expected(
            wallet={None: -(100 * XCH + 1 + 1000)},
            locked={None: 100 * XCH},
            other_max={None: 1},
            max_fee=1000,
        ),
    )


def test_put_mint_locks_the_cat() -> None:
    world = World()
    maker = world.party(5)
    usd = world.issue_cat(maker, 500)
    terms = builder.OptionTerms(
        builder.Asset(usd, 115 * CAT),
        builder.Asset(None, 100 * XCH),
        world.now() + HOUR,
        maker.puzzle_hash,
    )
    minted = builder.build_mint(terms, maker.funding(usd), maker.custody(), fee=10)

    effect = safety.analyse(
        minted.coin_spends,
        ours=[maker.puzzle_hash],
        locks=[bytes(minted.underlying.puzzle_hash())],
    )

    assert effect.wallet == {None: -11, usd: -115 * CAT}
    assert effect.locked == {usd: 115 * CAT}
    assert effect.fee == 10


def test_offer_moves_nothing_of_ours() -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd)
    world.push(minted.coin_spends)
    draft = builder.build_offer(
        minted.option, builder.Asset(usd, 2100), maker.puzzle_hash, maker.custody()
    )

    effect = safety.analyse(draft.coin_spends, ours=[maker.puzzle_hash])

    assert effect.wallet == {} and effect.locked == {} and effect.fee == 0
    assert effect.other == {None: 1}
    safety.require(effect, safety.Expected(other_max={None: 1}))


def test_cancel_and_clawback_bring_the_collateral_home() -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd)
    world.push(minted.coin_spends)
    lock = bytes(minted.underlying.puzzle_hash())

    cancel = builder.build_cancel(
        minted.option,
        minted.underlying,
        minted.locked,
        maker.funding(usd),
        maker.custody(),
        fee=700,
    )
    effect = safety.analyse(cancel, ours=[maker.puzzle_hash], locks=[lock])
    assert effect.wallet == {None: 100 * XCH + 1 - 700}
    assert effect.locked == {None: -100 * XCH}
    assert effect.other == {}

    world.advance(HOUR)
    clawback = builder.build_clawback(
        minted.underlying,
        minted.locked,
        maker.pk,
        builder.Funding(maker.puzzle_hash),
        maker.custody(),
        fee=300,
    )
    effect = safety.analyse(clawback, ours=[maker.puzzle_hash], locks=[lock])
    assert effect.wallet == {None: 100 * XCH - 300}
    assert effect.locked == {None: -100 * XCH}


def test_exercise_by_a_buyer_pays_the_creator() -> None:
    """Seen from the buyer: the strike leaves, the underlying arrives."""
    world, maker, taker, usd = _setup()
    minted = _call(world, maker, usd)
    world.push(minted.coin_spends)
    draft = builder.build_offer(
        minted.option, builder.Asset(usd, 2100), maker.puzzle_hash, maker.custody()
    )
    world.take(taker, draft.encode(world.signer().sign(draft.coin_spends)))
    loaded = world.load(minted.launcher_id)
    exercise = builder.build_exercise(
        loaded.option, loaded.underlying, loaded.locked, taker.funding(usd), taker.custody()
    )

    effect = safety.analyse(exercise, ours=[taker.puzzle_hash])

    assert effect.wallet == {None: 100 * XCH + 1, usd: -120 * CAT}
    assert effect.other == {usd: 120 * CAT}  # the strike, to the creator
    with pytest.raises(safety.SafetyError, match="not ours"):
        safety.require(effect, safety.Expected(wallet=effect.wallet))


def test_a_payment_to_a_stranger_is_refused() -> None:
    world, maker, taker, usd = _setup()
    # A transfer to somebody else, declared as a harmless withdraw-to-self.
    minted = _call(world, maker, usd)
    world.push(minted.coin_spends)
    clvm = sdk.Clvm()
    spends = sdk.Spends(clvm, maker.puzzle_hash)
    for coin in maker.xch_coins():
        spends.add_xch(coin)
    custody = maker.custody()
    deltas = spends.apply([sdk.Action.send(sdk.Id.xch(), taker.puzzle_hash, 5 * XCH, None)])
    finished = spends.prepare(deltas, sdk.Relation.AssertConcurrent)
    for pending in finished.pending_spends():
        finished.insert(
            pending.coin().coin_id(),
            custody.spend(clvm, pending.p2_puzzle_hash(), pending.conditions()),
        )
    finished.spend()

    effect = safety.analyse(clvm.coin_spends(), ours=[maker.puzzle_hash])

    assert effect.wallet == {None: -5 * XCH}
    assert effect.other == {None: 5 * XCH}
    with pytest.raises(safety.SafetyError, match="wallet"):
        safety.require(effect, safety.Expected(other_max={None: 1}))


@pytest.mark.parametrize(
    "expected, message",
    [
        (safety.Expected(locked={None: 100 * XCH}, other_max={None: 1}), "wallet"),
        (
            safety.Expected(wallet={None: -(100 * XCH + 1 + 1000)}, other_max={None: 1}),
            "locked collateral",
        ),
        (
            safety.Expected(wallet={None: -(100 * XCH + 1 + 1000)}, locked={None: 100 * XCH}),
            "not ours",
        ),
        (
            safety.Expected(
                wallet={None: -(100 * XCH + 1 + 1000)},
                locked={None: 100 * XCH},
                other_max={None: 1},
                max_fee=999,
            ),
            "fee",
        ),
    ],
)
def test_any_single_difference_is_refused(expected, message) -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd, fee=1000)
    effect = safety.analyse(
        minted.coin_spends,
        ours=[maker.puzzle_hash],
        locks=[bytes(minted.underlying.puzzle_hash())],
    )
    with pytest.raises(safety.SafetyError, match=message):
        safety.require(effect, expected)


def test_a_one_mojo_difference_is_refused() -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd)
    effect = safety.analyse(
        minted.coin_spends,
        ours=[maker.puzzle_hash],
        locks=[bytes(minted.underlying.puzzle_hash())],
    )
    with pytest.raises(safety.SafetyError):
        safety.require(
            effect,
            safety.Expected(
                wallet={None: -(100 * XCH + 2)}, locked={None: 100 * XCH}, other_max={None: 1}
            ),
        )


def test_a_reveal_that_is_not_the_coins_puzzle_is_refused() -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd)
    first, second = minted.coin_spends[0], minted.coin_spends[1]
    forged = sdk.CoinSpend(first.coin, bytes(second.puzzle_reveal), bytes(second.solution))
    with pytest.raises(safety.SafetyError):
        safety.analyse([forged], ours=[maker.puzzle_hash])


def test_a_coin_spent_twice_is_refused() -> None:
    world, maker, _, usd = _setup()
    minted = _call(world, maker, usd)
    spend = minted.coin_spends[0]
    with pytest.raises(safety.SafetyError, match="twice"):
        safety.analyse([spend, spend], ours=[maker.puzzle_hash])
