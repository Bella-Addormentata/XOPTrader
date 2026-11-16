"""``options.wallet.fund_from_wallet``: paying from the rest of the wallet.

The options address is one address of a wallet that has others. With the
switch on, what it is short of comes from them; everything the book owns
still lands at the options address. Seen on mainnet on 2026-11-15: the engine,
started again with contracts open, put the address's only coins into its own
offers, and the book had no coin left to start a cancel from.
"""

from __future__ import annotations

from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import chain, config, signing, store  # noqa: E402
from gui.services.options.tests.sim_world import ADDITIONAL_DATA, CAT, XCH, Party  # noqa: E402
from gui.services.options.tests.test_service import DAY, Rig, SimCoins, SimWallet  # noqa: E402


class WalletOfAddresses(SimWallet):
    """One wallet, several addresses: it has every one of their keys."""

    def __init__(self, world: Any, options: Party, others: list[Party]) -> None:
        super().__init__(world, options)
        self.parties = {party.puzzle_hash: party for party in [options, *others]}
        self.signer = signing.LocalKeySigner(
            [party.sk for party in self.parties.values()], ADDITIONAL_DATA
        )
        self.keys_asked: list[bytes] = []

    def synthetic_key(self, puzzle_hash: bytes) -> Any:
        self.keys_asked.append(bytes(puzzle_hash))
        return self.parties[bytes(puzzle_hash)].pk


class CoinsOfWallet(SimCoins):
    """The options address's coins, and the whole wallet's, less any it holds back.

    ``held_back`` stands for coins the wallet keeps for resting offers or
    pending transactions: ``get_spendable_coins`` lists them nowhere.
    """

    def __init__(self, options: Party, others: list[Party]) -> None:
        super().__init__(options)
        self.others = others
        self.held_back: set[bytes] = set()
        self.wallet_asked = 0

    def xch_coins(self) -> list[Any]:
        return [c for c in super().xch_coins() if bytes(c.coin_id()) not in self.held_back]

    def wallet_xch_coins(self) -> list[Any]:
        self.wallet_asked += 1
        coins = list(self.party.xch_coins())
        for party in self.others:
            coins.extend(party.xch_coins())
        return [c for c in coins if bytes(c.coin_id()) not in self.held_back]

    def cats(self, asset_id: bytes) -> list[Any]:
        return [c for c in super().cats(asset_id) if bytes(c.coin.coin_id()) not in self.held_back]

    def wallet_cat_coins(self, asset_id: bytes) -> list[Any]:
        self.wallet_asked += 1
        coins = []
        for party in [self.party, *self.others]:
            wrapped = sdk.cat_puzzle_hash(asset_id, party.puzzle_hash)
            coins.extend(party.world.sim.unspent_coins(wrapped, False))
        return [c for c in coins if bytes(c.coin_id()) not in self.held_back]

    def cat_of(self, coin: Any) -> Any:
        return chain.load_cat(self.party.world.sim, coin.coin_id())


def _rig(**overrides: Any) -> tuple[Rig, Party, Party, Party, Party]:
    """A book whose options address holds 1 XCH, in a wallet holding 1, 30 and 500 more.

    The quote asset (calls are struck in it) sits elsewhere in the wallet:
    5 at ``tiny`` and 900 at ``big``, none at the options address.
    """
    rig = Rig()
    poor = rig.world.party(1)
    tiny, small, big = rig.world.party(1), rig.world.party(30), rig.world.party(500)
    rig.world.send_cat(rig.taker, tiny.puzzle_hash, rig.usd, 5 * CAT)
    rig.world.send_cat(rig.taker, big.puzzle_hash, rig.usd, 900 * CAT)
    others = [tiny, small, big]
    rig.maker = poor
    rig.wallet = WalletOfAddresses(rig.world, poor, others)
    rig.coins = CoinsOfWallet(poor, others)
    wallet = {"port": 9300, "cert_path": "c", "key_path": "k", **overrides.pop("wallet", {})}
    rig.config = rig.make_config(put_strikes_pct=[], wallet=wallet, **overrides)
    rig.book = rig.make_book()
    return rig, poor, tiny, small, big


def test_it_is_off_by_default_and_only_a_real_true_turns_it_on() -> None:
    def parse(wallet: dict) -> config.OptionsConfig:
        section = {
            "enabled": True,
            "wallet": {"port": 9356, "cert_path": "c", "key_path": "k", **wallet},
            "quote_asset_id": "aa" * 32,
            "contract_xch": 1,
        }
        parsed = config.parse({"options": section})
        assert parsed is not None
        return parsed

    assert parse({}).wallet.fund_from_wallet is False
    assert parse({"fund_from_wallet": True}).wallet.fund_from_wallet is True
    with pytest.raises(config.OptionsConfigError, match="fund_from_wallet must be true or false"):
        parse({"fund_from_wallet": "yes"})


def test_without_it_a_short_options_address_mints_nothing() -> None:
    rig, poor, tiny, small, big = _rig()

    (report,) = rig.steps(1)

    assert rig.contracts() == []
    assert any("not enough XCH free at the options address" in p for p in report.problems)
    assert rig.coins.wallet_asked == 0  # the rest of the wallet is not even looked at
    assert (tiny.balance(), small.balance(), big.balance()) == (XCH, 30 * XCH, 500 * XCH)


def test_a_mint_takes_its_shortfall_from_the_smallest_coin_that_covers_it() -> None:
    rig, poor, tiny, small, big = _rig(wallet={"fund_from_wallet": True})
    total = sum(party.balance() for party in (poor, tiny, small, big))

    rig.steps(3)  # mint, see it confirm, offer it

    (call,) = rig.contracts(store.OPEN)
    assert rig.wallet.rejected == []
    assert call.underlying_amount == 20 * XCH
    # 1 XCH of its own and the 30 XCH coin (the smallest covering 19 XCH more);
    # the 1 XCH elsewhere could not cover it and the 500 XCH coin is untouched.
    assert (tiny.balance(), small.balance(), big.balance()) == (XCH, 0, 500 * XCH)
    # All the change came back to the options address: 31 - 20 XCH - the option mojo.
    assert poor.balance() == 11 * XCH - 1
    assert sum(party.balance() for party in (poor, tiny, small, big)) == total - 20 * XCH - 1
    # The funding coin's key was asked for, so it could be signed for.
    assert small.puzzle_hash in rig.wallet.keys_asked


def test_a_cancel_starts_from_a_wallet_coin_when_the_address_has_none_free() -> None:
    rig, poor, tiny, small, big = _rig(wallet={"fund_from_wallet": True})
    rig.steps(3)
    (call,) = rig.contracts(store.OPEN)
    # The engine, started again, puts every coin at the options address into
    # its own resting offers: the wallet lists none of them as spendable.
    rig.coins.held_back = {bytes(coin.coin_id()) for coin in poor.xch_coins()}
    held = poor.balance()

    rig.world.advance(14 * DAY - 1800)  # inside the stop window: unsold, so retired
    reports = rig.steps(4)

    assert [item.status for item in rig.contracts()] == [store.CLOSED]
    assert any("retiring" in action for report in reports for action in report.actions)
    assert rig.wallet.rejected == []
    # A call struck in the quote asset needs a seed coin of it, and the options
    # address has none: the smallest elsewhere (5 at tiny), not big's 900.
    assert (tiny.balance(rig.usd), big.balance(rig.usd)) == (0, 900 * CAT)
    assert poor.balance(rig.usd) == 5 * CAT  # its change came back to the options address
    # No fee here, so no XCH seed: the XCH elsewhere is untouched, and the
    # collateral and the option mojo came back to the options address.
    assert (tiny.balance(), big.balance()) == (XCH, 500 * XCH)
    assert poor.balance() == held + 20 * XCH + 1
    assert rig.events("collateral_returned")[-1].detail["by"] == store.CANCEL


def test_without_it_the_same_cancel_cannot_start() -> None:
    rig, poor, tiny, small, big = _rig(wallet={"fund_from_wallet": True})
    rig.steps(3)
    # The switch is turned off with the contract open and the address dry.
    rig.config = rig.make_config(put_strikes_pct=[])
    rig.book = rig.make_book()
    rig.coins.held_back = {bytes(coin.coin_id()) for coin in poor.xch_coins()}

    rig.world.advance(14 * DAY - 1800)
    reports = rig.steps(2)

    assert [item.status for item in rig.contracts()] == [store.OPEN]
    assert any(
        "not enough of the quote asset at the options address" in problem
        for report in reports
        for problem in report.problems
    )


def test_a_wallet_that_cannot_cover_it_either_says_so() -> None:
    rig, poor, tiny, small, big = _rig(wallet={"fund_from_wallet": True}, contract_xch=90)
    rig.coins.held_back = {bytes(coin.coin_id()) for coin in big.xch_coins()}

    (report,) = rig.steps(1)

    assert rig.contracts() == []
    assert any(
        "not enough XCH free at the options address or elsewhere in the wallet" in p
        for p in report.problems
    )
