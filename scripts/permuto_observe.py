#!/usr/bin/env python3
"""Read-only session recorder for the Permuto vol markets.

C-03. Everything here is a public GET; it places no orders, needs no account,
and cannot lose money. That is deliberate for the first live run: the venue
resets balances and depth_seconds on Sunday evening, so nothing traded before
then persists -- today's only product is UNDERSTANDING, and understanding is
cheaper and more trustworthy from observation than from a hastily written
quoting loop's fills.

What it answers, none of which we currently know from live data:

* **Is the short-horizon oracle jitter noise or information?** The central
  question for whether this is quotable at all. The oracle is a 60-second
  realized-vol estimate resampled every 5s, so consecutive prints share 55
  seconds of input -- most of the visible movement may be construction rather
  than news. Recording oracle and subsequent trades together lets that be
  measured instead of argued.
* **What does the ±2% depth-credit ring actually look like?** Depth accrues on
  min(bid, ask) inside the ring. Whether a balanced $1,000 is cheap or
  suicidal depends on the book we would be joining, and one glance at QQQ
  today showed a 52,848-size bid against a 2-size ask.
* **How does the book behave across the 16:00 ET close?** The one transition
  every entrant passes through, and the venue cancels all resting orders at
  the open on the other side of it.
* **Does anything pause?** A free dry run for C-11 -- the only thing the
  sponsor said bots must handle.

Sampling follows the venue's own cadence: the oracle resamples every 5s, so
that is the interesting resolution and anything slower aliases it.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

BASE = "https://perps.permuto.capital"
# A default urllib agent gets HTTP 403 on routes needing no credentials at
# all; the 403 reads as an auth failure and is not one.
HEADERS = {"User-Agent": "Mozilla/5.0 (XOPTrader observer)", "Accept": "application/json"}

MARKETS = ["QQQ-VOL-PERP", "NVDA-VOL-PERP", "TSLA-VOL-PERP"]
TICK_S = 5.0
TRADES_EVERY = 3      # ticks
META_EVERY = 12       # ticks


def get(path, timeout=8):
    req = urllib.request.Request(BASE + path, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def safe(path):
    """Never let one bad endpoint end a session that cannot be re-run."""
    try:
        return get(path)
    except Exception as exc:  # noqa: BLE001
        return {"__error__": "%s: %s" % (type(exc).__name__, exc)}


def main():
    out_path = sys.argv[1]
    stop = datetime.fromisoformat(sys.argv[2])

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    seen_trades = {m: set() for m in MARKETS}

    with open(out_path, "a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps({
            "observe_start": datetime.now(timezone.utc).isoformat(),
            "tick_s": TICK_S, "markets": MARKETS, "stop_at": sys.argv[2],
            "note": "read-only; oracle+L2 every tick, trades every %d, meta every %d"
                    % (TRADES_EVERY, META_EVERY),
        }) + "\n")
        fh.flush()

        n = 0
        while datetime.now(timezone.utc) < stop:
            n += 1
            row = {"ts": datetime.now(timezone.utc).isoformat(), "n": n}
            row["oracle"] = safe("/info/oracle").get("prices")

            books, funding = {}, {}
            for m in MARKETS:
                l2 = safe("/info/l2/" + m)
                # Store only the top of book plus the aggregate; the full
                # ladder would balloon the file and the ring is what matters.
                books[m] = {
                    "bids": (l2.get("bids") or [])[:8],
                    "asks": (l2.get("asks") or [])[:8],
                    "err": l2.get("__error__"),
                }
                f = safe("/info/funding/" + m)
                funding[m] = {k: f.get(k) for k in
                              ("hourly_rate", "premium", "oracle_price",
                               "predicted_rate")}
            row["l2"] = books
            row["funding"] = funding

            if n % TRADES_EVERY == 0:
                fresh = {}
                for m in MARKETS:
                    t = safe("/info/trades/" + m)
                    new = []
                    for tr in (t.get("trades") or []):
                        tid = tr.get("id")
                        if tid is not None and tid not in seen_trades[m]:
                            seen_trades[m].add(tid)
                            new.append(tr)
                    if new:
                        fresh[m] = new
                if fresh:
                    row["trades"] = fresh

            if n % META_EVERY == 0:
                flags = safe("/info/meta").get("flags") or {}
                row["meta"] = {k: flags.get(k) for k in
                               ("trading_paused", "pause_reason",
                                "pause_resume_at", "signup_closed",
                                "untraded_purge_at")}

            fh.write(json.dumps(row) + "\n")
            fh.flush()
            if n % 60 == 0:
                print("%s  tick %d" % (row["ts"][11:19], n), flush=True)

            time.sleep(max(0.5, TICK_S - (time.time() % TICK_S)))

    print("done: %d ticks -> %s" % (n, out_path), flush=True)


if __name__ == "__main__":
    main()
