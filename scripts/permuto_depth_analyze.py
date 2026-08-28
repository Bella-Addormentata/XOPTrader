#!/usr/bin/env python3
"""Read a permuto_depth_probe sample file and answer C-0S3.

The claim under test: carried (out-of-hours) ticks accrue depth_seconds.

The test is deliberately one-sided. Both `depth_seconds_5d` and
`depth_seconds_24h` are trailing-window accumulators, so an observed change is

    delta = accrual_during_sample - accrual_at_the_window_edge

The second term is never negative, so it can only drag the observed delta
*down*. Therefore:

  - Any sustained INCREASE while the oracle is frozen proves accrual. It
    cannot be produced by roll-off, and it cannot be produced by a sampler
    that credits nothing.
  - A flat or falling reading proves nothing either way, because zero accrual
    and (accrual - equal roll-off) look identical.

So a positive result is conclusive and a negative result is merely
uninformative. Report it that way rather than claiming symmetry.
"""

import json
import sys
from datetime import datetime


def load(path):
    header, rows = None, []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            if "probe_start" in d:
                header = d
            else:
                rows.append(d)
    return header, rows


def main():
    path = sys.argv[1]
    header, all_rows = load(path)
    rows = [r for r in all_rows if r.get("mms")]
    if len(rows) < 2:
        print("not enough samples yet (%d)" % len(rows))
        return

    t0, t1 = rows[0], rows[-1]
    span_s = (datetime.fromisoformat(t1["ts"]) - datetime.fromisoformat(t0["ts"])).total_seconds()

    # Was the oracle frozen for the WHOLE window? That is what makes this a
    # carried-session measurement rather than a mixed one, and it is the only
    # reason a depth gain proves anything.
    #
    # Two ways this check could lie by omission, both fixed here. It ran over
    # `rows`, which has already dropped every sample whose LEADERBOARD fetch
    # failed -- even though such a sample may carry a perfectly good, MOVING
    # oracle. And it then skipped samples with no oracle at all. So a window
    # that actually contained oracle movement could present one distinct
    # value and read as frozen. Missing coverage is now a disqualifier rather
    # than something to quietly filter away: an incomplete window cannot
    # confirm anything.
    oracle_samples = [r.get("oracle") for r in all_rows if "oracle" in r or "oracle_error" in r]
    missing = sum(1 for o in oracle_samples if not o)
    distinct = {json.dumps(o, sort_keys=True) for o in oracle_samples if o}
    frozen = (len(distinct) == 1 and missing == 0)

    print("samples      : %d over %.0f min" % (len(rows), span_s / 60))
    print("window       : %s -> %s UTC" % (t0["ts"][11:19], t1["ts"][11:19]))
    if frozen:
        oracle_note = "FROZEN throughout (carried), %d samples" % len(oracle_samples)
    elif missing:
        oracle_note = ("INCOMPLETE - %d of %d samples have no oracle reading; "
                       "cannot certify the window as carried"
                       % (missing, len(oracle_samples)))
    else:
        oracle_note = ("MOVED (%d distinct values) - window is NOT purely carried"
                       % len(distinct))
    print("oracle       : %s" % oracle_note)
    print()

    a = {m["u"]: m for m in t0["mms"]}
    b = {m["u"]: m for m in t1["mms"]}

    out = []
    for u, mb in b.items():
        ma = a.get(u)
        if not ma or ma.get("d5") is None or mb.get("d5") is None:
            continue
        d5 = mb["d5"] - ma["d5"]
        d24 = (mb.get("d24") or 0) - (ma.get("d24") or 0)
        out.append((d5, d24, u, mb))

    out.sort(reverse=True)
    # depth_seconds accrues at (balanced notional) x (elapsed seconds), so
    # d(depth)/dt read in depth-seconds per second IS the resting balanced
    # notional in dollars. That is the conversion we need for sizing, and it
    # is a lower bound while roll-off is subtracting.
    print("%-10s %16s %16s %14s %12s %10s"
          % ("account", "d(depth_5d)", "d(depth_24h)", "depth_5d now",
             "impl. $depth", "equity"))
    for d5, d24, u, mb in out[:12]:
        print("%-10s %+16.0f %+16.0f %14.0f %12s %10.0f"
              % (u, d5, d24, mb["d5"],
                 ("$%.0f" % (d5 / span_s)) if d5 > 0 else "-", mb["eq"]))

    risers = [o for o in out if o[0] > 0]
    implied_hr = [(o[0] / span_s * 3600) for o in risers]

    print()
    if risers and frozen:
        print("VERDICT: CONFIRMED. %d of %d accounts gained depth while the oracle"
              % (len(risers), len(out)))
        print("         was frozen. Roll-off can only subtract, so a gain during a")
        print("         carried session can only come from carried ticks accruing.")
        print("         Top accrual rate: %.0f depth-seconds/hour, i.e. about"
              % max(implied_hr))
        print("         $%.0f of balanced depth resting inside the 2%% ring."
              % (max(implied_hr) / 3600))
        print()
        print("         At that rate 300,000,000 takes %.1f hours; the contest"
              % (300e6 / max(implied_hr)))
        print("         window is 102.5 h, which needs ~$813 held throughout.")
    elif risers and not frozen:
        print("INCONCLUSIVE: accounts gained depth, but the oracle moved during the")
        print("         window, so some of the gain may be from live ticks.")
    else:
        print("INCONCLUSIVE: no account gained depth. This does NOT disprove accrual —")
        print("         zero accrual and accrual-cancelled-by-roll-off are")
        print("         indistinguishable here. It may also mean nobody was quoting")
        print("         two-sided inside the 2%% ring. Re-run across an open instead.")


if __name__ == "__main__":
    main()
