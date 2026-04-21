"""
compute_actual_pnl.py
=====================

Reconstructs the bot's true historical P&L from `trade_log` entries, bypassing
the corrupted `realized_pnl_mojos` column (cost basis was never tracked --
all historical fills were written with cost_basis_mojos in {0, 1}).

Two views are produced:

1. **Cash-flow PnL** -- pure (quote received from asks) - (quote paid for bids).
   Treated as the realised cash gain on closed cycles plus open cycles.

2. **Inventory-marked PnL** -- cash-flow + (net base inventory change valued at
   the most recent traded price).  Approximates what the bot would realise if
   it sold/bought the residual inventory at the current market.

The script is robust to the mid-life schema change in price_mojos:
  * legacy:  price_mojos = real quote_mojos per 1 base UNIT       (XCH)
  * current: price_mojos = quote_units_per_base_unit * 1e12      (pseudo-mojos)
A magnitude threshold of 1e9 cleanly separates the two.

Usage:
    python scripts/compute_actual_pnl.py [path/to/xop_trader.db]
"""

from __future__ import annotations

import collections
import sqlite3
import sys
from decimal import Decimal as D
from pathlib import Path

K = D(10) ** 12

ASSET_DENOM = {
    "XCH":     D(10) ** 12,
    "wUSDC.b": D(1000),
    "wUSDC":   D(1000),  # legacy pair-name w/o .b -- same asset
    "BYC":     D(1000),
    "DBX":     D(1000),
    "USDS":    D(1000),
}
USD_PER_UNIT = {
    "wUSDC.b": D(1),
    "wUSDC":   D(1),
    "BYC":     D(1),  # CrossChain BYC -- treated as USD-pegged
    "USDS":    D(1),
    "DBX":     None,  # not USD-pegged; flagged in output
}

FORMAT_THRESHOLD = D(10) ** 9  # below = legacy price encoding, above = new


def main(db_path: str) -> int:
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        "SELECT timestamp, pair_name, side, price_mojos, size_mojos, fee_mojos "
        "FROM trade_log ORDER BY timestamp ASC, id ASC"
    ).fetchall()

    stats = collections.defaultdict(lambda: {
        "bid_base":   D(0), "bid_quote": D(0),
        "ask_base":   D(0), "ask_quote": D(0),
        "bid_n": 0, "ask_n": 0,
        "old_format": 0, "new_format": 0,
    })

    for r in rows:
        pair = r["pair_name"]
        if "/" not in pair:
            continue
        base, quote = pair.split("/", 1)
        base_d = ASSET_DENOM.get(base)
        quote_d = ASSET_DENOM.get(quote)
        if not base_d or not quote_d:
            continue

        pm = D(r["price_mojos"])
        sm = D(r["size_mojos"])
        s = stats[pair]
        if pm < FORMAT_THRESHOLD:
            real_price = pm / quote_d           # quote_units per base_unit
            s["old_format"] += 1
        else:
            real_price = pm / K                 # quote_units per base_unit
            s["new_format"] += 1
        size_b = sm / base_d
        size_q = size_b * real_price

        if r["side"].lower() == "bid":
            s["bid_base"] += size_b
            s["bid_quote"] += size_q
            s["bid_n"] += 1
        else:
            s["ask_base"] += size_b
            s["ask_quote"] += size_q
            s["ask_n"] += 1

    print("CASH-FLOW PnL (legacy + current price formats auto-detected)\n")
    header = (
        f"{'pair':<14} {'bids':>5} {'asks':>5} {'old':>4} {'new':>4} "
        f"{'base_bought':>12} {'base_sold':>12} {'net_base':>10} "
        f"{'quote_paid':>12} {'quote_recv':>12} {'cash_pnl_q':>11} {'~USD':>10}"
    )
    print(header)
    print("-" * len(header))

    total_usd = D(0)
    total_xch_b = D(0)
    total_xch_s = D(0)
    for pair in sorted(stats.keys()):
        s = stats[pair]
        base, quote = pair.split("/", 1)
        cf = s["ask_quote"] - s["bid_quote"]
        upu = USD_PER_UNIT.get(quote)
        if upu is not None:
            usd = cf * upu
            usd_str = f"${float(usd):,.2f}"
            total_usd += usd
        else:
            usd_str = "(?)"
        if base == "XCH":
            total_xch_b += s["bid_base"]
            total_xch_s += s["ask_base"]
        print(
            f"{pair:<14} {s['bid_n']:>5} {s['ask_n']:>5} "
            f"{s['old_format']:>4} {s['new_format']:>4} "
            f"{float(s['bid_base']):>12.4f} {float(s['ask_base']):>12.4f} "
            f"{float(s['bid_base'] - s['ask_base']):>10.4f} "
            f"{float(s['bid_quote']):>12.4f} {float(s['ask_quote']):>12.4f} "
            f"{float(cf):>11.4f} {usd_str:>10}"
        )

    # XCH/USD price proxy = most recent XCH/wUSDC* fill (any side).
    xch_proxy = None
    for r in reversed(rows):
        if r["pair_name"] in ("XCH/wUSDC.b", "XCH/wUSDC"):
            pm = D(r["price_mojos"])
            xch_proxy = pm / K if pm >= FORMAT_THRESHOLD else pm / D(1000)
            break

    net_xch = total_xch_b - total_xch_s
    print()
    print(
        f"NET XCH FLOW : bought {float(total_xch_b):.4f}  "
        f"sold {float(total_xch_s):.4f}  net {float(net_xch):+.4f} XCH"
    )

    fee_mojos = conn.execute("SELECT SUM(fee_mojos) FROM offer_log").fetchone()[0] or 0
    fee_xch = D(fee_mojos) / K

    print("\n=== SUMMARY ===")
    print(f"Cash-flow PnL (USDC-equiv, BYC=$1, DBX excluded): ${float(total_usd):,.2f}")
    if xch_proxy:
        inv_usd = net_xch * xch_proxy
        fee_usd = fee_xch * xch_proxy
        print(f"Last XCH/USDC price proxy used                  : "
              f"${float(xch_proxy):.4f}/XCH")
        print(f"Net XCH inventory marked at proxy price         : "
              f"${float(inv_usd):,.2f}")
        print(f"Total realized + inventory PnL                  : "
              f"${float(total_usd + inv_usd):,.2f}")
        print(f"All on-chain fees (offer_log)                   : "
              f"{float(fee_xch):.6f} XCH = ${float(fee_usd):,.2f}")
        print(f"Net of fees                                     : "
              f"${float(total_usd + inv_usd - fee_usd):,.2f}")
    else:
        print(f"All on-chain fees (offer_log)                   : "
              f"{float(fee_xch):.6f} XCH (no XCH price proxy available)")
    print()
    print("NOTE: DBX is not USD-pegged; XCH/DBX flow is shown but not summed.")
    print("NOTE: cost_basis_mojos was sentineled in every historical fill, so the")
    print("      stored realized_pnl_mojos column is unusable.  The cash-flow view")
    print("      above is the most reliable reconstruction available.")
    return 0


if __name__ == "__main__":
    db = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).resolve().parent.parent / "data" / "xop_trader.db"
    )
    sys.exit(main(db))
