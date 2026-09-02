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


def test_an_unknown_side_is_a_programming_error_not_a_silent_zero():
    """A typo'd side must fail loudly rather than quietly banking nothing."""
    try:
        earning_window("buy", 0.1, _book(bid=0.09),
                       ring_pct=RING, tick_size=TICK)
    except ValueError:
        return
    raise AssertionError("expected ValueError for an unknown side")
