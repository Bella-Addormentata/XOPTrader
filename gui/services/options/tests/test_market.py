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
