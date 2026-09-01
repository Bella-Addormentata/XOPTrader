# Price discovery from trade history: why the tape is not a live input

Research note, **2026-09-01**, against v0.10.12 (branch
`fix/per-side-book-quality`, based on `main` e4ec93d).

**Verdict up front.** The XCH/BYC tape is right -- it says ~1.40 BYC/XCH,
which is BYC at par -- and we are still not going to feed it to the pricing
engine. Not because it is wrong, but because the engine already reaches
1.41 by a road that carries honest error bars, and because a history-fed
edge would *displace* that road rather than confirm it. Sections 3 and 5
are the argument; section 6 is the one thing the tape is actually good for.

This note exists so the next person to have this idea can re-derive the
answer in twenty minutes instead of re-litigating it for a week.

---

## 1. The question

On 2026-09-01, with XCH at 1.43 USD, the live XCH/BYC book on dexie looked
like this. The pair is configured `base=XCH, quote=BYC`, so every price
below is **BYC per XCH**.

| Side | Ladder (BYC/XCH)                              | Implied BYC/USD |
|------|-----------------------------------------------|-----------------|
| Ask  | **4.9995**, 5.0000, 9.7500, 10.0000 x3        | 0.286 at best   |
| Bid  | **1.5000**, 1.4283, 1.4066, 1.3793, 1.3699, 1.3514 | 0.953 at best |

Midpoint 3.2498. Spread 10,769 bps.

The bid ladder is a market. Six levels, monotone, spanning 1.3514 to
1.5000 -- about 1,000 bps top to bottom, which for a thin CAT pair is a
believable depth profile from participants who want XCH and are willing to
pay near par for it. The ask ladder is not a market. 4.9995 asks 3.5x
fair value for XCH; 10.0000 asks 7.1x. Nobody is going to lift those, and
nobody placed them expecting to be lifted.

Three live defects follow directly from taking the 3.2498 midpoint
seriously, all detailed in section 8: the whole pair gets suppressed every
block, our correctly-priced ask gets classified as aggressive against a
4.9995 reference, and the competitive anchor parks a bid at ~1.5001 against
a 1.4102 fair value -- a standing ~6.3% overpay on every XCH bought.

So the question, asked concretely: **dexie publishes completed offers, and
`trade_log` holds 632 XCH/BYC rows and 47 BYC/wUSDC.b rows. Why not
compute the price from the tape and use that instead of a midpoint we know
is broken?**

## 2. The empirical answer: the tape is right, and it is still unusable

The tape clusters at ~1.40 BYC/XCH. At XCH 1.43 USD that implies BYC at
1.021 USD. BYC's declared par is 1.00. The tape agrees with par to about
2%. Every part of the premise checks out: the trade history does say
roughly the right thing, and the book midpoint does not.

Three reasons that does not make it a live input.

**Staleness, and a freshness clock that cannot help.** There have been zero
BYC trades on *any* pair in the last 24 hours. Worse, we could not reliably
detect it if there had been: the last-trade freshness clock
(`cpp/src/execution/market_data.cpp:75-89`, stamped at 567-569) advances
only on a price *change*. That is deliberate and documented in the header
comment there -- a polled ticker cannot distinguish "traded again at the
same price" from "the previous print re-reported", so an unchanged value
proves nothing and correctly leaves the clock alone. It is a real
limitation, not a bug, and it is not fixable without a true per-trade
timestamp from the venue. Net effect: on a quiet pair, "the tape says 1.40"
is a claim whose age we cannot bound from below.

**The denominator is broken.** The only market route from BYC to a dollar
runs through wUSDC.b. The newest BYC/wUSDC.b print is 2026-08-24 -- eight
days stale -- and wUSDC.b is itself depegged after the 2026-08-25
warp.green bridge compromise, measured at ~0.80 USD on 2026-09-01 through
the XCH/wUSDC.b cross, which is itself a 2026-08-29 mid and therefore stale
too. A cross through a depegged
stablecoin with an eight-day-old print is not a weak measurement of BYC/USD;
it is a measurement of something else. Nothing in `docs/` carries the depeg
history -- `docs/warp-bridge.md` is a USDC(Base) -> wUSDC.b bridging runbook
and never mentions the compromise. The written record is `TODO.md` S30
("both issuers of our quote assets were compromised within a day") and the
bridge-compromise memory.

**The book is one-sided, not wide.** This is the load-bearing one, and it
is the point section 4 formalises. A 10,769 bps spread invites the reading
"the market is very uncertain about this price". It is not. One side of
this book is a market and the other side is empty, with six resting offers
of the fat-finger/wishful-price variety left lying in it. Any method that
combines the two sides -- a midpoint, a micro-price, a spread estimator, a
"tape versus book" reconciliation -- is combining a quote with the absence
of a quote and reporting the average as a price.

## 3. The central point: the engine is already there, by a better road

`fv::blend_quote_center` (`cpp/src/engine.cpp:6219-6249`) combined the
CoinGecko XCH/USD feed with BYC's **declared par** and produced
XCH/BYC = **1.41022765 at sigma 171 bps** (171 = sqrt(100^2 + 140^2)), then
replaced the book mid 3.2498 with 1.41141912 at `w_ext = 1.00`. It reached
the same ~1.41 the tape implies, using no trade history at all.

That is not a happy coincidence, and it is the reason the tape adds nothing:

> The ~1.40 tape cluster and the 1.41 model output **share the par input**.
> Their agreement is corroboration, not independent confirmation.

Both numbers are downstream of the same assumption -- that BYC is worth a
dollar. If that assumption is wrong, they are wrong together, in the same
direction, by the same amount, and neither will flag it.

But the deeper objection is not that the tape is redundant. It is that
wiring it in would make the engine's uncertainty accounting *worse*, in two
specific ways that are both properties of code as it stands today.

**(a) The par input is already spent, and priced.** BYC's par enters the
solve as an anchor carrying `fair_value_par_market_sigma_bps = 140`
(`cpp/include/xop/config.hpp:447-462`). That 140 is not arbitrary: the
comment there distinguishes a *wrapper* par (par by construction -- one unit
is a claim on one currency unit, `fair_value_par_sigma_bps = 100`) from a
*market-determined* par like BYC's, where "par is an aim the market usually
honours (7-day VWAP 1.001) but does not owe us, so the bar is wider". The
140 bps **is** our stated uncertainty about the peg. Adding the tape as a
second anchor whose value derives from the same peg would combine two
correlated observations in quadrature as though they were independent: the
reported sigma shrinks while the actual epistemic uncertainty -- *is BYC at
par right now?* -- is unchanged. We would be manufacturing confidence out
of an assumption we already counted.

**(b) The par anchor is FALLBACK-ONLY, so a history edge would displace it,
not confirm it.** `cpp/src/engine.cpp:5959-5970`:

```cpp
// [PARANCHOR] FALLBACK: only a solve that found NO path may
// consult the declared pars.  A solved pair -- however wide --
// never sees them, so market evidence always outranks a
// declaration.
if (!sol.ok && !par_fallback_anchors.empty()) {
```

Read that against the proposal. A trade-history edge is market evidence by
type. Feed it in and the pair now *solves*. `sol.ok` is true, the
`par_fallback_anchors` branch never runs, and the governed 140 bps par
anchor is never consulted. The engine's fair value for XCH/BYC would then
rest entirely on an edge which (i) is built on BYC's par anyway, (ii) is
of unknown age because of the freshness clock in section 2, and (iii) wears
a market edge's error bars rather than par's deliberately wide ones.

Same information in, smaller stated uncertainty out, and the `[PARANCHOR]`
fallback discipline silently bypassed. That is strictly worse than the
current behaviour even in the best case where the tape is fresh and honest.

**This is the whole argument.** Everything below is either the theory that
explains why, the list of standard techniques that additionally fail at our
sample size, or the one technique that survives.

## 4. What the literature actually licenses here

Not a reading list. Each of these is here because it settles something.

### Hasbrouck (1993): 4.9995 is a quote and has never been a print

Hasbrouck decomposes an observed transaction price into a random-walk
efficient price and a zero-mean stationary **pricing error**, and proposes
the standard deviation of that error (sigma_s) as a market-quality measure.
It is a decomposition of **transaction** prices.

Apply it here and the junk ask side simply is not in the sample. 4.9995 has
never traded. There is no pricing error to decompose because there was no
price. This is a sharper statement than "the ask is noisy evidence": the
ask is **not evidence at all** in Hasbrouck's sense, and no amount of
weighting rehabilitates it.

The framework also delivers the good news precisely. On the side that does
trade, the honest bid ladder (1.3514 .. 1.5000) and the ~1.40 tape cluster
are mutually consistent to a few hundred bps. Our pricing error on the
tradeable side is small. The 10,769 bps "spread" is not a measure of our
uncertainty; it is a measure of how far away the nearest resting fiction is.

### Glosten and Milgrom (1985): an adverse-selected side is ABSENT, not wide

Glosten-Milgrom derive bid and ask as **conditional expectations of value
given that a trade arrives on that side**. The ask is E[V | someone buys].
When adverse selection on a side becomes severe enough, the zero-profit
quote on that side does not become wide -- it fails to exist; the market
maker withdraws rather than post it. Market breakdown is an equilibrium
outcome of the model, not a failure of it.

That is the formal statement of section 2's third point, and it is why the
one-sided reading is not editorial. The 4.9995-to-10.0000 ladder is not
participants demanding a 250% premium to sell XCH; it is **nobody offering
XCH**, plus residue. Consequently `(best_bid + best_ask) / 2` is a category
error on this book: it averages a conditional expectation with a
non-existent one. Every threshold measured against that midpoint --
`bbo_sanity` Check 1 in Step 8 of `cpp/src/engine.cpp`, the `bid_cap` in
`cpp/src/strategy/liquidity.cpp:1044-1056` -- is comparing a real number to
a number with no referent.

Kyle (1985) fails here for the same reason and harder: lambda, price impact
per unit of order flow, presumes a continuous two-sided order flow into
which an informed trader can hide. There is no order flow on the ask side
to have an impact on.

### Merton (1980): more tape cannot pin a level -- this is the cleanest one

Merton's exploratory investigation of expected market return separates two
estimation problems that look similar and are not:

* the precision of a **volatility** estimate improves with finer sampling
  of a fixed calendar span -- more observations inside the window genuinely
  help;
* the precision of a **location** parameter (a mean, a level) depends on
  the **length of the sample period**, not on the observation count within
  it. Sampling the same week more finely does not tell you where the price
  is; it tells you how much it wiggles.

This is the single most useful sentence in the literature for our purposes.
The 632 XCH/BYC rows in `trade_log` can, in principle, tell us how much
XCH/BYC moves. They **cannot** tell us where XCH/BYC is right now, and
adding rows within the same window does not change that. The failure is not
"we need more trades"; it is that the tape is the wrong instrument for the
measurement.

The fix for a level is a second **independent** measurement. That is
exactly what the CoinGecko XCH/USD feed is, and it is exactly what
`cpp/include/xop/execution/mid_gate.hpp` already formalises in its
anchor-selection chain: the gate compares a candidate mid "against an
INDEPENDENT anchor -- never the pair's own history". A trade-history input
is, definitionally, the pair's own history. The architecture already
answered this question; Merton explains why the architecture is right.

### Easley-O'Hara (1992) and Dufour-Engle (2000), with the short-sale counterweight

Easley and O'Hara make time informative: in their sequential-trade model,
the *absence* of trade is itself a signal, weakening the posterior that an
information event occurred. Dufour and Engle extend this empirically --
price impact per trade declines as the duration between trades grows, so
slow markets are markets in which each print carries less news.

Read naively, this licenses precisely the argument we are refusing: BYC has
not traded in 24 hours, therefore nothing has happened, therefore the last
~1.40 print still stands.

**The counterweight is structural and it applies to us specifically.** Both
models assume informed traders can act on either side. A Chia offer file
requires you to hold and lock the asset you are selling; the offer is fully
collateralised at creation. That is a hard short-sale constraint. Under a
short-sale constraint, an informed seller who does not already hold BYC
*cannot trade at all* -- their information is absorbed by silence rather
than expressed in a print. Long durations then read as **bad news**, not as
no news. The sign of the inference flips depending on which friction
dominates, and we cannot tell from the tape which one does.

Easley, Kiefer, O'Hara and Paperman (1996) is the honest place to point for
infrequently traded assets. Their contribution is that the information
content of infrequent trading is *estimable* (PIN) precisely because low
trade counts hide information -- not that silence is neutral. Their
estimation still wants a meaningful count of buy- and sell-initiated trades
per day across many days; we have neither the count nor a two-sided market
to count within.

So the duration literature does not license "no trades means the last print
holds". It makes the sign of that inference ambiguous. **Ambiguity is not
an input.**

### Lee-Ready (1991) and Odders-White (2000): our one genuine advantage

Lee and Ready wrote the quote rule and the tick rule because equity tapes
do not record who initiated a trade -- the direction has to be *inferred*.
Odders-White audited that inference against the NYSE TORQ data, where the
true initiator is known, and found roughly **85% correct classification**.
The more important half of her result is that the remaining 15% is not
noise: misclassification is **systematic**, concentrated in midpoint trades,
small trades, and large or frequently traded stocks. Systematic error
propagates as *bias* into every downstream estimate -- spread decompositions,
adverse-selection components, PIN -- rather than averaging out.

We do not have this problem at all. Every completed offer on dexie names
the asset offered and the asset requested, and the taker is by construction
the party who accepted the offer file. **Trade-signing error is zero, not
15%.** `taker_fills` (180 XCH/BYC rows, 115 BYC/wUSDC.b) records
counterparty-set prices directly.

Be precise about what that buys, though. The advantage is over *direction
inference*, and it is not an own-fill filter. `is_own_fill` on
`MarketDataFeed::ingest_trade` (`cpp/src/execution/market_data.cpp:2457`)
is an in-memory ingest parameter that is never written to disk, so
separating our own fills out of the venue tape is a separate job that
nobody has built (section 7).

This is a real structural advantage over the equity microstructure
literature and it is why the estimator in section 6 is worth running at all.
It rescues nothing in section 5: knowing exactly who hit whom on 632 trades
spread across months still does not produce a level, for the Merton reason.

## 5. What degenerates at this trade count -- stated as refusals

Each of these is a technique someone will propose. Each is refused, with
the reason, so the refusal can be checked rather than trusted.

| Technique | Refused because |
|---|---|
| Roll (1984) | Undefined on positive autocovariance, which a large fraction of small samples produce (Harris 1990) -- and estimates the spread, not the level |
| Hasbrouck (1991) VAR | Needs orders of magnitude more observations than 632 |
| Hasbrouck (1995) IS / Gonzalo-Granger (1995) CS | No VECM across an 8-day gap; and Putnins (2013) shows the metrics measure noise avoidance under unequal noise |
| Stoikov (2018) micro-price | Symmetrises the two sides -- the exact assumption this book violates |
| Lo-MacKinlay (1990) caveat | Nonsynchronous trading contaminates every cross-series statistic before economics enters |

### REFUSED: Roll (1984). Two independent reasons, either sufficient.

Roll's implicit spread estimator is `S = 2 * sqrt(-cov(dP_t, dP_{t-1}))`,
recovering the effective spread from bid-ask bounce in an efficient market.

1. **It estimates the SPREAD, not the LEVEL.** Even a perfect Roll number
   is silent on the question we are asking, which is whether 1.41 or 3.25
   is the right price. Nothing about a spread estimate adjudicates a level.
2. **It is UNDEFINED whenever the sample autocovariance is positive** --
   `sqrt` of a positive number has no real value in that formula -- and
   Harris (1990) characterised how badly the estimator behaves at small
   samples: the sampling distribution is wide and skewed, and a large
   fraction of draws land on the wrong side of zero and return nothing.

   Be careful which sample that applies to. Our 632 XCH/BYC `trade_log`
   rows are **our own fills** -- 100% of them, by construction -- so they
   are not a 632-observation sample of the market at all. The series Roll
   would actually have to run on is the dexie venue tape: **at least 1,486**
   settled August XCH/BYC offers, of which 270 are ours (section 7), spread
   unevenly across the month. That count is a measured LOWER BOUND, not an
   estimate: it is the sum of both offer directions from
   `GET /v1/offers?status=4&sort=date_completed`, paginated to 11 pages of
   100 per direction on 2026-09-01, and pagination was truncated there
   rather than exhausted. Reproduce it with
   `scripts/byc_price_diagnostic.py`; do not quote a precise venue count
   from this document. The companion BYC/wUSDC.b series is worse
   still, with no print since 2026-08-24 and a hard eight-day gap through
   today. Roughly a thousand unevenly spaced counterparty prints on one
   pair, and an eight-day hole in the other, is squarely the regime Harris
   describes.

We are not going to ship an estimator that returns no answer on a large
fraction of small samples, on exactly the illiquid days when an answer
would matter.

### REFUSED: Hasbrouck (1991) VAR

The structural VAR on signed trades and quote revisions decomposes a
trade's information content into permanent and transitory components.
It is estimated on intraday equity data with thousands of events per
stock-day. The venue tape gives us on the order of a thousand-odd
counterparty prints per pair across a whole month -- not per day -- and
zero in the last 24 hours; our own `trade_log` is smaller still and is
100% our own fills, so it is not a sample of the market at all (section
7). Either way there is nothing to fit. This is not a tuning problem.

### REFUSED: Hasbrouck (1995) information shares and Gonzalo-Granger (1995) component shares

Both require a cointegrated VECM across two price series for the same
asset, and both are about *attributing* discovery between venues rather
than producing a price. Our second series is BYC/wUSDC.b, whose newest
print is 2026-08-24. You cannot estimate an error-correction term across a
gap longer than the effective horizon of the correction; the "correction"
degenerates into a single jump at the reconnect.

And suppose we could. Putnins (2013) shows that when the competing series
carry **unequal noise**, IS and CS measure relative avoidance of
transitory noise rather than leadership in incorporating information -- the
less noisy series wins the metric regardless of which one actually leads.
Our two series carry wildly unequal noise: a one-sided Chia book against an
eight-day-stale cross through a bridge-compromised stablecoin. The metric
would confidently report a winner and the result would mean nothing. This
is a case where getting a number is worse than getting an error, because
a number invites action.

### REFUSED: Stoikov (2018) micro-price (and Avellaneda-Stoikov 2008 downstream)

The micro-price is the right idea for the right book: an imbalance-adjusted
mid that converges to the expected future mid, weighting toward the side
with more size. Its construction takes imbalance
`I = bid_size / (bid_size + ask_size)` and the **spread** as state
variables, which presumes both sides are quotes drawn from the same
generating process -- that is what makes the symmetric weighting
meaningful.

That is exactly the assumption this book violates. Our ask side has no
generating process; per Glosten-Milgrom it is an absence. Feed `I` into the
micro-price here and it returns a number pulled some distance toward
whichever fiction the ask ladder happens to be stating today, and it will
move when the fiction moves. Avellaneda and Stoikov (2008) inherits the
same prerequisite one level up: a reservation price and an inventory skew
are defined relative to a mid that means something.

### Standing caveat: Lo and MacKinlay (1990)

Not a technique to refuse, a contamination to remember. Nonsynchronous
trading induces spurious autocorrelation and cross-autocorrelation purely
from unequal trading frequencies -- an artefact of the observation schedule,
with no economics in it. Our two BYC series have trading frequencies
differing by more than an order of magnitude, and one of them has a hard
zero over the last 24 hours. Any cross-series statistic -- lead-lag,
correlation, cointegration residual -- carries this artefact before any
economics enters. It is also a second reason our ~19% share of the dexie
tape matters: our own quoting cadence partly *sets* the observation
schedule we would be measuring.

## 6. What IS usable: the high-low family, as an operator diagnostic

One family survives, and not as a price input.

Corwin and Schultz (2012) estimate the bid-ask spread from daily **high and
low** prices. The insight: a single day's high-low range contains one day
of volatility plus one spread; a two-day range contains two days of
volatility plus, still, one spread. Volatility scales with time and the
spread does not, so the two can be separated from the ratio. Abdi and
Ranaldo (2017) add the close, estimating the spread from the deviation of
the close from a mid-range proxy on adjacent days, and is documented as the
most accurate of the family for **less liquid** stocks, with estimates only
marginally sensitive to trades-per-day above roughly five. Ardia, Guidotti
and Kroencke (2024) supersede all three by combining the moment conditions
available from open, high, low and close efficiently.

**There is a free high/low input, and it is not enough on its own.**
`TickerData::price_high` / `price_low`
(`cpp/include/xop/rpc/dexie_client.hpp:168-169`) are parsed at
`cpp/src/rpc/dexie_client.cpp:647-650`, correctly orientation-swapped for
inverted pairs at 821-828 (high and low exchange roles under inversion, and
the code gets this right), and then **discarded** -- a repo-wide grep finds
no other reader. But it is a single 24h bar, and both estimators are
**two-day** estimators, so it can be a range check and never a series.

### What was actually built, and on what inputs

`scripts/highlow_spread_estimator.py` exists as of 2026-09-01. Because one
bar is not a series, its inputs are **not** the free high/low above. Read
them before reading its output.

* The multi-day bars come from the **sampled third-party BBO series** we
  already store: `offer_log.book_best_bid` / `book_best_ask`, or the denser
  `snapshots.mid_price_mojos` + `spread_bps`, which invert back to the same
  two sides exactly (mid 3.24975 at 10,768.52 bps returns 1.5000 / 4.9995).
* `--dexie` fetches the `price_high` / `price_low` bar above. It is reported
  as an independent range check and gate test. It cannot drive either
  estimator, because one bar is not two.

So every number the tool prints comes from **quote samples standing in for
trade prices**. That is outside the regime Corwin-Schultz and Abdi-Ranaldo
were derived for -- neither paper's input is a quote series -- and it is the
first caveat on the output, not a footnote to it. The script prints it on
every run and its module docstring forbids importing the file. A spread
number for the engine comes from the book or from realized fills; it does
not come from here.

Two bar constructions are offered and the choice changes the answer, so the
tool declares which one produced a figure. `quote-touch` (the default) takes
the daily high from the ask side and the low from the bid side; `mid` takes
both from the mid, which strips the bid-ask bounce the estimators exist to
extract. Neither one bounds the other. A bar construction is a
**sensitivity choice**: one set of inputs among several, and the estimate it
yields can come out higher *or* lower than the estimate from another choice.
Report which bars produced a figure and read two constructions as two
readings, never as brackets.

> **RETRACTED 2026-09-01.** Earlier versions of this section, of the README
> and of the changelog argued that `quote-touch` bars widen the sampled
> range and therefore yield an **upper bound** on the estimated spread, so
> the discrepancy below held "a fortiori". That is mathematically wrong and
> is withdrawn outright -- not narrowed, not hedged to "usually". In
> Corwin-Schultz the two-day range term
> `gamma = [ln(max(H1,H2)/min(L1,L2))]^2` enters `alpha` with a **minus**
> sign, so the estimate is not monotonic in the sampled range: widening a
> high or a low can *lower* the output, or drive it negative. Worked
> counterexample, baseline `H1=1.010 L1=0.990 H2=1.012 L2=0.988`, raising
> only day one's high and changing nothing else:
>
> | day-1 high | estimate  |
> |------------|-----------|
> | 1.010 (baseline) | 174.8 bps |
> | 1.020      | 155.2 bps |
> | 1.050      |  64.8 bps |
> | 1.100      |  16.3 bps |
> | 1.300      | -23.3 bps (floors to zero) |
>
> A strictly wider range, a monotonically *falling* estimate. Abdi-Ranaldo
> is likewise nonlinear in the shifted log mid-ranges and carries no
> monotonicity guarantee either. Every claim that rested on this -- "upper
> bound", "ceiling argument", "a fortiori", "the conservative direction" --
> goes with it. This is recorded rather than quietly edited because the job
> of this document is to stop settled questions being re-litigated, and a
> claim that silently changes teaches the opposite lesson.
>
> `scripts/highlow_spread_estimator.py` carries the same retraction in its
> module docstring and in the one-sided-book banner it prints on every run.
> If any output of that tool still reads as a ceiling or bound argument, the
> tool is wrong and this section is right.

### The measured discrepancy -- descriptive, and not proof of anything

These estimators recover a spread from the ratio of an observed high to an
observed low, and on a trade-price series both are prices that occurred
inside the bar. On a market whose prints all cluster within a few percent
of 1.40, the family cannot return 10,769 bps -- that one is a bound on the
*inputs*, not on the bar construction: bounded log ranges bound `beta`, and
`gamma` only ever subtracts. The tool we have runs on quote samples rather
than prints, so it does not even get that for free. Nor is the analytic
bound the argument: Corwin-Schultz saturates at 2.0, i.e. 20,000 bps, so a
large CS output means the estimator has saturated and is not confirmation of
a large spread.

**And the discrepancy does not survive a like-for-like comparison either.**
This is the second retraction in this section, and it is the more important
one. An earlier revision reported XCH/BYC on 2026-09-01 as "the widest
estimate 1,824.8 bps against 14,666.7 bps posted -- 8.0x", and read that gap
as the surviving evidence. It was an artifact of a WINDOW MISMATCH: the
numerator is one quote sample at one instant, while the denominator averaged
20 adjacent day pairs spanning a month. Dividing an instant by a month-long
average measures the window, not the book.

Compared like-for-like against the single most recent day pair
(2026-08-30..2026-08-31), the same posted 14,666.7 bps sits at **0.99x** the
comparator's 14,863.9 bps. No gap. The tool now prints "No discrepancy to
look into" for this pair, and carries the window mismatch and its size in
the output so the comparison cannot be made carelessly again.

Even the 0.99x is soft: that comparator is 74% of the 20,000 bps ceiling
Corwin-Schultz saturates toward as alpha grows, so it is the estimator
running out of range rather than a measured width. A saturating divisor
flatters any ratio built on it.

The window median of the daily medians, 341.7 bps, sits *below* the
estimate, so the two posted figures straddle it and the sign of any "gap"
depends on which is read. Every one of these numbers moves between runs.
Quote the run, not this paragraph.

That divergence is **descriptive**. The posted spread and the estimator
output are different quantities computed different ways, and a large gap
between them is a flag worth an operator chasing. It is not proof of an
absent side. These estimators do not identify sides at all -- they consume a
high and a low and return a scalar.

**The absent-side conclusion for XCH/BYC does not rest on this section and
never needed to.** It rests on the direct book reading in section 1: bids at
1.5000, within 6.4% of the independent 1.41022765 anchor, against asks at
4.9995 to 10.0000, which is 3.5x to 7.1x that anchor. One side prices the
pair and the other does not. That is an observation of the book, it invokes
no estimator, and nothing retracted above touches it. The estimators were
being cited in support of a conclusion that already stood without them.
Keep the two apart.

Ardia et al. (2024) states explicitly that Roll, Corwin-Schultz and
Abdi-Ranaldo are all **downward biased** when trading is infrequent. Record
that as a documented property of those estimators on the trade-price bars
their paper studies. It is not a safety margin here: our bars are quote
samples, outside that regime, and after the retraction above there is no
surviving argument that any bar choice errs in a knowable direction -- see
limit 2.

### Three honest limits

1. **Degenerate when high equals low.** On a zero-print day the venue
   reports the same value for both (or re-reports the previous print), the
   log ratio is zero, and the estimate is zero or negative. Corwin's own
   March 2014 note recommends setting negative estimates to zero for
   monthly cross-sectional work; that is a smoothing convention for
   research panels, **not** a licence to show an operator "0 bps". Report
   *undefined*, and report why.
2. **On a one-sided book the level is unusable, and no direction of error
   is knowable.** Fed a *trade*-price series the estimator sees only the
   surviving side: the honest bid ladder's range (1.3514 .. 1.5000, ~1,000
   bps), and never the junk asks. Fed the shipped tool's default
   `quote-touch` bars, a junk ask at 10.0000 enters the range instead.
   Those are two different inputs giving two different numbers, and neither
   brackets the other -- see the retraction above; do not read the wider
   input as the higher estimate. What holds for both is that **no
   counterparty ever crossed the width being measured**, so the output is
   not a transaction cost either way. Report the discrepancy; do not put
   the number in a fee model.
3. **The two-trades-per-bar rule fails on daily bars for BYC, and the
   shipped tool cannot enforce it.** Ardia et al. require a bar frequency
   giving at least two trades per bar; Abdi-Ranaldo document only marginal
   sensitivity above ~5 trades/day. BYC has **zero** trades in the last 24
   hours on every pair, so daily trade bars fail the rule outright. The
   quote-sample bars the tool actually builds do not fail it, because they
   are not the quantity the rule is about: `MIN_SAMPLES_PER_BAR` applies the
   two-per-bar threshold to BBO *samples*, which is a strictly **weaker**
   test than the paper's -- a bar that clears it has not been shown to clear
   theirs. That substitution is the price of having a series at all, and it
   is why the day-pair count travels beside every figure and why the whole
   tool is a descriptive diagnostic rather than a measurement. Aggregating
   to weekly bars is no escape: the answer is stale by construction and
   Merton (1980) says the level precision does not improve anyway.

### Plumbing notes

* `DexieClient::get_trades()` used to pass `sort="date_completed_desc"`,
  which the dexie API does not recognise and *silently ignores* -- so it
  returned an arbitrary page rather than recent trades, and did so without
  erroring. Fixed 2026-09-01 to `"date_completed"`, which was verified to
  work and to return newest-first. What was NOT verified is which other
  sort keys the API accepts, so treat no list of them as exhaustive; the
  call site carries the same caveat. Fixing it changed nothing operational,
  because `get_trades()` still has no callers anywhere in `cpp/` or `gui/`,
  which is deliberate -- see section 3. (Named by symbol rather than by
  line: line numbers in this file drift within a single session.)
* Cite the 2012 *Journal of Finance* paper for the estimator formulas only.
  The pooled correlations often quoted alongside it (0.912 and 0.803) come
  from Corwin and Schultz's February 2009 working note on intraday
  application, not from the JF paper. Attribute them correctly or omit
  them.

## 7. Two standing caveats

**Par is an assumption, not an observation.** Every sentence above of the
form "and the tape agrees with par" is one input agreeing with itself. If
BYC's peg breaks, the ~1.40 tape cluster and the 1.41 model output break
together, in the same direction, by the same amount, and neither warns us.
The only thing that would is a genuinely independent BYC/USD measurement,
and we do not have one -- the sole market cross runs through wUSDC.b, itself
depegged to ~0.80 USD and last crossed on 2026-08-29.

This is also why the peg-suspension observation route cannot be repaired by
fixing arithmetic alone. `Engine::step_observe_asset_pegs` (cpp/src/engine.cpp) did
`static_cast<double>(snap.mid_price)` with no division by `kMojosPerXch`,
yielding `usd_obs ~ 3.4e-13` and a false
`[PEGSUSPEND] ... observed 0.0000 ... 100.0% off`. The correct pattern is
`usd_per_base_from_mid`, used correctly by `Engine::usd_per_xch` in the same file. But scale is
only half the defect: the *correctly scaled* observation on this book is
1.43 / 3.25 = **0.44 USD**, still 56% off par and still past `bail_pct`
10.0. The observation **source** has to change as well, and the source
problem is the one-sided-mid problem from section 1. Fixing the cast alone
converts a nonsense alarm into a plausible-looking wrong alarm, which is
worse.

**~19% of the dexie tape is ours -- and we cannot currently subtract it.**
270 of at least 1,486 settled August XCH/BYC offers *on the venue* are our
own fills (253 `trade_log` + 17 `taker_fills`, verified against the
database; the venue count is the measured lower bound described in section
5).  So our share is at most ~18%, and falls as the true venue count
rises. Any statistic computed on the raw venue tape is partly a measurement
of our own quoting policy: if we quote 1.41 and get filled, the tape says
1.41 because we said so. That is circular in exactly the direction that
would make a history-fed price look self-consistent.

Note carefully which population that 19% describes. It is a property of
**dexie's tape**, not of our database. `trade_log` and `taker_fills` hold
our own fills *by construction* -- they are 100% ours, so an own-fill share
computed from them alone is not 19%, it is meaningless.

**The obvious handle does not exist.** `is_own_fill`
(`cpp/src/execution/market_data.cpp:2457`, and again on
`ingest_trade_for_vpin` at 2830) is an in-memory parameter that gates whale
detection and the VPIN filter at ingest time. It is **never persisted**.
`PRAGMA table_info` on `trade_log` and on `taker_fills` returns no own-fill
or self-fill column, so there is nothing in the database to filter on after
the fact and nothing to reconstruct one from.

So the exclusion cannot happen locally. It has to happen one level up, on
the dexie tape itself, and it has to be built:

1. Pull dexie's settled offers for the pair over the window.
2. Match each against **our own offer ids** -- `offer_log.offer_id` for
   what we posted, `trade_log.offer_hash` for what filled -- and drop the
   matches. That join *is* the own-fill exclusion; there is no shortcut.
3. Persist the verdict. A tape row classified once must carry a stored
   own-fill marker, because step 2 is only possible while our offer ids for
   that window are still on disk.

Step 3 is the real prerequisite: **until an own-fill marker is actually
written down, no tape statistic is reproducible.** Until then the honest
counterparty-priced samples we hold are the `taker_fills` subsets -- 180
(XCH/BYC) and 115 (BYC/wUSDC.b) rows, priced by the counterparty rather
than by us -- against the rest of the venue tape, which we currently have
no way to isolate. Note the join in step 2 must cover `taker_fills` as
well as `offer_log` and `trade_log`: a trade where WE crossed someone
else's offer is still our flow on the venue tape, and matching only
offers we posted would leave it counted as a counterparty print. Every refusal in section 5 gets
*more* emphatic at those counts, not less.

## 8. What we changed as a result

Nothing about price discovery. The tape stays out of the live pricing path.
The work went to **per-side book quality** (branch
`fix/per-side-book-quality`), because all three live defects are the same
defect wearing different clothes: **a threshold measured against a
disqualified side**. Two of them pool that side into a midpoint; the third
reads it directly, per-side, and is wrong anyway. Pooling is the more
visible symptom, not the root -- the root is trusting a side that has no
quote in it.

* **`bbo_sanity` Check 1, Step 8 of `cpp/src/engine.cpp`.**
  `mid_dev = |1.41141912 - 3.24975| / 3.24975 = 0.5657` against
  `bbo_sanity_max_mid_dev` 0.50, so `fee_filtered_tiers.clear()` and
  "suppressing ALL offers this block". Measured against `best_bid` alone
  the deviation is `|1.41141912 - 1.5000| / 1.5000 = 0.0591` -- passes with
  an order of magnitude to spare. A *correct* fair value was suppressing
  the entire pair because it disagreed with the mean of an honest quote and
  an absent one.
* **`cpp/src/engine.cpp:10134-10141`, Check 2.**
  `ref = (side == Bid) ? best_bid : best_ask`, so our correctly priced ask
  at 1.41 is measured against 4.9995 -- deviation 0.7180 against
  `max_aggressive_dev` 0.10, verdict `SuppressAggressive`. A bid at the
  same 1.41 passes at 0.0600 against `max_passive_dev` 0.30. The check is
  already per-side and is still wrong, because the per-side reference is
  trusted unconditionally.
* **`cpp/src/strategy/liquidity.cpp:1044-1056`.**
  `bbo_ref_f = (best_comp_bid + best_comp_ask) / 2 = 3.24975` and
  `bid_cap = bbo_ref`. The competitive anchor then parks a bid at ~1.5001
  against a 1.4102 fair value: a **live ~6.3% overpay on every XCH bought**.
  The correct cap is `min(bbo_ref, anchor)`.

Thresholds are library defaults -- `cpp/include/xop/config.hpp:283-285`,
aggressive 0.10, passive 0.30, mid 0.50 -- and `config.yaml` carries no
`bbo_sanity` key at all. **Do not tune them.** The defaults are not wrong;
the input they are fed is.

### Explicitly NOT changed

* **The per-offer absurdity filter.** `offer_absurdity_ratio(band 3.0,
  step 0.5) = max(10.0, 6.0, 4.0) = 10.0`, so the pass band around the
  1.41022765 anchor is [0.141, 14.10]; 4.9995 is 3.545x and 10.0000 is
  7.09x, both comfortably inside. It does not strip the junk asks and that
  is deliberate: `cpp/include/xop/execution/mid_gate.hpp:300-309` explains
  that filtering at the gate's own band would make the book-confirmation
  escape unreachable, so a genuine market-wide repricing could never
  publish. It is a fat-finger guard, not a dislocation detector. Narrowing
  it to catch these asks trades a real safety property for a symptom.
* **The last-trade freshness clock** (`cpp/src/execution/market_data.cpp:75-89`,
  stamped 567-569). Advances only on a price change, by design, for the
  polling reason in section 2. Documented limitation, not a bug,
  unfixable without a true per-trade timestamp.
* **The BYC pairs' `enabled:` flags -- not by this work, but the operator
  has since changed one.** Nothing in this note is an argument either way;
  the flags are an operator decision and stay one. On 2026-09-01 XCH/BYC was
  re-enabled at the operator's request and then BACKED OUT the same day,
  pending deployment; BYC/wUSDC.b was left disabled throughout. The decision
  stands, only its timing changed: an enable is inert until the engine
  restarts, but the GUI relaunches the engine whenever the GUI restarts, so
  the flag was a latent trigger any unrelated restart could arm. Recorded
  here because the deployment order is not obvious and getting it wrong is
  expensive:

  - An enable takes effect **only on an engine restart**. A live config
    reload disables a pair in place but refuses to enable one, logging
    "restart the engine to start quoting it".
  - **That restart must come after this branch is merged, built and
    deployed.** On the pre-branch binary a restart with the flag set
    reproduces the 2026-08-30 incident: the ladder self-crosses and drops
    every tier, the mojo-scale bug in section 7 latches a false depeg about
    ten minutes in and cancels every offer on every pair touching BYC, and
    that bogus valuation feeds the 10% drawdown breaker -- which pauses the
    **whole engine** and takes XCH/DBX, the only earning pair, down with it.
    Section 8 addresses every link in that chain.
  - The two sides are **not** symmetric, and there is no per-pair one-sided
    switch, so both go on together. In this pair's own orientation (price =
    BYC per XCH) a **bid** pays BYC to buy XCH, selling our 52.58 BYC at
    about 1.01 USD into the honest side of the book -- an exit at par, and
    the reason to be there. An **ask** accumulates more BYC, for which there
    is no exit: nothing bids for BYC above about 0.29 USD. Rising BYC
    inventory is the signal to turn the pair back off.
  - **BYC/wUSDC.b stays disabled for an unrelated reason.** The 2026-08-25
    warp.green bridge compromise depegged wUSDC.b (~0.80 USD) and the pair
    has had no print since 2026-08-24, so it would be quoting into a dead
    book through a broken denominator -- the section 2 problem, not a
    liquidity judgement about BYC.

If this proposal comes back, the short answer is **section 3**: the engine
already reaches 1.41 without the tape, the tape's agreement is downstream of
the same par assumption, and adding it would displace a governed
fallback anchor with a market edge carrying borrowed error bars.

## Appendix: reproducing the measurements

The live bot owns `data/xop_trader.db`. Any script written against it must
be **read-only**:

```python
conn = sqlite3.connect("file:" + db_path + "?mode=ro", uri=True)
```

Never write, never `VACUUM`, never migrate. Model new scripts on
`scripts/offer_sizing.py`, which already does the read-only URI connection,
a module-level `LOOKBACK_DAYS`, lexical ISO cutoff comparison, and a median
helper.

**Timestamp format trap**, documented at `scripts/offer_sizing.py:181-186`
and 207-212: `trade_log.timestamp` uses `"T"` as the date/time separator
while `snapshots.created_at` uses a **space**. A lexical cutoff comparison
that works on one silently fails on the other.

Relevant tables:

| Table | XCH/BYC | BYC/wUSDC.b | Notes |
|---|---:|---:|---|
| `trade_log` | 632 | 47 | **our own fills only** -- 100% ours, no own-fill column |
| `taker_fills` | 180 | 115 | ours too, but counterparty-set prices |
| `offer_log` (`book_best_bid` / `book_best_ask`) | 4,793 | 2,205 | third-party BBO samples |
| `snapshots` | 206,694 rows total | | from 2026-04-03 onward |

Note the shape of that table: on **XCH/BYC** we have roughly **seven times
more BBO samples than trades** (4,793 / 632), and on **BYC/wUSDC.b** the
ratio is far starker at roughly **forty-seven times** (2,205 / 47). The
thinner the pair trades, the wider the gap. That asymmetry is itself an
argument -- the book is the thing we observe densely, which is why fixing
how we read the book (section 8) beats mining the thing we observe
sparsely.

---

## Bibliography

Every entry below was independently verified. Where an attribution is
commonly got wrong, the correction is noted inline.

Abdi, Farshid, and Angelo Ranaldo (2017). "A Simple Estimation of Bid-Ask
Spreads from Daily Close, High, and Low Prices." *Review of Financial
Studies* 30(12), 4437-4480. doi:10.1093/rfs/hhx084.
*Documented as most accurate of the family for less liquid stocks;
estimates only marginally sensitive to trades-per-day above roughly five.*

Ardia, David, Emanuele Guidotti, and Tim A. Kroencke (2024). "Efficient
estimation of bid-ask spreads from open, high, low, and close prices."
*Journal of Financial Economics* 161, 103916.
*Supersedes Roll / Corwin-Schultz / Abdi-Ranaldo; states explicitly that
all three are downward biased when trading is infrequent, and requires a
bar frequency giving at least two trades per bar.*

Avellaneda, Marco, and Sasha Stoikov (2008). "High-frequency trading in a
limit order book." *Quantitative Finance* 8(3), 217-224.

Corwin, Shane A., and Paul H. Schultz (2012). "A Simple Way to Estimate
Bid-Ask Spreads from Daily High and Low Prices." *Journal of Finance*
67(2), 719-760. doi:10.1111/j.1540-6261.2012.01729.x.
*Cite this paper for the estimator formulas only.*

Corwin, Shane A., and Paul H. Schultz (2009). "An Application of the
High-Low Spread Estimator to Intraday Data." Working note, February 2009.
*The 0.912 / 0.803 pooled correlations belong here, NOT to the 2012 JF
paper.*

Corwin, Shane A. (2014). "Dealing with Negative Values in the High-Low
Spread Estimator." Note, March 2014.
*Source of the "set negative estimates to zero" recommendation for monthly
work. A research smoothing convention, not an operator display rule.*

Dufour, Alfonso, and Robert F. Engle (2000). "Time and the Price Impact of
a Trade." *Journal of Finance* 55(6), 2467-2498.

Easley, David, and Maureen O'Hara (1992). "Time and the Process of Security
Price Adjustment." *Journal of Finance* 47(2), 577-605.

Easley, David, Nicholas M. Kiefer, Maureen O'Hara, and Joseph B. Paperman
(1996). "Liquidity, Information, and Infrequently Traded Stocks." *Journal
of Finance* 51(4), 1405-1436. doi:10.1111/j.1540-6261.1996.tb04074.x.
*Use this four-author 1996 paper for the infrequent-trading point. There is
no "Easley, Kiefer and O'Hara (1997)" to cite for it.*

Glosten, Lawrence R., and Paul R. Milgrom (1985). "Bid, ask and transaction
prices in a specialist market with heterogeneously informed traders."
*Journal of Financial Economics* 14(1), 71-100.

Gonzalo, Jesus, and Clive W. J. Granger (1995). "Estimation of Common
Long-Memory Components in Cointegrated Systems." *Journal of Business and
Economic Statistics* 13(1), 27-35.

Harris, Lawrence (1990). "Statistical Properties of the Roll Serial
Covariance Bid/Ask Spread Estimator." *Journal of Finance* 45(2), 579-590.

Hasbrouck, Joel (1991). "Measuring the Information Content of Stock
Trades." *Journal of Finance* 46(1), 179-207.

Hasbrouck, Joel (1993). "Assessing the Quality of a Security Market: A New
Approach to Transaction-Cost Measurement." *Review of Financial Studies*
6(1), 191-212.

Hasbrouck, Joel (1995). "One Security, Many Markets: Determining the
Contributions to Price Discovery." *Journal of Finance* 50(4), 1175-1199.
doi:10.1111/j.1540-6261.1995.tb04054.x.

Kyle, Albert S. (1985). "Continuous Auctions and Insider Trading."
*Econometrica* 53(6), 1315-1335.

Lee, Charles M. C., and Mark J. Ready (1991). "Inferring Trade Direction
from Intraday Data." *Journal of Finance* 46(2), 733-746.

Lo, Andrew W., and A. Craig MacKinlay (1990). "An econometric analysis of
nonsynchronous trading." *Journal of Econometrics* 45(1-2), 181-211.
*Earlier version: NBER WP 2960, May 1989.*

Merton, Robert C. (1980). "On estimating the expected return on the market:
An exploratory investigation." *Journal of Financial Economics* 8(4),
323-361.
*Volatility precision improves with finer sampling of a fixed span; a
LOCATION parameter's precision depends on the LENGTH of the sample period,
not the observation count within it.*

Odders-White, Elizabeth R. (2000). "On the occurrence and consequences of
inaccurate trade classification." *Journal of Financial Markets* 3(3),
259-286.
*~85% correct classification on NYSE TORQ audit data, and the errors are
SYSTEMATIC -- concentrated in midpoint trades, small trades, and large or
frequently traded stocks -- so they bias downstream estimates rather than
averaging out.*

Putnins, Talis J. (2013). "What do price discovery metrics really measure?"
*Journal of Empirical Finance* 23, 68-83.
*The surname carries diacritics; render it as "Putnins" in ASCII contexts
and do not mangle it.*

Roll, Richard (1984). "A Simple Implicit Measure of the Effective Bid-Ask
Spread in an Efficient Market." *Journal of Finance* 39(4), 1127-1139.

Stoikov, Sasha (2018). "The micro-price: a high-frequency estimator of
future prices." *Quantitative Finance* 18(12), 1959-1966.
