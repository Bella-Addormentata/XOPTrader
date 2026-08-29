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

**A THIRD EXPLANATION THIS CANNOT SEPARATE, added 2026-08-28.** Delayed
pre-close credit published INCREMENTALLY -- a backlog draining a little into
each bucket -- also rises smoothly across the boundary and is
indistinguishable from carried accrual here. The two fingerprints above
distinguish accrual from a SHARP stop and from roll-off; they do not
distinguish it from a slow drain, and nothing in the API contract bounds the
publication lag. So this probe settles the question only in the negative
direction. Confirming it needs a control account known to be flat through
the close, or a documented lag bound -- see `TODO-COMPETITION.md` C-0S3,
which is why the verdict was downgraded to strong evidence.

The oracle is sampled alongside, because the frozen-price transition marks
the carried boundary far more precisely than the wall clock does.

Read-only, unauthenticated: one leaderboard GET per 100 market makers plus
one oracle read, each minute. That is two GETs per minute at the present
field size (26) and grows a page at a time -- it is not a fixed two, and
this line used to say it was.
"""

import json
import os
import sys
import time
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
    # [review] BRACKET the capture, do not stamp it once at the top.
    #
    # The oracle is fetched after the last paginated leaderboard page, so
    # around the cash close a row could pair a leaderboard read taken while
    # the market was still live with an oracle read already showing the
    # frozen carried value -- and the analyzer would then count live accrual
    # as carried. One timestamp cannot express that; two can, and the
    # analyzer rejects any row whose bracket straddles an oracle transition.
    t_start = datetime.now(timezone.utc)
    row = {"ts": t_start.isoformat(), "t_leaderboard_start": t_start.isoformat()}
    try:
        # Page. The default page size is 20 and this asked for 100 without
        # checking the total -- so a field larger than 100 was silently
        # truncated, which is the same mistake as reading page one, just
        # further along. Nothing warns; the missing accounts simply are not
        # in the sample.
        mms, offset = [], 0
        lb = {}
        while True:
            lb = get("/exchange/leaderboard?limit=100&offset=%d" % offset)
            page = lb.get("market_makers", [])
            mms.extend(page)
            total = lb.get("market_makers_total")
            if not page or not isinstance(total, int) or len(mms) >= total:
                break
            offset += 100
        row["mm_total"] = lb.get("market_makers_total")
        row["finalized"] = lb.get("finalized")
        row["mms"] = [
            {
                # Full id for joins; the 8-char form is for display only.
                "user_id": m["user_id"],
                "u": m["user_id"][:8],
                "d5": m.get("depth_seconds_5d"),
                "d24": m.get("depth_seconds_24h"),
                "eq": float(m.get("equity") or 0),
                "pnl": float(m.get("total_pnl") or 0),
                "elig": m.get("prize_eligible"),
                "trades": m.get("trade_count"),
            }
            for m in mms
        ]
    except Exception as e:  # noqa: BLE001 - a bad sample must not end the run
        row["lb_error"] = "%s: %s" % (type(e).__name__, e)
    try:
        row["t_oracle"] = datetime.now(timezone.utc).isoformat()
        row["oracle"] = get("/info/oracle").get("prices")
    except Exception as e:  # noqa: BLE001
        row["oracle_error"] = "%s: %s" % (type(e).__name__, e)
    return row


def _aware_utc(text, what):
    """Parse an ISO timestamp that MUST carry an offset.

    [review] datetime.fromisoformat() happily accepts a naive value, and the
    rows this is compared against are timezone-aware UTC -- so a
    perfectly reasonable-looking argument parsed fine and then raised
    TypeError deep in the comparison, in the observer's case AFTER the output
    file had already been created. Reject it here, where the message can say
    what to type.
    """
    try:
        value = datetime.fromisoformat(text)
    except ValueError as exc:
        raise SystemExit("%s: %r is not an ISO-8601 timestamp (%s)"
                         % (what, text, exc))
    if value.tzinfo is None:
        raise SystemExit(
            "%s: %r has no UTC offset. Timestamps here are compared against "
            "timezone-aware UTC samples, so a naive value cannot be ordered "
            "against them. Use e.g. %sZ or %s+00:00."
            % (what, text, text, text))
    return value.astimezone(timezone.utc)


def main():
    out_path = sys.argv[1]
    stop_at = sys.argv[2]  # ISO8601 UTC, e.g. 2026-08-27T22:30:00+00:00
    stop = _aware_utc(stop_at, "stop time")

    # A bare filename has no dirname, and makedirs("") raises
    # FileNotFoundError -- so the probe would die before its first sample
    # over an argument that is perfectly reasonable.
    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    # Terminate a previous run's last line before appending our header. A
    # probe killed mid-write leaves the file without a trailing newline, and
    # appending the header straight onto it produces ONE unreadable line
    # carrying both the partial sample and the session boundary -- the
    # analyzer then cannot tell the two runs apart, so hours of downtime it
    # never observed get counted as continuous evidence. One seek is cheaper
    # than reconstructing that afterwards.
    if os.path.exists(out_path) and os.path.getsize(out_path):
        with open(out_path, "rb") as probe_fh:
            probe_fh.seek(-1, os.SEEK_END)
            unterminated = probe_fh.read(1) != b"\n"
        if unterminated:
            with open(out_path, "a", encoding="utf-8", newline="\n") as fh:
                fh.write("\n")

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
