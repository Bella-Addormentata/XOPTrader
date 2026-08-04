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

The computation itself lives in offer_sizing.py (importable by the GUI);
this file is the CLI presentation layer.

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
import sys
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))

from offer_sizing import (  # noqa: E402
    ASSET_NAMES,
    CONFIG_PATH,
    DB_PATH,
    HOLDINGS_CAP_FRAC,
    LOOKBACK_DAYS,
    DbReader,
    SizingParams,
    _apply_reserves,
    _asset_name,
    _fetch_dexie_tickers,
    _ideal_requirements,
    _load_config,
    _marginal_impact_per_usd,
    _recommend,
    _unlocks_note,
    _usd_prices,
)


def _fmt_units(value: float | None, base: str) -> str:
    return "n/a" if value is None else f"{value:,.2f} {base}"


def _print_pair(r: dict, params: SizingParams) -> None:
    base = r["base_name"]
    part = r["participation"]
    print(f"{r['name']}")
    print(f"  market volume/day:        {_fmt_units(r['market_volume_day'], base)}")
    print(f"  mapping:                  {r['mapping']}")
    print(f"  our volume/day:           {_fmt_units(r['our_volume_day'], base)}"
          f"  ({r['our_fills_day']:.1f} fills/day)")
    print(f"  participation now:        "
          f"{'n/a' if part is None else f'{part:.1%}'}"
          f"  (target {params.target_participation:.0%}, "
          f"cap {params.max_participation:.0%})")
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


def _print_ideal_balances(results: list[dict], usd: dict[str, float],
                          holdings: dict[str, float], config: dict,
                          params: SizingParams) -> None:
    fee_reserve = float(config.get("strategy", {}).get("fee_reserve_xch", 0.0))

    required, deployed, ladders = _ideal_requirements(
        results, usd, params.target_participation, params)
    ideal = _apply_reserves(required, deployed, fee_reserve,
                            params.liquid_floor)

    print()
    print("Ideal balances: what we SHOULD hold so participation, not "
          "holdings, binds")
    print(f"  method: unconstrained flow-keyed ladder per side (holdings "
          f"cap removed, all")
    print(f"  other caps + haircut kept) / {HOLDINGS_CAP_FRAC:.0%} holdings "
          f"cap = required locked asset;")
    print("  SUMMED per asset across pairs (all ladders post concurrently "
          "-- conservative);")
    if params.liquid_floor <= 1.0 - HOLDINGS_CAP_FRAC:
        floor_note = (f"subsumed by the {HOLDINGS_CAP_FRAC:.0%} cap (a "
                      f"funded asset is <= {HOLDINGS_CAP_FRAC:.0%} "
                      f"deployed), binds only above "
                      f"{1.0 - HOLDINGS_CAP_FRAC:.0%}")
    else:
        floor_note = (f"BINDING (exceeds the {1.0 - HOLDINGS_CAP_FRAC:.0%} "
                      f"liquidity the {HOLDINGS_CAP_FRAC:.0%} cap implies)")
    print(f"  + fee reserve {fee_reserve:g} XCH.  Liquid floor "
          f"{params.liquid_floor:.0%}: {floor_note}.")
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
        print(f"  A) To fully fund {params.target_participation:.0%} "
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
        results, usd, params.max_participation, params)
    ceil_ideal = _apply_reserves(ceil_required, ceil_deployed, fee_reserve,
                                 params.liquid_floor)
    ceiling_total = sum(ceil_ideal.get(a, 0.0) * usd[a]
                        for a in asset_ids if a in usd)
    print()
    print(f"  Capital ceiling: ~${ceiling_total:,.0f} at current market "
          f"volumes.  Participation")
    print(f"  above ~{params.max_participation:.0%} stops adding profit (we "
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
    params = SizingParams(
        target_participation=args.target_participation,
        max_participation=args.max_participation,
        inventory_days=args.inventory_days,
        liquid_floor=args.liquid_floor,
    )

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
    print(f"  target participation {params.target_participation:.0%}, "
          f"hard cap {params.max_participation:.0%}, "
          f"inventory {params.inventory_days:g} days of market volume, "
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
        results = [_recommend(p, strategy, db, tickers, params)
                   for p in pairs]
        holdings = db.holdings_units()
    finally:
        db.close()

    for r in results:
        _print_pair(r, params)

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
    _print_ideal_balances(results, usd, holdings, config, params)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
