# Sanity Failure Logging System

## Overview

A new `sanity_failures` database table has been added to track all offers that fail pre-posting sanity checks. This enables post-hoc analysis of pricing anomalies and helps identify patterns in failed offers.

## Database Schema

```sql
CREATE TABLE sanity_failures (
    id                     INTEGER PRIMARY KEY AUTOINCREMENT,
    block_height           INTEGER NOT NULL,
    pair_name              TEXT    NOT NULL,
    side                   TEXT    NOT NULL CHECK(side IN ('bid','ask')),
    tier                   INTEGER NOT NULL,
    proposed_price_mojos   INTEGER NOT NULL,
    reference_price_mojos  INTEGER NOT NULL,
    deviation_pct          REAL    NOT NULL,
    failure_reason         TEXT    NOT NULL,
    details                TEXT,
    created_at             TEXT    DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_sanity_failures_pair
    ON sanity_failures(pair_name, block_height DESC);
```

## Integration Points

### 1. Database Layer (cpp/src/database.cpp)
- **New struct**: `DbSanityFailure` - maps to sanity_failures table row
- **New method**: `Database::insert_sanity_failure(const DbSanityFailure& record)`
- **New prepared statement**: `stmt_insert_sanity_failure_`
- **Schema migration**: Added `kCreateSanityFailures` and `kIndexSanityFailuresPair` DDL

### 2. Engine Layer (cpp/src/engine.cpp - Step 8)
Two types of sanity checks are now logged:

#### **Crossed-Mid Guard** (existing)
- Filters out offers that would cross the model mid-price
- Already integrated; sanity logging deferred to BBO guard below

#### **BBO Proximity Sanity Guard** (new)
  
**Pair-Level Check:**
- Condition: Model mid deviates >15% from BBO mid
- Action: All tiers for the pair are suppressed
- Log Entry: `failure_reason = "bbo_deviation_pair_level"`
- Context: `details` field includes `model_mid`, `bbo_mid`, `model_mid_dev_pct`

**Tier-Level Check:**
- Condition: Individual tier price deviates >15% from side-specific BBO
- Action: That tier is suppressed; others may still post
- Log Entry: `failure_reason = "bbo_deviation_tier_level"`
- Context: `details` field includes `tier_price`, `side_bbo`, `tier_dev_pct`

## Usage Examples

### Query All Failures for a Pair (Last 100 blocks)
```sql
SELECT block_height, side, tier, proposed_price_mojos, 
       reference_price_mojos, deviation_pct, failure_reason
FROM sanity_failures
WHERE pair_name = 'XCH/BYC' AND block_height > (SELECT MAX(block_height) - 100 FROM sanity_failures)
ORDER BY block_height DESC;
```

### Find Patterns (Most Common Failure Reasons)
```sql
SELECT failure_reason, COUNT(*) as count, 
       AVG(deviation_pct) as avg_deviation,
       MAX(deviation_pct) as max_deviation
FROM sanity_failures
WHERE pair_name = 'XCH/BYC'
GROUP BY failure_reason
ORDER BY count DESC;
```

### Analyze Pair-Level Suppression Events
```sql
SELECT block_height, COUNT(DISTINCT tier) as tiers_suppressed,
       AVG(deviation_pct) as avg_deviation
FROM sanity_failures
WHERE pair_name = 'XCH/BYC' AND failure_reason = 'bbo_deviation_pair_level'
GROUP BY block_height
ORDER BY block_height DESC
LIMIT 20;
```

### Monitor Real-Time (via GUI)
The [GUI database service](gui/services/database_service.py) has full read access to the sanity_failures table via SQLite. Add a new dashboard tab to display recent failures grouped by pair and reason.

## Constants

Both sanity checks use the same threshold:
- **kMaxBboDeviation = 0.15** (15% deviation tolerance)

This threshold can be adjusted in [engine.cpp](cpp/src/engine.cpp) around line 4680 if needed.

## Related Files

- **Header**: [cpp/include/xop/database.hpp](cpp/include/xop/database.hpp) - DbSanityFailure struct
- **Database**: [cpp/src/database.cpp](cpp/src/database.cpp) - Schema & insert implementation
- **Engine**: [cpp/src/engine.cpp](cpp/src/engine.cpp) - Step 8 check & logging
- **Market Data**: [cpp/src/execution/market_data.cpp](cpp/src/execution/market_data.cpp) - Upstream microprice clamp (Layer 1)

## Performance Considerations

- **Insert Cost**: ~1ms per failure (sqlite3 prepared statement)
- **Indexed Query**: Lookups are O(log n) via `pair_name + block_height DESC` index
- **Storage**: ~200 bytes per row; expect 1000-5000 rows/day under normal operations
- **No Lock Contention**: Uses existing database mutex (same as offer_log)

## Future Enhancements

1. **Real-Time Alerting**: Trigger notification if failure rate exceeds threshold
2. **Cross-Pair Analysis**: Compare implied prices (XCH/BYC vs XCH/wUSDC + BYC/wUSDC)
3. **Minimum Depth Filter**: Only trust orderbook if both sides have ≥X liquidity
4. **Backtest Integration**: Pre-compute failure patterns to refine 15% threshold
5. **Circuit Breaker**: Auto-disable pair if suppressed N times in M blocks
