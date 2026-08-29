#!/usr/bin/env python3
"""Read a permuto_depth_probe sample file and answer C-0S3.

The claim under test: carried (out-of-hours) ticks accrue depth_seconds.

The test is deliberately one-sided. Both `depth_seconds_5d` and
`depth_seconds_24h` are trailing-window accumulators, so an observed change is

    delta = accrual_during_sample - accrual_at_the_window_edge

The second term is never negative, so it can only drag the observed delta
*down*. Therefore:

  - Any sustained INCREASE while the oracle is frozen cannot be produced by
    roll-off, and cannot be produced by a sampler that credits nothing.
  - A flat or falling reading proves nothing either way, because zero accrual
    and (accrual - equal roll-off) look identical.

  - **But an increase is not conclusive either.** A delayed counter published
    INCREMENTALLY -- a pre-close backlog draining a little into each bucket --
    rises in every sub-window too, and at this sampling rate is
    indistinguishable from carried accrual. The flat rate profile excludes a
    one-time delayed update; it says nothing about a draining one, and
    neither the API contract nor these samples bound the publication lag.

So a positive result is STRONG EVIDENCE, a negative result is uninformative,
and neither is proof. Separating accrual from incremental backfill needs a
control account known to be flat through the close, or a documented lag
bound. The verdict this tool prints says so.
"""

import json
import sys
from datetime import datetime, timezone


def load_sessions(path):
    """Split the file into probe SESSIONS, one per header.

    The probe appends, writing a fresh ``probe_start`` header each run. Keeping
    every row across headers made process downtime look like observed carried
    time: two one-row sessions an hour apart, the same oracle either side, and
    the unobserved gap between them counted as continuous evidence. Sessions
    are separated here and only one is analysed.

    Returns ``(sessions, malformed_count)``.
    """
    sessions = []
    malformed = 0
    # A session boundary may be hiding inside a record we cannot read, so the
    # break is remembered and applied to the NEXT row rather than opening an
    # empty session immediately -- a probe killed on its last write must not
    # leave a phantom trailing session for main() to analyse.
    pending_break = False
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except json.JSONDecodeError:
                # A probe killed mid-write leaves a partial final record.
                # Aborting the parse would throw away every valid sample
                # before it, which is the whole run.
                #
                # But skipping it and carrying on is not enough either. The
                # probe opens the file in append mode and writes its header
                # first, so restarting a killed probe -- the normal response
                # to a kill -- concatenates the unterminated tail with the
                # next run's header into ONE unreadable line. Dropping that
                # line silently swallows the session boundary, rejoins two
                # runs, and hands the hours of unobserved downtime between
                # them to the analysis as continuous evidence. That is the
                # exact failure this function exists to prevent, so any
                # unreadable record is treated as a discontinuity.
                malformed += 1
                pending_break = True
                sys.stderr.write("skipping malformed JSONL record\n")
                continue
            if "probe_start" in d:
                sessions.append({"header": d, "rows": [], "truncated": False})
                pending_break = False
            elif pending_break or not sessions:
                # Either a hand-edited file with rows before any header, or a
                # header that went down with the record we could not read.
                sessions.append(
                    {"header": None, "rows": [d], "truncated": pending_break}
                )
                pending_break = False
            else:
                sessions[-1]["rows"].append(d)
    return sessions, malformed


def _brackets_a_transition(row, prev_oracle, oracle_of):
    """True when this row's leaderboard and oracle reads may disagree.

    Rows written before the probe recorded `t_oracle` carry no bracket, so
    they cannot be checked and are kept -- flagging them would retroactively
    invalidate every historical session, which is a different claim from the
    one this guard makes.

    [review] Note what this does NOT do: it never compares `t_leaderboard_start`
    and `t_oracle` against the instant the oracle changed, because that instant
    is not observable -- it lies somewhere between two samples, and the probe
    records neither end of it. At the one call site the value comparison is
    already decided by the caller, so in practice this reduces to "is the row
    bracketed at all", and every bracketed transition row is dropped. That is
    deliberately conservative: it can discard a clean row, and it cannot admit
    a straddling one. Wrong in the cheap direction.
    """
    if "t_oracle" not in row or "t_leaderboard_start" not in row:
        return False
    if prev_oracle is None:
        return False
    return oracle_of(row) != prev_oracle


def _asserted_window(rows, carried_since, max_gap):
    """Validate the WHOLE tail from an asserted boundary, not the last run.

    Returns ``(window, provenance)`` like `frozen_window`. On rejection the
    provenance is a ``"contradicted: ..."`` string rather than None, because
    the operator needs to know their assertion and the data disagree -- that
    is a different message from "nothing qualified".

    [review] Filtering the ALREADY-SELECTED run by the boundary asks the
    wrong question. `best` is the trailing run, so an oracle transition
    occurring AFTER the asserted boundary splits the tail and leaves `best`
    as the island on the far side of it. The tool then certified that island
    and printed dollar sizing off it, while the samples showing the oracle
    moving inside the asserted carried session -- direct evidence the
    assertion is wrong -- were dropped without a word.

    An oracle that moves inside an asserted carried window is not a reason to
    narrow the window. It is the assertion and the data contradicting each
    other, and only the operator can settle which one is wrong. So the whole
    tail has to hold together: every sample carrying an oracle, all equal, in
    order, with no gap wide enough to hide a move.
    """
    stamped = [i for i, r in enumerate(rows)
               if r.get("ts") and datetime.fromisoformat(r["ts"]) >= carried_since]
    if not stamped:
        return [], None
    start = stamped[0]

    # Same bracket rule as the inferred path. If the first sample after the
    # boundary is the one that changed value and its capture straddles the
    # change, its leaderboard half may still be live -- so drop it rather
    # than let live accrual open a carried window.
    if start > 0 and rows[start].get("oracle") and rows[start - 1].get("oracle"):
        prev_key = json.dumps(rows[start - 1]["oracle"], sort_keys=True)
        first_key = json.dumps(rows[start]["oracle"], sort_keys=True)
        if first_key != prev_key and _brackets_a_transition(
                rows[start], prev_key, lambda r: first_key):
            start += 1

    tail = rows[start:]
    if len(tail) < 2:
        return [], None

    expected = None
    prev_ts = None
    for row in tail:
        ts_text = row.get("ts")
        if not ts_text:
            return [], ("contradicted: a sample after the asserted boundary "
                        "carries no timestamp, so its place in the run cannot "
                        "be checked")
        if not row.get("oracle"):
            return [], (f"contradicted: no oracle reading at {ts_text}, after "
                        "the asserted carried boundary -- an unread oracle "
                        "cannot show the value held still")
        key = json.dumps(row["oracle"], sort_keys=True)
        if expected is None:
            expected = key
        elif key != expected:
            return [], (f"contradicted: the oracle MOVED at {ts_text}, after "
                        "the asserted carried boundary. The assertion and the "
                        "data disagree; move --carried-since past the "
                        "transition, or the session was not carried")
        ts = datetime.fromisoformat(ts_text)
        if prev_ts is not None:
            gap = (ts - prev_ts).total_seconds()
            if gap < 0:
                return [], (f"contradicted: timestamps go backwards at "
                            f"{ts_text}, so the tail is not one ordered run")
            if gap > max_gap:
                return [], (f"contradicted: {gap:.0f}s of missing coverage "
                            f"before {ts_text}, inside the asserted carried "
                            "session -- the oracle could have moved and "
                            "returned unseen")
        prev_ts = ts
    return tail, "asserted"


def frozen_window(rows, interval_s, carried_since=None):
    """Trailing run of consecutive samples with ONE oracle value and no gaps.

    Returns ``(window, provenance)``. ``provenance`` is ``"asserted"`` when the
    operator supplied the carried boundary, ``"inferred"`` when the run merely
    looks like a freeze this file watched happen, ``"contradicted: ..."`` when
    an asserted boundary is refuted by the samples inside it, and ``None`` when
    nothing qualifies. Only an asserted boundary may certify carried accrual --
    see the note on `followed_a_transition` below.

    Two separate problems, one answer.

    The probe's own instructions say to sample from before the close until well
    after it -- but the analyzer demanded a single oracle value across the WHOLE
    file, so any normal pre-close movement made it report "not purely carried"
    while the first-to-last depth delta still quietly included live-session
    accrual. And observed equality either side of a missing stretch never proved
    the oracle held still inside it.

    So rather than judging the file, find the trailing stretch that actually
    qualifies: consecutive samples, every one carrying an oracle reading, all
    equal, and no adjacent pair further apart than a small multiple of the
    sampling interval. Depth endpoints are then taken from inside that window.
    """
    # SUFFIX AFTER THE LAST TRANSITION, not merely the longest flat run.
    #
    # [review round 3] Picking the longest equal-oracle run does not establish
    # that the run is CARRIED. A quiet stretch during live trading can easily
    # be longer than the post-close suffix, and the verdict was then reported
    # as CONFIRMED anyway -- the third time this tool could have certified
    # something it had not actually observed. The carried window is by
    # definition the one the oracle froze INTO most recently, so take the
    # suffix that follows the final oracle change and require it to be a
    # genuine freeze (i.e. something different came before it).
    max_gap = max(interval_s * 3, 30)

    # The asserted path does NOT go through the run scan below. It used to,
    # and reusing `best` there meant an oracle transition after the boundary
    # narrowed the window instead of refuting the assertion -- see
    # `_asserted_window`.
    if carried_since is not None:
        return _asserted_window(rows, carried_since, max_gap)

    best, run = [], []

    def flush():
        # The MOST RECENT completed run, not the longest. The comment above
        # says "suffix after the last transition" and the code kept the
        # longest instead -- so a quiet stretch of live trading longer than
        # the post-close run became `best`, and the is_suffix check below then
        # rejected an otherwise perfectly usable carried window. The selector
        # and its own description disagreed.
        nonlocal best
        if run:
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
        transition = bool(run) and key != prev_oracle
        if transition or gapped:
            if run:
                flush(); run = []
        # [review] DROP the transition row itself when its capture is
        # bracketed. The probe reads the leaderboard first and the oracle
        # after, so around the close a single row can pair a leaderboard
        # snapshot taken while the market was live with an oracle already
        # showing the frozen value -- and that row would then open the frozen
        # run and contribute live accrual to a carried verdict.
        #
        # Only rows the probe bracketed can be judged; older sessions carry
        # no timestamps and are kept, because treating "unknown" as
        # "straddling" would retroactively void every historical sample.
        if transition and _brackets_a_transition(row, prev_oracle,
                                                 lambda r: key):
            prev_ts, prev_oracle = ts, key
            continue
        run.append(row)
        prev_ts, prev_oracle = ts, key
    flush()

    # `best` is the trailing run; now insist it was preceded by a different
    # oracle value and that it really does close the file. A file that never
    # changes value has not observed a freeze happening at all.
    #
    # [review round 4] This pair is necessary and NOT sufficient, and the
    # tool used to treat it as sufficient. "Something different came before
    # it" is satisfied by ordinary intraday movement, and "it closes the
    # file" is satisfied by stopping the probe. So a session sampled entirely
    # inside the cash day, with the oracle merely quiet for its last half
    # hour, met both -- and a live plateau supplied both endpoints for a
    # CONFIRMED carried verdict, dollar sizing included. A frozen oracle is a
    # property of the ESTIMATOR; carriedness is a property of the CLOCK, and
    # the venue exposes no flag joining the two. Nothing in this file can
    # close that gap, so the window is still returned -- an inferred freeze
    # is worth measuring -- but it is labelled, and main() refuses to
    # certify anything the operator has not asserted with --carried-since.
    if not best:
        return [], None
    tail_start = rows.index(best[0])
    seen_before = {
        json.dumps(r.get("oracle"), sort_keys=True)
        for r in rows[:tail_start] if r.get("oracle")
    }
    this_value = json.dumps(best[0].get("oracle"), sort_keys=True)
    followed_a_transition = bool(seen_before - {this_value})
    is_suffix = (best[-1] is rows[-1])

    # An OPERATOR-SUPPLIED boundary is the ONLY way to know, and it is handled
    # at the top of this function. A probe started after the close never sees
    # the freeze happen, so it cannot certify the window from its own data
    # however obviously carried it is -- and the venue exposes no is_carried
    # flag to ask (still unanswered in the channel). Rather than let the tool
    # assume, it accepts the fact it cannot observe: --carried-since
    # <ISO8601>, recorded in the output so the claim travels with provenance.
    if not (followed_a_transition and is_suffix):
        return [], None
    return best, "inferred"


def _sustained_risers(rows, buckets):
    """Accounts that gained in EVERY sub-window, not just end to end.

    Backfill arrives once and stops; genuine accrual keeps arriving. Splitting
    the window and requiring a gain in each bucket is the cheap version of the
    rate-profile check that was done by hand for the recorded experiment.

    `rows` must be the FROZEN WINDOW. Handed the whole session this answers a
    question nobody asked -- whether the account rose during live trading.
    """
    usable = [r for r in rows if r.get("mms")]
    if len(usable) < buckets + 1:
        return set()
    step = len(usable) // buckets
    edges = [usable[i * step] for i in range(buckets)] + [usable[-1]]

    def d5(row):
        return {(m.get("user_id") or m["u"]): m.get("d5") for m in row["mms"]}

    always = None
    for a, b in zip(edges, edges[1:]):
        da, db = d5(a), d5(b)
        gained = {u for u, v in db.items()
                  if v is not None and da.get(u) is not None and v > da[u]}
        always = gained if always is None else (always & gained)
    return always or set()


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
    path = sys.argv[1]
    carried_since = None
    for arg in sys.argv[2:]:
        if arg.startswith("--carried-since="):
            carried_since = _aware_utc(arg.split("=", 1)[1],
                                       "--carried-since")
    sessions, malformed = load_sessions(path)
    if not sessions:
        print("no samples in %s" % path)
        return

    # On stdout, not just stderr: whoever reads the verdict is not reading the
    # terminal's error stream, and an unreadable record is a hole in the
    # observation the verdict rests on.
    if malformed:
        print("WARNING: %d unreadable record(s) skipped. Each is treated as a "
              "session break -- a probe killed mid-write leaves its partial "
              "line welded to the next run's header, and joining those two "
              "runs would count the unobserved gap as evidence.\n" % malformed)

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

    # Select the trailing genuinely-frozen stretch rather than judging the
    # whole file. The probe is documented to run ACROSS the close, so normal
    # pre-close movement is expected and must narrow the window rather than
    # invalidate the run -- while the depth endpoints must come from inside
    # the frozen part, or they silently include live-session accrual.
    window, provenance = frozen_window(session["rows"], interval_s, carried_since)
    window = [r for r in window if r.get("mms")]
    certified = provenance == "asserted"

    print("session      : %s%s"
          % (header.get("probe_start", "?")[:19] or "?",
             "  (starts after a truncated record; its own header was "
             "unreadable)" if session.get("truncated") else ""))
    print("samples      : %d in session, %d in the frozen window"
          % (len(rows), len(window)))

    if len(window) < 2:
        # A refuted assertion is not the same answer as "nothing qualified",
        # and the operator is the only one who can resolve it -- so say which
        # sample disagrees rather than reporting a bare INCONCLUSIVE.
        if isinstance(provenance, str) and provenance.startswith("contradicted"):
            print("carried      : ASSERTION REFUTED BY THE SAMPLES -- "
                  f"{provenance.split(': ', 1)[1]}")
            print("\nINCONCLUSIVE: the asserted carried session is not "
                  "supported by its own samples. Nothing is measured from a "
                  "window the data contradicts.")
            return
        print("oracle       : NO usable frozen window. Need >=2 consecutive "
              "samples with one oracle value and no gaps, AND either an "
              "observed transition into that value or an explicit "
              "--carried-since=<ISO8601>. A run that merely happens to be "
              "flat is not evidence of a carried session -- a quiet stretch "
              "of LIVE trading looks identical.")
        print("\nINCONCLUSIVE: nothing to measure. Re-run the probe entirely "
              "inside a carried session.")
        return

    t0, t1 = window[0], window[-1]
    span_s = (datetime.fromisoformat(t1["ts"])
              - datetime.fromisoformat(t0["ts"])).total_seconds()

    print("window       : %s -> %s UTC (%.0f min)"
          % (t0["ts"][11:19], t1["ts"][11:19], span_s / 60))
    print("oracle       : FROZEN across the selected window, complete "
          "coverage, no gaps > %.0fs" % max(interval_s * 3, 30))
    # The reader of a verdict cannot see which of these two the tool did, and
    # they are not the same claim, so say it on the same screen as the answer.
    if certified:
        print("carried      : ASSERTED by the operator, "
              "--carried-since=%s" % carried_since.isoformat())
    else:
        print("carried      : NOT ESTABLISHED. The oracle is flat and "
              "something different preceded it -- which is also exactly what "
              "a quiet stretch of LIVE trading looks like. Pass "
              "--carried-since=<ISO8601> to assert the boundary the venue "
              "does not expose.")
    print()

    # Join on the FULL id. The 8-char form is for display: two accounts
    # sharing a prefix would overwrite each other and produce a delta between
    # two different accounts, which is worse than no answer.
    key = lambda m: m.get("user_id") or m["u"]
    a = {key(m): m for m in t0["mms"]}
    b = {key(m): m for m in t1["mms"]}

    out = []
    for u, mb in b.items():
        ma = a.get(u)
        if not ma or ma.get("d5") is None or mb.get("d5") is None:
            continue
        d5 = mb["d5"] - ma["d5"]
        # [review] PRESERVE None. Coercing each endpoint to 0 independently
        # turns one unavailable reading into a fabricated 24-hour swing the
        # size of the other endpoint -- printed with a sign, next to real
        # numbers. The d5 path already refuses incomplete endpoints; this one
        # silently invented them.
        a24, b24 = ma.get("d24"), mb.get("d24")
        d24 = None if (a24 is None or b24 is None) else (b24 - a24)
        out.append((d5, d24, u, mb))

    # None is not orderable against a number, and d24 may now be None.
    out.sort(key=lambda row: row[0], reverse=True)
    # depth_seconds accrues at (balanced notional) x (elapsed seconds), so
    # d(depth)/dt read in depth-seconds per second IS the resting balanced
    # notional in dollars. That is the conversion we need for sizing, and it
    # is a lower bound while roll-off is subtracting.
    print("%-10s %16s %16s %14s %12s %10s"
          % ("account", "d(depth_5d)", "d(depth_24h)", "depth_5d now",
             "impl. $depth", "equity"))
    for d5, d24, u, mb in out[:12]:
        print("%-10s %+16.0f %16s %14.0f %12s %10.0f"
              % (u, d5,
                 ("%+.0f" % d24) if d24 is not None else "unavailable",
                 mb["d5"],
                 ("$%.0f" % (d5 / span_s)) if d5 > 0 else "-", mb["eq"]))

    risers = [o for o in out if o[0] > 0]

    print()
    # A single positive endpoint delta is EVIDENCE, not confirmation. The
    # docstring asks for a sustained increase, and delayed leaderboard
    # backfill is a documented alternative source of a one-off gain -- the
    # recorded experiment ruled it out with a multi-bucket flat rate profile,
    # which this generic path does not perform. So the verdict is graded by
    # how much of the window actually supports it.
    #
    # [review round 4] Bucketed over the WINDOW, never the session. This was
    # fed `rows` -- the whole across-close file -- so the one check standing
    # between "an account gained" and "carried ticks accrue" was evaluated
    # over live trading. It cut both ways: a genuine carried gain was
    # downgraded because the account did not also rise in the live buckets,
    # and a live riser plus one backfill credit inside a two-minute frozen
    # window was upgraded to CONFIRMED. The corroboration has to come from
    # the same samples as the claim.
    buckets = max(1, min(6, len(window) // 10))
    sustained = _sustained_risers(window, buckets) if buckets > 1 else set()
    # And from the same ACCOUNTS. `sustained` and `risers` were only tested
    # for non-emptiness, so one account rising steadily vouched for a
    # different account's single jump, and the headline rate below was then
    # quoted from a riser nothing had rate-checked.
    corroborated = [o for o in risers if o[2] in sustained]
    strong = certified and bool(corroborated)

    if strong:
        rate = max(o[0] / span_s * 3600 for o in corroborated)
        print("VERDICT: STRONG EVIDENCE. %d of %d accounts gained depth in "
              "every sub-window" % (len(corroborated), len(out)))
        print("         of an operator-asserted carried session. Roll-off can only")
        print("         subtract, so a sustained gain there cannot come from")
        print("         window decay.")
        # [review] NOT "confirmed", and the reason is a hypothesis this data
        # cannot exclude. A delayed counter published INCREMENTALLY -- a
        # backlog draining a little into each bucket -- rises in every
        # sub-window too, and is observationally identical to carried
        # accrual at this sampling rate. The earlier reasoning ruled out
        # backfill that "arrives once and stops", which the flat rate profile
        # does exclude; it never addressed the incremental case, and neither
        # the API contract nor these samples bound the publication lag.
        #
        # Separating them needs a control: an account known to be flat
        # through the close, or a documented lag bound. Until one exists this
        # is the strongest honest label, and the sizing that depends on it
        # should be read as resting on evidence rather than on a measurement.
        print("         NOT CONFIRMED: incremental backfill -- a delayed counter")
        print("         draining a little into each bucket -- is observationally")
        print("         identical here. Excluding it needs a flat-account control")
        print("         or a published lag bound; neither exists yet.")
        print("         Top corroborated rate: %.0f depth-seconds/hour, i.e. about"
              % rate)
        print("         $%.0f of balanced depth resting inside the 2%% ring."
              % (rate / 3600))
        print()
        print("         At that rate 300,000,000 takes %.1f hours; the contest"
              % (300e6 / rate))
        print("         window is 102.5 h, which needs ~$813 held throughout.")
    elif risers and not certified:
        print("VERDICT: NOT CERTIFIED. %d of %d accounts gained depth across a "
              "flat-oracle" % (len(risers), len(out)))
        print("         window, but nothing here establishes that window as CARRIED.")
        print("         A quiet stretch of live trading reads identically, and live")
        print("         accrual would explain the same gain. Re-run with")
        print("         --carried-since=<ISO8601> to assert the boundary, or probe")
        print("         across a close you actually observe.")
    elif risers:
        print("VERDICT: EVIDENCE, NOT CONFIRMATION. %d of %d accounts show a "
              "positive" % (len(risers), len(out)))
        print("         endpoint delta with the oracle frozen, but no account "
              "gained in")
        print("         every sub-window OF THAT WINDOW -- so delayed leaderboard")
        print("         backfill is not excluded.")
        print("         Re-run over a longer carried window before relying on "
              "this.")
    else:
        print("INCONCLUSIVE: no account gained depth. This does NOT disprove accrual —")
        print("         zero accrual and accrual-cancelled-by-roll-off are")
        print("         indistinguishable here. It may also mean nobody was quoting")
        print("         two-sided inside the 2%% ring. Re-run across an open instead.")


if __name__ == "__main__":
    main()
