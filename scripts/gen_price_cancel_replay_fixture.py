"""[S72 2026-09-20] Freeze recorded offers into the margin-rule replay fixture.

Reads the engine database STRICTLY READ-ONLY (sqlite URI mode=ro) and writes
cpp/tests/price_cancel_replay_fixture.inc, which
cpp/tests/test_price_cancel_replay.cpp replays through the real predicate
(execution::classify_tier_refresh_margin).  Nothing here decides anything: it
selects rows and copies integers.  Every proxy, threshold and count lives in
the C++ test, where it is visible and pinned.

WHAT IS RECORDED, AND WHAT IS NOT.  The margin rule judges a resting offer
against Step 7's ladder CENTRE and minimum half-spread FLOOR.  Neither is
persisted.  What the engine does persist, once per block in the same batch as
`snapshots`, is Step 7's OUTPUT: every ladder tier's price (strategy_quotes).
So for each block the fixture keeps the innermost bid and the innermost ask of
that ladder, and the test reconstructs

    centre' = (inner_bid + inner_ask) / 2
    floor'  = (inner_ask - inner_bid) / (2 centre')

`snapshots.mid_price_mojos` is deliberately NOT used as the centre: it is the
published book mid, and Step 7 quotes around a blended, inventory-shifted
centre that sat 267 bps away from it on XCH/DBX on 2026-09-14.

For each offer two witnesses are kept, as (inner_bid, inner_ask) in mojos:

  * SUBMIT  -- the ladder at the block the engine submitted the cancel (the
    first cancel_pending closure event, else the row's resolved_block).  This
    answers "would the margin rule have made THIS cancel?".
  * WORST   -- over the offer's recorded life from kMinRefreshAgeBlocks on,
    the block where edge / floor' was lowest.  The rule fires at SOME block of
    that life at retain r iff it fires at this one, so one row answers every r.

Usage:
    python scripts/gen_price_cancel_replay_fixture.py \
        --db C:/GitHub/XOPTrader/data/xop_trader.db [--hi-block N] [--days 14]
"""
from __future__ import annotations

import argparse
import sqlite3
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "cpp" / "tests" / "price_cancel_replay_fixture.inc"

BLOCKS_PER_DAY = 4608        # peak-height cadence, 18.75 s per block
MIN_REFRESH_AGE_BLOCKS = 12  # OfferManager::kMinRefreshAgeBlocks
MAX_LADDER_LAG_BLOCKS = 20   # a SUBMIT witness older than this is not one


def cause_of(status: str, reason: str | None) -> str:
    text = reason or ""
    if status == "filled":
        return "Filled"
    if text.startswith("price_adverse"):
        return "PriceAdverse"
    if text == "ttl_expired":
        return "TtlExpired"
    if text == "exposure_floor_rebalance":
        return "ExposureFloor"
    return "Other"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", required=True)
    ap.add_argument("--hi-block", type=int, default=0,
                    help="last created_block of the window (default: newest)")
    ap.add_argument("--days", type=int, default=14)
    args = ap.parse_args()

    uri = "file:" + Path(args.db).as_posix() + "?mode=ro"
    con = sqlite3.connect(uri, uri=True, timeout=60)
    cur = con.cursor()
    hi = args.hi_block or cur.execute(
        "select max(created_block) from offer_log").fetchone()[0]
    lo = hi - args.days * BLOCKS_PER_DAY

    inner: dict[str, dict[int, list[int]]] = defaultdict(dict)
    for pair, blk, side, px in cur.execute(
            "select pair_name, block_height, side, price_mojos "
            "from strategy_quotes where block_height between ? and ?",
            (lo - MAX_LADDER_LAG_BLOCKS, hi + 2 * BLOCKS_PER_DAY)):
        if px is None or px <= 0:
            continue
        slot = inner[pair].setdefault(blk, [0, 0])
        if side == "bid":
            slot[0] = max(slot[0], px)
        else:
            slot[1] = px if slot[1] == 0 else min(slot[1], px)

    submit_block: dict[str, int] = {}
    for oid, blk in cur.execute(
            "select offer_id, min(resolved_block) from offer_closure_events "
            "where observed_status='cancel_pending' and resolved_block > 0 "
            "group by offer_id"):
        submit_block[oid] = blk

    offers = cur.execute(
        "select offer_id, pair_name, side, price_mojos, created_block, "
        "resolved_block, status, cancel_reason from offer_log "
        "where created_block between ? and ? order by id", (lo, hi)).fetchall()
    newest = cur.execute("select max(block_height) from snapshots").fetchone()[0]
    con.close()

    rows: list[str] = []
    counts: dict[str, int] = defaultdict(int)
    for oid, pair, side, px, cb, rb, status, reason in offers:
        cause = cause_of(status, reason)
        counts[cause] += 1
        end = submit_block.get(oid) or rb or newest
        if not end or end < cb:
            end = newest
        ladder = inner.get(pair, {})
        is_ask = side == "ask"

        sub = (0, 0)
        for b in range(end, end - MAX_LADDER_LAG_BLOCKS - 1, -1):
            slot = ladder.get(b)
            if slot and slot[0] > 0 and slot[1] > slot[0]:
                sub = (slot[0], slot[1])
                break

        worst = (0, 0)
        worst_ratio = None
        for b in range(cb + MIN_REFRESH_AGE_BLOCKS, end + 1):
            slot = ladder.get(b)
            if not slot or slot[0] <= 0 or slot[1] <= slot[0]:
                continue
            centre = (slot[0] + slot[1]) / 2.0
            half = (slot[1] - slot[0]) / 2.0
            edge = (px - centre) if is_ask else (centre - px)
            ratio = edge / half
            if worst_ratio is None or ratio < worst_ratio:
                worst_ratio = ratio
                worst = (slot[0], slot[1])

        rows.append("    {Cause::%s, %s, %dLL, %dLL, %dLL, %dLL, %dLL}," % (
            cause, "true" if is_ask else "false", px,
            sub[0], sub[1], worst[0], worst[1]))

    header = [
        "// GENERATED by scripts/gen_price_cancel_replay_fixture.py -- do not edit.",
        "// Source: offer_log, offer_closure_events and strategy_quotes of the live",
        "// engine database, read with sqlite mode=ro.",
        "// Window: offers created in blocks [%d, %d] (%d days at %d blocks/day)."
        % (lo, hi, args.days, BLOCKS_PER_DAY),
        "// Rows: %d.  By recorded outcome: %s." % (
            len(rows), ", ".join("%s %d" % kv for kv in sorted(counts.items()))),
        "// Columns: cause, is_ask, resting price, SUBMIT inner_bid, SUBMIT inner_ask,",
        "//          WORST inner_bid, WORST inner_ask  (mojos; 0,0 = no ladder witness).",
        "// clang-format off",
    ]
    OUT.write_text("\n".join(header + rows) + "\n", encoding="utf-8", newline="\n")
    print("wrote", OUT, "rows", len(rows), dict(counts), "window", lo, hi)


if __name__ == "__main__":
    main()
