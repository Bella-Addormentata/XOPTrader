"""[MILLIETH-TOPUP 2026-10-24] Buying wmilliETH.b on Dexie (dexie_buy.py).

The fixtures are shaped like the live book read on 2026-10-24: the cheapest
ask was 0.547 wmilliETH.b for 1.8277 XCH, the rest at 10 XCH a unit.
"""

from __future__ import annotations

from decimal import Decimal

import pytest

from gui.services.warp import dexie_buy as D

WM = "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6"
TID_A = "aa" * 32
TID_B = "bb" * 32
TID_C = "cc" * 32


def _offer(trade_id, cat_units, xch_units, *, status=0, asset=WM,
           extra_offered=None, offer="offer1qqzexample"):
    offered = [{"id": asset, "amount": cat_units}]
    if extra_offered:
        offered.append(extra_offered)
    return {"id": "dx" + trade_id[:6], "trade_id": trade_id, "offer": offer,
            "status": status, "offered": offered,
            "requested": [{"id": "xch", "amount": xch_units}]}


BOOK = {"offers": [
    _offer(TID_B, 1.0, 10.0),
    _offer(TID_A, 0.547, 1.8277),
    _offer(TID_C, 2.0, 20.0),
]}


# -- parsing ------------------------------------------------------------------

def test_asks_are_parsed_exactly_and_sorted_cheapest_first():
    asks = D.parse_asks(BOOK, WM)
    assert [a.trade_id for a in asks] == [TID_A, TID_B, TID_C]
    a = asks[0]
    assert (a.cat_mojos, a.xch_mojos) == (547, 1_827_700_000_000)
    assert a.xch_per_unit == Decimal("1.8277") / Decimal("0.547")


@pytest.mark.parametrize("bad", [
    _offer(TID_A, 0.547, 1.8277, status=4),                    # not open
    _offer(TID_A, 0.5471, 1.8277),                             # sub-mojo CAT
    _offer(TID_A, 0.547, 1.8277, asset="ab" * 32),             # other CAT
    _offer(TID_A, 0.547, 1.8277,
           extra_offered={"id": "xch", "amount": 1}),          # a bundle
    _offer(TID_A, 0.547, 1.8277, offer="not-an-offer"),
    _offer("zz" * 32, 0.547, 1.8277),                          # bad trade id
    _offer(TID_A, -1, 1.8277),
    _offer(TID_A, True, 1.8277),
])
def test_anything_but_a_plain_cat_for_xch_ask_is_dropped(bad):
    assert D.parse_asks({"offers": [bad]}, WM) == []


def test_garbage_payloads_parse_to_nothing():
    for payload in (None, [], {}, {"offers": None}, {"offers": [1, "x"]}):
        assert D.parse_asks(payload, WM) == []


def test_fetch_asks_queries_the_pair_cheapest_first():
    seen = {}

    def get_json(url, params, headers):
        seen.update(url=url, params=params)
        return BOOK

    asks = D.fetch_asks(get_json, "https://api.dexie.space/v1/", WM)
    assert seen["url"] == "https://api.dexie.space/v1/offers"
    assert seen["params"]["offered"] == WM
    assert seen["params"]["requested"] == "xch"
    assert seen["params"]["status"] == 0
    assert seen["params"]["page"] == 1, "a short first page ends the book"
    assert asks[0].trade_id == TID_A


def test_fetch_asks_reads_past_a_full_first_page():
    """[review] The first page may be all excluded asks; later pages count."""
    pages = []

    def get_json(url, params, headers):
        pages.append(params["page"])
        if params["page"] == 1:
            return {"offers": [_offer(TID_B, 1.0, 10.0), _offer(TID_C, 2.0, 20.0)]}
        return {"offers": [_offer(TID_A, 0.547, 1.8277)]}

    asks = D.fetch_asks(get_json, "https://api.dexie.space/v1", WM, page_size=2)
    assert pages == [1, 2]
    assert [a.trade_id for a in asks] == [TID_A, TID_B, TID_C]


def test_fetch_asks_stops_at_its_page_budget():
    pages = []

    def get_json(url, params, headers):
        pages.append(params["page"])
        return {"offers": [_offer(TID_B, 1.0, 10.0), _offer(TID_C, 2.0, 20.0)]}

    D.fetch_asks(get_json, "https://api.dexie.space/v1", WM, page_size=2,
                 max_pages=3)
    assert pages == [1, 2, 3]


# -- reference price ---------------------------------------------------------

def test_the_reference_is_eth_per_thousand_over_xch():
    seen = {}

    def get_json(url, params, headers):
        seen.update(headers=headers)
        return {"ethereum": {"usd": 3950.0}, "chia": {"usd": 2.5}}

    ref = D.reference_xch_per_millieth(get_json, api_key="k")
    assert ref == Decimal("3950.0") / 1000 / Decimal("2.5")     # 1.58 XCH
    assert seen["headers"] == {"x-cg-demo-api-key": "k"}


@pytest.mark.parametrize("payload", [
    {}, {"ethereum": {"usd": 3950.0}}, {"ethereum": {"usd": 0}, "chia": {"usd": 2}},
    {"ethereum": {"usd": "x"}, "chia": {"usd": 2}}, None,
])
def test_no_reference_means_no_price(payload):
    assert D.reference_xch_per_millieth(lambda *a: payload) is None


# -- choosing ----------------------------------------------------------------

REF = Decimal("3.3")   # XCH per wmilliETH.b


def _pick(asks, **kw):
    args = dict(want_mojos=500, reference_xch_per_unit=REF,
                max_premium_pct=3.0, max_spend_xch_mojos=25 * 10 ** 12)
    args.update(kw)
    return D.pick_ask(asks, **args)


def test_the_cheapest_covering_ask_within_the_premium_is_taken():
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks).trade_id == TID_A        # 3.341 XCH/unit <= 3.399


def test_an_ask_above_the_premium_is_never_taken():
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks, max_premium_pct=1.0) is None    # 3.341 > 3.333


def test_our_own_ask_is_never_bought_back():
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks, own_trade_ids=["0x" + TID_A.upper()]) is None


def test_the_spend_cap_excludes_a_bigger_ask():
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks, max_spend_xch_mojos=10 ** 12) is None


def test_a_partial_top_up_beats_none():
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks, want_mojos=900).trade_id == TID_A


def test_an_ask_below_the_minimum_burn_is_never_taken():
    """[review] An ask too small to be worth unwrapping is skipped, even as
    the only one: its proceeds would not pay for their own gas."""
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks, min_mojos=547).trade_id == TID_A
    assert _pick(asks, min_mojos=548) is None


def test_nothing_without_a_reference_or_a_need():
    asks = D.parse_asks(BOOK, WM)
    assert _pick(asks, reference_xch_per_unit=Decimal(0)) is None
    assert _pick(asks, want_mojos=0) is None


# -- verifying ---------------------------------------------------------------

class FakeWallet:
    def __init__(self, summary, valid=True):
        self.summary, self.valid = summary, valid

    def get_offer_summary(self, offer):
        return self.summary

    def check_offer_validity(self, offer):
        return self.valid


def _ask():
    return D.parse_asks(BOOK, WM)[0]


def test_an_offer_the_wallet_reads_as_listed_passes():
    D.verify_offer(FakeWallet({"offered": {"0x" + WM: 547},
                               "requested": {"xch": 1_827_700_000_000}}),
                   _ask(), WM)


@pytest.mark.parametrize("summary,valid", [
    ({"offered": {WM: 546}, "requested": {"xch": 1_827_700_000_000}}, True),
    ({"offered": {WM: 547}, "requested": {"xch": 1_827_700_000_001}}, True),
    ({"offered": {WM: 547, "xch": 1}, "requested": {"xch": 1_827_700_000_000}},
     True),
    ({"offered": {WM: 547}, "requested": {"xch": 1_827_700_000_000,
                                          "ab" * 32: 5}}, True),
    ({"offered": {WM: 547}, "requested": {"xch": 1_827_700_000_000}}, False),
    ({}, True),
    (None, True),
])
def test_any_disagreement_with_the_listing_refuses(summary, valid):
    with pytest.raises(D.DexieBuyError):
        D.verify_offer(FakeWallet(summary, valid), _ask(), WM)


def test_the_cost_is_price_plus_our_fee():
    assert D.buy_cost_mojos(_ask(), 100_000_000) == 1_827_800_000_000
    assert D.buy_cost_mojos(_ask(), -5) == 1_827_700_000_000
