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


def load_sessions(path):
    """Split the file into probe SESSIONS, one per header.

    The probe appends, writing a fresh ``probe_start`` header each run. Keeping
    every row across headers made process downtime look like observed carried
    time: two one-row sessions an hour apart, the same oracle either side, and
    the unobserved gap between them counted as continuous evidence. Sessions
    are separated here and only one is analysed.
    """
    sessions = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            if "probe_start" in d:
                sessions.append({"header": d, "rows": []})
            elif sessions:
                sessions[-1]["rows"].append(d)
            else:  # rows before any header (hand-edited file)
                sessions.append({"header": None, "rows": [d]})
    return sessions


def frozen_window(rows, interval_s):
    """Longest run of consecutive samples with ONE oracle value and no gaps.

    Two separate problems, one answer.

    The probe's own instructions say to sample from before the close until well
    after it -- but the analyzer demanded a single oracle value across the WHOLE
    file, so any normal pre-close movement made it report "not purely carried"
    while the first-to-last depth delta still quietly included live-session
    accrual. And observed equality either side of a missing stretch never proved
    the oracle held still inside it.

    So rather than judging the file, find the longest stretch that actually
    qualifies: consecutive samples, every one carrying an oracle reading, all
    equal, and no adjacent pair further apart than a small multiple of the
    sampling interval. Depth endpoints are then taken from inside that window.
    """
    max_gap = max(interval_s * 3, 30)
    best, run = [], []

    def flush():
        nonlocal best
        if len(run) > len(best):
            best = list(run)

    prev_ts = None
    prev_oracle = None
    for row in rows:
        oracle = row.get("oracle")
        ts = datetime.fromisoformat(row["ts"]) if row.get("ts") else None
        if not oracle or ts is None:
            flush(); run = []; prev_ts = prev_oracle = None
            continue
        key = json.dumps(oracle, sort_keys=True)
        gapped = prev_ts is not None and (ts - prev_ts).total_seconds() > max_gap
        if run and (key != prev_oracle or gapped):
            flush(); run = []
        run.append(row)
        prev_ts, prev_oracle = ts, key
    flush()
    return best


def main():
    path = sys.argv[1]
    sessions = load_sessions(path)
    if not sessions:
        print("no samples in %s" % path)
        return

    # One session only. Analysing across a restart counts process downtime as
    # observed carried time -- reproduced with two one-row sessions an hour
    # apart, which the old code reported as CONFIRMED.
    if len(sessions) > 1:
        print("NOTE: %d probe sessions in this file; analysing the LAST one. "
              "Rows from earlier sessions are ignored -- the gaps between "
              "runs are not observed time.\n" % len(sessions))
    session = sessions[-1]
    header = session["header"] or {}
    interval_s = float(header.get("interval_s") or 60)

    rows = [r for r in session["rows"] if r.get("mms")]
    if len(rows) < 2:
        print("not enough samples in this session (%d)" % len(rows))
        return

    # Select the longest genuinely-carried stretch rather than judging the
    # whole file. The probe is documented to run ACROSS the close, so normal
    # pre-close movement is expected and must narrow the window rather than
    # invalidate the run -- while the depth endpoints must come from inside
    # the frozen part, or they silently include live-session accrual.
    window = frozen_window(session["rows"], interval_s)
    window = [r for r in window if r.get("mms")]

    print("session      : %s" % (header.get("probe_start", "?")[:19] or "?"))
    print("samples      : %d in session, %d in the frozen window"
          % (len(rows), len(window)))

    if len(window) < 2:
        print("oracle       : NO usable frozen window (need >=2 consecutive "
              "samples with one oracle value and no gaps)")
        print("\nINCONCLUSIVE: nothing to measure. Re-run the probe entirely "
              "inside a carried session.")
        return

    t0, t1 = window[0], window[-1]
    span_s = (datetime.fromisoformat(t1["ts"])
              - datetime.fromisoformat(t0["ts"])).total_seconds()
    frozen = True

    print("window       : %s -> %s UTC (%.0f min)"
          % (t0["ts"][11:19], t1["ts"][11:19], span_s / 60))
    print("oracle       : FROZEN across the selected window, complete "
          "coverage, no gaps > %.0fs" % max(interval_s * 3, 30))
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
