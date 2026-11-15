"""The options book, stepped against a simulated chain.

``Rig`` gives ``OptionsBook`` everything it needs without a wallet daemon, a
node or the internet: the SDK simulator is the chain, a key held in the test
signs (behaving like the reference wallet: its ``push`` reports nothing), a
callable returns the market, and a list stands in for the desk.

The tests follow contracts through their lives the way an operator would see
them: minted, offered, sold, exercised or expired, collateral returned.
"""

from __future__ import annotations

import math
from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import (  # noqa: E402
    builder,
    chain,
    config,
    desk,
    fees,
    market,
    quoting,
    signing,
    splash,
    store,
)
from gui.services.options.service import OptionsBook, unpack_bundle  # noqa: E402
from gui.services.options.tests.sim_world import (  # noqa: E402
    ADDITIONAL_DATA,
    CAT,
    XCH,
    Party,
    RejectedError,
    SimChain,
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
        self.pushed = 0
        self.rejected: list[str] = []
        self.fail_signing = False
        self.rate: int | None = 0  # what its node estimates, in mojos per cost
        self.rate_asked = 0
        self.unready: str | None = None  # why it cannot be relied on, if it cannot

    def not_ready(self) -> str | None:
        return self.unready

    def fee_rate(self) -> int | None:
        self.rate_asked += 1
        return self.rate

    def synthetic_key(self, puzzle_hash: bytes) -> Any:
        assert bytes(puzzle_hash) == self.party.puzzle_hash
        return self.party.pk

    def sign(self, coin_spends: Any) -> bytes:
        if self.fail_signing:
            raise signing.SigningError("wallet is locked")
        self.signed += 1
        return self.signer.sign(coin_spends)

    def push(self, coin_spends: Any, aggregated_signature: bytes) -> bytes:
        self.pushed += 1
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

    def held(self, asset_ids: Any) -> frozenset[bytes]:
        ids = {bytes(coin.coin_id()) for coin in self.party.xch_coins()}
        for asset_id in asset_ids:
            wrapped = sdk.cat_puzzle_hash(asset_id, self.party.puzzle_hash)
            ids.update(
                bytes(c.coin_id()) for c in self.party.world.sim.unspent_coins(wrapped, False)
            )
        return frozenset(ids)


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


class FakeSplash:
    """A Splash daemon: takes offers, or does not."""

    def __init__(self) -> None:
        self.sent: list[str] = []
        self.asked = 0
        self.down: str | None = None

    def broadcast(self, offer: str) -> None:
        self.asked += 1
        if self.down is not None:
            raise splash.SplashError(self.down)
        self.sent.append(offer)


class Rig:
    def __init__(self, reader: Any = None, **overrides: Any) -> None:
        self.world = World()
        # What its books read the chain through: the simulator, or a stand-in
        # for a node built over it.
        self.reader = (SimChain if reader is None else reader)(self.world.sim)
        self.maker = self.world.party(500)
        self.taker = self.world.party(500)
        self.usd = self.world.issue_cat(self.taker, 5000)
        self.world.send_cat(self.taker, self.maker.puzzle_hash, self.usd, 1000 * CAT)
        self.spot = 1.10
        self.sigma = 0.86
        self.market_error: str | None = None
        self.wallet = SimWallet(self.world, self.maker)
        self.desk = FakeDesk()
        self.splash = FakeSplash()
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
            # These tests follow contracts, not the chain-reading policy: look
            # at the chain on every step. test_chain_use.py holds the policy.
            "chain_poll_seconds": 0,
            "chain_refresh_seconds": 0,
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
            chain_reader=self.reader,
            market=self.read_market,
            desk=self.desk,
            splash=self.splash if self.config.splash.enabled else None,
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


def test_a_large_rise_withdraws_the_cheap_offer_then_sits_out_before_requoting() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    cheap = rig.live_offer(call)

    rig.spot = 1.25  # the call is now in the money; the old ask is far too low
    (report,) = rig.steps(1)
    assert any("withdrawing offers" in action for action in report.actions)
    with pytest.raises(RejectedError, match="DoubleSpend"):
        rig.world.take(rig.taker, cheap.offer_text)
    # The withdrawal must not outlive the offer it kills.
    (sent,) = [row for row in rig.store.broadcasts(None) if row.kind == store.WITHDRAW]
    assert sent.valid_until == cheap.expires_at

    # Confirmed, and then nothing is on offer for a while: the price is running.
    rig.spot = 1.40
    rig.steps(2)
    assert rig.store.offers(call.launcher_id, "live") == []
    assert len([row for row in rig.store.broadcasts(None) if row.kind == store.WITHDRAW]) == 1

    rig.world.advance(300)
    rig.steps(1)
    fresh = rig.live_offer(call)
    assert fresh.price_amount > cheap.price_amount * 1.5
    assert [offer.status for offer in rig.store.offers(call.launcher_id)][0] == "dead"
    assert call.launcher_id in {item.launcher_id for item in rig.contracts(store.OPEN)}


def _ask_gap_pct(rig: Rig, contract: store.Contract, offer: store.OfferRow) -> float:
    """How far the current ask is above an offer, as a percentage of notional."""
    view = rig.read_market()
    ask = quoting.ask_mojos(contract, view, rig.config, rig.world.now(), 0.0)
    notional = quoting.contract_size_xch(contract) * view.spot * rig.config.quote_unit_mojos
    return (ask - offer.price_amount) / notional * 100.0


@pytest.mark.parametrize("withdraw_pct, withdrawn", [(None, False), (1.0, True)])
def test_a_moderate_rise_is_repriced_not_withdrawn(withdraw_pct, withdrawn) -> None:
    rig = (
        Rig(put_strikes_pct=[])
        if withdraw_pct is None
        else Rig(put_strikes_pct=[], withdraw_pct=withdraw_pct)
    )
    assert rig.config.withdraw_pct == (2.0 if withdraw_pct is None else withdraw_pct)
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    cheap = rig.live_offer(call)

    rig.spot = 1.14
    assert 1.0 <= _ask_gap_pct(rig, call, cheap) < 2.0  # between the old trigger and the new
    (report,) = rig.steps(1)

    assert any("withdrawing offers" in action for action in report.actions) is withdrawn
    if not withdrawn:
        # A dearer offer is posted beside the old one, which runs out by itself.
        assert rig.live_offer(call).price_amount > cheap.price_amount
        assert len(rig.store.offers(call.launcher_id, "live")) == 2


def test_a_stale_offer_about_to_run_out_is_not_worth_a_withdrawal() -> None:
    rig = Rig(put_strikes_pct=[])
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    cheap = rig.live_offer(call)

    rig.world.advance(800)  # the offer has about a hundred seconds left
    rig.spot = 1.25
    assert _ask_gap_pct(rig, call, cheap) >= 2.0
    (report,) = rig.steps(1)

    # A withdrawal takes a block or two; the offer would be dead before it landed.
    assert not any("withdrawing" in action for action in report.actions)
    assert [row.kind for row in rig.store.broadcasts(None)] == [store.MINT]
    assert rig.live_offer(call).price_amount > cheap.price_amount  # re-quoted instead


def test_withdrawals_are_capped_per_hour_and_the_rest_stand_down() -> None:
    rig = Rig(
        call_strikes_pct=[100, 110],
        put_strikes_pct=[],
        max_withdrawals_per_hour=1,
        strike_drift_pct=90,  # keep retirement out of it: this is about withdrawals
    )
    rig.steps(5)
    first, second = rig.contracts(store.OPEN)
    stale = {item.launcher_id: rig.live_offer(item) for item in (first, second)}

    rig.spot = 1.30
    (report,) = rig.steps(1)

    assert sum("withdrawing offers" in action for action in report.actions) == 1
    assert sum("withdrawals are used up" in problem for problem in report.problems) == 1
    (sent,) = [row for row in rig.store.broadcasts(None) if row.kind == store.WITHDRAW]
    held_back = second if sent.launcher_id == first.launcher_id else first
    # The one that could not be withdrawn is not re-quoted: its stale offer is
    # left to run out, with no dearer one posted beside it.
    assert [offer.id for offer in rig.store.offers(held_back.launcher_id, "live")] == [
        stale[held_back.launcher_id].id
    ]
    rig.steps(2)
    assert len(rig.store.offers(held_back.launcher_id)) == 1

    rig.world.advance(900)  # it has run out
    rig.steps(1)
    requoted = rig.live_offer(held_back)
    assert requoted.price_amount > stale[held_back.launcher_id].price_amount
    assert len([row for row in rig.store.broadcasts(None) if row.kind == store.WITHDRAW]) == 1

    # An hour later the allowance is back.
    rig.world.advance(3600)
    rig.steps(1)
    rig.spot = 1.60
    (later,) = rig.steps(1)
    assert sum("withdrawing offers" in action for action in later.actions) == 1
    assert sum("withdrawals are used up" in problem for problem in later.problems) == 1


def test_with_no_withdrawals_allowed_stale_offers_are_left_to_run_out() -> None:
    rig = Rig(put_strikes_pct=[], max_withdrawals_per_hour=0)
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    cheap = rig.live_offer(call)

    rig.spot = 1.25
    reports = rig.steps(3)

    assert [row.kind for row in rig.store.broadcasts(None)] == [store.MINT]
    assert all(
        any("withdrawals are used up" in problem for problem in report.problems)
        for report in reports
    )
    assert rig.store.offers(call.launcher_id, "live") == [cheap]
    rig.world.take(
        rig.taker, cheap.offer_text
    )  # the exposure that was accepted, bounded by its life


def test_a_withdrawal_that_does_not_confirm_lapses_and_the_contract_is_offered_again() -> None:
    rig = Rig(put_strikes_pct=[])
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    cheap = rig.live_offer(call)
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.spot = 1.25
    rig.steps(1)  # the withdrawal is signed; no node hears it
    (sent,) = [row for row in rig.store.unresolved() if row.kind == store.WITHDRAW]
    assert sent.valid_until == cheap.expires_at

    # While it is unresolved the contract is not offered.
    rig.world.advance(600)
    rig.steps(1)
    assert rig.store.offers(call.launcher_id, "live") == [cheap]

    # The offer it was for has run out and the chain's clock, 32 blocks down,
    # is past it: the withdrawal can never confirm, so the contract is offered again.
    rig.world.advance(1000)
    (report,) = rig.steps(1)

    assert report.chain_looked == "a transaction ran out of time"
    assert any("back on offer" in action for action in report.actions)
    assert rig.store.unresolved() == []
    assert [event.detail for event in rig.events("broadcast_lapsed")] == [{"kind": "withdraw"}]
    assert rig.live_offer(call).price_amount > cheap.price_amount
    # And it truly cannot: a node that hears it now refuses it.
    spends, signature = unpack_bundle(sent.bundle)
    with pytest.raises(RejectedError, match="AssertBeforeSecondsAbsolute"):
        rig.world.push(spends, signature)


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
# Bundles that do not confirm
# ---------------------------------------------------------------------------


def _unheard(calls: list[int] | None = None) -> Any:
    """A push the node never sees, as when the wallet has lost its peer."""

    def push(coin_spends: Any, signature: bytes) -> bytes:
        if calls is not None:
            calls.append(1)
        return b"\x00" * 32

    return push


def test_a_mint_that_does_not_confirm_in_time_lapses_and_is_minted_again() -> None:
    rig = Rig(call_strikes_pct=[100, 110], put_strikes_pct=[], confirm_timeout_seconds=600)
    real_push = rig.wallet.push
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)  # one mint is signed over the address's only coin
    (stuck,) = rig.contracts(store.MINTING)
    (sent,) = rig.store.unresolved()
    assert sent.valid_until == sent.created_at + 600
    rig.world.advance(700)

    reports = rig.steps(3)

    # Late by our clock, but the chain's, read 32 blocks down, has not passed
    # its deadline: it is flagged, and its coin is not handed to a second mint.
    assert len(rig.events("broadcast_stalled")) == 1
    assert all(
        any("has not confirmed" in problem for problem in report.problems) for report in reports
    )
    assert rig.wallet.signed == 1
    assert [item.status for item in rig.contracts()] == [store.MINTING]

    # The outage ends and the process is restarted, too late for this mint.
    rig.wallet.push = real_push  # type: ignore[method-assign]
    rig.book = rig.make_book()
    rig.world.advance(700)  # the block that put it out of time is now buried deep
    (report,) = rig.steps(1)

    assert report.chain_looked == "a transaction ran out of time"
    assert any("did not confirm in time" in problem for problem in report.problems)
    assert [event.detail for event in rig.events("mint_failed")] == [{"reason": "lapsed"}]
    # Nothing was locked, the coin is free, and the slot is minted again at
    # once. The new mint spends the same coin, so its launcher has the same id
    # and takes over the failed contract's record.
    assert rig.wallet.signed == 2
    assert rig.store.contract(stuck.launcher_id).status == store.MINTING
    assert [(row.kind, row.status) for row in rig.store.broadcasts(None)] == [
        (store.MINT, "conflict"),
        (store.MINT, "pending"),
    ]
    assert rig.maker.balance() == 500 * XCH - 20 * XCH - builder.OPTION_COIN_AMOUNT
    rig.steps(4)
    assert len(rig.contracts(store.OPEN)) == 2
    assert len(rig.contracts()) == 2
    # The dead mint really is dead: the chain refuses it.
    spends, signature = unpack_bundle(sent.bundle)
    with pytest.raises(RejectedError, match="AssertBeforeSecondsAbsolute"):
        rig.world.push(spends, signature)


def test_a_mint_is_not_given_up_while_the_chain_s_clock_is_behind_ours() -> None:
    rig = Rig(put_strikes_pct=[], confirm_timeout_seconds=600)
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)
    rig.world.advance(1400)  # long past the deadline by our clock...
    rig.reader.chain_time = lambda depth=0: rig.world.now() - 900  # type: ignore[method-assign]

    (report,) = rig.steps(1)

    # ...but the chain has not got there, so the mint could still confirm.
    assert [item.status for item in rig.contracts()] == [store.MINTING]
    assert rig.wallet.signed == 1
    assert not any("did not confirm in time" in problem for problem in report.problems)


def _sold_and_expired(rig: Rig) -> store.Contract:
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.take(rig.taker, rig.live_offer(call).offer_text)
    rig.steps(1)
    rig.world.advance(14 * DAY)
    return call


def test_a_clawback_that_does_not_confirm_in_time_is_signed_again() -> None:
    rig = Rig(put_strikes_pct=[], limits={"max_contracts": 1}, confirm_timeout_seconds=600)
    call = _sold_and_expired(rig)
    real_push = rig.wallet.push
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)  # the clawback is signed; no node hears it
    (sent,) = rig.store.unresolved()
    assert (sent.kind, sent.valid_until) == (store.CLAWBACK, sent.created_at + 600)

    rig.wallet.push = real_push  # type: ignore[method-assign]
    rig.world.advance(1300)  # too late for that one
    (report,) = rig.steps(1)

    # Given up, and a new one signed in the same step: the collateral is
    # always wanted back.
    assert any("clawback" in p and "did not confirm in time" in p for p in report.problems)
    assert [(row.kind, row.status) for row in rig.store.broadcasts(None)][-2:] == [
        (store.CLAWBACK, "conflict"),
        (store.CLAWBACK, "pending"),
    ]
    rig.steps(1)
    assert rig.store.contract(call.launcher_id).status == store.CLOSED
    assert rig.events("collateral_returned")[0].detail["by"] == store.CLAWBACK


def test_with_no_chain_clock_nothing_is_given_a_deadline() -> None:
    # A bundle that could run out but never be seen to would hold its coins for
    # ever, so a reader that cannot tell the chain's time gets none.
    rig = Rig(
        reader=lambda sim: sim,
        put_strikes_pct=[],
        limits={"max_contracts": 1},
        confirm_timeout_seconds=600,
    )
    call = _sold_and_expired(rig)
    real_push = rig.wallet.push
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)
    assert [row.valid_until for row in rig.store.broadcasts(None)] == [None, None]

    rig.world.advance(6 * 3600)  # hours of outage
    reports = rig.steps(2)

    # Flagged, never given up, and still good when a node finally hears it.
    assert rig.events("broadcast_stalled")
    assert all("did not confirm in time" not in p for r in reports for p in r.problems)
    assert [row.status for row in rig.store.unresolved()] == ["stalled"]
    rig.wallet.push = real_push  # type: ignore[method-assign]
    rig.book = rig.make_book()
    rig.steps(2)
    assert rig.store.contract(call.launcher_id).status == store.CLOSED


def test_a_crash_while_a_verdict_is_written_leaves_it_to_be_read_again(monkeypatch) -> None:
    rig = Rig(put_strikes_pct=[])
    rig.steps(1)  # minted on chain; the store does not know yet

    def power_cut(*args: Any, **kwargs: Any) -> None:
        raise RuntimeError("power cut")

    with monkeypatch.context() as patch:
        patch.setattr(store.OptionStore, "_transition", staticmethod(power_cut))
        with pytest.raises(RuntimeError, match="power cut"):
            rig.book.step()

    # Nothing was half applied: not a confirmed broadcast beside a contract
    # that would then sit in "minting" for ever.
    assert [row.status for row in rig.store.unresolved()] == ["pending"]
    assert [item.status for item in rig.contracts()] == [store.MINTING]
    assert rig.events("collateral_locked") == []

    rig.book = rig.make_book()  # the process comes back
    rig.steps(1)

    assert [item.status for item in rig.contracts()] == [store.OPEN]
    assert len(rig.events("collateral_locked")) == 1
    assert rig.store.unresolved() == []


def _cancel_misses_expiry(rig: Rig) -> tuple[store.Contract, list[Any]]:
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.advance(14 * DAY - 1800)  # inside the stop window: it is retired
    real_push = rig.wallet.push
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)  # the cancel is signed, and the node never sees it
    assert rig.store.contract(call.launcher_id).status == store.CLOSING
    rig.wallet.push = real_push  # type: ignore[method-assign]
    rig.world.advance(3600)  # expiry passes: the cancel can never confirm now
    return call, rig.steps(3)


def _collateral_came_home_by_clawback(rig: Rig, call: store.Contract) -> None:
    # The collateral is not left locked behind a cancel the chain will refuse.
    assert rig.store.contract(call.launcher_id).status == store.CLOSED
    (returned,) = rig.events("collateral_returned")
    assert returned.detail == {"asset": None, "amount": 20 * XCH, "by": store.CLAWBACK}
    assert rig.maker.balance() == 500 * XCH - builder.OPTION_COIN_AMOUNT
    assert {row.kind: row.status for row in rig.store.broadcasts(None)} == {
        store.MINT: "confirmed",
        store.CANCEL: "conflict",
        store.CLAWBACK: "confirmed",
    }
    assert rig.store.unresolved() == []


def test_a_cancel_that_misses_expiry_lapses_and_the_collateral_is_clawed_back() -> None:
    rig = Rig(put_strikes_pct=[])

    call, reports = _cancel_misses_expiry(rig)

    _collateral_came_home_by_clawback(rig, call)
    # The chain's clock showed the cancel dead, so it was given up first and
    # the clawback then went out for a contract back on the book.
    assert [event.detail for event in rig.events("broadcast_lapsed")] == [{"kind": "cancel"}]
    problems = [problem for report in reports for problem in report.problems]
    assert any("cancel" in p and "did not confirm in time" in p for p in problems)


def test_without_a_chain_clock_a_missed_cancel_gives_way_to_a_clawback_sent_beside_it() -> None:
    # A reader that cannot tell the chain's time: nothing can be shown to have
    # lapsed, so the clawback is sent beside the cancel and the chain picks.
    rig = Rig(reader=lambda sim: sim, put_strikes_pct=[])

    call, reports = _cancel_misses_expiry(rig)

    _collateral_came_home_by_clawback(rig, call)
    assert rig.events("broadcast_lapsed") == []
    actions = [action for report in reports for action in report.actions]
    assert any("gave way to the clawback" in action for action in actions)


def test_a_cancel_and_a_clawback_in_flight_together_settle_on_whichever_confirms() -> None:
    rig = Rig(put_strikes_pct=[])
    rig.steps(3)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.advance(14 * DAY - 1800)
    held: list[tuple[Any, bytes]] = []
    real_push = rig.wallet.push
    rig.wallet.push = lambda spends, signature: held.append((spends, signature)) or b""  # type: ignore[method-assign]
    rig.steps(1)  # the cancel is signed and sits in a mempool, in time
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    # The book's clock reaches expiry before the chain's does, so a clawback
    # goes out beside the cancel...
    rig.book = OptionsBook(
        rig.config,
        book_store=rig.store,
        wallet=rig.wallet,
        coins=SimCoins(rig.maker),
        chain_reader=rig.world.sim,
        market=rig.read_market,
        clock=lambda: rig.world.now() + 3600,
    )
    rig.steps(1)
    assert {row.kind for row in rig.store.unresolved()} == {store.CANCEL, store.CLAWBACK}
    # ...and the chain, still short of expiry, takes the cancel.
    real_push(*held[0])

    rig.steps(2)

    assert rig.store.contract(call.launcher_id).status == store.CLOSED
    (returned,) = rig.events("collateral_returned")
    assert returned.detail["by"] == store.CANCEL
    assert {row.kind: row.status for row in rig.store.broadcasts(None)} == {
        store.MINT: "confirmed",
        store.CANCEL: "confirmed",
        store.CLAWBACK: "conflict",
    }
    assert rig.events("exercised") == []
    assert rig.maker.balance() == 500 * XCH


# ---------------------------------------------------------------------------
# Dry run over a book that already exists
# ---------------------------------------------------------------------------


def test_dry_run_over_a_live_book_reads_the_chain_and_sends_nothing() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.take(rig.taker, rig.live_offer(call).offer_text)
    # The operator switches dry run back on with contracts out.
    rig.config = rig.make_config(dry_run=True)
    rig.book = rig.make_book()
    signed, pushed = rig.wallet.signed, rig.wallet.pushed
    balance = rig.maker.balance()
    rig.world.advance(15 * DAY)  # everything expires

    reports = rig.steps(3)

    # The chain is still read, so the books are right...
    assert rig.store.contract(call.launcher_id).status == store.SOLD
    assert len(rig.events("sold")) == 1
    # ...but nothing is signed and nothing is sent, clawbacks included.
    assert (rig.wallet.signed, rig.wallet.pushed) == (signed, pushed)
    assert rig.maker.balance() == balance
    assert rig.events("collateral_returned") == []
    for report in reports:
        assert sum("dry run: would claw back" in action for action in report.actions) == 2

    # With dry run off again the collateral comes home.
    rig.config = rig.make_config()
    rig.book = rig.make_book()
    rig.steps(2)
    assert [item.status for item in rig.contracts()] == [store.CLOSED, store.CLOSED]


def test_dry_run_does_not_push_a_pending_bundle_again() -> None:
    rig = Rig(put_strikes_pct=[])
    calls: list[int] = []
    rig.wallet.push = _unheard(calls)  # type: ignore[method-assign]
    rig.steps(1)
    assert calls == [1]
    rig.world.advance(130)  # old enough to be pushed again

    rig.config = rig.make_config(put_strikes_pct=[], dry_run=True)
    rig.book = rig.make_book()
    rig.steps(2)
    assert calls == [1]

    rig.config = rig.make_config(put_strikes_pct=[])
    rig.book = rig.make_book()
    rig.steps(1)
    assert calls == [1, 1]


def test_withdraw_all_in_dry_run_says_what_it_would_do() -> None:
    rig = Rig()
    rig.steps(4)
    rig.config = rig.make_config(dry_run=True)
    rig.book = rig.make_book()
    signed = rig.wallet.signed

    report = rig.book.withdraw_all()

    assert sum("dry run: would withdraw offers" in action for action in report.actions) == 2
    assert any("set dry_run: false" in problem for problem in report.problems)
    assert rig.wallet.signed == signed
    assert len(rig.store.offers(status="live")) == 2


def test_refresh_brings_the_book_up_to_date_and_sends_nothing() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    rig.world.take(rig.taker, rig.live_offer(call).offer_text)
    signed, pushed = rig.wallet.signed, rig.wallet.pushed
    rig.world.advance(15 * DAY)

    report = rig.book.refresh()

    assert rig.store.contract(call.launcher_id).status == store.SOLD
    assert (rig.wallet.signed, rig.wallet.pushed) == (signed, pushed)
    assert sum(action.startswith("would claw back") for action in report.actions) == 2


# ---------------------------------------------------------------------------
# Two processes, one database
# ---------------------------------------------------------------------------


def test_a_second_process_over_the_same_database_skips_its_step(tmp_path) -> None:
    rig = Rig()
    path = tmp_path / "options_jobs.db"
    rig.store = store.OptionStore(path)
    rig.book = rig.make_book()
    other = store.OptionStore(path)

    with other.acting():  # another process is in the middle of a step
        (report,) = rig.steps(1)
        assert any("step skipped" in problem for problem in report.problems)
        assert any("nothing withdrawn" in item for item in rig.book.withdraw_all().problems)
        assert any("not refreshed" in item for item in rig.book.refresh().problems)
    # It planned nothing from rows the other process was about to change.
    assert rig.contracts() == [] and rig.wallet.signed == 0

    rig.steps(1)
    assert len(rig.contracts()) == 1


# ---------------------------------------------------------------------------
# Sales that are easy to miss
# ---------------------------------------------------------------------------


def test_a_sale_and_an_exercise_between_two_steps_are_both_booked() -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    offer = rig.live_offer(call)
    # Taken and exercised before the book looks again: the option is melted, so
    # there is no new owner to notice.
    rig.world.take(rig.taker, offer.offer_text)
    loaded = rig.world.load(call.launcher_id)
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
    kinds = [event.kind for event in rig.store.events(call.launcher_id)]
    assert kinds == ["collateral_locked", "sold", "exercised"]
    (sold,) = rig.events("sold")
    assert sold.detail == {"asset": rig.usd.hex(), "amount": offer.price_amount}
    statuses = [row.status for row in rig.store.offers(call.launcher_id)]
    assert statuses.count("taken") == 1 and "live" not in statuses
    assert rig.store.offers(call.launcher_id)[-1].id == offer.id


def test_a_sale_is_not_booked_without_its_premium_while_the_chain_is_unreadable(
    monkeypatch,
) -> None:
    rig = Rig()
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    offer = rig.live_offer(call)
    rig.world.take(rig.taker, offer.offer_text)

    def unreachable(*args: Any, **kwargs: Any) -> Any:
        raise chain.ChainError("node unreachable")

    with monkeypatch.context() as patch:
        patch.setattr(chain, "bundle_status", unreachable)
        (report,) = rig.steps(1)

    assert any("could not be read yet" in problem for problem in report.problems)
    assert rig.store.contract(call.launcher_id).status == store.OPEN
    assert rig.events("sold") == []

    rig.steps(1)

    (sold,) = rig.events("sold")
    assert sold.detail == {"asset": rig.usd.hex(), "amount": offer.price_amount}


# ---------------------------------------------------------------------------
# Fees
# ---------------------------------------------------------------------------


def _auto_fee(rig_cap: int = 10**9, **overrides: Any) -> Rig:
    limits = {"max_fee_mojos": rig_cap, **overrides.pop("limits", {})}
    return Rig(fee_mojos="auto", limits=limits, **overrides)


def _fees_paid(reports: list[Any]) -> list[int]:
    paid = []
    for report in reports:
        for action in report.actions:
            if ", fee " in action:
                paid.append(int(action.split(", fee ")[1].split(" ")[0]))
    return paid


def _mint_cost(rig: Rig) -> int:
    spends, _ = unpack_bundle(rig.store.broadcasts(None)[0].bundle)
    return fees.bundle_cost(spends)


def test_auto_fee_pays_the_admission_rate_as_soon_as_there_is_any_pressure() -> None:
    rig = _auto_fee(put_strikes_pct=[])
    rig.wallet.rate = 1  # the node's estimate: far short of what a full mempool admits

    reports = rig.steps(2)

    (fee,) = _fees_paid(reports)
    # Priced from the bundle's own cost (a call mint is about 53 million) at
    # 5 mojos per cost and a tenth, not at the estimate.
    assert fee == math.ceil(_mint_cost(rig) * 5.5)
    assert 280_000_000 < fee < 320_000_000
    assert rig.store.broadcasts(None)[0].fee == fee
    assert len(rig.contracts(store.OPEN)) == 1
    assert rig.maker.balance() == 500 * XCH - 20 * XCH - builder.OPTION_COIN_AMOUNT - fee
    assert rig.wallet.rejected == []


def test_auto_fee_follows_an_estimate_above_the_admission_rate() -> None:
    rig = _auto_fee(put_strikes_pct=[])
    rig.wallet.rate = 8

    (fee,) = _fees_paid(rig.steps(2))

    assert fee == math.ceil(_mint_cost(rig) * 8.8)


def test_a_transaction_that_runs_out_teaches_the_book_to_pay_more() -> None:
    rig = _auto_fee(put_strikes_pct=[], confirm_timeout_seconds=600)
    rig.wallet.rate = 0  # the node says there is room, so nothing is paid...
    real_push = rig.wallet.push
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)
    assert rig.store.broadcasts(None)[0].fee == 0

    rig.wallet.push = real_push  # type: ignore[method-assign]
    rig.world.advance(1300)  # ...and the mint never gets in
    (report,) = rig.steps(1)

    # The failure is the evidence. The mint is made again at the admission rate
    # although the estimate still reads nothing.
    assert any("fee rate raised to 5.5" in problem for problem in report.problems)
    (fee,) = _fees_paid([report])
    retry = rig.store.broadcasts(None)[-1]
    assert fee == retry.fee == math.ceil(fees.bundle_cost(unpack_bundle(retry.bundle)[0]) * 5.5)
    rig.steps(1)
    assert len(rig.contracts(store.OPEN)) == 1


def test_what_was_learned_survives_a_restart_doubles_on_a_second_failure_and_fades() -> None:
    rig = _auto_fee(put_strikes_pct=[], confirm_timeout_seconds=600)
    rig.wallet.rate = 0
    rig.wallet.push = _unheard()  # type: ignore[method-assign]
    rig.steps(1)
    rig.world.advance(1300)
    rig.steps(1)  # first failure: 5.5, and a second mint, unheard too
    assert float(rig.store.recall("fee_rate")) == pytest.approx(5.5)

    rig.book = rig.make_book()  # a new process
    rig.world.advance(1300)
    (report,) = rig.steps(1)  # second failure, at 5.5 this time

    assert any("fee rate raised to 11" in problem for problem in report.problems)
    # Double what the failed one really paid, which rounding put a hair over 5.5.
    assert float(rig.store.recall("fee_rate")) == pytest.approx(11.0)
    third = rig.store.broadcasts(None)[-1]
    third_cost = fees.bundle_cost(unpack_bundle(third.bundle)[0])
    assert third.fee == pytest.approx(third_cost * 11, rel=1e-6)

    # Six hours without another failure halve it, and after six more it is gone.
    learned = rig.book._learned_rate
    assert learned(rig.world.now() + 6 * 3600) == pytest.approx(5.5)
    assert learned(rig.world.now() + 13 * 3600) == 0.0


def test_a_fee_held_down_by_the_cap_is_reported() -> None:
    rig = _auto_fee(rig_cap=20_000_000, put_strikes_pct=[])
    rig.wallet.rate = 5

    (first, _) = rig.steps(2)

    assert any("held to limits.max_fee_mojos (20000000)" in p for p in first.problems)


def test_auto_fee_pays_nothing_while_the_mempool_has_room() -> None:
    rig = _auto_fee(put_strikes_pct=[])
    rig.wallet.rate = 0

    reports = rig.steps(2)

    assert _fees_paid(reports) == []
    assert rig.maker.balance() == 500 * XCH - 20 * XCH - builder.OPTION_COIN_AMOUNT


def test_auto_fee_never_exceeds_the_cap() -> None:
    rig = _auto_fee(rig_cap=20_000_000, put_strikes_pct=[])
    rig.wallet.rate = 5  # a full mempool: far more than the cap allows

    reports = rig.steps(2)

    assert _fees_paid(reports) == [20_000_000]
    assert rig.maker.balance() == 500 * XCH - 20 * XCH - builder.OPTION_COIN_AMOUNT - 20_000_000


def test_auto_fee_with_no_cap_never_asks_and_never_pays() -> None:
    rig = _auto_fee(rig_cap=0, put_strikes_pct=[])
    rig.wallet.rate = 5

    reports = rig.steps(2)

    assert _fees_paid(reports) == [] and rig.wallet.rate_asked == 0


def test_no_estimate_from_the_wallet_means_going_by_what_was_learned() -> None:
    rig = _auto_fee(put_strikes_pct=[])
    rig.wallet.rate = None

    reports = rig.steps(2)

    # Nothing learned yet, so nothing paid; a failure would raise it all the same.
    assert _fees_paid(reports) == []
    assert any("no fee estimate" in problem for problem in reports[0].problems)
    assert len(rig.contracts(store.OPEN)) == 1
    rig.store.remember("fee_rate", "5.5")
    rig.store.remember("fee_rate_at", str(rig.world.now()))
    rig.spot = 1.60  # the contract drifts out of its slot: retired, and its slot minted again
    paid = _fees_paid(rig.steps(1))
    assert paid and all(fee > 0 for fee in paid)


def test_the_fee_estimate_is_asked_for_every_few_minutes_not_every_bundle() -> None:
    rig = _auto_fee()
    rig.wallet.rate = 1
    rig.steps(4)  # two mints
    assert rig.wallet.rate_asked == 1

    rig.world.advance(400)
    rig.spot = 1.60  # both contracts drift out of their slots and are retired
    rig.steps(1)
    assert rig.wallet.rate_asked == 2


def test_every_kind_of_transaction_is_priced_and_the_books_still_balance() -> None:
    rig = _auto_fee(put_strikes_pct=[], limits={"max_contracts": 1})
    rig.wallet.rate = 2
    reports = rig.steps(3)  # mint, confirm, offer
    call = _by_side(rig, "call", store.OPEN)

    rig.spot = 1.25  # withdraw the stale offer
    reports += rig.steps(2)
    rig.world.advance(14 * DAY - 1800)  # inside the stop window: retired by a cancel
    reports += rig.steps(3)

    paid = _fees_paid(reports)
    assert len(paid) == 3  # mint, withdrawal, cancel
    mint_fee, withdraw_fee, cancel_fee = paid
    # A withdrawal is the cheapest of the three and a cancel the dearest.
    assert withdraw_fee < mint_fee < cancel_fee
    assert rig.store.contract(call.launcher_id).status == store.CLOSED
    assert rig.maker.balance() == 500 * XCH - sum(paid)
    assert rig.wallet.rejected == []


def test_a_fixed_fee_is_paid_as_given() -> None:
    rig = Rig(put_strikes_pct=[], fee_mojos=1_000, limits={"max_fee_mojos": 5_000})

    reports = rig.steps(2)

    assert _fees_paid(reports) == [1_000] and rig.wallet.rate_asked == 0


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


def test_nothing_is_done_while_the_wallet_is_not_ready() -> None:
    # A wallet on the wrong key, or behind the chain, lists coins that are not
    # ours to spend or are already spent. Nothing is built from what it says.
    rig = Rig()
    rig.wallet.unready = "the options wallet is still syncing"

    (report,) = rig.steps(1)

    assert report.problems == ["the options wallet is still syncing", "nothing done this step"]
    assert not report.actions and report.chain_requests == 0
    assert rig.wallet.signed == 0 and rig.contracts() == []

    rig.wallet.unready = None
    rig.steps(3)
    assert len(rig.contracts(store.OPEN)) == 2


def test_the_kill_switch_does_not_wait_for_the_wallet() -> None:
    rig = Rig()
    rig.steps(4)
    offers = [rig.live_offer(item) for item in rig.contracts(store.OPEN)]
    rig.wallet.unready = "the options wallet is still syncing"

    report = rig.book.withdraw_all()

    # Said, and done anyway: the coins it spends are read from the chain.
    assert "the options wallet is still syncing" in report.problems
    assert sum("withdrawing offers" in action for action in report.actions) == 2
    for offer in offers:
        with pytest.raises(RejectedError, match="DoubleSpend"):
            rig.world.take(rig.taker, offer.offer_text)


# ---------------------------------------------------------------------------
# Splash: each offer is also handed to a daemon that gossips it
# ---------------------------------------------------------------------------

SPLASH_ON = {"splash": {"enabled": True}}


def test_nothing_goes_to_splash_unless_it_is_switched_on() -> None:
    rig = Rig()
    rig.steps(4)
    assert len(rig.store.offers(status="live")) == 2
    assert rig.splash.asked == 0
    assert all(offer.splash_at is None for offer in rig.store.offers())


def test_each_offer_is_handed_to_splash_once_when_it_is_made() -> None:
    rig = Rig(**SPLASH_ON)
    rig.steps(4)
    live = rig.store.offers(status="live")

    assert sorted(rig.splash.sent) == sorted(offer.offer_text for offer in live)
    assert all(offer.splash_at is not None for offer in live)

    # While they stand they are not sent again: the daemon was told once.
    rig.steps(3)
    assert rig.splash.asked == 2


def test_an_offer_made_again_before_it_runs_out_is_announced_in_its_turn() -> None:
    rig = Rig(**SPLASH_ON)
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    first = rig.live_offer(call)

    rig.world.advance(800)  # inside the last three minutes of a 15 minute offer
    rig.steps(1)

    second = rig.live_offer(call)
    assert second.id != first.id and second.offer_text in rig.splash.sent
    # This is how a network with no memory hears of each contract again.
    assert len(rig.splash.sent) == 4


def test_a_splash_daemon_that_is_down_costs_one_request_a_step_and_no_offer() -> None:
    rig = Rig(**SPLASH_ON)
    rig.splash.down = "no answer from the Splash daemon at http://127.0.0.1:4000"
    reports = rig.steps(4)

    # The offers were made, are on the site and can be taken: Splash is extra.
    live = rig.store.offers(status="live")
    assert len(live) == 2 and len(rig.desk.uploads) == 2
    assert all(offer.splash_at is None for offer in live)
    for report in reports:
        said = [text for text in report.problems if "Splash" in text]
        assert len(said) <= 1  # one line a step, not one for every contract
    assert "not announced on Splash: no answer from the Splash daemon" in reports[-1].problems[0]
    assert "tried again while the offer stands" in reports[-1].problems[0]
    # One try on each step that had an offer to send, however many were waiting.
    asked = rig.splash.asked
    rig.steps(1)
    assert rig.splash.asked == asked + 1

    # The daemon comes back: both standing offers are handed over, once.
    rig.splash.down = None
    (report,) = rig.steps(1)
    assert sorted(rig.splash.sent) == sorted(offer.offer_text for offer in live)
    assert not any("Splash" in text for text in report.problems)
    rig.steps(2)
    assert len(rig.splash.sent) == 2

    call = _by_side(rig, "call", store.OPEN)
    rig.world.take(rig.taker, rig.live_offer(call).offer_text)


def test_an_offer_the_ask_has_left_behind_is_never_announced() -> None:
    rig = Rig(**SPLASH_ON)
    rig.splash.down = "refused"
    rig.steps(4)
    call = _by_side(rig, "call", store.OPEN)
    old = rig.live_offer(call)

    # A small move: a dearer offer is posted beside the old one, which is left
    # to run out. The daemon is back, and must hear of the new one only.
    rig.spot = 1.115
    rig.splash.down = None
    rig.steps(2)

    new = rig.live_offer(call)
    assert new.id != old.id and new.offer_text in rig.splash.sent
    assert old.offer_text not in rig.splash.sent
    (kept,) = [offer for offer in rig.store.offers(call.launcher_id) if offer.id == old.id]
    assert kept.status == "live" and kept.splash_at is None


def test_a_dry_run_sends_nothing_to_splash() -> None:
    rig = Rig(dry_run=True, **SPLASH_ON)
    rig.steps(3)
    assert rig.splash.asked == 0
