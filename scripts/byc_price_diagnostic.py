#!/usr/bin/env python3
"""Re-derive the cited BYC "7-day traded VWAP", and price the depth a VWAP hides.

The figure "BYC 7-day traded VWAP 1.001" is quoted as ground truth in FIVE
places, not the three this docstring used to list.  Re-measured 2026-09-01
with ``grep -rn "1\\.001" cpp/ config.yaml | grep -i vwap`` --

  * cpp/include/xop/execution/par_anchor.hpp:59  "BYC 7-day VWAP 1.001"
  * cpp/include/xop/config.hpp:449               "(7-day VWAP 1.001)"
  * cpp/src/engine.cpp, Engine::quote_usd_factor() "traded VWAP 1.001"
  * cpp/tests/test_fair_value.cpp:83             "dexie 7-day traded VWAP 1.001"
  * config.yaml, the BYC pegged_assets block    "(7-day VWAP 1.001 vs p50 spread 1163bps)"

All five are the same July measurement, and it is produced by NO code path --
it was taken by hand and copied forward.  The two that were missing from the
old list are load-bearing in their own right: config.hpp:449 is the doc
comment on ``par_market_sigma``, the error bar that decides how much the BYC
par anchor is trusted, and Engine::quote_usd_factor() uses it as one of the three
anchors ("traded VWAP 1.001, dexie tickers 1.011, executable bids
1.000-1.014") justifying the whole peg-beats-the-book branch.  This script
re-derives the figure on demand from the engine database and from dexie, and
answers the question a VWAP cannot: what depth is actually EXECUTABLE near
par.

Everything here is READ-ONLY.  The database is opened ``mode=ro``, dexie and
CoinGecko are only GETted, and config.yaml is not read at all (the GUI owns
that file and rewrites it on save).  Nothing is written anywhere.

What it reports, per pair:

  * LOCAL maker fills (``trade_log``) -- offers WE priced.
  * LOCAL taker fills (``taker_fills``) -- offers the COUNTERPARTY priced and
    we crossed.  Reported SEPARATELY, because a statistic that pools the two
    is partly a measurement of our own quoting.
  * OWN-FILL SHARE against the dexie tape.  ~19% of the August XCH/BYC tape is
    our own fills (270 of ~1,402), and any statistic that hides that is
    misleading.  The local database ALONE cannot show it: it contains only our
    fills, so a "share" computed from it is 100% by construction.  The share
    is therefore reported only when dexie supplies the denominator.
  * The live dexie book (status=0) in BOTH orientations with a USD column, and
    cumulative COUNTERPARTY depth at 1.00 / 1.05 / 1.10 USD per BYC.  Our own
    resting offers are EXCLUDED: offer_log.offer_id is the same 0x-hex trade
    id dexie returns as ``trade_id``, so ours can be matched out by identity.
    This is live, not theoretical -- on 2026-09-01, with both BYC pairs
    disabled, four of our XCH/BYC offers were still resting on dexie while
    recorded locally as 'cancelled'.  The exclusion's residual hole is
    printed beside the depth, not buried here -- see ``_print_book_side``.
  * The dexie settled tape (status=4) -- the third-party half of the market.

Every statistic carries its SAMPLE SIZE, and anything thinner than
``SPARSE_N`` observations is flagged inline.  A VWAP over 4 trades is not the
same object as a VWAP over 2,000, and this report never presents them alike.

Every VWAP also carries its WEIGHT UNIT, and the printed label is generated
from the weights actually used -- never hard-coded.  This is not decoration.
The settled tape used to weight by BYC amount while the local blocks weighted
by base units, and both printed "base units": correct by accident on
BYC/wUSDC.b (where the base IS BYC) and a lie on XCH/BYC (where the base is
XCH).  The convention now, applied everywhere: a native price in quote-per-
base is weighted in BASE units, and a USD-per-BYC price is weighted in BYC
units -- each series weighted by the denominator of its own quote.  Native
VWAPs are therefore comparable to each other and USD VWAPs to each other; a
native VWAP and a USD VWAP are NOT the same average, and the printed unit is
how you tell.

VWAP IS NOT ROBUST TO OUTLIERS.  It is computed because it is the cited
figure, but always ALONGSIDE a median and a trimmed mean, never instead of
them: one 1.093 print moved the BYC last-trade 9.3% off a 30-day median of
1.0010.

Usage::

    python scripts/byc_price_diagnostic.py
    python scripts/byc_price_diagnostic.py --days 30 --pair XCH/BYC
    python scripts/byc_price_diagnostic.py --no-network --json

Exit status is 0 whenever the script RAN, including when the window is empty
or dexie is unreachable -- sparse data is an answer, not an error.  Non-zero
only for bad arguments (2) and an unreadable database (1).
"""

from __future__ import annotations

import argparse
import json
import sqlite3
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"

# --- asset identity ---------------------------------------------------------
XCH_ID = "xch"
BYC_ID = "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac"
USDC_ID = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"

ASSET_NAMES: dict[str, str] = {
    XCH_ID: "XCH",
    BYC_ID: "BYC",
    USDC_ID: "wUSDC.b",
}

MOJOS_PER_XCH = 1_000_000_000_000
MOJOS_PER_CAT = 1_000

# trade_log.price_mojos and taker_fills.price_mojos are FIXED-POINT prices,
# not amounts: price = price_mojos / PRICE_SCALE, in QUOTE units per BASE unit,
# whatever the two assets' own mojo scales are.  Verified against the taker_fills
# deltas, which carry the same trade twice:
#   base_delta 2_000_000_000_000 mojos XCH  = 2 XCH
#   quote_delta   -2_909          mojos BYC = 2.909 BYC   -> 1.4545 BYC/XCH
#   price_mojos 1_454_500_000_000 / 1e12    =  1.4545     -> agrees
PRICE_SCALE = 1_000_000_000_000

# --- pair orientation -------------------------------------------------------
# XCH/BYC is configured base=XCH, quote=BYC, so its price is BYC PER XCH.
# BYC/wUSDC.b is base=BYC, quote=wUSDC.b, so its price is wUSDC.b PER BYC --
# that second one is the "1.001" figure's own orientation.
PAIRS: dict[str, dict[str, str]] = {
    "XCH/BYC": {"base": XCH_ID, "quote": BYC_ID},
    "BYC/wUSDC.b": {"base": BYC_ID, "quote": USDC_ID},
}

LOOKBACK_DAYS = 7.0

# Below this many observations a location estimate is a rumour, not a
# measurement, and every printed statistic says so inline.
SPARSE_N = 30

# Symmetric trim for the robust mean, per side.
TRIM_FRAC = 0.10

# The "is BYC executable at par?" question, in USD per BYC.
USD_LADDER_LEVELS = (1.00, 1.05, 1.10)

# Rows of each book side to print before summarising.
LADDER_ROWS = 8

# --- network ----------------------------------------------------------------
# requests is NOT a core dependency (pyproject.toml lists it only under the
# optional [gui] extra), so this script uses urllib from the stdlib and stays
# runnable from a bare interpreter.
DEXIE_OFFERS_URL = "https://api.dexie.space/v1/offers"
DEXIE_TICKERS_URL = "https://api.dexie.space/v2/prices/tickers"
COINGECKO_PRICE_URL = "https://api.coingecko.com/api/v3/simple/price"
USER_AGENT = "xop-byc-price-diagnostic/1.0"
HTTP_TIMEOUT = 30.0

# dexie SORT TRAP: pass "date_completed", never "date_completed_desc".  The API
# does not reject an unrecognised sort value -- it silently ignores it and
# returns the default order, so a caller asking for the newest trades quietly
# gets arbitrary ones (measured: the first row came back dated 2026-06-24 out
# of a window whose newest trade is 2026-08-30).  "date_completed" is ALREADY
# descending.
#
# The C++ side hit this too and is FIXED: see DexieClient::get_trades() in
# cpp/src/rpc/dexie_client.cpp, which now passes "date_completed" and carries
# the live-probe table of which sort keys are honoured vs silently ignored (the
# @param sort doc on get_offers() has the full version).  Read
# that comment rather than re-deriving this; nothing here is a live defect.
#
# Scope of what was actually measured, kept narrow on purpose: that
# "date_completed_desc" is not recognised and is silently ignored, and that
# "date_completed" works and returns newest-first.  Which OTHER strings dexie
# accepts as sort values was never tested, so do not read an exhaustive list
# of valid keys out of either comment.
DEXIE_SORT_COMPLETED = "date_completed"

DEXIE_PAGE_SIZE = 100
DEXIE_MAX_PAGES = 25


# ===================================================================
# Timestamps
# ===================================================================
#
# TIMESTAMP-FORMAT TRAP, worse than scripts/offer_sizing.py:181-186 records.
# That comment says trade_log.timestamp uses a "T" separator and
# snapshots.created_at uses a SPACE.  snapshots is indeed uniformly SPACE, but
# trade_log is MIXED: 509 of its 1,839 rows use a space (every pair, roughly
# 2026-04-24 to 2026-07-31), and taker_fills.taken_at is mixed the same way
# (581 space rows, 26 "T" rows).  Some rows also carry a trailing "Z" and
# fractional seconds.
#
# That matters because ' ' (0x20) sorts BEFORE 'T' (0x54).  A lexical cutoff
# built with a "T" separator silently drops every space-format row stamped on
# the cutoff DATE -- the boundary day is quietly truncated.  So every cutoff
# comparison here normalises BOTH sides to the space form via SQL REPLACE and
# a space-separated cutoff string.  Fractional seconds and the trailing "Z"
# sort AFTER an equal-length prefix, which is the inclusive behaviour wanted.

TS_CUTOFF_FMT = "%Y-%m-%d %H:%M:%S"
DEXIE_CUTOFF_FMT = "%Y-%m-%dT%H:%M:%S"


def _now_utc() -> datetime:
    return datetime.now(timezone.utc).replace(tzinfo=None)


def _cutoff(days: float, fmt: str = TS_CUTOFF_FMT) -> str:
    return (_now_utc() - timedelta(days=days)).strftime(fmt)


def _parse_ts(text: str) -> datetime | None:
    """Parse either stored form ('T' or space, optional 'Z', optional micros)."""
    if not text:
        return None
    cleaned = text.strip().replace("T", " ").rstrip("Z").strip()
    for fmt in ("%Y-%m-%d %H:%M:%S.%f", "%Y-%m-%d %H:%M:%S"):
        try:
            return datetime.strptime(cleaned, fmt)
        except ValueError:
            continue
    return None


def _age_days(text: str | None) -> float | None:
    ts = _parse_ts(text) if text else None
    if ts is None:
        return None
    return (_now_utc() - ts).total_seconds() / 86400.0


def _age_tag(text: str | None) -> str:
    """' (N.NN d old)' suffix, loudly marked when the number is not fresh."""
    age = _age_days(text)
    if age is None:
        return ""
    stale = " -- STALE" if age > 1.0 else ""
    return f" ({age:.2f} d old{stale})"


# ===================================================================
# Statistics
# ===================================================================


def _percentile(sorted_vals: list[float], q: float) -> float | None:
    """Linear-interpolation percentile of an ALREADY SORTED list.

    scripts/offer_sizing.py takes the upper middle element (``vals[n // 2]``),
    which is fine over thousands of snapshot spreads.  At the handful of
    observations a 7-day BYC window actually produces, the choice moves the
    answer, so this interpolates and the median below is the q=0.5 case of it.
    """
    if not sorted_vals:
        return None
    if len(sorted_vals) == 1:
        return float(sorted_vals[0])
    pos = q * (len(sorted_vals) - 1)
    low = int(pos)
    high = min(low + 1, len(sorted_vals) - 1)
    frac = pos - low
    return float(sorted_vals[low]) * (1.0 - frac) + float(sorted_vals[high]) * frac


def _median(vals: list[float]) -> float | None:
    return _percentile(sorted(vals), 0.5)


def _trimmed_mean(vals: list[float], frac: float = TRIM_FRAC) -> tuple[float | None, int]:
    """Symmetric trimmed mean; returns (value, observations dropped per side)."""
    if not vals:
        return None, 0
    ordered = sorted(vals)
    k = int(len(ordered) * frac)
    if len(ordered) - 2 * k < 1:
        k = 0
    kept = ordered[k: len(ordered) - k] if k else ordered
    return sum(kept) / len(kept), k


def _vwap(prices: list[float], weights: list[float]) -> float | None:
    total = sum(weights)
    if total <= 0.0:
        return None
    return sum(p * w for p, w in zip(prices, weights, strict=True)) / total


def _stats(prices: list[float], weights: list[float], weight_unit: str) -> dict:
    """Full location/dispersion block for one sample, sample size included.

    ``weight_unit`` is the name of the quantity in ``weights`` and it is
    MANDATORY, because the VWAP label is generated from it.  The bug this
    replaces was exactly a missing one: the settled tape weighted by BYC
    amount, the local blocks weighted by base units, and the printer labelled
    both "base units" from a hard-coded string.  On BYC/wUSDC.b the base IS
    BYC so the label happened to be true; on XCH/BYC the base is XCH and the
    printed number was a BYC-weighted average wearing a base-weighted name.
    Carrying the unit with the weights makes that class of lie unrepresentable.
    """
    n = len(prices)
    if n == 0:
        return {"n": 0, "sparse": True, "weight_unit": weight_unit, "weight_total": 0.0}
    ordered = sorted(prices)
    trimmed, dropped = _trimmed_mean(prices)
    return {
        "n": n,
        "sparse": n < SPARSE_N,
        "vwap": _vwap(prices, weights),
        "weight_unit": weight_unit,
        "mean": sum(prices) / n,
        "median": _percentile(ordered, 0.5),
        "trimmed_mean": trimmed,
        "trim_dropped_per_side": dropped,
        "p10": _percentile(ordered, 0.10),
        "p25": _percentile(ordered, 0.25),
        "p75": _percentile(ordered, 0.75),
        "p90": _percentile(ordered, 0.90),
        "min": ordered[0],
        "max": ordered[-1],
        "weight_total": sum(weights),
    }


def _sparse_note(n: int) -> str:
    if n == 0:
        return "  [NO DATA in window]"
    if n < SPARSE_N:
        return f"  [SPARSE: n={n} < {SPARSE_N}; treat as anecdote, not measurement]"
    return ""


# ===================================================================
# Units and USD
# ===================================================================


def _asset_name(asset_id: str) -> str:
    return ASSET_NAMES.get(asset_id, asset_id[:8])


def _norm_offer_id(value: object) -> str | None:
    """Normalise a Chia trade id for comparison across dexie and offer_log.

    Both sides store the same 32 bytes; only the presentation can differ, so
    case and any '0x' prefix are stripped before matching.
    """
    if not value:
        return None
    text = str(value).strip().lower()
    if text.startswith("0x"):
        text = text[2:]
    return text or None


def _mojos_per_unit(asset_id: str) -> int:
    return MOJOS_PER_XCH if asset_id == XCH_ID else MOJOS_PER_CAT


def _base_name(pair: str) -> str:
    return _asset_name(PAIRS[pair]["base"])


def _quote_name(pair: str) -> str:
    return _asset_name(PAIRS[pair]["quote"])


def _byc_units(pair: str, base_units: float, native_price: float) -> float | None:
    """BYC quantity implied by a BASE quantity traded at a pair-native price.

    The USD column is denominated PER BYC, so its VWAP has to be weighted in
    BYC.  On BYC/wUSDC.b the base already is BYC; on XCH/BYC the base is XCH
    and the BYC leg is base x price (price being BYC per XCH).
    """
    spec = PAIRS[pair]
    if base_units <= 0.0 or native_price <= 0.0:
        return None
    if spec["base"] == BYC_ID:
        return base_units
    if spec["quote"] == BYC_ID:
        return base_units * native_price
    return None


def _usd_per_byc(pair: str, native_price: float, usd: dict[str, float | None]) -> float | None:
    """Convert a pair-native price (quote per base) into USD per BYC."""
    spec = PAIRS[pair]
    if native_price <= 0.0:
        return None
    if spec["base"] == BYC_ID:
        quote_usd = usd.get(spec["quote"])
        return native_price * quote_usd if quote_usd else None
    if spec["quote"] == BYC_ID:
        base_usd = usd.get(spec["base"])
        return base_usd / native_price if base_usd else None
    return None


@dataclass
class UsdContext:
    """USD anchors, each carrying the provenance of its number.

    wUSDC.b is NOT assumed to be a dollar.  It is depegged (~0.65-0.80 USD)
    since the 2026-08-25 warp.green bridge compromise, so it is priced from
    the market -- dexie's wUSDC.b_XCH ticker crossed through XCH/USD -- and
    the source string says so.  Treating it as $1 would put the entire
    BYC/wUSDC.b USD column off by ~35%.
    """

    xch_usd: float | None = None
    xch_src: str = "unavailable"
    usdc_usd: float | None = None
    usdc_src: str = "unavailable"

    def per_asset(self) -> dict[str, float | None]:
        return {XCH_ID: self.xch_usd, USDC_ID: self.usdc_usd, BYC_ID: None}


# ===================================================================
# Database (READ-ONLY)
# ===================================================================


class DbReader:
    """Read-only reader.  Opens ``mode=ro``; never writes, migrates or VACUUMs.

    The live engine (xop_trader.exe) holds this database open, so the
    connection also sets a busy timeout and touches nothing that could take a
    write lock.
    """

    def __init__(self, db_path: Path) -> None:
        uri = f"file:{db_path.as_posix()}?mode=ro"
        self._con = sqlite3.connect(uri, uri=True, timeout=10)
        self._con.execute("PRAGMA busy_timeout = 10000")
        self._con.execute("PRAGMA query_only = 1")

    def close(self) -> None:
        self._con.close()

    def maker_fills(self, pair: str, days: float) -> list[dict]:
        """Our MAKER fills: rows of trade_log, i.e. offers WE priced.

        Cutoff normalised through REPLACE -- see the timestamp trap above.
        """
        base_id = PAIRS[pair]["base"]
        per_unit = _mojos_per_unit(base_id)
        rows = self._con.execute(
            "SELECT timestamp, side, price_mojos, size_mojos FROM trade_log "
            "WHERE pair_name = ? AND REPLACE(timestamp, 'T', ' ') >= ? "
            "ORDER BY REPLACE(timestamp, 'T', ' ')",
            (pair, _cutoff(days)),
        ).fetchall()
        return [
            {
                "ts": ts,
                "side": side,
                "price": price_mojos / PRICE_SCALE,
                "base_units": size_mojos / per_unit,
            }
            for ts, side, price_mojos, size_mojos in rows
            if price_mojos > 0 and size_mojos > 0
        ]

    def taker_fills(self, pair: str, days: float) -> list[dict]:
        """Our TAKER fills: rows of taker_fills, priced by the COUNTERPARTY.

        These are offers somebody else posted and we crossed, so their prices
        measure the other side of the market, not our quoting.  Kept in a
        separate sample for exactly that reason.
        """
        base_id = PAIRS[pair]["base"]
        quote_id = PAIRS[pair]["quote"]
        per_base = _mojos_per_unit(base_id)
        per_quote = _mojos_per_unit(quote_id)
        rows = self._con.execute(
            "SELECT taken_at, we_bought_base, price_mojos, base_delta_mojos, "
            "       quote_delta_mojos, strategy "
            "FROM taker_fills "
            "WHERE pair_name = ? AND REPLACE(taken_at, 'T', ' ') >= ? "
            "ORDER BY REPLACE(taken_at, 'T', ' ')",
            (pair, _cutoff(days)),
        ).fetchall()
        out: list[dict] = []
        for ts, bought, price_mojos, base_delta, quote_delta, strategy in rows:
            if price_mojos <= 0 or base_delta == 0:
                continue
            base_units = abs(base_delta) / per_base
            quote_units = abs(quote_delta) / per_quote
            price = price_mojos / PRICE_SCALE
            # Integrity cross-check: the stored fixed-point price should equal
            # the ratio of the two recorded deltas.  Reported, not enforced.
            implied = quote_units / base_units if base_units else 0.0
            rel_err = abs(implied - price) / price if price else None
            out.append(
                {
                    "ts": ts,
                    "side": "we bought base" if bought else "we sold base",
                    "price": price,
                    "base_units": base_units,
                    "strategy": strategy,
                    "delta_rel_err": rel_err,
                }
            )
        return out

    def latest_xch_usd(self) -> tuple[float | None, str | None]:
        row = self._con.execute(
            "SELECT xch_usd_rate, created_at FROM snapshots "
            "WHERE xch_usd_rate > 0 ORDER BY id DESC LIMIT 1"
        ).fetchone()
        return (float(row[0]), row[1]) if row else (None, None)

    def latest_usdc_usd(self) -> tuple[float | None, str | None]:
        """wUSDC.b in USD from the newest RAW DEX BBO on XCH/wUSDC.b.

        [PR #134 review] This used to read snapshots.mid_price_mojos and
        report the result as "market, NOT par".  It was neither.
        mid_price_mojos is MarketDataFeed::compute_mid() -- a blended
        micro-price across DEX, CEX and AMM legs -- and on XCH/wUSDC.b the CEX
        leg is CoinGecko's NATIVE USDC price, i.e. a number pinned near $1.
        Dividing xch_usd_rate by that aggregate therefore drags the inferred
        wrapper price back toward $1 by construction, and then labels the
        circularity a market observation. That is worse than no answer: the
        whole point of this figure is to detect wUSDC.b trading OFF par.

        offer_log persists the real third-party book (book_best_bid /
        book_best_ask), so the arithmetic BBO midpoint from there is an actual
        DEX cross with no CEX leg in it. If the book is absent the honest
        answer is None -- the caller already renders that as unavailable.
        """
        row = self._con.execute(
            "SELECT book_best_bid, book_best_ask, created_at FROM offer_log "
            "WHERE pair_name = 'XCH/wUSDC.b' AND book_best_bid > 0 "
            "AND book_best_ask > 0 ORDER BY id DESC LIMIT 1"
        ).fetchone()
        if not row:
            return None, None
        mid = (row[0] + row[1]) / 2.0 / PRICE_SCALE
        if not mid > 0:
            return None, None
        # xch_usd_rate rides on snapshots, so take the newest one at or before
        # this book sample rather than silently crossing two different instants.
        rate_row = self._con.execute(
            "SELECT xch_usd_rate FROM snapshots WHERE xch_usd_rate > 0 "
            "AND created_at <= ? ORDER BY id DESC LIMIT 1",
            (row[2],),
        ).fetchone()
        if not rate_row:
            return None, None
        return float(rate_row[0]) / mid, row[2]

    def our_offer_ids(self, pair: str) -> set[str]:
        """Every offer id this engine has ever recorded for the pair, normalised.

        offer_log.offer_id is the 0x-prefixed trade id of an offer WE posted,
        and dexie returns the same value on every /v1/offers row as
        ``trade_id`` (present under ``compact=1``).  So our own resting offers
        can be dropped out of the live book by IDENTITY rather than guessed at
        from price or size.  The id-space match was verified against the live
        XCH/DBX book on 2026-09-01: pending offer_log ids appear verbatim as
        dexie ``trade_id`` on the matching side.  No count is quoted here on
        purpose -- the number of resting offers changes every few minutes, so
        a figure written into a docstring is wrong almost immediately.  Run
        this script to see the current one.

        Deliberately NOT filtered by status, and that is not caution -- it is
        the only thing that works.  MEASURED 2026-09-01: four of our XCH/BYC
        offers were resting live on dexie (status=0) while offer_log recorded
        every one of them as 'cancelled', with resolved_at and a cancel_reason
        ('ttl_expired', 'price_adverse(2.536%)', 'price_adverse(2.021%)',
        'price_adverse(31.91%)') already written days earlier.  A cancel we
        recorded is not a cancel that broadcast.  Filtering this set to
        status='pending' would have excluded ZERO of the four and left all
        four counted as counterparty depth.

        There is no false-positive cost to the wide set: trade ids are unique
        per offer, so an id we have ever written is ours no matter what the
        row now says, and a genuinely dead id simply never matches anything on
        a live book.
        """
        rows = self._con.execute(
            "SELECT DISTINCT offer_id FROM offer_log "
            "WHERE pair_name = ? AND offer_id IS NOT NULL AND offer_id != ''",
            (pair,),
        ).fetchall()
        return {oid for oid in (_norm_offer_id(r[0]) for r in rows) if oid}

    def latest_book_sample(self, pair: str) -> dict | None:
        """Newest third-party BBO sample recorded beside one of our offers."""
        row = self._con.execute(
            "SELECT created_at, book_best_bid, book_best_ask FROM offer_log "
            "WHERE pair_name = ? AND (book_best_bid > 0 OR book_best_ask > 0) "
            "ORDER BY id DESC LIMIT 1",
            (pair,),
        ).fetchone()
        if not row:
            return None
        return {
            "at": row[0],
            "best_bid": row[1] / PRICE_SCALE if row[1] else None,
            "best_ask": row[2] / PRICE_SCALE if row[2] else None,
        }


# ===================================================================
# dexie (GET only)
# ===================================================================


def _http_json(url: str) -> dict:
    """GET one JSON document.  Every URL here is built from a module-level
    https:// constant plus urlencoded parameters -- no caller-supplied scheme
    can reach urlopen, which is what S310 exists to catch."""
    request = urllib.request.Request(  # noqa: S310
        url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"}
    )
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as resp:  # noqa: S310
        return json.load(resp)


def _dexie_offers(
    offered: str,
    requested: str,
    status: int,
    sort: str | None = None,
    stop_before: str | None = None,
) -> tuple[list[dict], bool]:
    """Page through /v1/offers.  Returns (offers, hit_page_cap).

    ``compact=1`` drops the multi-kilobyte ``offer`` blob from every row and
    keeps the amounts and dates this script actually reads.
    """
    collected: list[dict] = []
    for page in range(1, DEXIE_MAX_PAGES + 1):
        params = {
            "offered": offered,
            "requested": requested,
            "status": status,
            "page": page,
            "page_size": DEXIE_PAGE_SIZE,
            "compact": 1,
        }
        if sort:
            params["sort"] = sort
        payload = _http_json(f"{DEXIE_OFFERS_URL}?{urllib.parse.urlencode(params)}")
        # [PR #134 review] Dexie wraps every response in an explicit success
        # envelope.  Reading `offers` straight off a 200 turns a `success:
        # false` payload -- a rate limit, a bad filter, a partial outage --
        # into "the book is empty", which for an operator diagnostic is the
        # worst possible failure: it reports a real, actionable market state
        # that never happened.  A failed call must reach the warning path in
        # gather(), not be laundered into data.
        if not payload.get("success", False):
            raise RuntimeError(
                f"dexie /v1/offers returned success=false for "
                f"offered={offered} requested={requested} status={status} "
                f"page={page} (keys: {sorted(payload)[:6]})"
            )
        offers = payload.get("offers") or []
        collected.extend(offers)
        if len(offers) < DEXIE_PAGE_SIZE:
            return collected, False
        if stop_before is not None:
            oldest = offers[-1].get("date_completed")
            if oldest and oldest < stop_before:
                return collected, False
    return collected, True


def _single_leg(offer: dict) -> tuple[str, float, str, float] | None:
    """(offered_id, offered_amt, requested_id, requested_amt) for 1:1 offers.

    Multi-leg offers cannot be reduced to one price and are counted, not
    guessed at.
    """
    offered = offer.get("offered") or []
    requested = offer.get("requested") or []
    if len(offered) != 1 or len(requested) != 1:
        return None
    try:
        off_amt = float(offered[0].get("amount") or 0.0)
        req_amt = float(requested[0].get("amount") or 0.0)
    except (TypeError, ValueError):
        return None
    if off_amt <= 0.0 or req_amt <= 0.0:
        return None
    return offered[0].get("id"), off_amt, requested[0].get("id"), req_amt


def _native_price(pair: str, amounts: dict[str, float]) -> float | None:
    """Pair-native price (quote units per base unit) from an offer's amounts."""
    spec = PAIRS[pair]
    base_amt = amounts.get(spec["base"])
    quote_amt = amounts.get(spec["quote"])
    if not base_amt or not quote_amt:
        return None
    return quote_amt / base_amt


def _book_side(
    pair: str,
    we_sell_byc: bool,
    usd: dict[str, float | None],
    our_ids: set[str],
) -> dict:
    """One orientation of the live BYC book, with OUR OWN offers removed.

    ``we_sell_byc``  -> offers where the counterparty OFFERS the other asset
                        and REQUESTS BYC.  They are bidding for BYC; this is
                        the depth we could sell BYC into.
    ``not we_sell_byc`` -> offers where the counterparty OFFERS BYC.  They are
                        asking; this is the depth we could buy BYC from.

    OUR OWN OFFERS ARE NOT COUNTERPARTY DEPTH.  Reading our own quotes back
    as liquidity we could trade against is a self-portrait, and this script
    exists to inform the decision to re-enable exactly these pairs.

    This is NOT hypothetical and NOT contingent on re-enabling.  Measured
    2026-09-01, when both BYC pairs were still disabled and nothing was being
    quoted (XCH/BYC has since been re-enabled, effective on the next engine
    restart): FOUR of
    our offers were still resting on the live XCH/BYC book (all four recorded
    as 'cancelled' locally -- see ``DbReader.our_offer_ids``).  Unfiltered,
    this side reported 50 counterparty offers where there were 46.  The filter
    is by identity: dexie's ``trade_id`` against offer_log.offer_id.

    The residual hole is real and is counted, not hidden: rows dexie returns
    without a ``trade_id`` cannot be checked at all, and an offer of ours that
    this database never recorded (another instance, a hand-made offer, a
    rebuilt DB) is indistinguishable from a stranger's.  Both numbers are
    returned so the printer can state them beside the depth.
    """
    spec = PAIRS[pair]
    other_id = spec["quote"] if spec["base"] == BYC_ID else spec["base"]
    offered_id, requested_id = (other_id, BYC_ID) if we_sell_byc else (BYC_ID, other_id)
    raw, capped = _dexie_offers(offered_id, requested_id, status=0)

    rows: list[dict] = []
    skipped = 0
    own_excluded = 0
    own_byc_excluded = 0.0
    untagged = 0
    for offer in raw:
        trade_id = _norm_offer_id(offer.get("trade_id"))
        if trade_id is None:
            untagged += 1
        leg = _single_leg(offer)
        if leg is None:
            skipped += 1
            continue
        off_id, off_amt, req_id, req_amt = leg
        amounts = {off_id: off_amt, req_id: req_amt}
        byc_amt = amounts.get(BYC_ID)
        other_amt = amounts.get(other_id)
        if not byc_amt or not other_amt:
            skipped += 1
            continue
        if trade_id is not None and trade_id in our_ids:
            own_excluded += 1
            own_byc_excluded += byc_amt
            continue
        other_usd = usd.get(other_id)
        rows.append(
            {
                "id": offer.get("id"),
                "trade_id": offer.get("trade_id"),
                "byc": byc_amt,
                "other": other_amt,
                "other_name": _asset_name(other_id),
                "native": _native_price(pair, amounts),
                "usd_per_byc": (other_amt * other_usd / byc_amt) if other_usd else None,
            }
        )

    # Best first, from OUR side of the trade: selling BYC we want the highest
    # USD; buying BYC we want the lowest.  With no USD anchor, fall back to the
    # BYC-per-other ratio, which orders identically within one orientation.
    def key(row: dict) -> float:
        value = row["usd_per_byc"]
        if value is None:
            value = row["other"] / row["byc"]
        return -value if we_sell_byc else value

    rows.sort(key=key)
    cumulative = 0.0
    for row in rows:
        cumulative += row["byc"]
        row["cum_byc"] = cumulative
    return {
        "offers": rows,
        "page_cap_hit": capped,
        "skipped_multileg": skipped,
        "own_offers_excluded": own_excluded,
        "own_byc_excluded": own_byc_excluded,
        "own_ids_on_record": len(our_ids),
        "rows_without_trade_id": untagged,
        "raw_offers_seen": len(raw),
    }


def _depth_at_levels(rows: list[dict], we_sell_byc: bool) -> list[dict]:
    """Cumulative BYC (and USD notional) executable at each ladder level."""
    out: list[dict] = []
    for level in USD_LADDER_LEVELS:
        byc = 0.0
        usd = 0.0
        priced = 0
        for row in rows:
            price = row["usd_per_byc"]
            if price is None:
                continue
            priced += 1
            if (we_sell_byc and price >= level) or (not we_sell_byc and price <= level):
                byc += row["byc"]
                usd += row["byc"] * price
        out.append({"level": level, "byc": byc, "usd": usd, "offers_priced": priced})
    return out


def _settled_tape(pair: str, days: float, usd: dict[str, float | None]) -> dict:
    """Third-party settled trades (status=4) for the pair, both orientations.

    WEIGHTING: the native series is weighted in BASE units and the USD series
    in BYC, matching ``_local_block`` exactly so the two samples' VWAPs are
    like-for-like.  Previously this function weighted BOTH series by the BYC
    leg while ``_local_block`` used base units, and both printed the same
    hard-coded "base units" label -- so on XCH/BYC the settled VWAP was a
    BYC-weighted number being compared against base-weighted local ones under
    a name that fitted neither.
    """
    spec = PAIRS[pair]
    other_id = spec["quote"] if spec["base"] == BYC_ID else spec["base"]
    cutoff = _cutoff(days, DEXIE_CUTOFF_FMT)

    prices: list[float] = []
    weights: list[float] = []
    usd_prices: list[float] = []
    usd_weights: list[float] = []
    newest: str | None = None
    total = 0
    skipped = 0
    capped = False

    for offered_id, requested_id in ((BYC_ID, other_id), (other_id, BYC_ID)):
        raw, hit_cap = _dexie_offers(
            offered_id,
            requested_id,
            status=4,
            sort=DEXIE_SORT_COMPLETED,
            stop_before=cutoff,
        )
        capped = capped or hit_cap
        for offer in raw:
            completed = offer.get("date_completed")
            if not completed or completed < cutoff:
                continue
            leg = _single_leg(offer)
            if leg is None:
                skipped += 1
                continue
            off_id, off_amt, req_id, req_amt = leg
            amounts = {off_id: off_amt, req_id: req_amt}
            byc_amt = amounts.get(BYC_ID)
            base_amt = amounts.get(spec["base"])
            native = _native_price(pair, amounts)
            if not byc_amt or not base_amt or not native:
                skipped += 1
                continue
            total += 1
            if newest is None or completed > newest:
                newest = completed
            prices.append(native)
            weights.append(base_amt)
            in_usd = _usd_per_byc(pair, native, usd)
            if in_usd:
                usd_prices.append(in_usd)
                usd_weights.append(byc_amt)

    return {
        "count": total,
        "skipped_multileg": skipped,
        "page_cap_hit": capped,
        "newest": newest,
        "newest_age_days": _age_days(newest),
        "native": _stats(prices, weights, f"{_base_name(pair)} (base units)"),
        "usd": _stats(usd_prices, usd_weights, "BYC"),
    }


def _fetch_usd_context(db: DbReader, use_network: bool) -> tuple[UsdContext, list[str]]:
    """XCH/USD and wUSDC.b/USD, preferring live feeds, falling back to the DB.

    ORDER MATTERS AND IS NOT ALPHABETICAL.  The dexie wUSDC.b anchor is quoted
    in XCH (the wUSDC.b_XCH ticker), so it can only be turned into dollars by
    crossing through XCH/USD -- it DEPENDS on step 1.  This function used to
    run both network fetches together and only then consult the database, so a
    CoinGecko outage left ``ctx.xch_usd`` unset at the moment the dexie branch
    tested it: dexie was up, the ticker was fetched, the guard failed, and the
    wUSDC.b anchor was silently dropped even though the local snapshots table
    held a perfectly usable XCH rate.  The DB fallback for XCH/USD therefore
    runs BEFORE the branch that divides through it.  Steps, in dependency
    order:

      1. XCH/USD from CoinGecko (live, preferred).
      2. XCH/USD from the local snapshots table  <-- must precede step 3.
      3. wUSDC.b/USD from the dexie ticker, crossed through whatever step 1
         or step 2 produced.
      4. wUSDC.b/USD from the local XCH/wUSDC.b snapshot mid.
    """
    ctx = UsdContext()
    warnings: list[str] = []

    # 1. XCH/USD, live.
    if use_network:
        try:
            params = urllib.parse.urlencode({"ids": "chia", "vs_currencies": "usd"})
            payload = _http_json(f"{COINGECKO_PRICE_URL}?{params}")
            rate = float((payload.get("chia") or {}).get("usd") or 0.0)
            if rate > 0:
                ctx.xch_usd = rate
                ctx.xch_src = "CoinGecko simple/price id=chia (live)"
        except (urllib.error.URLError, ValueError, TypeError, OSError) as exc:
            warnings.append(f"CoinGecko XCH/USD unavailable ({type(exc).__name__}: {exc})")

    # 2. XCH/USD from the DB, before anything crosses through it.
    if ctx.xch_usd is None:
        rate, at = db.latest_xch_usd()
        if rate:
            ctx.xch_usd = rate
            ctx.xch_src = f"LOCAL snapshots.xch_usd_rate @ {at}{_age_tag(at)}"

    # 3. wUSDC.b/USD from the dexie wUSDC.b_XCH ticker x XCH/USD.
    if use_network:
        try:
            tickers = _http_json(DEXIE_TICKERS_URL).get("tickers") or []
            found = False
            for ticker in tickers:
                if ticker.get("base_id") == USDC_ID and ticker.get("target_id") == XCH_ID:
                    found = True
                    xch_per_usdc = float(ticker.get("last_price") or 0.0)
                    if xch_per_usdc <= 0:
                        warnings.append(
                            "dexie wUSDC.b_XCH ticker carried no usable last_price; "
                            "wUSDC.b/USD falls back to the local snapshot cross"
                        )
                    elif not ctx.xch_usd:
                        warnings.append(
                            "dexie wUSDC.b_XCH ticker fetched but NOT converted: no "
                            "XCH/USD anchor from CoinGecko or the local snapshots "
                            "table to cross it through"
                        )
                    else:
                        ctx.usdc_usd = xch_per_usdc * ctx.xch_usd
                        ctx.usdc_src = (
                            f"dexie {ticker.get('ticker_id')} last "
                            f"{xch_per_usdc:.6g} XCH x XCH/USD (market, NOT par)"
                        )
                    break
            if not found:
                warnings.append("dexie tickers carried no wUSDC.b/XCH pair")
        except (urllib.error.URLError, ValueError, TypeError, OSError) as exc:
            warnings.append(f"dexie tickers unavailable ({type(exc).__name__}: {exc})")

    # 4. wUSDC.b/USD from the DB.
    if ctx.usdc_usd is None:
        rate, at = db.latest_usdc_usd()
        if rate:
            ctx.usdc_usd = rate
            ctx.usdc_src = (
                f"LOCAL XCH/wUSDC.b snapshot mid @ {at}{_age_tag(at)}"
                " -- market cross, NOT par"
            )
    return ctx, warnings


# ===================================================================
# Gathering
# ===================================================================


@dataclass
class Options:
    db_path: Path
    days: float
    pairs: list[str]
    use_network: bool
    as_json: bool = False


@dataclass
class PairReport:
    name: str
    data: dict = field(default_factory=dict)


def _local_block(fills: list[dict], pair: str, usd: dict[str, float | None]) -> dict:
    """Location/dispersion for one local fill sample.

    Two series, two weightings, each by the denominator of its own quote:
    the native price is quote-per-BASE so it is weighted in base units, and
    the USD price is per-BYC so it is weighted in BYC.  They coincide on
    BYC/wUSDC.b and differ on XCH/BYC; ``weight_unit`` travels with each.
    """
    prices = [f["price"] for f in fills]
    weights = [f["base_units"] for f in fills]
    usd_prices: list[float] = []
    usd_weights: list[float] = []
    for fill in fills:
        converted = _usd_per_byc(pair, fill["price"], usd)
        byc = _byc_units(pair, fill["base_units"], fill["price"])
        if converted and byc:
            usd_prices.append(converted)
            usd_weights.append(byc)
    newest = fills[-1]["ts"] if fills else None
    errs = [f["delta_rel_err"] for f in fills if f.get("delta_rel_err") is not None]
    return {
        "native": _stats(prices, weights, f"{_base_name(pair)} (base units)"),
        "usd": _stats(usd_prices, usd_weights, "BYC"),
        "newest": newest,
        "newest_age_days": _age_days(newest),
        "max_delta_rel_err": max(errs) if errs else None,
    }


def gather(opts: Options) -> dict:
    db = DbReader(opts.db_path)
    try:
        usd_ctx, warnings = _fetch_usd_context(db, opts.use_network)
        usd = usd_ctx.per_asset()

        pairs: dict[str, dict] = {}
        for pair in opts.pairs:
            maker = db.maker_fills(pair, opts.days)
            taker = db.taker_fills(pair, opts.days)
            block: dict = {
                "orientation": (
                    f"base {_asset_name(PAIRS[pair]['base'])}, "
                    f"quote {_asset_name(PAIRS[pair]['quote'])}; price is "
                    f"{_asset_name(PAIRS[pair]['quote'])} per "
                    f"{_asset_name(PAIRS[pair]['base'])}"
                ),
                "maker": _local_block(maker, pair, usd),
                "taker": _local_block(taker, pair, usd),
                "local_bbo_sample": db.latest_book_sample(pair),
            }
            block["maker"]["source"] = "trade_log -- OUR offers; WE set the price"
            block["taker"]["source"] = (
                "taker_fills -- offers the COUNTERPARTY priced, which we crossed"
            )
            block["taker"]["strategies"] = sorted({f["strategy"] for f in taker})

            if opts.use_network:
                try:
                    our_ids = db.our_offer_ids(pair)
                    sell = _book_side(pair, True, usd, our_ids)
                    buy = _book_side(pair, False, usd, our_ids)
                    sell["meaning"] = (
                        "counterparty BIDS for BYC; depth we could SELL into"
                    )
                    buy["meaning"] = "counterparty ASKS BYC; depth we could BUY from"
                    sell["depth"] = _depth_at_levels(sell["offers"], True)
                    buy["depth"] = _depth_at_levels(buy["offers"], False)
                    block["book"] = {"sell_byc": sell, "buy_byc": buy}
                except (urllib.error.URLError, ValueError, TypeError, OSError) as exc:
                    warnings.append(
                        f"{pair}: dexie live book unavailable "
                        f"({type(exc).__name__}: {exc})"
                    )
                try:
                    block["settled"] = _settled_tape(pair, opts.days, usd)
                except (urllib.error.URLError, ValueError, TypeError, OSError) as exc:
                    warnings.append(
                        f"{pair}: dexie settled tape unavailable "
                        f"({type(exc).__name__}: {exc})"
                    )

            ours = block["maker"]["native"]["n"] + block["taker"]["native"]["n"]
            settled = (block.get("settled") or {}).get("count")
            if settled:
                block["own_fill_share"] = {
                    "our_fills": ours,
                    "tape_trades": settled,
                    "share": ours / settled,
                    "basis": (
                        "our maker + taker fills over dexie status=4 completions in "
                        "the same window; every fill of ours is one completed dexie "
                        "offer (ours when we made, theirs when we took), so the "
                        "ratio is like-for-like -- but a single block sweep can "
                        "register several rows on both sides, so read it as an "
                        "estimate"
                    ),
                }
            else:
                block["own_fill_share"] = {
                    "our_fills": ours,
                    "tape_trades": None,
                    "share": None,
                    "basis": (
                        "NOT COMPUTABLE: the local database records only OUR fills, "
                        "so any share derived from it alone is 100% by construction. "
                        "A denominator needs the dexie tape."
                    ),
                }
            pairs[pair] = block

        return {
            "generated_at": _now_utc().strftime("%Y-%m-%d %H:%M:%SZ"),
            "database": str(opts.db_path),
            "read_only": True,
            "window_days": opts.days,
            "cutoff_utc": _cutoff(opts.days),
            "network": opts.use_network,
            "sparse_threshold": SPARSE_N,
            "usd_context": {
                "xch_usd": usd_ctx.xch_usd,
                "xch_source": usd_ctx.xch_src,
                "usdc_usd": usd_ctx.usdc_usd,
                "usdc_source": usd_ctx.usdc_src,
            },
            # Re-measured 2026-09-01, five sites not three:
            #   grep -rn "1\.001" cpp/ config.yaml | grep -i vwap
            "cited_at": [
                'cpp/include/xop/execution/par_anchor.hpp:59  -- "BYC 7-day VWAP 1.001"',
                'cpp/include/xop/config.hpp:449               -- "(7-day VWAP 1.001)"',
                'cpp/src/engine.cpp Engine::quote_usd_factor() -- "traded VWAP 1.001"',
                'cpp/tests/test_fair_value.cpp:83             -- "dexie 7-day traded VWAP 1.001"',
                (
                    "config.yaml BYC pegged_assets block          -- "
                    + '"(7-day VWAP 1.001 vs p50 spread 1163bps)"'
                ),
            ],
            "cited_at_note": (
                "FIVE sites, re-measured 2026-09-01. This report previously "
                "claimed three; config.hpp:449 (the par_market_sigma doc "
                "comment, which sets how far the BYC par anchor is trusted) and "
                "engine.cpp Engine::quote_usd_factor() (one of the three anchors justifying the "
                "peg-beats-the-book branch) were missing from that list. All "
                "five quote the same uncomputed July hand-measurement."
            ),
            "warnings": warnings,
            "pairs": pairs,
            "caveats": CAVEATS,
        }
    finally:
        db.close()


CAVEATS = [
    (
        "EVERY VWAP HERE STATES ITS WEIGHT UNIT, and the label is generated from "
        + "the weights, never hard-coded. Native quote-per-base prices are weighted "
        + "in BASE units; USD-per-BYC prices are weighted in BYC. Those coincide on "
        + "BYC/wUSDC.b (base IS BYC) and differ on XCH/BYC (base is XCH). Compare a "
        + "native VWAP only with another native VWAP: local maker, local taker and "
        + "the settled tape now share one weighting, so those three ARE "
        + "like-for-like. A native and a USD VWAP are different averages."
    ),
    (
        "VWAP IS NOT ROBUST TO OUTLIERS. It is reported because it is the cited "
        + "figure, always beside a median and a trimmed mean. One 1.093 print moved "
        + "the BYC last-trade 9.3% off a 30-day median of 1.0010; the median moved "
        + "by nothing."
    ),
    (
        "Maker fills are OUR OWN quotes. A VWAP over them measures where we chose "
        + "to stand, not where the market is. Taker fills are the counterparty's "
        + "prices and are the more independent local sample -- which is why the two "
        + "are never pooled here."
    ),
    (
        "Measured over the whole of August, ~19% of the XCH/BYC tape is our own "
        + "fills (270 of ~1,402). The OWN-FILL SHARE printed above is this window's "
        + "own figure, measured independently. Either way, a market statistic that "
        + "silently pools our fills with everyone else's is part self-portrait."
    ),
    (
        "BYC's par is an ASSUMPTION, not an observation. The engine's fair-value "
        + "blend feeds BYC's declared par IN, so agreement between a ~1.40 BYC/XCH "
        + "tape cluster and a 1.41 model output is corroboration, not independent "
        + "confirmation."
    ),
    (
        "wUSDC.b is itself depegged since the 2026-08-25 warp.green bridge "
        + "compromise. Every BYC/wUSDC.b price is quoted in a broken numeraire; the "
        + "USD column here crosses it through the market, never through par."
    ),
    (
        "A traded VWAP says where trades HAPPENED, not what is EXECUTABLE now. "
        + "The depth ladder is the part that answers the question actually being "
        + "asked when this number gets cited."
    ),
    (
        "The depth ladder counts COUNTERPARTY offers only. Our own resting offers "
        + "are matched out by trade id (dexie trade_id == offer_log.offer_id). This "
        + "is not a precaution for when the pairs are re-enabled: on 2026-09-01, "
        + "when both BYC pairs were still disabled and nothing was being quoted "
        + "(XCH/BYC has since been re-enabled), FOUR of our "
        + "XCH/BYC offers were still live on dexie -- every one of them recorded "
        + "locally as 'cancelled', with a resolved_at and a cancel_reason. A "
        + "recorded cancel is not a broadcast cancel, so the exclusion matches on "
        + "id regardless of status. What the match cannot cover is stated on each "
        + "book side, beside the depth itself."
    ),
]


# ===================================================================
# Text report
# ===================================================================


def _fmt(value: float | None, nd: int = 4) -> str:
    return "n/a" if value is None else f"{value:.{nd}f}"


def _print_stats(label: str, stats: dict, unit: str, indent: str = "    ") -> None:
    """Print one stats block.  The VWAP's weight unit comes from the BLOCK.

    Never pass a weighting name in here as a literal: the label is read from
    ``stats['weight_unit']``, which ``_stats`` required at construction, so a
    VWAP and its stated weighting cannot drift apart.
    """
    n = stats.get("n", 0)
    weight_unit = stats.get("weight_unit", "?")
    print(f"{indent}{label}: n = {n}{_sparse_note(n)}")
    if not n:
        return
    dropped = stats.get("trim_dropped_per_side", 0)
    trim_tag = f"(trimmed {dropped}/side)" if dropped else "(no trim at this n)"
    print(
        f"{indent}  VWAP     {_fmt(stats['vwap'])} {unit}   [n={n}] "
        f"-- weighted by {weight_unit}, NOT robust"
    )
    print(f"{indent}  median   {_fmt(stats['median'])} {unit}   [n={n}] -- robust")
    print(
        f"{indent}  trim{int(TRIM_FRAC * 100)}%  {_fmt(stats['trimmed_mean'])} {unit}"
        f"   [n={n}] {trim_tag}"
    )
    print(f"{indent}  mean     {_fmt(stats['mean'])} {unit}   [n={n}]")
    print(
        f"{indent}  p10 {_fmt(stats['p10'])}  p25 {_fmt(stats['p25'])}  "
        f"p75 {_fmt(stats['p75'])}  p90 {_fmt(stats['p90'])}   [n={n}]"
    )
    print(
        f"{indent}  min {_fmt(stats['min'])}  max {_fmt(stats['max'])}  "
        f"volume {stats['weight_total']:.4f} {weight_unit}"
    )


def _print_usd_stats(block: dict, indent: str) -> None:
    """USD block, or the ACTUAL reason there isn't one -- empty window and
    missing USD anchor are different failures and must not print alike."""
    if block["usd"]["n"]:
        _print_stats("implied USD per BYC", block["usd"], "USD", indent)
    elif not block["native"]["n"]:
        print(f"{indent}implied USD per BYC: n/a (no fills in window)")
    else:
        print(f"{indent}implied USD per BYC: n/a (no USD anchor resolved for this pair)")


def _print_local(title: str, block: dict, pair: str) -> None:
    print(f"  {title}")
    print(f"    source: {block['source']}")
    if block.get("strategies"):
        print(f"    strategies: {', '.join(block['strategies'])}")
    _print_stats("native price", block["native"], f"{_quote_name(pair)}/{_base_name(pair)}")
    _print_usd_stats(block, "    ")
    if block["newest"]:
        print(f"    newest fill: {block['newest']}{_age_tag(block['newest'])}")
    else:
        print("    newest fill: none in window")
    if block.get("max_delta_rel_err") is not None:
        print(
            f"    integrity: stored price vs recorded deltas, max rel. error "
            f"{block['max_delta_rel_err']:.2e}"
        )


def _own_exclusion_lines(side: dict) -> list[str]:
    """The own-offer exclusion, stated in full: what was removed and what the
    removal cannot see.  Printed BESIDE THE DEPTH, every run, whether or not
    anything was excluded -- a reader sizing up re-enabling these pairs needs
    to know the basis of the number in front of them, and 'zero excluded'
    because we hold nothing is a different claim from 'zero excluded' because
    nothing could be checked."""
    excluded = side.get("own_offers_excluded", 0)
    on_record = side.get("own_ids_on_record", 0)
    untagged = side.get("rows_without_trade_id", 0)
    lines: list[str] = []
    if excluded:
        lines.append(
            f"OUR OWN offers excluded from the depth above: {excluded} "
            f"({side.get('own_byc_excluded', 0.0):,.3f} BYC), matched on dexie "
            f"trade_id vs offer_log.offer_id"
        )
    elif on_record:
        lines.append(
            f"OUR OWN offers excluded: 0 -- none of the {on_record} offer ids on "
            f"record for this pair is resting on this side right now"
        )
    else:
        lines.append(
            "OUR OWN offers excluded: 0 -- this database records NO offer ids "
            "for this pair, so nothing could be matched. That is consistent "
            "with never having quoted it, but does NOT establish it: a "
            "rebuilt DB or another instance would look identical (see the "
            "LIMITATION below)"
        )
    caveat = (
        "LIMITATION: exclusion is by recorded offer id only. Any offer of ours "
        "NOT in this database (another instance, a hand-made offer, a rebuilt "
        "DB) is counted above as counterparty depth."
    )
    if untagged:
        caveat += (
            f" {untagged} dexie row(s) carried no trade_id and could not be "
            f"checked either way."
        )
    lines.append(caveat)
    return lines


def _print_book_side(side: dict, pair: str, we_sell: bool) -> None:
    rows = side["offers"]
    verb = "SELL BYC into" if we_sell else "BUY BYC from"
    print(
        f"    {verb} -- {side['meaning']}: {len(rows)} live COUNTERPARTY offers "
        f"(of {side.get('raw_offers_seen', len(rows))} dexie returned)"
    )
    if side["skipped_multileg"]:
        print(f"      ({side['skipped_multileg']} multi-leg offers skipped: unpriceable)")
    if side["page_cap_hit"]:
        print(f"      (page cap {DEXIE_MAX_PAGES} hit -- ladder truncated)")
    if not rows:
        print("      (book empty on this side after excluding our own offers)")
        for line in _own_exclusion_lines(side):
            print(f"      {line}")
        return
    quote_name = _quote_name(pair)
    base_name = _base_name(pair)
    other = rows[0]["other_name"]
    print(
        f"      {'#':>2}  {'BYC':>12}  {other:>12}  "
        f"{quote_name + '/' + base_name:>14}  {'USD/BYC':>9}  {'cum BYC':>12}"
    )
    for index, row in enumerate(rows[:LADDER_ROWS], start=1):
        print(
            f"      {index:>2}  {row['byc']:>12,.3f}  {row['other']:>12,.3f}  "
            f"{_fmt(row['native'], 6):>14}  {_fmt(row['usd_per_byc'], 4):>9}  "
            f"{row['cum_byc']:>12,.3f}"
        )
    if len(rows) > LADDER_ROWS:
        print(f"      ... {len(rows) - LADDER_ROWS} deeper offers not shown")
    for level in side["depth"]:
        relation = ">=" if we_sell else "<="
        print(
            f"      cumulative COUNTERPARTY depth {relation} {level['level']:.2f} "
            f"USD/BYC: {level['byc']:,.3f} BYC (${level['usd']:,.2f})   "
            f"[over {level['offers_priced']} USD-priced offers]"
        )
    for line in _own_exclusion_lines(side):
        print(f"      {line}")


def render(report: dict) -> None:
    print("=" * 78)
    print("BYC PRICE DIAGNOSTIC -- re-deriving the cited traded VWAP, and the depth")
    print("=" * 78)
    print(f"generated  : {report['generated_at']}")
    print(f"database   : {report['database']}  (READ-ONLY, mode=ro)")
    print(f"window     : {report['window_days']:g} days, cutoff >= {report['cutoff_utc']} UTC")
    print(f"network    : {'ENABLED' if report['network'] else 'DISABLED (--no-network)'}")
    print(f"sparse mark: any statistic with n < {report['sparse_threshold']}")
    usd_ctx = report["usd_context"]
    print(f"XCH/USD    : {_fmt(usd_ctx['xch_usd'], 4)}  [{usd_ctx['xch_source']}]")
    print(f"wUSDC.b/USD: {_fmt(usd_ctx['usdc_usd'], 4)}  [{usd_ctx['usdc_source']}]")
    print(
        f"the figure this reproduces is cited, uncomputed, at "
        f"{len(report['cited_at'])} sites:"
    )
    for site in report["cited_at"]:
        print(f"    {site}")
    print(f"    NOTE: {report['cited_at_note']}")
    for warning in report["warnings"]:
        print(f"WARNING    : {warning}")

    for pair, block in report["pairs"].items():
        print()
        print("-" * 78)
        print(f"PAIR {pair}   ({block['orientation']})")
        print("-" * 78)
        print()
        _print_local("LOCAL MAKER FILLS", block["maker"], pair)
        print()
        _print_local("LOCAL TAKER FILLS", block["taker"], pair)

        sample = block.get("local_bbo_sample")
        if sample:
            print()
            print(f"  LOCAL THIRD-PARTY BBO SAMPLE (offer_log) @ {sample['at']}")
            print(
                f"    best_bid {_fmt(sample['best_bid'], 6)}   "
                f"best_ask {_fmt(sample['best_ask'], 6)}   "
                f"({_asset_name(PAIRS[pair]['quote'])} per "
                f"{_asset_name(PAIRS[pair]['base'])})"
            )

        print()
        share = block["own_fill_share"]
        print("  OWN-FILL SHARE")
        if share["share"] is None:
            print(f"    our fills in window: {share['our_fills']}")
            print(f"    {share['basis']}")
        else:
            print(
                f"    {share['our_fills']} of {share['tape_trades']} tape trades "
                f"= {share['share']:.1%} OURS"
                f"{_sparse_note(share['tape_trades'])}"
            )
            print(f"    basis: {share['basis']}")

        book = block.get("book")
        print()
        if book:
            print("  LIVE DEXIE BOOK (status=0), both orientations")
            _print_book_side(book["sell_byc"], pair, we_sell=True)
            print()
            _print_book_side(book["buy_byc"], pair, we_sell=False)
        else:
            print("  LIVE DEXIE BOOK: not fetched (network disabled or unavailable)")

        settled = block.get("settled")
        print()
        if settled:
            print(
                f"  DEXIE SETTLED TAPE (status=4, sort={DEXIE_SORT_COMPLETED}) "
                f"-- the whole market, not just us"
            )
            if settled["skipped_multileg"]:
                print(f"    {settled['skipped_multileg']} multi-leg trades skipped")
            if settled["page_cap_hit"]:
                print(f"    page cap {DEXIE_MAX_PAGES} hit -- window may be truncated")
            _print_stats(
                "native price", settled["native"], f"{_quote_name(pair)}/{_base_name(pair)}"
            )
            _print_usd_stats(settled, "    ")
            if settled["newest"]:
                print(
                    f"    newest settled trade: {settled['newest']}"
                    f"{_age_tag(settled['newest'])}"
                )
            else:
                print("    newest settled trade: none in window")
        else:
            print("  DEXIE SETTLED TAPE: not fetched (network disabled or unavailable)")

    print()
    print("=" * 78)
    print("HOW TO READ THIS")
    print("=" * 78)
    for index, caveat in enumerate(report["caveats"], start=1):
        print(f" {index}. {caveat}")
    print()


# ===================================================================
# CLI
# ===================================================================


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Read-only BYC price diagnostic: re-derive the cited traded VWAP "
            "and report the depth actually executable near par."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Exits 0 even when the window is empty or dexie is unreachable; "
            "sparse data is an answer, not an error. Exits 1 on an unreadable "
            "database and 2 on bad arguments."
        ),
    )
    parser.add_argument(
        "--db",
        default=str(DB_PATH),
        help=f"engine database, opened READ-ONLY (default: {DB_PATH})",
    )
    parser.add_argument(
        "--days",
        type=float,
        default=LOOKBACK_DAYS,
        help=f"lookback window in days (default: {LOOKBACK_DAYS:g})",
    )
    parser.add_argument(
        "--pair",
        action="append",
        metavar="NAME",
        help=(
            "pair to report; repeatable (default: "
            + " and ".join(PAIRS)
            + "). Choices: "
            + ", ".join(PAIRS)
        ),
    )
    parser.add_argument(
        "--no-network",
        action="store_true",
        help="skip the dexie and CoinGecko calls; use local data only",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        dest="as_json",
        help="emit the machine-readable report instead of the text one",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    pairs = args.pair or list(PAIRS)
    unknown = [p for p in pairs if p not in PAIRS]
    if unknown:
        parser.error(
            f"unknown pair(s) {', '.join(unknown)}; choose from {', '.join(PAIRS)}"
        )
    if args.days <= 0:
        parser.error("--days must be positive")

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found at {db_path}", file=sys.stderr)
        return 1

    opts = Options(
        db_path=db_path,
        days=float(args.days),
        pairs=pairs,
        use_network=not args.no_network,
        as_json=args.as_json,
    )

    try:
        report = gather(opts)
    except sqlite3.Error as exc:
        print(f"ERROR: cannot read {db_path}: {exc}", file=sys.stderr)
        return 1

    if opts.as_json:
        print(json.dumps(report, indent=2, default=str))
    else:
        render(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
