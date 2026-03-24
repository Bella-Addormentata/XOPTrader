# XOPTrader Comprehensive Code Review -- Pass 1

**Date:** 2026-03-24
**Reviewer:** Claude Code (Opus 4.6) -- Direct codebase access
**Method:** Full static review of all 30+ source files via 5 parallel analysis agents
**Scope:** Engine core, all strategy modules, risk/execution, data/monitoring/RPC, tests/build

## Original Prompt

> Perform and full and thorough comprehensive code review of the git project. Look for any and all errors or pitfalls and missing logical strategies. Any ways to clean up the code and timing. Place the review into the code review folder when complete.

---

## Executive Summary

The codebase has strong module design and extensive strategy research, but contains **4 critical**, **14 high**, and **30+ medium** severity issues. The most impactful findings are:

1. **Unit mismatch in new strategies** -- CoinAge/BlockCadence/Mempool strategies use `sigma_block^2 * tau_years`, producing inventory penalties ~6 orders of magnitude too small.
2. **Realized PnL in mojos-squared** -- `(price_mojos - cost_mojos) * size_mojos` produces wrong units without normalization.
3. **Missing `regime.cpp` in CMakeLists.txt** -- Potential linker errors for regime detection.
4. **Loss manager receives half-spread, not full spread** -- EV scenarios underestimate revenue by 50%.

---

## Severity Legend

- **P0 (Critical):** Produces incorrect results, crashes, or wrong money at risk.
- **P1 (High):** Significant logic error, safety gap, or silent data corruption.
- **P2 (Medium):** Correctness under edge cases, performance, maintainability.
- **P3 (Low):** Style, robustness, documentation.

---

## P0 -- Critical Findings

### C1. Unit mismatch: new strategies use sigma_block^2 * tau_years

**File:** `cpp/src/strategy/new_strategies.cpp:186-312`
**Impact:** CoinAgeWeightedQuoting, BlockCadenceAdaptiveSpread, and MempoolSentinelStrategy all call `annual_to_per_block_vol(sigma)` to get `sigma_block`, then `rolling_tau_years()` to get `tau` in years. The product `sigma_block^2 * tau_years` is ~6 orders of magnitude smaller than intended. These strategies produce almost zero inventory skew and negligible risk premium.
**Fix:** Pass `sigma_annual` (not `sigma_block`) with `tau_years` to `as_reservation_price()` and `as_half_spread()`.

### C2. Realized PnL formula produces mojos-squared

**File:** `cpp/src/engine.cpp:628-630`
**Impact:** `tr.realized_pnl_mojos = (fill.price - cost_basis) * fill.size` where all values are `Mojo` (int64). The product is in mojos^2, not mojos. The audit trail, PnL reporting, and all downstream monitoring are wrong. Also risks int64 overflow for large positions.
**Fix:** Normalize by dividing by `kMojosPerXch`, or use `__int128` intermediate: `pnl = (price - basis) * size / kMojosPerXch`.

### C3. Missing regime.cpp in CMakeLists.txt

**File:** `cpp/CMakeLists.txt:91-135`
**Impact:** `src/strategy/regime.cpp` exists on disk but is not in the `xop_core` source list. If it contains non-inline definitions, the build produces linker errors. The HMM Baum-Welch implementation lives there.
**Fix:** Add `src/strategy/regime.cpp` to the `add_library(xop_core ...)` source list.

### C4. Loss manager receives half-spread, not full spread

**File:** `cpp/src/engine.cpp:1003`
**Impact:** `mkt_params.spread_bps = pcs.spread_result.half_spread` passes the half-spread where the loss manager's 5 EV scenarios expect the full spread. Revenue estimates are off by 50%.
**Fix:** Pass `pcs.spread_result.total_spread_bps` instead.

---

## P1 -- High Findings

### H1. Step 5 processes uninitialized quotes when step 4 skipped a pair

**File:** `cpp/src/engine.cpp:786-917`
**Impact:** When step 4 sets `quote_valid = false` (stale data or zero mid), step 5 still processes the pair. `pcs.raw_quote` contains default (zero/garbage) values. The spread optimizer produces corrupt spread data that flows into steps 6-8.
**Fix:** Add `if (!pcs.quote_valid) continue;` at the top of the step 5 loop.

### H2. Step 6 also lacks quote_valid guard

**File:** `cpp/src/engine.cpp:923`
**Fix:** Add `if (!pcs.quote_valid) continue;` at the top of the step 6 loop.

### H3. Missing null check on fill_pair_cfg in step 2

**File:** `cpp/src/engine.cpp:616-630`
**Impact:** If `find_pair_config()` returns nullptr (unconfigured pair), `get_record()` receives empty AssetId, producing incorrect cost basis and realized PnL.
**Fix:** Add `if (!fill_pair_cfg) { spdlog::error(...); continue; }` before the get_record call.

### H4. Stale regime default: spread_mult=0 on first call

**File:** `cpp/src/strategy/avellaneda.cpp`, `glft.cpp`, `chia_edge.cpp` (constructors)
**Impact:** `RegimeInfo{}` defaults to `spread_mult = 0.0` and `skew_mult = 0.0`. Before the first `update_price()`, `compute_quotes()` produces zero-spread quotes with no inventory management.
**Fix:** Initialize `regime_` to `RegimeInfo{MarketRegime::Random, 1.0, 1.0, 1.0}` in each constructor.

### H5. vol_estimators_ operator[] creates null entry on line 713

**File:** `cpp/src/engine.cpp:713`
**Impact:** `vol_estimators_[pair.name]->update(mid)` uses `operator[]` which inserts a null `unique_ptr` if the key is missing. Dereferencing null crashes.
**Fix:** Use `vol_it->second->update(mid)` (the iterator from the find() two lines above).

### H6. Alert system: peak_pnl always equals total_pnl

**File:** `cpp/src/engine.cpp:1384`
**Impact:** `peak_pnl = total_pnl` means drawdown is always zero. The drawdown alert (rule 4) never fires, making the operator blind to losses.
**Fix:** Track a running high-water mark: `static Mojo peak = 0; peak = std::max(peak, total_pnl);`.

### H7. Database NULL column dereference

**File:** `cpp/src/database.cpp:295-298`
**Impact:** `sqlite3_column_text()` returns nullptr for SQL NULL values. Passing nullptr to `std::string` constructor is undefined behavior (crash).
**Fix:** Add null guards: `const char* p = ...; rec.field = p ? p : "";`.

### H8. DexieClient calls curl_global_init inside open()

**File:** `cpp/src/rpc/dexie_client.cpp:158`
**Impact:** `curl_global_init` is NOT thread-safe and must be called exactly once. It's already called in `main.cpp`. Calling it again in `open()` races with Chia RPC's curl usage.
**Fix:** Remove the `curl_global_init` call from `DexieClient::open()`.

### H9. Hardcoded XCH/USD rate = 2.70

**File:** `cpp/src/engine.cpp:1244`
**Impact:** PnL mark-to-market uses a fixed $2.70 XCH price. All USD-denominated PnL, drawdown alerts, and risk monitoring are wrong when the actual price differs.
**Fix:** Source from config or CEX feed. At minimum, make it `config_.strategy.xch_usd_fallback_price`.

### H10. SpreadOptimizer: mutable state in const method

**File:** `cpp/include/xop/strategy/spread.hpp:323-327`, `cpp/src/strategy/spread.cpp:514-519`
**Impact:** `compute_spread()` is `const` but mutates `sampler_` (PRNG state) and `last_thompson_index_` via `mutable`. If called concurrently on a shared-const reference, the mt19937 state corrupts.
**Fix:** Remove `const` from `compute_spread()` to honestly reflect mutation.

### H11. Blocking CURL in coroutine stalls io_context

**File:** `cpp/src/rpc/chia_rpc.cpp:351`
**Impact:** `curl_easy_perform()` runs synchronously inside an `asio::awaitable`, blocking the entire event loop for up to 30 seconds per RPC call.
**Note:** Phase 2 architectural fix. Requires thread pool dispatch.

### H12. Signal handler calls non-async-signal-safe code

**File:** `cpp/src/main.cpp:76-93`
**Impact:** `engine->shutdown()` calls spdlog (mutex), poll_timer_.cancel() (Asio internals), co_spawn. Any of these can deadlock in a signal context.
**Note:** Phase 2 fix. Should set atomic flag and let the event loop check it.

### H13. build_offer_dict mojo division likely wrong

**File:** `cpp/src/execution/offer_manager.cpp:598-613`
**Impact:** `quote_amount = ceil(tier.size * tier.price / kMojosPerXch)` -- if tier.price is mojos-per-mojo, the division produces values 10^12 too small. Offers would have negligible counterpart amounts.
**Note:** Requires unit convention audit across the entire execution path.

### H14. compute_coin_name is placeholder (not SHA-256)

**File:** `cpp/src/execution/coin_manager.cpp:421-457`
**Impact:** Coin locking and double-spend prevention are non-functional. The computed name never matches wallet-returned coin names.
**Note:** Phase 2 fix. Requires OpenSSL SHA-256 integration.

---

## P2 -- Medium Findings

### M1. Step 4: `mid <= 0.0` continue without setting quote_valid = false
**File:** `cpp/src/engine.cpp:739`
**Fix:** Add `pcs.quote_valid = false;` before `continue`.

### M2. Hardcoded PIN default = 0.15
**File:** `cpp/src/engine.cpp:806`
**Fix:** Source from `config_.strategy.default_pin`.

### M3. Hardcoded fill_rate_per_block = 0.03
**File:** `cpp/src/engine.cpp:1002`
**Fix:** Compute from PnL tracker historical data.

### M4. Hardcoded fill_rate_24h = 0.30
**File:** `cpp/src/engine.cpp:902`
**Fix:** Derive from actual fill count.

### M5. Hardcoded normal_spread_bps = 100.0
**File:** `cpp/src/engine.cpp:1375`
**Fix:** Compute rolling average or source from config.

### M6. last_block_ updated even when critical steps fail
**File:** `cpp/src/engine.cpp:473`
**Fix:** Track step failures; don't advance if steps 1/2 failed.

### M7. Integer truncation in mojo conversions (systematic)
**File:** `cpp/src/engine.cpp` (multiple lines)
**Fix:** Use `std::llround()` for unbiased rounding; validate range before cast.

### M8. Offer database records use synthetic/placeholder IDs
**File:** `cpp/src/engine.cpp:1149-1154`
**Fix:** Persist actual wallet offer IDs from the posting response.

### M9. poll_timer_.cancel() not thread-safe in shutdown()
**File:** `cpp/src/engine.cpp:261`
**Fix:** Wrap in `asio::post(ioc_, ...)`.

### M10. PairCycleState members not default-initialized
**File:** `cpp/include/xop/engine.hpp:407-414`
**Fix:** Add default member initializers to QuoteResult/SpreadResult/Quote (all doubles to 0.0).

### M11. Member declaration order mismatch with initialization
**File:** `cpp/include/xop/engine.hpp:265,279`
**Fix:** Move `pair_config_map_` declaration after `config_`.

### M12. Strategy portfolio weight clamping violated after renormalization
**File:** `cpp/src/strategy/strategy_portfolio.cpp:574-586`
**Fix:** Run one more clamp iteration after final normalization.

### M13. Spread floor double-counted in competition component
**File:** `cpp/src/strategy/spread.cpp:469-470,525`
**Fix:** In `calc_competition_bps()`, return 0.0 when no competition data; let the final floor handle minimum.

### M14. Order book tactics: one-sided inventory check ignores skew direction
**File:** `cpp/src/strategy/order_book_tactics.cpp:185-209`
**Fix:** Include signed `inventory_q` in BookState to determine which side to enlarge.

### M15. HMM backward beta initialization non-standard
**File:** `cpp/src/strategy/regime.cpp:719-723`
**Fix:** Remove the division of `beta[T-1]` by `scale[T-1]`, or verify against Rabiner (1989).

### M16. Inventory get_risk_status nested shared lock
**File:** `cpp/src/risk/inventory.cpp:410-438`
**Fix:** Factor out `get_risk_status_locked()` that assumes lock is held.

### M17. no_loss_constraint_ data race (non-atomic, unlocked write)
**File:** `cpp/include/xop/risk/inventory.hpp:333`
**Fix:** Make `std::atomic<bool>`.

### M18. Concentration uses raw mojos, not mark-to-market
**File:** `cpp/src/risk/limits.cpp:406-416`
**Note:** Known limitation, documented in prior reviews.

### M19. soft_limit_pct > hard_limit_pct not validated
**File:** `cpp/src/risk/limits.cpp`
**Fix:** Add validation in PreTradeCheck constructor.

### M20. cancel_all clears state even when cancellations fail
**File:** `cpp/src/execution/offer_manager.cpp:374-379`
**Fix:** Only remove confirmed-cancelled offers.

### M21. loss_manager constructor noexcept but calls spdlog
**File:** `cpp/src/risk/loss_manager.cpp:237`
**Fix:** Remove `noexcept`.

### M22. loss_manager picks quote asset non-deterministically
**File:** `cpp/src/risk/loss_manager.cpp:524-530`
**Fix:** Accept explicit quote asset ID parameter.

### M23. drift_analyzer set_config races with analyze_drift on cfg_
**File:** `cpp/src/risk/drift_analyzer.cpp:645 vs 124-308`
**Fix:** Protect cfg_ reads in analyze_drift with shared lock.

### M24. PnL sqlite3_bind_int truncates BlockHeight
**File:** `cpp/src/monitoring/pnl.cpp:329,375`
**Fix:** Use `sqlite3_bind_int64` / `sqlite3_column_int64`.

### M25. PnL insert_trade not mutex-protected
**File:** `cpp/src/monitoring/pnl.cpp:304-343`
**Fix:** Acquire `mtx_` or document single-thread requirement.

### M26. Alert rate limiter per-tier, not per-rule
**File:** `cpp/src/monitoring/alerts.cpp:177-211`
**Fix:** Key rate limiter on AlertRule, not AlertTier.

### M27. Detached threads in Telegram sends (no join mechanism)
**File:** `cpp/src/monitoring/alerts.cpp:344-401`
**Fix:** Use bounded worker queue instead of detached threads.

### M28. Config: uint32 values parsed as int (truncation)
**File:** `cpp/src/config.cpp:131,145`
**Fix:** Parse as int64_t and range-check explicitly.

### M29. State: Position::add silently drops on overflow
**File:** `cpp/src/state.cpp:218-220`
**Fix:** Return `bool` or throw to notify caller.

### M30. SSL verification disabled with no localhost guard
**File:** `cpp/src/rpc/chia_rpc.cpp:263-264`
**Fix:** If host is not localhost/127.0.0.1/::1, enable verification.

### M31. Prometheus label cardinality unbounded
**File:** `cpp/src/monitoring/metrics.cpp:275-276`
**Fix:** Pre-register expected label sets from config.

### M32. CMakeLists.txt: missing -Wshadow, -Wconversion, -Wsign-conversion
**File:** `cpp/CMakeLists.txt:62`
**Fix:** Add these warning flags for ISO/IEC 5055 compliance.

### M33. CMakeLists.txt: _FORTIFY_SOURCE=2 ineffective in Debug
**File:** `cpp/CMakeLists.txt:64`
**Fix:** Guard with generator expression for non-Debug configs only.

### M34. CMakeLists.txt: missing PIE flags for executable
**File:** `cpp/CMakeLists.txt:66`
**Fix:** Add `-fPIE` (compile) and `-pie` (link) to the executable target.

### M35. CMakeLists.txt: MSVC missing /WX
**File:** `cpp/CMakeLists.txt:68`
**Fix:** Add `/WX` for warning-as-error parity with GCC/Clang.

### M36. MC flow_seed == price RNG seed (correlated randomness)
**File:** `cpp/src/backtest.cpp:833-834`
**Fix:** Use `seed + n_paths + path_idx` for flow seed.

---

## P3 -- Low Findings (17 items)

- Engine: redundant pair_cfg null check (1163), kPollInterval not configurable, parse_cli std::exit bypasses cleanup
- Strategy: enforce_spread_floor can push bid negative, VR division fragility, q_ratio unclamped in size formula
- Strategy: GLFT header formula notation inconsistency, unsigned underflow loop in regime.cpp
- Risk: position_age_blocks uint32 subtraction edge case
- Execution: cancel_stale TTL overflow, evaluate_rebalance unsynchronized
- RPC: exponential backoff no cap, curl handle reuse without reset, no response body size limit
- Config: uppercase hex rejected for asset IDs, std::getenv not thread-safe
- Monitoring: PnL max drawdown when peak=0, pnl_history_ vector erase O(n)
- Types: RebalanceReason uint8 only 2 flags left, Mojo no strong typedef
- Build: stale Phase 2 comment, redundant include directory
- Main: version hardcoded, logging relative path

---

## Test Coverage Gaps

| Category | Headers | Tested | Coverage |
|----------|---------|--------|----------|
| Strategy | 10 | 4 | 40% |
| Risk | 5 | 1 | 20% |
| Execution | 3 | 1 | 33% |
| Data | 2 | 1 | 50% |
| Monitoring | 3 | 0 | 0% |
| Core | 5 | 0 | 0% |
| **Total** | **28** | **7** | **25%** |

**Critical untested modules:** PreTradeCheck (final risk gate), OfferManager (on-chain offers), LiquidityEngine (tier generation), ArbitrageDetector (AMM math), PnLTracker.

---

## Phase 2 Items (Architectural -- Not Fixed This Pass)

1. `co_spawn + use_future` deadlock on single-threaded io_context (requires coroutine rewrite)
2. Blocking CURL in coroutines (requires thread pool dispatch)
3. Signal handler async-signal-safety (requires atomic flag + event loop check)
4. compute_coin_name SHA-256 (requires OpenSSL integration)
5. ensure_split placeholder (requires wallet RPC integration)
6. build_offer_dict unit convention audit (requires end-to-end unit tracing)
7. Strategy instance shared across all pairs (requires per-pair strategy map)

---

## Remediation Priority

1. Fix C1-C4 (critical math/build errors)
2. Fix H1-H10 (high-severity code-fixable issues)
3. Fix M1-M36 (medium issues by subsystem)
4. Address test coverage gaps
5. Plan Phase 2 architectural items
