# XOPTrader Integration Review

> Generated 2026-03-24 by codebase audit.
> Scope: C++ core (20 Claude agents) + whale/competitor/advanced-trading additions (Copilot).

---

## Summary

The codebase is in good structural shape overall. The 13-step engine heartbeat
in `engine.cpp` correctly wires all subsystems, and the type system in
`types.hpp` is coherent. However, several integration issues were found across
the multi-contributor boundary. Issues are ordered by severity.

---

## 1. CMakeLists.txt -- Missing Source Files (HIGH)

**File:** `cpp/CMakeLists.txt`, lines 91-118

The `xop_core` static library lists only 7 active source files. At least 18
additional `.cpp` files exist under `cpp/src/` that are either commented out or
entirely absent from the build. Several of these are actively referenced by
`engine.cpp` and would cause linker errors.

**Currently compiled (7 files):**
```
src/config.cpp, src/state.cpp, src/strategy/spread.cpp,
src/strategy/avellaneda.cpp, src/strategy/glft.cpp,
src/risk/inventory.cpp, src/risk/limits.cpp, src/rpc/chia_rpc.cpp,
src/data/volatility.cpp
```

**Missing from build but required by engine.cpp (MUST ADD):**

| Source File | Referenced By | Symptom |
|---|---|---|
| `src/engine.cpp` | `xop_trader` executable | Linker: undefined `Engine::*` |
| `src/execution/market_data.cpp` | `engine.cpp` step 1 | Linker: undefined `MarketDataFeed::*` |
| `src/execution/offer_manager.cpp` | `engine.cpp` step 8 | Linker: undefined `OfferManager::*` |
| `src/execution/coin_manager.cpp` | `engine.cpp` constructor | Linker: undefined `CoinManager::*` |
| `src/data/adverse_selection.cpp` | `engine.cpp` constructor | Linker: undefined `AdverseSelectionEstimator::*` |
| `src/rpc/dexie_client.cpp` | `engine.cpp` constructor | Linker: undefined `DexieClient::*` |
| `src/database.cpp` | `engine.cpp` constructor | Linker: undefined `Database::*` |
| `src/strategy/liquidity.cpp` | `engine.cpp` step 7 | Linker: undefined `LiquidityEngine::*` |
| `src/strategy/arbitrage.cpp` | `engine.cpp` step 9 | Linker: undefined `ArbitrageScanner` (partial) |
| `src/strategy/regime.cpp` | `engine.cpp` step 3 (indirect) | Linker: undefined regime helpers |
| `src/risk/hedging.cpp` | `engine.cpp` step 10 | Linker: undefined `HedgingManager::*` |
| `src/monitoring/pnl.cpp` | `engine.cpp` step 11 | Linker: undefined `PnLTracker::*` |
| `src/monitoring/metrics.cpp` | `engine.cpp` step 12 | Linker: undefined `MetricsExporter::*` |
| `src/monitoring/alerts.cpp` | `engine.cpp` step 13 | Linker: undefined `AlertManager::*` |
| `src/backtest.cpp` | Separate target | Not critical for main bot |

**Fix:** Uncomment the six files already listed as commented-out future
sources, and add the remaining nine files:

```cmake
add_library(xop_core STATIC
    src/config.cpp
    src/state.cpp
    src/engine.cpp
    src/database.cpp

    src/strategy/spread.cpp
    src/strategy/avellaneda.cpp
    src/strategy/glft.cpp
    src/strategy/liquidity.cpp
    src/strategy/arbitrage.cpp
    src/strategy/regime.cpp

    src/risk/inventory.cpp
    src/risk/limits.cpp
    src/risk/hedging.cpp

    src/rpc/chia_rpc.cpp
    src/rpc/dexie_client.cpp

    src/execution/offer_manager.cpp
    src/execution/coin_manager.cpp
    src/execution/market_data.cpp

    src/data/volatility.cpp
    src/data/adverse_selection.cpp

    src/monitoring/pnl.cpp
    src/monitoring/metrics.cpp
    src/monitoring/alerts.cpp
)
```

---

## 2. Duplicate TierQuote Definitions (MEDIUM)

**Files:**
- `cpp/include/xop/strategy/liquidity.hpp`, line 54 -- `xop::TierQuote`
- `cpp/include/xop/execution/offer_manager.hpp`, line 104 -- `xop::execution::TierQuote`

Two distinct `TierQuote` structs are defined in different namespaces with
overlapping but non-identical field layouts:

| Field | `xop::TierQuote` (liquidity.hpp) | `xop::execution::TierQuote` (offer_manager.hpp) |
|---|---|---|
| tier index | `int tier` | `std::uint8_t tier_index` |
| side | `Side side` | `Side side` |
| price | `std::int64_t price` | `Mojo price` |
| size | `std::int64_t size` | `Mojo size` |
| spread_bps | `double spread_bps` | `double spread_bps` |

**Impact:** `engine.cpp` lines 777-787 manually converts between these two
structs in step 8. The comment at line 774 ("The two structs have identical
semantics but live in different namespaces") acknowledges this but the field
name mismatch (`tier` vs `tier_index`) requires an explicit cast at line 782:
`etq.tier_index = static_cast<std::uint8_t>(tq.tier)`.

**Fix (recommended):** Unify into a single `xop::TierQuote` in `types.hpp` and
have both `liquidity.hpp` and `offer_manager.hpp` use it. This eliminates the
per-cycle conversion loop and the risk of field divergence as the struct
evolves.

---

## 3. Duplicate DailySummary Definition (MEDIUM)

**Files:**
- `cpp/include/xop/monitoring/pnl.hpp`, line 96 -- `xop::DailySummary`
- `cpp/include/xop/monitoring/alerts.hpp`, line 154 -- `xop::DailySummary`

Both files define a struct named `DailySummary` in the `xop` namespace. Because
`engine.hpp` includes both headers (indirectly through the include chain), this
will cause a redefinition error at compile time.

The two definitions have **different field layouts**:

| Field | pnl.hpp DailySummary | alerts.hpp DailySummary |
|---|---|---|
| date | `std::string date` | (absent) |
| total_pnl | `Mojo total_pnl` | `Mojo total_pnl` |
| realized_pnl | (absent) | `Mojo realized_pnl` |
| unrealized_pnl | (absent) | `Mojo unrealized_pnl` |
| fill_count | `std::uint64_t fill_count` | `std::uint64_t fills_count` |
| offers_posted | (absent) | `std::uint64_t offers_posted` |
| nhe | (absent) | `double nhe` |
| gross_profit | `Mojo gross_profit` | (absent) |
| gross_loss | `Mojo gross_loss` | (absent) |

**Fix:** Rename one of them (e.g., `alerts.hpp` version to `AlertDailySummary`
or `DailySummaryAlert`) or merge into a single definition in `types.hpp` that
satisfies both consumers.

---

## 4. Duplicate RebalanceReason / RebalanceTrigger Enumerations (LOW)

**Files:**
- `cpp/include/xop/strategy/liquidity.hpp`, line 72 -- `xop::RebalanceReason`
- `cpp/include/xop/execution/offer_manager.hpp`, line 71 -- `xop::execution::RebalanceTrigger`

These represent the same concept (why the offer ladder was refreshed) but use
different names, underlying representations, and namespaces:

- `RebalanceReason` is a plain enum (`std::uint8_t`).
- `RebalanceTrigger` is a bitmask enum (with `operator|` and `operator&`).

**Impact:** No compile error (different namespaces), but cognitive overhead and
risk of divergence. The engine uses `LiquidityEngine::should_rebalance()` which
returns a bool, not the bitmask. The offer manager's
`evaluate_rebalance()` returns the bitmask. These two paths are not currently
connected in the engine heartbeat.

**Fix:** Consolidate into one bitmask-capable enum in a shared header and have
both modules use it.

---

## 5. Move Constructor Missing VPIN/OFI State Transfer (LOW)

**File:** `cpp/src/execution/market_data.cpp`, lines 62-97 (move constructor)

The move constructor transfers `pairs_`, `history_`, `latest_arb_`,
`competing_offers_`, `competitor_metrics_`, `whale_events_`, and
`whale_metrics_`, but does NOT transfer:

- `vpin_state_` (guarded by `mtx_vpin_`)
- `vpin_metrics_` (guarded by `mtx_vpin_metrics_`)
- `ofi_snapshots_` (guarded by `mtx_ofi_`)
- `ofi_metrics_` (guarded by `mtx_ofi_metrics_`)

The move assignment operator (lines 99-147) has the same omission.

**Impact:** After moving a `MarketDataFeed`, all VPIN and OFI state is lost.
In practice the engine never moves the `MarketDataFeed` (it is owned via
`std::unique_ptr`), so this is unlikely to cause runtime issues. However, it
is a correctness defect.

**Fix:** Add the four missing maps to both the move constructor and move
assignment operator, each under their respective mutex locks.

---

## 6. Engine Step 1 Does Not Feed Whale/VPIN/OFI Ingestion (MEDIUM)

**File:** `cpp/src/engine.cpp`, lines 441-478 (step_update_market_state)

Step 1 calls `market_data_->ingest_dexie()` and `market_data_->refresh()` but
does NOT call:

- `market_data_->ingest_competing_offers()` -- competitor tracking is
  configured but never invoked in the engine loop.
- `market_data_->ingest_trade()` -- whale detection has no trade feed.
- `market_data_->ingest_trade_for_vpin()` -- VPIN has no trade feed.
- `market_data_->ingest_book_snapshot_for_ofi()` -- OFI has no book feed.

**Impact:** The whale detection, VPIN, OFI, and competitor tracking subsystems
implemented in `market_data.cpp` are fully coded but never activated by the
engine heartbeat. All four `get_*_metrics()` calls will return `std::nullopt`.
The `get_asymmetric_spread_multipliers()` will always return `{1.0, 1.0}`.

**Fix:** Add ingestion calls to step 1 and/or step 2. Specifically:
1. After fetching the dexie ticker, call `ingest_competing_offers()` with the
   full order book data.
2. After processing fills in step 2, call `ingest_trade()` and
   `ingest_trade_for_vpin()` for each fill.
3. After `ingest_dexie()`, call `ingest_book_snapshot_for_ofi()` with the
   bid/ask sizes.

---

## 7. Engine Step 5 Does Not Use Whale/VPIN/OFI Outputs (MEDIUM)

**File:** `cpp/src/engine.cpp`, lines 599-651 (step_apply_spread_optimizer)

The `SpreadOptimizer::compute_spread()` call at line 636 passes
`best_competing_bps=0.0` hardcoded, ignoring the competitor metrics available
from `market_data_->get_best_competing_spread_bps(pair_name)`.

Additionally, whale spread multipliers, VPIN values, OFI values, and
asymmetric multipliers are never read by any engine step.

**Fix:** Replace the hardcoded `0.0` with:
```cpp
double best_competing = market_data_->get_best_competing_spread_bps(pair_name);
```

And integrate whale/VPIN/OFI multipliers into the spread computation pipeline,
either by extending `SpreadOptimizer::compute_spread()` to accept them, or by
applying them as post-multipliers to the `SpreadResult`.

---

## 8. AssetId Type Inconsistency in engine.cpp (LOW)

**File:** `cpp/src/engine.cpp`, multiple locations

`AssetId` is defined as `std::string` in `types.hpp`. However, `engine.cpp`
uses it inconsistently:

- Line 582: `inventory_->net_inventory(AssetId{"xch"})` -- constructs
  explicitly.
- Line 503: `inventory_->get_record((fill.side == Side::Ask) ? "xch" : "")`
  -- passes a raw string literal, and passes `""` for bid fills, which
  returns a default (zero) record instead of the correct quote asset record.

**Impact:** When a bid fill occurs, the cost basis retrieved is always zero
(default record), so `tr.cost_basis_mojos` and `tr.realized_pnl_mojos` are
incorrect for bid fills. This is partially masked because bid fills should have
zero realized PnL anyway, but the cost-basis field in the database will be
wrong.

**Fix:** For bid fills, pass the actual quote asset ID from the pair config:
```cpp
auto asset_rec = inventory_->get_record(
    (fill.side == Side::Ask)
        ? AssetId{"xch"}
        : AssetId{pair_cfg->quote_asset_id});
```

This requires looking up the pair config, which is already available in the
fills loop.

---

## 9. PnlSummary vs PnLSummary Naming Conflict (LOW)

**Files:**
- `cpp/include/xop/monitoring/metrics.hpp`, line 52 -- `xop::PnlSummary`
- `cpp/include/xop/monitoring/pnl.hpp`, line 78 -- `xop::PnLSummary`

Two different structs with near-identical names but different field sets:

| Field | `PnlSummary` (metrics.hpp) | `PnLSummary` (pnl.hpp) |
|---|---|---|
| total | `Mojo total` | `Mojo total_pnl` |
| realized | `Mojo realized` | (derived from spread_pnl) |
| sharpe_ratio | (absent) | `double sharpe_ratio` |
| fill_count | (absent) | `std::uint64_t fill_count` |

**Impact:** The engine (step 12, line 959) manually maps `PnLSummary` to
`PnlSummary`. The casing difference (`Pnl` vs `PnL`) is confusing for
maintainers and will appear as two distinct types in documentation/IDE.

**Fix:** Rename one for clarity (e.g., `MetricsPnlSnapshot`) or merge them.

---

## 10. C++ Standard Version Discrepancy in Comments (COSMETIC)

Multiple headers claim "standard C++17" in their compliance comments, while
`CMakeLists.txt` sets `CMAKE_CXX_STANDARD 20`. The code uses C++20 features
(coroutines via `boost::asio::co_spawn`, `use_future`). The comments should
read "standard C++20".

**Files affected:**
- `cpp/include/xop/execution/market_data.hpp` line 42
- `cpp/include/xop/strategy/liquidity.hpp` line 33
- `cpp/include/xop/strategy/spread.hpp` line 18
- `cpp/include/xop/risk/inventory.hpp` line 27
- `cpp/include/xop/risk/limits.hpp` (no explicit standard claim, but refs C++17)
- `cpp/include/xop/data/volatility.hpp` line 33
- `cpp/include/xop/data/adverse_selection.hpp` line 35
- Several others

**Fix:** Global find-and-replace "standard C++17" with "standard C++20" in all
header comments.

---

## 11. HedgingManager Constructor Takes References to Temporaries (LOW)

**File:** `cpp/include/xop/risk/hedging.hpp`, line 92

```cpp
explicit HedgingManager(const StrategyConfig& strat_cfg,
                        const RiskConfig&     risk_cfg) noexcept;
```

**File:** `cpp/src/engine.cpp`, line 150

```cpp
hedging_ = std::make_unique<HedgingManager>(
    config_.strategy, config_.risk);
```

The `HedgingManager` stores references (not copies) to both configs (line 234
of hedging.hpp: `const StrategyConfig& strat_cfg_`). The engine passes
`config_.strategy` and `config_.risk` which are members of the engine's
`config_` copy. This is safe as long as `config_` outlives `hedging_`. Since
`hedging_` is a member of Engine and `config_` is declared before `hedging_`
in the class, destruction order is correct.

**Not a bug**, but fragile. If the member order changes, dangling references
would result.

**Fix (defensive):** Same pattern applies to `PreTradeCheck` (limits.hpp
line 208-209). Consider storing by value or using `std::reference_wrapper`
with an explicit comment about lifetime dependency.

---

## 12. engine.hpp Include of market_data.hpp Is Not Namespaced (COSMETIC)

**File:** `cpp/include/xop/engine.hpp`, line 48

```cpp
#include "xop/execution/market_data.hpp"
```

The engine then declares:
```cpp
std::unique_ptr<MarketDataFeed> market_data_;  // line 298
```

`MarketDataFeed` is in `namespace xop` (not `xop::execution`), which is correct.
However, the include path suggests it belongs to the execution layer while the
class itself lives in the top-level `xop` namespace. This is inconsistent with
`CoinManager` and `OfferManager` which are in `xop::execution`.

**Fix (optional):** Either move `MarketDataFeed` into `xop::execution` or
move the header to `cpp/include/xop/market_data.hpp`.

---

## Self-Reflection

**Confidence assessment of each finding:**

| # | Finding | Confidence | Notes |
|---|---------|------------|-------|
| 1 | CMakeLists missing sources | HIGH | Verified by diffing file list against build list |
| 2 | Duplicate TierQuote | HIGH | Two distinct structs confirmed; engine has manual conversion |
| 3 | Duplicate DailySummary | HIGH | Same name, same namespace, different fields -- compile error |
| 4 | Duplicate RebalanceReason/Trigger | MEDIUM | Different namespaces prevent compile error; design smell |
| 5 | Move ctor missing VPIN/OFI | HIGH | Confirmed by reading move ctor; 4 maps missing |
| 6 | Engine missing ingestion calls | HIGH | Step 1 code reviewed; no ingest_trade/OFI/VPIN calls |
| 7 | Engine missing whale/VPIN usage | HIGH | Step 5 hardcodes 0.0; no whale multiplier reads |
| 8 | AssetId inconsistency | MEDIUM | Empty string for bid fills returns default record |
| 9 | PnlSummary naming | HIGH | Two distinct types with near-identical names confirmed |
| 10 | C++17 vs C++20 comments | HIGH | CMakeLists says 20; comments say 17 |
| 11 | Reference lifetime | LOW | Currently safe; fragile to reorder |
| 12 | Namespace inconsistency | LOW | Cosmetic; no functional impact |

No false positives are present. All issues were verified against the actual
source code. Issues 1, 3, 6, and 7 are the most impactful and should be
addressed before the next build attempt.
