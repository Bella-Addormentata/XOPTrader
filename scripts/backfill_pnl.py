"""Backfill realized_pnl_mojos, cost_basis_mojos, and fee_mojos for historical trades.

Uses the weighted-average cost basis method (IAS 2 / ASC 330) to recompute
PnL for all trades in chronological order.  Fees are joined from offer_log.

This script:
  1. Backs up the database first (xop_trader.db.bak)
  2. Reads all trades ordered by timestamp
  3. Recomputes cost_basis and realized_pnl using weighted-average method
  4. Joins fee_mojos from offer_log
  5. UPDATEs all rows in trade_log within a single transaction
"""

import shutil
import sqlite3
from pathlib import Path

DB_PATH = Path(r"c:\GitHub\XOPTrader\data\xop_trader.db")
K = 1_000_000_000_000  # kMojosPerXch = 10^12


def main() -> None:
    # Safety backup
    backup = DB_PATH.with_suffix(".db.bak")
    shutil.copy2(DB_PATH, backup)
    print(f"Backup created: {backup}")

    conn = sqlite3.connect(str(DB_PATH))
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA busy_timeout=5000")
    c = conn.cursor()

    # Get all trades chronologically
    c.execute(
        "SELECT id, trade_id, side, price_mojos, size_mojos, "
        "       realized_pnl_mojos, cost_basis_mojos, fee_mojos "
        "FROM trade_log ORDER BY timestamp, id"
    )
    trades = c.fetchall()
    print(f"Found {len(trades)} trades to process")

    # Get fees from offer_log (keyed by offer_id = trade_id).
    # Note: backfilled offers may show as 'cancelled' in offer_log even
    # though they were filled (the backfill marked them resolved).
    c.execute("SELECT offer_id, fee_mojos FROM offer_log")
    offer_fees = dict(c.fetchall())
    print(f"Found {len(offer_fees)} filled offers with fee data")

    # Weighted-average cost basis recomputation
    inventory = 0       # mojos of base asset held
    cost_basis = 0.0    # weighted average cost per kMojosPerXch of base

    updates = []  # (cost_basis, realized_pnl, fee, id)

    for row_id, trade_id, side, price, size, old_rpnl, old_cb, old_fee in trades:
        # Look up fee from offer_log
        fee = offer_fees.get(trade_id, old_fee)

        if side == "bid":
            # Buy: update weighted-average cost basis
            old_cost_total = cost_basis * inventory
            new_cost = float(price) * float(size)
            inventory += size
            if inventory > 0:
                cost_basis = (old_cost_total + new_cost) / inventory
            realized_pnl = 0
        else:
            # Sell: realize PnL = (price - cost_basis) * size / K
            if cost_basis > 0 and inventory > 0:
                realized_pnl = round(
                    (price - cost_basis) * size / K
                )
            else:
                realized_pnl = 0
            inventory -= size

        # Record the integer cost basis (mojos per kMojosPerXch of base)
        cb_int = round(cost_basis)

        updates.append((cb_int, realized_pnl, fee, row_id))

    # Apply all updates in a single transaction
    conn.execute("BEGIN")
    c.executemany(
        "UPDATE trade_log SET cost_basis_mojos = ?, realized_pnl_mojos = ?, "
        "fee_mojos = ? WHERE id = ?",
        updates,
    )
    conn.commit()
    print(f"Updated {len(updates)} rows")

    # Verify
    c.execute(
        "SELECT "
        "  SUM(realized_pnl_mojos) AS total_rpnl, "
        "  SUM(fee_mojos) AS total_fees, "
        "  SUM(CASE WHEN cost_basis_mojos > 0 THEN 1 ELSE 0 END) AS rows_with_cb "
        "FROM trade_log"
    )
    rpnl, fees, cb_rows = c.fetchone()
    print()
    print("=== Post-Backfill Summary ===")
    print(f"Total realized PnL:       {rpnl:>15,} mojos  ({rpnl / K:.6f} XCH)")
    print(f"Total fees:               {fees:>15,} mojos  ({fees / K:.6f} XCH)")
    print(f"Net PnL (after fees):     {rpnl - fees:>15,} mojos  ({(rpnl - fees) / K:.6f} XCH)")
    print(f"Rows with cost basis > 0: {cb_rows}/{len(trades)}")
    print(f"Final inventory:          {inventory / K:.6f} XCH")

    # Show per-period breakdown
    c.execute(
        "SELECT DATE(timestamp) AS d, COUNT(*) AS n, "
        "  SUM(realized_pnl_mojos), SUM(fee_mojos) "
        "FROM trade_log GROUP BY d ORDER BY d"
    )
    print()
    print("--- Daily Breakdown ---")
    print(f"  {'Date':<12} {'Fills':>6} {'Realized PnL':>18} {'Fees':>15}")
    for date, count, d_rpnl, d_fees in c.fetchall():
        print(f"  {date:<12} {count:>6} {d_rpnl:>18,} {d_fees:>15,}")

    conn.close()
    print(f"\nDone. Backup at: {backup}")


if __name__ == "__main__":
    main()
