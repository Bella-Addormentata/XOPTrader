"""Option spends, end to end on a simulated chain.

Each test builds spends with :mod:`gui.services.options.builder`, signs them and
submits them to the SDK simulator, then checks balances and the option's state
as :func:`chain.load_option` reads it back. The simulator runs every puzzle and
verifies the aggregate signature, so when a test expects a refusal it is the
chain's own rule doing the refusing, not this code. It does not model mempool
fee or cost policy.

Amounts follow the live desk: a covered call locks XCH and strikes in a
stablecoin CAT, a cash-secured put locks the CAT and strikes in XCH.
"""

from __future__ import annotations

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder  # noqa: E402
from gui.services.options.chain import OptionStatus  # noqa: E402
from gui.services.options.tests.sim_world import CAT, XCH, RejectedError, World  # noqa: E402

HOUR = 3600


@pytest.fixture()
def world() -> World:
    return World()


def _call_terms(world: World, maker, usd: bytes, *, expires_in: int = HOUR):
    """Covered call: 100 XCH locked, exercisable for 120 USD."""
    return builder.OptionTerms(
        underlying=builder.Asset(None, 100 * XCH),
        strike=builder.Asset(usd, 120 * CAT),
        expiration_seconds=world.now() + expires_in,
        creator_puzzle_hash=maker.puzzle_hash,
    )


def _put_terms(world: World, maker, usd: bytes, *, expires_in: int = HOUR):
    """Cash-secured put: 115 USD locked, exercisable for 100 XCH."""
    return builder.OptionTerms(
        underlying=builder.Asset(usd, 115 * CAT),
        strike=builder.Asset(None, 100 * XCH),
        expiration_seconds=world.now() + expires_in,
        creator_puzzle_hash=maker.puzzle_hash,
    )


def _mint(world: World, maker, terms: builder.OptionTerms, *, fee: int = 0):
    asset_id = terms.underlying.asset_id
    minted = builder.build_mint(terms, maker.funding(asset_id), maker.custody(), fee=fee)
    world.push(minted.coin_spends)
    return minted


def _sell(world: World, maker, taker, minted, premium: builder.Asset) -> None:
    draft = builder.build_offer(minted.option, premium, maker.puzzle_hash, maker.custody())
    world.take(taker, draft.encode(world.signer().sign(draft.coin_spends)))


# ---------------------------------------------------------------------------
# Mint
# ---------------------------------------------------------------------------


def test_mint_call_locks_the_xch_and_creates_the_option(world: World) -> None:
    maker, other = world.party(150), world.party(1)
    usd = world.issue_cat(other, 10)
    terms = _call_terms(world, maker, usd)

    minted = _mint(world, maker, terms, fee=1000)

    # 100 XCH locked, 1 mojo in the option coin, 1000 mojos to the farmer.
    assert maker.balance() == 50 * XCH - 1000 - builder.OPTION_COIN_AMOUNT
    assert int(minted.locked.coin.amount) == 100 * XCH
    assert minted.locked.cat is None

    loaded = world.load(minted.launcher_id)
    assert loaded.status is OptionStatus.LIVE
    assert loaded.terms == terms
    assert loaded.owner_puzzle_hash == maker.puzzle_hash
    assert bytes(loaded.locked.coin.coin_id()) == bytes(minted.locked.coin.coin_id())


def test_mint_put_locks_the_cat(world: World) -> None:
    maker = world.party(5)
    usd = world.issue_cat(maker, 500)
    terms = _put_terms(world, maker, usd)

    minted = _mint(world, maker, terms)

    assert maker.balance(usd) == (500 - 115) * CAT
    assert minted.locked.cat is not None
    assert bytes(minted.locked.cat.info.asset_id) == usd

    loaded = world.load(minted.launcher_id)
    assert loaded.status is OptionStatus.LIVE
    assert loaded.terms == terms
    assert loaded.locked.cat is not None


def test_mint_needs_the_collateral(world: World) -> None:
    maker, other = world.party(50), world.party(1)
    usd = world.issue_cat(other, 10)
    with pytest.raises(builder.OptionBuildError):
        builder.build_mint(_call_terms(world, maker, usd), maker.funding(), maker.custody())


def test_mint_refuses_a_change_address_without_a_key(world: World) -> None:
    maker, other = world.party(150), world.party(1)
    usd = world.issue_cat(other, 10)
    funding = builder.Funding(change_puzzle_hash=other.puzzle_hash, xch=maker.xch_coins())
    with pytest.raises(builder.OptionBuildError, match="change address"):
        builder.build_mint(_call_terms(world, maker, usd), funding, maker.custody())


def test_two_options_from_one_wallet_are_independent(world: World) -> None:
    maker, other = world.party(150), world.party(1)
    usd = world.issue_cat(other, 10)
    terms = builder.OptionTerms(
        builder.Asset(None, 20 * XCH),
        builder.Asset(usd, 24 * CAT),
        world.now() + HOUR,
        maker.puzzle_hash,
    )
    first = _mint(world, maker, terms)
    second = _mint(world, maker, terms)

    assert first.launcher_id != second.launcher_id
    assert maker.balance() == 110 * XCH - 2 * builder.OPTION_COIN_AMOUNT
    # Identical terms, different options: one's locked coin is not the other's.
    with pytest.raises(builder.OptionBuildError, match="different option|underlying coin"):
        builder.build_exercise(
            first.option, first.underlying, second.locked, maker.funding(usd), maker.custody()
        )


# ---------------------------------------------------------------------------
# Offer
# ---------------------------------------------------------------------------


def test_offer_sells_the_option_for_a_cat_premium(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))

    draft = builder.build_offer(
        minted.option, builder.Asset(usd, 2100), maker.puzzle_hash, maker.custody()
    )
    # An offer spends the option coin and nothing else.
    assert len(draft.coin_spends) == 1
    offer = draft.encode(world.signer().sign(draft.coin_spends))
    assert offer.startswith("offer1")
    # Making the offer moved nothing.
    assert world.load(minted.launcher_id).owner_puzzle_hash == maker.puzzle_hash

    world.take(taker, offer)

    assert maker.balance(usd) == 2100
    assert taker.balance(usd) == 1000 * CAT - 2100
    loaded = world.load(minted.launcher_id)
    assert loaded.owner_puzzle_hash == taker.puzzle_hash
    assert loaded.status is OptionStatus.LIVE
    # Selling the option does not release the collateral.
    assert maker.balance() == 50 * XCH - builder.OPTION_COIN_AMOUNT


def test_offer_sells_the_option_for_an_xch_premium(world: World) -> None:
    maker, taker = world.party(5), world.party(10)
    usd = world.issue_cat(maker, 500)
    minted = _mint(world, maker, _put_terms(world, maker, usd))
    before = maker.balance()

    _sell(world, maker, taker, minted, builder.Asset(None, 6 * XCH))

    assert maker.balance() == before + 6 * XCH
    assert world.load(minted.launcher_id).owner_puzzle_hash == taker.puzzle_hash


def test_offer_expiry_is_enforced_by_the_chain(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    draft = builder.build_offer(
        minted.option,
        builder.Asset(usd, 2100),
        maker.puzzle_hash,
        maker.custody(),
        expires_at_seconds=world.now() + 900,
    )
    offer = draft.encode(world.signer().sign(draft.coin_spends))

    world.advance(901)

    with pytest.raises(RejectedError, match="AssertBeforeSecondsAbsolute"):
        world.take(taker, offer)
    assert world.load(minted.launcher_id).owner_puzzle_hash == maker.puzzle_hash
    assert taker.balance(usd) == 1000 * CAT


def test_offer_can_be_replaced_without_touching_the_chain(world: World) -> None:
    """Requoting is a new offer file for the same coin; whichever is taken wins."""
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    signer = world.signer()

    def quote(price: int) -> str:
        draft = builder.build_offer(
            minted.option, builder.Asset(usd, price), maker.puzzle_hash, maker.custody()
        )
        return draft.encode(signer.sign(draft.coin_spends))

    stale, fresh = quote(2100), quote(2500)
    world.take(taker, fresh)

    assert maker.balance(usd) == 2500
    # The older quote spends the same coin, so it died with the take.
    with pytest.raises(RejectedError, match="DoubleSpend"):
        world.take(taker, stale)


def test_transfer_to_self_cancels_an_open_offer(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    draft = builder.build_offer(
        minted.option, builder.Asset(usd, 2100), maker.puzzle_hash, maker.custody()
    )
    offer = draft.encode(world.signer().sign(draft.coin_spends))

    moved = builder.build_transfer(
        minted.option, maker.puzzle_hash, maker.funding(), maker.custody(), fee=500
    )
    world.push(moved.coin_spends)

    with pytest.raises(RejectedError, match="DoubleSpend"):
        world.take(taker, offer)
    loaded = world.load(minted.launcher_id)
    assert loaded.owner_puzzle_hash == maker.puzzle_hash
    assert bytes(loaded.option.coin.coin_id()) == bytes(moved.option.coin.coin_id())
    assert bytes(loaded.option.coin.coin_id()) != bytes(minted.option.coin.coin_id())
    # The new coin can be offered again.
    again = builder.build_offer(
        loaded.option, builder.Asset(usd, 2200), maker.puzzle_hash, maker.custody()
    )
    world.take(taker, again.encode(world.signer().sign(again.coin_spends)))
    assert maker.balance(usd) == 2200


def test_offer_needs_the_owner_key(world: World) -> None:
    maker, other = world.party(150), world.party(1)
    usd = world.issue_cat(other, 10)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    with pytest.raises(builder.OptionBuildError, match="owner"):
        builder.build_offer(
            minted.option, builder.Asset(usd, 2100), other.puzzle_hash, other.custody()
        )


# ---------------------------------------------------------------------------
# Exercise
# ---------------------------------------------------------------------------


def test_buyer_exercises_a_call(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(usd, 2100))
    loaded = world.load(minted.launcher_id)
    xch_before = taker.balance()

    world.push(
        builder.build_exercise(
            loaded.option,
            loaded.underlying,
            loaded.locked,
            taker.funding(usd),
            taker.custody(),
            fee=700,
        )
    )

    # The buyer gets the 100 XCH (and the melted option's mojo) for 120 USD.
    assert taker.balance() == xch_before + 100 * XCH + builder.OPTION_COIN_AMOUNT - 700
    assert taker.balance(usd) == 1000 * CAT - 2100 - 120 * CAT
    # The creator is paid the strike on top of the premium, at its own address.
    assert maker.balance(usd) == 2100 + 120 * CAT
    closed = world.load(minted.launcher_id)
    assert closed.status is OptionStatus.CLOSED
    assert closed.option is None and closed.locked is None


def test_buyer_exercises_a_put(world: World) -> None:
    maker, taker = world.party(5), world.party(150)
    usd = world.issue_cat(maker, 500)
    minted = _mint(world, maker, _put_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(None, 6 * XCH))
    loaded = world.load(minted.launcher_id)
    maker_xch, taker_xch = maker.balance(), taker.balance()

    world.push(
        builder.build_exercise(
            loaded.option, loaded.underlying, loaded.locked, taker.funding(), taker.custody()
        )
    )

    # The buyer sells 100 XCH at the strike: 115 USD arrives, 100 XCH leaves.
    assert taker.balance(usd) == 115 * CAT
    assert taker.balance() == taker_xch - 100 * XCH + builder.OPTION_COIN_AMOUNT
    assert maker.balance() == maker_xch + 100 * XCH
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED


def test_exercise_after_expiry_is_refused_by_the_chain(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(usd, 2100))
    loaded = world.load(minted.launcher_id)

    world.advance(HOUR + 1)

    assert world.load(minted.launcher_id).status is OptionStatus.EXPIRED
    with pytest.raises(RejectedError, match="AssertBeforeSecondsAbsolute"):
        world.push(
            builder.build_exercise(
                loaded.option,
                loaded.underlying,
                loaded.locked,
                taker.funding(usd),
                taker.custody(),
            )
        )
    assert taker.balance(usd) == 1000 * CAT - 2100


def test_exercise_needs_the_strike(world: World) -> None:
    maker, taker, rich = world.party(150), world.party(10), world.party(1)
    usd = world.issue_cat(rich, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(None, 2 * XCH))
    loaded = world.load(minted.launcher_id)
    # The buyer holds no USD at all.
    with pytest.raises(builder.OptionBuildError):
        builder.build_exercise(
            loaded.option, loaded.underlying, loaded.locked, taker.funding(), taker.custody()
        )


def test_creator_unwinds_an_unsold_option_by_exercising_it(world: World) -> None:
    """Paying the strike to yourself releases the collateral before expiry."""
    maker = world.party(150)
    usd = world.issue_cat(maker, 200)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    xch_before = maker.balance()

    world.push(
        builder.build_exercise(
            minted.option,
            minted.underlying,
            minted.locked,
            maker.funding(usd),
            maker.custody(),
            fee=1000,
        )
    )

    assert maker.balance() == xch_before + 100 * XCH + builder.OPTION_COIN_AMOUNT - 1000
    # The strike left and came straight back.
    assert maker.balance(usd) == 200 * CAT
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED


# ---------------------------------------------------------------------------
# Cancel
# ---------------------------------------------------------------------------


def test_creator_cancels_a_call_without_holding_the_strike(world: World) -> None:
    maker, rich = world.party(150), world.party(5)
    usd = world.issue_cat(rich, 1000)
    world.send_cat(rich, maker.puzzle_hash, usd, 1)  # one mojo: a seed, not a strike
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    xch_before = maker.balance()
    assert maker.balance(usd) == 1 < 120 * CAT

    world.push(
        builder.build_cancel(
            minted.option,
            minted.underlying,
            minted.locked,
            maker.funding(usd),
            maker.custody(),
            fee=1000,
        )
    )

    assert maker.balance() == xch_before + 100 * XCH + builder.OPTION_COIN_AMOUNT - 1000
    assert maker.balance(usd) == 1
    assert rich.balance(usd) == 1000 * CAT - 1
    closed = world.load(minted.launcher_id)
    assert closed.status is OptionStatus.CLOSED
    assert closed.option is None and closed.locked is None


def test_creator_cancels_a_put_without_holding_the_strike(world: World) -> None:
    maker = world.party(2)
    usd = world.issue_cat(maker, 500)
    minted = _mint(world, maker, _put_terms(world, maker, usd))
    xch_before = maker.balance()
    assert xch_before < 100 * XCH  # nowhere near the 100 XCH strike

    world.push(
        builder.build_cancel(
            minted.option,
            minted.underlying,
            minted.locked,
            maker.funding(usd),
            maker.custody(),
            fee=1000,
        )
    )

    assert maker.balance(usd) == 500 * CAT
    assert maker.balance() == xch_before + builder.OPTION_COIN_AMOUNT - 1000
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED


def test_creator_cancels_an_option_struck_in_its_own_asset(world: World) -> None:
    maker = world.party(150)
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(None, 500 * XCH),
        world.now() + HOUR,
        maker.puzzle_hash,
    )
    minted = _mint(world, maker, terms)

    world.push(
        builder.build_cancel(
            minted.option, minted.underlying, minted.locked, maker.funding(), maker.custody()
        )
    )

    assert maker.balance() == 150 * XCH
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED


def test_cancel_moves_nothing_but_the_fee(world: World) -> None:
    """Whatever the strike, a cancel is neutral: only the fee leaves the wallet."""
    maker, rich = world.party(150), world.party(5)
    usd = world.issue_cat(rich, 1000)
    world.send_cat(rich, maker.puzzle_hash, usd, 7)
    xch_start, usd_start = maker.balance(), maker.balance(usd)
    minted = _mint(world, maker, _call_terms(world, maker, usd))

    world.push(
        builder.build_cancel(
            minted.option,
            minted.underlying,
            minted.locked,
            maker.funding(usd),
            maker.custody(),
            fee=250,
        )
    )

    assert maker.balance() == xch_start - 250
    assert maker.balance(usd) == usd_start
    assert rich.balance(usd) == 1000 * CAT - 7


def test_a_buyer_cannot_cancel(world: World) -> None:
    """For anyone but the creator the strike payment is real, so cancel is refused."""
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(usd, 2100))
    loaded = world.load(minted.launcher_id)

    with pytest.raises(builder.OptionBuildError, match="only the option's creator"):
        builder.build_cancel(
            loaded.option, loaded.underlying, loaded.locked, taker.funding(usd), taker.custody()
        )


def test_the_creator_cannot_cancel_an_option_it_has_sold(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(usd, 2100))
    loaded = world.load(minted.launcher_id)

    with pytest.raises(builder.OptionBuildError, match="owner"):
        builder.build_cancel(
            loaded.option, loaded.underlying, loaded.locked, maker.funding(usd), maker.custody()
        )
    # Even a bundle signed for the coin the maker used to own is dead.
    with pytest.raises(RejectedError):
        world.push(
            builder.build_cancel(
                minted.option,
                minted.underlying,
                minted.locked,
                maker.funding(usd),
                maker.custody(),
            )
        )
    assert world.load(minted.launcher_id).status is OptionStatus.LIVE


def test_cancel_needs_a_seed_coin_of_the_strike_asset(world: World) -> None:
    maker, rich = world.party(150), world.party(5)
    usd = world.issue_cat(rich, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    assert maker.balance(usd) == 0
    with pytest.raises(builder.OptionBuildError, match="coin of the strike asset"):
        builder.build_cancel(
            minted.option, minted.underlying, minted.locked, maker.funding(usd), maker.custody()
        )


def test_cancel_after_expiry_is_refused_by_the_chain(world: World) -> None:
    """Past expiry the exercise path is closed; the collateral comes back by clawback."""
    maker, rich = world.party(150), world.party(5)
    usd = world.issue_cat(rich, 1000)
    world.send_cat(rich, maker.puzzle_hash, usd, 1)
    minted = _mint(world, maker, _call_terms(world, maker, usd))

    world.advance(HOUR + 1)

    with pytest.raises(RejectedError, match="AssertBeforeSecondsAbsolute"):
        world.push(
            builder.build_cancel(
                minted.option,
                minted.underlying,
                minted.locked,
                maker.funding(usd),
                maker.custody(),
            )
        )


# ---------------------------------------------------------------------------
# Clawback
# ---------------------------------------------------------------------------


def test_creator_claws_back_xch_after_expiry(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    _sell(world, maker, taker, minted, builder.Asset(usd, 2100))
    xch_before = maker.balance()

    world.advance(HOUR)
    loaded = world.load(minted.launcher_id)
    assert loaded.status is OptionStatus.EXPIRED

    # No other coin is needed: the fee comes out of the returned collateral.
    funding = builder.Funding(change_puzzle_hash=maker.puzzle_hash)
    world.push(
        builder.build_clawback(
            loaded.underlying, loaded.locked, maker.pk, funding, maker.custody(), fee=1000
        )
    )

    assert maker.balance() == xch_before + 100 * XCH - 1000
    assert maker.balance(usd) == 2100  # the premium is kept
    closed = world.load(minted.launcher_id)
    assert closed.status is OptionStatus.CLOSED
    # The buyer still holds the option coin; it now controls nothing.
    assert closed.owner_puzzle_hash == taker.puzzle_hash
    assert closed.locked is None


def test_creator_claws_back_a_cat_after_expiry(world: World) -> None:
    maker = world.party(5)
    usd = world.issue_cat(maker, 500)
    minted = _mint(world, maker, _put_terms(world, maker, usd))
    xch_before = maker.balance()

    world.advance(HOUR)
    loaded = world.load(minted.launcher_id)
    world.push(
        builder.build_clawback(
            loaded.underlying,
            loaded.locked,
            maker.pk,
            maker.funding(),
            maker.custody(),
            fee=1000,
        )
    )

    assert maker.balance(usd) == 500 * CAT
    assert maker.balance() == xch_before - 1000


def test_clawback_before_expiry_is_refused_by_the_chain(world: World) -> None:
    maker, other = world.party(150), world.party(1)
    usd = world.issue_cat(other, 10)
    minted = _mint(world, maker, _call_terms(world, maker, usd))

    world.advance(HOUR - 60)

    funding = builder.Funding(change_puzzle_hash=maker.puzzle_hash)
    with pytest.raises(RejectedError, match="AssertSecondsAbsolute"):
        world.push(
            builder.build_clawback(
                minted.underlying, minted.locked, maker.pk, funding, maker.custody()
            )
        )
    assert world.load(minted.launcher_id).status is OptionStatus.LIVE


def test_clawback_refuses_a_key_that_is_not_the_creators(world: World) -> None:
    maker, other = world.party(150), world.party(1)
    usd = world.issue_cat(other, 10)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    funding = builder.Funding(change_puzzle_hash=other.puzzle_hash)
    with pytest.raises(builder.OptionBuildError, match="creator"):
        builder.build_clawback(minted.underlying, minted.locked, other.pk, funding, other.custody())


def test_nobody_else_can_claw_back(world: World) -> None:
    """A thief who rebuilds the terms with their own address gets a different puzzle."""
    maker, thief = world.party(150), world.party(1)
    usd = world.issue_cat(thief, 10)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    world.advance(HOUR)
    forged = sdk.OptionUnderlying(
        minted.launcher_id,
        thief.puzzle_hash,
        int(minted.underlying.seconds),
        int(minted.underlying.amount),
        minted.underlying.strike_type,
    )
    funding = builder.Funding(change_puzzle_hash=thief.puzzle_hash)
    with pytest.raises(builder.OptionBuildError, match="not locked"):
        builder.build_clawback(forged, minted.locked, thief.pk, funding, thief.custody())


# ---------------------------------------------------------------------------
# Input validation
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("amount", [0, -1, 1.5, True, "1"])
def test_asset_rejects_bad_amounts(amount) -> None:
    with pytest.raises(builder.OptionBuildError):
        builder.Asset(None, amount)


def test_asset_rejects_a_short_asset_id() -> None:
    with pytest.raises(builder.OptionBuildError):
        builder.Asset(b"\x01" * 31, 1)


@pytest.mark.parametrize("expiry", [0, -5, 1.0, True])
def test_terms_reject_bad_expiry(expiry) -> None:
    with pytest.raises(builder.OptionBuildError):
        builder.OptionTerms(builder.Asset(None, 1), builder.Asset(None, 1), expiry, b"\x01" * 32)


@pytest.mark.parametrize("fee", [-1, 1.5, True])
def test_mint_rejects_a_bad_fee(world: World, fee) -> None:
    maker = world.party(150)
    terms = builder.OptionTerms(
        builder.Asset(None, XCH),
        builder.Asset(b"\x07" * 32, CAT),
        world.now() + HOUR,
        maker.puzzle_hash,
    )
    with pytest.raises(builder.OptionBuildError, match="fee"):
        builder.build_mint(terms, maker.funding(), maker.custody(), fee=fee)


def test_exercise_refuses_terms_that_do_not_match_the_option(world: World) -> None:
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    minted = _mint(world, maker, _call_terms(world, maker, usd))
    # Same option, but claiming a strike of 1 USD instead of 120.
    cheap = sdk.OptionUnderlying(
        minted.launcher_id,
        maker.puzzle_hash,
        int(minted.underlying.seconds),
        int(minted.underlying.amount),
        sdk.OptionType.cat(usd, 1 * CAT),
    )
    with pytest.raises(builder.OptionBuildError, match="strike or expiry"):
        builder.build_exercise(
            minted.option, cheap, minted.locked, maker.funding(), maker.custody()
        )
