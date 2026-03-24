# Whale Trader Response

## Overview

A "whale trader" is a market participant whose individual trades are large enough
to materially shift the price or consume significant liquidity.  On thin DEX
markets such as Chia's Dexie, a single 50 XCH trade (~$135 K at $2.70/XCH) can
move the mid-price by tens of basis points, instantly making all resting quotes
stale.

This document covers:

1. [Detection criteria](#detection-criteria) — when a trade is classified as a whale trade.
2. [Risks and pitfalls](#risks-and-pitfalls) — what can go wrong.
3. [Response strategy](#response-strategy) — how XOPTrader adapts automatically.
4. [Configuration reference](#configuration-reference) — tuning the parameters.
5. [Architecture](#architecture) — C++ implementation details.

---

## Detection Criteria

XOPTrader classifies a trade as a **whale trade** if it meets *either* of two
independent thresholds:

| Criterion | Default | Rationale |
|-----------|---------|-----------|
| **Absolute size** ≥ `whale_trade_threshold` | 50 XCH | Protects against large directional hits regardless of daily volume |
| **Volume fraction** ≥ `whale_volume_fraction × vol_24h` | 5 % of 24-hour volume | Catches whales on illiquid pairs where 50 XCH may still be huge |

The OR logic means that on very thin markets (e.g. daily volume = 100 XCH) even
a 10 XCH trade (10 % of daily volume) triggers the whale guard.  On liquid pairs
(daily volume = 10 000 XCH) only the absolute 50 XCH threshold applies, since
50 XCH = 0.5 % of daily volume is below the fraction threshold.

### Whale Event Deque

Detected whale events are appended to a per-pair **rolling deque** keyed on
pair name.  Events older than `whale_window_blocks` blocks are evicted on each
new ingestion, so the deque always contains at most the most recent window of
activity.

```
ingest_trade(pair, side, size, block)
    ↓
is_whale? (absolute OR fraction threshold)
    ↓ yes
detect_and_update_whale()
    → push to whale_events_[pair]
    → evict stale events (> whale_window_blocks old)
    → recompute WhaleMetrics
    → log warning
```

---

## Risks and Pitfalls

### 1. Adverse Selection (information risk)

**Risk**: A whale likely has an informational edge (e.g. they know a large CEX
order is about to execute).  When they lift our ask, our remaining ask-side
inventory is at risk: the true price is almost certainly higher than where we
just sold, and our bids will be hit next at a loss.

**Example**: Whale buys 200 XCH from our ask at $2.70.  Underlying price rises
to $2.80.  Our remaining bids at $2.65 get hit, locking in a loss on both legs.

**Mitigation**: Widen spreads aggressively after a whale buy (ask side), making
it expensive for the whale to continue taking.  Optionally cancel and re-post
bids further away from mid.

---

### 2. Inventory Imbalance

**Risk**: Repeated large fills on one side produce a dangerously skewed position.
A market maker long 500 XCH on a falling market faces severe mark-to-market loss.

**Example**: Five consecutive 50 XCH whale buys fill all our asks.  We are now
long zero XCH and long 500 × $2.70 = $1 350 wUSDC — on a pair whose price is
rising.  We have no asks to profit from further upside.

**Mitigation**: The Avellaneda-Stoikov inventory skew already penalises
one-sided positions.  When whale activity is detected, `get_whale_spread_multiplier()`
returns a multiplier > 1.0 that the spread optimizer multiplies into its output,
widening both sides and reducing fill probability until the position normalises.

---

### 3. Price Gapping (quote staleness)

**Risk**: A large trade can gap the mid-price by 20-50 bps in a single block,
leaving all of our resting offers immediately unprofitable.

**Example**: Mid-price is $2.70.  Whale sells 150 XCH.  Dexie mid drops to
$2.65 before our next `refresh()` block.  Our bids at $2.68 are now *above* the
new mid — they get filled immediately at a loss.

**Mitigation**:
- `whale_window_blocks = 10` means the guard remains active for ~8.7 minutes
  (10 × 52 s), giving the market time to stabilise before we return to normal
  spreads.
- The strategy layer should also cancel and re-post all offers upon detecting a
  whale event, rather than waiting for natural expiry.

---

### 4. Liquidation Cascade

**Risk**: On a thin market, a large sell can trigger a cascade: our bids get
filled, the price falls further, then other market makers' bids get hit, driving
the price down further still.

**Mitigation**: The `whale_max_spread_multiplier = 3.0` default means that when
the whale window fills up, our spread reaches 3× normal (e.g. 120 bps instead
of 40 bps).  A **single** event only widens spreads to 1.2× (48 bps), but the
multiplier climbs linearly as events accumulate in the window, making it
progressively expensive for a cascading seller to continue filling our bids.
Combined with inventory skew from the GLFT/Avellaneda strategies, this
significantly reduces tail-loss exposure.

---

### 5. False Positives (normal large trades)

**Risk**: Not every large trade is an informed whale.  Legitimate passive
rebalancers (e.g. a treasury swapping stablecoins at end-of-month) can
trigger the guard unnecessarily, reducing profitability.

**Mitigation**: The linear interpolation formula (`spread_multiplier = 1.0 +
fraction × (max − 1.0)`) means that a **single** whale event at `whale_window_blocks =
10` only produces a multiplier of 1.2 (20 % widening), not the full 3×.  Three
events over 10 blocks give 1.6×.  Only a sustained barrage of whale trades
within the window maxes out the guard.  Operators can tune `whale_trade_threshold`
upward on liquid pairs to reduce false positives.

---

## Response Strategy

XOPTrader's automatic response is encoded in the **spread multiplier** returned
by `get_whale_spread_multiplier()`:

```
spread_multiplier = 1.0 + min(N / whale_window_blocks, 1.0)
                         × (whale_max_spread_multiplier − 1.0)
```

Where `N` = number of whale events in the rolling window.

### Example (defaults: window=10, max_multiplier=3.0)

| Events in window | Multiplier | Normal spread 40 bps → | Comment |
|-----------------|------------|------------------------|---------|
| 0 | 1.0× | 40 bps | No whale — normal spread |
| 1 | 1.2× | 48 bps | Single event: mild caution |
| 3 | 1.6× | 64 bps | Active whale: noticeable widening |
| 5 | 2.0× | 80 bps | Intense activity |
| 10 | 3.0× | 120 bps | Full guard: very wide spread |

### Integration with SpreadOptimizer

The engine queries `get_whale_spread_multiplier(pair)` on each block and applies
it to the final quoted spread:

```cpp
const double base_spread = spread_optimizer.compute_spread(
    volatility, inventory_q, best_competing_bps);

const double whale_mult = market_data_feed->get_whale_spread_multiplier(pair_name);
const double final_spread = base_spread * whale_mult;
```

This means the whale guard stacks multiplicatively with the existing competition
and volatility adjustments — all three widening pressures compound.

### Dominant Side Asymmetry (advanced)

`WhaleMetrics::dominant_side` records the direction of the most-recent whale
event.  A sophisticated strategy layer can use this to apply *asymmetric* spread
widening:

- Whale **bought** (Side::Bid): widen the **ask** (we expect further buying); keep
  bid more competitive to accumulate inventory cheaply.
- Whale **sold** (Side::Ask): widen the **bid** (we expect further selling); keep
  ask more competitive to offload existing inventory.

This is not implemented in the current strategy layer but the data is available
for operators who wish to build on it.

---

## Configuration Reference

These parameters are defined as defaults on `MarketDataConfig` (in
`cpp/include/xop/execution/market_data.hpp`) and are **not yet exposed in
`config.example.yaml`**.  Operators who want to change them today must do so at
construction time (by populating a `MarketDataConfig` before handing it to
`MarketDataFeed`) or at runtime via the provided setter methods:

```cpp
// Construction-time (set before creating MarketDataFeed)
MarketDataConfig cfg;
cfg.whale_trade_threshold      = 100LL * kMojosPerXch; // 100 XCH
cfg.whale_volume_fraction      = 0.03;                 // 3 %
cfg.whale_window_blocks        = 20;                   // ~17 min
cfg.whale_max_spread_multiplier = 4.0;                 // 4× at full window

auto feed = MarketDataFeed(cfg, state);

// Runtime (after construction, e.g. from operator console)
feed.set_whale_trade_threshold(100LL * kMojosPerXch);
feed.set_whale_volume_fraction(0.03);
feed.set_whale_window_blocks(20);
feed.set_whale_max_spread_multiplier(4.0);
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `whale_trade_threshold` | `Mojo` (int64) | 50 × 10^12 | Absolute minimum size to classify as whale |
| `whale_volume_fraction` | `double` | 0.05 | Volume-fraction trigger (OR with absolute) |
| `whale_window_blocks` | `std::size_t` | 10 | Rolling window length in blocks |
| `whale_max_spread_multiplier` | `double` | 3.0 | Maximum spread widening factor |

### Tuning Guidance

**High-liquidity pair** (daily vol > 5 000 XCH):
- Raise `whale_trade_threshold` to 100–200 XCH to avoid excessive false positives.
- Lower `whale_volume_fraction` to 0.02 (2 %) since even small-fraction large
  trades can still be significant.

**Low-liquidity pair** (daily vol < 500 XCH):
- Keep defaults or lower `whale_trade_threshold` to 20–30 XCH.
- Raise `whale_max_spread_multiplier` to 4.0–5.0 to increase protection.
- Raise `whale_window_blocks` to 20 to give more time for the market to settle.

**Conservative operator** (capital < $10 K):
- Raise `whale_max_spread_multiplier` to 5.0.
- Lower `whale_trade_threshold` to 20 XCH.
- Consider pausing quoting entirely (multiplier → ∞ or explicit pause flag)
  when `events_in_window >= 3`.

---

## Architecture

### New Types (`cpp/include/xop/types.hpp`)

```cpp
struct WhaleTradeEvent {
    std::string  pair_name;        // trading pair
    Side         side;             // Bid = whale bought; Ask = whale sold
    Mojo         size;             // trade size in mojos
    double       size_pct_vol;     // fraction of 24-hour volume
    BlockHeight  block_height;     // block of detection
    Timestamp    detected_at;      // wall-clock time
};

struct WhaleMetrics {
    std::string  pair_name;             // trading pair
    std::size_t  events_in_window;      // whale events in the rolling window
    Mojo         largest_trade_size;    // largest single whale trade in window
    double       spread_multiplier;     // recommended spread widening (>= 1.0)
    Side         dominant_side;         // most-recent whale event direction
    bool         is_active;             // true if events_in_window > 0
    BlockHeight  last_event_block;      // block of the most recent event
    Timestamp    last_updated;          // wall-clock time of last computation
};
```

### New Configuration (`MarketDataConfig`)

Four new fields added to `MarketDataConfig`:

- `whale_trade_threshold`
- `whale_volume_fraction`
- `whale_window_blocks`
- `whale_max_spread_multiplier`

### New Methods (`MarketDataFeed`)

**Ingestion:**
```cpp
void ingest_trade(const std::string& pair_name, Side side,
                  Mojo size, BlockHeight block_height);
```

**Accessors:**
```cpp
std::optional<WhaleMetrics> get_whale_metrics(const std::string& pair_name) const;
bool   is_whale_active(const std::string& pair_name) const;
double get_whale_spread_multiplier(const std::string& pair_name) const;
```

**Private helpers:**
```cpp
void   detect_and_update_whale(const std::string& pair_name, Side side,
                                Mojo size, BlockHeight block_height);
double compute_whale_spread_multiplier(std::size_t events_in_window) const;
```

### Thread Safety

Two new `std::shared_mutex` guards are added:

| Mutex | Protects | Lock type |
|-------|----------|-----------|
| `mtx_whale_events_` | `whale_events_` deque map | exclusive on write, shared on read |
| `mtx_whale_metrics_` | `whale_metrics_` map | exclusive on write, shared on read |

The existing lock-ordering rule is extended:

```
pairs_ → arb_/history_/competitors_ → competitor_metrics_
                                    → whale_events_ → whale_metrics_
```

No method acquires more than two mutexes in sequence (whale_events_ then
whale_metrics_, both released before return), so deadlock is impossible.

### Unit Tests (`cpp/tests/test_whale_detection.cpp`)

14 tests covering:
- Small trade not classified as whale.
- Absolute threshold (50 XCH).
- Volume-fraction threshold (5 % of 24h vol).
- Absolute threshold sufficient on high-volume pairs.
- Spread multiplier = 1.0 with no events.
- Linear scaling of multiplier with event count.
- Multiplier clamped at `whale_max_spread_multiplier`.
- `is_whale_active` false / true.
- Window expiry after `whale_window_blocks`.
- Independent per-pair tracking.
- `dominant_side` reflects most-recent event.
- `largest_trade_size` records maximum in window.
- `events_in_window` accumulates correctly.

---

## Avoiding Wipeout: Checklist

1. **Set conservative thresholds** — the default 50 XCH absolute and 5 % volume
   fraction is a reasonable starting point; tune downward for thin markets.

2. **Use the spread multiplier** — integrate `get_whale_spread_multiplier()` into
   the spread optimizer so that the full guard is automatic.

3. **Cancel stale quotes** — after detecting a whale event, cancel and re-post
   *all* offers, not just the side that was hit.

4. **Monitor the dominant side** — use `WhaleMetrics::dominant_side` to apply
   asymmetric widening and reduce adverse-selection exposure.

5. **Set inventory limits** — the inventory risk module (Section 15 of the
   strategy doc) limits total position size regardless of whale activity.
   A whale cannot force you to accumulate an unlimited position if inventory
   limits are respected.

6. **Review `whale_window_blocks`** — 10 blocks (~8.7 min) is the default.  On
   high-volatility days, extend this to 20–30 blocks so the guard remains active
   long enough for the market to stabilise.

7. **Test in simulation first** — run the backtester with historical Dexie data
   and inject synthetic whale events to validate that the spread multiplier
   behaviour matches expectations before going live.
