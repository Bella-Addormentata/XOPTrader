#!/usr/bin/env python3
"""Core computation for flow-keyed offer sizing and ideal balances.

Importable module shared by the ``recommend_offer_sizes.py`` CLI and the
GUI (Settings > Trading Pairs "Suggested (%)" column).  Everything here is
ADVISORY and READ-ONLY: config.yaml is parsed but never written, the
engine database is opened ``mode=ro``, and dexie is only GETted.

The full method is documented in ``recommend_offer_sizes.py``; in short:

  * Per enabled pair, a flow-keyed tier-0 offer size is recommended from
    dexie 7-day market volume vs our realized fills in ``trade_log``,
    subject to a participation hard cap, an inventory-days cap, a
    40%-of-holdings cap per locked side, an uncertainty haircut from the
    7-day median spread, and the config minimum-size floor.
  * The holdings cap is then INVERTED into ideal balances: the ladder each
    side would post if holdings never bound, divided by the 40% cap, gives
    the required locked-asset holdings, summed per asset across pairs
    (all ladders post concurrently).
  * ``suggested_ratio_targets`` expresses those ideal holdings as a
    per-pair base-fraction ratio target comparable to the engine's
    ``ratio_target_by_pair`` values.

No Qt imports here -- the GUI wraps these calls in its own worker thread.
"""

from __future__ import annotations

import sqlite3
import sys
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

import requests
import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"
CONFIG_PATH = REPO_ROOT / "config.yaml"

# The defaults above are for CLI use from a checkout.  The GUI loads this
# module BY PATH out of the PyInstaller bundle, where __file__ lives under
# sys._MEIPASS -- a per-launch temp directory holding the bundled scripts and
# nothing else.  REPO_ROOT is then that temp directory, so the defaults name a
# config.yaml and a database that have never existed, and every caller failed
# with "No such file or directory: ...\_MEI00004b102\config.yaml".
#
# Frozen callers therefore MUST pass explicit paths; the GUI knows them
# (ConfigService.path and EngineBridge.db_path).  Rather than let a bundled
# default produce a mystifying temp-directory error, refuse it by name.
# PyInstaller sets sys.frozen in the frozen PROCESS, however this module is
# loaded, so it answers the question directly.  An earlier version also
# matched "_MEI" anywhere in the path, which misfires on an ordinary checkout
# living under a directory that happens to contain that substring: the
# documented no-argument CLI would then refuse to run beside a config.yaml and
# database that are both right there.
_FROZEN = bool(getattr(sys, "frozen", False))


def _require_explicit(kind: str, value):
    """Return *value*, or explain why the bundled default cannot serve.

    An empty or whitespace-only value counts as ABSENT, not as a path.  Both
    call sites read ``_require_explicit(...) or DEFAULT``, so returning ""
    here fell straight through to the bundle-relative default while frozen --
    silently restoring the very failure this guard exists to prevent.
    """
    if isinstance(value, str) and not value.strip():
        value = None
    if value is not None:
        return value
    if _FROZEN:
        raise RuntimeError(
            f"{kind} path not supplied. This module was loaded from an "
            f"application bundle ({REPO_ROOT}), which contains no {kind}; the "
            "caller must pass an explicit path rather than rely on the "
            "checkout-relative default."
        )
    return None

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


@dataclass(frozen=True)
class SizingParams:
    """Owner-specified sizing principles (CLI flags mirror these fields)."""

    target_participation: float = 0.15
    max_participation: float = 0.30
    inventory_days: float = 2.5
    liquid_floor: float = 0.30


def _asset_name(asset_id: str) -> str:
    return ASSET_NAMES.get(asset_id, asset_id[:8])


def _mojos_per_unit(asset_id: str) -> int:
    return MOJOS_PER_XCH if asset_id == "xch" else MOJOS_PER_CAT


def _load_config(path: Path | str | None = None) -> dict:
    resolved = _require_explicit("config", path) or CONFIG_PATH
    with open(resolved, encoding="utf-8") as fh:
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
               tickers: list[dict], params: SizingParams) -> dict:
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
    recommended_daily = params.target_participation * market_volume_day
    naive_tier0 = recommended_daily / fills_divisor

    # Caps.  Each is expressed as a tier-0 ceiling.
    caps: dict[str, float] = {}
    # (a) participation hard cap: fills/day * tier0 <= max_p * market vol.
    caps["participation cap"] = \
        params.max_participation * market_volume_day / fills_divisor
    # (b) inventory-days cap on the whole ladder.
    caps["inventory cap"] = \
        params.inventory_days * market_volume_day / ladder_mult
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
            f"{params.target_participation:.0%} target implies "
            f"{recommended_daily:.1f}/day; {decided}"
        )
    else:
        result["rationale"] = "insufficient fill data; keep the minimum"
    return result


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


def _ideal_tier0(r: dict, target: float, params: SizingParams) -> float:
    """Tier-0 size if holdings never bound: participation + inventory-days
    caps and the uncertainty haircut still apply, then the config floor."""
    mkt = r.get("market_volume_day")
    if not mkt:
        return r["config_floor"]
    tier0 = min(target * mkt / r["fills_div"],
                params.max_participation * mkt / r["fills_div"],
                params.inventory_days * mkt / r["ladder_mult"])
    tier0 *= r["haircut"]
    return max(tier0, r["config_floor"])


def _requirements_from_ladders(results: list[dict], usd: dict[str, float],
                               ladders: dict[str, float]
                               ) -> tuple[dict[str, float], dict[str, float]]:
    """(required units, deployed units) per asset for given per-pair ladders:
    required = ladder / HOLDINGS_CAP_FRAC in the locked asset (base for the
    ask ladder, quote for the bid, at current USD crosses).  Requirements are
    SUMMED per asset across pairs -- all ladders post concurrently, so the
    sum is the conservative/correct aggregation."""
    required: dict[str, float] = {}
    deployed: dict[str, float] = {}
    for r in results:
        if r["name"] not in ladders:
            continue
        base_id, quote_id = r["base_id"], r["quote_id"]
        ladder = ladders[r["name"]]
        quote_per_base = usd[base_id] / usd[quote_id]
        required[base_id] = required.get(base_id, 0.0) \
            + ladder / HOLDINGS_CAP_FRAC
        required[quote_id] = required.get(quote_id, 0.0) \
            + ladder * quote_per_base / HOLDINGS_CAP_FRAC
        deployed[base_id] = deployed.get(base_id, 0.0) + ladder
        deployed[quote_id] = deployed.get(quote_id, 0.0) \
            + ladder * quote_per_base
    return required, deployed


def _ideal_requirements(results: list[dict], usd: dict[str, float],
                        target: float, params: SizingParams
                        ) -> tuple[dict[str, float], dict[str, float],
                                   dict[str, float]]:
    """(required units, deployed units, ideal ladder per pair), inverting
    the holdings cap (see _requirements_from_ladders for the summing)."""
    ladders: dict[str, float] = {}
    for r in results:
        if r["base_id"] not in usd or r["quote_id"] not in usd:
            continue
        ladders[r["name"]] = _ideal_tier0(r, target, params) \
            * r["ladder_mult"]
    required, deployed = _requirements_from_ladders(results, usd, ladders)
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


# ===================================================================
# Suggested per-pair ratio targets (GUI-facing)
# ===================================================================

def suggested_ratio_targets(results: list[dict], usd: dict[str, float],
                            params: SizingParams) -> dict[str, dict]:
    """Per-pair SUGGESTED ratio target = base_value / (base_value +
    quote_value) at the pair's IDEAL holdings.

    ATTRIBUTION RULE: each pair is attributed exactly the holdings the
    ladder-requirement math demands of it -- ladder / HOLDINGS_CAP_FRAC
    base units backing its ask ladder, and ladder * quote_per_base /
    HOLDINGS_CAP_FRAC quote units backing its bid ladder (the same
    per-pair-side terms `_ideal_requirements` sums into per-asset totals).
    Portfolio-level reserves (fee_reserve_xch, the liquid floor) belong to
    no single pair and are excluded.

    CONSEQUENCE: the flow-keyed inversion posts the same ladder on both
    sides of each book, so base and quote requirements carry equal USD
    value and the suggested ratio is 0.50 by construction.  It is computed,
    not hardcoded, so any future asymmetry in the math flows through.

    Returns {pair_name: {"ratio": float | None, "artifact": bool,
    "reason": str}} where "artifact" flags upper-bound figures: pairs with
    no direct dexie market (floor-sized) and pairs where the max(fills, 1)
    divisor engaged (fewer than one fill/day inflates the ideal ladder).
    """
    out: dict[str, dict] = {}
    for r in results:
        base_id, quote_id = r["base_id"], r["quote_id"]
        if base_id not in usd or quote_id not in usd:
            out[r["name"]] = {
                "ratio": None, "artifact": True,
                "reason": "no USD price cross for one leg",
            }
            continue
        ladder = _ideal_tier0(r, params.target_participation, params) \
            * r["ladder_mult"]
        base_value = ladder / HOLDINGS_CAP_FRAC * usd[base_id]
        quote_units = ladder * (usd[base_id] / usd[quote_id]) \
            / HOLDINGS_CAP_FRAC
        quote_value = quote_units * usd[quote_id]
        ratio = base_value / (base_value + quote_value)

        artifact = False
        reason = ""
        if r.get("market_volume_day") is None:
            artifact = True
            reason = ("no direct dexie market -- floor-sized ladder, "
                      "upper bound")
        elif r["our_fills_day"] < 1.0:
            artifact = True
            reason = ("max(fills,1) divisor engaged (<1 fill/day) -- "
                      "ideal ladder is an upper bound")
        out[r["name"]] = {"ratio": ratio, "artifact": artifact,
                          "reason": reason}
    return out


def compute_suggested_targets(config_path: Path | str | None = None,
                              db_path: Path | str | None = None,
                              params: SizingParams | None = None) -> dict:
    """One-call API for the GUI: load config, fetch dexie, read the DB
    (all read-only) and return the suggested per-pair ratio targets.

    Returns {"pairs": {name: {"ratio", "artifact", "reason"}},
    "params": SizingParams, "computed_at": iso-utc-str}.  Raises on
    operational failure (dexie unreachable, DB missing) -- callers fail
    soft to "n/a".
    """
    params = params or SizingParams()
    config = _load_config(config_path)
    tickers = _fetch_dexie_tickers()
    usd = _usd_prices(tickers)

    resolved_db = Path(_require_explicit("database", db_path) or DB_PATH)
    if not resolved_db.exists():
        raise FileNotFoundError(f"database not found at {resolved_db}")

    strategy = config.get("strategy", {})
    pairs = [p for p in config.get("pairs", []) if p.get("enabled")]

    db = DbReader(resolved_db)
    try:
        results = [_recommend(p, strategy, db, tickers, params)
                   for p in pairs]
    finally:
        db.close()

    return {
        "pairs": suggested_ratio_targets(results, usd, params),
        "params": params,
        "computed_at": datetime.now(timezone.utc)
        .strftime("%Y-%m-%d %H:%M:%SZ"),
    }


# ===================================================================
# Suggested per-asset portfolio allocation (GUI-facing)
# ===================================================================

def _corrected_ladder(r: dict, params: SizingParams) -> tuple[float, bool]:
    """Ideal ladder for allocation purposes, with the artifact correction.

    Pairs trading fewer than one fill/day hit the max(fills, 1) divisor,
    which keys the ladder to a fill count that is not really there and
    inflates the pair's capital share (measured on XCH/DBX: a 39.5
    XCH/side ladder for a ~26 XCH/day market).  NOTE: the market-volume
    inventory-days ceiling does NOT bind that inflated ladder (tier-0
    ceiling 6.59 vs 3.95 at the 15% target), so the honest fix applies
    the same sheddable-in-normal-flow principle to OUR targeted flow
    instead: ladder <= inventory_days * target_participation *
    market_volume_day, floored at the config-minimum ladder.

    Returns (ladder in base units, corrected?).
    """
    ladder = _ideal_tier0(r, params.target_participation, params) \
        * r["ladder_mult"]
    mkt = r.get("market_volume_day")
    if mkt and r["our_fills_day"] < 1.0:
        shed_cap = params.inventory_days * params.target_participation * mkt
        floor_ladder = r["config_floor"] * r["ladder_mult"]
        corrected_ladder = max(min(ladder, shed_cap), floor_ladder)
        if corrected_ladder < ladder:
            return corrected_ladder, True
    return ladder, False


def suggested_portfolio_allocation(config_path: Path | str | None = None,
                                   db_path: Path | str | None = None,
                                   params: SizingParams | None = None
                                   ) -> dict:
    """Suggested per-asset share of TOTAL portfolio value, summing to 100.

    This is the "best allocation of current capital" answer: the ideal
    holdings from the holdings-cap inversion (with the artifact correction
    of _corrected_ladder and the fee reserve / liquid floor applied),
    normalized to percentages -- how to SPLIT whatever total exists.
    Scaling the ideal to the current total and normalizing are the same
    operation, so the shares are exact for any capital level.

    For NEWLY ADDED capital the marginal priority differs (see
    "marginal_note"): the next deposited dollar goes to the asset whose
    holdings cap currently binds hardest, not to these shares.

    Returns {"assets": {SYMBOL: {"suggested_pct", "current_pct",
    "corrected"}}, "corrections": [str], "marginal_note": str,
    "computed_at": str}.  SYMBOL is the upper-cased display symbol
    (XCH, WUSDC.B, ...) matching the GUI allocation panel's keys.
    Raises on operational failure -- callers fail soft to "n/a".
    """
    params = params or SizingParams()
    config = _load_config(config_path)
    tickers = _fetch_dexie_tickers()
    usd = _usd_prices(tickers)

    resolved_db = Path(_require_explicit("database", db_path) or DB_PATH)
    if not resolved_db.exists():
        raise FileNotFoundError(f"database not found at {resolved_db}")

    strategy = config.get("strategy", {})
    pairs = [p for p in config.get("pairs", []) if p.get("enabled")]

    db = DbReader(resolved_db)
    try:
        results = [_recommend(p, strategy, db, tickers, params)
                   for p in pairs]
        holdings = db.holdings_units()
    finally:
        db.close()

    ladders: dict[str, float] = {}
    corrections: list[str] = []
    corrected_pairs: set[str] = set()
    for r in results:
        if r["base_id"] not in usd or r["quote_id"] not in usd:
            continue
        ladder, corrected = _corrected_ladder(r, params)
        ladders[r["name"]] = ladder
        if corrected:
            corrected_pairs.update((r["base_id"], r["quote_id"]))
            corrections.append(
                f"{r['name']}: <1 fill/day makes the flow-keyed ladder an "
                f"upper-bound artifact; capped at "
                f"{params.inventory_days:g} days of our targeted flow "
                f"({ladder:,.1f} {r['base_name']}/side instead of "
                f"{_ideal_tier0(r, params.target_participation, params) * r['ladder_mult']:,.1f})"
            )

    required, deployed = _requirements_from_ladders(results, usd, ladders)
    fee_reserve = float(strategy.get("fee_reserve_xch", 0.0))
    ideal = _apply_reserves(required, deployed, fee_reserve,
                            params.liquid_floor)

    asset_ids = [a for a in ASSET_NAMES if a in ideal or a in holdings]
    ideal_total = sum(ideal.get(a, 0.0) * usd[a]
                      for a in asset_ids if a in usd)
    current_total = sum(holdings.get(a, 0.0) * usd[a]
                        for a in asset_ids if a in usd)
    if ideal_total <= 0:
        raise RuntimeError("ideal portfolio value is zero; cannot "
                           "derive allocation shares")

    assets: dict[str, dict] = {}
    for a in asset_ids:
        if a not in usd:
            continue
        assets[_asset_name(a).upper()] = {
            "suggested_pct": ideal.get(a, 0.0) * usd[a] / ideal_total * 100.0,
            "current_pct": (holdings.get(a, 0.0) * usd[a] / current_total
                            * 100.0) if current_total > 0 else 0.0,
            "corrected": a in corrected_pairs,
        }

    # The tension, stated explicitly: marginal priority for NEW capital.
    best_asset, best_impact = None, 0.0
    for a in asset_ids:
        impact = _marginal_impact_per_usd(a, results)
        if impact > best_impact:
            best_asset, best_impact = a, impact
    if best_asset is not None:
        marginal_note = (
            f"For NEWLY ADDED capital the marginal priority differs: "
            f"{_asset_name(best_asset)} first "
            f"(~${best_impact:.2f} recommended volume/day per $1).  These "
            f"shares answer 'how to allocate what exists', not 'where the "
            f"next deposited dollar goes'."
        )
    else:
        marginal_note = (
            "No holdings-bound pair today; the marginal destination of new "
            "capital is not distinguishable from these shares."
        )

    return {
        "assets": assets,
        "corrections": corrections,
        "marginal_note": marginal_note,
        "params": params,
        "computed_at": datetime.now(timezone.utc)
        .strftime("%Y-%m-%d %H:%M:%SZ"),
    }
