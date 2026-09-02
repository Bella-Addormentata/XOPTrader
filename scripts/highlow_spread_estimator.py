#!/usr/bin/env python3
"""Low-frequency high/low bid-ask spread estimators, run as a cross-check.

OPERATOR DIAGNOSTIC ONLY.  Nothing in this module may ever become a live
engine input -- not a fair-value term, not a spread floor, not a gate
threshold, not a sanity bound.  The numbers it prints are produced from
QUOTE samples standing in for trade prices (see "Bar construction" below),
which is outside the regime every estimator here was derived for.  They are
one more description of a market, not measurements of a transaction cost.
If a future reader wants a spread number for the engine, take it from the
book or from realized fills; do not import this file.

WHY IT EXISTS
-------------
The XCH/BYC book currently reports a spread of ~10,769 bps (best bid 1.5000
BYC/XCH, best ask 4.9995, mid 3.24975).  Corwin-Schultz and Abdi-Ranaldo
estimate an EFFECTIVE spread from daily high/low (and close) alone -- from
the range the price actually travelled, not from the quotes standing on the
book.  Feeding them the same market returns a number one to two orders of
magnitude smaller.

WHAT THAT IS, AND WHAT IT IS NOT.  It is a DESCRIPTIVE DISCREPANCY: the
posted book spread and the estimator output are different quantities
computed different ways, and a large gap between them is worth an operator's
attention.  It is NOT proof that a side of the book is absent, and this tool
no longer says that it is.  Neither estimator identifies SIDES at all --
both consume a daily high, a daily low and a close, and there is no step in
either derivation at which a bid can be told from an ask.  Nor can an
average over a month of day pairs distinguish a genuine spread that widened
yesterday from a quote that went missing yesterday.

The absent-side conclusion for XCH/BYC is separately and far better
supported by direct observation of the book -- bids at 1.5000, within 6.4%
of the 1.41022765 anchor, against asks at 4.9995-10.0, which is 3.5x-7.1x
it -- and by the engine's per-side anchor test,
cpp/include/xop/execution/book_side_quality.hpp, which scores each side
against an INDEPENDENT anchor carrying no book input.  That test owns the
per-side verdict.  It needs no help from the estimators here, and a
diagnostic built on quote samples must not pre-empt it.

Bar construction
----------------
Neither paper's input exists in this database: there is no continuous
trade-price series with daily highs and lows.  What exists is a sampled
third-party BBO series, and only ONE table actually persists it:
offer_log.book_best_bid / book_best_ask.

`snapshots` does NOT.  An earlier revision of this file claimed that
snapshots.mid_price_mojos + spread_bps "reconstruct the same bid and ask
exactly", citing mid 3.24975 with spread 10768.52 bps giving back 1.5000 /
4.9995.  That holds only because that particular book's published mid
happened to be the arithmetic midpoint.  mid_price_mojos is a depth-weighted
orderbook micro-price blended across DEX, CEX and AMM legs, while spread_bps
is computed separately from dex_best_bid/ask -- so centring the one on the
other fabricates two shifted sides rather than recovering the real ones.  On
the only enabled pair the micro-price left the book on 46.9% of ingests, which
put both reconstructed sides a full half-spread below the truth.  That source
is now refused rather than silently wrong.

Two bar constructions are offered, and BOTH are declared in the output because
the choice changes the answer:

  quote-touch (default)
      H_t = max(best_ask) over the day, L_t = min(best_bid) over the day,
      C_t = last mid.  Assumes a trade occurred at every quote that was ever
      touched.  This is a SENSITIVITY CONSTRUCTION -- one choice of bar
      inputs among several.  It does give the widest daily RANGE these
      quotes can justify, but a wider range does NOT give a larger estimate:
      see "Not monotonic in the sampled range" below.

  mid
      H_t = max(mid), L_t = min(mid), C_t = last mid.  Strips the bid-ask
      bounce that both estimators exist to extract, so it is biased hard
      toward zero.  A second sensitivity choice, not a bracket endpoint.

Not monotonic in the sampled range
----------------------------------
AN EARLIER VERSION OF THIS FILE CLAIMED THAT quote-touch, BEING THE WIDEST
RANGE, WAS AN UPPER BOUND ON THE ESTIMATE, AND THAT THE ARGUMENT THEREFORE
HELD "A FORTIORI".  THAT CLAIM IS FALSE.  IT IS RETRACTED, NOT HEDGED.  The
worked numbers are written out here so nobody reintroduces it.

Corwin-Schultz SUBTRACTS the two-day-range term:

    beta  = [ln(H1/L1)]^2 + [ln(H2/L2)]^2         (the two single days)
    gamma = [ln(max(H1,H2)/min(L1,L2))]^2         (the TWO-DAY range)
    alpha = (sqrt(2*beta) - sqrt(beta))/(3-2*sqrt2)
            - sqrt(gamma/(3-2*sqrt2))
    S     = 2(e^alpha - 1)/(1 + e^alpha)

Widening a high or a low raises beta -- and it raises gamma too, and gamma
enters with a MINUS sign.  Widening can therefore LOWER the estimate, or
push it negative so it floors to zero.  Baseline H1=1.010 L1=0.990
H2=1.012 L2=0.988, moving only day 1's high UPWARD and changing nothing
else:

    baseline (H1 = 1.010)      174.8 bps
    H1 = 1.020 (wider)         155.2 bps   LOWER
    H1 = 1.050 (wider)          64.8 bps   LOWER
    H1 = 1.100 (wider)          16.3 bps   LOWER
    H1 = 1.300 (wider)         -23.3 bps   LOWER (floors to zero)

Abdi-Ranaldo is likewise nonlinear in the shifted log mid-ranges it
multiplies, (c_t - eta_t)(c_t - eta_t1), and carries no monotonicity
guarantee either.

CONSEQUENCE, and it binds every line of output this file prints: the
estimate one bar construction yields can be either HIGHER or LOWER than the
estimate another yields, and no run of this tool bounds the estimate from
above.  Wording of the form "upper bound", "ceiling argument", "a fortiori",
"conservative direction", "at most" or "understates" is invalid here.  Do
not reintroduce it.

Two raw-spread figures, and they are NOT interchangeable
--------------------------------------------------------
The report prints two raw book spread columns because there are two
different questions and one number cannot answer both:

  obs med   the median, across the window, of each day's own median raw book
            spread.  A robust WINDOW-LEVEL summary.  It is not the book now.

  obs last  the SINGLE most recent quote sample's raw book spread.  One
            observation, no averaging.  This is the CURRENT posted book.

Only "obs last" can answer "is this book dislocated RIGHT NOW", so the
dislocation flag and the DISCREPANCY block both read it.  A median does
not move until the new state dominates half the day's samples, so it lags a
fresh dislocation in exactly the direction that hides one.  An earlier
version of this tool computed the latest bar's MEDIAN, labelled it "obs now"
and quoted it to the operator as the current posted book spread; that is the
bug this split exists to prevent, and the reason the column headers and the
legend below spell out which is which.

Method
------
  * Per pair, daily bars over --days, from --source (offer_log or snapshots).
  * Corwin-Schultz over every pair of ADJACENT calendar days (the estimator
    is defined on consecutive days; day pairs straddling a gap in our
    sampling are counted and skipped, never silently joined).
  * Abdi-Ranaldo over the same adjacent day pairs.
  * The MOST RECENT adjacent day pair is also reported on its own, because
    the headline figures average the whole lookback while the posted book
    spread is a single instant, and a ratio between those two is not
    like-for-like.  See "Comparing windows" below.
  * A freshness gate, a degeneracy gate and a SAMPLE gate, any of which
    REFUSES a pair outright rather than printing a plausible-looking number.
    The freshness gate ages the newest bar that actually CONTRIBUTES to an
    estimate, not the newest bar seen -- a fresh but degenerate or too-thin
    bar must not vouch for a month-old estimate.  Both ages are carried, in
    separately named fields, so they cannot be read for one another.

Comparing windows
-----------------
The report prints a DISCREPANCY block, not a verdict.  Its two operands used
to cover wildly different windows: "obs last" is ONE quote sample at ONE
instant, while cs_bps / ar_bps average every adjacent day pair in the
lookback -- up to a month of them.  A month-long average cannot tell a
newly widened genuine spread from a side that went missing this morning, so
a ratio between the two supports no conclusion about the current book.

The block therefore compares the instant against the estimate over the
SINGLE MOST RECENT day pair, which is the closest to like-for-like this data
allows, and prints the residual mismatch explicitly: the instant's
timestamp, the two days the comparator spans, and the span the headline
figures average over.  Two days against one instant is still not a match.
It is stated rather than hidden.

SAMPLE SIZE
-----------
Both estimators are TWO-DAY estimators.  Every figure they return is an
average over ADJACENT DAY PAIRS, so the day-pair count -- not the bar count
-- is the sample size, and it is the number quoted beside every estimate in
the output.  Usable bars and usable day pairs are not the same quantity:
bars survive the gap-skipping step that day pairs do not, so a handful of
bars scattered around a sampling gap can leave one or two adjacent pairs
behind.  MIN_USABLE_DAY_PAIRS gates that directly.

Usage:
    .venv/Scripts/python.exe scripts/highlow_spread_estimator.py
    .venv/Scripts/python.exe scripts/highlow_spread_estimator.py \
        --pair XCH/BYC --days 30 --source offer_log
    .venv/Scripts/python.exe scripts/highlow_spread_estimator.py --dexie
    .venv/Scripts/python.exe scripts/highlow_spread_estimator.py --json

Exit codes: 0 = report printed (a REFUSED pair is a printed result, not an
error), 2 = operational error (database missing, dexie unreachable, no such
pair).  Read-only throughout: the database is opened mode=ro and dexie is
only GETted.
"""

from __future__ import annotations

import argparse
import json
import math
import sqlite3
import sys
import urllib.error
import urllib.request
from collections import defaultdict
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"

DEXIE_MARKETS_URL = "https://api.dexie.space/v1/markets"

# offer_log.book_best_bid / book_best_ask and snapshots.mid_price_mojos are
# fixed-point PRICES (quote per base), not asset quantities, so the scale is
# a single constant independent of the assets' own decimals.  Verified
# against the live book: book_best_bid 1500000000000 -> 1.5000 BYC/XCH.
PRICE_SCALE = 1_000_000_000_000

DEFAULT_LOOKBACK_DAYS = 30.0

# --- Gate constants -------------------------------------------------------
# Ardia, Guidotti and Kroencke (2024), "Efficient estimation of bid-ask
# spreads from open, high, low, and close prices", JFE 161, 103916, show
# that Roll (1984), Corwin-Schultz (2012) and Abdi-Ranaldo (2017) are all
# DOWNWARD BIASED when trading is infrequent, and require a bar frequency
# that delivers at least two trades per bar.  We have no trade series here,
# only quote samples, so the requirement is applied to samples -- a strictly
# WEAKER test than the paper's, since a quote sample is not a trade.  A bar
# that clears this gate has still not been shown to clear theirs.
MIN_SAMPLES_PER_BAR = 2

# Minimum non-degenerate bars before either estimator is reported at all.
#
# DERIVED, not chosen: MIN_USABLE_DAY_PAIRS + 1 (see below).  N usable bars
# yield at most N-1 adjacent day pairs, so any value above that would be
# dominated -- the day-pair gate would refuse everything this one admitted,
# and the operator would read a "need 4 bars" message while the tool was
# really enforcing five consecutive ones.  Defined after MIN_USABLE_DAY_PAIRS
# so the two cannot drift apart; it is a cheap early-out, and the day-pair
# gate is the real sample-adequacy test.

# Minimum ADJACENT DAY PAIRS before either estimator is reported at all.
#
# MIN_USABLE_BARS above gates BARS, which is NOT the sample size here.  Both
# estimators are two-day estimators: Corwin-Schultz consumes a pair of
# consecutive days and Abdi-Ranaldo's moment condition spans the same two,
# so every reported figure is an average over adjacent day pairs.  Bars and
# day pairs come apart because the estimation loop skips any pair straddling
# a gap in our sampling: four usable bars sitting either side of a gap can
# yield as few as one adjacent pair, and before this gate existed the tool
# would print headline Corwin-Schultz and Abdi-Ranaldo figures AND a full
# DISCREPANCY block off that single observation.  The only guard was
# "cs_values is non-empty", which refuses zero pairs and accepts one.
#
# Ardia, Guidotti and Kroencke (2024), "Efficient estimation of bid-ask
# spreads from open, high, low, and close prices", JFE 161, 103916, is the
# authority for gating on sample adequacy: their result is that these
# estimators' bias and dispersion are governed by how much usable data each
# estimate averages over, which is exactly the quantity a bar count
# misreports here.
#
# Held at the same 4 as MIN_USABLE_BARS.  Note this is the STRICTER of the
# two: four adjacent day pairs require five CONSECUTIVE usable bars, not
# merely five usable bars.  It is a floor and not a sufficiency claim --
# four day pairs remains a tiny sample, which is why the count is printed
# beside every estimate rather than only checked.
MIN_USABLE_DAY_PAIRS = 4

# See the derivation note above.
MIN_USABLE_BARS = MIN_USABLE_DAY_PAIRS + 1

# The newest USABLE bar must be no older than this or the whole pair is
# refused.  USABLE, not merely newest seen: the bar this ages has to be one
# that actually feeds an estimator, or a fresh degenerate/thin bar certifies
# a month-old estimate as current.  BYC pairs went enabled:false on
# 2026-08-31 and BYC/wUSDC.b last printed on 2026-08-24, so this gate
# genuinely fires in production.
MAX_BAR_AGE_DAYS = 2.0

# A raw book spread at or above this is treated as a DISLOCATION flag, not a
# spread: no two-sided market quotes 20% and expects a fill.  Used only to
# label the output, never to filter data.
DISLOCATION_SPREAD_BPS = 2000.0

# Corwin-Schultz constant 3 - 2*sqrt(2) ~ 0.171573.
CS_K = 3.0 - 2.0 * math.sqrt(2.0)

BPS = 10_000.0

# S = 2(e^a - 1)/(1 + e^a) -> 2.0 as alpha -> +inf, so Corwin-Schultz cannot
# print above 20,000 bps whatever the inputs.  A CS figure well up this range
# is the estimator saturating, not a measured width -- and the discrepancy
# block divides by it, so it says so when the comparator gets there.
CS_SATURATION_BPS = 2.0 * BPS
CS_SATURATION_WARN_FRACTION = 0.5

SOURCES = ("offer_log", "snapshots")
BAR_MODES = ("quote-touch", "mid")

# Mirrors scripts/offer_sizing.py ASSET_NAMES, inverted: dexie keys markets
# by CAT asset id, our pair names are symbols.
ASSET_IDS: dict[str, str] = {
    "XCH": "xch",
    "WUSDC.B": "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d",
    "BYC": "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac",
    "DBX": "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20",
}

ONE_SIDED_NOTE = (
    "ONE-SIDED BOOK: when one side of the book is absent, the surviving side "
    "supplies BOTH the daily high and the daily low. A high/low estimator "
    "cannot see a spread that no counterparty ever crossed, so any number it "
    "returns for such a market is NOT a usable transaction cost -- do not "
    "quote it as one. It is also NOT a bound in either direction on what the "
    "traded range could justify: Corwin-Schultz SUBTRACTS the two-day range "
    "term, so widening the sampled highs and lows can LOWER the estimate or "
    "floor it to zero (worked numbers in the module docstring), and "
    "Abdi-Ranaldo carries no monotonicity guarantee either. The bar "
    "construction is a SENSITIVITY choice, not a ceiling. And neither "
    "estimator identifies SIDES, so nothing printed below can tell you which "
    "side is missing; that question belongs to the engine's per-side anchor "
    "test, cpp/include/xop/execution/book_side_quality.hpp."
)


NOT_MONOTONIC_NOTE = (
    "NOT AN UPPER BOUND, IN ANY BAR MODE. The bar construction chosen with "
    "--bars is a SENSITIVITY CHOICE, not a ceiling: these estimators are NOT "
    "monotonic in the sampled range. Corwin-Schultz SUBTRACTS the two-day "
    "range term gamma, so widening a high or a low raises gamma as well as "
    "beta and can LOWER the output or drive it negative -- from a baseline "
    "H1=1.010 L1=0.990 H2=1.012 L2=0.988 at 174.8 bps, raising ONLY H1 gives "
    "155.2 bps at 1.020, 64.8 at 1.050, 16.3 at 1.100 and -23.3 at 1.300, "
    "which floors to zero. Abdi-Ranaldo carries no monotonicity guarantee "
    "either. An earlier version of this tool claimed quote-touch bars gave "
    "an upper bound and that its argument therefore held a fortiori; that "
    "claim is RETRACTED. Another bar choice can give a HIGHER number than "
    "the one printed below, or a lower one."
)


def _median(values: list[float]) -> float | None:
    """Median of *values*, or None when empty.

    Same convention as scripts/offer_sizing.py: sort, take the middle
    element (upper median on even counts), no interpolation.
    """
    if not values:
        return None
    ordered = sorted(values)
    return float(ordered[len(ordered) // 2])


@dataclass
class Bar:
    """One daily bar built from sampled quotes.

    TWO RAW-SPREAD FIGURES LIVE HERE and conflating them is the bug this
    docstring exists to prevent.

      spread_bps        the MEDIAN over every quote sample in the day.  A
                        summary of the day.  Robust, and by construction
                        blind to anything that has not yet dominated half
                        the day's samples.
      last_spread_bps   the SINGLE newest quote sample of the day, stamped
                        with last_sample_at.  Noisy, and the only one of the
                        two that describes the book as it currently stands.

    A dislocation that arrives late in the day does not move the median
    until it owns half the day's samples, so the median lags in exactly the
    direction that hides a fresh dislocation.  Anything presenting CURRENT
    state -- the dislocation flag, the "obs last" column, the DISCREPANCY
    block -- must read last_spread_bps.  Anything summarising the WINDOW
    reads spread_bps, which is what a median is right for.
    """

    day: str
    high: float
    low: float
    close: float
    samples: int
    spread_bps: float          # MEDIAN raw book spread across the day
    last_spread_bps: float     # raw book spread of the day's LAST sample
    last_sample_at: str        # timestamp of that last sample
    bid_span_bps: float        # dispersion of the bid side within the day
    ask_span_bps: float        # dispersion of the ask side within the day

    @property
    def degenerate(self) -> bool:
        """H == L (a zero-print day) collapses both estimators.

        Corwin-Schultz reads ln(H/L) = 0 into beta AND gamma, so alpha = 0
        and S = 0 exactly; Abdi-Ranaldo reads eta_t = c_t and returns 0 the
        same way.  Both then look like a measured zero spread rather than an
        absence of information, which is precisely the failure this gate
        exists to prevent.  Non-positive prices are folded in here too.
        """
        return not (self.high > self.low > 0.0)

    @property
    def dislocated(self) -> bool:
        """Is the book dislocated AS OF THE LAST SAMPLE?

        Reads last_spread_bps, never the daily median.  The flag is a claim
        about the book NOW, and a median cannot make one: it has to wait for
        half the day's samples to agree before it moves at all.
        """
        return self.last_spread_bps >= DISLOCATION_SPREAD_BPS

    @property
    def median_dislocated(self) -> bool:
        """The same threshold against the day's MEDIAN.

        Not the flag.  Carried only so the report can say when the two
        disagree, which is precisely the case the flag above exists to
        catch and the old median-driven flag could not.
        """
        return self.spread_bps >= DISLOCATION_SPREAD_BPS


# ==========================================================================
# Estimators
# ==========================================================================

def corwin_schultz(bar_t: Bar, bar_t1: Bar) -> float:
    """Corwin and Schultz (2012), JF 67(2), 719-760, "A Simple Way to
    Estimate Bid-Ask Spreads from Daily High and Low Prices".

    Two consecutive single days t and t+1, and the two-day high and low
    formed over both:

        beta  = [ln(H_t / L_t)]^2 + [ln(H_t1 / L_t1)]^2
        gamma = [ln(H_2day / L_2day)]^2
        alpha = (sqrt(2*beta) - sqrt(beta)) / (3 - 2*sqrt(2))
                - sqrt(gamma / (3 - 2*sqrt(2)))
        S     = 2 * (exp(alpha) - 1) / (1 + exp(alpha))

    where H_2day = max(H_t, H_t1) and L_2day = min(L_t, L_t1).

    The intuition the paper trades on: the two-day range contains one
    spread, the sum of two single-day ranges contains two, and volatility
    scales with the square root of time while the spread does not.  The
    difference isolates the spread.

    S is a PROPORTIONAL spread (a fraction of price), so bps = S * 10_000.
    Bounded above by 2.0 as alpha -> +inf, i.e. 20,000 bps -- worth knowing
    before treating a large output as informative.

    The paper's overnight-return adjustment (its eq. 15, which shifts the
    day-t+1 high and low when the two days do not overlap) is deliberately
    NOT applied: it corrects for a closed session, and a Chia DEX book runs
    continuously with no session boundary for prices to jump across.

    Callers must pre-screen both bars with Bar.degenerate; this function
    assumes H > L > 0 on both.
    """
    beta = math.log(bar_t.high / bar_t.low) ** 2 \
        + math.log(bar_t1.high / bar_t1.low) ** 2
    two_day_high = max(bar_t.high, bar_t1.high)
    two_day_low = min(bar_t.low, bar_t1.low)
    gamma = math.log(two_day_high / two_day_low) ** 2

    alpha = (math.sqrt(2.0 * beta) - math.sqrt(beta)) / CS_K \
        - math.sqrt(gamma / CS_K)
    exp_alpha = math.exp(alpha)
    return 2.0 * (exp_alpha - 1.0) / (1.0 + exp_alpha)


def abdi_ranaldo_sq(bar_t: Bar, bar_t1: Bar) -> float:
    """Abdi and Ranaldo (2017), RFS 30(12), 4437-4480, "A Simple Estimation
    of Bid-Ask Spreads from Daily Close, High, and Low Prices".

    With lower-case letters for logs, c_t = ln(C_t), h_t = ln(H_t),
    l_t = ln(L_t), and the mid-range

        eta_t = (h_t + l_t) / 2

    the estimator's moment condition is

        E[ 4 * (c_t - eta_t) * (c_t - eta_t1) ] = S^2

    so this returns the per-day-pair S^2 term

        S^2_t = 4 * (c_t - eta_t) * (c_t - eta_t1)

    which the caller aggregates.  Unlike Corwin-Schultz, the close is used,
    which is what lets a single close sit between two mid-range estimates of
    the same efficient price; the spread falls out of the covariance.

    The paper documents this estimator as MOST ACCURATE FOR LESS LIQUID
    STOCKS, and reports that it is only MARGINALLY SENSITIVE to the number
    of trades per day once that number is above roughly five -- which is why
    it is the more appropriate of the two for a market as thin as this one,
    and why its disagreement with the posted book spread carries weight.
    That insensitivity has a floor, however: see MIN_SAMPLES_PER_BAR and
    Ardia, Guidotti and Kroencke (2024) above.

    S^2_t is frequently NEGATIVE in small samples.  That is expected -- it is
    a sample covariance, not a variance -- and the sign is reported, never
    hidden.
    """
    eta_t = (math.log(bar_t.high) + math.log(bar_t.low)) / 2.0
    eta_t1 = (math.log(bar_t1.high) + math.log(bar_t1.low)) / 2.0
    c_t = math.log(bar_t.close)
    return 4.0 * (c_t - eta_t) * (c_t - eta_t1)


# ==========================================================================
# Database (read-only)
# ==========================================================================

class DbReader:
    """Read-only reader.  Opened with a mode=ro URI: the live bot owns this
    file and this script must never write, VACUUM or migrate it."""

    def __init__(self, db_path: Path) -> None:
        uri = f"file:{db_path.as_posix()}?mode=ro"
        self._con = sqlite3.connect(uri, uri=True, timeout=10)
        self._con.execute("PRAGMA busy_timeout = 10000")

    def close(self) -> None:
        self._con.close()

    @staticmethod
    def _cutoff(days: float) -> str:
        """Lexical ISO cutoff.

        TIMESTAMP FORMAT TRAP (see scripts/offer_sizing.py:181-186, 207-212):
        trade_log.timestamp separates date and time with 'T', while
        snapshots.created_at and offer_log.created_at use a SPACE.  Both
        tables read here are the space-separated kind, so one cutoff format
        serves both -- but do not copy this cutoff into a trade_log query.
        """
        return (datetime.now(timezone.utc) - timedelta(days=days)) \
            .strftime("%Y-%m-%d %H:%M:%S")

    def pairs(self, source: str, days: float) -> list[str]:
        cutoff = self._cutoff(days)
        if source == "offer_log":
            sql = ("SELECT DISTINCT pair_name FROM offer_log "
                   "WHERE created_at >= ? AND book_best_bid > 0 "
                   "AND book_best_ask > 0")
        else:
            sql = ("SELECT DISTINCT pair_name FROM snapshots "
                   "WHERE created_at >= ? AND mid_price_mojos > 0 "
                   "AND spread_bps IS NOT NULL")
        return sorted(r[0] for r in self._con.execute(sql, (cutoff,)))

    def quotes(self, pair: str, source: str,
               days: float) -> tuple[list[tuple[str, float, float]], int]:
        """[(created_at, bid, ask)] in quote-per-base units, plus the count
        of CROSSED/LOCKED samples dropped (ask <= bid).

        A crossed sample is not a spread observation -- it is two stale
        halves of the book sampled at different instants -- and admitting it
        would put a negative range into a log.  XCH/DBX carries a few.
        """
        cutoff = self._cutoff(days)
        if source == "offer_log":
            rows = self._con.execute(
                "SELECT created_at, book_best_bid, book_best_ask "
                "FROM offer_log WHERE pair_name = ? AND created_at >= ? "
                "AND book_best_bid > 0 AND book_best_ask > 0 "
                "ORDER BY created_at",
                (pair, cutoff),
            ).fetchall()
            raw = [(ts, b / PRICE_SCALE, a / PRICE_SCALE) for ts, b, a in rows]
        else:
            # REFUSED.  [PR #134 review] This branch used to reconstruct the
            # two sides as mid +/- mid*spread_bps/2e4 and called the inversion
            # "exact", on the strength of one worked example (mid 3.24975 with
            # spread 10768.52 bps giving back 1.50000 / 4.99950).  That example
            # only works because that book's published mid HAPPENED to be the
            # arithmetic midpoint.  In general it is not:
            #
            #   * `snapshots` has NO bid or ask column.  Verified against the
            #     live schema -- the only price fields are mid_price_mojos and
            #     spread_bps.
            #   * mid_price_mojos is MarketDataFeed::compute_mid() (persisted at
            #     engine.cpp:16530), which is a depth-weighted orderbook
            #     micro-price blended across DEX, CEX and AMM legs.
            #   * spread_bps is computed separately, from dex_best_bid/ask.
            #
            # Centring a BBO-derived spread on a blended micro-price mid does
            # not recover the sides, it fabricates two shifted ones.  Measured
            # on the only enabled pair: the micro-price left the book on 46.9%
            # of XCH/DBX ingests and was clamped onto best_bid, so on nearly
            # half the samples this reconstruction put BOTH sides a full
            # half-spread below the truth -- correct width, wrong level, and
            # the error varies sample to sample rather than cancelling.
            #
            # offer_log persists the real thing (book_best_bid/book_best_ask)
            # and is the default source.  Re-enable this branch only if the
            # BBO is actually persisted into snapshots.
            raise SystemExit(
                "--source snapshots is disabled: the snapshots table stores a "
                "blended micro-price mid and a separately-computed BBO spread, "
                "with no bid or ask column, so the two sides cannot be "
                "recovered from it. Use --source offer_log, which persists "
                "book_best_bid/book_best_ask directly."
            )

        clean = [(ts, b, a) for ts, b, a in raw if a > b > 0.0]
        return clean, len(raw) - len(clean)


# ==========================================================================
# Bar building
# ==========================================================================

def build_bars(quotes: list[tuple[str, float, float]], mode: str) -> list[Bar]:
    """Daily bars from a sampled quote series, newest last.

    Every bar carries BOTH the day's MEDIAN raw spread and the day's LAST
    sample's raw spread, with the timestamp of that last sample.  See Bar
    for which figure answers which question; they are not interchangeable
    and the report must never print one under the other's label.
    """
    by_day: dict[str, list[tuple[str, float, float]]] = defaultdict(list)
    for ts, bid, ask in quotes:
        by_day[ts[:10]].append((ts, bid, ask))

    bars: list[Bar] = []
    for day in sorted(by_day):
        # Sorted here rather than trusted from the caller.  Three fields --
        # close, last_spread_bps and last_sample_at -- all mean "the newest
        # sample of the day", and all three would silently degrade into
        # "whichever row the query happened to hand back last" if the
        # ORDER BY in DbReader.quotes were ever dropped or the caller ever
        # concatenated two sources.  The sort is cheap; a wrong "current
        # book spread" quoted to an operator is not.
        samples = sorted(by_day[day], key=lambda s: s[0])
        bids = [b for _, b, _ in samples]
        asks = [a for _, _, a in samples]
        mids = [(b + a) / 2.0 for _, b, a in samples]
        # Per-sample raw book spread, in day order, so [-1] is the newest.
        spreads = [(a - b) / ((a + b) / 2.0) * BPS for _, b, a in samples]
        if mode == "quote-touch":
            high, low = max(asks), min(bids)
        else:
            high, low = max(mids), min(mids)
        bid_ref = _median(bids) or 1.0
        ask_ref = _median(asks) or 1.0
        bars.append(Bar(
            day=day,
            high=high,
            low=low,
            close=mids[-1],
            samples=len(samples),
            spread_bps=float(_median(spreads) or 0.0),
            last_spread_bps=spreads[-1],
            last_sample_at=samples[-1][0],
            bid_span_bps=(max(bids) - min(bids)) / bid_ref * BPS,
            ask_span_bps=(max(asks) - min(asks)) / ask_ref * BPS,
        ))
    return bars


# ==========================================================================
# Per-pair estimation, with the gates
# ==========================================================================

def estimate_pair(pair: str, bars: list[Bar], crossed_dropped: int,
                  max_age_days: float, source: str, days: float) -> dict:
    """Run both estimators over *bars* subject to the freshness, degeneracy
    and sample gates.  Returns a result dict; "refused" carries the reason
    when no estimate is reported.

    *source* and *days* are carried only so the output can state which quote
    series and which window every descriptive figure was read off; they do
    not affect the estimates.
    """
    result: dict = {
        "pair": pair,
        "source": source,
        "window_days": days,
        "bars_seen": len(bars),
        "bars_used": 0,
        "bars_degenerate": 0,
        "bars_thin": 0,
        "crossed_dropped": crossed_dropped,
        "day_pairs_used": 0,
        "day_pairs_nonadjacent": 0,
        "cs_negative": 0,
        "ar_negative": 0,
        "cs_bps": None,
        "ar_bps": None,
        "ar_twoday_bps": None,
        # The MOST RECENT adjacent day pair, reported apart from the
        # lookback-wide averages above so the discrepancy block has a
        # comparator whose window is close to the instant it is compared
        # against.  See the "Comparing windows" note in the module docstring.
        "latest_day_pair": None,
        "latest_pair_cs_bps": None,
        "latest_pair_ar_bps": None,
        "latest_pair_ar_negative": None,
        "day_pair_span": None,
        # Three DISTINCT raw-spread figures, deliberately not collapsed into
        # one field.  See the block below where they are filled in.
        "observed_spread_window_median_bps": None,
        "latest_raw_bar_median_bps": None,
        "observed_spread_last_sample_bps": None,
        "observed_spread_last_sample_at": None,
        # TWO DISTINCT BARS AND TWO DISTINCT AGES, named apart on purpose.
        #
        #   latest_raw_bar*      bars[-1]: the newest bar SEEN. It may be
        #                        degenerate or too thin to feed either
        #                        estimator. Descriptive only.
        #   newest_usable_bar*   the newest bar that survives the degeneracy
        #                        and thinness filter, i.e. the newest bar
        #                        that actually CONTRIBUTES. This is the one
        #                        the freshness gate ages.
        #
        # They were one field once, and the gate read the raw one: a single
        # fresh unusable bar could certify a month-old estimate as fresh.
        "latest_raw_bar": bars[-1].day if bars else None,
        "latest_raw_bar_samples": bars[-1].samples if bars else None,
        "latest_raw_bar_age_days": None,
        "newest_usable_bar": None,
        "newest_usable_bar_age_days": None,
        # "dislocated" is THE flag and is driven by the last sample.
        # "dislocated_by_median" is reported only so a disagreement between
        # the two can be stated out loud; nothing branches on it.
        "dislocated": False,
        "dislocated_by_median": False,
        "refused": None,
        "notes": [],
    }
    if not bars:
        result["refused"] = "no quote samples in window"
        return result

    latest = bars[-1]
    # THREE DIFFERENT RAW-SPREAD FIGURES, kept apart on purpose:
    #
    #   window median   median across the window of each day's own median.
    #                   A window-level summary.  NOT the current book.
    #   latest bar med  the newest day's median.  Also NOT the current book:
    #                   a dislocation arriving late in the day does not move
    #                   it until it dominates half that day's samples.
    #   last sample     the single newest quote sample, with its timestamp.
    #                   THIS is the current posted book, and it is the only
    #                   one of the three permitted to drive the dislocation
    #                   flag or the DISCREPANCY block.
    #
    # The tool used to publish the latest bar's MEDIAN under the heading
    # "obs now" and it was quoted to an operator as the current posted book
    # spread in an argument about whether a book was dislocated right then.
    # A daily median cannot answer that question.
    result["observed_spread_window_median_bps"] = _median(
        [b.spread_bps for b in bars])
    result["latest_raw_bar_median_bps"] = latest.spread_bps
    result["observed_spread_last_sample_bps"] = latest.last_spread_bps
    result["observed_spread_last_sample_at"] = latest.last_sample_at
    result["dislocated"] = latest.dislocated
    result["dislocated_by_median"] = latest.median_dislocated
    if latest.dislocated:
        result["notes"].append(
            f"DISLOCATION FLAG: the most recent quote sample, taken at "
            f"{latest.last_sample_at}, shows a raw book spread of "
            f"{latest.last_spread_bps:,.0f} bps, at or above the "
            f"{DISLOCATION_SPREAD_BPS:,.0f} bps threshold. The estimates "
            f"above do not bound that figure in either direction -- they "
            f"are a different quantity computed a different way, and the "
            f"estimator is not monotonic in the sampled range. Read the gap "
            f"as a discrepancy to look into, not as a measurement of it."
        )
        # SIDE DISPERSION IS DESCRIPTIVE ONLY.
        #
        # This block used to convert the two span figures into a claim about
        # WHICH SIDE was absent -- "the ask side never moved all day while
        # the bid did", and so on.  That inference is withdrawn.  It failed
        # on both counts a claim like it has to survive:
        #
        #   * It is not the measurement it was being read as.  bid_span_bps
        #     and ask_span_bps are the dispersion of OUR QUOTE SAMPLES within
        #     a day.  Zero dispersion means a side did not move BETWEEN THE
        #     INSTANTS WE SAMPLED; it is not evidence about whether any
        #     counterparty would cross that side.  Dislocation and sample
        #     dispersion are different quantities and the tool was equating
        #     them.
        #   * It is sample-dependent, and this tool's own two sources
        #     disagree on the same book on the same day.  XCH/BYC on
        #     2026-08-31: --source offer_log gives ask dispersion 0 bps, and
        #     the denser --source snapshots gives 9,502 bps.  The sparser
        #     series therefore pointed the opposite way to the denser one,
        #     with no caveat attached to say so.
        #
        # The per-side verdict is not this tool's to give.  It belongs to
        # cpp/include/xop/execution/book_side_quality.hpp, which scores each
        # side against an INDEPENDENT anchor carrying no book input, rather
        # than against the side's own dispersion.  A diagnostic built on
        # quote samples must not pre-empt a test built on an anchor.
        #
        # The numbers are still printed, because the raw spans are a real
        # observation about the sample and a reader may want them.  What is
        # printed with them is the source, the window, the date, and the
        # statement that they do not rank the sides.
        result["notes"].append(
            f"SIDE DISPERSION (descriptive, NOT a per-side verdict): on bar "
            f"{latest.day}, read from source '{source}' over a {days:g}-day "
            f"window, sampled bid quotes spanned "
            f"{latest.bid_span_bps:,.0f} bps and sampled ask quotes spanned "
            f"{latest.ask_span_bps:,.0f} bps. Quote-sample dispersion is NOT "
            f"a measurement of book dislocation: it reports how far each "
            f"side moved between the instants THIS source happened to "
            f"sample, not whether a counterparty would cross either side, "
            f"and it changes with the source and the window -- the two "
            f"sources available here can and do disagree about the same "
            f"book on the same day. This tool does not say which side is "
            f"junk. That verdict belongs to the engine's per-side anchor "
            f"test, cpp/include/xop/execution/book_side_quality.hpp, which "
            f"scores each side against an independent anchor instead of "
            f"against its own dispersion."
        )

    if latest.dislocated != latest.median_dislocated:
        # The two straddle the threshold.  Say so out loud, in both
        # directions: a last sample above a median below is a dislocation
        # the median has not caught up with yet, and the reverse is one the
        # book has already come out of.  Either way the operator is looking
        # at a flag that appears to contradict the "obs med" column beside
        # it, and is owed the reason rather than left to guess.
        lagging = "has not yet caught up with" if latest.dislocated \
            else "has not yet let go of"
        result["notes"].append(
            f"MEDIAN AND LAST SAMPLE STRADDLE THE FLAG THRESHOLD: bar "
            f"{latest.day} has a median raw spread of "
            f"{latest.spread_bps:,.0f} bps over {latest.samples} samples, "
            f"while its last sample ({latest.last_sample_at}) shows "
            f"{latest.last_spread_bps:,.0f} bps -- opposite sides of the "
            f"{DISLOCATION_SPREAD_BPS:,.0f} bps threshold. The flag follows "
            f"the LAST SAMPLE, because the question it answers is about the "
            f"book NOW; the median {lagging} it, since a median cannot move "
            f"until half the day's samples agree."
        )

    # RAW newest-bar age.  DESCRIPTIVE ONLY -- this is the age of bars[-1],
    # the newest bar SEEN, which may be degenerate or too thin to feed
    # either estimator.  It stamps the "obs last" figure and nothing else.
    # The freshness gate below does not read it.
    today = datetime.now(timezone.utc).date()
    raw_age_days = (today - date.fromisoformat(latest.day)).days
    result["latest_raw_bar_age_days"] = raw_age_days

    # --- DEGENERACY GATE (per bar) ----------------------------------------
    # H == L means the day produced a single price: no range, no information.
    # Refuse the bar rather than let it return a plausible-looking zero.
    #
    # THIS RUNS BEFORE THE FRESHNESS GATE, and the order is the point: the
    # freshness gate has to age a bar that actually contributes, and which
    # bars contribute is not known until this filter has run.
    usable: list[Bar] = []
    for bar in bars:
        if bar.degenerate:
            result["bars_degenerate"] += 1
            continue
        if bar.samples < MIN_SAMPLES_PER_BAR:
            # Ardia, Guidotti and Kroencke (2024), JFE 161, 103916.
            result["bars_thin"] += 1
            continue
        usable.append(bar)
    result["bars_used"] = len(usable)

    # --- FRESHNESS GATE (newest USABLE bar) -------------------------------
    # THE AGE THAT MATTERS IS THE AGE OF THE NEWEST BAR THAT CONTRIBUTES.
    #
    # This gate used to test bars[-1] -- and it did so BEFORE the filter
    # above ran, so the bar it aged was the newest bar seen, usable or not.
    # One fresh but unusable bar (a degenerate H==L day, or a day with a
    # single quote sample) was therefore enough to pass the gate while every
    # bar feeding the estimators was weeks old: five older usable bars and
    # today's junk bar would publish a month-stale estimate under a
    # freshness stamp saying zero days. The two ages live in separately
    # named fields for the same reason, and the refusal below says which one
    # it tested.
    if usable:
        newest_usable = usable[-1]
        usable_age_days = (today
                           - date.fromisoformat(newest_usable.day)).days
        result["newest_usable_bar"] = newest_usable.day
        result["newest_usable_bar_age_days"] = usable_age_days
        if usable_age_days > max_age_days:
            same = newest_usable.day == latest.day
            result["refused"] = (
                f"FRESHNESS GATE: the newest USABLE bar -- the newest one "
                f"that would actually contribute to an estimate -- is "
                f"{newest_usable.day}, {usable_age_days} days old, limit "
                f"{max_age_days:g}. THIS IS THE AGE THE GATE TESTED. The "
                f"newest bar SEEN is {latest.day} ({raw_age_days} days old)"
                + (", the same bar. " if same else
                   ", which is degenerate or too thin to contribute and so "
                   "cannot vouch for the estimate's freshness. ")
                + "A high/low estimator run on a stale book reports the "
                  "spread of a market that has stopped existing."
            )
            return result

    if len(usable) < MIN_USABLE_BARS:
        result["refused"] = (
            f"DEGENERACY GATE: only {len(usable)} usable bars "
            f"({result['bars_degenerate']} degenerate H==L, "
            f"{result['bars_thin']} below {MIN_SAMPLES_PER_BAR} samples); "
            f"need {MIN_USABLE_BARS} (and {MIN_USABLE_DAY_PAIRS} adjacent "
            f"day pairs, i.e. {MIN_USABLE_DAY_PAIRS + 1} CONSECUTIVE usable "
            f"bars, to produce an estimate). Newest usable bar "
            f"{result['newest_usable_bar'] or 'n/a'}; newest bar seen "
            f"{latest.day} ({raw_age_days} days old)."
        )
        return result

    # --- Estimation over ADJACENT calendar-day pairs ----------------------
    cs_values: list[float] = []
    ar_sq_values: list[float] = []
    # (day_t, day_t1, floored CS, raw AR S^2) per ADJACENT pair, in day
    # order, so [-1] is the most recent two-day window.  Kept because the
    # headline figures average the whole lookback while the posted book
    # spread is one instant; comparing those two directly is not
    # like-for-like, and the newest pair is the nearest comparator this data
    # affords.  See "Comparing windows" in the module docstring.
    pair_records: list[tuple[str, str, float, float]] = []
    for bar_t, bar_t1 in zip(usable, usable[1:], strict=False):
        if (date.fromisoformat(bar_t1.day) - date.fromisoformat(bar_t.day)).days != 1:
            # Both estimators are defined on CONSECUTIVE days.  Our sampling
            # has gaps (the bot is not always quoting every pair), and
            # joining across a gap would silently price a multi-day range as
            # a two-day one.
            result["day_pairs_nonadjacent"] += 1
            continue
        result["day_pairs_used"] += 1

        spread = corwin_schultz(bar_t, bar_t1)
        if spread < 0.0:
            result["cs_negative"] += 1
        # Corwin (2014) / the paper's own practice: negative two-day
        # estimates are set to zero before averaging into a monthly-style
        # figure.  The RAW negative count is reported alongside because a
        # high negative rate is not noise to be cleaned -- it is the
        # estimator telling us it is out of its regime on this data.
        cs_values.append(max(spread, 0.0))

        sq = abdi_ranaldo_sq(bar_t, bar_t1)
        if sq < 0.0:
            result["ar_negative"] += 1
        ar_sq_values.append(sq)

        pair_records.append((bar_t.day, bar_t1.day, max(spread, 0.0), sq))

    # --- SAMPLE GATE (adjacent day pairs) ---------------------------------
    # The real sample size.  MIN_USABLE_BARS above counted BARS, which
    # survive the gap-skipping just above while day pairs do not, so passing
    # it is no evidence at all about how many two-day terms were actually
    # averaged.  The `not cs_values` half is redundant while
    # MIN_USABLE_DAY_PAIRS >= 1 and is kept so the guard cannot be voided by
    # lowering the constant.
    if result["day_pairs_used"] < MIN_USABLE_DAY_PAIRS or not cs_values:
        nonadj = result["day_pairs_nonadjacent"]
        result["refused"] = (
            f"SAMPLE GATE: only {result['day_pairs_used']} adjacent day "
            f"pairs from {len(usable)} usable bars "
            f"({nonadj} candidate pair{'' if nonadj == 1 else 's'} straddled "
            f"a sampling gap); need {MIN_USABLE_DAY_PAIRS}. Both estimators "
            f"are TWO-DAY estimators and neither is defined on "
            f"non-consecutive days, so the ADJACENT DAY PAIR count is the "
            f"sample size -- {len(usable)} usable bars is not {len(usable)} "
            f"observations. Averaging this few two-day terms reports "
            f"sampling noise as a spread (Ardia, Guidotti & Kroencke 2024, "
            f"JFE 161, 103916)."
        )
        return result

    result["cs_bps"] = sum(cs_values) / len(cs_values) * BPS

    # Abdi-Ranaldo aggregates in VARIANCE space -- average the S^2 terms,
    # then floor at zero and take the root.  A negative mean is not a small
    # spread; it is no estimate at all, and is labelled as such below.
    ar_mean_sq = sum(ar_sq_values) / len(ar_sq_values)
    result["ar_mean_sq"] = ar_mean_sq
    result["ar_bps"] = (math.sqrt(ar_mean_sq) * BPS) if ar_mean_sq > 0.0 else 0.0
    if ar_mean_sq <= 0.0:
        result["notes"].append(
            "Abdi-Ranaldo mean S^2 is negative -- the estimator returns no "
            "estimate here, not a zero spread."
        )
    # The paper's "two-day corrected" variant floors each daily term before
    # averaging instead of after.  It cannot go negative, so it is reported
    # as a bracket rather than as the headline.
    two_day = [math.sqrt(v) for v in ar_sq_values if v > 0.0]
    result["ar_twoday_bps"] = (sum(two_day) / len(ar_sq_values) * BPS
                               if two_day else 0.0)

    # --- The MOST RECENT two-day estimate ---------------------------------
    # Everything above averages over the whole lookback.  The posted book
    # spread the report compares against is a SINGLE quote sample at a
    # single instant.  Those cover different windows, and the ratio between
    # them cannot support a claim about the current book: a lookback-wide
    # average cannot distinguish a genuine spread that widened yesterday
    # from a quote that vanished yesterday.  So the newest adjacent day pair
    # is carried separately and the discrepancy block is computed against
    # IT, with the residual mismatch -- two days against one instant --
    # printed rather than hidden.  n = 1 day pair, which is why it is a
    # comparator and never a headline figure.
    #
    # pair_records is non-empty here: the sample gate above returns unless
    # day_pairs_used >= MIN_USABLE_DAY_PAIRS, and every counted pair appends.
    last_t, last_t1, last_cs, last_ar_sq = pair_records[-1]
    result["latest_day_pair"] = f"{last_t}..{last_t1}"
    result["day_pair_span"] = f"{pair_records[0][0]}..{pair_records[-1][1]}"
    result["latest_pair_cs_bps"] = last_cs * BPS
    result["latest_pair_ar_bps"] = (math.sqrt(last_ar_sq) * BPS
                                    if last_ar_sq > 0.0 else 0.0)
    result["latest_pair_ar_negative"] = last_ar_sq <= 0.0

    neg_rate = result["cs_negative"] / result["day_pairs_used"]
    if neg_rate > 0.5:
        result["notes"].append(
            f"Corwin-Schultz returned a negative estimate on "
            f"{result['cs_negative']} of {result['day_pairs_used']} day pairs "
            f"({neg_rate:.0%}) -- above 50% the zero-flooring dominates the "
            f"average and the reported figure is an artefact of the flooring, "
            f"not a measurement."
        )
    return result


# ==========================================================================
# dexie (optional, read-only GET)
# ==========================================================================

def fetch_dexie_markets() -> dict:
    """GET the dexie markets document.

    Standard library only: pyproject.toml lists requests under the OPTIONAL
    [gui] extra, so a diagnostic that reaches for it stops working on a
    checkout without the GUI installed.  The URL is the module constant
    above -- a literal https endpoint, not caller-supplied.
    """
    req = urllib.request.Request(  # noqa: S310 - literal https constant
        DEXIE_MARKETS_URL,
        headers={"User-Agent": "xop-trader-highlow-diagnostic/1.0"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:  # noqa: S310
        return json.load(resp)


def dexie_high_low(payload: dict, pair: str,
                   max_age_days: float = MAX_BAR_AGE_DAYS) -> dict:
    """TickerData-style daily high/low for *pair*, in OUR orientation.

    *max_age_days* is the operator's --max-age-days, threaded in rather than
    read off the module constant.  It used to be the constant: the header
    printed the operator's value while this gate silently applied 2.0, so
    --max-age-days 5 produced a report claiming a 5-day limit and refusing
    at 2.  The value actually applied is returned in the row so the report
    can print the limit that fired rather than one it assumed.

    These are the same price_high / price_low that dexie_client.cpp:647-650
    parses into TickerData and dexie_client.cpp:821-828 orientation-swaps --
    and that nothing in the engine then reads.  They are free input.

    ORIENTATION: dexie keys markets by pair_id and quotes CAT_XCH, i.e.
    price is XCH per CAT.  Our XCH/BYC is BYC per XCH, so the swap is a
    RECIPROCAL AND A SIDE FLIP: our high is 1/their low and our low is
    1/their high.  Getting only the reciprocal right silently inverts the
    bar.

    A single 24h high/low is ONE bar.  Corwin-Schultz and Abdi-Ranaldo both
    need two consecutive bars, so this cannot drive either estimator on its
    own; it is reported as an independent range check and gate test.
    """
    base, _, quote = pair.partition("/")
    base_id = ASSET_IDS.get(base.upper())
    quote_id = ASSET_IDS.get(quote.upper())
    out: dict = {"pair": pair, "available": False, "reason": ""}
    if base_id is None or quote_id is None:
        out["reason"] = f"no dexie asset id known for {pair}"
        return out

    markets = payload.get("markets") or {}
    entry = None
    inverted = False
    for pair_id, listings in markets.items():
        for market in listings:
            market_id = market.get("id")
            if pair_id == base_id and market_id == quote_id:
                entry, inverted = market, False
                break
            if pair_id == quote_id and market_id == base_id:
                # dexie quotes it the other way round from us.
                entry, inverted = market, True
                break
        if entry is not None:
            break
    if entry is None:
        out["reason"] = f"dexie lists no market for {pair}"
        return out

    prices = entry.get("prices") or {}
    high = (prices.get("high") or {}).get("daily")
    low = (prices.get("low") or {}).get("daily")
    last = prices.get("last") or {}
    if not high or not low or high <= 0 or low <= 0:
        out["reason"] = "dexie reports no daily high/low"
        return out

    last_price = last.get("price")
    if inverted:
        our_high, our_low = high, low
    else:
        # dexie price is XCH-per-CAT while our pair is CAT-per-XCH.
        our_high, our_low = 1.0 / low, 1.0 / high
        # The last price needs the SAME reciprocal, or the report mixes two
        # orientations in one block and reads as a 2x discrepancy that is
        # not there.
        if last_price:
            last_price = 1.0 / float(last_price)

    # Freshness applies here too, and from a better clock than the database
    # has: dexie stamps the last trade, so this is real trade staleness
    # rather than sampling staleness.
    #
    # FRACTIONAL, not timedelta.days.  This is a datetime subtraction, so
    # .days FLOORS to whole days: a 2.96-day-old print reported as 2 and
    # PASSED a limit documented as 2.0 days.  The bar ages elsewhere in this
    # file are date-minus-date, where .days is exact and no such rounding
    # exists -- this was the only datetime subtraction, and the only place
    # the floor could bite.
    last_date = last.get("date")
    stale_days = None
    if last_date:
        try:
            stamped = datetime.fromisoformat(
                str(last_date).replace("Z", "+00:00"))
            stale_days = ((datetime.now(timezone.utc) - stamped)
                          .total_seconds() / 86400.0)
        except ValueError:
            stale_days = None

    out.update({
        "available": True,
        "high": our_high,
        "low": our_low,
        "last_price": last_price,
        "last_date": last_date,
        "last_trade_age_days": stale_days,
        "inverted": not inverted,
        "range_bps": math.log(our_high / our_low) * BPS,
        "degenerate": not (our_high > our_low > 0.0),
        "stale": stale_days is not None and stale_days > max_age_days,
        # The limit this row was actually judged against, carried so the
        # report prints the number that fired instead of assuming one.
        "max_age_days": max_age_days,
    })
    return out


# ==========================================================================
# Presentation
# ==========================================================================

def _fmt_bps(value: float | None) -> str:
    return "     n/a" if value is None else f"{value:>8,.1f}"


def print_report(results: list[dict], dexie_rows: list[dict],
                 source: str, mode: str, days: float,
                 max_age_days: float) -> None:
    print("Low-frequency high/low spread estimators -- OPERATOR DIAGNOSTIC")
    print("=" * 78)
    print("Corwin & Schultz (2012) JF 67(2) 719-760; "
          "Abdi & Ranaldo (2017) RFS 30(12) 4437-4480.")
    print("Gates per Ardia, Guidotti & Kroencke (2024) JFE 161 103916.")
    print()
    print(f"source: {source}   bars: {mode}   window: {days:g} days   "
          f"freshness limit: {max_age_days:g} days")
    print(f"sample gate: at least {MIN_USABLE_DAY_PAIRS} adjacent day pairs "
          f"-- the sample size, because both")
    print("             estimators are two-day estimators "
          "(Ardia et al. 2024)")
    print(f"generated: {datetime.now(timezone.utc):%Y-%m-%d %H:%M:%SZ}")
    print()
    print("NOT A LIVE ENGINE INPUT. These estimates come from quote samples "
          "standing in for")
    print("trade prices, which is outside the regime all three estimators "
          "were derived for.")
    print()
    for line in _wrap(NOT_MONOTONIC_NOTE, 78):
        print(line)
    print()
    for line in _wrap(ONE_SIDED_NOTE, 78):
        print(line)
    print()

    # 'obs med' and 'obs last' are two different quantities and the headers
    # say so.  The previous pair of headers was 'obs bps' / 'obs now', which
    # named neither: 'obs now' was in fact the latest bar's MEDIAN, and it
    # was read off this table and quoted as the current posted book spread.
    header = (f"{'pair':<16}{'bars':>5}{'n pairs':>8}{'degen':>6}{'thin':>5}"
              f"{'CS-':>5}{'AR-':>5}{'CS bps':>10}{'AR bps':>10}"
              f"{'obs med':>10}{'obs last':>11}")
    print(header)
    print("-" * len(header))
    for r in results:
        if r["refused"]:
            # No estimate is shown, so no sample size is shown.  '-' rather
            # than 0 means "not reported here", NOT "zero pairs": freshness
            # and degeneracy refusals genuinely never reach the pairing loop,
            # but a SAMPLE GATE refusal does reach it and holds a real count
            # (2 or 3).  Printing 0 for both would assert something false
            # about the second.  The per-pair refusal text below states the
            # actual count for every gate that has one.
            print(f"{r['pair']:<16}{r['bars_seen']:>5}{'-':>8}"
                  f"{r['bars_degenerate']:>6}{r['bars_thin']:>5}"
                  f"{'-':>5}{'-':>5}{'REFUSED':>10}{'REFUSED':>10}"
                  f"{_fmt_bps(r['observed_spread_window_median_bps']):>10}"
                  f"{_fmt_bps(r['observed_spread_last_sample_bps']):>11}")
            continue
        print(f"{r['pair']:<16}{r['bars_used']:>5}{r['day_pairs_used']:>8}"
              f"{r['bars_degenerate']:>6}{r['bars_thin']:>5}"
              f"{r['cs_negative']:>5}{r['ar_negative']:>5}"
              f"{_fmt_bps(r['cs_bps']):>10}{_fmt_bps(r['ar_bps']):>10}"
              f"{_fmt_bps(r['observed_spread_window_median_bps']):>10}"
              f"{_fmt_bps(r['observed_spread_last_sample_bps']):>11}")
    print()
    print("bars  = non-degenerate daily bars used (bars SEEN when REFUSED)")
    print("n pairs = ADJACENT DAY PAIRS = THE SAMPLE SIZE. Both estimators "
          "are two-day")
    print("        estimators, so bars are not observations; "
          f"below {MIN_USABLE_DAY_PAIRS} the pair is REFUSED.")
    print("degen = bars skipped, high == low (zero-print day)")
    print("thin  = bars skipped, fewer than "
          f"{MIN_SAMPLES_PER_BAR} samples (Ardia et al. 2024)")
    print("CS-/AR- = RAW negative estimates before zero-flooring "
          "(Corwin 2014)")
    # The two raw-spread columns are a WINDOW MEDIAN and a SINGLE MOST
    # RECENT OBSERVATION.  The legend they replace read "obs bps = median
    # RAW book spread across the window; obs now = latest bar", which left
    # a reader to guess whether "latest bar" meant the newest observation
    # or an average over the newest day -- it meant the latter, and it was
    # quoted to an operator as the former.  Spell both out.
    print("obs med  = RAW book spread, WINDOW MEDIAN: the median across the "
          "window of each")
    print("           day's own median sample. A summary of the window. "
          "NOT the book now,")
    print("           and it cannot move until half a day's samples agree.")
    print("obs last = RAW book spread, SINGLE MOST RECENT QUOTE SAMPLE: "
          "one observation,")
    print("           no averaging -- the book as it currently stands. "
          "The dislocation")
    print("           flag and the DISCREPANCY block read THIS column, "
          "never obs med.")
    # One line per pair rather than a wrapped run-on: a wrapped list splits
    # timestamps across lines and separates a stamp from its pair, which is
    # the wrong failure mode for the one column whose whole point is being
    # pinned to an instant.
    stamps = [(r["pair"], r["observed_spread_last_sample_at"])
              for r in results if r.get("observed_spread_last_sample_at")]
    if stamps:
        print("           obs last was sampled at (UTC):")
        for pair, taken_at in stamps:
            print(f"             {pair:<16} {taken_at}")
    print()
    print("Corwin-Schultz is BOUNDED: S = 2(e^a - 1)/(1 + e^a) -> 2.0 as "
          "a -> +inf, i.e. it")
    print("cannot exceed 20,000 bps. Do not read a large CS output as "
          "confirmation of a")
    print("large spread -- read it as the estimator saturating.")
    print()

    for r in results:
        print(f"{r['pair']}")
        if r["refused"]:
            for line in _wrap(r["refused"], 74):
                print(f"  {line}")
        else:
            n = r["day_pairs_used"]
            for line in _wrap(
                    f"SAMPLE SIZE n = {n} adjacent day pairs, from "
                    f"{r['bars_used']} usable bars. Both estimators are "
                    f"two-day estimators, so bars are not observations. "
                    f"Skipped non-adjacent {r['day_pairs_nonadjacent']}, "
                    f"crossed samples dropped {r['crossed_dropped']}.", 74):
                print(f"  {line}")
            print(f"  Corwin-Schultz  {r['cs_bps']:>10,.1f} bps   "
                  f"(n={n} pairs; {r['cs_negative']}/{n} raw negatives "
                  f"floored to zero)")
            print(f"  Abdi-Ranaldo    {r['ar_bps']:>10,.1f} bps   "
                  f"(n={n} pairs; {r['ar_negative']}/{n} negative S^2 terms)")
            print(f"    two-day variant {r['ar_twoday_bps']:>8,.1f} bps   "
                  f"(n={n} pairs; each term floored before averaging)")
            # The current book, the window summary, and the newest day's
            # median -- printed as three separate lines under three separate
            # labels, because they are three separate numbers and only the
            # first is an answer to "what is the book doing now".
            obs_last = r["observed_spread_last_sample_bps"] or 0.0
            obs_med = r["observed_spread_window_median_bps"] or 0.0
            print(f"  raw book spread NOW    {obs_last:>10,.1f} bps   "
                  f"(LAST SAMPLE, taken "
                  f"{r['observed_spread_last_sample_at']})")
            print(f"  raw book spread MEDIAN {obs_med:>10,.1f} bps   "
                  f"(WINDOW MEDIAN of {r['bars_seen']} daily medians; "
                  f"NOT now)")
            print(f"    newest bar SEEN {r['latest_raw_bar']}: median "
                  f"{r['latest_raw_bar_median_bps']:,.1f} bps over "
                  f"{r['latest_raw_bar_samples']} samples (also NOT now)")
            # TWO BARS, TWO AGES, SPELLED OUT SEPARATELY. The freshness gate
            # ages the newest bar that CONTRIBUTES; the newest bar seen may
            # be degenerate or too thin, and then it vouches for nothing.
            print(f"    bar ages: newest SEEN {r['latest_raw_bar']} "
                  f"({r['latest_raw_bar_age_days']}d), newest USABLE "
                  f"{r['newest_usable_bar']} "
                  f"({r['newest_usable_bar_age_days']}d)")
            print("              the freshness gate tested the USABLE age; "
                  "the seen bar may")
            print("              be degenerate or thin and vouches for "
                  "nothing")

            # --- DISCREPANCY, made as like-for-like as the data allows -----
            # obs_last is ONE sample at ONE instant. cs_bps / ar_bps average
            # every adjacent day pair in the lookback -- up to a month. A
            # ratio between those compares an instant against a month, and a
            # month-long average cannot tell a genuine spread that widened
            # yesterday from a quote that vanished yesterday, so no claim
            # about the CURRENT book survives the mismatch. The ratio below
            # therefore uses the MOST RECENT day pair, and the mismatch that
            # remains -- two days against one instant -- is printed.
            recent_cs = r["latest_pair_cs_bps"] or 0.0
            recent_ar = r["latest_pair_ar_bps"] or 0.0
            recent = max(recent_cs, recent_ar)
            window_max = max(r["cs_bps"] or 0.0, r["ar_bps"] or 0.0,
                             r["ar_twoday_bps"] or 0.0)
            print(f"  most recent day pair {r['latest_day_pair']}: "
                  f"CS {recent_cs:,.1f} bps, AR {recent_ar:,.1f} bps")
            print("    n=1 pair -- the like-for-like comparator for "
                  "'raw book spread NOW',")
            print("    never a headline figure")
            if obs_last > 0.0:
                obs_day = (r["observed_spread_last_sample_at"] or "")[:10]
                try:
                    lag_days = (
                        date.fromisoformat(obs_day)
                        - date.fromisoformat(
                            r["latest_day_pair"].split("..")[1])).days
                except ValueError:
                    lag_days = None
                obs_med_str = _fmt_bps(
                    r["observed_spread_window_median_bps"]).strip()
                preamble = (
                    f"DISCREPANCY (descriptive -- NOT a verdict about "
                    f"either side of the book). THE TWO OPERANDS COVER "
                    f"DIFFERENT WINDOWS, and here is by how much: the "
                    f"posted figure is ONE quote sample at ONE instant, "
                    f"{obs_last:,.1f} bps at "
                    f"{r['observed_spread_last_sample_at']}; the comparator "
                    f"is the estimate over the single most recent day pair "
                    f"{r['latest_day_pair']} (n=1"
                    + ("" if lag_days is None else
                       ", ending on that sample's own calendar day"
                       if lag_days == 0 else
                       f", ending {lag_days} day"
                       f"{'' if lag_days == 1 else 's'} before that sample")
                    + f"). Two days against one instant is still not a "
                    f"match; it is the closest this data allows. The "
                    f"headline CS/AR figures above average n={n} day pairs "
                    f"spanning {r['day_pair_span']} -- a different window "
                    f"again, and NOT what this ratio uses (their largest is "
                    f"{window_max:,.1f} bps). Window median raw spread for "
                    f"reference {obs_med_str} bps. "
                )
                if recent > 0.0:
                    ratio = obs_last / recent
                    if ratio <= 1.0 and r["dislocated"]:
                        # A RATIO AT OR BELOW 1.0 IS AGREEMENT, NOT AN
                        # ALL-CLEAR.
                        #
                        # This branch used to print "No discrepancy to look
                        # into." for XCH/BYC while the DISLOCATION FLAG note
                        # three lines below reported the same book at 14,667
                        # bps.  Both were emitted, adjacent, in the same run,
                        # and the output contradicted itself.
                        #
                        # The cause is that the ratio compares two
                        # quantities and carries no information about the
                        # MAGNITUDE of either.  The divisor is the LARGER of
                        # the two comparators (max of CS and AR, chosen so
                        # the tool understates rather than overstates a
                        # discrepancy), which minimises the ratio -- so a
                        # dislocated book whose estimator agrees it is
                        # dislocated lands here, at 0.99x, and got read out
                        # as though the book were fine.
                        #
                        # So this branch now consults the same two facts the
                        # elif below does: the posted magnitude and
                        # r["dislocated"].  When the flag is set the
                        # all-clear is never emitted; what is said instead is
                        # that the ESTIMATOR AGREES THE BOOK IS WIDE.  That
                        # is a materially different statement and the
                        # operator needs it.
                        body = (
                            f"NOT AN ALL-CLEAR. The posted book spread is "
                            f"{obs_last:,.1f} bps, at or above the "
                            f"{DISLOCATION_SPREAD_BPS:,.0f} bps dislocation "
                            f"threshold, so this book carries the "
                            f"dislocation flag; and the like-for-like "
                            f"comparator, {recent:,.1f} bps, is of the SAME "
                            f"MAGNITUDE, which is the only reason the ratio "
                            f"is {ratio:.2f}x. A ratio at or below 1.0 says "
                            f"the two quantities AGREE. It says nothing "
                            f"about how large either of them is, and here "
                            f"they agree at a dislocated width: what this "
                            f"line reports is that THE ESTIMATOR AGREES THE "
                            f"BOOK IS WIDE, not that the book is fine. The "
                            f"agreement closes the DISCREPANCY, not the "
                            f"dislocation -- the finding is the DISLOCATION "
                            f"FLAG note below, and it stands. It remains "
                            f"NOT a finding that a side of the book is "
                            f"absent, and this tool does not draw one: "
                            f"neither estimator identifies SIDES -- both "
                            f"consume a high, a low and a close, with no "
                            f"step at which a bid is told from an ask -- "
                            f"and a high/low range cannot distinguish a "
                            f"genuinely widened spread from a quote that "
                            f"went missing. The per-side question belongs "
                            f"to the engine's per-side anchor test, "
                            f"cpp/include/xop/execution/book_side_quality"
                            f".hpp, which scores each side against an "
                            f"INDEPENDENT anchor. Take the verdict from "
                            f"there, not from here."
                        )
                    elif ratio <= 1.0:
                        # Reached only with the dislocation flag CLEAR, so
                        # the magnitude is known to be below the threshold
                        # and the all-clear is about a book that is
                        # genuinely narrow.  Said with the figure attached,
                        # and scoped to the direction this tool exists to
                        # watch (posted wider than estimated), because a
                        # bare all-clear is what went wrong above.
                        body = (
                            f"The posted book spread, {obs_last:,.1f} bps, "
                            f"is at or below the most recent two-day "
                            f"estimate ({ratio:.2f}x) and below the "
                            f"{DISLOCATION_SPREAD_BPS:,.0f} bps dislocation "
                            f"threshold, so the book carries no dislocation "
                            f"flag. No discrepancy to look into in the "
                            f"direction this block watches -- a posted "
                            f"spread far WIDER than the estimate."
                        )
                    elif r["dislocated"]:
                        body = (
                            f"The posted book spread is {ratio:,.1f}x the "
                            f"most recent two-day estimate, on a book "
                            f"already carrying the dislocation flag. THAT "
                            f"IS A LARGE DISCREPANCY BETWEEN TWO QUANTITIES "
                            f"COMPUTED DIFFERENT WAYS AND AN OPERATOR "
                            f"SHOULD LOOK AT IT. It is NOT a finding that a "
                            f"side of the book is absent, and this tool "
                            f"does not draw one: neither estimator "
                            f"identifies SIDES -- both consume a high, a "
                            f"low and a close, with no step at which a bid "
                            f"is told from an ask -- and a high/low range "
                            f"cannot distinguish a genuinely widened spread "
                            f"from a quote that went missing. The per-side "
                            f"question belongs to the engine's per-side "
                            f"anchor test, "
                            f"cpp/include/xop/execution/book_side_quality"
                            f".hpp, which scores each side against an "
                            f"INDEPENDENT anchor. Take the verdict from "
                            f"there, not from here."
                        )
                    else:
                        body = (
                            f"The posted book spread is {ratio:,.1f}x the "
                            f"most recent two-day estimate, on a book NOT "
                            f"carrying the dislocation flag. Consistent "
                            f"with the ordinary quoted-versus-effective "
                            f"spread wedge plus estimator bias. Nothing "
                            f"here identifies a missing side either way -- "
                            f"that is book_side_quality.hpp's question."
                        )
                else:
                    body = (
                        f"NO RATIO IS REPORTED: the most recent day pair "
                        f"{r['latest_day_pair']} produced no positive "
                        f"estimate (Corwin-Schultz floored to zero and "
                        f"Abdi-Ranaldo's S^2 term was non-positive), so "
                        f"there is nothing like-for-like to divide the "
                        f"{obs_last:,.1f} bps posted spread by. The "
                        f"lookback-wide figures above cover a different "
                        f"window and are not a substitute for one."
                    )
                # The comparator is now a DIVISOR, so a saturating
                # Corwin-Schultz term silently shrinks the ratio and can
                # turn a real gap into "nothing to look into". The bounded-
                # ness note in the header covers the headline figures; this
                # covers the one number the ratio actually rests on.
                if recent_cs >= (CS_SATURATION_WARN_FRACTION
                                 * CS_SATURATION_BPS):
                    body += (
                        f" CAVEAT ON THE DIVISOR: the comparator's "
                        f"Corwin-Schultz term, {recent_cs:,.1f} bps, is "
                        f"{recent_cs / CS_SATURATION_BPS:.0%} of the "
                        f"{CS_SATURATION_BPS:,.0f} bps ceiling that "
                        f"S = 2(e^a - 1)/(1 + e^a) imposes as alpha -> "
                        f"+inf. Read it as the estimator saturating rather "
                        f"than as a measured width, and the ratio above as "
                        f"correspondingly soft."
                    )
                for line in _wrap(preamble + body, 74):
                    print(f"  {line}")
        for note in r["notes"]:
            for line in _wrap("NOTE: " + note, 74):
                print(f"  {line}")
        print()

    if dexie_rows:
        print("dexie GET /v1/markets -- TickerData price_high / price_low")
        print("-" * 78)
        print("A single 24h high/low is ONE bar. Both estimators need two "
              "consecutive bars,")
        print("so this is a range check and a gate test, not an estimate.")
        print()
        for d in dexie_rows:
            print(f"{d['pair']}")
            if not d.get("available"):
                print(f"  unavailable: {d['reason']}")
                print()
                continue
            swap = " (orientation-swapped from dexie's CAT_XCH quoting)" \
                if d["inverted"] else ""
            print(f"  24h high {d['high']:.6f}  low {d['low']:.6f}{swap}")
            print(f"  log range {d['range_bps']:,.1f} bps")
            last_price = d.get("last_price")
            # Two decimals, because the age is FRACTIONAL and the whole
            # point of making it so is that a reader can tell 2.96 from 2.
            # Printing the floor here would put the gate's real input back
            # out of sight even after the gate itself was fixed.
            print(f"  last trade "
                  f"{'n/a' if last_price is None else f'{last_price:.6f}'} "
                  f"at {d.get('last_date')}"
                  + ("" if d.get("last_trade_age_days") is None
                     else f" ({d['last_trade_age_days']:.2f} days ago)"))
            if d["degenerate"]:
                print("  DEGENERACY GATE: high == low. One print, no range. "
                      "Refused.")
            if d.get("stale"):
                # The limit that actually fired, read off the row, not the
                # module constant: this gate takes the operator's
                # --max-age-days and the two used to disagree.
                print(f"  FRESHNESS GATE: last trade is "
                      f"{d['last_trade_age_days']:.2f} days old, limit "
                      f"{d.get('max_age_days', max_age_days):g}. Refused.")
            print()


def _wrap(text: str, width: int) -> list[str]:
    words = text.split()
    lines: list[str] = []
    current = ""
    for word in words:
        candidate = f"{current} {word}".strip()
        if len(candidate) > width and current:
            lines.append(current)
            current = word
        else:
            current = candidate
    if current:
        lines.append(current)
    return lines


# ==========================================================================
# CLI
# ==========================================================================

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Corwin-Schultz and Abdi-Ranaldo low-frequency spread "
                    "estimators, as an operator diagnostic. Read-only.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="OPERATOR DIAGNOSTIC ONLY -- never a live engine input.",
    )
    parser.add_argument("--db", default=str(DB_PATH),
                        help=f"engine database (default {DB_PATH})")
    parser.add_argument("--days", type=float, default=DEFAULT_LOOKBACK_DAYS,
                        help=f"lookback window in days "
                             f"(default {DEFAULT_LOOKBACK_DAYS:g})")
    parser.add_argument("--pair", action="append", metavar="BASE/QUOTE",
                        help="restrict to this pair (repeatable; "
                             "default all pairs with data in the window)")
    parser.add_argument("--source", choices=SOURCES, default="offer_log",
                        help="quote series: offer_log book_best_bid/ask "
                             "(third-party BBO samples) or snapshots "
                             "mid+spread (denser; default offer_log)")
    parser.add_argument("--bars", choices=BAR_MODES, default="quote-touch",
                        help="bar construction: a SENSITIVITY choice, NOT a "
                             "bound -- these estimators are not monotonic in "
                             "the sampled range, so another mode can return "
                             "a higher number as easily as a lower one "
                             "(default quote-touch, the widest daily range)")
    parser.add_argument("--max-age-days", type=float, default=MAX_BAR_AGE_DAYS,
                        help=f"freshness gate: refuse a pair whose newest "
                             f"USABLE bar -- the newest one that actually "
                             f"contributes to an estimate, not merely the "
                             f"newest one seen -- is older than this. Also "
                             f"gates the --dexie last-trade age, which is "
                             f"measured in FRACTIONAL days "
                             f"(default {MAX_BAR_AGE_DAYS:g})")
    parser.add_argument("--dexie", action="store_true",
                        help="also GET api.dexie.space/v1/markets for live "
                             "TickerData-style 24h high/low")
    parser.add_argument("--json", action="store_true",
                        help="emit JSON instead of the text report")
    args = parser.parse_args(argv)

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"error: database not found at {db_path}", file=sys.stderr)
        return 2

    db = DbReader(db_path)
    try:
        available = db.pairs(args.source, args.days)
        if args.pair:
            wanted = list(dict.fromkeys(args.pair))
            missing = [p for p in wanted if p not in available]
            if missing:
                print(f"error: no {args.source} data in the last "
                      f"{args.days:g} days for: {', '.join(missing)}",
                      file=sys.stderr)
                print(f"       available: {', '.join(available) or '(none)'}",
                      file=sys.stderr)
                return 2
            selected = wanted
        else:
            selected = available

        results = []
        for pair in selected:
            quotes, crossed = db.quotes(pair, args.source, args.days)
            bars = build_bars(quotes, args.bars)
            results.append(
                estimate_pair(pair, bars, crossed, args.max_age_days,
                              args.source, args.days))
    finally:
        db.close()

    dexie_rows: list[dict] = []
    if args.dexie:
        try:
            payload = fetch_dexie_markets()
        except (urllib.error.URLError, TimeoutError, ValueError) as exc:
            print(f"error: dexie unreachable: {exc}", file=sys.stderr)
            return 2
        dexie_rows = [dexie_high_low(payload, p, args.max_age_days)
                      for p in selected]

    if args.json:
        print(json.dumps({
            "diagnostic": "highlow_spread_estimator",
            "operator_diagnostic_only": True,
            "not_a_live_engine_input": True,
            "one_sided_book_note": ONE_SIDED_NOTE,
            # The bar mode is a sensitivity choice, not a ceiling: these
            # estimators are not monotonic in the sampled range.  A JSON
            # consumer must not treat any figure below as an upper bound.
            "not_an_upper_bound_note": NOT_MONOTONIC_NOTE,
            "estimate_is_monotonic_in_sampled_range": False,
            "source": args.source,
            "bars": args.bars,
            "days": args.days,
            "max_age_days": args.max_age_days,
            # The sample size for every estimate in "pairs" is that pair's
            # day_pairs_used, NOT bars_used: both estimators are two-day
            # estimators.  min_day_pairs is the gate below which a pair is
            # refused outright.
            "sample_size_field": "day_pairs_used",
            # Which raw-spread field means what, so a JSON consumer cannot
            # repeat the mistake the text report used to invite.
            # observed_spread_last_sample_bps is a SINGLE observation and is
            # the only field describing the book NOW; it is what "dislocated"
            # is computed from.  observed_spread_window_median_bps and
            # latest_raw_bar_median_bps are medians and are window/day
            # summaries -- neither is current state.
            "current_spread_field": "observed_spread_last_sample_bps",
            "current_spread_timestamp_field": "observed_spread_last_sample_at",
            "window_spread_field": "observed_spread_window_median_bps",
            # The like-for-like comparator for current_spread_field.  cs_bps
            # and ar_bps average day_pairs_used pairs across the whole
            # lookback; comparing an instant against that is not
            # like-for-like, so a consumer wanting a ratio must use these.
            "recent_estimate_fields": ["latest_pair_cs_bps",
                                       "latest_pair_ar_bps"],
            "recent_estimate_window_field": "latest_day_pair",
            "window_estimate_span_field": "day_pair_span",
            # Neither estimator identifies SIDES.  A discrepancy between
            # current_spread_field and the recent estimate is descriptive; a
            # per-side verdict comes from the engine's anchor test.
            "per_side_verdict_source":
                "cpp/include/xop/execution/book_side_quality.hpp",
            # TWO bar ages, deliberately distinct.  The freshness gate reads
            # newest_usable_bar_age_days -- the newest bar that contributes
            # to an estimate.  latest_raw_bar_age_days is the newest bar
            # SEEN and is descriptive only; it may be degenerate or thin.
            "freshness_gate_field": "newest_usable_bar_age_days",
            "raw_newest_bar_age_field": "latest_raw_bar_age_days",
            "min_day_pairs": MIN_USABLE_DAY_PAIRS,
            "min_usable_bars": MIN_USABLE_BARS,
            "generated_at": datetime.now(timezone.utc)
            .strftime("%Y-%m-%d %H:%M:%SZ"),
            "pairs": results,
            "dexie": dexie_rows,
        }, indent=2))
    else:
        print_report(results, dexie_rows, args.source, args.bars,
                     args.days, args.max_age_days)
    return 0


if __name__ == "__main__":
    sys.exit(main())
