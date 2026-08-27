# Permuto Capital perps competition — scoping

Working document for whether and how XOPTrader competes at
<https://perps.permuto.capital>. Separate from `TODO.md`, which tracks the
live Chia bot.

**Status: scoped, nothing built — and the contest starts in under 5 days.**
Figures read from the live venue 2026-08-26 unless noted; contest terms from
the official rules page.

> ## ⏱ THE CLOCK
>
> | | |
> | --- | --- |
> | **Sign-up closes** | Mon **31 Aug 2026, 17:00 ET** (21:00 UTC) |
> | **Contest runs** | Mon **31 Aug 09:30 ET** → Fri **4 Sep 16:00 ET** |
> | Duration | 4.3 days, ~32 cash-session hours |
> | As of 2026-08-26 23:06 UTC | **4.6 days to start, 4.9 to sign-up close** |
>
> **New participants cannot join after the first day of competition.** If we
> are entering at all, registration is the only thing that has a hard
> deadline, and it comes before any code needs to exist.
>
> **Prizes** (each category, paid in XCH): 1st **$15,000**, 2nd **$5,000**,
> 3rd **$2,500**. Traders and Market Makers are separate categories and one
> entrant may win in both — $45,000 total pool. **This document has been wrong twice; corrections
are recorded rather than quietly edited out, because the errors are
instructive.**

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
2. **"Monotonic — banked score cannot be taken away" — WRONG.** The
   OpenAPI text says monotonic; observation says otherwise. Two samples
   240s apart, no intervention: leader went 252,758,105 → 252,695,155,
   **−62,950**. Eligibility is a rolling window you must *hold through the
   snapshot*, not a bank you fill once.
3. **"The oracle is synthetic" — WRONG.** Based on a bad test (continuous
   candle *series*, so I concluded no market hours). The *values* are
   frozen outside the cash session. It is clearly market-linked.
4. **"The gate costs 0.6% of capital" — WRONG.** Observed cost of reaching
   84% of the gate was **−$166,985** (a third of seed). Three MMs sit at
   exactly zero equity.

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

Skill states the gate as ≈$3,000 balanced depth for ~28 hours. **But it
decays** (correction 2), so it must be *held*, not banked and abandoned.

⚠ **The official rules and the agent skill do not obviously agree.** The
rules say market makers are ranked by profit "**combined with** liquidity
scoring", while the skill is explicit that the two metrics are "**not**
added into one score" — depth is an eligibility gate, then rank is by net
PnL alone. The skill is the more specific document and is what the API
actually exposes (`prize_eligible` is a boolean, separate from `total_pnl`),
but this is worth clarifying with the Sponsor before optimising for either
reading. They imply different strategies.

**The contest window changes the arithmetic.** Eligibility integrates
`[CONTEST_START, CONTEST_END]` once set, so **depth banked before 31 Aug
does not count**. 300,000,000 over ~32 cash-session hours is ~$2,600 of
balanced depth held continuously — but only ~32 of the 103 wall-clock hours
are cash session, and whether *carried* (out-of-hours) ticks credit is still
unresolved. If they do not, the gate must be cleared inside those 32 hours.

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

So the shape that fits the evidence: **bank eligibility overnight and at
weekends, quote the session only where statistics support it.** That would
also explain the leaderboard — accounts chasing `Σ_markets` depth during
the session, in TSLA as hard as NVDA, getting run over.

**This is a hypothesis, and it is now materially weaker than when first
written.** Two venue rules found in the API skill on 2026-08-26 cut against
it:

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
credits carried ticks is undocumented.

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

**Recommended:** start observation immediately (free), and spend real money
early on exactly one thing — a single qualifying fill to survive the
untraded purge.

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
- [ ] **C-0S** Clarify with the Sponsor whether MM rank is *gate-then-PnL*
      (agent skill) or *PnL combined with liquidity score* (official rules).
      They imply different strategies. Cheap to ask, expensive to guess.
- [x] **C-01** Gate quantified. ≈$3,000 balanced depth, but **decaying**.
- [ ] **C-02** Auth path + BLS key handling. **Operator decision.**
- [ ] **C-03** Analysis-mode observer (read-only, no key). Settles: **is the
      short-horizon jitter estimator noise or information** (the central
      question, given the 60s/5s overlapping-window construction); do
      carried ticks credit depth; per-market session statistics; would-be
      PnL of a candidate rule. Endpoints listed in
      `docs/permuto-api-reference.md` §7. **Do first, after C-00.**
- [ ] **C-04** Perps position model — positions, margin, funding,
      liquidation distance.
- [ ] **C-05** Execution adapter — `order`/`modify`/`cancel`/
      `batch_upsert`/`cancel_all`, place vs mutate tokens.
- [ ] **C-06** Risk layer — reuse breakers; add liquidation proximity and a
      funding budget. Never let a position price at 0 in silence.
- [ ] **C-07** Session scheduler — stand down at 13:00Z; quote per-market
      on measured statistics, not by rule.
- [ ] **C-08** Secure the qualifying fill early (untraded purge).
- [ ] **C-10** **Self-trade guard.** A two-sided quoter can cross its own
      bid and ask when the oracle moves through both; wash trading and
      self-dealing are disqualifying. Must exist before the first quote,
      not after.
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
