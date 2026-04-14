#!/usr/bin/env python3
"""Summarize offer_closure_events for operator analytics pivots.

Examples:
  python scripts/report_offer_closure_events.py
  python scripts/report_offer_closure_events.py --pair XCH/DBX --top 15
  python scripts/report_offer_closure_events.py --since-block 8120000 --recent 25
"""

from __future__ import annotations

import argparse
import sqlite3
import sys
from pathlib import Path
from typing import Sequence


DEFAULT_DB_PATH = Path(__file__).resolve().parents[1] / "data" / "xop_trader.db"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--db",
        type=Path,
        default=DEFAULT_DB_PATH,
        help=f"SQLite database path (default: {DEFAULT_DB_PATH})",
    )
    parser.add_argument(
        "--pair",
        default="",
        help="Optional pair_name filter (for example: XCH/DBX)",
    )
    parser.add_argument(
        "--since-block",
        type=int,
        default=None,
        help="Only include events with resolved_block >= this value",
    )
    parser.add_argument(
        "--until-block",
        type=int,
        default=None,
        help="Only include events with resolved_block <= this value",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=10,
        help="Maximum rows to show in grouped sections (default: 10)",
    )
    parser.add_argument(
        "--recent",
        type=int,
        default=15,
        help="Maximum rows to show in the recent-events section (default: 15)",
    )
    return parser.parse_args(argv)


def open_db(path: Path) -> sqlite3.Connection:
    uri = f"file:{path.resolve()}?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.row_factory = sqlite3.Row
    return conn


def require_table(conn: sqlite3.Connection, table_name: str) -> None:
    row = conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1",
        [table_name],
    ).fetchone()
    if row is None:
        raise RuntimeError(
            f"Database does not contain '{table_name}'. Run the updated trader binary "
            "or tests to apply the latest migrations first."
        )


def build_filters(args: argparse.Namespace) -> tuple[list[str], list[object]]:
    clauses: list[str] = []
    params: list[object] = []
    if args.pair:
        clauses.append("pair_name = ?")
        params.append(args.pair)
    if args.since_block is not None:
        clauses.append("COALESCE(resolved_block, 0) >= ?")
        params.append(args.since_block)
    if args.until_block is not None:
        clauses.append("COALESCE(resolved_block, 0) <= ?")
        params.append(args.until_block)

    return clauses, params


def render_where(clauses: Sequence[str]) -> str:
    return f"WHERE {' AND '.join(clauses)}" if clauses else ""


def query_rows(
    conn: sqlite3.Connection,
    sql: str,
    params: Sequence[object],
) -> list[sqlite3.Row]:
    return conn.execute(sql, params).fetchall()


def print_section(title: str) -> None:
    print()
    print(title)
    print("-" * len(title))


def stringify(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def print_table(rows: Sequence[sqlite3.Row], columns: Sequence[tuple[str, str]]) -> None:
    if not rows:
        print("(no rows)")
        return

    widths = [len(header) for _, header in columns]
    rendered_rows: list[list[str]] = []
    for row in rows:
        rendered = [stringify(row[key]) for key, _ in columns]
        rendered_rows.append(rendered)
        for index, value in enumerate(rendered):
            widths[index] = max(widths[index], len(value))

    header = "  ".join(
        header.ljust(widths[index]) for index, (_, header) in enumerate(columns)
    )
    divider = "  ".join("-" * width for width in widths)
    print(header)
    print(divider)
    for rendered in rendered_rows:
        print(
            "  ".join(
                value.ljust(widths[index]) for index, value in enumerate(rendered)
            )
        )


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    db_path = args.db.resolve()
    if not db_path.exists():
        print(f"Database not found: {db_path}", file=sys.stderr)
        return 1

    try:
        conn = open_db(db_path)
    except sqlite3.Error as exc:
        print(f"Failed to open database: {exc}", file=sys.stderr)
        return 1

    try:
        require_table(conn, "offer_closure_events")
        filter_clauses, params = build_filters(args)
        where = render_where(filter_clauses)

        summary_sql = f"""
            SELECT
                COUNT(*) AS total_events,
                COUNT(DISTINCT offer_id) AS distinct_offers,
                COALESCE(MIN(resolved_block), 0) AS first_block,
                COALESCE(MAX(resolved_block), 0) AS last_block,
                COALESCE(MIN(created_at), '') AS first_seen,
                COALESCE(MAX(created_at), '') AS last_seen
            FROM offer_closure_events
            {where}
        """
        summary = conn.execute(summary_sql, params).fetchone()
        if summary is None or summary["total_events"] == 0:
            print("No offer_closure_events matched the requested filters.")
            return 0

        print("XOPTrader Offer Closure Events Report")
        print("====================================")
        print(f"Database: {db_path}")
        filters = []
        if args.pair:
            filters.append(f"pair={args.pair}")
        if args.since_block is not None:
            filters.append(f"since_block={args.since_block}")
        if args.until_block is not None:
            filters.append(f"until_block={args.until_block}")
        print(f"Filters: {'; '.join(filters) if filters else 'none'}")
        print(
            "Window: "
            f"blocks {summary['first_block']} -> {summary['last_block']} | "
            f"events {summary['total_events']} across {summary['distinct_offers']} offers"
        )
        print(f"Seen:   {summary['first_seen']} -> {summary['last_seen']}")

        breakdown_sql = f"""
            SELECT
                event_type,
                observed_status,
                COUNT(*) AS events,
                COUNT(DISTINCT offer_id) AS offers
            FROM offer_closure_events
            {where}
            GROUP BY event_type, observed_status
            ORDER BY events DESC, event_type, observed_status
        """
        print_section("Event Breakdown")
        print_table(
            query_rows(conn, breakdown_sql, params),
            [
                ("event_type", "event_type"),
                ("observed_status", "status"),
                ("events", "events"),
                ("offers", "offers"),
            ],
        )

        primary_where = render_where(
            [
                "event_type = 'status_update'",
                "observed_status IN ('cancelled', 'expired')",
                "COALESCE(closure_reason, '') <> ''",
                *filter_clauses,
            ]
        )
        primary_sql = f"""
            SELECT
                pair_name,
                closure_reason,
                COUNT(*) AS events,
                COUNT(DISTINCT offer_id) AS offers,
                COALESCE(MIN(resolved_block), 0) AS first_block,
                COALESCE(MAX(resolved_block), 0) AS last_block
            FROM offer_closure_events
            {primary_where}
            GROUP BY pair_name, closure_reason
            ORDER BY events DESC, pair_name, closure_reason
            LIMIT ?
        """
        print_section("Primary Close Causes")
        print_table(
            query_rows(conn, primary_sql, [*params, max(1, args.top)]),
            [
                ("pair_name", "pair"),
                ("closure_reason", "closure_reason"),
                ("events", "events"),
                ("offers", "offers"),
                ("first_block", "first_block"),
                ("last_block", "last_block"),
            ],
        )

        reconcile_where = render_where(
            [
                "event_type = 'reconcile_observation'",
                "COALESCE(closure_reason, '') <> ''",
                *filter_clauses,
            ]
        )
        reconcile_sql = f"""
            SELECT
                pair_name,
                closure_reason,
                COUNT(*) AS events,
                COUNT(DISTINCT offer_id) AS offers,
                COALESCE(MIN(resolved_block), 0) AS first_block,
                COALESCE(MAX(resolved_block), 0) AS last_block
            FROM offer_closure_events
            {reconcile_where}
            GROUP BY pair_name, closure_reason
            ORDER BY events DESC, pair_name, closure_reason
            LIMIT ?
        """
        print_section("Reconcile Follow-Ups")
        print_table(
            query_rows(conn, reconcile_sql, [*params, max(1, args.top)]),
            [
                ("pair_name", "pair"),
                ("closure_reason", "closure_reason"),
                ("events", "events"),
                ("offers", "offers"),
                ("first_block", "first_block"),
                ("last_block", "last_block"),
            ],
        )

        recent_sql = f"""
            SELECT
                created_at,
                pair_name,
                substr(offer_id, 1, 16) AS offer_prefix,
                event_type,
                previous_status,
                observed_status,
                COALESCE(closure_reason, '') AS closure_reason,
                COALESCE(resolved_block, 0) AS resolved_block
            FROM offer_closure_events
            {where}
            ORDER BY id DESC
            LIMIT ?
        """
        print_section("Recent Events")
        print_table(
            query_rows(conn, recent_sql, [*params, max(1, args.recent)]),
            [
                ("created_at", "created_at"),
                ("pair_name", "pair"),
                ("offer_prefix", "offer_id"),
                ("event_type", "event_type"),
                ("previous_status", "previous"),
                ("observed_status", "observed"),
                ("closure_reason", "closure_reason"),
                ("resolved_block", "block"),
            ],
        )
    except (RuntimeError, sqlite3.Error) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    finally:
        conn.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))