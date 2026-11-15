"""The desk monitor: other makers' offers, valued and followed to their end."""

from __future__ import annotations

from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, desk, market, monitor, store  # noqa: E402
from gui.services.options.tests.sim_world import (  # noqa: E402
    ADDITIONAL_DATA,
    CAT,
    XCH,
    SimChain,
    World,
)

DAY = 86400


class Board:
    """A desk that lists whatever offers it is handed."""

    def __init__(self) -> None:
        self.offers: dict[str, str] = {}
        self.down = False
        self.suggestions = 0
        self.fetches = 0

    def post(self, offer_id: str, text: str) -> None:
        self.offers[offer_id] = text

    def list_offers(self) -> desk.Listing:
        if self.down:
            raise desk.DeskLayoutError("/options: the options table was not found")
        return desk.Listing(tuple(self.offers), (), len(self.offers))

    def fetch_offer(self, offer_id: str) -> str | None:
        self.fetches += 1
        return self.offers.get(offer_id)

    def suggested_price(self, launcher_id: bytes) -> desk.Suggestion:
        self.suggestions += 1
        return desk.Suggestion(True, 1.10, 0.86, 0.05, 1.5)


class Rig:
    def __init__(self, node: Any = None) -> None:
        self.world = World()
        self.maker = self.world.party(300)
        self.taker = self.world.party(50)
        self.usd = self.world.issue_cat(self.taker, 2000)
        self.board = Board()
        self.store = store.OptionStore(":memory:")
        self.view: market.Market | None = market.Market(
            spot=1.10, sigma=0.86, bid=1.08, ask=1.12, as_of=0.0
        )
        self.monitor = monitor.DeskMonitor(
            desk_client=self.board,
            chain_reader=self.world.sim if node is None else node(self.world.sim),
            market=self.read_market,
            book_store=self.store,
            quote_asset_id=self.usd,
            quote_unit_mojos=CAT,
            agg_sig_me_additional_data=ADDITIONAL_DATA,
            clock=self.world.now,
        )

    def read_market(self) -> market.Market:
        if self.view is None:
            raise market.MarketError("no feed")
        return self.view

    def list_call(self, offer_id: str, *, ask: builder.Asset | None = None, ttl: int | None = 900):
        terms = builder.OptionTerms(
            builder.Asset(None, 20 * XCH),
            builder.Asset(self.usd, 24 * CAT),
            self.world.now() + 14 * DAY,
            self.maker.puzzle_hash,
        )
        minted = builder.build_mint(terms, self.maker.funding(), self.maker.custody())
        self.world.push(minted.coin_spends)
        draft = builder.build_offer(
            minted.option,
            ask or builder.Asset(self.usd, 1500),
            self.maker.puzzle_hash,
            self.maker.custody(),
            expires_at_seconds=None if ttl is None else self.world.now() + ttl,
        )
        text = draft.encode(self.world.signer().sign(draft.coin_spends))
        self.board.post(offer_id, text)
        return minted, text


def test_a_new_listing_is_validated_and_valued() -> None:
    rig = Rig()
    minted, _ = rig.list_call("offerA")

    report = rig.monitor.scan()

    (seen,) = report.new
    assert report.listed == 1 and report.problems == []
    assert seen.launcher_id == minted.launcher_id
    assert (seen.side, seen.size_xch, seen.strike) == ("call", 20.0, pytest.approx(1.20))
    assert seen.ask == pytest.approx(1.5)
    assert seen.site_premium == 1.5
    # A bought call is hedged by selling at the bid, so it is worth less than at spot.
    assert 0 < seen.hedged_value < seen.our_value
    assert seen.edge == pytest.approx(seen.hedged_value - 1.5)
    assert rig.store.listings(open_only=True)[0].offer_id == "offerA"


def test_a_known_listing_is_only_touched_not_refetched() -> None:
    rig = Rig()
    rig.list_call("offerA")
    rig.monitor.scan()
    first_seen = rig.store.listings()[0].first_seen

    rig.world.advance(60)
    report = rig.monitor.scan()

    assert report.new == [] and report.closed == {}
    assert rig.board.suggestions == 1
    (row,) = rig.store.listings()
    assert row.first_seen == first_seen and row.last_seen > first_seen


def test_outcomes_are_read_from_the_chain() -> None:
    rig = Rig()
    _, taken_text = rig.list_call("taken")
    withdrawn, _ = rig.list_call("withdrawn")
    rig.list_call("expired")
    rig.list_call("still-good", ttl=None)
    rig.monitor.scan()

    rig.world.take(rig.taker, taken_text)
    rig.world.push(
        builder.build_transfer(
            withdrawn.option, rig.maker.puzzle_hash, rig.maker.funding(), rig.maker.custody()
        ).coin_spends
    )
    rig.world.advance(1000)
    rig.board.offers.clear()
    report = rig.monitor.scan()

    assert report.closed == {
        "taken": monitor.TAKEN,
        "withdrawn": monitor.WITHDRAWN,
        "expired": monitor.EXPIRED,
    }
    # Delisted, but with no expiry and an unspent coin it can still be taken.
    assert [row.offer_id for row in rig.store.listings(open_only=True)] == ["still-good"]


def test_a_delisted_offer_with_no_expiry_is_followed_until_it_is_taken() -> None:
    rig = Rig()
    _, text = rig.list_call("undated", ttl=None)
    rig.monitor.scan()
    rig.board.offers.clear()

    # Hours off the listing end nothing: whoever kept the file can still take it.
    rig.world.advance(5 * 3600)
    assert rig.monitor.scan().closed == {}
    assert [row.offer_id for row in rig.store.listings(open_only=True)] == ["undated"]

    rig.world.take(rig.taker, text)
    assert rig.monitor.scan().closed == {"undated": monitor.TAKEN}


def test_a_delisted_offer_with_no_expiry_ends_with_its_contract() -> None:
    rig = Rig()
    rig.list_call("undated", ttl=None)
    rig.monitor.scan()
    rig.board.offers.clear()

    rig.world.advance(13 * DAY)
    assert rig.monitor.scan().closed == {}
    rig.world.advance(1 * DAY + 60)  # past the contract's own expiry
    assert rig.monitor.scan().closed == {"undated": monitor.EXPIRED}
    assert rig.store.listings(open_only=True) == []


def test_scans_ask_the_chain_only_about_what_is_new_or_being_followed() -> None:
    from gui.services.options.tests.test_chain_use import Node  # a node that counts requests

    rig = Rig(Node)
    rig.list_call("listed")
    rig.list_call("undated", ttl=None)

    first = rig.monitor.scan()
    assert len(first.new) == 2 and first.chain_requests > 0  # each new option is validated once

    rig.world.advance(300)
    assert rig.monitor.scan().chain_requests == 0  # nothing new, nothing delisted

    del rig.board.offers["undated"]
    for _ in range(3):  # still takeable, so followed: one request a scan
        rig.world.advance(300)
        followed = rig.monitor.scan()
        assert followed.closed == {} and followed.chain_requests == 1


def test_a_desk_outage_concludes_nothing() -> None:
    rig = Rig()
    rig.list_call("offerA")
    rig.monitor.scan()
    rig.board.down = True

    report = rig.monitor.scan()

    assert report.closed == {} and report.new == []
    assert any("could not read the desk" in problem for problem in report.problems)
    assert len(rig.store.listings(open_only=True)) == 1


def test_without_a_market_view_listings_are_still_recorded() -> None:
    rig = Rig()
    rig.view = None
    rig.list_call("offerA")

    report = rig.monitor.scan()

    assert report.market_error == "no feed"
    (seen,) = report.new
    assert seen.ask == pytest.approx(1.5)
    assert seen.our_value is None and seen.edge is None


def test_an_offer_priced_in_another_asset_is_recorded_unvalued() -> None:
    rig = Rig()
    rig.list_call("offerA", ask=builder.Asset(None, 2 * XCH))

    (seen,) = rig.monitor.scan().new

    assert seen.side is None and seen.ask is None and seen.edge is None
    assert rig.board.suggestions == 0


def test_an_offer_for_a_dead_option_is_a_problem_not_a_price() -> None:
    rig = Rig()
    minted, _ = rig.list_call("offerA")
    # The maker cancels the contract but its offer is still on the board.
    rig.world.push(_cancel_with_seed(rig, minted))

    report = rig.monitor.scan()

    assert report.new == []
    assert any("closed" in problem for problem in report.problems)
    (row,) = rig.store.listings()
    assert row.outcome == monitor.WITHDRAWN and row.our_value is None and row.edge is None
    # Judged once: still on the board, it is neither fetched nor reported again.
    fetches = rig.board.fetches
    again = rig.monitor.scan()
    assert rig.board.fetches == fetches and again.problems == [] and again.new == []


def test_an_offer_for_a_coin_that_has_moved_on_is_not_a_price() -> None:
    rig = Rig()
    minted, _ = rig.list_call("stale")
    # The maker moved the option (withdrawing this offer) but the board still lists it.
    # The contract is live and the signature is good; the offer is dead all the same.
    rig.world.push(
        builder.build_transfer(
            minted.option, rig.maker.puzzle_hash, rig.maker.funding(), rig.maker.custody()
        ).coin_spends
    )

    report = rig.monitor.scan()

    assert report.new == []
    assert any("already been spent" in problem for problem in report.problems)
    (row,) = rig.store.listings()
    assert row.outcome == monitor.WITHDRAWN
    assert row.our_value is None and row.hedged_value is None and row.site_premium is None
    assert rig.board.suggestions == 0
    assert rig.store.listings(open_only=True) == []


def test_an_offer_another_quote_sold_out_from_under_is_not_a_price() -> None:
    rig = Rig()
    minted, first = rig.list_call("first")
    # A second quote on the same option coin; taking either kills the other.
    second = builder.build_offer(
        minted.option,
        builder.Asset(rig.usd, 1400),
        rig.maker.puzzle_hash,
        rig.maker.custody(),
        expires_at_seconds=rig.world.now() + 900,
    )
    rig.board.post("second", second.encode(rig.world.signer().sign(second.coin_spends)))
    rig.world.take(rig.taker, first)

    report = rig.monitor.scan()

    assert report.new == []
    outcomes = {row.offer_id: row.outcome for row in rig.store.listings()}
    assert outcomes == {"first": monitor.TAKEN, "second": monitor.WITHDRAWN}


def test_an_offer_that_has_run_out_is_not_a_price() -> None:
    rig = Rig()
    rig.list_call("late", ttl=300)
    rig.world.advance(600)  # the board has not noticed

    report = rig.monitor.scan()

    assert report.new == []
    assert any("run out" in problem for problem in report.problems)
    (row,) = rig.store.listings()
    assert row.outcome == monitor.EXPIRED and row.our_value is None


def _cancel_with_seed(rig: Rig, minted: Any) -> list[Any]:
    rig.world.send_cat(rig.taker, rig.maker.puzzle_hash, rig.usd, 1)
    return builder.build_cancel(
        minted.option,
        minted.underlying,
        minted.locked,
        rig.maker.funding(rig.usd),
        rig.maker.custody(),
    )


def test_an_offer_whose_signature_is_bad_is_a_problem() -> None:
    rig = Rig()
    _, text = rig.list_call("offerA")
    bundle = sdk.decode_offer(text)
    forged = sdk.SpendBundle(bundle.coin_spends, sdk.Signature.infinity())
    rig.board.post("offerA", sdk.encode_offer(forged))

    report = rig.monitor.scan()

    assert report.new == []
    assert any("signature does not verify" in problem for problem in report.problems)


def test_a_freshly_fetched_stale_price_values_nothing() -> None:
    # A feed can hand over a price observed hours ago. The book refuses to
    # quote from one; the monitor must not record valuations from one either.
    rig = Rig()
    rig.monitor = monitor.DeskMonitor(
        desk_client=rig.board,
        chain_reader=rig.world.sim,
        market=rig.read_market,
        book_store=rig.store,
        quote_asset_id=rig.usd,
        quote_unit_mojos=CAT,
        agg_sig_me_additional_data=ADDITIONAL_DATA,
        clock=rig.world.now,
        max_spot_age_seconds=600,
    )
    rig.view = market.Market(
        spot=1.10, sigma=0.86, bid=1.08, ask=1.12, as_of=rig.world.now() - 7200
    )
    rig.list_call("offerA")

    report = rig.monitor.scan()

    assert report.market_error.startswith("the spot price is 72") and "s old" in report.market_error
    (seen,) = report.new
    assert seen.ask == pytest.approx(1.5)  # still recorded...
    assert seen.our_value is None and seen.edge is None  # ...but not valued

    # A price that is recent enough is used as before.
    rig.view = market.Market(spot=1.10, sigma=0.86, bid=1.08, ask=1.12, as_of=rig.world.now() - 60)
    rig.list_call("offerB")
    report = rig.monitor.scan()
    assert report.market_error == ""
    assert report.new[0].our_value is not None


class Clocked(SimChain):
    """The simulator's chain, noting each time its clock is asked and how deep."""

    def __init__(self, sim: Any) -> None:
        super().__init__(sim)
        self.asked: list[int] = []

    def chain_time(self, depth: int = 0) -> int:
        self.asked.append(depth)
        return super().chain_time(depth)


def _ahead(rig: Rig, seconds: int) -> Clocked:
    """Give the rig a monitor whose machine clock runs ahead of the chain's."""
    reader = Clocked(rig.world.sim)  # a reader that can tell the chain's time
    rig.monitor = monitor.DeskMonitor(
        desk_client=rig.board,
        chain_reader=reader,
        market=rig.read_market,
        book_store=rig.store,
        quote_asset_id=rig.usd,
        quote_unit_mojos=CAT,
        agg_sig_me_additional_data=ADDITIONAL_DATA,
        clock=lambda: rig.world.now() + seconds,
    )
    return reader


def test_an_offer_is_not_called_expired_while_the_chain_can_still_take_it() -> None:
    # An offer's end is judged by the chain against the chain's own clock,
    # which runs behind this machine's. Closing it by local time would record
    # it as expired for good, and the fill that follows would never be seen.
    rig = Rig()
    _ahead(rig, 300)
    _, text = rig.list_call("offerA", ttl=900)
    rig.monitor.scan()
    assert len(rig.store.listings(open_only=True)) == 1

    rig.world.advance(700)  # 200 s left by the chain's clock; 100 s past by ours
    del rig.board.offers["offerA"]
    report = rig.monitor.scan()
    assert report.closed == {} and len(rig.store.listings(open_only=True)) == 1

    rig.world.take(rig.taker, text)  # and it is taken after all
    report = rig.monitor.scan()
    assert report.closed == {"offerA": monitor.TAKEN}


def test_an_untaken_offer_is_closed_once_the_chain_s_clock_is_past_its_end() -> None:
    rig = Rig()
    reader = _ahead(rig, 0)
    rig.list_call("offerA", ttl=900)
    rig.monitor.scan()
    del rig.board.offers["offerA"]

    # Just past its end: the clock 32 blocks down is not there yet, and is not
    # even asked.
    rig.world.advance(1000)
    assert rig.monitor.scan().closed == {} and reader.asked == []

    # Ten minutes past, it is asked, 32 blocks down, says so, and the offer is closed.
    rig.world.advance(700)
    assert rig.monitor.scan().closed == {"offerA": monitor.EXPIRED}
    assert reader.asked == [32]

    # Another offer runs out straight after: the clock is not asked again for a
    # quarter of an hour, and the offer waits for it.
    rig.list_call("offerB", ttl=60)
    rig.monitor.scan()
    del rig.board.offers["offerB"]
    rig.world.advance(700)
    assert rig.monitor.scan().closed == {} and reader.asked == [32]
    rig.world.advance(300)
    assert rig.monitor.scan().closed == {"offerB": monitor.EXPIRED}
    assert reader.asked == [32, 32]
