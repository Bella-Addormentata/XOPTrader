"""The options book, stepped against a simulated chain.

``Rig`` gives ``OptionsBook`` everything it needs without a wallet daemon, a
node or the internet: the SDK simulator is the chain, a key held in the test
signs (behaving like the reference wallet: its ``push`` reports nothing), a
callable returns the market, and a list stands in for the desk.

The tests follow contracts through their lives the way an operator would see
them: minted, offered, sold, exercised or expired, collateral returned.
"""

from __future__ import annotations

from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, chain, config, desk, market, signing, store  # noqa: E402
from gui.services.options.service import OptionsBook  # noqa: E402
from gui.services.options.tests.sim_world import (  # noqa: E402
    ADDITIONAL_DATA,
    CAT,
    XCH,
    Party,
    RejectedError,
    World,
)

DAY = 86400


class SimWallet:
    """Signs with the party's key; broadcasts without reporting the outcome."""

    def __init__(self, world: World, party: Party) -> None:
        self.world = world
        self.party = party
        self.signer = signing.LocalKeySigner([party.sk], ADDITIONAL_DATA)
        self.signed = 0
        self.rejected: list[str] = []
        self.fail_signing = False

    def synthetic_key(self, puzzle_hash: bytes) -> Any:
        assert bytes(puzzle_hash) == self.party.puzzle_hash
        return self.party.pk

    def sign(self, coin_spends: Any) -> bytes:
        if self.fail_signing:
            raise signing.SigningError("wallet is locked")
        self.signed += 1
        return self.signer.sign(coin_spends)

    def push(self, coin_spends: Any, aggregated_signature: bytes) -> bytes:
        bundle = sdk.SpendBundle(list(coin_spends), sdk.Signature.from_bytes(aggregated_signature))
        try:
            self.world.push_bundle(bundle)
        except RejectedError as exc:
            self.rejected.append(str(exc))
        return bytes(bundle.hash())


class SimCoins:
    def __init__(self, party: Party) -> None:
        self.party = party

    def puzzle_hash(self) -> bytes:
        return self.party.puzzle_hash

    def xch_coins(self) -> list[Any]:
        return self.party.xch_coins()

    def cats(self, asset_id: bytes) -> list[Any]:
        return self.party.cats(asset_id)


class FakeDesk:
    def __init__(self) -> None:
        self.uploads: list[str] = []
        self.down = False

    def upload_offer(self, offer: str) -> Any:
        if self.down:
            raise desk.DeskError("site unreachable")
        self.uploads.append(offer)
        return desk.Upload(
            offer_id=f"id{len(self.uploads)}", launcher_id=b"\x00" * 32, created=True
        )


class Rig:
    def __init__(self, **overrides: Any) -> None:
        self.world = World()
        self.maker = self.world.party(500)
        self.taker = self.world.party(500)
        self.usd = self.world.issue_cat(self.taker, 5000)
        self.world.send_cat(self.taker, self.maker.puzzle_hash, self.usd, 1000 * CAT)
        self.spot = 1.10
        self.sigma = 0.86
        self.market_error: str | None = None
        self.wallet = SimWallet(self.world, self.maker)
        self.desk = FakeDesk()
        self.store = store.OptionStore(":memory:")
        self.expiry = self.world.now() + 14 * DAY
        self.config = self.make_config(**overrides)
        self.book = self.make_book()

    def make_config(self, **overrides: Any) -> config.OptionsConfig:
        section: dict[str, Any] = {
            "enabled": True,
            "dry_run": False,
            "network": "testnet11",
            "wallet": {"port": 9300, "cert_path": "c", "key_path": "k"},
            "quote_asset_id": self.usd.hex(),
            "contract_xch": 20,
            "expiries": [self.expiry],
            "call_strikes_pct": [110],
            "put_strikes_pct": [90],
            "strike_step": 0.05,
            "stop_before_expiry_seconds": 3600,
            "limits": {
                "max_locked_xch": 100,
                "max_locked_quote": 200,
                "max_contracts": 6,
                "max_contracts_per_expiry": 6,
            },
        }
        for key, value in overrides.items():
            if isinstance(value, dict) and isinstance(section.get(key), dict):
                section[key] = {**section[key], **value}
            else:
                section[key] = value
        parsed = config.parse({"options": section})
        assert parsed is not None
        return parsed

    def make_book(self) -> OptionsBook:
        return OptionsBook(
            self.config,
            book_store=self.store,
            wallet=self.wallet,
            coins=SimCoins(self.maker),
            chain_reader=self.world.sim,
            market=self.read_market,
            desk=self.desk,
            clock=self.world.now,
        )

    def read_market(self) -> market.Market:
        if self.market_error is not None:
            raise market.MarketError(self.market_error)
        return market.Market(
            spot=self.spot,
            sigma=self.sigma,
            bid=self.spot * 0.98,
            ask=self.spot * 1.02,
            as_of=self.world.now(),
        )

    def steps(self, count: int) -> list[Any]:
        return [self.book.step() for _ in range(count)]

    def contracts(self, *statuses: str) -> list[store.Contract]:
        return self.store.contracts(statuses or None)

    def live_offer(self, contract: store.Contract) -> store.OfferRow:
        return self.store.offers(contract.launcher_id, "live")[-1]

    def events(self, kind: str) -> list[store.Event]:
        return [event for event in self.store.events() if event.kind == kind]


def _by_side(rig: Rig, side: str, *statuses: str) -> store.Contract:
    (contract,) = [item for item in rig.contracts(*statuses) if item.side == side]
    return contract


# ---------------------------------------------------------------------------
# Nothing happens unless it is allowed to
# ---------------------------------------------------------------------------


def test_dry_run_reports_the_next_mint_and_touches_nothing() -> None:
    rig = Rig(dry_run=True)
    xch, usd = rig.maker.balance(), rig.maker.balance(rig.usd)

    (report,) = rig.steps(1)

    assert report.dry_run
    assert any(action.startswith("dry run: would mint") for action in report.actions)
    assert rig.contracts() == []
    assert rig.wallet.signed == 0
    assert rig.desk.uploads == []
    assert (rig.maker.balance(), rig.maker.balance(rig.usd)) == (xch, usd)


def test_unset_limits_mint_nothing() -> None:
    rig = Rig(limits={"max_locked_xch": None})

    (report,) = rig.steps(1)

    assert "not set" in report.blocked
    assert rig.contracts() == []
    assert rig.wallet.signed == 0


def test_unusable_market_stops_quoting_but_not_housekeeping() -> None:
    rig = Rig()
    rig.steps(4)
    assert len(rig.contracts(store.OPEN)) == 2
    offers_before = len(rig.store.offers())
    rig.market_error = "the spot price is 4000 s old"
    rig.world.advance(15 * DAY)  # everything expires while the market is dark

    reports = rig.steps(3)

    assert all("4000 s old" in report.market_error for report in reports)
    assert len(rig.store.offers()) == offers_before  # no new quotes
    # ...but the expired collateral still came home.
    assert [item.status for item in rig.contracts()] == [store.CLOSED, store.CLOSED]
    # All of it but the two one-mojo option coins, which a clawback leaves behind.
    assert rig.maker.balance() == 500 * XCH - 2 * builder.OPTION_COIN_AMOUNT
    assert rig.maker.balance(rig.usd) == 1000 * CAT


def test_spot_outside_the_executable_range_stops_quoting() -> None:
    rig = Rig()
    rig.read_market = lambda: market.Market(  # type: ignore[method-assign]
        spot=1.10, sigma=0.86, bid=1.20, ask=1.25, as_of=rig.world.now()
    )
    rig.book = rig.make_book()

    (report,) = rig.steps(1)

    assert "outside the executable range" in report.market_error
    assert rig.contracts() == []


# ---------------------------------------------------------------------------
# The grid
# ---------------------------------------------------------------------------


def test_grid_is_minted_one_contract_a_step_then_offered() -> None:
    rig = Rig()

    first, second, third = rig.steps(3)

    assert sum("minting" in action for action in first.actions) == 1
    call = _by_side(rig, "call", store.OPEN)
    put = _by_side(rig, "put", store.OPEN)
    # 110% and 90% of 1.10 on a 0.05 grid, for 20 XCH.
    assert (call.underlying_asset, call.underlying_amount) == (None, 20 * XCH)
    assert (call.strike_asset, call.strike_amount) == (rig.usd, 24 * CAT)  # 1.20 * 20
    assert (put.underlying_asset, put.underlying_amount) == (rig.usd, 20 * CAT)  # 1.00 * 20
    assert (put.strike_asset, put.strike_amount) == (None, 20 * XCH)
    assert rig.maker.balance() == 500 * XCH - 20 * XCH - 2 * builder.OPTION_COIN_AMOUNT
    assert rig.maker.balance(rig.usd) == 1000 * CAT - 20 * CAT
    assert len(rig.events("collateral_locked")) == 2
    assert rig.wallet.rejected == []

    rig.steps(1)
    for contract in (call, put):
        offer = rig.live_offer(contract)
        assert offer.price_asset == rig.usd and offer.price_amount > 0
        assert offer.expires_at == pytest.approx(rig.world.now() + 900, abs=120)
        assert offer.desk_offer_id is not None
    assert len(rig.desk.uploads) == 2
    # What was uploaded is a real, signed offer for the right option.
    view = desk.inspect_offer(rig.desk.uploads[0], agg_sig_me_additional_data=ADDITIONAL_DATA)
    assert view.signature_valid
    assert view.launcher_id in {call.launcher_id, put.launcher_id}
    assert view.requested_asset_id == rig.usd
    assert third is not None


def test_limits_cap_the_book() -> None:
    rig = Rig(
        call_strikes_pct=[100, 110, 120],
        put_strikes_pct=[],
        limits={"max_locked_xch": 45},
    )

    rig.steps(8)

    # 45 XCH allows two 20 XCH calls, not three.
    assert len(rig.contracts()) == 2
    assert rig.maker.balance() == 500 * XCH - 40 * XCH - 2 * builder.OPTION_COIN_AMOUNT


def test_a_step_with_nothing_to_do_does_nothing() -> None:
    rig = Rig()
    rig.steps(5)
    signed, offers = rig.wallet.signed, len(rig.store.offers())

    (report,) = rig.steps(1)

    assert report.actions == []
    assert rig.wallet.signed == signed
    assert len(rig.store.offers()) == offers


def test_offers_are_refreshed_before_they_expire() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    first = rig.live_offer(call)

    rig.world.advance(800)  # inside the last three minutes of a 15 minute offer
    rig.steps(1)

    second = rig.live_offer(call)
    assert second.id != first.id
    assert second.expires_at > first.expires_at
    # The older offer is not withdrawn; it simply runs out.
    rig.world.advance(200)
    rig.steps(1)
    assert [offer.status for offer in rig.store.offers(call.launcher_id)][0] == "dead"


def test_a_small_move_reprices_with_a_new_offer() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    before = rig.live_offer(call)

    rig.spot = 1.115
    rig.steps(1)

    after = rig.live_offer(call)
    assert after.id != before.id
    assert after.price_amount > before.price_amount
    assert len(rig.store.offers(call.launcher_id, "live")) == 2


def test_a_large_rise_withdraws_the_cheap_offer_before_requoting() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    cheap = rig.live_offer(call)

    rig.spot = 1.25  # the call is now in the money; the old ask is far too low
    (report,) = rig.steps(1)
    assert any("withdrawing offers" in action for action in report.actions)
    with pytest.raises(RejectedError, match="DoubleSpend"):
        rig.world.take(rig.taker, cheap.offer_text)

    rig.steps(2)
    fresh = rig.live_offer(call)
    assert fresh.price_amount > cheap.price_amount * 1.5
    assert [offer.status for offer in rig.store.offers(call.launcher_id)][0] == "dead"
    assert rig.contracts(store.OPEN) and call.launcher_id in {
        item.launcher_id for item in rig.contracts(store.OPEN)
    }


# ---------------------------------------------------------------------------
# Sales, exercise, expiry
# ---------------------------------------------------------------------------


def test_a_sale_is_recorded_and_the_slot_is_refilled() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    offer = rig.live_offer(call)
    usd_before = rig.maker.balance(rig.usd)

    rig.world.take(rig.taker, offer.offer_text)
    rig.steps(2)

    assert rig.store.contract(call.launcher_id).status == store.SOLD
    assert rig.maker.balance(rig.usd) == usd_before + offer.price_amount
    (sold,) = rig.events("sold")
    assert sold.detail == {"asset": rig.usd.hex(), "amount": offer.price_amount}
    assert [row.status for row in rig.store.offers(call.launcher_id)].count("taken") == 1
    # The slot is empty again, so a new call is minted in its place.
    replacement = _by_side(rig, "call", store.MINTING, store.OPEN)
    assert replacement.launcher_id != call.launcher_id
    assert replacement.slot == call.slot


def test_exercise_is_noticed_and_booked() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.take(rig.taker, rig.live_offer(call).offer_text)
    rig.steps(1)
    loaded = rig.world.load(call.launcher_id)
    usd_before = rig.maker.balance(rig.usd)

    rig.world.push(
        builder.build_exercise(
            loaded.option,
            loaded.underlying,
            loaded.locked,
            rig.taker.funding(rig.usd),
            rig.taker.custody(),
        )
    )
    rig.steps(1)

    assert rig.store.contract(call.launcher_id).status == store.EXERCISED
    assert rig.maker.balance(rig.usd) == usd_before + 24 * CAT
    (exercised,) = rig.events("exercised")
    assert exercised.detail == {"strike_asset": rig.usd.hex(), "strike_amount": 24 * CAT}


def test_sold_contract_is_clawed_back_after_expiry() -> None:
    rig = Rig(put_strikes_pct=[], limits={"max_contracts": 1})
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.take(rig.taker, rig.live_offer(call).offer_text)
    rig.steps(1)
    xch_before = rig.maker.balance()

    rig.world.advance(14 * DAY)
    rig.steps(2)

    assert rig.store.contract(call.launcher_id).status == store.CLOSED
    assert rig.maker.balance() == xch_before + 20 * XCH
    (returned,) = rig.events("collateral_returned")
    assert returned.detail == {"asset": None, "amount": 20 * XCH, "by": store.CLAWBACK}


def test_unsold_contracts_are_retired_before_expiry_and_offers_stop() -> None:
    rig = Rig()
    rig.steps(4)
    assert len(rig.contracts(store.OPEN)) == 2

    rig.world.advance(14 * DAY - 1800)  # inside the stop window, not yet expired
    rig.steps(4)

    assert [item.status for item in rig.contracts()] == [store.CLOSED, store.CLOSED]
    assert rig.maker.balance() == 500 * XCH
    assert rig.maker.balance(rig.usd) == 1000 * CAT
    assert {event.detail["by"] for event in rig.events("collateral_returned")} == {store.CANCEL}
    assert rig.store.offers(status="live") == []


def test_a_contract_that_drifts_out_of_its_slot_is_retired_and_replaced() -> None:
    rig = Rig(put_strikes_pct=[], strike_drift_pct=10)
    rig.steps(3)
    old = _by_side(rig, "call", store.OPEN)

    rig.spot = 1.60  # the 1.20 strike is now far from 110% of spot
    rig.steps(4)

    assert rig.store.contract(old.launcher_id).status == store.CLOSED
    new = _by_side(rig, "call", store.MINTING, store.OPEN)
    assert new.strike_amount == 35 * CAT  # 1.75 * 20


# ---------------------------------------------------------------------------
# Things going wrong
# ---------------------------------------------------------------------------


def test_a_mint_that_loses_its_coins_fails_and_locks_nothing() -> None:
    rig = Rig(put_strikes_pct=[])
    # The wallet "pushes" but the node never sees it...
    rig.wallet.push = lambda coin_spends, signature: b"\x00" * 32  # type: ignore[method-assign]
    rig.steps(1)
    (minting,) = rig.contracts(store.MINTING)
    # ...and the funding coin is spent by something else.
    (coin,) = [c for c in rig.maker.xch_coins()]
    clvm = sdk.Clvm()
    clvm.spend_standard_coin(
        coin, rig.maker.pk, clvm.delegated_spend([clvm.create_coin(rig.maker.puzzle_hash, 1, None)])
    )
    rig.world.push(clvm.coin_spends())

    (report,) = rig.steps(1)

    assert rig.store.contract(minting.launcher_id).status == store.FAILED
    assert any("lost its coins" in problem for problem in report.problems)
    assert rig.events("mint_failed")


def test_a_bundle_that_never_confirms_is_flagged_not_forgotten() -> None:
    rig = Rig(put_strikes_pct=[], confirm_timeout_seconds=600)
    rig.wallet.push = lambda coin_spends, signature: b"\x00" * 32  # type: ignore[method-assign]
    rig.steps(1)

    rig.world.advance(700)
    (report,) = rig.steps(1)

    assert any("has not confirmed" in problem for problem in report.problems)
    assert rig.events("broadcast_stalled")
    # Still counted, so the slot is not minted twice.
    assert len(rig.contracts(store.MINTING)) == 1
    assert len(rig.contracts()) == 1


def test_a_pending_bundle_is_pushed_again() -> None:
    rig = Rig(put_strikes_pct=[])
    real_push = rig.wallet.push
    rig.wallet.push = lambda coin_spends, signature: b"\x00" * 32  # type: ignore[method-assign]
    rig.steps(1)
    rig.wallet.push = real_push  # type: ignore[method-assign]

    rig.world.advance(130)
    rig.steps(2)

    assert len(rig.contracts(store.OPEN)) == 1


def test_wallet_that_cannot_sign_changes_nothing() -> None:
    rig = Rig()
    rig.wallet.fail_signing = True

    (report,) = rig.steps(1)

    assert any("not sent" in problem for problem in report.problems)
    assert rig.contracts() == []
    assert rig.maker.balance() == 500 * XCH


def test_desk_outage_leaves_a_valid_unlisted_offer() -> None:
    rig = Rig()
    rig.desk.down = True

    reports = rig.steps(4)

    call = _by_side(rig, "call", store.OPEN)
    offer = rig.live_offer(call)
    assert offer.desk_offer_id is None
    assert any("not listed" in problem for report in reports for problem in report.problems)
    rig.world.take(rig.taker, offer.offer_text)  # still takeable by anyone who has it


def test_not_enough_funds_skips_that_contract_only() -> None:
    rig = Rig()
    # Send away all but a little of the quote asset: the put cannot be funded.
    rig.world.send_cat(rig.maker, rig.taker.puzzle_hash, rig.usd, 995 * CAT)

    reports = rig.steps(3)

    assert [item.side for item in rig.contracts()] == ["call"]
    assert any("cannot mint" in problem for report in reports for problem in report.problems)


def test_restart_picks_up_where_it_left_off() -> None:
    rig = Rig()
    rig.steps(2)
    before = {item.launcher_id for item in rig.contracts()}

    rig.book = rig.make_book()  # a new process over the same database and chain
    rig.steps(3)

    assert {item.launcher_id for item in rig.contracts()} == before
    assert len(rig.contracts(store.OPEN)) == 2
    assert len(rig.store.offers(status="live")) == 2


# ---------------------------------------------------------------------------
# Kill switch
# ---------------------------------------------------------------------------


def test_withdraw_all_kills_every_open_offer() -> None:
    rig = Rig()
    rig.steps(4)
    offers = [rig.live_offer(item) for item in rig.contracts(store.OPEN)]

    report = rig.book.withdraw_all()

    assert sum("withdrawing offers" in action for action in report.actions) == 2
    for offer in offers:
        with pytest.raises(RejectedError, match="DoubleSpend"):
            rig.world.take(rig.taker, offer.offer_text)
    # The contracts are still ours, and nothing was lost.
    for contract in rig.contracts():
        assert rig.world.load(contract.launcher_id).owner_puzzle_hash == rig.maker.puzzle_hash
    assert chain.OptionStatus.LIVE is rig.world.load(rig.contracts()[0].launcher_id).status
