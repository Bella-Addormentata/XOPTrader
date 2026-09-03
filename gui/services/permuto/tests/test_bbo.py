"""The book-aware placement window.

Every test here is anchored to a book actually observed on 2026-09-02, when
the account banked zero depth-seconds for a full session. Two of them cover
defects found by running this module against the live venue rather than by
reading it -- both would have shipped silently.
"""

from gui.services.permuto.bbo import (
    Book,
    earning_window,
    required_offset_pct,
)

RING = 2.0
TICK = 0.0001


def _book(bid=None, ask=None, market="NVDA-VOL-PERP"):
    return Book(market=market, best_bid=bid, best_ask=ask)


# --------------------------------------------------------------------------- #
# The ring bounds BOTH sides.
# --------------------------------------------------------------------------- #

def test_an_ask_must_clear_the_bid_and_stay_under_the_ring_ceiling():
    """Crossing the book is refused post-only; leaving the ring earns nothing."""
    oracle = 0.100
    window = earning_window("ask", oracle, _book(bid=0.1010),
                            ring_pct=RING, tick_size=TICK)
    assert window.low == 0.1010, "must sit strictly above the best bid"
    assert abs(window.high - 0.102) < 1e-12, "ring ceiling is oracle * 1.02"
    assert window.open


def test_a_bid_is_bounded_by_the_ring_ABOVE_it_not_only_below():
    """A bid over +ring_pct rests happily and earns exactly nothing.

    [regression] The first revision applied only the near edge per side, so a
    bid was capped by the best ask alone. With the ask side empty -- the
    venue's normal state on 2026-09-02 -- that left the bid unbounded above,
    and a leg placed there is 'failure wearing success's clothes': resting,
    accepted, and outside the credit ring.
    """
    oracle = 0.100
    window = earning_window("bid", oracle, _book(bid=0.0990, ask=None),
                            ring_pct=RING, tick_size=TICK)
    assert window.high <= oracle * 1.02 + 1e-12, (
        "bid window must not extend past the ring ceiling")
    assert window.low >= oracle * 0.98 - 1e-12


def test_an_empty_opposing_side_does_not_produce_an_infinite_window():
    """[regression] OverflowError: cannot convert float infinity to integer.

    An empty ask side used to make the bid's upper bound float('inf'), and the
    tick count is computed from the width. Every VOL market had an empty ask
    side when this was found, so the crash was not an edge case -- it was the
    first live call.
    """
    window = earning_window("bid", 0.100, _book(bid=0.0990, ask=None),
                            ring_pct=RING, tick_size=TICK)
    assert isinstance(window.ticks, int)
    assert window.ticks >= 0


def test_an_empty_opposing_side_requires_no_crossing_offset():
    oracle = 0.100
    assert required_offset_pct(
        "ask", oracle, _book(bid=None, ask=0.1010),
        ring_pct=RING, tick_size=TICK) == 0.0
    assert required_offset_pct(
        "bid", oracle, _book(bid=0.0990, ask=None),
        ring_pct=RING, tick_size=TICK) == 0.0


# --------------------------------------------------------------------------- #
# Quantisation.
# --------------------------------------------------------------------------- #

def test_a_one_tick_window_yields_a_price_rather_than_a_refusal():
    """A window one tick wide is placeable, and must not be discarded.

    NOT a regression test: the quantiser's epsilon was suspected of throwing
    this case away on 2026-09-02, and it does not -- setting ``_EPS = 0.0``
    changes nothing here. The property is still worth pinning, because the
    squeeze leaves windows exactly this narrow and rounding the wrong way
    silently forfeits the only placeable price on the book.
    """
    oracle = 0.085276
    book = _book(bid=0.0868)
    window = earning_window("ask", oracle, book,
                            ring_pct=RING, tick_size=TICK)
    assert window.ticks >= 1, "precondition: this book has room for one ask"

    offset = required_offset_pct("ask", oracle, book,
                                 ring_pct=RING, tick_size=TICK)
    assert offset is not None, "a one-tick window must produce a price"
    price = oracle * (1.0 + offset / 100.0)
    assert price > book.best_bid, "the leg must not cross the book"
    assert price <= oracle * 1.02 + 1e-9, "the leg must earn credit"


def test_the_returned_offset_always_lands_inside_the_ring():
    """Whatever the book, an offset we return must be a price that EARNS."""
    oracle = 0.0354
    for bid in (0.0340, 0.0350, 0.0355, 0.0358):
        book = _book(bid=bid)
        offset = required_offset_pct("ask", oracle, book,
                                     ring_pct=RING, tick_size=TICK)
        if offset is None:
            continue
        price = oracle * (1.0 + offset / 100.0)
        assert price > bid
        assert price <= oracle * 1.02 + 1e-9


# --------------------------------------------------------------------------- #
# The squeeze: saying "impossible" is the point.
# --------------------------------------------------------------------------- #

def test_a_bid_parked_on_the_ring_ceiling_closes_the_ask_side():
    """The 2026-09-02 squeeze, exactly as measured.

    Competitors held bids at +2.00% of oracle -- the ring ceiling itself --
    leaving a window 0.000002 wide against a 0.0001 tick. No tick-aligned ask
    both rests and earns, and the honest answer is None so the caller stops
    spending rate-limit tokens on certain refusals.
    """
    oracle = 0.072616
    book = _book(bid=0.074067)  # +2.0000% -- on the ceiling
    window = earning_window("ask", oracle, book,
                            ring_pct=RING, tick_size=TICK)
    assert window.ticks == 0
    assert required_offset_pct("ask", oracle, book,
                               ring_pct=RING, tick_size=TICK) is None


def test_a_bid_above_the_ring_ceiling_closes_it_too():
    """NVDA, 2026-09-02: best bid 0.212001 against a ceiling of 0.212013."""
    oracle = 0.207856
    book = _book(bid=0.212001)
    assert required_offset_pct("ask", oracle, book,
                               ring_pct=RING, tick_size=TICK) is None


# --------------------------------------------------------------------------- #
# Degenerate input must never raise into the quoting loop.
# --------------------------------------------------------------------------- #

def test_a_missing_oracle_or_tick_closes_the_window_without_raising():
    book = _book(bid=0.10)
    for oracle, tick in ((0.0, TICK), (-1.0, TICK), (0.1, 0.0)):
        window = earning_window("ask", oracle, book,
                                ring_pct=RING, tick_size=tick)
        assert window.ticks == 0
        assert required_offset_pct("ask", oracle, book,
                                   ring_pct=RING, tick_size=tick) is None
    assert required_offset_pct(
        "ask", 0.0, _book(bid=None),
        ring_pct=RING, tick_size=TICK) is None


def test_an_unknown_side_is_a_programming_error_not_a_silent_zero():
    """A typo'd side must fail loudly rather than quietly banking nothing."""
    try:
        earning_window("buy", 0.1, _book(bid=0.09),
                       ring_pct=RING, tick_size=TICK)
    except ValueError:
        return
    raise AssertionError("expected ValueError for an unknown side")


def test_empty_opposing_side_includes_exact_ring_boundary_prices():
    """When opposing side is absent, ring floor/ceiling boundaries are inclusive."""
    oracle = 0.1000  # ring: [0.0980, 0.1020]
    # Ask window with no bids: lower bound 0.0980 is inclusive (0.0980 is legal ask)
    w_ask = earning_window("ask", oracle, _book(bid=None, ask=None),
                           ring_pct=RING, tick_size=TICK)
    assert abs(w_ask.first - 0.0980) < 1e-9
    assert abs(w_ask.last - 0.1020) < 1e-9
    assert w_ask.ticks == 41

    # Bid window with no asks: upper bound 0.1020 is inclusive (0.1020 is legal bid)
    w_bid = earning_window("bid", oracle, _book(bid=None, ask=None),
                           ring_pct=RING, tick_size=TICK)
    assert abs(w_bid.first - 0.0980) < 1e-9
    assert abs(w_bid.last - 0.1020) < 1e-9
    assert w_bid.ticks == 41


def test_fetch_book_validates_payload_shape_and_rejects_malformed(monkeypatch):
    from gui.services.permuto import bbo

    # 1. Error payload
    monkeypatch.setattr(bbo, "_get", lambda url, timeout: {"error": "market paused"})
    assert bbo.fetch_book("NVDA-VOL-PERP", base_url="https://test") is None

    # 2. Non-dict payload
    monkeypatch.setattr(bbo, "_get", lambda url, timeout: ["not", "a", "dict"])
    assert bbo.fetch_book("NVDA-VOL-PERP", base_url="https://test") is None

    # 3. Non-list bids/asks
    monkeypatch.setattr(bbo, "_get", lambda url, timeout: {"bids": "junk", "asks": []})
    assert bbo.fetch_book("NVDA-VOL-PERP", base_url="https://test") is None

    # 4. Unparseable first price
    monkeypatch.setattr(bbo, "_get", lambda url, timeout: {"bids": [{"price": "NaN"}], "asks": []})
    assert bbo.fetch_book("NVDA-VOL-PERP", base_url="https://test") is None

    # 5. Non-positive first price
    monkeypatch.setattr(bbo, "_get", lambda url, timeout: {"bids": [{"price": "-0.05"}], "asks": []})
    assert bbo.fetch_book("NVDA-VOL-PERP", base_url="https://test") is None

    # 6. Valid book
    monkeypatch.setattr(bbo, "_get", lambda url, timeout: {
        "bids": [{"price": "0.1001", "size": "100"}],
        "asks": [{"price": "0.1005", "size": "100"}]
    })
    valid = bbo.fetch_book("NVDA-VOL-PERP", base_url="https://test")
    assert valid is not None
    assert valid.best_bid == 0.1001
    assert valid.best_ask == 0.1005


def test_microtick_fallback_opens_window_under_subtick_squeeze():
    """When a competitor bid sits at 0.104103 (6 decimals) near the +2.0% ceiling (0.104111),
    coarse 0.0001 ticks would shut the window, but micro-ticks (1e-6) allow resting asks."""
    from gui.services.permuto.bbo import placement_prices, rests_and_earns

    oracle = 0.102070
    book = _book(bid=0.104103, ask=None, market="QQQ-VOL-PERP")
    window = earning_window("ask", oracle, book, ring_pct=RING, tick_size=TICK, allow_subtick=True)
    assert window.open, "micro-tick fallback must open the earning window"
    assert window.first == 0.104104
    assert window.last <= oracle * 1.02 + 1e-9

    prices = placement_prices(oracle, oracle, book, preferred_offset_pct=0.25, ring_pct=RING, tick_size=TICK, allow_subtick=True)
    assert prices is not None
    bid, ask = prices
    assert ask == 0.104104
    assert rests_and_earns("ask", ask, oracle, book, ring_pct=RING, tick_size=TICK)
    assert rests_and_earns("bid", bid, oracle, book, ring_pct=RING, tick_size=TICK)
