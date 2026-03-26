# XOPTrader Master TODO List

**Created:** 2026-03-24
**Last Updated:** 2026-03-25 (implementation pass 2: T3-05, T4-12, T4-23, T4-25, T6-05, T6-08, T6-10)
**Source:** Consolidated from all code reviews, logic reviews, and counter-research review in `docs/CODE REVIEWS/`

This document tracks all findings from the review cycle that have **not yet been implemented**. Items already fixed by the Claude Code 3-pass review (commits `d18d396`, `b76ec65`, `18e67f8`) are excluded.

**Status Key:** `[ ]` = Not started | `[~]` = In progress | `[x]` = Complete

---

## Tier 1 — Critical: Must Fix Before Any Live Trading

These are blocking production bugs or severe logic errors identified by multiple reviewers.

### T1-01: Wire full `xop::Engine` into `main.cpp` (replace stub)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.1, CODEREVIEW-GPT-5.4 §1, CODEREVIEW-GPT5.3-Codex §P0-1, CODEREVIEW-Gemini §1
- **Files:** `cpp/src/main.cpp`, `cpp/include/xop/engine.hpp`
- **Issue:** `main.cpp` defines a local stub `Engine` class. The real `xop::Engine` with 13-step orchestration is never instantiated. The shipped binary performs no actual trading.
- **Status:** `[x]` — Verified: `main.cpp` includes `engine.hpp` and uses `xop::Engine` throughout.

### T1-02: Replace blocking CURL with async HTTP
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.2, CODEREVIEW-ClaudeCode-Opus §Phase2-2, LOGICREVIEW-Gemini §1.1
- **Files:** `cpp/src/rpc/chia_rpc.cpp`, `cpp/src/rpc/dexie_client.cpp`
- **Issue:** `curl_easy_perform()` blocks the single-threaded `io_context` event loop for up to 30+ seconds per RPC call. `dexie_client.cpp` also uses `std::this_thread::sleep_for()` for rate limiting, blocking the event loop.
- **Fix:** Dispatch CURL to a thread pool via `boost::asio::post()`, or use async HTTP (Beast / CURLM multi-handle).
- **Status:** `[x]` — CURL dispatched to boost::asio thread_pool; dexie_client converted to coroutines with non-blocking rate limiting.

### T1-03: Eliminate `co_spawn(..., use_future).get()` deadlock pattern
- **Source:** CODEREVIEW-Claude-Opus-4.6 §7.2, CODEREVIEW-GPT-5.4 §2, CODEREVIEW-GPT5.3-Codex §P0-2, CODEREVIEW-ClaudeCode-Opus §Phase2-1
- **Files:** `cpp/src/engine.cpp` (lines ~214, ~345, ~589, ~1017, ~1310)
- **Issue:** Blocking `future.get()` on the same `io_context` thread the coroutine is scheduled on = potential deadlock in single-threaded mode.
- **Fix:** Convert step methods to `co_await` pipeline end-to-end; avoid `.get()`/`wait_for()` inside handlers on the same event loop.
- **Status:** `[x]` — Engine converted to native co_await chain. poll_loop_coro(), on_new_block_coro(), step methods all awaitables. open_connections() co_awaited from poll_loop_coro(). shutdown() uses co_spawn+detached.

### T1-04: Implement proper SHA-256 coin name computation
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.5, CODEREVIEW-ClaudeCode-Opus §Phase2-4
- **Files:** `cpp/src/execution/coin_manager.cpp`
- **Issue:** `compute_coin_name()` returns concatenated hex strings, not SHA-256 hash. All coin identity, locking, and double-spend tracking is broken.
- **Fix:** Implement `SHA256(parent_coin_info || puzzle_hash || amount)` using OpenSSL.
- **Status:** `[x]` — SHA-256 via OpenSSL EVP with CLVM integer encoding. Overflow guards on mojo arithmetic.

### T1-05: Activate coin splitting RPC call
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.6, CODEREVIEW-ClaudeCode-Opus §Phase2-5
- **Files:** `cpp/src/execution/coin_manager.cpp`
- **Issue:** `ensure_split()` has the `send_transaction` RPC call commented out. Multi-tier offer ladder requires pre-split coins.
- **Status:** `[x]` — Activated co_await wallet_->send_transaction(). Overflow guards on total_needed calculation.

### T1-06: Implement Dexie offer submission
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.7, CODEREVIEW-GPT-5.4 §14
- **Files:** `cpp/src/execution/offer_manager.cpp`
- **Issue:** `submit_to_dexie()` always returns `true` without actually submitting to the DEX.
- **Status:** `[x]` — submit_to_dexie() calls dexie_client_->submit_offer() with full error handling.

### T1-07: Fix dangling `c_str()` pointer in TLS configuration
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.4
- **Files:** `cpp/src/rpc/chia_rpc.cpp`
- **Issue:** If `config` object lifetime ends before CURL uses the TLS cert path, it's a use-after-free.
- **Fix:** Store TLS paths in member variables that outlive the CURL handle.
- **Status:** `[x]` — TLS path strings materialized in perform_request() with lifetime spanning curl_easy_perform(). Passed to configure_tls() by const ref.

### T1-08: Fix asset/pair identity mismatch in fills and risk checks
- **Source:** CODEREVIEW-GPT-5.4 §3, CODEREVIEW-GPT5.3-Codex §P0-3, LOGICREVIEW-GPT5.3-Codex §CRITICAL-1
- **Files:** `cpp/src/execution/offer_manager.cpp`, `cpp/src/state.cpp`, `cpp/src/risk/limits.cpp`
- **Issue:** Fill handling writes positions by `pair_name`, but risk checks read by `asset_id`. State positions may be fragmented or invisible to risk gates.
- **Fix:** Normalize all position updates to canonical `AssetId`. Derive pair exposure from asset-level balances.
- **Status:** `[x]` — detect_fills() uses pair_config_map_ to resolve base_asset_id. Engine uses base_asset_id throughout.

### T1-09: Fix inventory updates hardcoded to `"xch"` for all pairs
- **Source:** CODEREVIEW-GPT-5.4 §4, CODEREVIEW-GPT5.3-Codex §P0-4
- **Files:** `cpp/src/engine.cpp` (lines ~595-638, ~731-738)
- **Issue:** After every fill, engine updates `inventory_->record_buy("xch", ...)` regardless of pair. Quote asset and non-XCH base assets are never updated.
- **Fix:** Update both base and quote asset legs per fill; resolve asset IDs from `PairConfig`.
- **Status:** `[x]` — Verified: `engine.cpp` line 676-682 correctly uses `fill_pair_cfg->base_asset_id`.

### T1-10: Fix bid size semantics inconsistency between ladder and execution
- **Source:** CODEREVIEW-GPT-5.4 §5
- **Files:** `cpp/src/strategy/liquidity.cpp`, `cpp/src/execution/offer_manager.cpp`
- **Issue:** `LiquidityEngine` sets bid `TierQuote.size` as quote-side capital; `OfferManager::build_offer_dict()` interprets it as base quantity. Orders are materially mis-sized.
- **Fix:** Make `TierQuote` explicit with `base_size` + `quote_notional`, or enforce one invariant.
- **Status:** `[~]` — TierQuote.size is consistently base quantity in final builder. Semantics consistent but undocumented; consider adding explicit doc/strong types. Verified 2026-03-25: no functional bug, but documentation/typing gap remains.

### T1-11: Create per-pair strategy instances (shared mutable state bug)
- **Source:** CODEREVIEW-GPT-5.4 §6, CODEREVIEW-ClaudeCode-Opus §Phase2-7
- **Files:** `cpp/src/engine.cpp` (lines ~111-116)
- **Issue:** One mutable `AvellanedaStoikov` instance shared across all pairs. `price_buffer_`, `regime_`, `cost_basis_` state bleeds between unrelated markets.
- **Fix:** Maintain one strategy instance per pair, or make strategy stateless and pass pair state explicitly.
- **Status:** `[x]` — Per-pair strategy instances via strategies_ map in engine. Each pair gets independent state.

### T1-12: Fix inventory units mismatch passed to Avellaneda-Stoikov
- **Source:** CODEREVIEW-GPT-5.4 §7, LOGICREVIEW-GPT-5.4 §2, LOGICREVIEW-Gemini §3, LOGICREVIEW-20260325-Claude-Opus-4.6 §LR-1
- **Files:** `cpp/src/engine.cpp` (lines ~1144, ~1198), `cpp/src/strategy/avellaneda.cpp`
- **Issue:** `q_max` documented in base-asset units, but engine passes mojos via `net_inventory()`. `q_ratio = q / q_max` off by ~10^12. Dimensional analysis confirms: mid (XCH) minus q (mojos) × gamma × sigma² × tau produces r ≈ -10^9, clamping all bids to zero. Sizing formula also broken: q_ratio ≈ 10^12 → bid_size = 0 always.
- **Fix:** Convert q to base units: `double q = static_cast<double>(net_inventory(...)) / static_cast<double>(pair_cfg->base_mojos_per_unit);` Same conversion needed in Step 5 (line ~1198).
- **Status:** `[x]` — Inventory converted from mojos to base-asset display units via `/ pair_cfg->base_mojos_per_unit` in both Step 4 (compute_quotes) and Step 5 (compute_spread).

### T1-13: Implement global maximum spread cap
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §ENG-1 (CRITICAL), LOGICREVIEW-Grok §3.1
- **Files:** `cpp/src/engine.cpp` (Step 5)
- **Issue:** 6+ independent multiplicative spread adjustments can compound to ~14× base spread (≈936 bps round-trip). Effectively withdraws from market.
- **Fix:** Add `max_spread_bps` config param (e.g., 500 bps) as final clamp after all multipliers.
- **Status:** `[x]` — Verified: `engine.cpp` lines 971-987 implements configurable `max_half_spread_bps` cap.

### T1-14: CURL handle thread safety
- **Source:** CODEREVIEW-Claude-Opus-4.6 §3.3
- **Files:** `cpp/src/rpc/chia_rpc.cpp`
- **Issue:** Single CURL handle reused across all RPC requests with no synchronization. Concurrent coroutines (e.g., shutdown cancel-all) corrupt handle state.
- **Fix:** Per-request handle pool or mutex protection.
- **Status:** `[x]` — Per-request CURL handles with ScopedCurlEasy/ScopedCurlSlist RAII wrappers. Dead CURLM multi-handle removed.

---

## Tier 2 — High: Must Fix Before Paper Trading

### T2-01: Fix strategy price skew being discarded and rebuilt as symmetric
- **Source:** CODEREVIEW-GPT-5.4 §8
- **Files:** `cpp/src/engine.cpp` (Step 6, lines ~904-917)
- **Issue:** Step 4 computes A-S bid/ask with inventory-aware skew. Step 6 rebuilds from `mid ± half_spread`, losing the skew.
- **Fix:** Preserve strategy bid/ask as base quote; apply spread/risk adjustments on top.
- **Status:** `[x]` — Verified: Step 6 preserves the strategy’s raw quote skew via `reservation_mid`. Whale/OFI multipliers may still override downstream (acceptable).

### T2-02: Add thread safety to volatility, adverse_selection, strategies
- **Source:** CODEREVIEW-Claude-Opus-4.6 §4.1
- **Files:** `cpp/src/data/volatility.cpp`, `cpp/src/data/adverse_selection.cpp`, `cpp/src/strategy/new_strategies.cpp`, `cpp/src/strategy/order_book_tactics.cpp`, `cpp/src/strategy/chia_edge.cpp`, `cpp/src/strategy/strategy_portfolio.cpp`, `cpp/src/monitoring/metrics.cpp`
- **Issue:** 8+ modules have zero thread synchronization on mutable state. Any future multi-threading or coroutine interleaving causes data races.
- **Fix:** Add mutexes consistent with `state.cpp` pattern, or annotate as explicitly single-threaded.
- **Status:** `[x]` — std::shared_mutex added to volatility, adverse_selection, metrics, A-S, GLFT, new_strategies, order_book_tactics, chia_edge. Config snapshots in MarketDataFeed. Lock ordering documented.

### T2-03: Replace detached threads in alerts with worker queue
- **Source:** CODEREVIEW-Claude-Opus-4.6 §4.2, CODEREVIEW-ClaudeCode-Opus §M27
- **Files:** `cpp/src/monitoring/alerts.cpp`
- **Issue:** Detached threads for Telegram sends have no cleanup path. On exit, undefined behavior.
- **Fix:** Persistent worker thread with message queue, or async HTTP client.
- **Status:** `[x]` — Worker thread with std::queue + mutex + condition_variable. Proper start/stop/drain lifecycle.

### T2-04: Fix OFI normalization (sliding window)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §4.4
- **Files:** `cpp/src/execution/market_data.cpp`
- **Issue:** OFI denominator grows without bound over time, driving normalized OFI → 0.
- **Fix:** Normalize within a fixed sliding window or use rolling mean/variance.
- **Status:** `[x]` — Verified: OFI normalization uses bounded `avg_size * (snaps.size()-1)` denominator, clamped to [-1, 1].

### T2-05: Fix Yang-Zhang variance bias (track n_valid)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §4.5, LOGICREVIEW-Claude-Opus-4.6 §VOL-1
- **Files:** `cpp/src/data/volatility.cpp`
- **Issue:** Skipped observations (zero/negative prices) don't adjust sample count `n`. Denominator stays `n_candles - 1`, underestimating volatility → spreads too tight.
- **Fix:** Track `n_valid` separately; use `n_valid - 1` as denominator. Also consider coarser candle granularity (10-block) so most candles have meaningful OHLC.
- **Status:** `[x]` — Verified: `n_valid` tracked separately and used as denominator in volatility.cpp.

### T2-06: Fix tax CSV acquisition dates
- **Source:** CODEREVIEW-Claude-Opus-4.6 §4.6
- **Files:** `cpp/src/monitoring/pnl.cpp`
- **Issue:** Both `Date Acquired` and `Date Sold` set to sell date. IRS Form 8949 export is incorrect.
- **Fix:** Look up original buy fill(s) or store average acquisition timestamp in `AssetRecord`.
- **Status:** `[x]` — acquisition_ts added to TradeRecord and trade_log table. Weighted-average acquisition timestamp tracked in PairPnL.

### T2-07: Fix `const_cast` on SQLite prepared statements
- **Source:** CODEREVIEW-Claude-Opus-4.6 §4.7
- **Files:** `cpp/src/monitoring/pnl.cpp`
- **Issue:** `const` methods use `const_cast` to mutate `sqlite3_stmt`. Concurrent const accessors corrupt statement state.
- **Fix:** Remove `const` qualifier or use separate statement instances per query.
- **Status:** `[x]` — Removed const from 4 methods, eliminated all const_cast.

### T2-08: Make SSL verification configurable (default ON for Chia CA)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §9.8, CODEREVIEW-ClaudeCode-Opus §M30
- **Files:** `cpp/src/rpc/chia_rpc.cpp`
- **Issue:** SSL verification completely disabled (`CURLOPT_SSL_VERIFYPEER = 0`). MITM on wallet RPC could redirect funds.
- **Fix:** Load Chia CA cert and verify. Only disable for localhost.
- **Status:** `[x]` — SSL verification configurable via config_.verify_ssl (default true). Chia CA cert loaded.

### T2-09: Persist actual wallet offer IDs (not placeholders)
- **Source:** CODEREVIEW-GPT-5.4 §11, CODEREVIEW-GPT5.3-Codex §P0-5, CODEREVIEW-Claude-Opus-4.6 §7.4, CODEREVIEW-ClaudeCode-Opus §M-V1
- **Files:** `cpp/src/engine.cpp` (Step 8)
- **Issue:** Placeholder `offer_id` persisted to DB. Real wallet/Dexie IDs assigned later. `update_offer_status()` throws on mismatch.
- **Fix:** Insert offer records after `OfferManager::post_quotes()` returns actual IDs.
- **Status:** `[x]` — PostedOffer struct with offer_id field. Actual wallet-assigned IDs persisted.

### T2-10: Fix trade log `timestamp` column always empty
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §M-V1
- **Files:** `cpp/src/engine.cpp` (~line 610), `cpp/src/database.cpp` (~line 250)
- **Issue:** `tr.timestamp = ""` — all trade records have empty timestamp. Time-range queries broken.
- **Fix:** Populate timestamp in engine with ISO 8601 string, or use SQL default.
- **Status:** `[x]` — Verified: `tr.timestamp = PnLTracker::timestamp_to_iso(fill.timestamp)` populates from fill data.

### T2-11: Wire Prometheus metrics cardinality guard (pass asset_ids)
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §M-V2
- **Files:** `cpp/src/engine.cpp` (~line 235)
- **Issue:** `metrics_->init()` called with empty `asset_ids`. Cardinality bounding is dead code.
- **Fix:** Extract configured asset IDs from `pair_config_map_` and pass to `init()`.
- **Status:** `[x]` — Verified: `metrics_->init()` called with deduplicated `asset_ids` extracted from `config_.pairs`.

### T2-12: Deprecate or remove unguarded `send_alert(AlertTier, ...)` overload
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §M-V3
- **Files:** `cpp/src/monitoring/alerts.cpp`
- **Issue:** Tier-based `send_alert()` bypasses rate limiting. Future callers could flood Telegram.
- **Fix:** Mark `[[deprecated]]` or make private.
- **Status:** `[x]` — send_alert(AlertTier, ...) marked [[deprecated]].

### T2-13: Use `std::llround()` for mojo price conversions (truncation bias)
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §M-V4, CODEREVIEW-ClaudeCode-Opus §M7
- **Files:** `cpp/src/engine.cpp` (lines ~991-998 and others)
- **Issue:** `static_cast<Mojo>()` truncates toward zero. Systematic −0.5 mojo bias on ask prices compounds over thousands of quotes.
- **Fix:** Use `std::llround()` for unbiased rounding.
- **Status:** `[x]` — Verified: All 10 mojo conversions now use `std::llround()`.

### T2-14: Fix `OfferManager::evaluate_rebalance()` asset-ID usage
- **Source:** LOGICREVIEW-GPT-5.4 §D.1, LOGICREVIEW-GPT5.3-Codex §CRITICAL-2
- **Files:** `cpp/src/execution/offer_manager.cpp`
- **Issue:** Queries `state_->inventory_skew(pair_name, pair_name)` — passing same string for both base/quote. Rebalance triggers fire incorrectly.
- **Fix:** Use configured `base_asset_id` and `quote_asset_id`.
- **Status:** `[x]` — Verified: `evaluate_rebalance()` resolves proper asset IDs from `pair_config_map_`.

### T2-15: Fix tier ladder skew direction (may reinforce imbalance)
- **Source:** LOGICREVIEW-GPT-5.4 §D.2, LOGICREVIEW-GPT5.3-Codex §CRITICAL-3
- **Files:** `cpp/src/execution/offer_manager.cpp`
- **Issue:** Positive skew (long base) increases bid size and decreases ask size — this reinforces the long position instead of reducing it. Positive feedback loop.
- **Fix:** Define global sign convention; verify bid_size decreases and ask_size increases when long inventory.
- **Status:** `[x]` — Verified: `bid_size_mult = 1.0 - 0.5*skew`, `ask_size_mult = 1.0 + 0.5*skew`. Correct sign convention.

### T2-16: Fix risk concentration using raw balances instead of mark-to-market
- **Source:** CODEREVIEW-GPT-5.4 §12, CODEREVIEW-GPT5.3-Codex §P1-8, LOGICREVIEW-GPT5.3-Codex §HIGH-1, LOGICREVIEW-Gemini §4
- **Files:** `cpp/src/risk/limits.cpp`
- **Issue:** `compute_concentration()` sums raw mojo balances across different assets. Not economically comparable.
- **Fix:** Mark positions to common numeraire using latest mid/mark prices.
- **Status:** `[x]` — mark_to_xch() converts positions to XCH numeraire via mid prices from State. Conservative fallback when price unavailable.

### T2-17: Fix record_sell() return value ignored during fill processing
- **Source:** LOGICREVIEW-GPT-5.4 §C.3
- **Files:** `cpp/src/engine.cpp` (Step 2)
- **Issue:** `record_sell()` can reject a sell due to no-loss rule, but engine doesn't check returned bool. Inventory state desynchronizes from actual executed fills.
- **Fix:** Check return value; if rejected, log error and flag state inconsistency.
- **Status:** `[x]` — record_sell() return value checked with error logging on rejection.

### T2-18: Fix `build_offer_dict` quote-asset denomination assumption
- **Source:** CODEREVIEW-Claude-Opus-4.6 §5.5, CODEREVIEW-ClaudeCode-Opus §Phase2-6
- **Files:** `cpp/src/execution/offer_manager.cpp`
- **Issue:** Divides quote amount by `kMojosPerXch` always. CAT tokens use different denomination (typically 10^3).
- **Fix:** Look up denomination per asset from pair config or token registry.
- **Status:** `[x]` — Uses pair.quote_mojos_per_unit from PairConfig (default 10^3 for CAT).

### T2-19: Add Kelly sizing division-by-zero guard
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §INV-1 (HIGH)
- **Files:** `cpp/src/risk/inventory.cpp`
- **Issue:** Kelly formula divides by `σ² × τ`. On cold start (first ~10 blocks), sigma = 0.0, producing `inf`/`nan`.
- **Fix:** `if (sigma < 1e-10 || tau < 1e-10) return 0.0;`
- **Status:** `[x]` — Verified: Guards for `sigma < 1e-10 || tau < 1e-10` and `sigma_sq_tau < 1e-15`, both return 0.

### T2-20: Add crowding recovery mechanism (prevent death spiral)
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §SP-3/FL-1 (HIGH)
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** Crowding detection halves weight → fewer fills → lower fill rate → more crowding → permanent suppression at `min_weight`.
- **Fix:** Implement cooldown timer after N blocks at min_weight, or geometric decay instead of binary halving, or fill-rate floor below which crowding is not flagged.
- **Status:** `[x]` — Verified: Cooldown timer, geometric decay (`crowding_decay_factor`), and fill-rate floor all implemented.

---

## Tier 3 — Medium: Quality, Robustness, and Correctness

### T3-01: Consolidate 4 variance ratio implementations into shared `RegimeDetector`
- **Source:** CODEREVIEW-Claude-Opus-4.6 §5.1, LOGICREVIEW-Claude-Opus-4.6 §RD-3, LOGICREVIEW-GPT-5.4 §2 (too many regime detectors)
- **Files:** `cpp/src/data/volatility.cpp`, `cpp/src/strategy/avellaneda.cpp`, `cpp/src/strategy/glft.cpp`, `cpp/src/strategy/new_strategies.cpp`, `cpp/src/strategy/chia_edge.cpp`, `cpp/src/strategy/regime.cpp`
- **Issue:** Four VR implementations with different denominators (N vs N-1), different horizons, and no hysteresis. Strategies can disagree on regime classification.
- **Fix:** Share one canonical `RegimeDetector` (the one in `regime.cpp`) across all consumers.
- **Status:** `[x]` — All 4 local VR implementations removed from A-S, GLFT, ChiaEdge, new_strategies. All delegate to shared RegimeDetector. VolatilityEstimator local VR deprecated with [[deprecated]].

### T3-02: Add PreTradeCheck tests (`enforce_no_loss`, `flash_crash`, `apply_limits`)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §6.1/§10
- **Files:** `cpp/tests/` (new test file needed)
- **Issue:** System's last line of defense before real money is deployed has zero test coverage.
- **Status:** `[ ]`

### T3-03: Add GLFT strategy tests
- **Source:** CODEREVIEW-Claude-Opus-4.6 §10
- **Files:** `cpp/tests/` (new test file needed)
- **Issue:** Primary trading strategy with zero tests.
- **Status:** `[~]` — Two `GlftTest` cases exist in `test_avellaneda.cpp` (inventory skew only). No dedicated GLFT test file; spread/quote/regime logic untested.

### T3-04: Add config parsing tests
- **Source:** CODEREVIEW-Claude-Opus-4.6 §10
- **Files:** `cpp/tests/` (new test file needed)
- **Issue:** No tests that YAML loader correctly rejects invalid input.
- **Status:** `[ ]`

### T3-05: Implement fill-rate feedback loop from database
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.2, CODEREVIEW-GPT-5.4 §15
- **Files:** `cpp/src/engine.cpp`, `cpp/include/xop/database.hpp`, `cpp/src/database.cpp`
- **Issue:** `fill_rate_24h = 0.30` and `fill_rate_per_block = 0.03` hardcoded. Critical input to Kelly sizing, loss manager, Thompson Sampler.
- **Fix:** Added `Database::fill_rate_since_block()` querying resolved offers from offer_log. Engine computes rolling fill rate from last ~4608 blocks (24h) with safe fallbacks.
- **Status:** `[x]`

### T3-06: Add stale-data escalation (widen spread → pull quotes)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.7, CODEREVIEW-GPT-5.4 §10, CODEREVIEW-Gemini §1.2
- **Files:** `cpp/src/engine.cpp`, `cpp/src/execution/market_data.cpp`
- **Issue:** No staleness counter. No distinction between 1-block-stale and 100-blocks-stale. Offers on dangerously stale data invite adverse selection.
- **Fix:** Track `blocks_since_last_update` per pair. Widen spreads proportionally; pull quotes entirely after configurable max-staleness.
- **Status:** `[x]` — `get_staleness_fraction()` added to MarketDataFeed. Engine Step 5 applies graduated spread widening (1x→2x) when data age exceeds 50-100% of stale_threshold.

### T3-07: Wire Strategic Loss Manager decisions into Step 6
- **Source:** CODEREVIEW-Claude-Opus-4.6 §6.3, CODEREVIEW-GPT-5.4 §13
- **Files:** `cpp/src/engine.cpp` (Step 6)
- **Issue:** Loss manager is consulted and logged but never acts. No `LossDecision` flow.
- **Fix:** Call `loss_manager_->evaluate()` and bypass `enforce_no_loss()` when recommendation is to accept loss.
- **Status:** `[x]` — Verified: `should_rebalance_at_loss()` called; when approved, `enforce_no_loss()` is re-applied with constraint disabled.

### T3-08: Wire NHE computation into Step 10
- **Source:** CODEREVIEW-Claude-Opus-4.6 §6.4
- **Files:** `cpp/src/engine.cpp` (Step 10), `cpp/src/risk/hedging.cpp`
- **Issue:** `compute_nhe()` exists but is never called. Layer 2 (NHE) computed nowhere.
- **Fix:** Wire into heartbeat; alert when NHE drops below 0.70.
- **Status:** `[x]` — NHE accumulators reset per cycle. compute_nhe() wired into Step 10 with alert.

### T3-09: Add max-drawdown global circuit breaker
- **Source:** CODEREVIEW-Claude-Opus-4.6 §6.5
- **Files:** `cpp/src/engine.cpp`
- **Issue:** No global stop-loss. Slow series of adverse fills can deplete capital without triggering alerts.
- **Fix:** Add `max_drawdown_pct` parameter; transition engine to `Paused` when HWM drawdown exceeds threshold.
- **Status:** `[x]` — 10% default max drawdown. HWM seeded on first cycle. Active from startup (no profit-gate bypass).

### T3-10: Add flash-crash state machine (Normal → Crash → Recovery → Normal)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §6.6, LOGICREVIEW-GPT5.3-Codex §HIGH-2
- **Files:** `cpp/src/engine.cpp`, `cpp/src/risk/limits.cpp`
- **Issue:** `check_flash_crash()` detects but never called from heartbeat. No state machine. Also uses global-max anchor rather than rolling max-drawdown.
- **Fix:** Add `flash_crash_state_` member; gate Step 8 during crash/recovery phases. Use rolling peak-to-trough.
- **Status:** `[x]` — FlashCrashState enum (Normal/Crash/Recovery). All pairs checked with worst-case aggregation. Step 8 gated during crash.

### T3-11: Implement graduated soft-limit response (proportional sizing)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §6.2, LOGICREVIEW-Claude-Opus-4.6 §RL-1
- **Files:** `cpp/src/risk/limits.cpp`
- **Issue:** Soft limit and hard limit both zero the quote size. No graduation.
- **Fix:** At soft limit, use convex reduction: `factor = 1.0 - ((r - soft) / (hard - soft))²` instead of zeroing.
- **Status:** `[x]` — Verified: Linear interpolation between soft and hard limits with `std::clamp`. Graduated response.

### T3-12: Pre-build `pair_config_map` to eliminate linear lookups
- **Source:** CODEREVIEW-Claude-Opus-4.6 §9.1, CODEREVIEW-GPT-5.4 §cleanup-2
- **Files:** `cpp/src/engine.cpp`
- **Issue:** Same linear search pattern for pair config appears 6+ times.
- **Fix:** Build `std::unordered_map<std::string, const PairConfig*>` in constructor.
- **Status:** `[x]` — Verified: `pair_config_map_` built at startup; O(1) lookup via `find_pair_config()`.

### T3-13: Replace `assert()` with runtime `throw` for config validation
- **Source:** CODEREVIEW-Claude-Opus-4.6 §9.3, CODEREVIEW-Gemini §2.1
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`, `cpp/src/strategy/chia_edge.cpp`, `cpp/src/risk/limits.cpp`
- **Issue:** `assert()` stripped in Release builds. Invalid config silently accepted in production.
- **Fix:** `if (...) throw std::invalid_argument(...)`.
- **Status:** `[x]` — Verified: Named files use `throw` instead of `assert()`. Note: `assert()` persists in `backtest.cpp`, `hedging.cpp`, `dexie_client.cpp`, `chia_rpc.cpp`.

### T3-14: Walk-forward test window fix (window should slide)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §5.3
- **Files:** `cpp/src/backtest.cpp`
- **Issue:** `window_end` always set to `total_blocks`. Later windows have progressively larger test sets, making metrics incomparable.
- **Fix:** `window_end = window_start + train_blocks + test_blocks` clamped to `total_blocks`.
- **Status:** `[x]` — Verified: `window_end = std::min(window_start + fixed_window_size, total_blocks)` — window slides correctly.

### T3-15: Fix drift analyzer Monte Carlo starting from balanced (q=0) instead of current inventory
- **Source:** CODEREVIEW-Claude-Opus-4.6 §5.4
- **Files:** `cpp/src/risk/drift_analyzer.cpp`
- **Issue:** `simulate_drift()` always starts from q=0 regardless of `current_ratio`. Biases time-to-recovery estimates.
- **Fix:** Initialize from `current_ratio`.
- **Status:** `[x]` — Verified: Simulation starts from `q_initial = (current_ratio - 0.5) * total_value / xch_price`.

### T3-16: Validate crossed book data before ingestion
- **Source:** CODEREVIEW-Claude-Opus-4.6 §5.6
- **Files:** `cpp/src/execution/market_data.cpp`
- **Issue:** `ingest_dexie()` doesn't validate `best_bid < best_ask`. Crossed book causes nonsensical mid-price.
- **Fix:** Guard: `if (best_bid >= best_ask) { log_warn; return; }`.
- **Status:** `[x]` — Verified: Crossed-book check rejects `bid >= ask` with warning.

### T3-17: Fix `strategy_portfolio.cpp` component auto-creation via `operator[]`
- **Source:** CODEREVIEW-Claude-Opus-4.6 §5.7
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** `record_pnl()` uses `components_[component]` which default-constructs unknown components with zero weight.
- **Fix:** Use `components_.find()` and reject unknown components.
- **Status:** `[x]` — Verified: `record_pnl()` uses `components_.find()` instead of `operator[]`.

### T3-18: Fix Loss Manager EV double-counting carrying cost
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §LM-1
- **Files:** `cpp/src/risk/loss_manager.cpp`
- **Issue:** Carrying cost appears with positive sign in `EV_rebalance` and negative sign in `EV_hold`, doubling its impact.
- **Fix:** Include carrying cost in only one branch of the comparison.
- **Status:** `[x]` — Verified: Carrying cost appears only in `ev_rebalance`; `ev_no_action` uses only `adverse_selection_ev_hold`. No double-count.

### T3-19: Add hysteresis to order-book tactic selection
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §OBT-1
- **Files:** `cpp/src/strategy/order_book_tactics.cpp`
- **Issue:** Tactic alternates every block when `inventory_ratio` oscillates near threshold. Quote instability reduces fill probability.
- **Fix:** Require N consecutive blocks (default 3) confirming new tactic before switching.
- **Status:** `[x]` — tactic_hysteresis_blocks config (default 3). Counter accumulates before switch. HybridRebalance bypasses for safety.

### T3-20: Fix NHE returning 0.0 when no volume (should be 1.0 or nullopt)
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §HG-1
- **Files:** `cpp/src/risk/hedging.cpp`
- **Issue:** Zero volume = NHE 0.0 → false "below target" alerts.
- **Fix:** Return 1.0 or `std::nullopt` when V = 0.
- **Status:** `[x]` — Verified: Returns 1.0 when `total_volume <= 0.0`.

### T3-21: Use `std::deque` for `pnl_history_` (O(1) front removal)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §9.4
- **Files:** `cpp/src/monitoring/pnl.cpp`
- **Issue:** `std::vector::erase(begin())` is O(n) per trim.
- **Status:** `[x]` — Changed to std::deque<PnLSnapshot>, erase(begin()) → pop_front().

### T3-22: Use `steady_clock` for staleness in `MempoolSentinelStrategy`
- **Source:** CODEREVIEW-Claude-Opus-4.6 §9.5
- **Files:** `cpp/src/strategy/new_strategies.cpp`
- **Issue:** `system_clock` susceptible to NTP corrections.
- **Status:** `[x]` — first_seen_steady uses steady_clock::time_point.

### T3-23: Fix move assignment operator lock ordering risk in `MarketDataFeed`
- **Source:** CODEREVIEW-Claude-Opus-4.6 §9.7
- **Files:** `cpp/src/execution/market_data.cpp`
- **Issue:** Two locks acquired without global ordering → ABBA deadlock potential.
- **Fix:** Use `std::scoped_lock(mtx_a, mtx_b)`.
- **Status:** `[x]` — std::scoped_lock(mtx_a, mtx_b) in move assignment. Lock ordering documented in header.

### T3-24: Add dependency-aware gating in engine step failure handling
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §ENG-2, LOGICREVIEW-GPT5.3-Codex §HIGH-4
- **Files:** `cpp/src/engine.cpp`
- **Issue:** Failed Step 1 allows Steps 4-8 to proceed with stale data. No per-pair validity tracking.
- **Fix:** If Step 1 fails for a pair, block Steps 4-8 for that pair. Track `data_quality` flags.
- **Status:** `[x]` — Per-pair market_data_valid flag in PairCycleState gates steps 4-8. Invalid data skips quoting for that pair.

### T3-25: Add stale-quote detection for own offers (price deviation trigger)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.1
- **Files:** `cpp/src/engine.cpp`, `cpp/src/execution/offer_manager.cpp`
- **Issue:** Offers only cancelled by TTL. An offer posted at mid=100 may still be active after mid moves to 110. Adverse selection risk.
- **Fix:** Check distance from current mid; cancel immediately if deviation exceeds outer tier spacing.
- **Status:** `[x]` — Verified: `evaluate_rebalance()` cancels offers on >2% mid-price deviation; engine also calls `cancel_stale()` by TTL.

### T3-26: Fix Brock-Hommes weight updates unstable with sparse fills
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §SP-4
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** 2-3 fills per lookback window. Single fortunate fill → 10%+ reallocation.
- **Fix:** Weight PnL updates by fill count: `effective_beta = beta × min(1.0, fill_count / 10)`.
- **Status:** `[x]` — effective_beta = beta_intensity * min(1.0, fill_count / min_fills_for_full_weight). Default 10 fills.

### T3-27: Dynamic PIN threshold based on volatility
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §PIN-1
- **Files:** `cpp/src/data/adverse_selection.cpp`
- **Issue:** Fixed 30 bps threshold classifies ~22% of random noise as "adverse".
- **Fix:** `threshold = 1.5 × sigma_block × sqrt(observation_blocks)`.
- **Status:** `[x]` — effective_adverse_threshold() computes 1.5 * sigma_block * sqrt(obs_blocks). Falls back to fixed threshold.

### T3-28: Fix HMM state sorting (by stddev, not mean)
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §RD-2
- **Files:** `cpp/src/strategy/regime.cpp`
- **Issue:** Post Baum-Welch, states sorted by emission mean. If return distribution has drift, sort order may be wrong.
- **Fix:** Sort by emission standard deviation.
- **Status:** `[x]` — hmm_sort_states_by_stddev() sorts by emission stddev ascending. Permutes all arrays + transition matrix.

### T3-29: Fix `config.cpp` rejecting uppercase hex in asset IDs
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §L-V4
- **Files:** `cpp/src/config.cpp`
- **Issue:** `validate_asset_id()` only accepts `[0-9a-f]{64}`. Chia tools sometimes emit uppercase.
- **Fix:** Case-insensitive check or normalize to lowercase.
- **Status:** `[x]` — to_lower lambda normalizes asset IDs to lowercase before validation.

### T3-30: Fix OU drift simulation numerical instability (Euler → exact)
- **Source:** LOGICREVIEW-Gemini §6
- **Files:** `cpp/src/risk/drift_analyzer.cpp`
- **Issue:** Forward Euler `q -= theta * q` can oscillate if θ > 1.0.
- **Fix:** Use exact discrete solution: `q = q_old * exp(-theta)`.
- **Status:** `[x]` — Verified: Uses `q *= std::exp(-theta)` with explicit comment about replacing Euler.

### T3-31: Fix ChiaEdge multiplicative vs additive savings
- **Source:** LOGICREVIEW-Gemini §5
- **Files:** `cpp/src/strategy/chia_edge.cpp`
- **Issue:** Independent additive savings multiplied together. 4 × 10bps savings = 40bps savings (additive), but multiplication gives 0.6561× (leaks 5.39 bps).
- **Fix:** Consolidate savings into additive basis points pool.
- **Status:** `[~]` — Design decision: `composite_edge_multiplier()` intentionally uses multiplicative product of 5 edge factors. The "fix" to make additive may not be correct for this use case. Review intent.

### T3-32: Fix `wallet_fingerprint: 0` accepted without rejection
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11 (Config Gaps)
- **Files:** `cpp/src/config.cpp`
- **Issue:** Sentinel value 0 should fail validation.
- **Status:** `[x]` — Guard wallet_fingerprint == 0 throws ConfigError.

### T3-33: Fix competition spread formula sign (widen vs. undercut)
- **Source:** CODEREVIEW-GPT5.3-Codex §P1-7
- **Files:** `cpp/src/strategy/spread.cpp`
- **Issue:** `calc_competition_bps` returns `max(floor, best_competing + epsilon)` — widens vs. competitor. Docs imply undercut/join-inside.
- **Fix:** Re-validate intended sign. Competitiveness usually means tightening.
- **Status:** `[x]` — Fixed to max(s_floor, best_competing - epsilon) (undercut). Competition caps base spread via min(base, s_comp).

### T3-34: Validate weight clamping convergence (`N × min_weight ≤ 1.0`)
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §SP-5
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** Non-convergence when `8 × min_weight > 1.0`.
- **Fix:** Validate in constructor.
- **Status:** `[x]` — Constructor validates kNumComponents * cfg_.min_weight > 1.0 throws. Max iteration guard in clamp_weights.

### T3-35: Separate self-generated fills from market-generated toxicity signals
- **Source:** LOGICREVIEW-GPT-5.4 §3 (endogenous feedback loops)
- **Files:** `cpp/src/data/adverse_selection.cpp`, `cpp/src/execution/market_data.cpp`
- **Issue:** Whale/VPIN/OFI fed from own fills → self-reinforcing → widening spiral.
- **Fix:** Use own fills for attribution/calibration, not as primary toxicity input.
- **Status:** `[x]` — is_own_fill parameter on ingest_trade() and ingest_trade_for_vpin(). Own fills skip whale detection and VPIN accumulation.

---

## Tier 4 — Low/Enhancement: Quality Improvements and Strategic Features

### T4-01: CEX reference price integration (OKX/Gate.io)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.5, CODEREVIEW-GPT-5.4 §14
- **Issue:** No off-chain price reference. System blind to DEX-CEX divergence.
- **Status:** `[ ]`

### T4-02: Add reorg/confirmation-depth protection
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.4, LOGICREVIEW-Claude-Opus-4.6 §10.4
- **Issue:** No mechanism to detect reverted fills, roll back cost basis, or mark offers as uncertain during chain reorg.
- **Fix:** Add confirmation-depth parameter (e.g., 6 blocks); only record fills behind current height by that depth.
- **Status:** `[ ]`

### T4-03: Fee budget tracking and alerting
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.6
- **Issue:** No cumulative fee tracking. During Chia congestion, fees could exceed spread earnings.
- **Fix:** Add `fee_budget_per_day` config; warn and reduce quoting frequency when approaching budget.
- **Status:** `[ ]`

### T4-04: Environment variable substitution in YAML config
- **Source:** CODEREVIEW-Claude-Opus-4.6 §12
- **Issue:** SSL paths and Telegram tokens stored as plaintext in YAML.
- **Status:** `[ ]`

### T4-05: Expose VPIN/OFI/whale/competitor config params in YAML
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11
- **Issue:** Only code-configurable; not in config file.
- **Status:** `[ ]`

### T4-06: Integration test framework (backtest as CI test)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §10
- **Issue:** No integration test target in CMake; only unit tests.
- **Status:** `[ ]`

### T4-07: Add engine startup/shutdown and fill processing tests
- **Source:** CODEREVIEW-GPT-5.4 §16, CODEREVIEW-GPT5.3-Codex (Test Gaps)
- **Issue:** Most operationally dangerous paths have zero test coverage.
- **Status:** `[ ]`

### T4-08: Add `TierQuote` to wallet offer translation tests
- **Source:** CODEREVIEW-GPT-5.4 §16
- **Status:** `[ ]`

### T4-09: Implement partial inventory recovery / aging schedule
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.3
- **Issue:** No DCA, time-based spreading, or progressive loss-floor relaxation for aged underwater positions.
- **Fix:** After N blocks underwater, gradually relax no-loss floor (e.g., accept 10bps after 1000 blocks).
- **Status:** `[ ]`

### T4-10: Implement per-tier fill-rate monitoring and dynamic allocation
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §10.3
- **Issue:** Fixed `tier_size_pct` doesn't adapt to which tiers are actually filling.
- **Fix:** Track per-tier fill rates; adjust allocations toward active tiers.
- **Status:** `[ ]`

### T4-11: Implement offer-state reconciliation daemon
- **Source:** CODEREVIEW-GPT5.3-Codex (Missing Safeguards §4), CODEREVIEW-GPT-5.4 §cleanup-4
- **Issue:** No periodic reconciliation of DB/state with wallet truth. Orphaned/cancelled offers can accumulate.
- **Status:** `[ ]`

### T4-12: Feature-gate incomplete live paths (arb, hedging, Dexie submission)
- **Source:** CODEREVIEW-GPT-5.4 §14 (Missing Safeguards §6)
- **Issue:** Incomplete features (arbitrage execution, hedging trades, Dexie submission) return success silently.
- **Fix:** Verified all previously-flagged stubs are now fully implemented — no feature-gating needed.
- **Status:** `[x]` — N/A, all paths implemented.

### T4-13: Consolidate fill accounting into single function
- **Source:** CODEREVIEW-GPT-5.4 §cleanup-3
- **Issue:** Fill → state/inventory/PnL/database updates scattered across multiple locations.
- **Fix:** One unit-tested function updates all systems atomically.
- **Status:** `[~]` — `step_process_fills` consolidates fill handling in one function, but still uses `.get()` pattern and no atomic accounting.

### T4-14: Introduce typed quantity wrappers (base_size, quote_notional, price)
- **Source:** CODEREVIEW-GPT-5.4 §cleanup-4
- **Issue:** Unit confusion between mojos, XCH, base, quote, price throughout codebase.
- **Fix:** Strong types to catch unit mismatches at compile time.
- **Status:** `[ ]`

### T4-15: Implement adaptive block time estimation
- **Source:** LOGICREVIEW-Gemini §2, LOGICREVIEW-Grok §4.1
- **Issue:** Fixed 52s block time assumption. Chia blocks follow exponential distribution.
- **Fix:** Rolling average from recent block arrivals.
- **Status:** `[ ]`

### T4-16: Online κ / fill-intensity calibration
- **Source:** LOGICREVIEW-Grok §4.2
- **Issue:** Static κ parameter becomes stale as market evolves.
- **Fix:** Online estimation from recent fill data.
- **Status:** `[ ]`

### T4-17: Add `MempoolSentinelStrategy` mempool sign/comment fix
- **Source:** LOGICREVIEW-GPT-5.4 §A.6
- **Issue:** `mempool_skew_adjustment()` has sign inconsistency with comments.
- **Status:** `[ ]`

### T4-18: Add Guéant correction citation to A-S half-spread formula
- **Source:** LOGICREVIEW-Gemini §1
- **Issue:** Formula uses GLFT-corrected kappa/gamma positions. Without comments, maintainers may "fix" it back to flawed original.
- **Status:** `[ ]`

### T4-19: Replace A-S sawtooth tau with constant risk-decay horizon or GLFT asymptotic
- **Source:** LOGICREVIEW-Gemini §2, LOGICREVIEW-Claude-Opus-4.6 §AS-1, COUNTERRESEARCH §2.2 (CR-3)
- **Issue:** Sawtooth creates periodic vulnerability windows and exploitable quoting cycles. Counter-research (Cartea et al. 2015 §10.3) corroborates with explicit academic opposition.
- **Fix:** Use constant horizon equal to desired inventory half-life, or GLFT time-independent asymptotic equations. Alternative: exponential decay per Stoikov (2018) "The micro-price".
- **Cross-ref:** T5-CR3 (counter-research corroboration)
- **Status:** `[ ]`

### T4-20: Add cross-strategy disagreement detection for inventory direction
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §10.2
- **Issue:** When strategies disagree on direction, blend averages them. Correct response is to reduce size.
- **Fix:** Compute disagreement metric; when high, shrink sizes on both sides.
- **Status:** `[ ]`

### T4-21: Update vcpkg baseline (18 months stale)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11
- **Issue:** `vcpkg baseline 2024.09.30` — potential security patches missing.
- **Status:** `[ ]`

### T4-22: Add GCC/Clang global `-Werror`
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §L-V3
- **Issue:** Only `-Werror=return-type` set; other warnings don't fail builds.
- **Status:** `[ ]`

### T4-23: Fix diagnostic count functions not checking `sqlite3_step()` return
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §L-V6
- **Files:** `cpp/src/database.cpp`
- **Fix:** Added `int rc =` check + `spdlog::warn` on failure for `trade_count()`, `offer_count()`, `snapshot_count()`.
- **Status:** `[x]`

### T4-24: Document which config fields are required vs. optional
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11
- **Status:** `[~]` — `config.example.yaml` marks alerts as optional; most other fields undocumented. `config.cpp` enforces some but doesn't document which.

### T4-25: Add no-HTML-escaping guard for Telegram alert messages
- **Source:** CODEREVIEW-Claude-Opus-4.6 §12
- **Issue:** Injection risk in Telegram Bot API messages.
- **Fix:** Added `html_escape` lambda in `post_telegram()` escaping `&<>"` before URL-encoding. Unsafe fallback path now uses escaped text.
- **Status:** `[x]`

### T4-26: Add multivariate / cross-asset correlation modeling
- **Source:** LOGICREVIEW-Grok §4.4, LOGICREVIEW-Gemini §4.3 (gap)
- **Issue:** No cross-pair risk correlation considered.
- **Status:** `[ ]`

### T4-27: Create calibration registry for all thresholds
- **Source:** LOGICREVIEW-GPT-5.4 (Cross-Cutting §2)
- **Issue:** Thresholds occupy multiple roles (theory/data/operator) simultaneously without labeling.
- **Fix:** Document each as: literature-derived, data-estimated, or operator safety override.
- **Status:** `[ ]`

---

## Tier 5 — Counter-Research: Academic Challenges to Cited Literature

Items derived from [COUNTERRESEARCH-20260325-1](docs/CODE%20REVIEWS/COUNTERRESEARCH-20260325-1-GitHubCopilot-Claude-Sonnet-4.6.md). These address known academic disputes, empirical rebuttals, or limitations of the theories XOPTrader relies on. Inline `COUNTER-RESEARCH NOTE` comments have been placed at each affected code site.

### T5-CR1: Validate VPIN against realized adverse fills before relying on multiplier
- **Source:** COUNTERRESEARCH §7 (CR-1, HIGH); Andersen & Bondarenko (2014)
- **Files:** `cpp/src/engine.cpp` (Step 5 VPIN multiplier), `cpp/src/execution/market_data.cpp`
- **Issue:** VPIN has no incremental predictive power beyond raw volume and volatility. Can fire on pure noise-trader correlation. The 50% spread widening at max toxicity may amplify noise rather than protect.
- **Fix:** Track VPIN-activation → realized-adverse-fill correlation over rolling 500-fill window. If correlation < threshold (e.g., 0.15), attenuate or disable VPIN multiplier. Consider ensemble with volatility as a cross-check.
- **Status:** `[x]` — Rolling-window VPIN validation gate: per-block dedup, kVpinActivationThreshold, precision tracking with rolling counters, burn-in gating, kMaxPendingActivations cap.

### T5-CR2: Extend OFI to multi-level (top 5–10 book levels)
- **Source:** COUNTERRESEARCH §8 (CR-2, HIGH); Xu, Lehalle & Alfonsi (2023)
- **Files:** `cpp/src/execution/market_data.cpp` (`ingest_book_snapshot_for_ofi`)
- **Issue:** Current OFI uses best-level bid/ask only. Multi-level OFI explains 10–30% more return variance. CHIA's shallow book (2–5 levels) makes multi-level computation feasible.
- **Fix:** Extend `ingest_book_snapshot_for_ofi()` to accept vector of `(price, size)` per side. Weight each level's contribution by inverse distance from mid.
- **Status:** `[ ]`

### T5-CR3: Replace A-S sawtooth tau with exponential decay or GLFT asymptotic
- **Source:** COUNTERRESEARCH §2.2 (CR-3, HIGH); Cartea, Jaimungal & Penalva (2015) §10.3
- **Files:** `cpp/src/strategy/avellaneda.cpp` (`compute_tau`)
- **Issue:** Sawtooth tau creates a deterministic, exploitable complacency window post-reset in 24/7 markets. Adversaries can time orders to exploit the post-reset phase.
- **Fix:** Replace `block_height % horizon_blocks` modular arithmetic with exponential decay `τ(t) = τ₀ · exp(−λt)` per Stoikov (2018), or use GLFT's time-independent asymptotic equations.
- **Cross-ref:** T4-19 (same fix from logic review perspective)
- **Status:** `[x]` — Exponential-decay tau in both A-S and GLFT. Fill-driven reset via record_fill() wired through StrategyBase. tau_min < tau_max constructor guard. Unit tests updated.

### T5-CR4: Add PIN/VPIN cross-validation against known-informed-trading episodes
- **Source:** COUNTERRESEARCH §6 (CR-4, MEDIUM); Duarte & Young (2009); Collin-Dufresne & Fos (2015)
- **Files:** `cpp/src/data/adverse_selection.cpp`
- **Issue:** PIN may measure illiquidity friction rather than genuine informed trading. Collin-Dufresne & Fos (2015) find PIN is lowest when known informed traders (Schedule 13D filers) are most active.
- **Fix:** During backtesting, inject known adverse-selection episodes (e.g., post-hack dumps) and verify that PIN/VPIN elevate at those times. If correlation is weak, reduce PIN weight in spread decisions.
- **Status:** `[ ]`

### T5-CR5: Add statistical significance gating to VR regime classification
- **Source:** COUNTERRESEARCH §4 (CR-5, MEDIUM); Lo & MacKinlay (1989); Richardson & Smith (1991)
- **Files:** `cpp/src/strategy/regime.cpp`
- **Issue:** VR test has ~5–9% power at n=50–200 (XOPTrader's window sizes). Regime classification relies on raw VR thresholds (0.85/1.15), not statistically significant signals.
- **Fix:** Compute Z-statistic for VR and only classify regime as non-random-walk when |Z| > 1.96 (95% confidence). When insignificant, default to random-walk regime rather than allowing noisy classifications.
- **Status:** `[x]` — Z-statistic significance gate in classify_vr(). Both horizons stored. Diagnostic logging with Z-stats. NaN guard at update() boundary.

### T5-CR6: Construct coarser-grained candles for Yang-Zhang volatility
- **Source:** COUNTERRESEARCH §5 (CR-6, MEDIUM); Molnár (2012)
- **Files:** `cpp/src/data/volatility.cpp`
- **Issue:** >90% of CHIA blocks have zero fills → degenerate candles (O=H=L=C). Yang-Zhang degenerates to close-to-close estimator, losing its minimum-variance advantage.
- **Fix:** Aggregate blocks into 10-block windows (~8.7 min) to ensure most candles have at least one fill and meaningful OHLC variation. Consider Garman-Klass (1980) as competitive alternative for continuous 24/7 markets.
- **Status:** `[ ]`

### T5-CR7: Implement discounted Thompson Sampling for spread optimizer
- **Source:** COUNTERRESEARCH §10 (CR-7, MEDIUM); Besbes, Gur & Zeevi (2014)
- **Files:** `cpp/src/strategy/spread.cpp` (`ThompsonSampler::record_outcome`)
- **Issue:** Standard Beta posterior is too slow to forget outdated spread-width feedback across regime changes. Regret guarantees fail under non-stationarity.
- **Fix:** Replace `alpha += 1, beta += 1` with `alpha = alpha * γ + success, beta = beta * γ + failure` where γ ∈ [0.95, 0.99]. This discounted posterior forgets stale evidence geometrically.
- **Status:** `[x]` — Discounted Thompson Sampling with gamma=0.97 default, alpha/beta floor at 1.0, constructor validation.

### T5-CR8: Amplify GLFT inventory skew coefficient for sparse discrete fills
- **Source:** COUNTERRESEARCH §3.1 (CR-8, MEDIUM); Fodra & Pham (2015); Laruelle, Lehalle & Pagès (2011)
- **Files:** `cpp/src/strategy/glft.cpp` (`fill_intensity`)
- **Issue:** GLFT's continuous-time exponential fill intensity was calibrated for dense electronic markets. On CHIA (~1 fill/hour/pair), optimal inventory skew should be larger than GLFT produces — trader should shed inventory more aggressively per fill.
- **Fix:** Apply sparse-fill correction factor: multiply inventory skew coefficient by `max(1.0, expected_fills_per_hour_dense / actual_fills_per_hour)`. Calibrate from fill-rate database.
- **Status:** `[x]` — Sparse-fill correction in inventory_skew(): effective_phi = phi * min(max(1, dense/actual), cap). Config validated. Unit tests added.

### T5-CR9: Enforce minimum fill count for Brock-Hommes weight updates
- **Source:** COUNTERRESEARCH §9.2 (CR-9, MEDIUM); Brock, Hommes & Wagener (2006)
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** 2–3 fills per evaluation window vs. BHW's own requirement of 30+ observations. Single fortunate fill can cause 10%+ reallocation.
- **Fix:** Already mitigated by T3-26 (fill-count dampened beta). Recommend calibrating `min_fills_for_full_weight ≥ 10` and documenting the BHW threshold requirement.
- **Cross-ref:** T3-26 (implemented mitigation)
- **Status:** `[x]` — Mitigated by T3-26 fill-count dampening. Inline comment added with BHW citation.

### T5-CR10: Replace Kyle lambda linear impact with square-root model
- **Source:** COUNTERRESEARCH §12 (CR-10, LOW); Gatheral (2010); Almgren et al. (2005)
- **Files:** Documentation / `cpp/src/strategy/spread.cpp` (if Kyle lambda is used in spread formulae)
- **Issue:** Kyle's λ linear impact is inconsistent with no-dynamic-arbitrage conditions and empirically rejected in favour of square-root impact (√volume).
- **Fix:** If linear impact is used in any spread or cost computation, replace with `impact = η · sign(Q) · |Q|^0.5` per Almgren empirical estimates.
- **Status:** `[ ]`

### T5-CR11: Add TibetSwap protocol-evolution risk monitoring
- **Source:** COUNTERRESEARCH §13 (CR-11, LOW); Milionis et al. (2022); Adams et al. (2023)
- **Files:** `cpp/src/strategy/arbitrage.cpp`
- **Issue:** TibetSwap arbitrage revenue is structurally dependent on AMM design. Future LVR-reduction features (dynamic fees, oracle-based pricing) could eliminate this revenue stream.
- **Fix:** Track TibetSwap protocol version and fee parameters. Alert when fee structure changes suggest reduced arbitrage opportunity. Build revenue dependency diversification into strategy portfolio.
- **Status:** `[ ]`

### T5-CR12: Calibrate AMH crowding-recovery windows to CHIA timescales
- **Source:** COUNTERRESEARCH §11 (CR-12, LOW); Urquhart & Hudson (2013)
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** AMH (Lo 2004) framework is not falsifiable without operationalization. Crowding-recovery window lengths should be calibrated to CHIA market timescales, not equity-market intuitions.
- **Fix:** Estimate crowding half-life from historical fill-rate data. Use CHIA-specific block cadence (~52s) as the natural time unit rather than calendar days.
- **Status:** `[ ]`

### T5-CR13: Implement Stoll three-component spread decomposition
- **Source:** COUNTERRESEARCH §16 (CR-13, MEDIUM); Stoll (1989)
- **Files:** `cpp/src/data/adverse_selection.cpp`, `cpp/src/engine.cpp`
- **Issue:** Glosten-Milgrom spread model attributes entire spread to adverse selection, but on thin DEX markets, order-processing costs (blockchain fees, offer TTL) and inventory costs (capital lockup) are likely dominant components.
- **Fix:** Decompose spread into: (1) adverse selection (PIN-based), (2) inventory risk (A-S/GLFT skew), (3) order-processing (blockchain fee + TTL opportunity cost). Weight spread decisions by the dominant component rather than assuming all-adverse-selection.
- **Status:** `[ ]`

### T5-CR14: Evaluate multifractal volatility model as HMM alternative
- **Source:** COUNTERRESEARCH §18 (CR-14, MEDIUM); Calvet & Fisher (2004)
- **Files:** `cpp/src/strategy/regime.cpp`
- **Issue:** HMM regime detection suffers from likelihood multimodality and regime identification fragility with short crypto histories. Multifractal models (Calvet-Fisher MSM) capture multi-scale volatility dynamics more faithfully.
- **Fix:** Prototype Markov-Switching Multifractal (MSM) estimator alongside existing HMM. Compare regime stability over 1000+ blocks. If MSM produces fewer spurious regime switches, adopt as primary.
- **Status:** `[ ]`

### T5-CR15: Model time-varying liquidity risk (Acharya-Pedersen dynamics)
- **Source:** COUNTERRESEARCH §17 (CR-15, LOW); Acharya & Pedersen (2005)
- **Files:** Documentation / spread computation
- **Issue:** Static Amihud-Mendelson spread-return framework ignores liquidity *risk* dynamics. Time-varying spread is more relevant for CHIA's intermittent liquidity.
- **Fix:** Track rolling spread volatility (σ_spread over past N blocks). When spread-of-spread is high, increase safety margin on tier sizing to account for liquidity uncertainty.
- **Status:** `[ ]`

---

## Tier 6 — New Findings from 2026-03-25 Review Cycle

Items from [CODEREVIEW-20260325-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/CODEREVIEW-20260325-GitHubCopilot-Claude-Opus-4.6.md) and [LOGICREVIEW-20260325-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/LOGICREVIEW-20260325-GitHubCopilot-Claude-Opus-4.6.md).

### T6-01: Remove residual `assert()` calls in production code
- **Source:** CODEREVIEW-20260325 §CR-1
- **Files:** `cpp/src/backtest.cpp` (L505-506), `cpp/src/risk/hedging.cpp` (L61), `cpp/src/rpc/chia_rpc.cpp` (L332), `cpp/src/rpc/dexie_client.cpp` (L40-41)
- **Issue:** 6 `assert()` calls compiled out in Release. Two are guarded by subsequent `if` checks (hedging.cpp, chia_rpc.cpp); four are not (backtest.cpp, dexie_client.cpp).
- **Status:** `[x]` — All 6 `assert()` replaced: backtest.cpp → `throw std::invalid_argument`; hedging.cpp → redundant assert removed (runtime guard retained); chia_rpc.cpp → `throw std::invalid_argument`; dexie_client.cpp → `throw std::invalid_argument`. `#include <cassert>` replaced with `#include <stdexcept>` where needed.

### T6-02: Fix `pyproject.toml` deprecated build backend
- **Source:** CODEREVIEW-20260325 §CR-3
- **Files:** `pyproject.toml` (L3)
- **Issue:** `setuptools.backends._legacy:_Backend` is deprecated and will break in future setuptools.
- **Fix:** Use `setuptools.build_meta`.
- **Status:** `[x]` — Build backend changed to `setuptools.build_meta`.

### T6-03: Consolidate `requirements.txt` into `pyproject.toml`
- **Source:** CODEREVIEW-20260325 §CR-4
- **Files:** `gui/requirements.txt`, `pyproject.toml`
- **Issue:** Two dependency manifests with non-overlapping packages. Confusing and error-prone.
- **Fix:** Move GUI deps (`PySide6`, `pyqtgraph`, `requests`) into `[project.optional-dependencies] gui = [...]`.
- **Status:** `[x]` — GUI deps already in pyproject.toml `[project.optional-dependencies] gui`. requirements.txt updated with comment pointing to pyproject.toml as canonical source.

### T6-04: Add upper bounds to Python dependency versions
- **Source:** CODEREVIEW-20260325 §CR-5
- **Files:** `pyproject.toml`, `gui/requirements.txt`
- **Issue:** `PySide6>=6.6.0`, `PyYAML>=6.0`, `requests>=2.31.0` have no upper bounds. Breaking changes in major versions will not be caught.
- **Fix:** Add `<7.0.0` or `<N+1.0` upper bounds. Add `requires-python = ">=3.11,<4"`.
- **Status:** `[x]` — Upper bounds added to all deps in pyproject.toml. `requires-python = ">=3.11,<4"` set. gui/requirements.txt bounds aligned.

### T6-05: Add SHA-256 verification for C++ FetchContent downloads
- **Source:** CODEREVIEW-20260325 §CR-5
- **Files:** `cpp/cmake/dependencies.cmake`
- **Issue:** nlohmann_json, spdlog, yaml-cpp fetched via git tag with no hash verification. Supply-chain attack vector.
- **Fix:** Pinned GIT_TAG to full commit SHAs: nlohmann_json `9cca280a4d0ccf7c29f049debff758194de23041` (v3.11.3), spdlog `27cb4c76708608465c413f6d0e6b8d99a4d84302` (v1.14.1), yaml-cpp `f7320141120f720aecc4c32be25586e7da9eb978` (0.8.0).
- **Status:** `[x]`

### T6-06: Fix `config.example.yaml` truncated asset IDs and internal inconsistencies
- **Source:** CODEREVIEW-20260325 §CR-6
- **Files:** `config.example.yaml` (L29, L32, L43-44)
- **Issue:** Asset IDs truncated (32 of 64 hex chars). `tier_spacing_bps: [60, 200, 500, 1000]` has tiers exceeding `max_half_spread_bps: 250`. `wallet_fingerprint: 0` should be a clear placeholder.
- **Fix:** Use full 64-char placeholder asset IDs. Align tier_spacing with max_half_spread_bps. Use descriptive placeholder for fingerprint.
- **Status:** `[x]` — Asset IDs padded to 64 hex chars. tier_spacing_bps reduced to [60,120,180,240] (all ≤ max_half_spread_bps 250). wallet_fingerprint uses descriptive placeholder.

### T6-07: Add error handling in GUI service initialization
- **Source:** CODEREVIEW-20260325 §CR-7
- **Files:** `gui/main.py`
- **Issue:** `bridge.initialise()` failures not caught; UI shows responsive but is disconnected from backend.
- **Fix:** Wrap in try/except, show QMessageBox error, exit gracefully.
- **Status:** `[x]` — `bridge.initialise()` wrapped in try/except with QMessageBox.critical and sys.exit(1).

### T6-08: Add LTO for Release builds
- **Source:** CODEREVIEW-20260325 §CR-10
- **Files:** `cpp/CMakeLists.txt`
- **Issue:** No Link-Time Optimization in Release builds. 5-15% speedup available.
- **Fix:** Added `CheckIPOSupported` check + `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON` after hardening flags.
- **Status:** `[x]`

### T6-09: Packaging files missing required assets
- **Source:** CODEREVIEW-20260325 §CR-8, §CR-9
- **Files:** `packaging/windows/installer.iss`, `packaging/linux/install.sh`, `packaging/linux/xop_trader.desktop`, `packaging/linux/uninstall.sh`
- **Issue:** Windows installer references `icon.ico` not in repo. Desktop file has stale `Exec` path. No uninstall script for Linux.
- **Fix:** Icon generated at CI time via `generate_icon.py`. Desktop `Exec` path updated to `~/.local/bin/xop_trader_gui`. Uninstall script (`uninstall.sh`) added with `--purge` option. CI bundles uninstall script in Linux release tarball.
- **Status:** `[x]`

### T6-10: Offer fee hardcoded (100M mojos) across all pairs
- **Source:** CODEREVIEW-20260325 (offer_manager.cpp analysis)
- **Files:** `cpp/include/xop/config.hpp`, `cpp/src/config.cpp`, `cpp/src/execution/offer_manager.cpp`, `cpp/src/execution/coin_manager.cpp`, `config.example.yaml`
- **Issue:** Offer creation fee and split fee hardcoded at 100,000,000 mojos (~0.0001 XCH). If Chia network fee dynamics change, this must be configurable.
- **Fix:** Added `offer_fee_mojos` field to `StrategyConfig` (default 100M). YAML parsing with validation. All 5 hardcoded constants replaced with `strategy_cfg_.offer_fee_mojos`.
- **Status:** `[x]`

---

## Already Fixed (Reference Only)

The following were resolved by Claude Code's 3-pass review cycle (commits `d18d396`, `b76ec65`, `18e67f8`):

- ✅ C1: sigma_block² × tau_years unit mismatch in new_strategies.cpp
- ✅ C2: Realized PnL in mojos-squared (engine.cpp)
- ✅ C3: regime.cpp in CMakeLists.txt (false positive)
- ✅ C4: Loss manager receives half-spread not full spread
- ✅ R1: quote_valid never set true after compute_quotes
- ✅ H1-H2: Steps 5/6 quote_valid guards
- ✅ H3: Null check on fill_pair_cfg
- ✅ H4: Stale regime default (spread_mult=0)
- ✅ H5: vol_estimators_ operator[] null deref
- ✅ H6: peak_pnl = total_pnl (drawdown alert broken)
- ✅ H7: Database NULL column dereference
- ✅ H8: Duplicate curl_global_init in dexie_client
- ✅ H9: Hardcoded XCH/USD 2.70 → named constant
- ✅ M1-M36: Various medium fixes (value-init, member ordering, competition floor dedup, atomic no_loss_constraint, nested lock elimination, soft>hard validation, cancel_all selective removal, loss_manager noexcept, drift_analyzer shared lock + config by-value return, sqlite3_bind_int64, PnL insert_trade mutex, Prometheus label cardinality bound, config uint32 parse, Position::add returns bool, compiler warnings, FORTIFY_SOURCE guard, PIE flags, MSVC /WX, MC flow_seed domain separation)

---

## Summary Statistics

**Last verification:** 2026-03-25 (implementation pass: T1-12, T3-06, T6-01..07)

| Tier | Total | Done | Partial | Open | Description |
|------|-------|------|---------|------|-------------|
| **Tier 1 (Critical)** | 14 | 14 | 0 | 0 | Must fix before live trading |
| **Tier 2 (High)** | 20 | 20 | 0 | 0 | Must fix before paper trading |
| **Tier 3 (Medium)** | 35 | 30 | 2 | 3 | Quality, robustness, correctness |
| **Tier 4 (Low/Enhancement)** | 27 | 0 | 2 | 25 | Improvements and strategic features |
| **Tier 5 (Counter-Research)** | 15 | 6 | 0 | 9 | Academic challenges to cited literature |
| **Tier 6 (New 2026-03-25)** | 10 | 5 | 0 | 5 | Build, packaging, config, code quality |
| **Total** | **121** | **75** | **4** | **42** | |
| **Already Fixed (pre-TODO)** | ~50 | — | — | — | From Claude Code 3-pass cycle |

### Blocking Items for Live Trading
1. **T1-10** — TierQuote.size documentation/typing gap (functional but undocumented)
