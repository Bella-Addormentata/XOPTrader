# Permuto Capital perps competition — scoping

Working document for whether and how XOPTrader competes at
<https://perps.permuto.capital>. Separate from `TODO.md`, which tracks the
live Chia bot.

**Status: scoped, nothing built — and the contest starts in 4 days.**
Figures read from the live venue 2026-08-26 unless noted; contest terms from
the official rules page, sharpened by the Discord archive read 2026-08-27.

> ## ⏱ THE CLOCK
>
> | | |
> | --- | --- |
> | **Sign-up closes** | Mon **31 Aug 2026, 17:00 ET** (21:00 UTC) |
> | **Balances reset** | Sun **30 Aug**, evening — venue paused until the open |
> | **Contest runs** | Mon **31 Aug 09:30 ET** → Fri **4 Sep 16:00 ET** |
> | Duration | 4.3 days, ~32 cash-session hours |
> | As of 2026-08-27 | **~4 days to start, ~4.3 to sign-up close** |
>
> **New participants cannot join after the first day of competition.** If we
> are entering at all, registration is the only thing that has a hard
> deadline, and it comes before any code needs to exist.
>
> **Prizes** (each category, paid in XCH): 1st **$15,000**, 2nd **$5,000**,
> 3rd **$2,500**. Traders and Market Makers are separate categories and one
> entrant may win in both — $45,000 total pool. **This document has been
> wrong twice; corrections are recorded rather than quietly edited out,
> because the errors are instructive.**

> ## C-00 — eligibility: the CONTEST is open to us, live trading is not
>
> The product page carries a geographic restriction, verbatim:
>
> > "Available to non-U.S. participants only. Permuto svPerps is currently
> > only open to eligible participants outside of the United States."
>
> **The official rules confirm this.** Entry is open to "legal residents of
> the 50 US states, District of Columbia, and worldwide territories". The
> non-U.S. restriction on the product page applies to **live trading**, not
> to the contest, which runs on *simulated* dollars — explicitly "not real
> funds or testnet/mainnet cryptocurrency", so the `/exchange/faucet`
> testnet wUSDC.b is a separate thing from the contest seed.
>
> Excluded: Cuba, Crimea, Donetsk, Luhansk, Iran, North Korea, **Quebec**,
> anyone under US sanctions or on Treasury blocked-persons lists, and
> employees/contractors of the Sponsor and their families.
>
> Two things to keep straight:
>
> - **Do not carry contest work into live trading** without settling
>   eligibility separately. The paper/live boundary is where the
>   restriction bites.
> - **XOPTrader is open source and used worldwide.** Any perps capability
>   built here is usable by operators who *are* eligible to trade it live,
>   whatever we do with our own account.

---

## Corrections to earlier versions of this file

1. **"The entire field is failing the gate" — WRONG.** That rested on a
   default `GET /exchange/leaderboard` fetch, which is **paginated at 20**
   while `market_makers_total` is 25+. The row called "the best" at 7.2M
   depth-seconds is sixth. The real leader sits at **84.3%** of the gate.
2. **"Monotonic — banked score cannot be taken away" — WRONG, then right
   again.** The OpenAPI text says monotonic; observation said otherwise.
   Two samples 240s apart, no intervention: leader went 252,758,105 →
   252,695,155, **−62,950**.
   **Superseded 2026-08-27.** Gene Hoffman has since said it plainly, twice
   — "The score only increases while quoting" and "Score only goes up;
   stopping quoting only stops accrual". Both statements reconcile with the
   observation once you notice the leaderboard column is `depth_5d`, a
   **trailing 5-day window**: banked credit does not decay, it *ages out*.
   The distinction matters, because the contest period (Mon 09:30 → Fri
   16:00 ET, ~102.5 h) fits inside a 120 h window — so nothing banked
   during the contest can age out before it ends. "Hold through the
   snapshot" was the wrong lesson to draw.
3. **"The oracle is synthetic" — WRONG.** Based on a bad test (continuous
   candle *series*, so I concluded no market hours). The *values* are
   frozen outside the cash session. It is clearly market-linked.
4. **"The gate costs 0.6% of capital" — WRONG.** Observed cost of reaching
   84% of the gate was **−$166,985** (a third of seed). Three MMs sit at
   exactly zero equity.
5. **"The gate is meant to be cleared during the cash session" — WRONG.**
   Inferred from Gene calibrating 300M as "≈$3,000 for about 28 hours
   (roughly 80%+ of cash-session hours)", which I read as a statement about
   *which* hours count. It was a statement about magnitude only. Carried
   ticks do accrue (confirmed 2026-08-27), so the denominator is 102.5
   hours, not 32.5 — and the depth needed falls from ~$2,564 to ~$813. The
   lesson: a figure offered as a rough guide is not a rule, and I built an
   argument on the phrasing of an aside.

---

## What the venue is

**Volatility perpetual futures**, settled on Chia. A `MARK` of 9.13% is a
vol number, not a price.

Three markets active (`GET /info/meta`), 10x max leverage, lot size 1,
impact notional 6000: `QQQ-VOL-PERP`, `NVDA-VOL-PERP`, `TSLA-VOL-PERP`.

42 markets appear in `/info/funding/predicted` — plain stock perps plus
`f`/`g` prefixed legs of Permuto's registered equity product — but none are
tradeable (`/info/l2/AAPL-PERP` → `market not found`, `/info/stats` empty).
**Watch for activation.**

## The oracle: a 60-second realized-vol estimate, resampled every 5s

The product page states what it is:

> "a single number built from price movement over the **preceding 60
> seconds** … **refreshed every five seconds** during U.S. market hours"

**This reframes every volatility number below.** A realized-vol estimate
built from sixty seconds of data carries a large standard error, so much of
the dispersion we measured is **estimator noise, not market information**.
And at a 5s refresh over a 60s window, consecutive prints share ~55/60 of
their input — they are autocorrelated *by construction*, which means the
VR(2) ≈ 1.0 "random walk" reading below is **confounded by overlapping
windows** and is not evidence that the underlying quantity wanders.

If the jitter is mostly sampling error around a slower true volatility,
quoting around a **smoothed** oracle is sound and the noise is edge rather
than toxicity. **That is now the central untested hypothesis of the whole
exercise**, and C-03 exists to settle it. Full treatment in
`docs/permuto-api-reference.md` §0.

## Measured behaviour: market-linked, session-shaped, brutally volatile

Mean intrabar range by hour, weekdays only, from hourly candles:

| hours (UTC) | mean intrabar range |
|---|---|
| 00:00–12:00 | **0.0%** |
| **13:00** (09:00 ET) | **488.8%** |
| 14:00–18:00 | 277% → 160%, decaying |
| 19:00 | 92% |
| 20:00 | 11% |
| 21:00–23:00 | ~0% |

Weekends are flat too: Sat 0.4%, Sun 0.2% intrabar, against 57–88% on
weekdays. So the oracle tracks the US cash session and is **dead for ~13
hours a day and all weekend**.

Short-horizon behaviour, 200 samples at 2s over 7.5 minutes:

| oracle | sd | VR(2) | VR(5) | VR(10) |
|---|---|---|---|---|
| QQQ-VOL | 12.5% | 0.98 | 0.99 | 0.92 |
| NVDA-VOL | 20.6% | 0.97 | 0.75 | **0.54** |
| TSLA-VOL | 25.8% | 1.00 | 1.15 | **1.20** |

Random walk at 2s across all three. They diverge with horizon: **NVDA
mean-reverts** (maker-friendly), **TSLA trends** (hostile), QQQ is a coin
flip. *One 7.5-minute sample, one regime — treat as indicative only.*

`/info/price_certificate` (documented as the BLS-signed oracle snapshot)
returns `No price certificate available yet`, so there is **no signed
provenance** to inspect. Source of the feed is undocumented.

**Historical resolution is hourly ONLY.** `?tf=60` and `?tf=300` are
accepted but ignored — both return 3600s spacing. We cannot resolve the
13:00Z spike from history and must collect our own high-resolution series.

## Scoring — C-01 answered

Two independent metrics, not summed:

1. **Eligibility:** banked balanced depth ≥ **300,000,000**
2. **Rank among eligible:** net PnL descending, ties on `user_id` ascending

```
# per market, per ~10s tick, only within ±2% of a FRESH oracle:
balanced_depth_usdc = min(bid_notional_in_band, ask_notional_in_band)
depth_seconds += Σ_markets balanced_depth_usdc × 10
```

Skill states the gate as ≈$3,000 balanced depth for ~28 hours, and the
sponsor confirms that figure verbatim in Discord. It does **not** decay
(correction 2): banked credit only rises, and the whole contest fits inside
one 5-day window.

✅ **C-0S RESOLVED 2026-08-27 — the skill is right.** The rules page says
market makers are ranked by profit "combined with liquidity scoring", which
read as a possible sum. The sponsor has settled it in Discord:

> Among prize-eligible Market Makers, standing and prizes are determined
> solely by net PnL (mark-to-market equity minus the finalized competition
> seed). **depth_seconds does not change rank order and is not combined
> with PnL.**
> — Gene Hoffman, 2026-08-11

Depth is a pure eligibility gate; rank is net PnL alone. That matches what
the API exposes (`prize_eligible` a boolean, separate from `total_pnl`).
Treat the gate as a constraint to *satisfy*, never as a score to maximise.

**The contest window changes the arithmetic.** Eligibility integrates
`[CONTEST_START, CONTEST_END]` once set, so on the face of it **depth banked
before 31 Aug does not count**. 300,000,000 over ~32 cash-session hours is
~$2,600 of balanced depth held continuously — but only ~32 of the 103
wall-clock hours are cash session, and whether *carried* (out-of-hours)
ticks credit is still unresolved. If they do not, the gate must be cleared
inside those 32 hours.

✅ **Both halves settled 2026-08-27.** `depth_seconds` **is** reset for the
contest — Gene Hoffman, "planning to do that already" — so nothing banked
in the sandbox carries in. But **carried (out-of-hours) ticks do accrue**,
so the usable window is the full **102.5 wall-clock hours**, not 32.5 cash
hours. Held continuously that is only **~$813** of balanced depth, or
**~$1,190** if we quote the four overnight sessions and stand down for the
cash open. See the Discord-archive section below.

Rules: one-sided quotes earn **zero** for that market (`min(bid, ask)`);
pauses/restarts/stale oracle advance the window and add nothing.

**There are two bands and they do different jobs.** `vol_oracle_band_pct`
(default 5) is the *legal placement* band — outside it, orders are rejected
HTTP 400. `vol_aggressive_ring_pct` (default 2) is the inner *aggressive*
ring; outside the ring only **passive** rests are allowed (bids ≤ oracle,
asks ≥ oracle). The ±2% that governs depth credit is the ring, not the band.

### The live field

| user | depth_5d | % of gate | total_pnl | equity |
|---|---|---|---|---|
| 987b9539… | 252,782,915 | **84.3%** | −166,985 | 333,015 |
| 9941a3ad… | 189,574,130 | 63.2% | **−500,000** | **0** |
| 9572d78f… | 52,321,079 | 17.4% | −496,442 | 3,558 |
| 8744712c… | 28,971,642 | 9.7% | −237,696 | 262,304 |
| 0c936334… | 18,536,850 | 6.2% | **−500,000** | **0** |
| 23dfb630… | 7,230,120 | 2.4% | +34 | 500,034 |

**Depth and PnL are strongly inversely correlated.** Everyone who banked
real depth is deeply negative; the only non-negative MM has none. Three
accounts are wiped. Top OAuth *trader* is +$1.33M on a $100k seed.

Live, not leftover: `finalized: false`, newest trade ~80s old, one trade
per ~22s on QQQ, and `market_makers_total` climbed 24 → 25 → 26 during one
working session.

## Working hypothesis: the depth gate and the danger are in different hours

Depth accrues on **resting size in-band**. In-band is trivially satisfied
when the oracle is frozen — and it is frozen ~13 hours a day plus weekends.
The 488% hour at the open is where a resting quote gets destroyed.

So the shape that fits the evidence: **bank eligibility in the carried
hours, quote the session only where statistics support it.** That would
also explain the leaderboard — accounts chasing `Σ_markets` depth during
the session, in TSLA as hard as NVDA, getting run over.

✅ **Confirmed on the load-bearing point, 2026-08-27: carried ticks do
accrue depth.** The gate can be banked out-of-hours. Quoting only the four
overnight sessions of the contest needs ~$1,190 of balanced depth held
throughout, against ~$2,564 to clear it inside the cash session — see the
Discord-archive section. (There are no weekends *inside* the contest window,
so the weekend half of the original phrasing is moot; and depth is reset at
the start, so it cannot be pre-banked either.)

**Two venue rules still cut against doing it naively**, and they are what
turn "cheap" into "cheap but not free":

- **Carried sessions cost 8× margin.** While the oracle is *carried* (the
  venue's own term for the frozen out-of-hours state), risk-increasing
  places require stressed initial margin (`VOL_CARRIED_STRESS_MULTIPLE`,
  default 8). Reduce-only is exempt. Overnight depth may be low-risk but it
  is **not low-capital**.
- **Everything is cancelled at the open.** At carried→live, *all* resting
  orders and pending triggers on that vol market are cancelled and must be
  re-quoted. So overnight depth cannot roll into the session, and — usefully
  — we cannot be caught holding stale quotes into the 488% hour even by
  accident. The venue already protects makers from the thing C-07 was
  written to avoid.

Still unresolved and decisive: **do carried ticks accrue depth at all?**
Carried is evidently not the same as stale (stale placement returns HTTP
503, carried placement is permitted at 8× IM), but whether the ~10s sampler
credits carried ticks is undocumented. The Discord archive adds evidence on
both sides without closing it — see below — and the sponsor's own
calibration of the gate against *cash-session hours* is the strongest hint
that carried ticks do not count.

A third strike against the hypothesis: the archive shows experienced
entrants **deliberately hunting resting MM size during carried hours**,
when the oracle is pinned high. Overnight depth is not quiet; it is where
the predators are.

## Clarifications from the Chia Discord (#svPerps, 2026-08-27)

⚠ **Informal.** These are statements in chat by Gene Hoffman (tagged *Chia
Employees*), not amendments to the official rules page. Where they differ
from the written rules, treat the written rules as binding and the chat as
intent. Recorded because three of them change items in this document.

**1. Pre-competition IS a warm-up, and real money comes later.** Asked
directly whether the current market is a warm-up test phase, the answer was:

> Barring any serious issues (which we don't expect) we would roll out real
> money a few weeks after the end of the competition.

So the present environment is explicitly for practice, and no real capital
is at stake until weeks *after* the contest ends. That removes the main
reason for hesitating to trade the pre-competition market.

**2. The one-account rule binds during the contest, not before.** Asked
whether multiple accounts are allowed given the rules forbid them:

> In the competition, yes - now? meh.

**3. A Trader account AND a Market Maker account appear to be fine.**
Another entrant disclosed running both (separate binaries, separate AWS
instances, no backchannel) and asked for a "cheating check". After review:

> After reviewing it looks like an honest trade that happened to hit your MM
> too but you were hitting the other MMs about equally

This reconciles with the official rules, which allow one entrant to win in
both categories. "Multiple accounts" evidently means multiple *within* a
category, not one per category.

**4. The self-trading bar is "not preferential", not "never".** Point 3 is
the useful part: the same entrant's two bots *did* trade with each other,
and that was judged acceptable because the flow was not directed at their own
MM more than at others. That is a far more implementable spec than "never
cross yourself" — and it matches their own reasoning that avoiding it
entirely would require a backchannel between the bots, which would be worse.

**5. There is a channel for pre-clearing.** An entrant can post their
addresses and ask for a review before the contest. It is also where scoring
questions get answered — **C-0S has since been resolved there** (see the
next section); two other questions have taken its place.

## From the Discord archive (searched 2026-08-27)

The `#svPerps` thread only renders the current day in the client, but
**server-wide search reaches the whole history**. Searching `depth_seconds`,
`oracle`, `carried`, `reset` and `overnight` surfaced the posts below. Same
caveat as the section above — chat, not rules — except that several of these
are the *only* place a rule is stated at all, and two of them **reverse**
rules that were published earlier.

### C-0S — ANSWERED. Depth is a pure gate; rank is net PnL alone.

Gene Hoffman, **2026-08-11 16:14**, verbatim:

> Slight update to how we're going to judge the MM prizes:
>
> Market Maker prizes. A Market Maker is prize-eligible only if, over the
> Contest Period (Monday 9:30 a.m. ET through Friday 4:00 p.m. ET), their
> banked depth_seconds is at least 300,000,000. Among prize-eligible Market
> Makers, standing and prizes are determined solely by net PnL
> (mark-to-market equity minus the finalized competition seed).
> **depth_seconds does not change rank order and is not combined with PnL.**
>
> depth_seconds. Every ~10 seconds, for each volatility market, the Exchange
> measures two-sided resting notional within ±2% of a fresh oracle as
> min(bid, ask). Those market depths are summed and multiplied by 10
> seconds. **The score only increases while quoting**; platform pause,
> deploy gaps, and missing/stale oracle add zero. Guidance: 300,000,000 is
> approximately $3,000 average balanced depth for about 28 hours (roughly
> 80%+ of cash-session hours). Larger size for less time, or smaller size
> for more time, both count.

That settles gate-then-PnL vs combined in favour of the skill, in the
sponsor's own words. **C-0S needs no Discord question.** It also pins the
Contest Period definition and is the origin of the ≈$3,000-for-28-hours
calibration the skill quotes.

### The uptime era is over — and one rule was reversed

Gene Hoffman, **2026-08-06 17:30**, announcing the change:

> **Leaderboard liquidity score is depth_seconds (not uptime %)** […]
> Score only goes up; stopping quoting only stops accrual. Operator pause /
> deploy gaps / missing oracle: window advances with zero depth (no free
> credit). **There is no 95% uptime gate and no scarce-side / sticky-spread
> qualification.** Keep two-sided size inside ±2% after fills
> (batch_upsert) so ticks keep earning depth.

Compare his **2026-07-20 21:14** market-shape post, which said the opposite:

> MM uptime: balanced quotes still ±2%/2%/$500; when |impact_premium| ≥
> 0.5%, scarce-side only counts; win requires ≥95% uptime_5d.
> **Pause / oracle gap / deploy still credit 100% up for MM uptime.**

**Free credit during outages is gone.** Anything written against the old
regime — 95% uptime, scarce-side qualification, sticky spread, the $500/side
balanced-quote test — is dead. Two consequences for us:

- The 300M must be earned in genuinely *live, oracle-fresh, quoting* time.
  Venue-side outages shrink the usable window below the contest wall clock;
  we cannot plan to the full ~32.5 cash-session hours.
- **Our own downtime is pure loss against the gate.** A second, independent
  argument for `S31` (dead man's switch), and for restart latency being a
  first-class metric rather than an ops nicety.

*Why the change happened*, from Drewski241 **2026-07-29**: he pulled the
full 257-row MM leaderboard and found **zero accounts ≥95% on any uptime
field** (best active MM ~83.6% `uptime_5d`); Round 1 had the same outcome at
a 98% threshold. Nobody could ever qualify, so the metric was replaced. That
is also the answer to "is the leaderboard leftover data?" — it is live, but
balances have been reset repeatedly (7/17, 7/20, 8/6, 8/21).

### RESOLVED: carried ticks DO accrue depth (asked 2026-08-27 15:06)

Jakub Hadamcik, **2026-08-27 15:10**, answering our question directly:

> And yes, when underlying markets are closed, you keep collecting depth
> seconds

⚠ **Entrant-sourced, not sponsor-sourced.** Jakub is a competitor, not
Permuto staff. He is also the entrant who records every trade, book change
and oracle print, has been consistently candid in the channel, and gains
nothing by telling the field the gate is easier than it looks. Treat it as
strong but verifiable — see "Verify it ourselves" below.

**This reverses the inference in the previous draft.** The argument against
accrual was that Gene calibrates the gate as "≈$3,000 average balanced depth
for about 28 hours (roughly 80%+ of cash-session hours)", which reads as
though only the cash session counts. That was over-reading a figure offered
as a rough magnitude. The evidence *for* accrual — Gene's own "vol perps
keep a **continuous carried IV oracle** so they can trade around the clock"
(2026-06-11) and the band being "±50% around the **live (or carried)**
oracle IV" (2026-06-17) — was the better guide. Carried is a real oracle,
and a real oracle is a fresh one.

**The arithmetic changes a lot.** The gate is 300,000,000 depth-seconds over
a 102.5-hour contest window:

| Quote during | Seconds | Balanced depth needed, held throughout |
| --- | ---: | ---: |
| Whole contest window (102.5 h) | 369,000 | **~$813** |
| Carried hours only (4 nights, 70 h) | 252,000 | **~$1,190** |
| Cash session only (32.5 h) | 117,000 | ~$2,564 |

Gene's own calibration checks out against this: $3,000 × 28 h × 3,600 =
302,400,000. The 28 hours was never a claim about *which* hours.

**So the "bank the gate in the quiet hours" shape is back on**, and it is
now the cheapest route to eligibility by a factor of ~2 against quoting the
cash session. Two things still argue against doing it naively:

- **8× stressed initial margin** on risk-increasing places while carried.
  Cheap in *depth* terms is not cheap in *capital* terms.
- **It is the identified hunting ground.** See the adversarial-precedent
  section below: entrants explicitly short resting MM size overnight when
  the oracle is pinned high. Small, balanced, and reduce-only-biased is the
  shape that survives; a fat overnight grid is what gets farmed.

**Verify it ourselves before relying on it.** The leaderboard is public and
unauthenticated. Sample `depth_5d` for two or three actively-quoting MMs
just after a 16:00 ET close and again ~90 minutes later. A clear rise
confirms accrual. Note the 5-day window cuts both ways — roll-off can mask
a small gain — so prefer accounts with large recent accrual and use a long
enough gap that accrual dominates.

### RESOLVED: depth_seconds IS reset for the contest

Jakub Hadamcik, **2026-08-26 15:45**, replying to the reset announcement:

> Ideally reset also depth seconds because competition is less than 5 days
> long, it would count pre-competition depth seconds as well

Gene Hoffman, **2026-08-26 15:45**:

> **planning to do that already**

**So pre-competition depth does not carry in**, and this document's original
assertion — "depth banked before 31 Aug does not count" — is correct after
all. There is no weekend-banking shortcut. The full 300,000,000 has to be
earned inside `[CONTEST_START, CONTEST_END]`.

*How this was missed:* the answer is four words long and contains none of
the terms worth searching for. It was found only by following the message
link in Jakub's reply. Keyword search over a chat archive systematically
misses short answers — check the replies to the *question*, not just the
keyword.

### The reset schedule

- Gene, **2026-08-26 15:37**: "We will reset balances Sunday evening during
  a trading pause that will un-pause when the competition starts." So:
  **paused from Sunday 30 Aug evening until Monday 31 Aug 09:30 ET.**
- Gene, **2026-08-27 11:40**: "all accounts will be reset to their starting
  balances shortly before the 2nd competition starts".
- Gene, **2026-08-21 15:40**: "Balances reset, market open, everyone who
  signed up since 8/6 is still in. **Taking new entrant sign ups through
  Monday, August 31 at 5 PM ET**" — independently confirms the sign-up
  deadline in THE CLOCK above.
- Still unanswered (codingisart, **2026-08-27 01:26**): does the reset
  *flatten positions* and cancel resting orders, or only zero balances and
  PnL? Assume flat-at-the-bell; build nothing that depends on carrying
  inventory through it.
- Useful: the sponsor has granted **individual equity resets on request** in
  the sandbox (Ethaneal, 2026-07-22, after going bust). Blowing up in
  practice is recoverable — one more reason to trade the warm-up hard.

### Venue mechanics stated only in chat

Not in the rules page; these belong in `docs/permuto-api-reference.md` when
that document is next revised.

**Mark for uPnL** — Gene, **2026-08-26 18:16**:

> uPnL is against oracle plus a small, rate-limited book-mid basis — not
> last price, not raw mid, and not raw oracle unless the basis is at zero
> (empty/out-of-band book)

Since MM standing is *solely* net PnL and PnL is marked this way, the mark
cannot be walked by posting a lopsided book: the basis is rate-limited and
anchored to the oracle. That rules out the obvious mark-painting exploit —
and it also means our equity curve moves on oracle prints we never traded.

**Notional caps** — Gene, **2026-07-20 21:14**: risk-increasing exposure
capped at **$500k/market and $1M portfolio** (position *plus* risk-increasing
resting); reduce-only exempt. Place/modify/trigger/batch share the same
LimitRest and notional gates — expect **HTTP 400** out of band or over cap.
A grid sized for the gate has to fit under the portfolio cap across all
markets at once.

**Funding V3 (H=2)** — Gene, **2026-07-20 21:19**: hourly =
`clamp(impact_premium / 2, ±10%)` with **no interest-rate clamp**; VOL
markets settle every **60 s**, rate scaled to the interval. Funding tracks
the skew we actually quote into, so a persistently one-sided book is charged
for it every minute.

**ADL** — after every forced-liquidation flat, auto-deleveraging closes
opposite OI **at mark**, uPnL-ranked and pro-rata. Our winning positions can
be closed by someone else's liquidation. Visible in trade history as mark
fills even when insurance covers the cash.

### The three bands, stated cleanly

Drewski241, **2026-07-23 22:17**, the clearest summary anyone has written:

> ±5% is where orders are allowed to live; ±2% ring controls whether you can
> quote aggressively or must quote passively; ±2% proximity is what the MM
> leaderboard counts […] **If your oracle is stale, you will place orders
> that look fine to you but get cancelled or score zero against the host's
> real-time oracle.**

That last sentence is a design requirement, not a warning. Our quoting loop
must be driven by **the venue's oracle over WS**, never by a locally
resampled or cached copy — a stale local oracle produces orders we believe
are scoring and that are in fact worth nothing. Aggressive rests outside the
ring "reject at place and **purge on oracle move**", so a lagging oracle also
silently empties our book.

### Adversarial precedent: the off-hours squeeze is a known play

Jakub Hadamcik, **2026-07-29 16:34**, describing it openly:

> I just shorted whatever MMs provided me with since NVDA oracle is 41%.
> This allows me to collect very profitable shorts and they have to (again)
> choose between uptime or profit. […] Funding doesn't justify it for longs
> because it stabilized at -0.014676% costing me about $3750/h

Ethaneal, **2026-07-30**: "especially when it is a two minute oracle spike".
Jakub again, **2026-08-05**: "I managed to not get liquidated today on market
open just because I got super lucky on market pop being low enough. But I was
carrying huge short which I wasn't able to release anywhere."

Sophisticated entrants deliberately hunt resting MM size during carried hours
and at the open. Combined with the 8× stressed initial margin off-hours,
**quoting meaningful size overnight is the identified way to lose.**

This does *not* kill the bank-the-gate-overnight plan — carried ticks are
confirmed to accrue, and the gate only needs ~$1,190 of balanced depth held
across the four contest nights. It sets the *size*: the gate is cheap enough
that we never need a fat overnight grid, and a fat overnight grid is exactly
what gets farmed. Quote the minimum that clears the gate, biased reduce-only,
and treat any overnight fill as information rather than inventory to defend.

### Questions asked in `#svPerps` — posted 2026-08-27 15:06, both ANSWERED

Both were unanswered by anyone and both changed strategy materially. Posted
as a single message at 15:06; first reply at 15:09, both answered by 15:10.
The channel answers this kind of question fast — use it. Posted:

> Two questions on depth_seconds accounting, if I may. (1) Do carried
> (out-of-hours) ticks accrue depth_seconds - is a carried IV oracle treated
> as "fresh" by the ~10s sampler, given that placement seems to treat live
> and carried alike? (2) Will depth_seconds be reset on Sunday along with
> balances? That was raised on the 26th but I didn't see an answer - since
> the contest is shorter than the 5-day window, pre-competition depth would
> otherwise carry into the 300,000,000 gate. Asking now because the two
> answers imply quite different overnight sizing. Thanks!

**The answers:**

1. **Carried ticks DO accrue.** Jakub Hadamcik, 15:10 — "And yes, when
   underlying markets are closed, you keep collecting depth seconds."
   *Entrant, not staff* — verify (C-0S3).
2. **`depth_seconds` IS reset.** Jakub pointed at Gene Hoffman's reply to
   his own 2026-08-26 request — "planning to do that already". Sponsor,
   authoritative. No pre-banking.

Net: no shortcut before Monday, but the gate is roughly a third as expensive
as the cash-session-only reading implied. Both revisions are worked through
in the two sections above.

## Conduct rules that bind a two-sided quoter

From the official rules, disqualifying conduct includes **wash trading,
self-dealing, collusion**, operating **multiple accounts**, false
information, and tampering with Exchange operations.

⚠ **Self-trading is a live risk for us specifically.** A two-sided quoter
with a bid and an ask on the same market *can* cross itself when the oracle
moves through both — and the API skill references "opposite-side self-trade
refresh" as a real scenario in the `batch_place` docs. That is exactly the
shape of accidental self-dealing. Any quoting loop needs an explicit
self-trade guard before it goes near this contest, not after.

**One account only.** No hedging our own entry with a second identity, and
no separate "test" account running alongside.

## THE TRAP: quote-only accounts get wiped

> Bots that only quoted lose session, API keys, and **seed**. Re-signup is
> impossible while `signup_closed` stays true.

`flags.untraded_purge_at` is the clock; ≥1 real fill required (cancels and
IOC misses do not count). Currently `null`, `signup_closed: false`.

## Connecting: API, not an embedded browser

Unattended bots need no Sage UI and no browser:

| step | call |
|---|---|
| 1 | `POST /exchange/wallet_link_challenge` → `{challenge_token, nonce}` |
| 2 | *(local)* AugSchemeMPL over 32 decoded nonce bytes |
| 3 | `POST /exchange/wallet_auth` → session token |

Poll `flags.signup_closed` **before** step 1 (403 for a new identity).
Verified public read access, no auth: `/health`, `/info/*`,
`/exchange/leaderboard`, `/exchange/session`.

**Needs a local BLS private key** — a new class of secret here. Operator
decision.

### GUI: a Permuto tab

Not an embedded web view — that is a second UI we do not control and cannot
drive a bot from. Show: connection/session state; per-market oracle, mark,
our quotes, **in-band or not**; **depth-seconds against 300,000,000 with
current accrual rate**; net PnL vs the $500k seed; positions and
liquidation distance; `untraded_purge_at` countdown; kill switch.

## Analysis mode

The existing startup analysis (`startup_analysis_blocks`, `MarketAnalyzer`,
`BotStatus::Analyzing`) is **block-driven and dexie-specific** — it counts
Chia blocks and ingests dexie tickers. It cannot be pointed at Permuto
as-is.

But the *concept* is exactly right, and the venue forces it: history is
hourly only, so the intra-hour behaviour that decides everything can only
be obtained by sampling ourselves. **Run an analysis-mode observer
continuously until the contest starts** — no auth, no key, no capital, no
risk.

It should record, per market: oracle at 1–2s resolution; realised variance
ratios by hour-of-day; L2 depth and spread; what depth-seconds we *would*
have banked under a candidate quoting rule; and the realised PnL that rule
*would* have taken. That last one is the number that matters — it is what
the zero-equity accounts did not know before committing.

## Session timing: stop at the open, not the close

Asked whether to pause around both. The data says the risk is **not
symmetric**:

- **13:00Z open — 488.8% mean range.** Stand down. This single hour is the
  most likely explanation for the wipeouts.
- **Close is calm** — 19:00Z at 92%, 20:00Z at 11%, decaying monotonically
  into it. Unlike a real equity close there is no auction spike *visible at
  hourly resolution*.

Caveat: hourly bars would hide a 5-minute closing spike, and `tf` is
ignored, so **this cannot be confirmed from history** — it is a specific
thing for the analysis-mode observer to settle.

## Starting before the competition

Possible: `signup_closed: false`, trading live, competition mode already on.

Two reasons for care:

1. **Pre-contest depth probably does not count.** Eligibility uses the
   rolling 5d bank *only while `CONTEST_START` is unset*; once set it
   integrates the contest window.
2. **The seed is one-shot.** `withdrawals_enabled: false`, re-signup blocked
   once closed, three accounts already at zero. Practising with the seed
   risks entering the real contest broke.

**Recommended:** start observation immediately (free), and spend *seed*
early on exactly one thing — a single qualifying fill to survive the
untraded purge. (Seed, not real money: the contest runs on simulated
dollars with `withdrawals_enabled: false`. The cost of that fill is
contest standing, not capital.)

## Why this is worth doing even if we never place a live perp order

The contest is paper money, so the direct payoff is a prize. **The durable
payoff is capability that flows back into the dexie bot** — and several
things Permuto forces on us are gaps XOPTrader already has.

| capability | why Permuto forces it | value back on dexie |
| --- | --- | --- |
| **Dead man's switch** | `schedule_cancel` is first-class; quoting without one is reckless | **The clearest win.** On 2026-08-25 the engine sat wedged ~4h with live offers, then six stale bids were picked off the moment the node returned. "No completed cycle in N minutes ⇒ cancel everything" would have prevented it outright. Filed as **S31** in `TODO.md` |
| **Paper / observer mode** | history is hourly only, so a rule must be scored on live data before risking capital | XOPTrader has **no paper mode**; every strategy change ships straight to a live book |
| **Jump-aware estimation** | the oracle is a jump process and continuous-path models are misspecified | Dexie CATs jump too — the 100× outlier at `price=0.013810` is the same shape. Feeds S20 |
| **Estimator noise vs information** | the oracle is a 60s RV estimate, so much of its jitter is sampling error | The same question S20 asks: junk print, or real repricing? |
| **Balanced two-sided depth** | `min(bid, ask)` — a lifted side earns zero | We do not track whether our dexie book is two-sided; one-sided quoting is invisible today |
| **Batch quote maintenance** | `batch_upsert` restores a lifted side in one call | Step 8's cancel-and-repost is two operations with a window between |

The perps-specific work — funding, margin, liquidation — does **not**
transfer and must not leak into shared code.

## Sequenced work

- [x] **C-00** Eligibility — confirmed by the official rules: open to legal
      residents of the 50 US states, DC and territories. The non-U.S.
      restriction applies to **live trading**, not the contest. Do not cross
      that boundary without settling it separately.
- [ ] **C-0R** **REGISTER, or decide not to enter.** Sign-up closes Mon
      **31 Aug 17:00 ET** and no one may join after day one. This is the
      only hard deadline and it does not require any code. **Operator
      decision, and the first one.**
- [x] **C-0S** MM rank is *gate-then-PnL*. Answered in the #svPerps
      Discord, 2026-08-11: "depth_seconds does not change rank order and is
      not combined with PnL." The agent skill was right; the rules page
      phrasing is loose.
- [x] **C-0S2** **ANSWERED within four minutes of asking, 2026-08-27.**
      (a) **Carried out-of-hours ticks DO accrue `depth_seconds`** — "when
      underlying markets are closed, you keep collecting depth seconds"
      (Jakub Hadamcik, an entrant, not staff — verify before betting on
      it). (b) **`depth_seconds` IS reset** for the contest — Gene Hoffman,
      "planning to do that already". So there is no weekend pre-banking,
      but the usable window is the full 102.5 h, which drops the depth
      needed from ~$2,564 to ~$813 held throughout.
- [ ] **C-0S3** **Verify (a) ourselves from the public leaderboard.** Sample
      `depth_5d` for two or three actively-quoting MMs just after a 16:00 ET
      close and again ~90 min later; a clear rise confirms accrual. No auth
      needed, no code beyond a fetch. Do this before sizing anything on it —
      the whole eligibility plan now rests on one entrant's statement.
- [x] **C-01** Gate quantified. ≈$3,000 balanced depth for ~28 hours,
      **not decaying** — the sponsor confirms the accumulator only rises;
      what falls is the 5-day display window.
- [ ] **C-02** Auth path + BLS key handling. **Operator decision.**
- [ ] **C-03** Analysis-mode observer (read-only, no key). Settles: **is the
      short-horizon jitter estimator noise or information** (the central
      question, given the 60s/5s overlapping-window construction); do
      carried ticks credit depth; per-market session statistics; would-be
      PnL of a candidate rule. Endpoints listed in
      `docs/permuto-api-reference.md` §7. **Do first, after C-00.**
      **Use a range-based estimator, not variance ratios.** Permuto serves
      OHLC, so Parkinson / Garman–Klass computed from the same bars gives an
      independent and better-conditioned estimate of the same quantity — and
      the *difference* from the oracle measures the estimator noise directly
      instead of inferring it. See `docs/advanced-trading-methods.md` §4,
      "Ideas taken from two external repositories". Separate the session from
      the carried hours in the estimator rather than smoothing across them.
- [ ] **C-04** Perps position model — positions, margin, funding,
      liquidation distance.
- [ ] **C-05** Execution adapter — `order`/`modify`/`cancel`/
      `batch_upsert`/`cancel_all`, place vs mutate tokens.
      **Drive it from the venue's oracle over WS, never a local copy.** A
      stale local oracle produces orders that look fine to us, get purged or
      rejected by the host, and score zero — the failure is silent on our
      side. Prefer `batch_upsert` for grid refresh, and re-quote on
      carried→live because the sequencer cancels everything at the open.
- [ ] **C-06** Risk layer — reuse breakers; add liquidation proximity and a
      funding budget. Never let a position price at 0 in silence. Budget
      funding at `clamp(impact_premium/2, ±10%)` per hour settling every
      60 s, and model **ADL**: a forced-liquidation flat anywhere closes
      opposite OI at mark, so our winners can be closed by someone else's
      blow-up. Respect the $500k/market and $1M portfolio risk-increasing
      caps or every place returns HTTP 400.
- [ ] **C-07** Session scheduler — stand down at 13:00Z; quote per-market
      on measured statistics, not by rule.
- [ ] **C-08** Secure the qualifying fill early (untraded purge).
- [ ] **C-10** **Self-trade guard — re-scoped.** Still needed, but the bar
      per Discord is *not preferential*, not *never*: incidental crossing
      between one entrant's own Trader and MM accounts was reviewed and
      accepted because the flow hit other MMs about equally. So the
      requirement is to avoid systematically directing flow at our own
      quotes, and to be able to SHOW that if asked — not to make crossing
      impossible. Much cheaper than the original framing.
- [ ] **C-09** Poll `CONTEST_START` / `untraded_purge_at` / `signup_closed`
      continuously; all three change the plan when they flip.

## Operator decisions

- Compete at all, given every MM with real depth is deeply negative.
- Where the BLS signing key lives; fresh identity or existing.
- Testnet vs mainnet — prizes may require mainnet; unconfirmed.
- **Keep competition code behind an adapter boundary?** Still recommended,
  but for a sharper reason than "the live bot is fragile": the *strategy
  and observation* layers are what we want flowing back to dexie, while
  funding/margin/liquidation must not. The goal is a boundary that lets the
  first cross and blocks the second — not a wall.

## Companion documents

- `docs/permuto-api-reference.md` — all 59 API routes, what each does, how
  we would use it, and the constraints that bind a market maker.
- `docs/advanced-trading-methods.md` §4 — the SVPerp literature, what
  transfers, and which published methods cannot run on this venue.

## Sources

- <https://perps.permuto.capital> — in-app API docs; `/llms.txt`
- `/.well-known/agent-skills/perps-market-maker-auth/SKILL.md` — auth, "How to win"
- `/.well-known/agent-skills/perps-api/SKILL.md` — trading endpoints, limits
- `GET /info/meta`, `/info/oracle`, `/info/candles`, `/exchange/leaderboard` — read 2026-08-26
- Chia Network Discord, `#permuto-capital` › *svPerps* thread — live thread
  read 2026-08-27; full history reached by server search on `depth_seconds`,
  `oracle`, `carried`, `reset`, `overnight`. Statements by Gene Hoffman
  (tagged *Chia Employees*) are treated as sponsor intent, not as rules.
