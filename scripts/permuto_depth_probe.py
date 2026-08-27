#!/usr/bin/env python3
"""Sample Permuto's public MM leaderboard across the equity cash close.

Question being settled (C-0S3 in TODO-COMPETITION.md): do *carried*
(out-of-hours) ticks accrue depth_seconds? An entrant says yes; the sponsor
has not said either way, and our whole eligibility plan rests on it.

Why sampling works despite the rolling windows. `depth_seconds_5d` and
`depth_seconds_24h` are trailing-window accumulators, so

    d(depth)/dt = accrual_now - accrual_at_window_edge

and neither series is pure accrual. But the two candidate explanations leave
very different fingerprints at the 20:00 UTC close:

  - If carried ticks earn nothing, every quoting account's accrual drops to
    zero at the same instant. Sharp, simultaneous, universal.
  - Window roll-off is smooth, gradual, and specific to each account's
    history five days / 24 hours ago.

So we do not need to disentangle the window: we only need the shape of the
change at the boundary. Sampling every 60s from before the close to well
after it makes that shape visible.

The oracle is sampled alongside, because the frozen-price transition marks
the carried boundary far more precisely than the wall clock does.

Read-only: two unauthenticated GETs per minute.
"""

import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

BASE = "https://perps.permuto.capital"
HEADERS = {"User-Agent": "Mozilla/5.0", "Accept": "application/json"}
INTERVAL_S = 60

# Late August is EDT, UTC-4, so the 16:00 ET cash close is 20:00 UTC.
# Recorded rather than computed: this box has no tzdata, and a hardcoded
# offset that is written down beats a silent wrong conversion.
CLOSE_UTC_HINT = "20:00 UTC == 16:00 ET (EDT, UTC-4)"


def get(path, timeout=20):
    req = urllib.request.Request(BASE + path, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def sample():
    """One observation. Returns a dict; never raises."""
    row = {"ts": datetime.now(timezone.utc).isoformat()}
    try:
        lb = get("/exchange/leaderboard?limit=100")
        row["mm_total"] = lb.get("market_makers_total")
        row["finalized"] = lb.get("finalized")
        row["mms"] = [
            {
                "u": m["user_id"][:8],
                "d5": m.get("depth_seconds_5d"),
                "d24": m.get("depth_seconds_24h"),
                "eq": float(m.get("equity") or 0),
                "pnl": float(m.get("total_pnl") or 0),
                "elig": m.get("prize_eligible"),
                "trades": m.get("trade_count"),
            }
            for m in lb.get("market_makers", [])
        ]
    except Exception as e:  # noqa: BLE001 - a bad sample must not end the run
        row["lb_error"] = "%s: %s" % (type(e).__name__, e)
    try:
        row["oracle"] = get("/info/oracle").get("prices")
    except Exception as e:  # noqa: BLE001
        row["oracle_error"] = "%s: %s" % (type(e).__name__, e)
    return row


def main():
    out_path = sys.argv[1]
    stop_at = sys.argv[2]  # ISO8601 UTC, e.g. 2026-08-27T22:30:00+00:00
    stop = datetime.fromisoformat(stop_at)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps({"probe_start": datetime.now(timezone.utc).isoformat(),
                             "interval_s": INTERVAL_S,
                             "stop_at": stop_at,
                             "close_note": CLOSE_UTC_HINT}) + "\n")
        fh.flush()

        n = 0
        while datetime.now(timezone.utc) < stop:
            row = sample()
            fh.write(json.dumps(row) + "\n")
            fh.flush()
            n += 1
            if n % 10 == 0:
                print("%s  %d samples" % (row["ts"], n), flush=True)
            # Sleep to the next whole interval so timestamps stay on a grid
            # even when a request is slow.
            time.sleep(max(1.0, INTERVAL_S - (time.time() % INTERVAL_S)))

    print("done: %d samples -> %s" % (n, out_path), flush=True)


if __name__ == "__main__":
    main()
