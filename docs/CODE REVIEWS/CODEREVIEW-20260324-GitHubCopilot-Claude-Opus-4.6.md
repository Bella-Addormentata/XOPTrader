# XOPTrader Comprehensive Code Review

**Date:** March 24, 2026  
**Reviewer:** GitHub Copilot (Claude Opus 4.6)  
**Repository:** dorkmo/XOPTrader  
**Branch:** main  

---

## Prompt Context

> "Now please perform a comprehensive code review of XOPTrader. Look for logical errors and coding errors. Also look for pitfalls and missing logical strategies. Also look for ways to clean up the code and timing of logical strategies. Please place your review in the code review folder when you are complete. Please include this prompt in the note for added context. Please consider anything we might have missed for this review."

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [Critical Bugs — Must Fix Before Live Trading](#3-critical-bugs--must-fix-before-live-trading)
4. [High-Severity Issues — Must Fix Before Paper Trading](#4-high-severity-issues--must-fix-before-paper-trading)
5. [Logical Errors in Strategy Layer](#5-logical-errors-in-strategy-layer)
6. [Risk Management Gaps](#6-risk-management-gaps)
7. [Timing and Ordering Issues](#7-timing-and-ordering-issues)
8. [Missing Logical Strategies](#8-missing-logical-strategies)
9. [Code Quality and Cleanup Opportunities](#9-code-quality-and-cleanup-opportunities)
10. [Test Coverage Analysis](#10-test-coverage-analysis)
11. [Build System and Configuration](#11-build-system-and-configuration)
12. [Security Concerns](#12-security-concerns)
13. [Performance Considerations](#13-performance-considerations)
14. [Recommendations and Prioritized Action Items](#14-recommendations-and-prioritized-action-items)

---

## 1. Executive Summary

XOPTrader is an ambitious C++20 CHIA DEX market-making engine implementing Avellaneda-Stoikov and GLFT strategies with extensive risk management, multi-tier liquidity provision, whale detection, and a backtesting framework. The codebase shows strong theoretical grounding in market microstructure literature and thorough inline documentation.

**Strengths:**
- Excellent mathematical documentation within source files
- Robust type system (mojos as `int64_t`, 128-bit wide-multiply for overflow safety)
- Thread-safe `State` class with deadlock-free locking design
- Comprehensive 13-step engine heartbeat with graceful degradation
- Strong config validation covering domain constraints

**Critical Findings:**
- **7 blocking production bugs** that would prevent the system from functioning on-chain
- **Severe thread-safety gaps** across 8+ modules that are called from the engine loop
- **4 independent, inconsistent** variance ratio implementations
- **~74% of modules have zero test coverage**, including the PreTradeCheck (the system's core safety gate)
- **The `main.cpp` engine is a stub** — the full `engine.cpp` implementation is not wired in

---

## 2. Architecture Overview

```
main.cpp (STUB engine)
    ├── engine.cpp (Full 13-step orchestrator — NOT WIRED TO main.cpp)
    │   ├── Step 1:  Market State     → market_data.cpp, dexie_client.cpp
    │   ├── Step 2:  Fill Processing  → offer_manager.cpp, inventory.cpp
    │   ├── Step 3:  Analytics        → volatility.cpp, adverse_selection.cpp
    │   ├── Step 4:  Quote Compute    → avellaneda.cpp / glft.cpp
    │   ├── Step 5:  Spread Optimize  → spread.cpp, chia_edge.cpp, order_book_tactics.cpp
    │   ├── Step 6:  Risk Limits      → limits.cpp, loss_manager.cpp
    │   ├── Step 7:  Tier Ladder      → liquidity.cpp
    │   ├── Step 8:  Offer Mgmt       → offer_manager.cpp, coin_manager.cpp
    │   ├── Step 9:  Arbitrage        → arbitrage.cpp
    │   ├── Step 10: Hedging          → hedging.cpp
    │   ├── Step 11: PnL              → pnl.cpp
    │   ├── Step 12: Metrics          → metrics.cpp
    │   └── Step 13: Alerts           → alerts.cpp
    │
    ├── state.cpp       (Thread-safe shared state)
    ├── database.cpp    (SQLite persistence)
    └── config.cpp      (YAML loader)
```

The architecture is sound in design but has significant gaps in implementation completeness and integration.

---

## 3. Critical Bugs — Must Fix Before Live Trading

### 3.1 `main.cpp` Engine Is a Stub

**File:** `src/main.cpp`  
**Severity:** 🔴 Critical  

The `Engine` class defined inline in `main.cpp` is a minimal stub that only runs a 52-second heartbeat timer. The fully implemented `Engine` class in `engine.cpp` with all 13 steps, subsystem wiring, and lifecycle management is **never instantiated**. The program compiles and runs but performs zero trading.

**Fix:** Replace the stub engine in `main.cpp` with an instantiation of the full `xop::Engine` class from `engine.hpp`.

---

### 3.2 Blocking CURL in Coroutines

**Files:** `src/rpc/chia_rpc.cpp`, `src/rpc/dexie_client.cpp`  
**Severity:** 🔴 Critical  

`curl_easy_perform()` is called synchronously inside `co_await` contexts, blocking the **entire** `io_context` thread. Since the engine is single-threaded by design, every RPC call freezes the event loop for the full duration of the HTTP roundtrip (potentially 30+ seconds on timeout).

Additionally, `dexie_client.cpp` uses `std::this_thread::sleep_for()` for rate limiting, which blocks the event loop thread.

**Fix:** Dispatch CURL operations to a dedicated thread pool using `boost::asio::post()`, or switch to an async HTTP library (Beast, or CURLM multi-handle with asio integration).

---

### 3.3 CURL Handle Not Thread-Safe

**File:** `src/rpc/chia_rpc.cpp`  
**Severity:** 🔴 Critical  

A single CURL handle is reused across all RPC requests with no synchronization. If any concurrent coroutines are ever co-spawned (e.g., the shutdown cancel-all path), the handle state will be corrupted.

**Fix:** Implement a per-request CURL handle pool, or protect the shared handle with a mutex.

---

### 3.4 Dangling `c_str()` Pointer in TLS Configuration

**File:** `src/rpc/chia_rpc.cpp`  
**Severity:** 🔴 Critical  

```cpp
curl_easy_setopt(curl_, CURLOPT_SSLCERT, config.cert_path.c_str());
```

If `config` is a temporary `std::string` or if the `config` object's lifetime ends before the CURL handle uses the pointer, libcurl will read from freed memory. This is a use-after-free vulnerability.

**Fix:** Store the TLS paths in member variables that outlive the CURL handle, or pass them via `CURLOPT_SSLCERT_BLOB`.

---

### 3.5 Coin Identity Tracking Is Broken

**File:** `src/execution/coin_manager.cpp`  
**Severity:** 🔴 Critical  

`compute_coin_name()` returns a concatenated hex string instead of a proper SHA-256 hash. Chia coin names are computed as `SHA256(parent_coin_info || puzzle_hash || amount)`. All coin identity, locking, and tracking is fundamentally broken.

```cpp
// CURRENT (broken):
return parent_hex + puzzle_hex + amount_hex;

// SHOULD BE:
return sha256(parent_bytes || puzzle_bytes || amount_bytes).hex();
```

**Fix:** Implement proper SHA-256 coin name computation using OpenSSL or a lightweight SHA-256 library.

---

### 3.6 Coin Splitting Never Executed

**File:** `src/execution/coin_manager.cpp`  
**Severity:** 🔴 Critical  

The `ensure_split()` method has the `send_transaction` RPC call **commented out**. Coins appear split in the internal tracking but are never actually split on-chain. The multi-tier offer ladder requires pre-split coins to create multiple concurrent offers.

**Fix:** Uncomment and implement the `send_transaction` RPC call, with proper error handling and confirmation.

---

### 3.7 Dexie Offer Submission Is a Stub

**File:** `src/execution/offer_manager.cpp`  
**Severity:** 🔴 Critical  

`submit_to_dexie()` always returns `true` without actually submitting the offer to the DEX. Offers are created in the wallet but never reach the dexie marketplace.

**Fix:** Implement the actual dexie API submission using `DexieClient::post_offer()`.

---

## 4. High-Severity Issues — Must Fix Before Paper Trading

### 4.1 Thread Safety Gaps Across 8+ Modules

**Severity:** 🟠 High  

The following modules have **zero thread synchronization** on mutable member state, yet are accessed from the engine's event loop (which may context-switch between coroutine continuations):

| Module | Mutable State At Risk |
|--------|----------------------|
| `volatility.cpp` | `candles_`, `sigma_block_`, `regime_info_` |
| `adverse_selection.cpp` | `history_`, `alpha_`, `beta_`, `pin_` |
| `new_strategies.cpp` | `coins_`, `price_buffer_`, `regime_`, `pending_takes_` |
| `order_book_tactics.cpp` | `own_offer_ids_` |
| `chia_edge.cpp` | `price_buffer_`, `regime_`, `cost_basis_` |
| `strategy_portfolio.cpp` | `components_`, `weights_` |
| `metrics.cpp` | `running_` flag (not atomic, not mutex-protected) |

While the engine is currently single-threaded, any future multi-threading (or even the coroutine interleaving during `co_spawn`) could cause data races.

**Fix:** Add `std::shared_mutex` or `std::mutex` protection consistent with `state.cpp`'s pattern. Or, annotate these classes as explicitly `// NOT THREAD-SAFE — must only be called from the engine loop` and enforce at the architecture level.

---

### 4.2 Detached Threads in Alert System

**File:** `src/monitoring/alerts.cpp`  
**Severity:** 🟠 High  

```cpp
std::thread([url, payload]() {
    // HTTP POST to Telegram
}).detach();
```

Detached threads have **no cleanup path**. On program exit, they may be terminated mid-HTTP-request, leaving partial state. Worse, if the bot shuts down while threads are in-flight accessing stack-captured variables, this is undefined behavior.

**Fix:** Use a persistent worker thread with a message queue, or an asio-based async HTTP client.

---

### 4.3 Alert Rate Limiting Is Per-Tier, Not Per-Rule

**File:** `src/monitoring/alerts.cpp`  
**Severity:** 🟠 High  

Rate limiting is applied per severity tier (Critical, Warning, Info), not per rule. A critical `NodeDesync` alert will suppress all subsequent critical alerts (including `ExposureBreach`) for the entire cooldown period. In a cascading failure scenario, this means the most important alerts are silently dropped.

**Fix:** Track `last_sent_` per rule ID (or per rule+pair combination).

---

### 4.4 OFI Signal Degrades Over Time

**File:** `src/execution/market_data.cpp`  
**Severity:** 🟠 High  

OFI normalization divides by the sum of all bid+ask sizes across the **entire** window. As more snapshots accumulate, the denominator grows without bound, driving normalized OFI toward zero regardless of actual order flow imbalance. This makes OFI useless after a few hundred blocks.

**Fix:** Normalize by the sum within a fixed sliding window, or use a rolling mean/variance normalization.

---

### 4.5 Yang-Zhang Volatility Estimator Bias

**File:** `src/data/volatility.cpp`  
**Severity:** 🟠 High  

`recompute_yang_zhang()` skips observations where the price is zero or negative (defensive guard), but **does not adjust the sample count `n`**. The denominator stays `n_candles - 1`, causing the variance to be systematically underestimated when any observations are skipped. The same bug exists in `recompute_variance_ratio()`.

**Impact:** Volatility is underestimated → spreads are too tight → increased adverse selection losses.

**Fix:** Track `n_valid` separately from the buffer size and use `n_valid - 1` as the denominator.

---

### 4.6 Tax CSV Reports Wrong Acquisition Dates

**File:** `src/monitoring/pnl.cpp`  
**Severity:** 🟠 High  

Both `Date Acquired` and `Date Sold` in the IRS Form 8949 CSV export are set to the sell date. The actual acquisition date should come from the buy-side fill record.

**Fix:** Look up the original buy fill(s) that established the cost basis, or at minimum store the average acquisition timestamp in the `AssetRecord`.

---

### 4.7 `const_cast` on SQLite Prepared Statements

**File:** `src/monitoring/pnl.cpp`  
**Severity:** 🟠 High  

`get_trade_log()` and `query_trades()` are `const` methods but use `const_cast` to call `sqlite3_bind_*` and `sqlite3_step` on prepared statements. If two const accessors run concurrently, they will corrupt the same `sqlite3_stmt` state (SQLite statements are not thread-safe).

**Fix:** Either remove the `const` qualifier, or use separate statement instances per query (one per thread).

---

### 4.8 `curl_global_init()` Called Per Client Instance

**File:** `src/rpc/dexie_client.cpp`  
**Severity:** 🟠 High  

`curl_global_init(CURL_GLOBAL_DEFAULT)` is called in every `DexieClient::open()` invocation. The libcurl documentation states this function is **not thread-safe** and must be called **exactly once** per process before any other curl function.

**Fix:** Call `curl_global_init()` once in `main()` before constructing any clients, and `curl_global_cleanup()` at exit.

---

## 5. Logical Errors in Strategy Layer

### 5.1 Four Independent, Inconsistent Variance Ratio Implementations

**Files:** `volatility.cpp`, `avellaneda.cpp`/`glft.cpp`, `new_strategies.cpp`, `chia_edge.cpp`, `regime.cpp`

The codebase contains **four separate implementations** of the variance ratio test:

| Location | k-period | Variance (N or N-1) | Used For |
|----------|----------|---------------------|----------|
| `volatility.cpp` | 5 | N (population) | Regime detection for vol estimator |
| `avellaneda.cpp` / `glft.cpp` | 5 | N-1 (sample) | Strategy-internal regime |
| `new_strategies.cpp` | 5 | N-1 (sample) | CoinAge/BlockCadence regime |
| `chia_edge.cpp` | 5 | N-1 (sample) | Edge multiplier regime |
| `regime.cpp` | 5 + 20 (dual) | N-1 (sample) | Standalone detector with HMM |

The population (N) vs. sample (N-1) denominator inconsistency means the same price data produces different VR values depending on which module processes it. With a window of ~50 observations, the difference in VR at the regime boundaries (0.85/1.15) is ~2%, which can flip a regime classification.

**Fix:** Consolidate into a single `RegimeDetector` class (the one in `regime.cpp` is the most sophisticated) and share it across all consumers.

---

### 5.2 Monte Carlo Flow RNG Uses Fixed Seed

**File:** `src/backtest.cpp`  
**Severity:** 🟡 Medium  

The Monte Carlo simulator uses a **fixed seed** (`12345u`) for the order-flow random number generator in `build_synthetic_blocks()`. This means all N Monte Carlo paths share the **same order-flow pattern**; only the price path varies. This defeats the purpose of Monte Carlo simulation, which should test robustness across diverse market microstructure scenarios.

**Fix:** Seed the flow RNG uniquely per path (e.g., `base_seed + path_index`).

---

### 5.3 Walk-Forward Test Window Doesn't Actually Slide

**File:** `src/backtest.cpp`  
**Severity:** 🟡 Medium  

In `walk_forward_optimize()`, the `window_end` is always set to `total_blocks` regardless of the `advance_blocks` parameter. The train window slides forward, but the test window always extends to the end of the data. This means later windows have progressively larger test sets, making their Sharpe/drawdown metrics incomparable to earlier windows.

**Fix:** Set `window_end = window_start + train_blocks + test_blocks`, clamped to `total_blocks`.

---

### 5.4 Drift Analyzer Monte Carlo Starts From Balanced Inventory

**File:** `src/risk/drift_analyzer.cpp`  
**Severity:** 🟡 Medium  

`simulate_drift()` always starts from `q = 0` (balanced inventory) regardless of the `current_ratio` parameter passed to `analyze_drift()`. The Monte Carlo paths simulate from a hypothetical balanced state, not from the actual imbalanced position. This biases the expected time-to-recovery and the risk estimates.

**Fix:** Initialize the simulation starting inventory from `current_ratio`, not from 0.0.

---

### 5.5 Offer Manager Quote-Asset Denomination Assumption

**File:** `src/execution/offer_manager.cpp`  
**Severity:** 🟡 Medium  

`build_offer_dict()` divides the quote amount by `kMojosPerXch` (10^12). This assumes the quote asset always uses XCH's mojo denomination. CAT tokens on Chia have a different denomination (typically 10^3 mojos per CAT unit). Using the wrong conversion factor will produce offers with wildly incorrect sizes.

**Fix:** Look up the denomination for each asset from the pair config or a token metadata registry.

---

### 5.6 Crossed Book Data Ingested Without Validation

**File:** `src/execution/market_data.cpp`  
**Severity:** 🟡 Medium  

`ingest_dexie()` does not validate that `best_bid < best_ask`. A crossed book (bid > ask) could be ingested from a dexie API error or a race condition, causing the mid-price calculation to produce nonsensical values and potentially triggering spurious arbitrage signals.

**Fix:** Add a guard: `if (best_bid >= best_ask) { log_warn; return; }`.

---

### 5.7 `strategy_portfolio.cpp` Component Auto-Creation

**File:** `src/strategy/strategy_portfolio.cpp`  
**Severity:** 🟡 Medium  

`record_pnl()` uses `components_[component]` which **default-constructs** a new `ComponentState` via `operator[]` if the component doesn't exist. This bypasses the uniform-weight initialization logic, creating a component with zero weight and zero history. Subsequent weight-rebalancing passes may produce unexpected distributions.

**Fix:** Use `components_.find()` and reject unknown components, or ensure all components are pre-registered.

---

## 6. Risk Management Gaps

### 6.1 PreTradeCheck Is Completely Untested

**Severity:** 🔴 Critical Risk  

`PreTradeCheck` in `limits.cpp` is the system's **last line of defense** before real money is deployed. It implements:
- `enforce_no_loss()` — the CORE RULE of the system
- `apply_limits()` — inventory limits, CAT cap, capital-per-pair
- `check_flash_crash()` — circuit breaker
- `is_stable_after_crash()` — recovery gate

**None of these functions have any test coverage.** The inventory tests exercise cost-basis tracking (a prerequisite), but the actual risk gates themselves are untested.

---

### 6.2 Soft Limit Behaves Identically to Hard Limit

**File:** `src/risk/limits.cpp`  
**Severity:** 🟡 Medium  

In `apply_limits()`, both the soft limit and hard limit branches do the same thing: set `bid_size = 0` (or `ask_size = 0`). The strategy document describes the soft limit as "begin aggressive quote skewing" and the hard limit as "pull quotes entirely." The current implementation pulls quotes at both thresholds, providing no graduated response.

```cpp
// Base overweight?
if (base_conc >= risk_cfg_.hard_limit_pct) {
    quote.bid_size = 0;  // Pull bids
} else if (base_conc >= risk_cfg_.soft_limit_pct) {
    quote.bid_size = 0;  // Also pulls bids — same as hard limit!
}
```

**Fix:** At soft limit, reduce `bid_size` proportionally (e.g., `bid_size *= (1 - (base_conc - soft_pct) / (hard_pct - soft_pct))`) instead of zeroing it.

---

### 6.3 Strategic Loss Manager Is Queried But Never Acts

**File:** `src/engine.cpp` (Step 6)  
**Severity:** 🟡 Medium  

The Strategic Loss Manager is constructed and consulted when inventory ratio exceeds 60%, but the engine never uses its recommendation. The code just logs that it's consulting the manager, then continues with the standard no-loss enforcement:

```cpp
if (loss_manager_ && loss_manager_->config().enabled) {
    if (inv_ratio > 0.60) {
        spdlog::debug("[step6] {} inv_ratio={:.2f} — consulting loss manager",
            pair_name, inv_ratio);
        // ... no action taken based on the result
    }
}
```

**Fix:** Call `loss_manager_->evaluate()` and, if the recommendation is to accept a loss, bypass the `enforce_no_loss()` gate for this cycle with proper logging and alerting.

---

### 6.4 Hedging Layer Is Mostly Placeholder

**File:** `src/engine.cpp` (Step 10)  
**Severity:** 🟡 Medium  

Step 10 (Run Hedging) computes Layer 3 exposure and logs the number of positions, but:
- Layer 2 (NHE) is computed nowhere — the `compute_nhe()` function exists but is never called
- Layer 4 (pairs hedging) mentions "Phase 2" empty target map
- No rebalancing trades are ever suggested or executed

**Fix:** Wire `compute_nhe()` into the heartbeat, compute it from PnL fill history, and add alerting when NHE drops below 0.70.

---

### 6.5 No Stop-Loss or Max-Loss Circuit Breaker

**Severity:** 🟡 Medium  

While the system has a never-sell-at-loss constraint and flash-crash detection, there is **no global stop-loss or max-loss threshold** that halts trading when cumulative losses exceed a configured limit. A slow series of adverse fills (each individually profitable but cumulatively value-destroying due to inventory buildup) could deplete capital without triggering any alert.

**Fix:** Add a global `max_drawdown_pct` parameter that transitions the engine to `Paused` status when the high-water-mark drawdown exceeds the threshold.

---

### 6.6 No Recovery/Restart Strategy After Crash Detection

**Severity:** 🟡 Medium  

`check_flash_crash()` detects a crash and `is_stable_after_crash()` detects recovery, but there is no state machine integrating the two. The engine never calls `check_flash_crash()` during the heartbeat — it's only available as a static method, requiring manual integration.

**Fix:** Add a `flash_crash_state_` member to the engine (or `PreTradeCheck`) that tracks: Normal → CrashDetected → WaitingForRecovery → Normal, and gates Step 8 (offer management) during the crash/recovery phases.

---

## 7. Timing and Ordering Issues

### 7.1 Fill Processing Before Analytics Update Creates Stale Signals

**File:** `src/engine.cpp`  

The 13-step heartbeat processes fills (Step 2) **before** updating analytics (Step 3). This means:
- VPIN and OFI are computed from **current-block fills** but with **previous-block volatility and regime**
- Whale detection is triggered by Step 2 fills, but the spread multiplier isn't applied until Step 5

For the first block after a whale trade, the system responds with stale analytics. This is a deliberate design choice (fills are time-critical for cost-basis), but the timing gap should be documented and the spread multiplier should carry forward if the whale detection fires mid-cycle.

---

### 7.2 `co_spawn` with `use_future` Blocks the Event Loop

**Files:** `src/engine.cpp` (Steps 2, 8, shutdown)  
**Severity:** 🟡 Medium  

The engine uses `co_spawn(ioc_, ..., use_future)` followed by `future.get()` to synchronously wait for coroutine results. Since the engine runs on a **single** `io_context` thread, `future.get()` cannot complete because the coroutine needs the same `io_context` thread to run. This is a **deadlock** in a single-threaded context.

The code may work if `ioc_.run()` has already been called from another thread, or if the coroutines are using a separate executor, but the architecture states "single io_context thread." This needs verification.

**Fix:** Use `co_await` instead of `use_future` + `future.get()`, or run the io_context on multiple threads.

---

### 7.3 Market Data Lock Contention During Refresh

**File:** `src/execution/market_data.cpp`  

`refresh()` holds `mtx_pairs_` for the entire duration of its execution, including calls to `check_arbitrage()` and `append_price_history()`, which acquire their own locks. This creates:
1. Long critical sections that block all other market data access during refresh
2. A nested lock ordering dependency (`mtx_pairs_` → `mtx_arb_` / `mtx_history_`)

While the lock ordering appears consistent (documented in comments), the long hold time means market data queries from monitoring or alerting subsystems will be blocked for the entire refresh cycle.

**Fix:** Compute aggregated data under the lock, release the lock, then dispatch signals (arb, history) without holding `mtx_pairs_`.

---

### 7.4 Offer Posting Before Fill Recording in Database

**File:** `src/engine.cpp` (Step 8)  

New offers are posted (Step 8) and inserted into the `offer_log` database table using a **synthetic placeholder offer_id** constructed from `pair_name + block_height + tier + side`. The actual offer_id (from the wallet RPC response) is assigned later by `OfferManager`, but the database record already has the wrong ID.

This means:
- `update_offer_status()` will fail to find the offer by its real ID (throws `std::runtime_error`)
- The audit trail has placeholder IDs that don't match on-chain data

**Fix:** Insert the offer record into the database **after** `OfferManager::post_quotes()` returns the actual offer IDs.

---

### 7.5 PnL Snapshot Uses Same Block's PnL for All Pairs

**File:** `src/engine.cpp` (Step 11)  

`pnl_->get_total_pnl()` returns aggregate PnL across all pairs, but this single value is stored in every per-pair snapshot. This means you cannot determine per-pair PnL contribution from the snapshot table.

**Fix:** Compute per-pair PnL in `PnLTracker` and store the pair-specific value in each snapshot.

---

## 8. Missing Logical Strategies

### 8.1 No Stale-Quote Detection for Own Offers

The system cancels offers based on TTL (block age), but does not check whether its own outstanding offers are **stale relative to the current mid-price**. An offer posted at mid=100 that is still active after mid moves to 110 is 1000 bps away from the new fair value. Waiting for TTL expiry to cancel it exposes the system to adverse selection.

**Recommendation:** Add a price-deviation trigger to `should_rebalance()` that computes the distance between each pending offer's price and the current mid. Cancel immediately if the deviation exceeds the outer tier spacing.

---

### 8.2 No Fill-Rate Feedback Loop

The `fill_rate_24h` in `BookState` is hardcoded to `0.30`, and the actual fill rate is never computed from historical data. Fill rate is a critical input to:
- Kelly sizing (`compute_kelly_size`)
- Loss manager break-even calculations
- Spread optimization (Thompson Sampler feedback)

Without accurate fill rate data, these subsystems operate on guesses.

**Recommendation:** Compute rolling fill rate from the database trade log: `fills_in_last_N_blocks / N`.

---

### 8.3 No Partial Inventory Recovery Strategy

The system's no-loss constraint means underwater inventory is held indefinitely. There is no mechanism for dollar-cost-averaging down, hedging the exposure via correlated assets, or implementing time-based spreading (selling small portions at progressively lower loss thresholds as the position ages).

**Recommendation:** Implement an aging schedule: after N blocks underwater, gradually relax the no-loss floor (e.g., accept 10bps loss after 1000 blocks, 20bps after 2000 blocks) to prevent inventory from becoming permanently stuck.

---

### 8.4 No Reorg/Fork Protection

Chia can experience chain reorganizations. The system has no mechanism to:
- Detect that a fill was reverted by a reorg
- Roll back the cost-basis change from a reverted fill
- Mark offers as uncertain during a reorg

**Recommendation:** Add a "confirmation depth" parameter (e.g., 6 blocks) and only record fills that are behind the current height by at least this depth.

---

### 8.5 No CEX Reference Price Integration

The CexCandle struct and `cex_mid` field exist throughout the codebase, but no CEX data source is actually wired in. The system operates blind to off-chain price information, unable to detect:
- DEX prices lagging CEX by 500+ bps
- Arbitrage opportunities
- Informed flow from CEX-to-DEX latency arbitrageurs

CEX reference price is marked "Phase 2" throughout, but it's essential for production operation.

---

### 8.6 No Fee Budget Tracking

The system includes blockchain fee constants but does not track cumulative fee expenditure. On Chia, farmers can include fees in mempool priority; during congestion periods, the fee per transaction can spike. Without fee budget tracking, the system could spend more on fees than it earns in spread.

**Recommendation:** Add a `fee_budget_per_day` config parameter and track cumulative daily fees. Warn and reduce quoting frequency when approaching the budget.

---

### 8.7 No Graceful Degradation for API Failures

Step 1 fetches data from dexie for each pair. If the dexie API is down or rate-limited, the system logs a warning and continues with stale data from the previous cycle. However:
- There is no staleness counter — the system never escalates from "stale" to "dangerously stale"
- There is no distinction between "1 block stale" and "100 blocks stale"
- Offers posted with seriously stale data could be adversely selected

**Recommendation:** Track `blocks_since_last_update` per pair and widen spreads proportionally, pulling quotes entirely after a configurable max-staleness threshold.

---

## 9. Code Quality and Cleanup Opportunities

### 9.1 Duplicated Pair-Config Lookup Pattern

Throughout `engine.cpp`, this pattern appears **6 times**:

```cpp
const PairConfig* pair_cfg = nullptr;
for (const auto& pc : config_.pairs) {
    if (pc.name == pair_name) { pair_cfg = &pc; break; }
}
```

**Fix:** Pre-build an `std::unordered_map<std::string, const PairConfig*> pair_config_map_` in the constructor and use it everywhere.

---

### 9.2 Duplicated Variance Ratio Test (4× Implementations)

Consolidate the 4 independent VR implementations into `regime.cpp`'s dual-horizon version and share via a utility function or the `RegimeDetector` class.

---

### 9.3 `assert()` Used for Runtime Config Validation

**Files:** `strategy_portfolio.cpp`, `chia_edge.cpp`, `limits.cpp`

`assert()` is disabled in release builds (`-DNDEBUG`). Config validation that uses assert will silently accept invalid parameters in production.

**Fix:** Replace `assert()` with `if (...) throw std::invalid_argument(...)` for any input that comes from configuration.

---

### 9.4 `pnl.cpp` Uses `std::vector::erase(begin())` for Trimming

`pnl_history_` is a `std::vector<double>` trimmed by erasing the front element — an O(n) operation on every trim. For a history of 10,000+ entries, this is significant.

**Fix:** Use `std::deque<double>` which has O(1) front removal.

---

### 9.5 `MempoolSentinelStrategy` Uses `system_clock` for Staleness

**File:** `src/strategy/new_strategies.cpp`

`system_clock` is susceptible to system clock adjustments (NTP correction, manual changes). For staleness measurements, `steady_clock` should be used.

---

### 9.6 Hardcoded Magic Numbers

Several hardcoded values should be moved to configuration:

| Location | Value | What It Is |
|----------|-------|-----------|
| `engine.cpp` Step 5 | `0.5` | VPIN → spread multiplier scale factor |
| `engine.cpp` Step 5 | `0.3` | OFI → spread multiplier scale factor |
| `engine.cpp` Step 5 | `0.30` | Hardcoded `fill_rate_24h` |
| `engine.cpp` Step 11 | `2.70` | XCH/USD rate |
| `order_book_tactics.cpp` | `0.4` | Inventory ratio asymmetric sizing threshold |
| `drift_analyzer.cpp` | `0.003` | Hardcoded half-spread for simulation |
| `strategy_portfolio.cpp` | `4.0` | `kNumResidual` for weight distribution |

---

### 9.7 Move Assignment Operator Lock Ordering Risk

**File:** `src/execution/market_data.cpp`

The move assignment operator acquires two locks from two different `MarketDataFeed` objects without a global ordering guarantee. If two objects are simultaneously move-assigned in opposite directions, this is a classic ABBA deadlock.

**Fix:** Use `std::scoped_lock(mtx_a, mtx_b)` which handles deadlock-free acquisition via `std::lock()`.

---

### 9.8 SSL Verification Completely Disabled

**File:** `src/rpc/chia_rpc.cpp`

```cpp
curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
```

While this is intentional for self-signed certificates (Chia's default), it means **zero server authentication**. A MITM attacker could intercept wallet RPC calls and redirect funds.

**Fix:** Load the Chia CA certificate and verify against it: `CURLOPT_CAINFO = "~/.chia/mainnet/config/ssl/ca/chia_ca.crt"`.

---

## 10. Test Coverage Analysis

### Coverage Summary

| Category | Modules | Tested | Coverage |
|----------|---------|--------|----------|
| Strategy | 8 | 3 (A-S, Spread, Regime via A-S) | **37%** |
| Risk | 5 | 1 (Inventory) | **20%** |
| Execution | 3 | 0 | **0%** |
| Data/Analytics | 2 | 1 (Volatility) | **50%** |
| Market Data | 1 | 1 (via advanced_trading tests) | **100%** |
| Core | 5 (Engine, State, DB, Config, Backtest) | 0 | **0%** |
| Monitoring | 3 (PnL, Metrics, Alerts) | 0 | **0%** |
| RPC | 2 (Chia, Dexie) | 0 | **0%** |
| **Total** | **29** | **6** | **~21%** |

### Critical Test Gaps

1. **`PreTradeCheck` (limits.cpp)** — The CORE RULE of the system is untested
2. **GLFT Strategy** — A primary trading strategy with zero tests
3. **Config parsing** — No tests that the YAML loader correctly rejects invalid input
4. **State thread safety** — No multi-threaded stress tests
5. **LiquidityEngine** — The multi-tier ladder generation is untested
6. **Database** — No tests for crash recovery, concurrent reads, or constraint violations

---

## 11. Build System and Configuration

### Build Issues

| Issue | Severity |
|-------|----------|
| vcpkg baseline `2024.09.30` is 18 months stale — potential security patches missing | Medium |
| `prometheus-cpp` is `REQUIRED` with no FetchContent fallback — CI without vcpkg will fail | Medium |
| `_FORTIFY_SOURCE=2` set unconditionally but requires `-O1` to take effect — debug builds get no protection | Low |
| MSVC `/await:strict` flag is not needed on MSVC 19.34+ and may emit deprecation warnings | Low |
| No integration test target in CMake — only unit tests | Medium |

### Config Gaps

| Gap | Detail |
|-----|--------|
| `config.example.yaml` has truncated 32-char asset IDs with "replace" comments | Should fail validation |
| VPIN, OFI, whale, competitor parameters not exposed in YAML | Only code-configurable |
| Volatility regime multipliers not in YAML | Silently uses compiled defaults |
| `wallet_fingerprint: 0` accepted without rejection | Sentinel value should be caught |
| No documentation of which config fields are required vs. optional | Source of operator confusion |

---

## 12. Security Concerns

| Finding | Severity | Detail |
|---------|----------|--------|
| SSL verification disabled for all RPC | High | MITM on wallet RPC could redirect funds |
| No HTML escaping in Telegram messages | Medium | Injection risk in Telegram Bot API |
| No parameterised query for `PRAGMA` calls | Low | Not injection-vulnerable, but inconsistent with the stated policy |
| Secrets stored as plaintext in YAML | Medium | SSL paths, Telegram tokens in clear text. Should support env-var substitution |
| No API key rotation mechanism | Low | Compromised Telegram token requires manual config update |

---

## 13. Performance Considerations

| Finding | Impact |
|---------|--------|
| `backtest.cpp` `build_blocks()` scans all offers per block — O(blocks × offers) | Long backtest times |
| `adverse_selection.cpp` recomputes entire posterior on every fill — O(n²) | Degrades over time |
| `pnl.cpp` builds summary under mutex with O(n) Sharpe/drawdown — blocks readers | Metrics latency |
| `market_data.cpp` holds locks during `refresh()` — blocks concurrent readers | Market data latency |
| `metrics.cpp` calls `family->Add()` every heartbeat instead of caching gauge pointers | Unnecessary allocation |
| Monte Carlo in `drift_analyzer.cpp` is single-threaded despite being embarrassingly parallel | Slow analysis |
| `get_ticker()` in `dexie_client.cpp` fetches ALL tickers and filters client-side | Excessive bandwidth |

---

## 14. Recommendations and Prioritized Action Items

### Tier 1 — Blockers for Any Real Trading (Must Fix)

| # | Item | Est. Effort |
|---|------|-------------|
| 1 | Wire full `Engine` into `main.cpp` (replace stub) | 1 day |
| 2 | Replace blocking CURL with async HTTP (thread pool or Beast) | 3-5 days |
| 3 | Implement proper SHA-256 coin name computation | 0.5 day |
| 4 | Activate coin splitting RPC call | 1 day |
| 5 | Implement dexie offer submission | 1 day |
| 6 | Fix dangling `c_str()` in TLS config | 0.5 hour |
| 7 | Fix `co_spawn` + `use_future` deadlock risk | 2-3 days |

### Tier 2 — Required for Paper Trading

| # | Item | Est. Effort |
|---|------|-------------|
| 8 | Add thread safety to volatility, adverse_selection, strategies | 2-3 days |
| 9 | Replace detached threads in alerts with worker queue | 1 day |
| 10 | Fix OFI normalization (sliding window) | 0.5 day |
| 11 | Fix Yang-Zhang variance bias (n_valid tracking) | 0.5 day |
| 12 | Fix tax CSV acquisition dates | 0.5 day |
| 13 | Make SSL verification configurable (default ON for Chia CA) | 1 day |
| 14 | Implement graduated soft-limit response (proportional sizing) | 0.5 day |
| 15 | Fix offer_id placeholder in database | 0.5 day |
| 16 | Add `curl_global_init()` once in main | 0.5 hour |

### Tier 3 — Quality and Robustness

| # | Item | Est. Effort |
|---|------|-------------|
| 17 | Consolidate 4 VR implementations into shared RegimeDetector | 2 days |
| 18 | Add PreTradeCheck tests (enforce_no_loss, flash_crash, limits) | 2 days |
| 19 | Add GLFT strategy tests | 1 day |
| 20 | Add config parsing tests | 1 day |
| 21 | Implement fill-rate feedback loop from database | 1 day |
| 22 | Add stale-data escalation (widen spread → pull quotes) | 1 day |
| 23 | Wire Strategic Loss Manager decisions into Step 6 | 1 day |
| 24 | Wire NHE computation into Step 10 | 0.5 day |
| 25 | Add reorg/confirmation-depth protection | 2 days |
| 26 | Pre-build pair_config_map to eliminate linear lookups | 0.5 hour |
| 27 | Replace `assert()` with runtime `throw` for config validation | 0.5 day |
| 28 | Add max-drawdown global circuit breaker | 1 day |
| 29 | Walk-forward window fix (test window should slide) | 0.5 day |
| 30 | Monte Carlo flow RNG per-path seeding | 0.5 hour |

### Tier 4 — Strategic Enhancements

| # | Item | Est. Effort |
|---|------|-------------|
| 31 | CEX reference price integration (OKX/Gate.io) | 3-5 days |
| 32 | Stale-quote detection for own offers | 1 day |
| 33 | Inventory aging/partial-recovery schedule | 2 days |
| 34 | Fee budget tracking and alerting | 1 day |
| 35 | Graceful API degradation with staleness escalation | 1 day |
| 36 | Environment variable substitution in YAML config | 1 day |
| 37 | Expose VPIN/OFI/whale/competitor config in YAML | 1 day |
| 38 | Integration test framework (backtest as CI test) | 2 days |

---

## Appendix: File-by-File Issue Count

| File | Critical | High | Medium | Low |
|------|----------|------|--------|-----|
| `main.cpp` | 1 (stub engine) | — | — | — |
| `engine.cpp` | — | 1 (co_spawn deadlock) | 4 (timing, placeholder IDs, hardcoded values, PnL per-pair) | 1 (pair lookup) |
| `chia_rpc.cpp` | 2 (blocking curl, dangling c_str) | 1 (handle thread safety) | 1 (SSL verification) | — |
| `dexie_client.cpp` | 1 (blocking sleep) | 1 (curl_global_init) | 1 (const_cast) | — |
| `coin_manager.cpp` | 2 (SHA-256, splitting) | — | — | — |
| `offer_manager.cpp` | 1 (submission stub) | — | 1 (denomination) | — |
| `market_data.cpp` | — | 1 (OFI degradation) | 2 (crossed book, lock contention) | — |
| `volatility.cpp` | — | 1 (variance bias) | — | 1 (thread safety) |
| `adverse_selection.cpp` | — | — | 1 (O(n²)) | 1 (thread safety) |
| `alerts.cpp` | — | 2 (detached threads, rate limiting) | 1 (HTML injection) | — |
| `metrics.cpp` | — | — | 1 (running_ flag) | 1 (cache gauges) |
| `pnl.cpp` | — | 2 (tax dates, const_cast) | 1 (vector erase) | — |
| `limits.cpp` | — | — | 1 (soft=hard) | — |
| `loss_manager.cpp` | — | — | 1 (never acts) | — |
| `hedging.cpp` | — | — | 1 (NHE never called) | — |
| `drift_analyzer.cpp` | — | — | 2 (MC start, hardcoded spread) | — |
| `new_strategies.cpp` | — | — | 2 (VR inconsistency, system_clock) | 1 (thread safety) |
| `order_book_tactics.cpp` | — | — | 1 (hardcoded threshold) | 1 (thread safety) |
| `strategy_portfolio.cpp` | — | — | 2 (auto-create, assert) | — |
| `chia_edge.cpp` | — | — | 1 (4th VR impl) | 1 (assert) |
| `backtest.cpp` | — | — | 3 (MC seed, walk-forward, build_blocks O(n²)) | — |
| `avellaneda.cpp` | — | — | — | — |
| `glft.cpp` | — | — | — | — |
| `spread.cpp` | — | — | — | — |
| `liquidity.cpp` | — | — | — | — |
| `arbitrage.cpp` | — | — | — | — |
| `state.cpp` | — | — | — | — |
| `config.cpp` | — | — | — | — |
| `database.cpp` | — | — | — | — |
| **TOTALS** | **7** | **9** | **25** | **8** |

---

*Review completed March 24, 2026 by GitHub Copilot (Claude Opus 4.6). This document should be maintained as a living checklist — mark items as resolved with commit hashes as fixes are landed.*
