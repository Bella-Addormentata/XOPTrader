#!/usr/bin/env python3
"""Advisory per-market offer-size recommendations keyed to market flow.

The engine's tier sizing is pool-proportional in design but minimum-clamped
in practice: per-tier allocations land below min_offer_size_units_override,
so every live fill is exactly 1.00 base unit regardless of how much flow the
market actually has.  This script recommends a tier-0 size per enabled pair
from measured market volume.  It is ADVISORY ONLY -- it reads the config,
the engine database and dexie, and changes nothing.

Sizing principles (owner-specified):
  * Target participation ~15% of market volume (--target-participation).
  * Hard cap 25-30% (--max-participation): above that we ARE the market
    and degrade our own price discovery.
  * Inventory cap in days-of-market-volume (--inventory-days, default 2.5):
    never commit a ladder we could not shed in normal flow.
  * Ladder also capped at 40% of current holdings of the locked asset
    (each side of the book locks one asset; the tighter side binds).
  * Thin / uncertain markets get smaller size via a spread haircut:
    clamp(1 - (spread_p50_bps - 300) / 2000, 0.4, 1.0).

Method, per enabled pair:
  1. market_volume_day = dexie 7d volume / 7, in OUR base units, from
     GET /v2/prices/tickers matched by CAT id.  Dexie lists XCH pairs as
     CAT_XCH (base=CAT, target=XCH), so for our XCH-base pairs the XCH-side
     volume is target_volume_7d -- the inversion is shown in the output.
  2. our_fills_day / our_volume_day from trade_log (realized_pnl_mojos IS
     NOT NULL, last 7 days).
  3. recommended_tier0 = (target * market_volume_day) / max(our_fills_day, 1)
     ASSUMPTION: fill COUNT is roughly size-inelastic for modest size
     changes near the top of book -- a taker taking 1 XCH usually takes 2
     if offered at the same price.  Recommended size scales volume through
     size-per-fill, not through more fills.
  4. Caps: participation hard cap, inventory-days cap, holdings cap; then
     the uncertainty haircut; then floored at the config minimum.
  5. Ladder total = tier0 * sum(tier_size_pct) / tier_size_pct[0]
     (per-pair override when present, else strategy defaults, else equal).

The script then INVERTS the holdings cap to answer: what balances SHOULD
we hold so that participation (the market opportunity), not our holdings,
is the binding constraint?  Per pair-side, the unconstrained flow-keyed
ladder (participation + inventory-days caps and the uncertainty haircut
still apply; the holdings cap is removed) divided by the 40% cap gives the
required locked-asset holdings -- base asset for the ask ladder, quote for
the bid (converted at current prices).  Requirements are SUMMED per asset
across pairs because all ladders post concurrently (XCH backs the ask
ladders of every XCH-base pair at the same time), which is the
conservative aggregation.  The engine fee reserve (fee_reserve_xch) is
added on top; the --liquid-floor operating reserve is checked but is
mathematically subsumed by the 40% cap (a fully funded asset is at most
40% deployed, i.e. at least 60% liquid) unless the floor exceeds 0.60.
Two answers are produced: (A) what fully funding the target costs, and
(B) the best proportional allocation of the CURRENT total, plus the
capital ceiling above which the strategy cannot deploy usefully at
current market volumes (participation beyond the hard cap stops adding
profit).

Usage:
    .venv/Scripts/python.exe scripts/recommend_offer_sizes.py
    .venv/Scripts/python.exe scripts/recommend_offer_sizes.py \
        --target-participation 0.15 --max-participation 0.30 \
        --inventory-days 2.5

Exit codes: 0 = recommendations printed, 2 = operational error (dexie
unreachable, database missing, ...).  Read-only throughout.
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

import requests
import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"
CONFIG_PATH = REPO_ROOT / "config.yaml"

DEXIE_TICKERS_URL = "https://api.dexie.space/v2/prices/tickers"

MOJOS_PER_XCH = 1_000_000_000_000
MOJOS_PER_CAT = 1_000

LOOKBACK_DAYS = 7.0
HOLDINGS_CAP_FRAC = 0.40

ASSET_NAMES: dict[str, str] = {
    "xch": "XCH",
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": "wUSDC.b",
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": "BYC",
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20": "DBX",
}


def _asset_name(asset_id: str) -> str:
    return ASSET_NAMES.get(asset_id, asset_id[:8])


def _mojos_per_unit(asset_id: str) -> int:
    return MOJOS_PER_XCH if asset_id == "xch" else MOJOS_PER_CAT


def _load_config() -> dict:
    with open(CONFIG_PATH, encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def _fetch_dexie_tickers() -> list[dict]:
    resp = requests.get(DEXIE_TICKERS_URL, timeout=30)
    resp.raise_for_status()
    payload = resp.json()
    return payload.get("tickers", [])


def _match_ticker(tickers: list[dict], base_id: str, quote_id: str) -> dict | None:
    """Find the dexie ticker for our (base, quote) pair, either orientation."""
    for t in tickers:
        b, g = t.get("base_id"), t.get("target_id")
        if {b, g} == {base_id, quote_id}:
            return t
    return None


def _market_volume_day(ticker: dict, our_base_id: str) -> tuple[float, str]:
    """7d volume / 7 in OUR base units, plus a mapping note for the output.

    Dexie reports base_volume_7d in the ticker's base currency and
    target_volume_7d in its target currency.  Our base asset may be on
    either side (dexie lists XCH pairs as CAT_XCH, so our XCH-base pairs
    are inverted relative to dexie's orientation).
    """
    if ticker.get("base_id") == our_base_id:
        vol7d = float(ticker.get("base_volume_7d") or 0.0)
        side = "base_volume_7d (same orientation)"
    else:
        vol7d = float(ticker.get("target_volume_7d") or 0.0)
        side = "target_volume_7d (INVERTED: dexie quotes this pair " \
               "the other way round)"
    note = f"dexie {ticker.get('ticker_id')} {side}"
    return vol7d / LOOKBACK_DAYS, note


def _ticker_price_in_base(ticker: dict, our_base_id: str) -> float | None:
    """last_price expressed as OUR-base units per OUR-quote unit."""
    last = float(ticker.get("last_price") or 0.0)
    if last <= 0.0:
        return None
    if ticker.get("base_id") == our_base_id:
        # dexie last_price = target per base = our-quote per our-base.
        return 1.0 / last
    # dexie last_price = target per base = our-base per our-quote.
    return last


class DbReader:
    def __init__(self, db_path: Path) -> None:
        uri = f"file:{db_path.as_posix()}?mode=ro"
        self._con = sqlite3.connect(uri, uri=True, timeout=10)
        self._con.execute("PRAGMA busy_timeout = 10000")

    def close(self) -> None:
        self._con.close()

    def fills_last_days(self, pair: str, days: float) -> tuple[int, int, int | None]:
        """(fill count, total size_mojos, modal size_mojos) for realized fills.

        trade_log.timestamp is an ISO-8601 TEXT column ('T' separator, some
        rows with a trailing 'Z'), so the cutoff is compared lexically.
        """
        cutoff = (datetime.now(timezone.utc) - timedelta(days=days)) \
            .strftime("%Y-%m-%dT%H:%M:%S")
        rows = self._con.execute(
            "SELECT size_mojos FROM trade_log "
            "WHERE pair_name = ? AND realized_pnl_mojos IS NOT NULL "
            "AND timestamp >= ?",
            (pair, cutoff),
        ).fetchall()
        sizes = [int(r[0]) for r in rows]
        modal = Counter(sizes).most_common(1)[0][0] if sizes else None
        return len(sizes), sum(sizes), modal

    def holdings_units(self) -> dict[str, float]:
        """inventory_state total_quantity (mojos) -> units, keyed by asset id."""
        rows = self._con.execute(
            "SELECT asset_id, total_quantity FROM inventory_state"
        ).fetchall()
        return {
            asset_id: qty / _mojos_per_unit(asset_id)
            for asset_id, qty in rows
        }

    def spread_p50_bps(self, pair: str, days: float) -> float | None:
        """7-day median of snapshots.spread_bps (created_at uses a space
        separator, unlike trade_log)."""
        cutoff = (datetime.now(timezone.utc) - timedelta(days=days)) \
            .strftime("%Y-%m-%d %H:%M:%S")
        vals = [
            r[0] for r in self._con.execute(
                "SELECT spread_bps FROM snapshots "
                "WHERE pair_name = ? AND created_at >= ? "
                "AND spread_bps IS NOT NULL ORDER BY spread_bps",
                (pair, cutoff),
            )
        ]
        if not vals:
            return None
        return float(vals[len(vals) // 2])


def _tier_weights(pair_cfg: dict, strategy: dict) -> list[float]:
    weights = pair_cfg.get("tier_size_pct_override") \
        or strategy.get("tier_size_pct")
    if not weights:
        num_tiers = int(strategy.get("num_tiers", 6))
        weights = [1.0 / num_tiers] * num_tiers
    return [float(w) for w in weights]


def _uncertainty_haircut(spread_p50: float | None) -> tuple[float, str]:
    if spread_p50 is None:
        return 1.0, "no snapshot spread data -- haircut skipped"
    factor = 1.0 - (spread_p50 - 300.0) / 2000.0
    factor = max(0.4, min(1.0, factor))
    return factor, f"spread p50 {spread_p50:.0f} bps -> x{factor:.2f}"


def _recommend(pair_cfg: dict, strategy: dict, db: DbReader,
               tickers: list[dict], args: argparse.Namespace) -> dict:
    name = pair_cfg["name"]
    base_id = pair_cfg["base_asset_id"]
    quote_id = pair_cfg["quote_asset_id"]
    base_name = _asset_name(base_id)
    per_unit = _mojos_per_unit(base_id)

    config_floor = float(
        pair_cfg.get("min_offer_size_units_override",
                     strategy.get("min_offer_size_units", 1.0))
    )
    weights = _tier_weights(pair_cfg, strategy)
    ladder_mult = sum(weights) / weights[0]

    fills, size_mojos, modal_mojos = db.fills_last_days(name, LOOKBACK_DAYS)
    our_fills_day = fills / LOOKBACK_DAYS
    our_volume_day = size_mojos / per_unit / LOOKBACK_DAYS
    modal_units = modal_mojos / per_unit if modal_mojos is not None else None

    spread_p50 = db.spread_p50_bps(name, LOOKBACK_DAYS)
    haircut, haircut_note = _uncertainty_haircut(spread_p50)

    result = {
        "name": name, "base_name": base_name,
        "base_id": base_id, "quote_id": quote_id,
        "our_fills_day": our_fills_day, "our_volume_day": our_volume_day,
        "modal_units": modal_units, "config_floor": config_floor,
        "ladder_mult": ladder_mult, "haircut": haircut,
        "haircut_note": haircut_note, "num_tiers": len(weights),
        "fills_div": max(our_fills_day, 1.0),
        "tighter_side": None, "holdings_bound": False, "floored": False,
    }

    ticker = _match_ticker(tickers, base_id, quote_id)
    if ticker is None:
        result.update({
            "market_volume_day": None,
            "mapping": "NO dexie ticker lists this pair "
                       f"({base_name}<->{_asset_name(quote_id)}) -- flow-keyed "
                       "sizing not computable",
            "participation": None,
            "recommended_tier0": config_floor,
            "ladder_total": config_floor * ladder_mult,
            "binding": "config floor (no market data)",
            "rationale": "no direct dexie market; keep the config minimum "
                         "and rely on spread, not size, for protection",
        })
        return result

    market_volume_day, mapping = _market_volume_day(ticker, base_id)
    participation = (our_volume_day / market_volume_day
                     if market_volume_day > 0 else None)
    result.update({"market_volume_day": market_volume_day,
                   "mapping": mapping, "participation": participation})

    if market_volume_day <= 0:
        result.update({
            "recommended_tier0": config_floor,
            "ladder_total": config_floor * ladder_mult,
            "binding": "config floor (zero market volume)",
            "rationale": "dexie reports zero 7d volume; keep the minimum",
        })
        return result

    # Step 3: size to hit target participation at the current fill count.
    fills_divisor = max(our_fills_day, 1.0)
    recommended_daily = args.target_participation * market_volume_day
    naive_tier0 = recommended_daily / fills_divisor

    # Caps.  Each is expressed as a tier-0 ceiling.
    caps: dict[str, float] = {}
    # (a) participation hard cap: fills/day * tier0 <= max_p * market vol.
    caps["participation cap"] = \
        args.max_participation * market_volume_day / fills_divisor
    # (b) inventory-days cap on the whole ladder.
    caps["inventory cap"] = \
        args.inventory_days * market_volume_day / ladder_mult
    # (c) holdings cap: each side's ladder locks one asset; take the
    # tighter side, converted at the dexie last price.  NOTE: XCH holdings
    # back every XCH-base ladder, so these per-pair caps overlap.
    holdings = db.holdings_units()
    side_caps: dict[str, float] = {}
    if base_id in holdings:
        side_caps["base"] = HOLDINGS_CAP_FRAC * holdings[base_id]
    price_in_base = _ticker_price_in_base(ticker, base_id)
    if quote_id in holdings and price_in_base is not None:
        side_caps["quote"] = \
            HOLDINGS_CAP_FRAC * holdings[quote_id] * price_in_base
    if side_caps:
        tighter = min(side_caps, key=side_caps.get)
        result["tighter_side"] = tighter
        caps["holdings cap"] = side_caps[tighter] / ladder_mult

    chain = ["target participation"]
    tier0 = naive_tier0
    for cap_name, cap_value in caps.items():
        if cap_value < tier0:
            tier0 = cap_value
            chain = [cap_name]

    # Uncertainty haircut, then the config floor.
    if haircut < 1.0:
        tier0 *= haircut
        chain.append("uncertainty haircut")
    floored = tier0 < config_floor
    if floored:
        tier0 = config_floor
        chain.append("config floor")
    binding = chain[-1]

    result.update({
        "recommended_tier0": tier0,
        "ladder_total": tier0 * ladder_mult,
        "binding": binding,
        "holdings_bound": "holdings cap" in chain,
        "floored": floored,
    })

    # One-line rationale.
    if participation is not None and modal_units:
        ratio = tier0 / modal_units
        if ratio >= 1.5:
            verdict = f"GROW ~{ratio:.1f}x"
        elif ratio > 0.67:
            verdict = "HOLD"
        else:
            verdict = f"SHRINK to ~{ratio:.1f}x"
        if floored:
            decided = (f"{' + '.join(chain[:-1])} land(s) below the "
                       f"{config_floor:g} floor")
        else:
            decided = f"{binding} decides"
        result["rationale"] = (
            f"{verdict}: at {participation:.1%} of a "
            f"{market_volume_day:.0f} {result['base_name']}/day market, "
            f"{args.target_participation:.0%} target implies "
            f"{recommended_daily:.1f}/day; {decided}"
        )
    else:
        result["rationale"] = "insufficient fill data; keep the minimum"
    return result


def _fmt_units(value: float | None, base: str) -> str:
    return "n/a" if value is None else f"{value:,.2f} {base}"


def _print_pair(r: dict, args: argparse.Namespace) -> None:
    base = r["base_name"]
    part = r["participation"]
    print(f"{r['name']}")
    print(f"  market volume/day:        {_fmt_units(r['market_volume_day'], base)}")
    print(f"  mapping:                  {r['mapping']}")
    print(f"  our volume/day:           {_fmt_units(r['our_volume_day'], base)}"
          f"  ({r['our_fills_day']:.1f} fills/day)")
    print(f"  participation now:        "
          f"{'n/a' if part is None else f'{part:.1%}'}"
          f"  (target {args.target_participation:.0%}, "
          f"cap {args.max_participation:.0%})")
    print(f"  current effective size:   {_fmt_units(r['modal_units'], base)}"
          f"  (modal fill, last 7d)")
    print(f"  recommended tier-0 size:  {_fmt_units(r['recommended_tier0'], base)}")
    print(f"  recommended ladder total: {_fmt_units(r['ladder_total'], base)}"
          f"  ({r['num_tiers']} tiers, x{r['ladder_mult']:.2f} of tier-0, "
          f"per side)")
    print(f"  uncertainty haircut:      {r['haircut_note']}")
    print(f"  binding constraint:       {r['binding']}")
    print(f"  rationale:                {r['rationale']}")
    print()


def _usd_prices(tickers: list[dict]) -> dict[str, float]:
    """USD per unit, per asset id.  wUSDC.b is the numeraire ($1.00 exactly,
    matching the accounting policy); XCH is derived from the wUSDC.b_XCH
    ticker and every other CAT crosses through its CAT_XCH ticker."""
    usdc = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
    xch_per_unit: dict[str, float] = {}
    for t in tickers:
        if t.get("target_id") == "xch" and t.get("base_id"):
            last = float(t.get("last_price") or 0.0)
            if last > 0:
                xch_per_unit[t["base_id"]] = last
    if usdc not in xch_per_unit:
        raise RuntimeError("dexie has no wUSDC.b_XCH ticker; cannot "
                           "establish a USD anchor")
    xch_usd = 1.0 / xch_per_unit[usdc]
    prices = {"xch": xch_usd, usdc: 1.0}
    for asset_id, xch_per in xch_per_unit.items():
        prices.setdefault(asset_id, xch_per * xch_usd)
    return prices


def _ideal_tier0(r: dict, target: float, args: argparse.Namespace) -> float:
    """Tier-0 size if holdings never bound: participation + inventory-days
    caps and the uncertainty haircut still apply, then the config floor."""
    mkt = r.get("market_volume_day")
    if not mkt:
        return r["config_floor"]
    tier0 = min(target * mkt / r["fills_div"],
                args.max_participation * mkt / r["fills_div"],
                args.inventory_days * mkt / r["ladder_mult"])
    tier0 *= r["haircut"]
    return max(tier0, r["config_floor"])


def _ideal_requirements(results: list[dict], usd: dict[str, float],
                        target: float, args: argparse.Namespace
                        ) -> tuple[dict[str, float], dict[str, float],
                                   dict[str, float]]:
    """(required units, deployed units, ideal ladder per pair), inverting the
    holdings cap: required = ladder / HOLDINGS_CAP_FRAC in the locked asset
    (base for the ask ladder, quote for the bid, at current USD crosses).
    Requirements are SUMMED per asset across pairs -- all ladders post
    concurrently, so the sum is the conservative/correct aggregation."""
    required: dict[str, float] = {}
    deployed: dict[str, float] = {}
    ladders: dict[str, float] = {}
    for r in results:
        base_id, quote_id = r["base_id"], r["quote_id"]
        if base_id not in usd or quote_id not in usd:
            continue
        ladder = _ideal_tier0(r, target, args) * r["ladder_mult"]
        ladders[r["name"]] = ladder
        quote_per_base = usd[base_id] / usd[quote_id]
        required[base_id] = required.get(base_id, 0.0) \
            + ladder / HOLDINGS_CAP_FRAC
        required[quote_id] = required.get(quote_id, 0.0) \
            + ladder * quote_per_base / HOLDINGS_CAP_FRAC
        deployed[base_id] = deployed.get(base_id, 0.0) + ladder
        deployed[quote_id] = deployed.get(quote_id, 0.0) \
            + ladder * quote_per_base
    return required, deployed, ladders


def _apply_reserves(required: dict[str, float], deployed: dict[str, float],
                    fee_reserve_xch: float, liquid_floor: float
                    ) -> dict[str, float]:
    """Operating reserves on top of the ladder-backing requirement.  The
    liquid floor is checked per asset but with the 40% holdings cap a fully
    funded asset is at most 40% deployed (>= 60% liquid), so the floor only
    binds above --liquid-floor 0.60."""
    ideal = dict(required)
    if liquid_floor < 1.0:
        for asset_id, dep in deployed.items():
            floor_req = dep / (1.0 - liquid_floor)
            if floor_req > ideal.get(asset_id, 0.0):
                ideal[asset_id] = floor_req
    ideal["xch"] = ideal.get("xch", 0.0) + fee_reserve_xch
    return ideal


def _unlocks_note(asset_id: str, results: list[dict]) -> str:
    parts = []
    for r in results:
        if r["base_id"] == asset_id:
            tag = " (binding now)" if (r["holdings_bound"]
                                       and r["tighter_side"] == "base") else ""
            parts.append(f"ask {r['name']}{tag}")
        elif r["quote_id"] == asset_id:
            tag = " (binding now)" if (r["holdings_bound"]
                                       and r["tighter_side"] == "quote") else ""
            parts.append(f"bid {r['name']}{tag}")
    return ", ".join(parts) if parts else "reserves only"


def _marginal_impact_per_usd(asset_id: str, results: list[dict]) -> float:
    """Extra recommended USD volume/day per extra USD of this asset, at
    current holdings.  Adding $1 to the tighter side of a holdings-bound
    pair raises that side's tier-0 cap by HOLDINGS_CAP_FRAC/ladder_mult
    dollars-of-base, which fills fills_div times a day with the haircut
    applied -- the USD terms cancel, leaving a dimensionless $/day per $."""
    impact = 0.0
    for r in results:
        if not r["holdings_bound"]:
            continue
        side_asset = r["base_id"] if r["tighter_side"] == "base" \
            else r["quote_id"]
        if side_asset != asset_id:
            continue
        impact += r["fills_div"] * r["haircut"] \
            * HOLDINGS_CAP_FRAC / r["ladder_mult"]
    return impact


def _print_ideal_balances(results: list[dict], usd: dict[str, float],
                          holdings: dict[str, float], config: dict,
                          args: argparse.Namespace) -> None:
    fee_reserve = float(config.get("strategy", {}).get("fee_reserve_xch", 0.0))

    required, deployed, ladders = _ideal_requirements(
        results, usd, args.target_participation, args)
    ideal = _apply_reserves(required, deployed, fee_reserve,
                            args.liquid_floor)

    print()
    print("Ideal balances: what we SHOULD hold so participation, not "
          "holdings, binds")
    print(f"  method: unconstrained flow-keyed ladder per side (holdings "
          f"cap removed, all")
    print(f"  other caps + haircut kept) / {HOLDINGS_CAP_FRAC:.0%} holdings "
          f"cap = required locked asset;")
    print("  SUMMED per asset across pairs (all ladders post concurrently "
          "-- conservative);")
    if args.liquid_floor <= 1.0 - HOLDINGS_CAP_FRAC:
        floor_note = (f"subsumed by the {HOLDINGS_CAP_FRAC:.0%} cap (a "
                      f"funded asset is <= {HOLDINGS_CAP_FRAC:.0%} "
                      f"deployed), binds only above "
                      f"{1.0 - HOLDINGS_CAP_FRAC:.0%}")
    else:
        floor_note = (f"BINDING (exceeds the {1.0 - HOLDINGS_CAP_FRAC:.0%} "
                      f"liquidity the {HOLDINGS_CAP_FRAC:.0%} cap implies)")
    print(f"  + fee reserve {fee_reserve:g} XCH.  Liquid floor "
          f"{args.liquid_floor:.0%}: {floor_note}.")
    print()
    ladder_bits = " | ".join(
        f"{r['name']} {ladders[r['name']]:,.2f} {r['base_name']}"
        for r in results if r["name"] in ladders)
    print(f"  ideal ladder/side: {ladder_bits}")
    print()

    asset_ids = [a for a in ASSET_NAMES if a in ideal or a in holdings]
    current_total = sum(holdings.get(a, 0.0) * usd[a]
                        for a in asset_ids if a in usd)
    ideal_total = sum(ideal.get(a, 0.0) * usd[a]
                      for a in asset_ids if a in usd)

    header = (f"  {'asset':<9} {'current':>12} {'ideal':>12} "
              f"{'delta':>12} {'delta $':>9}  unlocks")
    print(header)
    print("  " + "-" * (len(header) + 10))
    for a in asset_ids:
        cur = holdings.get(a, 0.0)
        idl = ideal.get(a, 0.0)
        delta = idl - cur
        print(f"  {_asset_name(a):<9} {cur:>12,.2f} {idl:>12,.2f} "
              f"{delta:>+12,.2f} {delta * usd[a]:>+9,.2f}  "
              f"{_unlocks_note(a, results)}")
    print(f"  {'TOTAL $':<9} {current_total:>12,.2f} {ideal_total:>12,.2f} "
          f"{ideal_total - current_total:>+12,.2f}")
    print()

    # (A) Full funding.
    gap = ideal_total - current_total
    if gap > 0:
        print(f"  A) To fully fund {args.target_participation:.0%} "
              f"participation you would add ~${gap:,.0f}")
        print(f"     (${current_total:,.0f} now -> ${ideal_total:,.0f}).")
    else:
        print(f"  A) Current capital ${current_total:,.0f} already covers "
              f"the ${ideal_total:,.0f} ideal; only the mix differs.")

    # (B) Best allocation of what exists: same math, scaled proportionally.
    print()
    scale = current_total / ideal_total if ideal_total > 0 else 0.0
    print(f"  B) Best allocation of the current ${current_total:,.0f} "
          f"(same math scaled x{scale:.3f}):")
    for a in asset_ids:
        cur = holdings.get(a, 0.0)
        alloc = ideal.get(a, 0.0) * scale
        delta = alloc - cur
        print(f"     {_asset_name(a):<9} {cur:>12,.2f} -> {alloc:>12,.2f} "
              f"({delta:>+12,.2f} units, {delta * usd[a]:>+8,.2f} $)")
    print("     caveat: proportional scaling keeps each pair's SHARE fixed; "
          "XCH/DBX's share")
    print("     is inflated by the max(fills,1) divisor on a <1 fill/day "
          "market, so treat")
    print("     its slice as an upper bound.  Where (B) disagrees with the "
          "rebalance path")
    print("     below (e.g. it sells the asset the path buys first), trust "
          "the path for the")
    print("     first dollars -- it is the measured marginal impact, not a "
          "fixed-share scale.")

    # Capital ceiling: the same derivation at the hard participation cap.
    ceil_required, ceil_deployed, _ = _ideal_requirements(
        results, usd, args.max_participation, args)
    ceil_ideal = _apply_reserves(ceil_required, ceil_deployed, fee_reserve,
                                 args.liquid_floor)
    ceiling_total = sum(ceil_ideal.get(a, 0.0) * usd[a]
                        for a in asset_ids if a in usd)
    print()
    print(f"  Capital ceiling: ~${ceiling_total:,.0f} at current market "
          f"volumes.  Participation")
    print(f"  above ~{args.max_participation:.0%} stops adding profit (we "
          f"become the market), so capital beyond")
    print("  this level cannot be deployed usefully by this strategy.")

    # Rebalance path: order buys by marginal impact per dollar today.
    print()
    print("  Rebalance path (marginal recommended USD volume/day per $1 "
          "added, at current")
    print("  holdings; only holdings-bound pair-sides count):")
    rows = []
    for a in asset_ids:
        delta = ideal.get(a, 0.0) - holdings.get(a, 0.0)
        if delta <= 0:
            continue
        rows.append((a, _marginal_impact_per_usd(a, results), delta))
    rows.sort(key=lambda t: t[1], reverse=True)
    for rank, (a, impact, delta) in enumerate(rows, 1):
        floored_pairs = [r["name"] for r in results
                         if r["floored"] and r["holdings_bound"]
                         and (r["base_id"] == a
                              if r["tighter_side"] == "base"
                              else r["quote_id"] == a)]
        note = (f" (first dollars only clear the config floor on "
                f"{', '.join(floored_pairs)})" if floored_pairs else "")
        if impact <= 0:
            note = (" (no marginal volume today -- needed at full funding "
                    "once the quote sides fund up)")
        print(f"    {rank}. {_asset_name(a):<9} ~${impact:.2f}/day per $1, "
              f"gap {delta:+,.2f} units (${delta * usd[a]:+,.2f}){note}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Advisory offer-size recommendations from market flow. "
                    "Reads config/db/dexie; changes nothing.")
    parser.add_argument("--target-participation", type=float, default=0.15,
                        help="Target share of market volume (default 0.15).")
    parser.add_argument("--max-participation", type=float, default=0.30,
                        help="Hard participation cap (default 0.30).")
    parser.add_argument("--inventory-days", type=float, default=2.5,
                        help="Ladder cap in days of market volume "
                             "(default 2.5).")
    parser.add_argument("--liquid-floor", type=float, default=0.30,
                        help="Operating reserve: fraction of each asset "
                             "kept liquid in the ideal-balance derivation "
                             "(default 0.30; subsumed by the 40%% holdings "
                             "cap unless > 0.60).")
    parser.add_argument("--db", default=str(DB_PATH),
                        help="Engine SQLite database (opened read-only).")
    args = parser.parse_args()

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found at {db_path}", file=sys.stderr)
        return 2

    try:
        config = _load_config()
    except OSError as exc:
        print(f"ERROR: cannot read {CONFIG_PATH}: {exc}", file=sys.stderr)
        return 2

    try:
        tickers = _fetch_dexie_tickers()
    except requests.RequestException as exc:
        print(f"ERROR: dexie unreachable: {exc}", file=sys.stderr)
        return 2

    strategy = config.get("strategy", {})
    pairs = [p for p in config.get("pairs", []) if p.get("enabled")]

    print("Offer-size recommendations (ADVISORY -- nothing is changed)")
    print(f"  lookback:            last {LOOKBACK_DAYS:g} days")
    print(f"  target participation {args.target_participation:.0%}, "
          f"hard cap {args.max_participation:.0%}, "
          f"inventory {args.inventory_days:g} days of market volume, "
          f"holdings cap {HOLDINGS_CAP_FRAC:.0%} of the locked asset")
    print("  ASSUMPTION: fill count is roughly size-inelastic for modest "
          "size changes near")
    print("  the top of book (a taker taking 1 XCH usually takes 2 at the "
          "same price), so")
    print("  recommendations scale size-per-fill, not fill frequency.")
    print("  NOTE: dexie volume includes our own fills; XCH holdings back "
          "every XCH-base")
    print("  ladder, so the per-pair holdings caps overlap.")
    print()

    db = DbReader(db_path)
    try:
        results = [_recommend(p, strategy, db, tickers, args) for p in pairs]
        holdings = db.holdings_units()
    finally:
        db.close()

    for r in results:
        _print_pair(r, args)

    # Compact summary.
    header = (f"{'pair':<14} {'mkt/day':>10} {'ours/day':>9} {'part.':>6} "
              f"{'now':>6} {'rec t0':>8} {'ladder':>8}  binding")
    print(header)
    print("-" * len(header))
    for r in results:
        mkt = ("n/a" if r["market_volume_day"] is None
               else f"{r['market_volume_day']:,.1f}")
        part = ("n/a" if r["participation"] is None
                else f"{r['participation']:.1%}")
        now = ("n/a" if r["modal_units"] is None
               else f"{r['modal_units']:,.2f}")
        print(f"{r['name']:<14} {mkt:>10} {r['our_volume_day']:>9,.2f} "
              f"{part:>6} {now:>6} {r['recommended_tier0']:>8,.2f} "
              f"{r['ladder_total']:>8,.2f}  {r['binding']}")

    try:
        usd = _usd_prices(tickers)
    except RuntimeError as exc:
        print(f"\nIdeal-balance section skipped: {exc}", file=sys.stderr)
        return 0
    _print_ideal_balances(results, usd, holdings, config, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
