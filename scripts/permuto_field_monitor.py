"""Watch the whole market-maker field bank depth while we bank none.

WHY. On 2026-09-02 our account held depth_seconds_5d = 0 for an entire
session while the leader sat at 634.5M. The BBO diagnosis says the ask side
is unplaceable: competitors' bids rest at the +2% ring ceiling, and credit is
min(bid, ask). That explains OUR zero. It does NOT explain how anyone else is
earning against the same book -- and that gap is the one remaining question
worth measuring overnight.

So this records, every poll: our depth, every other maker's depth, and the
per-minute rate for each. If the field is also flat, the wall is universal
and the contest is frozen. If the field keeps climbing while asks are
impossible, they are earning by some route we have not identified, and the
deltas plus the concurrent L2 snapshot are the evidence needed to find it.

Read-only and unauthenticated: GET /exchange/leaderboard and GET /info/l2.
Places no orders and holds no session.

    python permuto_field_monitor.py --user <id> [--hours 18] [--interval 300]

Appends JSONL to ../data/field_monitor.jsonl
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui.services.permuto.bbo import (
    DEFAULT_RING_PCT,
    Book,
    active_ring_pct,
    earning_window,
)

BASE = "https://perps.permuto.capital"
HEADERS = {"User-Agent": "Mozilla/5.0 (XOPTrader)", "Accept": "application/json"}
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "data", "field_monitor.jsonl")
MARKETS = ("NVDA-VOL-PERP", "QQQ-VOL-PERP", "TSLA-VOL-PERP")


def get(path, timeout=25):
    req = urllib.request.Request(BASE + path, headers=HEADERS, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def fetch_field():
    """Every market maker, paging past the default limit of 20."""
    rows, offset, page = [], 0, {}
    while True:
        page = get("/exchange/leaderboard?offset=%d&limit=20" % offset)
        mm = page.get("market_makers") or []
        rows.extend(mm)
        raw_total = page.get("market_makers_total")
        total = raw_total if isinstance(raw_total, int) and raw_total > 0 else 0
        offset += 20
        if len(mm) < 20 or (total > 0 and len(rows) >= total):
            return rows, page


def ring_state():
    """Is an ask placeable at all, per market? The reason we bank nothing."""
    out = {}
    try:
        prices = (get("/info/oracle") or {}).get("prices") or {}
        meta = get("/info/meta") or {}
        ring_pct, ring_source = active_ring_pct(meta)
        specs = {}
        for entry in (meta.get("markets") or []):
            if isinstance(entry, dict) and entry.get("symbol"):
                try:
                    specs[entry["symbol"]] = float(entry.get("tick_size") or 0.0001)
                except (TypeError, ValueError):
                    pass
    except Exception as exc:  # noqa: BLE001
        return {"error": str(exc)[:120]}
    for market in MARKETS:
        try:
            oracle = float(prices[market.replace("-PERP", "")])
            book_raw = get("/info/l2/%s?levels=1" % market)
            bids = book_raw.get("bids") or []
            asks = book_raw.get("asks") or []
            best_bid = float(bids[0]["price"]) if bids else None
            best_ask = float(asks[0]["price"]) if asks else None
            tick_size = specs.get(market, 0.0001)
            book_obj = Book(market=market, best_bid=best_bid, best_ask=best_ask)
            window = earning_window("ask", oracle, book_obj, ring_pct=ring_pct, tick_size=tick_size)
            ring_hi = oracle * (1.0 + ring_pct / 100.0)
            out[market] = {
                "oracle": oracle,
                "best_bid": best_bid,
                "best_ask": best_ask,
                "ring_hi": ring_hi,
                "ring_pct": ring_pct,
                "ring_source": ring_source,
                "tick_size": tick_size,
                "ask_ticks": window.ticks,
            }
        except Exception as exc:  # noqa: BLE001
            out[market] = {"error": str(exc)[:120]}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--user", required=True)
    ap.add_argument("--hours", type=float, default=18.0)
    ap.add_argument("--interval", type=float, default=300.0)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    deadline = time.time() + args.hours * 3600.0
    prev = {}
    prev_t = None

    with open(OUT, "a", encoding="utf-8") as fh:
        fh.write(json.dumps({"field_monitor_started": time.time(),
                             "user": args.user,
                             "interval_s": args.interval}) + "\n")
        fh.flush()
        while time.time() < deadline:
            row = {"t": time.time(),
                   "iso": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
            try:
                rows, page = fetch_field()
                now_t = row["t"]
                dt_min = ((now_t - prev_t) / 60.0) if prev_t else None

                movers, entries, field_total, ours = [], [], 0.0, None
                for r in rows:
                    uid = str(r.get("user_id", ""))
                    d5 = float(r.get("depth_seconds_5d") or 0.0)
                    field_total += d5
                    rate = None
                    if dt_min and uid in prev and dt_min > 0:
                        rate = round((d5 - prev[uid]) / dt_min, 1)
                    entry = {"user": uid[:12], "depth_5d": d5,
                             "per_min": rate,
                             "eligible": bool(r.get("prize_eligible"))}
                    entries.append(entry)
                    if uid.startswith(args.user):
                        ours = entry
                    if rate:
                        movers.append(entry)
                    prev[uid] = d5

                movers.sort(key=lambda e: -(e["per_min"] or 0.0))
                row["ours"] = ours
                row["field_total_depth"] = field_total
                row["makers_gaining"] = len([m for m in movers
                                             if (m["per_min"] or 0) > 0])
                row["top_gainers"] = movers[:5]
                row["makers"] = entries
                row["eligible_count"] = len(
                    [r for r in rows if r.get("prize_eligible")])
                row["rebuild_status"] = page.get("leaderboard_rebuild_status")
                row["ring"] = ring_state()
                prev_t = now_t
            except Exception as exc:  # noqa: BLE001 - never die on one poll
                row["error"] = str(exc)[:200]
            fh.write(json.dumps(row) + "\n")
            fh.flush()
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
