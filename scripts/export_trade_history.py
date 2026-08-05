#!/usr/bin/env python3
"""Export the full trade_log to a durable CSV in data/trade_history/.

Produces data/trade_history/trades_full.csv -- a complete, human-readable,
restart-proof export of every fill the bot has ever recorded.  The engine
separately appends new fills to trades_live.csv as they settle (PnLTracker::
append_history_csv, 2026-07-30); this script regenerates the full historical
export on demand and is safe to re-run at any time.

Column layout matches trades_live.csv exactly so the two files can be
concatenated / diffed:

    timestamp_utc, trade_id, pair, side,
    price_pseudo_mojos, size_base_mojos,
    price_quote_per_base, size_base_units, quote_amount,
    fee_xch_mojos, cost_basis_pseudo_mojos,
    realized_pnl_quote_mojos, realized_pnl_usd, block_height

Era handling (see db/schema.md and scripts/compute_actual_pnl.py):
  * price_mojos < 1e9  -> legacy encoding (Apr 3-5 2026): quote mojos per
    base display unit.  price_quote_per_base = price / quote_mojos_per_unit.
  * price_mojos >= 1e9 -> pseudo encoding (Apr 14 2026 onward):
    quote_units_per_base * 1e12.  price_quote_per_base = price / 1e12.

Fee note: trade_log.fee_mojos has been 0 since June 2026 (engine bug, fixed
2026-07-30 -- the fee was read from State after the offer was removed).  This
export therefore takes the fee from offer_log.fee_mojos (the creation fee for
the filled offer), falling back to trade_log.fee_mojos for rows with no
offer_log match.

The database is opened read-only; this script never mutates anything.
"""

from __future__ import annotations

import csv
import sqlite3
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DB_PATH = REPO_ROOT / "data" / "xop_trader.db"
OUT_DIR = REPO_ROOT / "data" / "trade_history"
OUT_PATH = OUT_DIR / "trades_full.csv"

MOJOS_PER_XCH = 1_000_000_000_000

# Legacy-vs-pseudo price encoding threshold (compute_actual_pnl.py uses the
# same value): no realistic legacy quote-mojo price reaches 1e9, and no
# pseudo price is below it.
FORMAT_THRESHOLD = 1_000_000_000

# Per-pair denominations: (base_mojos_per_unit, quote_mojos_per_unit,
# usd_per_quote_unit or None when unknown).  wUSDC/wUSDC.b/USDS/BYC are
# USD-pegged CATs; DBX is not pegged, so its USD column is left blank.
PAIR_INFO: dict[str, tuple[int, int, float | None]] = {
    "XCH/wUSDC":    (MOJOS_PER_XCH, 1000, 1.0),
    "XCH/wUSDC.b":  (MOJOS_PER_XCH, 1000, 1.0),
    "XCH/USDS":     (MOJOS_PER_XCH, 1000, 1.0),
    "XCH/BYC":      (MOJOS_PER_XCH, 1000, 1.0),
    "XCH/DBX":      (MOJOS_PER_XCH, 1000, None),
    "BYC/wUSDC.b":  (1000,          1000, 1.0),
}

HEADER = [
    "timestamp_utc", "trade_id", "pair", "side",
    "price_pseudo_mojos", "size_base_mojos",
    "price_quote_per_base", "size_base_units", "quote_amount",
    "fee_xch_mojos", "cost_basis_pseudo_mojos",
    "realized_pnl_quote_mojos", "realized_pnl_usd", "block_height",
]


def main() -> int:
    if not DB_PATH.exists():
        print(f"ERROR: database not found at {DB_PATH}", file=sys.stderr)
        return 1

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    uri = f"file:{DB_PATH.as_posix()}?mode=ro"
    con = sqlite3.connect(uri, uri=True, timeout=10)
    cur = con.cursor()

    rows = cur.execute(
        """
        SELECT t.timestamp, t.trade_id, t.pair_name, t.side,
               t.price_mojos, t.size_mojos,
               COALESCE(NULLIF(t.fee_mojos, 0), o.fee_mojos, 0) AS fee_mojos,
               t.cost_basis_mojos, t.realized_pnl_mojos, t.block_height
        FROM trade_log t
        LEFT JOIN offer_log o ON o.offer_id = t.trade_id
        ORDER BY t.timestamp ASC, t.id ASC
        """
    ).fetchall()
    con.close()

    unknown_pairs: set[str] = set()
    written = 0

    with OUT_PATH.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(HEADER)

        for (ts, trade_id, pair, side, price, size,
             fee, basis, realized, block) in rows:
            price = int(price or 0)
            size = int(size or 0)
            realized = int(realized or 0)

            price_disp = ""
            size_disp = ""
            quote_amt_disp = ""
            pnl_usd_disp = ""

            info = PAIR_INFO.get(pair)
            if info is None:
                unknown_pairs.add(pair)
            else:
                base_denom, quote_denom, usd_per_quote = info
                if price < FORMAT_THRESHOLD:
                    price_units = price / quote_denom       # legacy encoding
                else:
                    price_units = price / MOJOS_PER_XCH     # pseudo encoding
                size_units = size / base_denom
                price_disp = f"{price_units:.12g}"
                size_disp = f"{size_units:.12g}"
                quote_amt_disp = f"{price_units * size_units:.12g}"
                if usd_per_quote is not None:
                    pnl_usd_disp = f"{realized / quote_denom * usd_per_quote:.12g}"

            writer.writerow([
                ts, trade_id, pair, side,
                price, size,
                price_disp, size_disp, quote_amt_disp,
                int(fee or 0), int(basis or 0),
                realized, pnl_usd_disp, int(block or 0),
            ])
            written += 1

    print(f"Wrote {written} rows to {OUT_PATH}")
    if unknown_pairs:
        print("WARNING: no denomination info for pairs (display columns "
              f"left blank): {sorted(unknown_pairs)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
