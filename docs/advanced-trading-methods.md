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
they cannot hedge in the underlying.

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
oracles behaves that way.** A maker who assumes reversion because the
underlying is "volatility" will be right on NVDA and systematically run over
on TSLA.

### Implications for quoting

1. **Reservation spread must cover vol-of-vol, not vol.** In
   Avellaneda–Stoikov the inventory penalty scales with σ of the *quoted
   instrument*. Here the quoted instrument is itself a volatility, so the
   relevant σ is vol-of-vol — 12–26% here, far above what a price-based
   calibration would produce.
2. **Reversion is an empirical question per market, not a property of the
   asset class.** Measure the variance ratio per oracle, per session
   segment; do not assume it from the fact that the underlying is variance.
3. **Session awareness is mandatory.** Thirteen dead hours a day is not
   noise to be smoothed — it is a different regime, and any estimator
   calibrated on a blended day will misprice both halves.
4. **Funding is the only tether, so basis can persist.** With no
   cash-and-carry, a mark/oracle gap is not self-correcting on any
   particular timescale; it closes only as fast as funding makes it
   expensive. Sizing a mean-reversion trade on basis needs the funding
   cadence as an explicit input.
5. **No hedge exists in the underlying.** Inventory acquired on an SVPerp
   can only be flattened on the same venue, so inventory risk is closer to
   a single-venue CAT than to a hedgeable perp.

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

That distinction is not pedantry for us. These oracles jump violently — a
488% mean intrabar range at the open, 20.8% moves in two seconds. **Any
replication argument that assumes a continuous price path is invalid in
exactly our regime.** If we ever reason about fair value from replication,
it must be the jump-robust formulation.

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

So a market maker there is **unhedged by construction**. Inventory can only
be flattened by trading back on the same book, against the same flow that
created it. Steps 1 and 2 are all that is available, and they control the
*rate* of inventory accumulation without providing any way to offload it.

This is the most important practical conclusion in this section, and it is
consistent with the live evidence: on the venue leaderboard, every market
maker that accumulated meaningful two-sided depth is deeply negative, three
are at exactly zero equity, and the only non-negative MM has essentially no
depth. That is the signature of unhedgeable inventory in a jumping series,
not of bad execution.

Implication for any SVPerp strategy we build: **inventory limits are the
primary risk control**, not spread. Spread governs how fast inventory
arrives; with no hedge, only a hard cap governs how much of it we can be
holding when the 13:00Z hour arrives.

### Open questions

- Is the 13:00Z spike a genuine open-effect or an artefact of the oracle
  restarting after 13 idle hours? Not resolvable from venue history —
  `/info/candles` accepts `tf` but **ignores it**, always returning hourly
  bars.
- Does a frozen oracle still count as "fresh" for liquidity-incentive
  purposes? Decides whether overnight quoting is cheap credit or no credit.
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
