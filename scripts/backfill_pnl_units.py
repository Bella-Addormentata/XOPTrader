"""Backfill ``trade_log.realized_pnl_mojos`` after the PnL unit-conversion fix.

Background
----------
Prior to the [PNL-UNIT-FIX] in ``cpp/src/engine.cpp`` the engine computed::

    realized_pnl_mojos = (fill.price - cost_basis) * fill.size / kMojosPerXch

That formula omitted the ``quote_mojos_per_unit / base_mojos_per_unit``
dimensional factor.  For CAT-quoted pairs such as ``XCH/wUSDC.b`` the result
was therefore inflated by ``kMojosPerXch / 1e3 = 1e9`` -- the "billions of
dollars" that showed up in the GUI.

This script rewrites ``trade_log.realized_pnl_mojos`` to the correct
quote-asset mojo value, using each fill's pair-specific denominations.  The
``cost_basis_mojos == 0`` / ``cost_basis_mojos == 1`` startup-sentinel rule
is preserved (those fills are forced to PnL = 0).

The script makes a backup copy of the database before mutating it and only
updates rows whose new value differs from the stored one.
"""

from __future__ import annotations

import argparse
import shutil
import sqlite3
import sys
from datetime import datetime
from pathlib import Path

K_MOJOS_PER_XCH = 1_000_000_000_000  # 1e12

# Per-asset mojos-per-unit; matches ``parse_pairs()`` auto-detection in C++
# (``XCH`` -> 1e12, all CAT tokens -> 1e3).
_ASSET_DENOM = {
    "XCH": K_MOJOS_PER_XCH,
    "wUSDC.b": 1_000,
    "wUSDC": 1_000,
    "BYC": 1_000,
    "DBX": 1_000,
    "USDS": 1_000,
}


def _denoms_for(pair_name: str) -> tuple[int, int] | None:
    """Return ``(base_denom, quote_denom)`` for a ``BASE/QUOTE`` pair."""
    if "/" not in pair_name:
        return None
    base_sym, quote_sym = pair_name.split("/", 1)
    base = _ASSET_DENOM.get(base_sym)
    quote = _ASSET_DENOM.get(quote_sym)
    if base is None or quote is None:
        return None
    return base, quote


def _correct_pnl(
    side: str,
    price_mojos: int,
    size_mojos: int,
    cost_basis_mojos: int | None,
    base_denom: int,
    quote_denom: int,
) -> int:
    """Recompute realized PnL in quote-asset mojos for one fill."""
    if side.lower() != "ask":
        return 0
    if cost_basis_mojos is None or cost_basis_mojos <= 1:
        # Startup sentinel: cost basis unknown -> realized PnL undefined.
        return 0
    diff = float(price_mojos) - float(cost_basis_mojos)
    pnl = (
        diff
        * float(size_mojos)
        * float(quote_denom)
        / (float(base_denom) * float(K_MOJOS_PER_XCH))
    )
    return int(round(pnl))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--db",
        default=r"c:\GitHub\XOPTrader\data\xop_trader.db",
        help="Path to the SQLite database",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Report changes without writing to the database.",
    )
    parser.add_argument(
        "--no-backup",
        action="store_true",
        help="Skip the timestamped backup copy (not recommended).",
    )
    args = parser.parse_args()

    db_path = Path(args.db)
    if not db_path.exists():
        print(f"ERROR: database not found: {db_path}", file=sys.stderr)
        return 2

    if not args.dry_run and not args.no_backup:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        backup = db_path.with_suffix(db_path.suffix + f".pnlfix.{ts}.bak")
        shutil.copy2(db_path, backup)
        print(f"Backup written: {backup}")

    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()

    rows = cur.execute(
        """
        SELECT id, pair_name, side, price_mojos, size_mojos,
               cost_basis_mojos, realized_pnl_mojos
        FROM trade_log
        """
    ).fetchall()

    updates: list[tuple[int, int]] = []
    skipped_unknown_pair: dict[str, int] = {}
    summary_by_pair: dict[str, dict[str, int]] = {}

    for r in rows:
        denoms = _denoms_for(r["pair_name"])
        if denoms is None:
            skipped_unknown_pair[r["pair_name"]] = skipped_unknown_pair.get(
                r["pair_name"], 0
            ) + 1
            continue
        base_denom, quote_denom = denoms

        new_pnl = _correct_pnl(
            r["side"],
            int(r["price_mojos"]),
            int(r["size_mojos"]),
            int(r["cost_basis_mojos"])
            if r["cost_basis_mojos"] is not None
            else None,
            base_denom,
            quote_denom,
        )
        old_pnl = int(r["realized_pnl_mojos"] or 0)

        bucket = summary_by_pair.setdefault(
            r["pair_name"], {"old_sum": 0, "new_sum": 0, "rows": 0}
        )
        bucket["old_sum"] += old_pnl
        bucket["new_sum"] += new_pnl
        bucket["rows"] += 1

        if new_pnl != old_pnl:
            updates.append((new_pnl, r["id"]))

    print(f"Total trade_log rows scanned : {len(rows)}")
    print(f"Rows queued for update       : {len(updates)}")
    if skipped_unknown_pair:
        print("Skipped (unknown pair name):")
        for k, v in sorted(skipped_unknown_pair.items()):
            print(f"  {k}: {v} row(s)")

    print()
    print(
        f"{'pair':<14} {'rows':>5} {'old_sum (raw)':>22} "
        f"{'new_sum (qmojos)':>22}"
    )
    for pair, info in sorted(summary_by_pair.items()):
        print(
            f"{pair:<14} {info['rows']:>5} "
            f"{info['old_sum']:>22} {info['new_sum']:>22}"
        )

    if args.dry_run:
        print("\n(dry-run -- no changes written)")
        conn.close()
        return 0

    if updates:
        cur.executemany(
            "UPDATE trade_log SET realized_pnl_mojos = ? WHERE id = ?",
            updates,
        )
        conn.commit()
        print(f"\nUpdated {len(updates)} rows.")
    else:
        print("\nNo updates required.")

    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
