"""Sample the Permuto oracle and pause flags across a close -> open gap.

WHY
---
The overnight inventory curfew (gui/services/permuto/curfew.py) permits a
bounded LONG overnight and forbids new shorts, on the reasoning that the
oracle is a 60-second trailing realised-vol estimate: it freezes on a calm
end-of-day window while the first print after the reopen comes from the
most violent minute of the day, so the reopening print is systematically
HIGHER and a carried short is what gets liquidated.

That reasoning is sound but UNMEASURED.  The size of the overnight long
allowance (OVERNIGHT_LONG_FRACTION, currently 0.25) is a reasoned default,
not a number anyone has checked against reality, and whether holding it is
profitable also depends on funding -- which settles every 60s here and
could easily eat the jump it is trying to capture.

This script collects the evidence: how long the oracle actually stays
frozen, exactly when it unfreezes, and how far it moves when it does.

WHAT IT IS NOT
--------------
Read-only, and unauthenticated.  It touches /info/oracle and /info/meta --
the same public routes any visitor can read -- and it places no orders,
holds no session, and cannot affect the trading loop.  Funding is NOT
sampled here because it needs an authenticated session, and a measurement
script has no business holding trading credentials; pull funding from the
account history afterwards instead.

USAGE
-----
    python scripts/permuto_gap_probe.py [--hours 20] [--interval 30]

Writes append-only JSONL to data/gap_probe.jsonl, one object per sample.
Safe to stop and restart; safe to run alongside the GUI.
"""

from __future__ import annotations

import argparse
import json
import os
import time
import urllib.error
import urllib.request

BASE = "https://perps.permuto.capital"
HEADERS = {"User-Agent": "Mozilla/5.0", "Accept": "application/json"}
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "data", "gap_probe.jsonl")

# 16:00 ET close / 09:29 ET un-pause, EDT = UTC-4 for the contest week.
# Recorded rather than computed: this box has no tzdata, and a hardcoded
# offset that is written down beats a silent wrong conversion.
NOTE = "20:00Z == 16:00 ET close; 13:29Z == 09:29 ET un-pause (EDT, UTC-4)"


def get(path, timeout=15):
    req = urllib.request.Request(BASE + path, headers=HEADERS, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def sample():
    """One observation, or an error row.  Never raises."""
    row = {"t": time.time(), "iso": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                                  time.gmtime())}
    try:
        row["prices"] = (get("/info/oracle") or {}).get("prices")
    except Exception as exc:  # noqa: BLE001
        row["oracle_error"] = str(exc)[:200]
    try:
        flags = (get("/info/meta") or {}).get("flags") or {}
        row["flags"] = {k: flags.get(k) for k in
                        ("trading_paused", "paused_at", "pause_resume_at",
                         "pause_reason")}
    except Exception as exc:  # noqa: BLE001
        row["meta_error"] = str(exc)[:200]
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=20.0)
    ap.add_argument("--interval", type=float, default=30.0)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    deadline = time.time() + args.hours * 3600.0
    last_prices = None
    still_since = None

    with open(OUT, "a", encoding="utf-8") as fh:
        fh.write(json.dumps({"probe_started": time.time(), "note": NOTE,
                             "interval_s": args.interval}) + "\n")
        fh.flush()
        while time.time() < deadline:
            row = sample()
            prices = row.get("prices")
            # Mark the freeze/unfreeze edge, which is the whole point: the
            # first MOVE after a still stretch is the reopening print.
            if prices and prices == last_prices:
                still_since = still_since or row["t"]
                row["still_for_s"] = round(row["t"] - still_since, 1)
            elif prices:
                if still_since is not None:
                    row["unfroze_after_s"] = round(row["t"] - still_since, 1)
                    row["previous_prices"] = last_prices
                still_since = row["t"]
                last_prices = prices
            fh.write(json.dumps(row) + "\n")
            fh.flush()
            time.sleep(args.interval)


if __name__ == "__main__":
    main()
