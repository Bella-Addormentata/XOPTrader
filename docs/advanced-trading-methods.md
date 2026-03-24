# Advanced Trading Methods

Three scholarly-grounded methods layered on top of the existing whale detection
system: **VPIN** (flow-toxicity estimation), **OFI** (order-flow imbalance), and
**asymmetric spread widening** (skewed quoting toward the informed side).

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

**Bid-side delta** (`δ_bid`):

- If `bid_t > bid_{t−1}`: a new, higher bid arrived → `δ_bid = +bid_size_t`
- If `bid_t < bid_{t−1}`: the best bid was taken → `δ_bid = −bid_size_{t−1}`
- If `bid_t == bid_{t−1}`: price unchanged → `δ_bid = bid_size_t − bid_size_{t−1}`

**Ask-side delta** (`δ_ask`):

- If `ask_t < ask_{t−1}`: a new, lower ask arrived → `δ_ask = −ask_size_t`
- If `ask_t > ask_{t−1}`: the best ask was taken → `δ_ask = +ask_size_{t−1}`
- If `ask_t == ask_{t−1}`: price unchanged → `δ_ask = ask_size_{t−1} − ask_size_t`

**OFI** = `δ_bid − δ_ask`

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
asym = feed.get_asymmetric_spread_multipliers(pair);

final_bid_spread = base_spread_bps * whale_mult * vpin_mult * ofi_mult * asym.bid_multiplier;
final_ask_spread = base_spread_bps * whale_mult * vpin_mult * ofi_mult * asym.ask_multiplier;
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

Each public method acquires at most one mutex.

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
