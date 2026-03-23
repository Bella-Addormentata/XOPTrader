# Competitor Detection and Response Strategy

## Overview

XOPTrader now includes comprehensive competitor detection capabilities to adapt when other market makers appear on CHIA DEX exchanges. This document explains the system architecture, integration points, and strategic response mechanisms.

## Problem Statement

While CHIA DEX markets currently have zero professional market maker competition (as documented in the strategy paper), the system must be prepared to respond if competitors emerge. The original spread optimizer included a competition component (`s_competition = max(s_floor, best_competing + epsilon)`) but always received `best_competing_bps = 0.0` because no competitor detection was implemented.

## Solution Architecture

### 1. Type Definitions (`types.hpp`)

Three new types support competitor tracking:

#### `CompetingOffer`
Represents an individual offer from another market participant:
```cpp
struct CompetingOffer {
    std::string  offer_id;         // unique offer identifier from dexie
    std::string  pair_name;        // trading pair
    Side         side;             // bid or ask
    Mojo         price;            // offer price (mojos)
    Mojo         size;             // offered quantity (mojos)
    BlockHeight  first_seen_block; // when first observed
    BlockHeight  last_seen_block;  // most recent observation
    Timestamp    last_seen_ts;     // wall-clock last seen
};
```

#### `CompetitorMetrics`
Aggregated statistics about competing market makers per pair:
```cpp
struct CompetitorMetrics {
    std::string  pair_name;
    std::size_t  num_competing_offers;   // total non-own offers
    double       best_competing_bid_bps; // best bid spread vs mid
    double       best_competing_ask_bps; // best ask spread vs mid
    double       best_competing_spread_bps; // tightest two-sided spread
    std::size_t  competing_depth_bids;   // number of bid offers
    std::size_t  competing_depth_asks;   // number of ask offers
    bool         new_competitor_detected; // first time seeing competitors
    Timestamp    last_updated;
};
```

### 2. MarketDataFeed Extensions (`market_data.hpp/cpp`)

#### Configuration
New `MarketDataConfig` fields control competitor tracking:

```cpp
struct MarketDataConfig {
    // ... existing fields ...

    bool enable_competitor_tracking{true};
    Mojo min_competitor_offer_size{1'000'000'000'000LL}; // 1 XCH minimum
    double competitor_alert_threshold_bps{50.0}; // alert if spread < 50 bps
};
```

#### Data Ingestion
New method to parse individual offers from dexie order book:

```cpp
void ingest_competing_offers(
    const std::string& pair_name,
    const std::vector<CompetingOffer>& competing_offers,
    const std::unordered_set<std::string>& own_offer_ids);
```

**Filtering Logic:**
- Excludes our own offers via `own_offer_ids` set
- Filters out dust offers below `min_competitor_offer_size` (default 1 XCH)
- Only tracks "serious" market maker offers

#### Metrics Computation
Internal method to analyze competing offers:

```cpp
std::optional<CompetitorMetrics> compute_competitor_metrics(
    const std::string& pair_name,
    double mid_price);
```

**Computation Steps:**
1. Separate competing offers into bids and asks
2. Find best competing bid (highest price) and best competing ask (lowest price)
3. Compute spreads in basis points relative to mid-price
4. Calculate tightest two-sided spread: `(best_ask - best_bid) / mid * 10000`
5. Detect if this is a new competitor (first time seeing any competing offers)
6. Log warnings if competing spread < alert threshold

#### Public Accessors
Three methods provide competitor data to the engine:

```cpp
// Full metrics structure
std::optional<CompetitorMetrics> get_competitor_metrics(
    const std::string& pair_name) const;

// The key value fed into SpreadOptimizer::compute_spread()
double get_best_competing_spread_bps(const std::string& pair_name) const;

// Simple count for monitoring
std::size_t get_num_competing_offers(const std::string& pair_name) const;
```

### 3. Integration with Engine (`engine.cpp`)

**Per-Block Heartbeat Integration:**

The engine's 13-step cycle needs to be extended at **Step 1 (Fetch market data)**:

```cpp
// EXISTING: Step 1 - Fetch market data
void Engine::step_fetch_market_data(const std::vector<std::string>& enabled_pairs) {
    for (const auto& pair : enabled_pairs) {
        // Fetch dexie order book data
        auto order_book = dexie_client_->get_offers(pair);

        // EXISTING: Aggregate best bid/ask
        market_data_feed_->ingest_dexie(
            pair,
            order_book.best_bid,
            order_book.best_ask,
            order_book.last_trade,
            order_book.volume_24h);

        // NEW: Parse individual competing offers
        std::vector<CompetingOffer> competing_offers;
        std::unordered_set<std::string> own_offer_ids;

        // Get our own offer IDs from OfferManager
        auto pending_offers = offer_manager_->get_pending_offers(pair);
        for (const auto& offer : pending_offers) {
            own_offer_ids.insert(offer.offer_id);
        }

        // Parse individual offers from dexie response
        for (const auto& dexie_offer : order_book.offers) {
            CompetingOffer offer;
            offer.offer_id = dexie_offer.id;
            offer.pair_name = pair;
            offer.side = dexie_offer.price > order_book.mid ? Side::Ask : Side::Bid;
            offer.price = convert_to_mojos(dexie_offer.price);
            offer.size = convert_to_mojos(dexie_offer.size);
            offer.first_seen_block = current_block_height_;
            offer.last_seen_block = current_block_height_;
            offer.last_seen_ts = std::chrono::system_clock::now();

            competing_offers.push_back(offer);
        }

        // Ingest competing offers for tracking
        market_data_feed_->ingest_competing_offers(pair, competing_offers, own_offer_ids);

        // Compute competitor metrics (happens automatically inside MarketDataFeed)
        // This will log warnings if tight competing spreads detected
    }
}
```

**Step 5 (Apply Spread Optimizer) - Updated Call:**

```cpp
// BEFORE (hardcoded best_competing_bps = 0.0):
pcs.spread_result = spread_opt_->compute_spread(
    mid, sigma, q, q_max, pin,
    Venue::Dexie, /*best_competing_bps=*/0.0,  // ← ALWAYS 0.0!
    hour_utc, day_of_week);

// AFTER (dynamic competitor-aware):
const double best_competing_bps =
    market_data_feed_->get_best_competing_spread_bps(pair_name);

pcs.spread_result = spread_opt_->compute_spread(
    mid, sigma, q, q_max, pin,
    Venue::Dexie, best_competing_bps,  // ← DYNAMIC!
    hour_utc, day_of_week);
```

## Strategic Response Mechanisms

### 1. Spread Adaptation

When competitors are detected, the `s_competition` component of the spread model becomes active:

```
s_competition = max(s_floor_bps, best_competing_bps + epsilon_bps)
```

**Behavior:**
- **No competitors**: Falls back to `s_floor_bps` (40 bps default)
- **Wide competitors** (e.g., 200 bps spread): We maintain tight spreads (`max(40, 200+2) = 202 bps`)
- **Tight competitors** (e.g., 45 bps spread): We improve by epsilon (`max(40, 45+2) = 47 bps`)
- **Extremely tight competitors** (e.g., 30 bps): We maintain floor (`max(40, 30+2) = 40 bps`)

**Configuration Tuning** (`spread.hpp`):
```cpp
struct SpreadConfig {
    double s_floor_bps{40.0};      // Minimum profitable spread
    double epsilon_bps{2.0};       // Improvement over competitor
    // ... other components ...
};
```

**Strategic Implications:**
- **Price improvement**: We always try to beat competitors by `epsilon` (default 2 bps)
- **Profit protection**: Never go below `s_floor` (maintains minimum 40 bps edge)
- **No race-to-zero**: The floor prevents destructive competition

### 2. Multi-Tier Ladder Adjustment

When competitors appear, consider adjusting the multi-tier offer ladder spacing:

**Default Configuration** (zero competition):
```yaml
tiers:
  - spread_offset_bps: 60    # Tier 0: tight, high fill probability
  - spread_offset_bps: 200   # Tier 1: moderate
  - spread_offset_bps: 500   # Tier 2: wide
  - spread_offset_bps: 1000  # Tier 3: very wide (inventory reduction)
```

**Recommended Adjustment** (tight competition detected):
```yaml
tiers:
  - spread_offset_bps: 40    # Tier 0: match s_floor, be most competitive
  - spread_offset_bps: 100   # Tier 1: tighter spacing
  - spread_offset_bps: 300   # Tier 2: moderate
  - spread_offset_bps: 800   # Tier 3: wide
```

**Rationale:**
- Concentrate liquidity near the competitive spread
- Maintain deeper tiers for adverse selection protection
- Reduce average spread to capture more fills

### 3. Rebalancing Trigger Adjustment

When competitors are present, consider tightening rebalancing triggers to respond faster:

**Default Triggers** (zero competition):
```cpp
OfferManagerConfig {
    double price_dev_threshold_bps{200.0};   // 2% price move triggers rebalance
    double inventory_skew_threshold{0.60};   // 60% skew triggers rebalance
    BlockHeight ttl_blocks{69};              // ~1 hour TTL
};
```

**Recommended Adjustment** (tight competition):
```cpp
OfferManagerConfig {
    double price_dev_threshold_bps{100.0};   // 1% (faster price tracking)
    double inventory_skew_threshold{0.50};   // 50% (more aggressive inventory management)
    BlockHeight ttl_blocks{52};              // ~45 minutes (refresh quotes more often)
};
```

### 4. Alert Configuration

The system logs warnings when tight competing spreads are detected:

```cpp
// In market_data.cpp:
if (best_competing_spread_bps > 0.0 &&
    best_competing_spread_bps < config_.competitor_alert_threshold_bps) {
    spdlog::warn("MarketDataFeed: tight competitor detected on pair={}, "
                 "competing_spread={:.1f}bps (threshold={:.1f}bps)",
                 pair_name, best_competing_spread_bps,
                 config_.competitor_alert_threshold_bps);
}
```

**Telegram Alert Integration** (future):
When adding to the alert system (`alerts.hpp`), include:
- **Rule**: `competitor_tight_spread`
- **Trigger**: `best_competing_spread_bps < 50 bps AND new_competitor_detected`
- **Priority**: `medium`
- **Message**: "New competitor detected on {pair} with {spread}bps spread"

## Monitoring and Metrics

### Prometheus Metrics (to be added)

```cpp
// In prometheus_exporter.cpp
METRIC(xop_competing_offers_total, Gauge)
    .help("Number of competing offers on the order book")
    .labels({"pair"});

METRIC(xop_best_competing_spread_bps, Gauge)
    .help("Tightest competing spread in basis points")
    .labels({"pair"});

METRIC(xop_competitor_detections_total, Counter)
    .help("Total number of new competitor detection events")
    .labels({"pair"});
```

### Grafana Dashboard Panel

**Competitor Activity** panel:
- Time series: `xop_competing_offers_total{pair="XCH/wUSDC"}`
- Time series: `xop_best_competing_spread_bps{pair="XCH/wUSDC"}`
- Annotation: Vertical marker when `xop_competitor_detections_total` increments

## Testing Strategy

### Unit Tests (`tests/test_competitor_detection.cpp`)

Key test cases:
1. **No competitors**: `get_best_competing_spread_bps()` returns 0.0
2. **Own offers filtered**: Offers in `own_offer_ids` are excluded
3. **Dust filtering**: Offers below `min_competitor_offer_size` are excluded
4. **Best spread calculation**: Correct computation of tightest two-sided spread
5. **New competitor detection**: `new_competitor_detected` flag set correctly
6. **Alert threshold**: Warning logged when spread < threshold

### Integration Test Scenarios

**Scenario 1: Competitor Appears**
1. Start with zero competitors
2. Inject competing offers via dexie mock
3. Verify `best_competing_bps` fed into spread optimizer
4. Verify spread widens to `max(40, competing_spread + 2)`
5. Verify alert logged

**Scenario 2: Competitor Leaves**
1. Start with active competitors
2. Remove competing offers from order book
3. Verify `best_competing_bps` returns to 0.0
4. Verify spread falls back to `s_floor`

**Scenario 3: Tight Competition**
1. Inject competitor with 35 bps spread (below our floor)
2. Verify we maintain 40 bps floor (don't go below profitable)
3. Verify no destructive race-to-zero

## Configuration Reference

### Complete `config.yaml` Settings

```yaml
market_data:
  enable_competitor_tracking: true
  min_competitor_offer_size_xch: 1.0  # Minimum 1 XCH to be considered "serious"
  competitor_alert_threshold_bps: 50.0  # Alert if competing spread < 50 bps

spread_optimizer:
  s_floor_bps: 40.0       # Minimum profitable spread
  epsilon_bps: 2.0        # Price improvement over competitor
  # ... other spread components ...

offer_manager:
  # Adjust these when competitors detected:
  price_dev_threshold_bps: 100.0   # Faster rebalancing
  inventory_skew_threshold: 0.50   # More aggressive inventory management
  ttl_blocks: 52                   # Refresh quotes more frequently
```

## Migration Path

### Phase 1: Infrastructure (Completed)
- ✅ Add `CompetingOffer` and `CompetitorMetrics` types
- ✅ Extend `MarketDataFeed` with competitor tracking
- ✅ Implement `ingest_competing_offers()` and `compute_competitor_metrics()`

### Phase 2: Engine Integration (Required)
- [ ] Modify `Engine::step_fetch_market_data()` to parse individual offers
- [ ] Build `own_offer_ids` set from `OfferManager`
- [ ] Call `ingest_competing_offers()` each block
- [ ] Update `Engine::step_apply_spread_optimizer()` to use dynamic `best_competing_bps`

### Phase 3: Monitoring (Recommended)
- [ ] Add Prometheus metrics for competitor activity
- [ ] Create Grafana dashboard panel
- [ ] Add Telegram alert rule for tight competitor spreads

### Phase 4: Testing (Recommended)
- [ ] Write unit tests for `compute_competitor_metrics()`
- [ ] Create integration test scenarios
- [ ] Run backtests with simulated competitor activity

## Performance Considerations

### CPU Impact
- **Per-block cost**: O(N) where N = number of offers in order book
- **Typical N**: 10-50 offers per pair on dexie (very low)
- **Filtering cost**: Hash set lookup for each offer (O(1) amortized)
- **Negligible overhead**: < 1ms per block even with 100 offers

### Memory Impact
- **Per-pair storage**: ~40 bytes per `CompetingOffer`
- **Typical count**: 10-20 competing offers per pair
- **Total memory**: < 1 KB per pair (trivial)

### Lock Contention
- **Three new mutexes**: `mtx_competitors_`, `mtx_competitor_metrics_`
- **Access pattern**: Writer-heavy (engine writes each block, strategies read)
- **Mitigation**: Used `std::shared_mutex` for concurrent reads
- **No deadlock risk**: No method acquires more than one mutex

## Security and Privacy

### Offer Tracking
- **No PII collected**: Only public on-chain offer data
- **No wallet identification**: Cannot determine if offers are from same operator
- **Passive observation**: Read-only analysis of public order book

### Anti-Gaming
- **Floor protection**: Competitors cannot force us below profitable spreads
- **Own-offer exclusion**: Cannot "game" ourselves by detecting our own offers
- **Dust filtering**: Ignores tiny offers that aren't from serious market makers

## Future Enhancements

### Pattern Recognition (Phase 5)
- **Offer refresh tracking**: Detect if competitors use fixed rebalancing intervals
- **Size clustering**: Identify if multiple offers likely from same operator
- **Predictive positioning**: Anticipate competitor rebalancing and adjust preemptively

### Adaptive Epsilon (Phase 6)
- **Dynamic improvement**: Adjust `epsilon_bps` based on fill rate
- **Aggressive mode**: Increase epsilon when losing fills to competitors
- **Passive mode**: Reduce epsilon when fill rate is high (no need to compete as hard)

### Cross-Venue Arbitrage (Phase 7)
- **TibetSwap competitor tracking**: Extend to AMM pools
- **Cross-DEX positioning**: Maintain tighter spreads on venue with most competition

## Conclusion

The competitor detection system provides XOPTrader with the ability to:

1. **Detect** other market makers via order book analysis
2. **Adapt** spread pricing dynamically via the competition component
3. **Alert** operators when significant competition appears
4. **Maintain** profitability via the spread floor protection

While CHIA DEX markets currently have zero professional competition, this infrastructure ensures the system can respond intelligently if competitors emerge in the future, maintaining competitive positioning without sacrificing profitability.
