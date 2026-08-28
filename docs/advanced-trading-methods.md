# Advanced Trading Methods

Three scholarly-grounded methods layered on top of the existing whale detection
system: **VPIN** (flow-toxicity estimation), **OFI** (order-flow imbalance), and
**asymmetric spread widening** (skewed quoting toward the informed side).

Section 4 is different in kind: **SVPerps** (statistical volatility
perpetuals) are a venue type we do not yet trade, recorded here because the
literature is thin and split across two fields, and because we have primary
measurements from a live venue that the literature does not contain.

---

## 1  VPIN — Volume-Synchronized Probability of Informed Trading

### Reference

Easley, D., López de Prado, M. & O'Hara, M. (2012). "Flow Toxicity and
Liquidity in a High-frequency World."  *The Review of Financial Studies*,
25(5), 1457–1493.

### Concept

VPIN estimates the probability that incoming order flow is *informed* (toxic).
Unlike the binary whale flag, VPIN produces a continuous signal in **[0, 1]**
that can smoothly modulate spread width, size, and quoting aggression.

The key insight is to partition trades into fixed-**volume** bars ("buckets")
rather than time bars.  This synchronises the estimator to market activity:
during periods of heavy trading the bars fill quickly, producing more frequent
updates exactly when they are most needed.

### Algorithm

1. Incoming trades are classified as buyer- or seller-initiated and
   accumulated into the current bucket.
2. When total volume in the bucket reaches `vpin_bucket_size` (default:
   10 XCH), the bucket is frozen and pushed to a rolling deque.
3. VPIN is the mean absolute imbalance across the last `vpin_window_buckets`
   (default: 50) completed buckets, normalised by the bucket size:

```
VPIN = (1/N) × Σ_i |buy_vol_i − sell_vol_i| / bucket_size
```

### Interpretation

| VPIN range | Flow quality | Recommended action |
|------------|-------------|-------------------|
| 0.0 – 0.2 | Balanced, uninformed | Normal quoting |
| 0.2 – 0.5 | Mildly imbalanced | Slight widening |
| 0.5 – 0.8 | Significantly toxic | Widen spreads, reduce size |
| 0.8 – 1.0 | Highly informed flow | Maximum widening or pause |

### Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `vpin_bucket_size` | double | 10.0 | Volume per bucket (base-asset units) |
| `vpin_window_buckets` | size_t | 50 | Completed buckets in rolling window |

### API

```cpp
// Ingest EVERY trade (not just whales) for VPIN.
feed.ingest_trade_for_vpin("XCH/wUSDC", Side::Bid, 3.5);

// Read toxicity signal.
double toxicity = feed.get_vpin("XCH/wUSDC");       // [0, 1]
auto   vm       = feed.get_vpin_metrics("XCH/wUSDC"); // full struct
```

---

## 2  OFI — Order Flow Imbalance

### Reference

Cont, R., Kukanov, A. & Stoikov, S. (2014). "The Price Impact of Order Book
Events."  *Journal of Financial Econometrics*, 12(1), 47–88.

### Concept

OFI aggregates signed volume changes at the **best bid and ask** into a single
predictor of short-term price moves.  By monitoring how the order book is
shifting *between* blocks, we can detect buying or selling pressure **before**
a large trade confirms — enabling preemptive spread widening.

### Algorithm

For each pair of consecutive order-book snapshots *(t−1, t)*:

**Bid-side delta** (`e^B`):

- If `bid_t > bid_{t−1}`: a new, higher bid arrived (bid improved) → `e^B = +bid_size_t`
- If `bid_t < bid_{t−1}`: the best bid was taken (bid weakened) → `e^B = −bid_size_{t−1}`
- If `bid_t == bid_{t−1}`: price unchanged → `e^B = bid_size_t − bid_size_{t−1}`

**Ask-side delta** (`e^A`):

- If `ask_t < ask_{t−1}`: a new, lower ask arrived (ask improved) → `e^A = +ask_size_t`
- If `ask_t > ask_{t−1}`: the best ask was taken (ask weakened) → `e^A = −ask_size_{t−1}`
- If `ask_t == ask_{t−1}`: price unchanged → `e^A = ask_size_t − ask_size_{t−1}`

**OFI** = `e^B − e^A`

Positive OFI → buy pressure (bids strengthening, asks retreating).
Negative OFI → sell pressure (asks strengthening, bids retreating).

The **normalised OFI** is clamped to [−1, 1] using the total volume across all
snapshots in the window as the normalisation factor.

### Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ofi_window_size` | size_t | 20 | Order-book snapshots retained |

### API

```cpp
// Ingest book state once per block (after ingest_dexie).
feed.ingest_book_snapshot_for_ofi("XCH/wUSDC", best_bid, bid_size, best_ask, ask_size);

// Read order-flow imbalance.
double ofi = feed.get_normalized_ofi("XCH/wUSDC");  // [-1, 1]
auto   om  = feed.get_ofi_metrics("XCH/wUSDC");     // full struct
```

---

## 3  Asymmetric Spread Widening

### Concept

The original whale-detection system applies a **symmetric** spread multiplier:
both bid and ask are widened by the same factor.  This wastes edge: the side
*opposite* the whale's direction has lower adverse-selection risk and can be
quoted tighter.

Asymmetric widening skews the multiplier toward the **informed** side:

- **Whale buys** (dominant_side = Bid) → widen ask, keep bid tight.
- **Whale sells** (dominant_side = Ask) → widen bid, keep ask tight.

### Formula

Given the symmetric multiplier `m` (from the whale detector) and an asymmetry
factor `α ∈ [0, 1]` (`asymmetric_skew_factor`):

```
excess = m − 1.0

high_mult = 1.0 + excess × (1 + α)   // applied to the informed side
low_mult  = 1.0 + excess × (1 − α)   // applied to the uninformed side

average(high_mult, low_mult) = m      // total widening is preserved
```

### Example

With defaults (`m` = 1.2 from a single whale event, `α` = 0.5):

```
excess    = 0.2
high_mult = 1.0 + 0.2 × 1.5 = 1.30   (informed side)
low_mult  = 1.0 + 0.2 × 0.5 = 1.10   (uninformed side)

Average: (1.30 + 1.10) / 2 = 1.20 = m  ✓
```

| Scenario | bid_multiplier | ask_multiplier |
|----------|---------------|---------------|
| No whale | 1.0 | 1.0 |
| Whale buying, α = 0.5 | 1.10 | 1.30 |
| Whale selling, α = 0.5 | 1.30 | 1.10 |
| Whale buying, α = 0.0 | 1.20 | 1.20 |
| Whale buying, α = 1.0 | 1.00 | 1.40 |

### Configuration

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `asymmetric_skew_factor` | double | 0.5 | 0 = symmetric, 1 = fully asymmetric |

### API

```cpp
auto am = feed.get_asymmetric_spread_multipliers("XCH/wUSDC");
double bid_spread = base_spread * am.bid_multiplier;
double ask_spread = base_spread * am.ask_multiplier;
```

---

## 4  SVPerps — Statistical Volatility Perpetuals

### Reference

**There is no peer-reviewed literature on "SVPerps" as a standardised
product.** The term is crypto-native, used in DeFi whitepapers and
quantitative trading circles rather than in journals. Anyone researching
these has to bridge two literatures that do not usually cite each other:
volatility derivatives from traditional quantitative finance, and perpetual
contract design from crypto.

Verified — found and confirmed to exist:

1. Shiller, R.J. — originator of the perpetual-futures concept; BitMEX
   introduced it to crypto in 2016.
2. "Designing funding rates for perpetual futures in cryptocurrency
   markets." arXiv:2506.08573.
3. "Perpetual future contracts in centralized and decentralized exchanges:
   Mechanism and traders' behavior." *Electronic Markets* 34(1), 2024.
   doi:10.1007/s12525-024-00715-1.
4. "Exploring the Impact: How Decentralized Exchange Designs Shape Traders'
   Behavior on Perpetual Future Contracts." arXiv:2402.03953.
5. Avellaneda, M. & Stoikov, S. (2008) — already in this document, and the
   quoting model this section ultimately feeds.

Cited to us but **NOT independently verified** — titles could not be
confirmed against a source, and the citations supplied were bare domains
rather than DOIs. Treat as leads, not authorities:

- Cox et al., "The Valuation of Options for Alternative Stochastic
  Processes" — foundations of modelling volatility as a process.
- "Pricing Variance Swaps under Hybrid CEV and Stochastic Volatility" —
  how a variance-swap leg computes and pays realized variance.
- "Analytic Approximations for Pricing Perpetual American Strangle Options
  under Stochastic Volatility", *J. Comput. Appl. Math.*
- Chitra et al. (2026), "Perpetual Futures in Decentralised Finance:
  Mechanics, Economic Risks, and Design", *DeFi Markets*. Searching found
  adjacent real work but not this title.
- "Probability-based portfolio in crypto-perpetual futures market"
  (ProSP), *Physica A*-style, Aug 2026.

For protocol blueprints rather than theory: Deri Protocol and Volatility
Protocol documentation.

### Concept

An SVPerp gives perpetual exposure to a *statistical volatility* series
rather than to a price. Two ingredients, from the two literatures:

- **The payoff leg** is the variance-swap idea: settle against realized
  (statistical) variance rather than against an asset price.
- **The tether** is the crypto perpetual mechanism: no expiry, and a
  continuous funding rate that pulls mark toward the oracle. This replaces
  the expiry-and-settle machinery of a variance swap.

The consequence that matters for market making: **there is no cash-and-carry
arbitrage to anchor the contract.** A conventional perp is disciplined by
arbitrageurs who can hold the underlying. Nobody can hold "QQQ 30-day
realized vol". The oracle and the funding rate are the *only* things holding
mark to fair value, so a maker on an SVPerp is quoting against a reference
with **no hedge on this venue** — and, under XOPTrader's current scope, no
hedge at all. (Not "no hedge exists in the underlying": these names have
deep listed options. See the hedging correction later in this document.)

### What we measured on a live SVPerp venue

Primary data, Permuto Capital (`perps.permuto.capital`), 2026-08-26. This
is the part not available in any of the literature above.

**The oracle is session-shaped.** Mean intrabar range by hour (UTC),
weekdays, from hourly candles:

| hours (UTC) | mean intrabar range |
| ----------- | ------------------- |
| 00:00–12:00 | 0.0%                |
| 13:00 (09:00 ET) | **488.8%**     |
| 14:00–18:00 | 277% → 160%         |
| 19:00       | 92%                 |
| 20:00       | 11%                 |
| 21:00–23:00 | ~0%                 |

Weekends: 0.2–0.4% against 57–88% on weekdays. So the series is frozen for
~13 hours a day and all weekend, then detonates at the equity open and
decays monotonically through the session. **A vol oracle is not a
round-the-clock process even when the perp trading on it is.**

**Vol-of-vol is enormous and market-specific.** 200 samples at 2s:

| oracle   | sd     | VR(2) | VR(5) | VR(10) |
| -------- | ------ | ----- | ----- | ------ |
| QQQ-VOL  | 12.5%  | 0.98  | 0.99  | 0.92   |
| NVDA-VOL | 20.6%  | 0.97  | 0.75  | **0.54** |
| TSLA-VOL | 25.8%  | 1.00  | 1.15  | **1.20** |

Variance ratio < 1 is mean-reverting, ~1 a random walk, > 1 trending. All
three are a random walk at 2s and **diverge with horizon**: NVDA reverts,
TSLA trends, QQQ is neither.

This is the practically important finding, and it cuts against the theory.
Stochastic-volatility models (Heston and descendants) assume variance is
mean-reverting — that is the defining property. **Only one of these three
oracles behaved that way IN THIS SAMPLE.**

⚠ **This is one 7.5-minute window, and it does not support a durable
classification of any market.** Two reasons to hold it loosely, both of
which cut against the reading above:

- **Overlapping windows confound the result.** The oracle is a 60-second
  trailing estimate resampled every 5s, so consecutive prints share ~55/60
  of their input and are autocorrelated *by construction*. The near-1
  short-horizon VRs are partly that artefact, not a statement about the
  underlying quantity.
- **One sample, one session segment.** These are volatility oracles; their
  character plausibly changes across the session, and the 13:00Z hour alone
  carries a 488% mean intrabar range.

So the honest statement is: **in this window NVDA reverted and TSLA
trended**. Whether that persists is exactly what the C-03 observer exists to
find out, and a maker should measure per market rather than inherit either
conclusion from here.

### Implications for quoting

1. **Reservation spread must cover vol-of-vol, not vol.** In
   Avellaneda–Stoikov the inventory penalty scales with σ of the *quoted
   instrument*. Here the quoted instrument is itself a volatility, so the
   relevant σ is vol-of-vol — 12–26% here, far above what a price-based
   calibration would produce.
2. **Reversion is an empirical question per market, not a property of the
   asset class — and not settled by the sample above.** Measure the variance
   ratio per oracle, per session segment, over a full day at minimum; do not
   assume it from the fact that the underlying is variance, and do not
   inherit it from a 7.5-minute window.
3. **Session awareness is mandatory.** Thirteen dead hours a day is not
   noise to be smoothed — it is a different regime, and any estimator
   calibrated on a blended day will misprice both halves.
4. **Funding is the only tether, so basis can persist.** With no
   cash-and-carry, a mark/oracle gap is not self-correcting on any
   particular timescale; it closes only as fast as funding makes it
   expensive. Sizing a mean-reversion trade on basis needs the funding
   cadence as an explicit input.
5. **No hedge exists ON THIS VENUE.** Inventory acquired on an SVPerp can
   only be flattened on Permuto under XOPTrader's current scope, so
   inventory risk behaves more like a single-venue CAT than a hedgeable
   perp. (Not "no hedge exists in the underlying" -- these names have deep
   listed options. See the hedging correction later in this document; the
   conclusion holds, the reason is narrower.)

### The closest live comparable: Bitfinex BVIV (and why it is not that close)

Bitfinex lists `BVIVF0:USTF0`, a perpetual on the **Bitcoin Implied Volatility
Index** published by Volmex — the nearest thing to a production svPerp on a
major venue, and worth reading precisely because the differences are larger
than the similarities.

| | **BVIV** (Bitfinex/Volmex) | **Permuto svPerps** |
| --- | --- | --- |
| underlying | 30-day **implied** vol | 60-second **realized** vol |
| source | actual option prices, two markets | range of a resampled price series |
| horizon | 30 days | 60 seconds |
| hedge | listed BTC options exist | none on-venue |
| mark | Index × USD/USDt | oracle + rate-limited book-mid basis |
| funding | on average spread vs mark; **may pause trading** | `clamp(premium/2, ±10%)`, 60 s settlement |
| leverage | 20× (IM 5.00%, MM 2.50%) | 10× (`/info/meta`) |

**The horizon differs by roughly 43,000×, and almost everything else follows
from that.** A 30-day implied-vol index is smooth, strongly mean-reverting, and
sits on top of a liquid instrument that can hedge it. A 60-second realized-vol
estimate resampled every 5 s is none of those things: it is noisy by
construction (consecutive prints share 55 seconds of input), it has no
tradeable underlying, and — as measured here — it moves 10–13% in seconds.

So BVIV validates the *product category* while quietly confirming the thing
that makes ours hard. It is the version of this instrument built where a hedge
exists. Quoting it is an options-desk problem; quoting Permuto's is an
inventory-limits problem, which is the conclusion §"No hedge exists ON THIS
VENUE" reaches from the other direction.

Two smaller transfers worth keeping:

- **Bitfinex pauses trading during funding**, "for a period of time lasting
  several seconds or longer". Permuto settles VOL funding every 60 s, so the
  obvious worry is a brief pause on every settlement. **Measured: no.** Across
  54 pause-state samples on 2026-08-28 `trading_paused` was false throughout,
  while `premium` moved every 5 s and `hourly_rate` stepped on 60-second
  boundaries — exactly the documented `sample_interval_secs: 5` /
  `settlement_interval_secs: 60`, with no trading interruption. A useful
  negative: C-11 pause handling has to cover operator pauses and the Sunday
  reset, not a funding hiccup every minute.
- **Dynamic tick size and minimum order size**, both derived from the
  contract's own price rather than fixed. Permuto publishes static
  `tick_size` 0.0001 and `lot_size` 1, which at a QQQ-VOL oracle near 0.07
  makes one tick ~0.14% of price — coarse relative to a ±2% ring, and worth
  remembering when placing ladder levels that must stay strictly inside it.

### Market making an SVPerp — the two-literature blueprint

No paper is titled "Market Making for SVPerps". The mathematical blueprint
has to be assembled from two fields that do not cite each other: **optimal
perpetual control** (how to quote given a funding cash flow) and **variance
swap replication** (how to hedge the volatility leg).

#### The perp leg — funding-aware quoting

**Le, N.A. (2026). "Funding-Aware Optimal Market Making for Perpetual
DEXs." arXiv:2605.06405, 7 May 2026.** *Verified.*

Extends Avellaneda–Stoikov by making the **funding rate a state variable**.
The insight is that inventory generates two distinct exposures, not one:
mark-to-market, and a *state-dependent funding cash flow*. Classical A–S
models only the first, so its optimal spread is wrong whenever funding is
material. Formulated as a reduced inventory-funding control problem and
solved with a monotone finite-difference HJB scheme; optimal spreads come
out of discrete inventory value differences.

Two details from the paper that matter to us more than the headline result:

- Calibrated on **Hyperliquid** data, and the author reports that real
  funding innovations have **heavy tails** beyond the Gaussian
  Ornstein–Uhlenbeck baseline the model uses. Our oracles are heavier-tailed
  still (20.8% two-second moves), so the Gaussian-OU calibration is a floor
  on the risk, not an estimate of it.
- Results were **mixed on SOL** — gains against unscaled baselines, no clear
  advantage once risk-scaled. Funding-awareness is not a free improvement;
  it pays where funding is large relative to spread.

Practical takeaway: skew quotes by *which side of the funding you are on*,
not only by inventory sign. Being long inventory while receiving funding is
a materially different state from being long while paying it, and A–S
cannot express the difference.

**Reinforcement learning for automated market making in perpetual futures**
(ScienceDirect, S240591882600022X). *Unverified — 403 on fetch.* Cited for
adaptive quoting under volatility regimes: static rules do not survive, and
quotes must widen on regime shifts to avoid being adversely filled during
spikes. Consistent with our own measurements, but we could not read it.

#### The volatility leg — replication, and why it does not apply here

**Martin, I. (2011). "Simple Variance Swaps." NBER Working Paper 16884,
March 2011. doi:10.3386/w16884.** *Verified.*

⚠ **A correction to how this is usually cited.** The familiar results —
the log-contract price and the **1/K² strike weighting** — belong to the
*standard* variance swap (Demeterfi, Derman, Kamal & Zou, 1999). Martin's
paper exists precisely because that replication **breaks when prices jump**;
it was motivated by the 2008–09 jumps that dried up the single-name variance
swap market entirely. The *simple* variance swap is a different contract
chosen to stay robust and hedgeable under jumps.

That distinction matters to us, but the argument below was originally made
too strongly and is corrected here.

What we observed is violent movement in the **oracle** — 488% mean intrabar
range at the open, 20.8% moves in two seconds. What the replication
continuity assumption concerns is the path of the **underlying equity**.
Those are not the same series, and one does not demonstrate the other: a
60-second trailing realized-vol estimator resampled every 5s will step
discontinuously whenever a single large return enters or leaves its window,
which is a property of the estimator's construction rather than evidence of
a jump in QQQ, NVDA or TSLA.

So the honest statement is: **if** the underlying paths jump, the standard
1/K² replication is the wrong tool and the jump-robust formulation is
required — and the oracle's behaviour makes that worth testing rather than
assuming. Settling it needs a jump test on the underlying source returns,
not on the oracle. Until that runs, treat jump-robustness as the prudent
default for any fair-value reasoning, not as an established fact about
these names.

**"Variance Swap Replication: Discrete or Continuous?"** (MDPI,
*JRFM* 11(1), 11). *Unverified — 403 on fetch.* Cited for discretisation
error: real option chains have finitely many strikes, so replication carries
tracking error that must be managed by surface interpolation.

#### The blueprint's third step is unavailable to us

The standard three-step cycle — (1) quote via funding-aware HJB, (2) filter
by volatility regime, (3) hedge residual variance exposure with an options
strip — **cannot be completed on Permuto.**

Step 3 requires an options chain on the underlying to build the replicating
portfolio. Permuto lists three vol perps and nothing else: no options, no
spot vol instrument, and no venue on which the same exposure trades. There
is also no cash-and-carry anchor, since nobody can hold realized variance.

So **Permuto offers no on-venue hedge**, and under XOPTrader's current scope
the position is unhedged: inventory can only be flattened by trading back on
the same book, against the same flow that created it. Steps 1 and 2 are all
that is available, and they control the *rate* of inventory accumulation
without providing any way to offload it.

⚠ **Not "unhedged by construction" — that was too strong.** QQQ, NVDA and
TSLA have deep listed options markets, so a variance exposure in these names
is hedgeable in principle by an external strip. What is true is narrower and
still decisive for us: there is no hedge *on this venue*, we have no external
options adapter and no plan to build one, and such a hedge would carry tenor
mismatch, oracle-vs-market basis, separate collateral, latency across venues,
and account access we do not have. The conclusion for our scope is unchanged;
the reason is "out of scope and expensive", not "impossible".

The live evidence is consistent with this: on the venue leaderboard, every
market maker that accumulated meaningful two-sided depth is deeply negative,
three are at exactly zero equity, and the only non-negative MM has
essentially no depth.

⚠ **That correlation does not identify a cause.** It is consistent with
unhedgeable inventory, but it cannot separate inventory risk from leverage,
quote placement, adverse selection or execution quality — the leaderboard
exposes none of those. The earlier claim that this is "the signature of
unhedgeable inventory, not of bad execution" asserted more than the data can
carry. What survives is that depth and PnL move against each other on this
venue, which is reason enough to treat inventory as the thing to control.

Implication for any SVPerp strategy we build: **inventory limits are the
primary risk control**, not spread. Spread governs how fast inventory
arrives; with no hedge, only a hard cap governs how much of it we can be
holding when the 13:00Z hour arrives.

### Four advanced MM methods, and which can actually run on our venue

A survey of methods proposed for SVPerp market making. **Citations for this
set were supplied as bare domains (arxiv.org, medium.com, ssrn.com) with no
paths or DOIs, so none could be verified.** Recorded as *ideas to evaluate*,
not as literature. The value added here is the applicability check against
Permuto, which is verified.

#### 1. Jump-aware volatility filters — APPLICABLE, and the most relevant

Classical A–S assumes continuous paths. Statistical volatility indices gap
instead: they spike when liquidations cascade. The proposal is a jump-aware
Realized GARCH with autoregressive jump intensity, widening the ask and
leaning short-vol ahead of a predicted upward jump.

Directly relevant — with the same caveat the Martin note above carries, and
for the same reason. Our measurements show a 488% mean intrabar range at the
equity open and 20.8% two-second moves **in the ORACLE**, and a 60-second
trailing estimator resampled every 5s steps discontinuously by construction
whenever one large return enters or leaves its window. That is not evidence
of jumps in QQQ, NVDA or TSLA. Treat jump-awareness as the prudent default
for quoting this series, not as an established property of the underlying;
settling it needs a jump test on the underlying returns.

One caveat on the proposed *signal*: it assumes on-chain flow and liquidation
observability. Permuto runs an off-chain sequencer with a CLOB, so there is
no mempool to read. The usable inputs are `/info/l2` book deltas,
`/info/trades`, and `/info/funding/predicted` — not chain flow.

#### 2. RL for funding-rate farming — APPLICABLE BUT DANGEROUS HERE

The proposal: a PPO/DDPG agent on
`ΔPnL − λ₁·InventoryRisk + λ₂·FundingEarned`, learning to accumulate large
directional inventory purely to farm funding paid by leveraged speculators.

⚠ **On Permuto this is the strategy that has already bankrupted people.**
Farming funding means holding large unhedged inventory, and per the section
above there is no hedge available on this venue — no options, no second
market, no cash-and-carry. Three MMs currently sit at exactly zero equity.
The reward function above prices inventory risk with a tunable λ₁; the
venue prices it with a liquidation. Any funding-farming term must sit behind
a hard inventory cap, not a soft penalty.

Note also the funding cadence: `markets[].funding_timing` is authoritative
and VOL markets settle every **60s**, not the 3600s in the top-level
fallback. Funding accrues 60× more often than the platform default implies.

#### 3. Cross-market synthetic tenor surfaces — NOT APPLICABLE

The method maps options IV, ATM straddles, and perp funding into a unified
surface over synthetic tenors, then arbitrages the SVPerp against a
replicating options strip.

**Verified unavailable on-venue.** `/info/meta` lists exactly three markets
— symbols `QQQ-VOL-PERP`, `TSLA-VOL-PERP`, `NVDA-VOL-PERP`, on oracle
tickers `QQQ-VOL`, `TSLA-VOL`, `NVDA-VOL` — with no option-like instrument
and nothing else on this venue trading the same exposure. (External listed
options on the same names do exist; see the hedging correction above.) The method presumes a
Deribit-plus-Hyperliquid world. This is the same wall as the replication
leg: the arbitrage that would make the operation statistical rather than
directional does not exist here.

#### 4. vAMM pool provision — NOT APPLICABLE

**Verified: Permuto is a CLOB, not a vAMM.** Neither published skill
contains any pool, vault, LP-token or deposit-share concept (zero matches),
while the API is unambiguously order-driven: `/info/l2` snapshots,
`POST /exchange/order` with GTC/ALO/IOC (see the note on FOK in
`docs/permuto-api-reference.md` §2), `batch_upsert` to modify resting
orders. There is no "deposit and act as the house" path — capital
is committed as limit orders and liquidity is provided actively.

### Venue mechanics we had missed

Found while checking the above, and material to any strategy:

- **"Carried" is the venue's own term for the frozen oracle.** While the
  equity cash session is closed the vol oracle is *carried*, confirming the
  13 dead hours measured from candles.
- **Overnight quoting costs 8× margin.** Risk-increasing places during a
  carried session require stressed initial margin
  (`VOL_CARRIED_STRESS_MULTIPLE`, default **8**). Reduce-only is exempt.
  This substantially weakens the "bank depth cheaply overnight" hypothesis —
  the depth may be low-risk but it is not low-capital.
- **Every resting order is cancelled at the open.** On carried→live, *all*
  resting orders and pending triggers on that vol market are cancelled and
  must be re-quoted. This answers an open question: we cannot carry quotes
  into the 488% hour even if we wanted to, and the venue is protecting
  makers from exactly that.
- **Two bands, not one.** `vol_oracle_band_pct` (default **5**) is the legal
  placement band; `vol_aggressive_ring_pct` (default **2**) is the inner
  ring. Outside the inner ring only *passive* rests are allowed (bids ≤
  oracle, asks ≥ oracle). The ±2% figure that governs depth credit is the
  same inner ring.
- **Prices are decimal annualized IV** (`0.176` = 17.6%), must be within the
  legal band of a *fresh* oracle, and a stale oracle returns HTTP **503**.

### Ideas taken from two external repositories

Read for ideas, not copied. Recorded here with what each one is actually
worth to us, because the useful parts were not the parts either repository
advertises.

#### `anthonymakarewicz/volatility-trading` (MIT, Python)

Its headline strategies — VRP harvesting and skew mispricing — are
delta-hedged **options** strategies and cannot run on a venue with no options
chain. Same wall as the replication leg above. But two of its components are
directly on our critical path.

**1. Range-based realized-volatility estimators — the best idea in either
repo.** `rv_forecasting/vol_estimators.py` implements a family beyond
close-to-close: Parkinson, Garman–Klass, Rogers–Satchell and Yang–Zhang.
These use the full OHLC bar rather than only the close, and the standard
result is that they extract substantially more information from the same
sample — Parkinson and Garman–Klass are several times more statistically
efficient than close-to-close, and Yang–Zhang is built to survive
overnight gaps.

That matters here for a specific reason. **Permuto serves OHLC**
(`/info/candles` returns `open`/`high`/`low`/`close`), and the venue's own
oracle is a 60-second trailing estimate whose short-window noise is the
central open question in `TODO-COMPETITION.md`. A range-based estimator
computed from those bars is *independent and better-conditioned*, and it is
the best instrument this venue actually serves.

⚠ **It is not the same quantity, so the difference is a bound, not an
identification.** Three mismatches, none of which the difference can
separate from the noise it is supposed to be measuring:

- **Horizon.** `tf` is accepted and ignored; `/info/candles` returns 3600s
  bars whatever you ask for (`docs/permuto-api-reference.md` §1). The oracle
  is a 60s trailing estimate. An hourly range and a 60-second RV are not
  comparable magnitudes.
- **Input series.** `base_asset`/`oracle_ticker` is `QQQ-VOL`, so the bars
  are bars *of the volatility series*. A range estimator over them measures
  variation of the vol series — vol-of-vol — not the underlying equity's
  realized volatility.
- **Model.** Parkinson and Garman–Klass assume driftless GBM over the bar,
  which the oracle's construction does not satisfy.

So the difference between the two series **bounds** the oracle's estimator
noise and confounds it with horizon, input and model mismatch; it does not
measure it. Identifying the estimator noise needs matching-frequency returns
of QQQ/NVDA/TSLA themselves, which this venue does not serve. C-03 must
therefore either source those externally, or record the range estimator for
what it is — an hourly vol-of-vol proxy, useful as a regime signal and as an
upper bound, not as a noise measurement.

It is still better conditioned than the variance-ratio work above, which has
already had to be walked back once as confounded by overlapping windows. It
should go into the C-03 observer — as a proxy carrying the three mismatches
above, not as the answer to the noise question.

**2. Explicit regular-trading-hours handling.** `rv_intraday(close,
rth_start="09:30", rth_end="16:00")` and `overnight_return(...)` treat the
session and the overnight gap as *separate objects* rather than smoothing
across them. That is exactly the problem the measured session shape poses —
thirteen dead hours a day, a 488% open, and a "carried" oracle state the
venue names itself. Any estimator we calibrate on a blended day will
misprice both halves, and this is the established way to avoid it.

**3. HAR lag structure** (`RV_D`, `RV_W`, `RV_M` in
`rv_forecasting/features.py`) is the standard decomposition for realized
variance, reportedly ~30% out-of-sample R² in their write-up. Worth knowing,
but **transfer is doubtful**: HAR is built on *daily* realized vol from
intraday returns, whereas our oracle is a 60-second estimate refreshed every
5s. The multi-timescale idea is right; the specific daily/weekly/monthly
components are not obviously meaningful at our sampling rate. Treat as a
direction, not a recipe.

Its `options/risk/{sizing,margin,scenarios}.py` may be worth a look when
C-04 (the perps position model) starts, since sizing under volatility
uncertainty is the same problem in a different wrapper.

#### `SidShah2953/Perpetuals-Research` (no licence declared, Python)

**No licence means no grant of rights** — readable, not copyable. Nothing
here is taken.

Its analysis does not cover market making or basis/arbitrage, so the
research content is of limited use to us. The useful idea is
**architectural**: `dataCollection/` separates a per-venue client
(`hyperliquid/`, `edgex/`, `dydx/`) from a shared `common/{http, types,
classification}` layer, with each venue exposing the same
`perpetuals/{markets, candles, funding}` surface.

That is precisely the adapter boundary `TODO-COMPETITION.md` argues for —
strategy and observation shared, venue mechanics isolated — and it is
useful confirmation that someone else building against several perp venues
converged on the same split. It also treats **funding as a first-class
collected stream** rather than a derived quantity, which matches VOL markets
settling every 60s.

### Open questions

- Is the 13:00Z spike a genuine open-effect or an artefact of the oracle
  restarting after 13 idle hours? Not resolvable from venue history —
  `/info/candles` accepts `tf` but **ignores it**, always returning hourly
  bars. (Partly moot for quoting: all resting orders are cancelled at
  carried→live anyway, so we cannot be holding stale quotes into it.)
- **Does a carried (frozen) oracle count as "fresh" for depth credit?
  STRONG EVIDENCE that it does, 2026-08-27 — not confirmation.** Stated in
  the sponsor's channel and then measured: two accounts gained 10.27M and
  8.37M depth-seconds across 122 minutes with the oracle frozen at a single
  value, which roll-off cannot produce.

  The measurement excludes a ONE-TIME delayed credit, which is what the flat
  rate profile rules out. It does not exclude incremental backfill — a
  pre-close backlog draining a little into each bucket rises in every
  sub-window too and is indistinguishable at this sampling rate. Separating
  them needs a control account known to be flat through the close, or a
  documented lag bound; neither exists. See `TODO-COMPETITION.md` C-0S3.
  What constrains the overnight window either way is the 8× carried margin
  and the entrants hunting resting size there.
- How is the oracle actually computed? Permuto documents a BLS-signed
  price certificate, but `/info/price_certificate` returns
  `No price certificate available yet`, so no provenance is exposed.

---

## Integration Guide

### Combining All Signals

The three signals can be composed multiplicatively with the existing spread
computation pipeline:

```
base_spread_bps = SpreadOptimizer::compute_spread(...)

// Layer 1: Whale detection (existing).
whale_mult = feed.get_whale_spread_multiplier(pair);

// Layer 2: VPIN toxicity (new — replaces or augments whale mult).
vpin = feed.get_vpin(pair);
vpin_mult = 1.0 + vpin * (max_vpin_widening - 1.0);

// Layer 3: OFI preemptive widening (new — optional, experimental).
ofi = feed.get_normalized_ofi(pair);
ofi_mult = 1.0 + abs(ofi) * ofi_sensitivity;

// Layer 4: Asymmetric skewing (new — applied per-side).
// NOTE: asym already incorporates the whale symmetric multiplier, so do NOT
// multiply by whale_mult separately — that would double-count whale widening.
asym = feed.get_asymmetric_spread_multipliers(pair);

final_bid_spread = base_spread_bps * vpin_mult * ofi_mult * asym.bid_multiplier;
final_ask_spread = base_spread_bps * vpin_mult * ofi_mult * asym.ask_multiplier;
```

### Suggested Defaults for Chia

| Parameter | Conservative | Moderate | Aggressive |
|-----------|-------------|----------|-----------|
| `vpin_bucket_size` | 20.0 XCH | 10.0 XCH | 5.0 XCH |
| `vpin_window_buckets` | 100 | 50 | 25 |
| `ofi_window_size` | 30 | 20 | 10 |
| `asymmetric_skew_factor` | 0.3 | 0.5 | 0.8 |

---

## Architecture

### New Types (in `cpp/include/xop/types.hpp`)

| Type | Purpose |
|------|---------|
| `VpinBucket` | Single volume bar with buy/sell accumulation |
| `VpinMetrics` | Aggregated VPIN statistics |
| `OfiMetrics` | Order flow imbalance statistics |
| `AsymmetricMultipliers` | Per-side spread widening factors |

### New Methods (on `MarketDataFeed`)

| Method | Description |
|--------|-------------|
| `ingest_trade_for_vpin(pair, side, volume)` | Feed every trade into VPIN pipeline |
| `get_vpin_metrics(pair)` | Full VPIN statistics |
| `get_vpin(pair)` | Scalar toxicity in [0, 1] |
| `ingest_book_snapshot_for_ofi(pair, bid, bid_sz, ask, ask_sz)` | Feed book state for OFI |
| `get_ofi_metrics(pair)` | Full OFI statistics |
| `get_normalized_ofi(pair)` | Scalar imbalance in [−1, 1] |
| `get_asymmetric_spread_multipliers(pair)` | Per-side multipliers |

### Thread Safety

Four new `std::shared_mutex` guards are added, following the existing lock
ordering convention:

```
mtx_pairs_ → ... → mtx_whale_metrics_ → mtx_vpin_ → mtx_vpin_metrics_ → mtx_ofi_ → mtx_ofi_metrics_
```

Public methods (and their helpers) may acquire multiple mutexes, but they always
do so in this global order; no method acquires locks in a conflicting order.

### Unit Tests

18 tests in `cpp/tests/test_advanced_trading.cpp` covering:

- VPIN: no data, balanced/one-sided/mixed flow, bucket counts, window trimming,
  volume percentages.
- OFI: single snapshot, bid/ask strengthening, stable book, window trimming,
  normalisation bounds.
- Asymmetric: no whale, whale buying/selling, skew factor 0, average
  preservation.

---

## Further Reading

1. Easley, D., López de Prado, M. & O'Hara, M. (2012). "Flow Toxicity and
   Liquidity in a High-frequency World."  *The Review of Financial Studies*.
2. Cont, R., Kukanov, A. & Stoikov, S. (2014). "The Price Impact of Order Book
   Events."  *Journal of Financial Econometrics*.
3. Avellaneda, M. & Stoikov, S. (2008). "High-frequency trading in a limit
   order book."  *Quantitative Finance*.
4. Cartea, Á., Jaimungal, S. & Penalva, J. (2015). *Algorithmic and
   High-Frequency Trading*.  Cambridge University Press.
5. Brunnermeier, M.K. & Pedersen, L.H. (2005). "Predatory Trading."  *The
   Journal of Finance*.
6. Easley, D., Kiefer, N.M., O'Hara, M. & Paperman, J.B. (1996). "Liquidity,
   Information, and Infrequently Traded Stocks."  *The Journal of Finance*.
7. "Designing funding rates for perpetual futures in cryptocurrency
   markets."  arXiv:2506.08573.
8. "Perpetual future contracts in centralized and decentralized exchanges:
   Mechanism and traders' behavior."  *Electronic Markets* 34(1), 2024.
9. "Exploring the Impact: How Decentralized Exchange Designs Shape Traders'
   Behavior on Perpetual Future Contracts."  arXiv:2402.03953.
10. Le, N.A. (2026). "Funding-Aware Optimal Market Making for Perpetual
    DEXs."  arXiv:2605.06405.
11. Martin, I. (2011). "Simple Variance Swaps."  NBER Working Paper 16884.
    doi:10.3386/w16884.
12. Demeterfi, K., Derman, E., Kamal, M. & Zou, J. (1999). "More Than You
    Ever Wanted To Know About Volatility Swaps."  Goldman Sachs
    Quantitative Strategies Research Notes.  (Source of the 1/K^2
    replication weighting often misattributed to Martin.)
