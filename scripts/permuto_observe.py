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
import math
import os
import sys
import time
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

# The documented DEFAULT of vol_aggressive_ring_pct, used only when
# /info/meta cannot be read. It is a venue parameter, not a constant -- see
# _ring_pct_from_meta.
DEFAULT_RING_PCT = 2.0


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


def _ring_pct_from_meta(meta):
    """`vol_aggressive_ring_pct` out of a /info/meta payload, or None.

    [review] The ring percentage was hard-coded at 2.0 here while the venue
    PUBLISHES it and the reference documents 2 as a *default*, not a
    constant. If the sponsor retunes it for the contest, every credit_usd in
    the recording is computed against the wrong band and cannot be
    recomputed afterwards -- only eight levels of each ladder are kept, and
    the ring can hold more.

    Searched by key name rather than by path: the nesting of the band
    parameters inside /info/meta is not recorded anywhere we can check
    offline, and a guessed path that misses would fall silently back to the
    default -- which is exactly the failure this lookup exists to catch.
    """
    stack = [meta]
    while stack:
        node = stack.pop()
        if isinstance(node, dict):
            if "vol_aggressive_ring_pct" in node:
                try:
                    value = float(node["vol_aggressive_ring_pct"])
                except (TypeError, ValueError):
                    return None
                # [review] float() accepts NaN, the infinities and negatives.
                # A NaN here is the worst of them and it is SILENT: every
                # in-ring test `abs(price - oracle) / oracle * 100.0 <= nan`
                # is False, so both sides total zero, credit_usd is a
                # perfectly well-formed 0.0, and ring_pct_src is stamped
                # "meta" -- a recording that looks like a measured absence of
                # depth rather than a broken band, for as long as the venue
                # serves that value. Falling back to the documented default
                # is the honest reading of an unusable one.
                if not (math.isfinite(value) and value > 0.0):
                    return None
                return value
            stack.extend(node.values())
        elif isinstance(node, list):
            stack.extend(node)
    return None


def _ring_depth(bids, asks, oracle, ring_pct=DEFAULT_RING_PCT):
    """Balanced notional inside the depth-credit ring, per the venue's rule.

    Depth accrues on ``min(bid, ask)`` within +/-``ring_pct`` of a fresh
    oracle (the venue's `vol_aggressive_ring_pct`, 2 by default), so the
    minimum -- not either side, and not the sum -- is what a market earns.
    Recorded per tick because the ladder it is derived from is not kept.
    """
    if not oracle or not (oracle > 0.0):
        return None

    def side(levels):
        total, n = 0.0, 0
        for lvl in levels:
            try:
                price = float(lvl["price"])
                size = float(lvl["size"])
            except (KeyError, TypeError, ValueError):
                continue
            # [review] Numeric is not the same as valid. The conversion
            # rejected junk that would not parse and then admitted anything
            # that would -- including a negative size, which subtracts from
            # the aggregate, and NaN or Infinity, which json.loads accepts as
            # bare literals and which propagate to a non-finite credit_usd.
            # NaN additionally writes `NaN` into the JSONL, which strict
            # parsers reject for the whole line.
            #
            # One malformed level poisons the recorder's central derived
            # value, and it cannot be recovered afterwards: only eight levels
            # of each ladder are kept and the ring can hold more.
            if not (math.isfinite(price) and math.isfinite(size)
                    and price > 0.0 and size > 0.0):
                continue
            if abs(price - oracle) / oracle * 100.0 <= ring_pct:
                total += price * size
                n += 1
        return total, n

    bid_usd, bid_n = side(bids)
    ask_usd, ask_n = side(asks)
    return {
        "bid_usd": round(bid_usd, 4),
        "ask_usd": round(ask_usd, 4),
        "credit_usd": round(min(bid_usd, ask_usd), 4),
        "bid_levels": bid_n,
        "ask_levels": ask_n,
    }


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
    stop = _aware_utc(sys.argv[2], "stop time")

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    seen_trades = {m: set() for m in MARKETS}

    # [review] Same unterminated-line repair the depth probe already does.
    # A run killed mid-write leaves no trailing newline, and appending the
    # next session's header straight onto it produces ONE unreadable line
    # carrying both the partial sample and the session boundary -- so the
    # analyzer cannot tell the runs apart and counts downtime it never
    # observed as continuous evidence. One seek is cheaper than
    # reconstructing that later.
    if os.path.exists(out_path) and os.path.getsize(out_path):
        with open(out_path, "rb") as probe_fh:
            probe_fh.seek(-1, os.SEEK_END)
            unterminated = probe_fh.read(1) != b"\n"
        if unterminated:
            with open(out_path, "a", encoding="utf-8", newline="\n") as fh:
                fh.write("\n")

    # Read the ring BEFORE the first sample, not on the first META_EVERY
    # tick: rows written before that would otherwise be measured against an
    # assumed band. A failed read does not blank the session -- the
    # documented default is used and SAID SO in the header and on every row,
    # so a reader can discount the aggregate instead of finding nothing at
    # all where a measurement should be.
    ring_pct = _ring_pct_from_meta(safe("/info/meta"))
    ring_pct_src = "meta" if ring_pct is not None else "default"
    if ring_pct is None:
        ring_pct = DEFAULT_RING_PCT

    with open(out_path, "a", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps({
            "observe_start": datetime.now(timezone.utc).isoformat(),
            "tick_s": TICK_S, "markets": MARKETS, "stop_at": sys.argv[2],
            "ring_pct": ring_pct, "ring_pct_src": ring_pct_src,
            "note": "read-only; oracle+L2 every tick, trades every %d, meta every %d"
                    % (TRADES_EVERY, META_EVERY),
        }) + "\n")
        fh.flush()

        n = 0
        while datetime.now(timezone.utc) < stop:
            n += 1
            row = {"ts": datetime.now(timezone.utc).isoformat(), "n": n}
            oracle_doc = safe("/info/oracle")
            row["oracle"] = oracle_doc.get("prices")
            # [review] Keep the failure. This is the tick-level series the
            # jitter and trade analysis runs on, and discarding __error__ made
            # an outage indistinguishable from a success carrying no prices.
            if "__error__" in oracle_doc:
                row["oracle_err"] = oracle_doc["__error__"]

            books, funding = {}, {}
            for m in MARKETS:
                # [review] RE-READ THE ORACLE PER BOOK. It used to be fetched
                # once at the top of the tick, and each of the seven requests
                # below carries an 8-second timeout -- so the last book could
                # be classified against an oracle several 5-second resamples
                # old, and the +/-2% ring aggregate computed from it is then
                # simply wrong. The aggregate is the one number this recorder
                # exists to produce, and it cannot be recomputed later.
                #
                # The extra call is cheap next to the L2 fetch it accompanies,
                # and the value used is stored beside the ring so a reader can
                # see which oracle each book was measured against rather than
                # having to assume.
                per_book_raw = safe("/info/oracle")
                per_book_oracle = per_book_raw.get("prices") or {}
                l2 = safe("/info/l2/" + m + "?levels=500")
                l2_err = l2.get("__error__")
                bids = l2.get("bids") or []
                asks = l2.get("asks") or []
                # THE AGGREGATE IS THE POINT, and an earlier version stored
                # only eight levels while its own comment claimed otherwise.
                # The +/-2% ring can hold more than eight, so a truncated
                # ladder cannot reconstruct balanced in-ring depth -- the one
                # question this recorder exists to answer. Computed here,
                # against the oracle from this same tick, because it cannot
                # be recovered later from a ladder we did not keep.
                ticker = m.replace("-PERP", "")
                # [review] NO FALLBACK to the tick-start oracle. Doing that
                # computed a non-null ring against a value possibly several
                # 5-second resamples old -- silently recreating the exact
                # stale-oracle misclassification this per-book fetch was
                # added to prevent, and producing a number a reader would
                # trust. When the adjacent read fails the ring is simply
                # unavailable, and the error is recorded so the gap is
                # visible rather than filled in.
                oracle_err = per_book_raw.get("__error__")
                ora = per_book_oracle.get(ticker) if not oracle_err else None
                # [review] AN UNREAD BOOK IS NOT AN EMPTY BOOK. A failed L2
                # fetch leaves bids/asks as [], and _ring_depth then happily
                # returns credit_usd 0.0 with zero levels -- a
                # valid-looking measurement of a market we never saw, which
                # any aggregation that does not join on `err` averages in as
                # a real zero. The oracle side already got this right (a bad
                # oracle read yields no ring at all); the book side did not.
                # Same rule both ways: we looked and there was nothing, or
                # we did not look.
                books[m] = {
                    "bids": bids[:8] if not l2_err else None,
                    "asks": asks[:8] if not l2_err else None,
                    "n_bid_levels": len(bids) if not l2_err else None,
                    "n_ask_levels": len(asks) if not l2_err else None,
                    "ring": None if l2_err else _ring_depth(bids, asks, ora,
                                                            ring_pct),
                    # Which oracle this book was measured against, so a
                    # reader never has to assume it was the tick's -- and the
                    # error when there was none, so a null ring can be told
                    # apart from a book with no depth.
                    "ring_oracle": ora,
                    "ring_oracle_err": oracle_err,
                    "err": l2_err,
                }
                # [review] The per-market route, not /info/funding/predicted.
                # The reference labels this one "historical funding", which
                # reads as though `premium` and `hourly_rate` would come back
                # empty here -- they do not. The C-11 measurement recorded in
                # TODO-COMPETITION came out of this exact call: `premium`
                # moved every 5s and `hourly_rate` stepped on 60-second
                # boundaries across a 54-minute run. `predicted_rate` may
                # well be absent; a missing key lands as None, which is
                # visible, and switching routes would cost the fields that
                # are actually arriving.
                f = safe("/info/funding/" + m)
                funding[m] = {k: f.get(k) for k in
                              ("hourly_rate", "premium", "oracle_price",
                               "predicted_rate")}
                # [review] Keep the FAILURE, not just its shape. safe()
                # returns {"__error__": ...}, and projecting only the four
                # known keys discarded it -- so a failed request recorded
                # exactly the same all-null row as a successful one whose
                # payload happened to lack those fields. Downstream cannot
                # then exclude the missing observations, and reads them as
                # funding data.
                if "__error__" in f:
                    funding[m]["__error__"] = f["__error__"]
            row["l2"] = books
            row["funding"] = funding
            # Per row, not only in the header: a run that spans a mid-session
            # retune must not present two different bands as one number.
            row["ring_pct"] = ring_pct
            row["ring_pct_src"] = ring_pct_src

            if n % TRADES_EVERY == 0:
                fresh, trade_errs = {}, {}
                for m in MARKETS:
                    t = safe("/info/trades/" + m)
                    # [review] The same hole, and here it inverts a finding.
                    # A failed request yields no "trades" key, the loop sees
                    # an empty list, and the row records neither trades nor
                    # an error -- indistinguishable from a successful poll
                    # over a quiet market. Missing observations would read as
                    # an apparent no-fill interval, which is precisely the
                    # quantity this recording exists to measure.
                    if "__error__" in t:
                        trade_errs[m] = t["__error__"]
                        continue
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
                if trade_errs:
                    row["trades_err"] = trade_errs

            if n % META_EVERY == 0:
                meta = safe("/info/meta")
                flags = meta.get("flags") or {}
                # [review] Same again, and this poll is the observer's only
                # PAUSE-STATE evidence -- an all-null flag object with no
                # error reads as a venue that answered and was not paused.
                if "__error__" in meta:
                    row["meta_err"] = meta["__error__"]
                row["meta"] = {k: flags.get(k) for k in
                               ("trading_paused", "pause_reason",
                                "pause_resume_at", "signup_closed",
                                "untraded_purge_at")}
                # Same call, so refreshing the band is free: a change during
                # the run is caught within a minute rather than at the end.
                # This row keeps the OLD value -- it is what its books were
                # measured against -- and carries the change alongside, so
                # the boundary is visible instead of implied.
                fresh_ring = _ring_pct_from_meta(meta)
                if fresh_ring is not None:
                    # [review] Provenance updates on every successful read;
                    # only the VALUE change is an event. Tying both to
                    # `fresh_ring != ring_pct` meant a startup failure
                    # followed by a refresh reporting the default left
                    # ring_pct_src stuck on "default" forever -- reading as a
                    # guess when metadata had in fact confirmed it.
                    if fresh_ring != ring_pct:
                        row["ring_pct_changed"] = {"from": ring_pct,
                                                   "to": fresh_ring}
                    ring_pct, ring_pct_src = fresh_ring, "meta"

            fh.write(json.dumps(row) + "\n")
            fh.flush()
            if n % 60 == 0:
                print("%s  tick %d" % (row["ts"][11:19], n), flush=True)

            time.sleep(max(0.5, TICK_S - (time.time() % TICK_S)))

    print("done: %d ticks -> %s" % (n, out_path), flush=True)


if __name__ == "__main__":
    main()
