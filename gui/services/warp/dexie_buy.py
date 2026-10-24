"""[MILLIETH-TOPUP 2026-10-24] Buy wmilliETH.b on Dexie for the ETH gas reserve.

The warp rebalancer's last ETH source. When ETH on Base is below its band and
neither milliETH on Base nor spendable wmilliETH.b on Chia can cover it, it
takes ONE Dexie ask for wmilliETH.b, paid in XCH, and the next consults
unwrap it to Base and withdraw it to ETH.

Every number that decides whether money moves is checked twice, by two
parties that do not share a failure:

* **Dexie** lists the ask. Its listing picks the candidate and nothing more:
  amounts, price and identity all come from a third-party API.
* **Our own wallet** decodes the offer file (``get_offer_summary``) and must
  report EXACTLY the amounts Dexie listed -- the maker offering only
  wmilliETH.b, asking only XCH -- and ``check_offer_validity`` must say its
  coins are still unspent. Only then is it taken.

The price ceiling comes from a third source, CoinGecko: an ask is eligible
only when its XCH per wmilliETH.b is within ``max_premium_pct`` of
ETH-in-USD / 1000 / XCH-in-USD. With no reference price, nothing is bought.
Our own resting asks (the engine quotes the same pair) are excluded by trade
id, so the rebalancer never pays a fee to buy back its own inventory.

Transport is injected (``get_json``) so every rule here is testable offline.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from typing import Any, Callable, Iterable, Optional

#: (url, params, headers) -> parsed JSON.
GetJson = Callable[[str, dict, dict], Any]

XCH_MOJOS_PER_UNIT = 10 ** 12
CAT_MOJOS_PER_UNIT = 10 ** 3
MILLIETH_PER_ETH = 1000

COINGECKO_SIMPLE_PRICE = "https://api.coingecko.com/api/v3/simple/price"

_HEX64 = re.compile(r"^[0-9a-f]{64}$")


class DexieBuyError(RuntimeError):
    """A buy was refused: no reference price, no eligible ask, or an offer
    our wallet does not read the way Dexie listed it."""


@dataclass(frozen=True)
class DexieAsk:
    """One open Dexie offer selling wmilliETH.b for XCH, in mojos."""

    dexie_id: str
    trade_id: str      # lowercase hex, no 0x: the offer's id in our wallet too
    offer: str         # the offer file (bech32m "offer1...")
    cat_mojos: int     # wmilliETH.b we would receive
    xch_mojos: int     # XCH we would pay (before our network fee)

    @property
    def xch_per_unit(self) -> Decimal:
        """XCH per wmilliETH.b (display units on both sides)."""
        return (Decimal(self.xch_mojos) / XCH_MOJOS_PER_UNIT) / (
            Decimal(self.cat_mojos) / CAT_MOJOS_PER_UNIT)


def _norm_id(value: Any) -> str:
    text = str(value or "").strip().lower()
    return text[2:] if text.startswith("0x") else text


def _exact_mojos(amount: Any, per_unit: int) -> Optional[int]:
    """Display units -> mojos, only when that is a whole positive number."""
    if isinstance(amount, bool):
        return None
    try:
        mojos = Decimal(str(amount)) * per_unit
    except (InvalidOperation, ValueError):
        return None
    if not mojos.is_finite() or mojos <= 0 or mojos != mojos.to_integral_value():
        return None
    return int(mojos)


def parse_asks(payload: Any, asset_id: str) -> list[DexieAsk]:
    """Dexie ``/offers`` JSON -> open asks for *asset_id*, cheapest first.

    Anything not exactly "offers one asset_id amount, requests one XCH
    amount, status open" is dropped -- a multi-asset or royalty-bearing
    bundle is not a price this module knows how to judge.
    """
    want = _norm_id(asset_id)
    offers = payload.get("offers") if isinstance(payload, dict) else None
    asks: list[DexieAsk] = []
    for o in offers if isinstance(offers, list) else []:
        if not isinstance(o, dict) or o.get("status") != 0:
            continue
        offered, requested = o.get("offered"), o.get("requested")
        if not (isinstance(offered, list) and len(offered) == 1
                and isinstance(requested, list) and len(requested) == 1):
            continue
        give, take = offered[0], requested[0]
        if not (isinstance(give, dict) and isinstance(take, dict)):
            continue
        if _norm_id(give.get("id")) != want or _norm_id(take.get("id")) != "xch":
            continue
        cat = _exact_mojos(give.get("amount"), CAT_MOJOS_PER_UNIT)
        xch = _exact_mojos(take.get("amount"), XCH_MOJOS_PER_UNIT)
        trade_id = _norm_id(o.get("trade_id"))
        offer = str(o.get("offer") or "")
        if cat is None or xch is None or not _HEX64.match(trade_id):
            continue
        if not offer.startswith("offer1"):
            continue
        asks.append(DexieAsk(str(o.get("id") or ""), trade_id, offer, cat, xch))
    asks.sort(key=lambda a: (a.xch_per_unit, a.xch_mojos))
    return asks


def fetch_asks(get_json: GetJson, api_base: str, asset_id: str,
               *, page_size: int = 20, max_pages: int = 5) -> list[DexieAsk]:
    """Open Dexie asks for *asset_id* priced in XCH, cheapest first.

    Reads page after page (Dexie's ``page`` is 1-based) until a short page
    ends the book or *max_pages* is reached: the cheapest asks may all be
    ones the caller excludes (our own, ones already attempted), and an
    eligible ask further down must still be seen.
    """
    url = api_base.rstrip("/") + "/offers"
    asks: list[DexieAsk] = []
    for page in range(1, int(max_pages) + 1):
        params = {"offered": _norm_id(asset_id), "requested": "xch",
                  "status": 0, "page_size": int(page_size), "page": page,
                  "sort": "price"}
        payload = get_json(url, params, {})
        asks.extend(parse_asks(payload, asset_id))
        offers = payload.get("offers") if isinstance(payload, dict) else None
        if not isinstance(offers, list) or len(offers) < int(page_size):
            break
    asks.sort(key=lambda a: (a.xch_per_unit, a.xch_mojos))
    return asks


def reference_xch_per_millieth(get_json: GetJson,
                               api_key: str = "") -> Optional[Decimal]:
    """CoinGecko's XCH per milliETH: (ETH in USD / 1000) / (XCH in USD).

    None when either price is missing or not positive -- the caller then
    buys nothing rather than buy blind.
    """
    headers = {"x-cg-demo-api-key": api_key} if api_key else {}
    data = get_json(COINGECKO_SIMPLE_PRICE,
                    {"ids": "ethereum,chia", "vs_currencies": "usd"}, headers)
    try:
        eth = Decimal(str(data["ethereum"]["usd"]))
        xch = Decimal(str(data["chia"]["usd"]))
    except (KeyError, TypeError, InvalidOperation, ValueError):
        return None
    if not (eth.is_finite() and xch.is_finite()) or eth <= 0 or xch <= 0:
        return None
    return eth / MILLIETH_PER_ETH / xch


def pick_ask(asks: Iterable[DexieAsk], *, want_mojos: int,
             reference_xch_per_unit: Decimal, max_premium_pct: float,
             max_spend_xch_mojos: int,
             own_trade_ids: Iterable[str] = (),
             min_mojos: int = 1) -> Optional[DexieAsk]:
    """The ask to take, or None.

    Eligible: not ours, at most ``max_spend_xch_mojos``, priced at most
    ``max_premium_pct`` above the reference, and offering at least
    ``min_mojos`` (the smallest burn worth making at current gas). Among
    those, the cheapest one that covers *want_mojos*; failing that, the
    cheapest one at all -- a partial top-up still buys gas, and the next
    consult can buy again.
    """
    floor = max(1, int(min_mojos))
    if not (reference_xch_per_unit > 0) or want_mojos < 1:
        return None
    ceiling = reference_xch_per_unit * (
        Decimal(1) + Decimal(str(max_premium_pct)) / Decimal(100))
    own = {_norm_id(t) for t in own_trade_ids}
    eligible = sorted(
        (a for a in asks
         if a.trade_id not in own
         and a.cat_mojos >= floor
         and a.xch_mojos <= int(max_spend_xch_mojos)
         and a.xch_per_unit <= ceiling),
        key=lambda a: (a.xch_per_unit, a.xch_mojos))
    for a in eligible:
        if a.cat_mojos >= want_mojos:
            return a
    return eligible[0] if eligible else None


def verify_offer(wallet: Any, ask: DexieAsk, asset_id: str) -> None:
    """Raise :class:`DexieBuyError` unless our wallet reads *ask* as listed.

    The maker must offer exactly ``cat_mojos`` of *asset_id* and nothing
    else, and request exactly ``xch_mojos`` of XCH and nothing else; and the
    wallet must report the offer still valid (its coins unspent).
    """
    summary = wallet.get_offer_summary(ask.offer)
    offered = summary.get("offered") if isinstance(summary, dict) else None
    requested = summary.get("requested") if isinstance(summary, dict) else None
    if not isinstance(offered, dict) or not isinstance(requested, dict):
        raise DexieBuyError("the wallet could not summarise the offer")
    offered_n = {_norm_id(k): v for k, v in offered.items()}
    requested_n = {_norm_id(k): v for k, v in requested.items()}
    if offered_n != {_norm_id(asset_id): ask.cat_mojos}:
        raise DexieBuyError(
            f"the offer gives {offered_n}, not the {ask.cat_mojos} "
            "wmilliETH.b mojos Dexie listed")
    if requested_n != {"xch": ask.xch_mojos}:
        raise DexieBuyError(
            f"the offer asks {requested_n}, not the {ask.xch_mojos} XCH "
            "mojos Dexie listed")
    if not wallet.check_offer_validity(ask.offer):
        raise DexieBuyError("the offer is no longer valid (already taken "
                            "or cancelled)")


def buy_cost_mojos(ask: DexieAsk, fee_mojos: int) -> int:
    """XCH the take spends: the ask's price plus our network fee."""
    return int(ask.xch_mojos) + max(0, int(fee_mojos))
