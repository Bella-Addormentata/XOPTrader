"""Spot, volatility and the executable range, from scripted feeds."""

from __future__ import annotations

import json
import math

import pytest

from gui.services.options import market

USD = bytes.fromhex("fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d")


def _history(hours: int = 720, sigma_per_hour: float = 0.01) -> list[list[float]]:
    # Alternating +/- moves: a known standard deviation per hour.
    price, rows = 1.0, []
    for hour in range(hours):
        rows.append([hour * 3600 * 1000.0, price])
        price *= math.exp(sigma_per_hour if hour % 2 == 0 else -sigma_per_hour)
    return rows


def _offer(give_code: str, give: float, want: float) -> dict:
    other = "wUSDC.b" if give_code == "XCH" else "XCH"
    return {
        "offered": [{"code": give_code, "amount": give}],
        "requested": [{"code": other, "amount": want}],
    }


class Feeds:
    def __init__(self) -> None:
        self.spot = 1.11
        self.observed: object = 4940  # a minute before the 5000.0 the clock reads
        self.history = _history()
        self.asks = [_offer("XCH", 100.0, 113.0), _offer("XCH", 50.0, 58.0)]
        self.bids = [_offer("wUSDC.b", 109.0, 100.0), _offer("wUSDC.b", 1.33, 1.149)]
        self.calls: list[str] = []
        self.fail: str | None = None

    def __call__(self, url: str) -> tuple[int, str]:
        self.calls.append(url)
        if self.fail and self.fail in url:
            return 500, "oops"
        if "simple/price" in url:
            quote: dict = {"usd": self.spot}
            if self.observed is not None:
                quote["last_updated_at"] = self.observed
            return 200, json.dumps({"chia": quote})
        if "market_chart" in url:
            return 200, json.dumps({"prices": self.history})
        if "offered=xch" in url:
            return 200, json.dumps({"offers": self.asks})
        return 200, json.dumps({"offers": self.bids})


def _source(feeds: Feeds, now: list[float] | None = None, **kwargs) -> market.MarketSource:
    clock = (lambda: now[0]) if now is not None else (lambda: 5000.0)
    return market.MarketSource(USD, hedge_size_xch=20.0, get=feeds, clock=clock, **kwargs)


def test_volatility_is_annualised_from_the_series_own_spacing() -> None:
    hourly = [(at / 1000.0, price) for at, price in _history(sigma_per_hour=0.01)]
    assert market.realized_volatility(hourly) == pytest.approx(0.01 * math.sqrt(24 * 365), rel=0.01)
    # The same moves five minutes apart are twelve times as many per year.
    fast = [(at / 12.0, price) for at, price in hourly]
    assert market.realized_volatility(fast) == pytest.approx(
        0.01 * math.sqrt(24 * 365 * 12), rel=0.01
    )


def test_volatility_needs_enough_history() -> None:
    with pytest.raises(market.MarketError, match="too short"):
        market.realized_volatility([(float(i), 1.0 + i / 100) for i in range(10)])
    with pytest.raises(market.MarketError):
        market.realized_volatility([(float(i * 3600), 1.0) for i in range(100)])  # flat


def test_executable_price_takes_whole_offers_best_first() -> None:
    levels = [(1.10, 5.0), (1.12, 30.0), (1.50, 100.0)]
    assert market.executable_price(levels, 5.0) == pytest.approx(1.10)
    # Needs the second offer, all of it.
    assert market.executable_price(levels, 20.0) == pytest.approx((1.10 * 5 + 1.12 * 30) / 35)
    assert market.executable_price(levels, 500.0) is None
    assert market.executable_price([], 1.0) is None


def test_read_combines_the_feed_and_the_book() -> None:
    feeds = Feeds()

    view = _source(feeds).read()

    assert view.spot == 1.11
    assert view.sigma == pytest.approx(0.01 * math.sqrt(24 * 365), rel=0.01)
    assert view.ask == pytest.approx(1.13)  # the 100 XCH ask covers a 20 XCH hedge
    # The dust bid at 1.158 cannot fill 20 XCH, so the real bid sets the price.
    assert view.bid == pytest.approx((1.33 + 109.0) / (1.149 + 100.0))
    # As old as the observation, not as the request that fetched it.
    assert view.as_of == 4940.0
    assert any("include_last_updated_at=true" in url for url in feeds.calls)
    # Without it CoinGecko rounds the price to the cent: 1.15 for 1.1512.
    assert any("simple/price" in url and "precision=full" in url for url in feeds.calls)
    view.require_usable(now=5000.0, max_age_seconds=600, max_spread_pct=10, require_book=True)


def test_a_freshly_fetched_stale_price_is_stale() -> None:
    feeds = Feeds()
    feeds.observed = 5000 - 3600  # the feed answers at once, with an hour-old price

    view = _source(feeds).read()

    assert view.as_of == 1400.0
    with pytest.raises(market.MarketError, match="3600 s old"):
        view.require_usable(now=5000.0, max_age_seconds=600, max_spread_pct=10, require_book=True)


@pytest.mark.parametrize("observed", [None, "soon", 0, -5, float("nan")])
def test_a_price_without_a_usable_time_is_refused(observed: object) -> None:
    feeds = Feeds()
    feeds.observed = observed
    with pytest.raises(market.MarketError, match="when the XCH price was observed|unusable time"):
        _source(feeds).read()


def test_a_price_stamped_in_the_future_is_refused_beyond_clock_skew() -> None:
    feeds = Feeds()
    feeds.observed = 5000 + 120  # two minutes ahead: ordinary skew, read as "now"
    assert _source(feeds).read().as_of == 5000.0

    feeds.observed = 5000 + 3600
    with pytest.raises(market.MarketError, match="in the future"):
        _source(feeds).read()


def test_history_is_fetched_once_and_reused() -> None:
    feeds, now = Feeds(), [5000.0]
    source = _source(feeds, now)
    source.read()
    now[0] += 3600
    source.read()
    assert sum("market_chart" in url for url in feeds.calls) == 1
    now[0] += 6 * 3600
    source.read()
    assert sum("market_chart" in url for url in feeds.calls) == 2


def test_multi_asset_and_malformed_offers_are_ignored() -> None:
    feeds = Feeds()
    feeds.asks = [
        {"offered": [{"amount": 1.0}, {"amount": 2.0}], "requested": [{"amount": 1.0}]},
        {"offered": [{"amount": "x"}], "requested": [{"amount": 1.0}]},
        {"nonsense": True},
        _offer("XCH", 25.0, 28.0),
    ]
    assert _source(feeds).read().ask == pytest.approx(1.12)


def test_a_book_outage_leaves_spot_but_no_range() -> None:
    feeds = Feeds()
    feeds.fail = "dexie"
    view = _source(feeds).read()
    assert view.spot == 1.11 and view.bid is None and view.ask is None
    with pytest.raises(market.MarketError, match="hedge size"):
        view.require_usable(now=5000.0, max_age_seconds=600, max_spread_pct=10, require_book=True)
    view.require_usable(now=5000.0, max_age_seconds=600, max_spread_pct=10, require_book=False)


@pytest.mark.parametrize("fail", ["simple/price", "market_chart"])
def test_no_external_price_means_no_market(fail: str) -> None:
    feeds = Feeds()
    feeds.fail = fail
    with pytest.raises(market.MarketError):
        _source(feeds).read()


def _view(**overrides) -> market.Market:
    values = {"spot": 1.11, "sigma": 0.86, "bid": 1.09, "ask": 1.13, "as_of": 1000.0}
    values.update(overrides)
    return market.Market(**values)


@pytest.mark.parametrize(
    "view, now, message",
    [
        (_view(), 2000.0, "s old"),
        (_view(spot=0.0), 1000.0, "no spot"),
        (_view(sigma=float("nan")), 1000.0, "no volatility"),
        (_view(bid=1.14, ask=1.13), 1000.0, "crossed"),
        (_view(bid=0.90, ask=1.30), 1000.0, "spread"),
        (_view(spot=1.20), 1000.0, "outside the executable range"),
        (_view(spot=1.00), 1000.0, "outside the executable range"),
    ],
)
def test_unusable_views_are_refused(view: market.Market, now: float, message: str) -> None:
    with pytest.raises(market.MarketError, match=message):
        view.require_usable(now=now, max_age_seconds=600, max_spread_pct=10, require_book=True)


def test_the_coingecko_key_goes_in_a_header_and_only_to_coingecko() -> None:
    feeds = Feeds()
    sent: list[tuple[str, dict | None]] = []

    def get(url: str, headers: dict | None = None) -> tuple[int, str]:
        sent.append((url, headers))
        return feeds(url)

    market.MarketSource(
        USD, hedge_size_xch=20.0, get=get, clock=lambda: 5000.0, coingecko_api_key="CG-secret"
    ).read()

    for url, headers in sent:
        assert "CG-secret" not in url  # an address ends up in error messages
        if "coingecko" in url:
            assert headers == {"x-cg-demo-api-key": "CG-secret"}
        else:
            assert headers is None
    assert sum("coingecko" in url for url, _ in sent) == 2


def test_the_book_is_asked_for_best_price_first() -> None:
    feeds = Feeds()
    _source(feeds).read()
    book = [url for url in feeds.calls if "/v1/offers" in url]
    assert len(book) == 2 and all("sort=price" in url for url in book)


# ---------------------------------------------------------------------------
# Refusals carry a short reason; our own offers are not liquidity
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "view, now, reason",
    [
        (_view(), 2000.0, market.STALE),
        (_view(bid=None), 1000.0, market.NO_BOOK),
        (_view(bid=1.14, ask=1.13), 1000.0, market.CROSSED),
        (_view(bid=0.90, ask=1.30), 1000.0, market.TOO_WIDE),
        (_view(spot=1.20), 1000.0, market.OFF_PEG),
    ],
)
def test_a_refusal_names_its_reason_in_words_that_do_not_change(
    view: market.Market, now: float, reason: str
) -> None:
    # The journal groups stops by these, so a spot that moves a cent does not
    # look like a new kind of stop.
    with pytest.raises(market.MarketError) as caught:
        view.require_usable(now=now, max_age_seconds=600, max_spread_pct=10, require_book=True)
    assert caught.value.reason == reason


def test_a_feed_failure_is_market_data_unavailable() -> None:
    feeds = Feeds()
    feeds.fail = "simple/price"
    with pytest.raises(market.MarketError) as caught:
        _source(feeds).read()
    assert caught.value.reason == market.NO_DATA


def _owned(offer: dict, trade_id: str) -> dict:
    return {**offer, "trade_id": trade_id}


def test_the_wallets_own_offers_are_left_out_of_the_range() -> None:
    # On 2026-11-15 the engine's resting offers, left on Dexie while it was
    # stopped, were the edges of the range all evening.
    feeds = Feeds()
    ours = "0x" + "ab" * 32
    feeds.asks = [_owned(_offer("XCH", 100.0, 110.0), ours), _offer("XCH", 100.0, 113.0)]
    feeds.bids = [_owned(_offer("wUSDC.b", 112.0, 100.0), "0x" + "cd" * 32)]
    feeds.bids.append(_offer("wUSDC.b", 109.0, 100.0))

    counted = _source(feeds).read()
    source = _source(feeds, own_offers=lambda: [bytes.fromhex("ab" * 32)])
    mine_left_out = source.read()

    assert counted.ask == pytest.approx(1.10)
    assert mine_left_out.ask == pytest.approx(1.13)
    assert mine_left_out.bid == counted.bid == pytest.approx(1.12)  # not ours: still counted
    assert source.left_out == 1


class PagedFeeds(Feeds):
    """Dexie as it answers: a page of offers at a time, with how many there are."""

    def __call__(self, url: str) -> tuple[int, str]:
        if "/v1/offers" not in url:
            return super().__call__(url)
        self.calls.append(url)
        query = dict(part.split("=", 1) for part in url.split("?", 1)[1].split("&"))
        size, page = int(query["page_size"]), int(query["page"])
        side = self.asks if query["offered"] == "xch" else self.bids
        offers = side[(page - 1) * size : page * size]
        return 200, json.dumps({"count": len(side), "page": page, "offers": offers})


def _pages(feeds: Feeds, side: str) -> list[int]:
    """The pages asked for, in order, of the asks (XCH offered) or the bids."""
    mark = "offered=xch&" if side == "asks" else "&requested=xch&"
    asked = [url for url in feeds.calls if mark in url]
    return [int(url.split("&page=", 1)[1].split("&", 1)[0]) for url in asked]


def test_a_page_of_our_own_offers_does_not_hide_the_book_behind_it() -> None:
    # A first page that is all ours leaves nothing of it; the hedge is
    # covered by others' offers on the next one.
    feeds = PagedFeeds()
    ids = [f"{index:064x}" for index in range(100)]
    feeds.asks = [_owned(_offer("XCH", 1.0, 1.10), "0x" + tid) for tid in ids]
    feeds.asks.append(_offer("XCH", 100.0, 113.0))
    source = _source(feeds, own_offers=lambda: [bytes.fromhex(tid) for tid in ids])

    view = source.read()

    assert view.ask == pytest.approx(1.13)
    assert source.left_out == 100
    assert _pages(feeds, "asks") == [1, 2]


def test_further_pages_are_read_only_while_the_hedge_is_not_yet_covered() -> None:
    feeds = PagedFeeds()
    feeds.asks = [_offer("XCH", 1.0, 1.10) for _ in range(250)]  # 100 XCH a page
    feeds.bids = [_offer("wUSDC.b", 0.1, 0.1) for _ in range(1000)]  # 10 XCH a page

    view = _source(feeds).read()  # a hedge of 20 XCH

    assert view.ask == pytest.approx(1.10) and _pages(feeds, "asks") == [1]
    assert view.bid == pytest.approx(1.0) and _pages(feeds, "bids") == [1, 2]


def test_a_side_that_never_covers_the_hedge_is_read_for_a_few_pages_at_most() -> None:
    feeds = PagedFeeds()
    feeds.asks = [_offer("XCH", 0.01, 0.011) for _ in range(2000)]  # 1 XCH a page

    view = _source(feeds).read()

    assert view.ask is None
    assert _pages(feeds, "asks") == [1, 2, 3, 4, 5]


def test_if_the_wallet_cannot_list_its_offers_every_offer_counts(caplog) -> None:
    feeds = Feeds()
    feeds.asks = [_owned(_offer("XCH", 100.0, 110.0), "0x" + "ab" * 32)]
    asked: list[int] = []

    def failing() -> list[bytes]:
        asked.append(1)
        raise RuntimeError("wallet down")

    source = _source(feeds, own_offers=failing)
    with caplog.at_level("WARNING"):
        first = source.read()
        source.read()

    assert first.ask == pytest.approx(1.10)  # as if nothing were known to be ours
    assert len(asked) == 2  # asked every read...
    assert sum("could not ask the wallet" in r.message for r in caplog.records) == 1  # ...said once


# ---------------------------------------------------------------------------
# [PEG] A stale Dexie offer is not a depeg
# ---------------------------------------------------------------------------


def _check(view: market.Market, tolerance: float) -> None:
    view.require_usable(
        now=1000.0,
        max_age_seconds=600,
        max_spread_pct=10,
        require_book=True,
        peg_tolerance_pct=tolerance,
    )


@pytest.mark.parametrize(
    "spot, bid, ask",
    [
        (0.9435, 0.9594, 0.9665),  # 2026-11-24 12:29 CT: 1.66% under a stale 5 XCH bid
        (0.9450, 0.9686, 0.9834),  # 10:37 CT: 2.44% under the stale bid before it
        (1.1500, 1.0900, 1.1300),  # above the ask instead: 1.77%
    ],
)
def test_spot_a_little_outside_the_range_is_quoted_within_the_tolerance(spot, bid, ask) -> None:
    view = _view(spot=spot, bid=bid, ask=ask)
    _check(view, 3.0)
    with pytest.raises(market.MarketError) as caught:
        _check(view, 0.0)  # the strict rule, as before the tolerance
    assert caught.value.reason == market.OFF_PEG


def test_spot_further_outside_than_the_tolerance_is_still_a_possible_depeg() -> None:
    view = _view(spot=1.00, bid=1.05, ask=1.06)  # 4.76% under the bid
    with pytest.raises(market.MarketError) as caught:
        _check(view, 3.0)
    assert caught.value.reason == market.OFF_PEG
    assert str(caught.value) == (
        "spot 1.0000 is 4.76% outside the executable range 1.0500-1.0600, more than the "
        "3% allowed; the quote asset may be off its peg"
    )


def test_a_book_crossed_by_a_stale_offer_is_judged_by_the_same_tolerance() -> None:
    # 2026-11-24 13:13-13:35 CT: the stale 0.9594 bid sat over a fresh ask.
    view = _view(spot=0.9450, bid=0.9594, ask=0.9590)
    assert view.gaps_pct() == (pytest.approx(0.0417, abs=1e-3), pytest.approx(1.45, abs=0.01))
    _check(view, 3.0)
    with pytest.raises(market.MarketError) as caught:
        _check(view, 0.0)
    assert caught.value.reason == market.CROSSED
    assert str(caught.value) == "the Dexie book is crossed by 0.04% at the hedge size"

    wide = _view(spot=1.00, bid=1.05, ask=1.00)  # crossed by 4.88%
    with pytest.raises(market.MarketError) as caught:
        _check(wide, 3.0)
    assert caught.value.reason == market.CROSSED
    assert "more than the 3% allowed" in str(caught.value)


def test_a_crossed_book_still_has_to_hold_spot_within_the_tolerance() -> None:
    view = _view(spot=0.90, bid=0.9594, ask=0.9590)  # crossed a little, spot 6.2% under
    with pytest.raises(market.MarketError) as caught:
        _check(view, 3.0)
    assert caught.value.reason == market.OFF_PEG
    assert "range 0.9590-0.9594" in str(caught.value), "the range runs low to high"


def test_a_sound_view_or_one_with_no_book_has_no_gaps() -> None:
    assert _view().gaps_pct() == (0.0, 0.0)
    assert _view(bid=None).gaps_pct() == (0.0, 0.0)
    assert _view(spot=1.13).gaps_pct() == (0.0, 0.0), "at the ask is inside"


@pytest.mark.parametrize(
    "overrides",
    [
        {"bid": float("nan")},
        {"ask": float("nan")},
        {"bid": float("inf")},
        {"ask": float("-inf")},
        {"bid": 0.0},
        {"ask": -1.0},
    ],
)
@pytest.mark.parametrize("tolerance", [0.0, 3.0])
def test_a_book_price_that_is_not_a_positive_number_is_refused(overrides, tolerance) -> None:
    """[review #263] NaN fails every comparison: the gaps of a NaN bid are no gaps at all,
    so the view is refused before they are worked out, whatever the tolerance."""
    view = _view(**{"spot": 1.10, "bid": 1.09, "ask": 1.10, **overrides})
    with pytest.raises(market.MarketError) as caught:
        _check(view, tolerance)
    assert caught.value.reason == market.NO_DATA


def test_a_dexie_offer_with_an_amount_that_is_not_a_number_is_left_out() -> None:
    """[review #263] json reads NaN and Infinity, and `<= 0` lets NaN through."""
    feeds = Feeds()
    feeds.bids = [_offer("wUSDC.b", float("nan"), 100.0), *feeds.bids]
    feeds.asks = [_offer("XCH", float("inf"), 100.0), *feeds.asks]

    view = _source(feeds).read()

    assert view.bid == pytest.approx((1.33 + 109.0) / (1.149 + 100.0))
    assert view.ask == pytest.approx(1.13)
    assert market.executable_price([(float("nan"), 50.0), (1.10, 50.0)], 20.0) == 1.10
    assert market.executable_price([(1.0, float("nan")), (1.10, 50.0)], 20.0) == 1.10
