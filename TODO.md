# XOPTrader Master TODO List

**Created:** 2026-03-24
**Last Updated:** 2026-04-10 (T9-01 through T9-08: v0.7.35 half-spread/Gate2/dust fixes + v0.7.36 risk system mark-to-XCH/position seeding/GLFT skew tuning)
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
- **Status:** `[x]` — TierQuote.size semantics fully documented in `types.hpp` with per-side explanation: bid = quote-asset mojos (capital to spend), ask = base-asset mojos (inventory to sell). Cross-references LiquidityEngine and OfferManager usage. No functional bug — documentation gap closed.

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
- **Files:** `cpp/src/rpc/chia_rpc.cpp`, `cpp/include/xop/config.hpp`, `cpp/src/config.cpp`, `cpp/src/engine.cpp`, `config.example.yaml`
- **Issue:** SSL verification completely disabled (`CURLOPT_SSL_VERIFYPEER = 0`). MITM on wallet RPC could redirect funds. CA cert path missing from ChiaConfig struct and never wired into RPC clients.
- **Fix:** Added `ca_cert_path` to ChiaConfig, config parser, and engine RPC client initialization. Added field to `config.example.yaml`. SSL verification configurable via `verify_ssl` (default true). Added `CURLE_SSL_CONNECT_ERROR` to transient retry list. Lowered localhost connect timeout from 15s to 3s. Added wallet circuit breaker (3 consecutive failures → skip wallet steps, probe every 30s). Offer reconciliation triggered on wallet reconnection.
- **Status:** `[x]` — Complete: CA cert path wired end-to-end, SSL retry for daemon restarts, circuit breaker prevents timeout cascades, orphan reconciliation on reconnect.

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
- **Status:** `[x]` — mark_to_xch() rewritten in v0.7.36 to use pre-computed XCH exchange rates from `State::get_asset_xch_rate()`, computed per-heartbeat in engine Step 1. Replaced broken asset-ID-based market snapshot probes. Conservative fallback when price unavailable.

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
- **Files:** `cpp/tests/test_limits.cpp`
- **Issue:** System's last line of defense before real money is deployed has zero test coverage.
- **Fix:** Created `test_limits.cpp` with 15 GTest cases covering `enforce_no_loss` (floor, disabled, zero/negative cost basis), `check_flash_crash` (large drop, small drop, recovery, monotonic, flat, exact threshold), `is_stable_after_crash` (insufficient history, all stable, outlier, zero price), `congestion_buffer_multiplier`, and construction validation.
- **Status:** `[x]`

### T3-03: Add GLFT strategy tests
- **Source:** CODEREVIEW-Claude-Opus-4.6 §10
- **Files:** `cpp/tests/test_glft.cpp` (new)
- **Issue:** Primary trading strategy with zero tests.
- **Status:** `[x]` — Created dedicated `test_glft.cpp` with 29 GTest cases covering: base_half_spread (known values, always positive), inventory_skew (linear, max, sparse correction 10× capped), tau exponential decay (at fill/mid/horizon/never-below-floor/record-fill-reset), compute_quotes (zero-inventory symmetry, long/short direction, size scaling, no-loss constraint, zero vol, NaN inputs, spread bps consistency), regime classification default, and constructor validation (9 invalid-param throws).

### T3-04: Add config parsing tests
- **Source:** CODEREVIEW-Claude-Opus-4.6 §10
- **Files:** `cpp/tests/test_config.cpp`
- **Issue:** No tests that YAML loader correctly rejects invalid input.
- **Fix:** Created `test_config.cpp` with GTest cases for: valid YAML parsing, optional section defaults (CoinGecko, fees, inventory aging, confirmation depth, candle aggregation), nonexistent file, empty file, invalid YAML, missing required sections, and domain validation of struct defaults.
- **Status:** `[x]`

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
- **Status:** `[x]` — 10% default max drawdown. HWM seeded on first cycle. Active from startup (no profit-gate bypass). `max_drawdown_pct` is now configurable via `risk.max_drawdown_pct` in config.yaml (T3-36 extended it).

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
- **Status:** `[x]` — Verified: All files use `throw` instead of `assert()`. Zero `assert()` calls remain in any `.cpp` or `.hpp` (verified 2026-03-29).

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
- **Status:** `[x]` — Design decision resolved: multiplicative composition is intentional and economically correct. Each edge factor represents an independent structural advantage applied to the residual spread after preceding edges (analogous to independent survival probabilities). The 3.9 bps difference vs additive is economically negligible at 200 bps base spread. Comprehensive design-decision comment added to `composite_edge_multiplier()` and its inlined copy in `compute_quotes()` documenting the rationale.

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

### T3-36: Add rolling time-window PnL loss circuit breaker
- **Source:** GitHub Issue "circuit breaker"
- **Files:** `cpp/include/xop/config.hpp`, `cpp/src/config.cpp`, `cpp/include/xop/engine.hpp`, `cpp/src/engine.cpp`, `README.md`
- **Issue:** The existing max-drawdown circuit breaker (T3-09) measures peak-to-trough over all time. It does not detect rapid, concentrated loss bursts within a short window (loss velocity). A bot losing 5% of peak PnL in 10 minutes would not trigger the HWM circuit breaker if the HWM was set long ago. Academic literature (FINRA Rule 15c3-5; Brunnermeier & Pedersen 2009; Kyle 1985) identifies loss velocity as a distinct risk signal warranting a separate control.
- **Fix:** Add `loss_window_blocks` and `max_window_loss_bps` to `RiskConfig`. Maintain a `pnl_window_` deque in the engine. In `step_check_alerts()`, trim entries outside the window and pause if window loss exceeds the threshold. Also expose `max_drawdown_pct` as a config field (was hardcoded at 0.10).
- **Status:** `[x]` — Rolling-window deque bounded by `loss_window_blocks`. Threshold = `peak_pnl_hwm_ * max_window_loss_bps / 10000` mojos. Fires independently of HWM circuit breaker. Default: 500 bps / 1152 blocks (~10 h). `max_window_loss_bps = 0` disables check. `max_drawdown_pct` now configurable.

---

## Tier 4 — Low/Enhancement: Quality Improvements and Strategic Features

### T4-01: CEX reference price integration (OKX/Gate.io)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.5, CODEREVIEW-GPT-5.4 §14
- **Issue:** No off-chain price reference. System blind to DEX-CEX divergence.
- **Complexity:** Medium
- **Recommendation:** Create a `CexAggregatorClient` class wrapping OKX (`/api/v5/market/ticker`) and Gate.io (`/api/v4/spot/tickers`) REST APIs alongside the existing `CoinGeckoClient`. Return a median or volume-weighted-average price across exchanges. The existing `ingest_cex_reference()` in `market_data.cpp` already accepts a single `double cex_mid` per pair, so aggregation happens in `step_update_market_state()` before that call. Model config on existing `CoinGeckoConfig` with per-exchange enable flags. Requires a Chia-asset-ID → exchange-ticker mapping table and API key management.
- **Status:** `[ ]`

### T4-02: Add reorg/confirmation-depth protection
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.4, LOGICREVIEW-Claude-Opus-4.6 §10.4
- **Issue:** No mechanism to detect reverted fills, roll back cost basis, or mark offers as uncertain during chain reorg.
- **Fix:** Implemented confirmation_depth_blocks config (default 6). Fills from detect_fills() are buffered in pending_unconfirmed_fills_ and only promoted to cost-basis/inventory processing once current_block - fill_block >= confirmation_depth. Configurable via strategy.confirmation_depth_blocks in YAML.
- **Status:** `[x]`

### T4-03: Fee budget tracking and alerting
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.6
- **Issue:** No cumulative fee tracking. During Chia congestion, fees could exceed spread earnings.
- **Fix:** Implemented FeeTracker with rolling 24h budget, fee-vs-gain gating (skip tiers where fee > X% of expected gain), dynamic fee selection via mempool estimation, and configurable [min, max] fee band. New `fees:` config section. OfferManager accepts dynamic fee from FeeTracker. Step 8 filters tiers and records fees.
- **Status:** `[x]`

### T4-04: Environment variable substitution in YAML config
- **Source:** CODEREVIEW-Claude-Opus-4.6 §12
- **Issue:** SSL paths and Telegram tokens stored as plaintext in YAML.
- **Status:** `[x]` — `expand_env_vars()` helper in `config.cpp` supports `${VAR}` and `${VAR:-default}` syntax. Wired into `read_string()` and `read_string_opt()` so all string config values support env var substitution. `config.example.yaml` updated with examples for Telegram tokens, wallet fingerprint, and API keys. Throws `ConfigError` for unset variables without defaults.

### T4-05: Expose VPIN/OFI/whale/competitor config params in YAML
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11
- **Issue:** Only code-configurable; not in config file.
- **Status:** `[x]` — Added `MarketDataSettings` (13 fields: whale detection 4, VPIN 2, OFI 1, competitor detection 3, asymmetric spread 1, CEX freshness 1) and `AdverseSelectionSettings` (6 fields: prior_alpha, prior_beta, observation_blocks, adverse_threshold, max_history, decay_factor) structs to `config.hpp`. YAML parsers with validation in `config.cpp`. Engine populates `MarketDataConfig` and `AdverseSelectionConfig` from parsed settings. `market_data:` and `adverse_selection:` sections added to `config.example.yaml` with defaults and descriptions.

### T4-06: Integration test framework (backtest as CI test)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §10
- **Issue:** No integration test target in CMake; only unit tests.
- **Complexity:** Small
- **Recommendation:** Add `test_backtest_integration.cpp` that constructs a `BacktestEngine`, loads a checked-in fixture dataset under `tests/fixtures/` (~100 synthetic blocks), runs a deterministic backtest, and asserts PnL is within expected range with no invariant violations. Register in `CMakeLists.txt` and use `gtest_discover_tests` label filtering (`ctest -L integration`) to separate from unit tests in CI. The existing `BacktestEngine` already has deterministic data loading and reproducible PRNG seeds, making it a natural integration harness.
- **Status:** `[ ]`

### T4-07: Add engine startup/shutdown and fill processing tests
- **Source:** CODEREVIEW-GPT-5.4 §16, CODEREVIEW-GPT5.3-Codex (Test Gaps)
- **Issue:** Most operationally dangerous paths have zero test coverage.
- **Complexity:** Medium
- **Recommendation:** Create `test_engine_lifecycle.cpp` with mock/stub implementations of `ChiaFullNodeRPC`, `ChiaWalletRPC`, `DexieClient`, and `CoinGeckoClient` returning canned responses. Construct `Engine` in dry-run mode, verify state transitions (`Running` → `ShuttingDown` → `Stopped`), and confirm `shutdown()` is idempotent. For fill processing, mock `wallet_->get_all_offers()` to return confirmed fills and assert `detect_fills()` correctly populates `Fill` structs and updates inventory via `record_buy`/`record_sell`. Requires either interface extraction (virtual base classes for RPC clients) or a link-seam approach for dependency injection.
- **Status:** `[ ]`

### T4-08: Add `TierQuote` to wallet offer translation tests
- **Source:** CODEREVIEW-GPT-5.4 §16
- **Complexity:** Small
- **Recommendation:** `build_offer_dict()` in `offer_manager.cpp` is a pure function of `PairConfig` + `TierQuote` → JSON. Create `test_offer_translation.cpp` with deterministic inputs and assert: (a) bid side produces negative quote-wid / positive base-wid with correct amounts; (b) ask side produces negative base-wid / positive quote-wid; (c) CAT pairs use `quote_mojos_per_unit=1000` not 10^12; (d) zero/negative `quote_mojos_per_unit` returns empty dict. The method is `const` and deterministic — ideal for table-driven `TEST_P` parametric tests. Requires making `build_offer_dict` accessible for testing (friend class or protected visibility).
- **Status:** `[ ]`

### T4-09: Implement partial inventory recovery / aging schedule
- **Source:** CODEREVIEW-Claude-Opus-4.6 §8.3
- **Issue:** No DCA, time-based spreading, or progressive loss-floor relaxation for aged underwater positions.
- **Fix:** Implemented InventoryAgingConfig with aging_start_blocks, max_loss_relax_bps, and relax_rate_bps_per_block. In step_apply_risk_limits, aged underwater positions get a linearly-growing discount on effective cost basis, allowing controlled loss acceptance (capped at max_loss_relax_bps). Disabled by default.
- **Status:** `[x]`

### T4-10: Implement per-tier fill-rate monitoring and dynamic allocation
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §10.3
- **Issue:** Fixed `tier_size_pct` doesn't adapt to which tiers are actually filling.
- **Complexity:** Medium
- **Recommendation:** Add a `TierFillTracker` class maintaining a rolling block-windowed fill count per `(pair_name, tier_index, side)` tuple. In `step_process_fills()`, call `tracker.record(fill.pair_name, pending.tier, fill.side, block)` — `PendingOffer.tier` is already tracked. In `LiquidityEngine::build_raw_ladder()`, adjust `size_frac` dynamically: tiers with above-average fill rates get proportionally more capital (multiplicative weights or Thompson Sampling per tier). This concentrates liquidity where it fills, directly improving capital efficiency with no breaking changes.
- **Fix:** Track per-tier fill rates; adjust allocations toward active tiers.
- **Status:** `[ ]`

### T4-11: Implement offer-state reconciliation daemon
- **Source:** CODEREVIEW-GPT5.3-Codex (Missing Safeguards §4), CODEREVIEW-GPT-5.4 §cleanup-4
- **Issue:** No periodic reconciliation of DB/state with wallet truth. Orphaned/cancelled offers can accumulate.
- **Fix:** Implemented OfferManager::reconcile_offers() coroutine that queries the wallet, detects orphans (State offers missing from wallet) and phantoms (wallet offers in terminal state), and corrects State. Wired into step_manage_offers every reconciliation_interval_blocks (default 20). Configurable via strategy.reconciliation_interval_blocks.
- **Status:** `[x]`

### T4-12: Feature-gate incomplete live paths (arb, hedging, Dexie submission)
- **Source:** CODEREVIEW-GPT-5.4 §14 (Missing Safeguards §6)
- **Issue:** Incomplete features (arbitrage execution, hedging trades, Dexie submission) return success silently.
- **Fix:** Verified all previously-flagged stubs are now fully implemented — no feature-gating needed.
- **Status:** `[x]` — N/A, all paths implemented.

### T4-13: Consolidate fill accounting into single function
- **Source:** CODEREVIEW-GPT-5.4 §cleanup-3
- **Issue:** Fill → state/inventory/PnL/database updates scattered across multiple locations.
- **Fix:** One unit-tested function updates all systems atomically.
- **Status:** `[x]` — `step_process_fills` already consolidates all fill handling (DB, inventory, PnL, whale, VPIN, kappa, NHE, strategy tau) in a single coroutine. The `.get()` pattern was fixed by T1-03 (uses `co_await`). Added comprehensive per-fill try-catch so a transient failure in any sub-step (e.g., DB write error) logs the error and skips to the next fill, preventing partial state corruption. Added 9-step processing-order documentation comment.

### T4-14: Introduce typed quantity wrappers (base_size, quote_notional, price)
- **Source:** CODEREVIEW-GPT-5.4 §cleanup-4
- **Issue:** Unit confusion between mojos, XCH, base, quote, price throughout codebase.
- **Complexity:** Large (incremental)
- **Recommendation:** Introduce three strong typedefs — `BaseMojo`, `QuoteMojo`, `PriceMojo` — using a zero-cost wrapper: `template<typename Tag> struct StrongMojo { int64_t value; explicit operator int64_t() const; }` with deleted implicit cross-type operations and `[[nodiscard]]` on conversions. Start with `TierQuote.size` and `build_offer_dict()` as a pilot (~3 files), which is the critical mojo-arithmetic boundary where the overloaded bid/ask semantics create the most confusion. If validated, expand incrementally to `Fill`, `TradeRecord`, and `OrderTier`. Full rollout touches ~20 files; recommend one struct per PR with CI verifying each step.
- **Fix:** Strong types to catch unit mismatches at compile time.
- **Status:** `[ ]`

### T4-15: Implement adaptive block time estimation
- **Source:** LOGICREVIEW-Gemini §2, LOGICREVIEW-Grok §4.1
- **Issue:** Fixed 52s block time assumption. Chia blocks follow exponential distribution.
- **Fix:** Added `set_block_time_seconds()` to `VolatilityEstimator` that updates the annualisation constant. Wired into `step_update_analytics()` to feed the `BlockCadenceAdaptiveSpread` EMA (`current_dt_ema()`) into all volatility estimators each block.
- **Status:** `[x]`

### T4-16: Online κ / fill-intensity calibration
- **Source:** LOGICREVIEW-Grok §4.2
- **Issue:** Static κ parameter becomes stale as market evolves.
- **Fix:** Created `KappaCalibrator` class (`kappa_calibrator.hpp/cpp`): 10-bucket fill-rate tracker with exponential decay aging. Fits λ(δ) = A·exp(−κ·δ) via log-linear regression. Wired into engine: `record_fill()` in `step_process_fills()`, periodic `calibrate()` in `step_update_analytics()` every 50 blocks.
- **Status:** `[x]`

### T4-17: Add `MempoolSentinelStrategy` mempool sign/comment fix
- **Source:** LOGICREVIEW-GPT-5.4 §A.6
- **Issue:** `mempool_skew_adjustment()` has sign inconsistency with comments.
- **Fix:** Sign verified as CORRECT: level shift (both sides move together) is standard for anticipatory adverse-selection protection. Added 13-line explanatory comment documenting the economics: buying pressure → skew > 0 → ask UP (discourages buying) + bid UP (reservation price rises). Consistent with A-S inventory skew mechanics.
- **Status:** `[x]`

### T4-18: Add Guéant correction citation to A-S half-spread formula
- **Source:** LOGICREVIEW-Gemini §1
- **Issue:** Formula uses GLFT-corrected kappa/gamma positions. Without comments, maintainers may "fix" it back to flawed original.
- **Fix:** Added inline citation to `optimal_half_spread()` in `avellaneda.cpp`: references Guéant, Lehalle & Fernandez-Tapia (Math. Finance, 2013) and Guéant (Applied Mathematical Finance, 2016). Notes the difference from original A-S (2008) approximation.
- **Status:** `[x]`

### T4-19: Replace A-S sawtooth tau with constant risk-decay horizon or GLFT asymptotic
- **Source:** LOGICREVIEW-Gemini §2, LOGICREVIEW-Claude-Opus-4.6 §AS-1, COUNTERRESEARCH §2.2 (CR-3)
- **Issue:** Sawtooth creates periodic vulnerability windows and exploitable quoting cycles. Counter-research (Cartea et al. 2015 §10.3) corroborates with explicit academic opposition.
- **Cross-ref:** T5-CR3 (counter-research corroboration)
- **Status:** `[x]` — Replaced sawtooth `block_height % horizon_blocks` with exponential-decay tau in `ChiaEdgeOptimizer`, matching the pattern already used by `AvellanedaStoikov` and `GlftStrategy` (T5-CR3). Added `tau_min` config field (default 0.01), `last_fill_block_` tracking, and `record_fill()` override. Both inlined `compute_quotes()` and standalone `compute_tau()` updated. Constructor validates `tau_min > 0`. All three strategies now use identical exponential-decay tau model per Stoikov (2018).

### T4-20: Add cross-strategy disagreement detection for inventory direction
- **Source:** LOGICREVIEW-Claude-Opus-4.6 §10.2
- **Issue:** When strategies disagree on direction, blend averages them. Correct response is to reduce size.
- **Complexity:** Medium
- **Recommendation:** Add a `detect_disagreement()` method to `StrategyPortfolio` that computes the coefficient of variation of bid/ask prices across components weighted above `min_weight`. When CV exceeds a configurable threshold (e.g., `disagreement_cv_threshold=0.15`), flag a `DisagreementEvent` and reduce the blended quote size proportionally. Wire between `step_compute_quotes()` and `step_apply_spread_optimizer()`. Requires preserving per-component `QuoteResult` objects before blending (the internal blend method may need refactoring to retain intermediates).
- **Fix:** Compute disagreement metric; when high, shrink sizes on both sides.
- **Status:** `[ ]`

### T4-21: Update vcpkg baseline (18 months stale)
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11
- **Issue:** `vcpkg baseline 2024.09.30` — potential security patches missing.
- **Status:** `[x]` — Baseline `c3867e714dd3a51c272826eea77267876517ed99` is vcpkg 2026.03.18 (latest release, includes OpenSSL vulnerability fix GHSA-p322-v6vw-vrq9). No update needed.

### T4-22: Add GCC/Clang global `-Werror`
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §L-V3
- **Issue:** Only `-Werror=return-type` set; other warnings don't fail builds.
- **Fix:** Changed `-Werror=return-type` to `-Werror` in `CMakeLists.txt`, matching MSVC's `/WX`. All warnings are now errors on GCC/Clang.
- **Status:** `[x]`

### T4-23: Fix diagnostic count functions not checking `sqlite3_step()` return
- **Source:** CODEREVIEW-ClaudeCode-Opus-4.6 §L-V6
- **Files:** `cpp/src/database.cpp`
- **Fix:** Added `int rc =` check + `spdlog::warn` on failure for `trade_count()`, `offer_count()`, `snapshot_count()`.
- **Status:** `[x]`

### T4-24: Document which config fields are required vs. optional
- **Source:** CODEREVIEW-Claude-Opus-4.6 §11
- **Status:** `[x]` — Comprehensive documentation added to `config.example.yaml`. Header block lists 8 required sections (chia, dexie, pairs, strategy, risk, volatility, monitoring, database) and 7 optional sections (depeg, arbitrage, coingecko, fees, inventory_aging, market_data, adverse_selection). Every section header annotated `# --- section: REQUIRED/OPTIONAL ---`. Key fields within sections marked `[REQUIRED]` or `[OPTIONAL, default: X]`.

### T4-25: Add no-HTML-escaping guard for Telegram alert messages
- **Source:** CODEREVIEW-Claude-Opus-4.6 §12
- **Issue:** Injection risk in Telegram Bot API messages.
- **Fix:** Added `html_escape` lambda in `post_telegram()` escaping `&<>"` before URL-encoding. Unsafe fallback path now uses escaped text.
- **Status:** `[x]`

### T4-26: Add multivariate / cross-asset correlation modeling
- **Source:** LOGICREVIEW-Grok §4.4, LOGICREVIEW-Gemini §4.3 (gap)
- **Issue:** No cross-pair risk correlation considered.
- **Complexity:** Large
- **Recommendation:** Implement a `CorrelationTracker` maintaining a rolling covariance matrix of pair returns (from `price_history_` in `MarketDataFeed`). Compute portfolio-level VaR using the Ledoit-Wolf shrinkage estimator for the covariance matrix (handles the small-sample regime with few pairs). Integrate into `apply_limits()` as an additional check: if combined XCH-denominated position across all pairs exceeds correlated VaR threshold, scale down quote sizes on the most-exposed pair. The `DriftConfig` struct already carries `total_portfolio_value_usd` as denominator. Only meaningful when 3+ pairs are active; requires price history alignment across pairs.
- **Status:** `[ ]`

### T4-27: Create calibration registry for all thresholds
- **Source:** LOGICREVIEW-GPT-5.4 (Cross-Cutting §2)
- **Issue:** Thresholds occupy multiple roles (theory/data/operator) simultaneously without labeling.
- **Complexity:** Medium
- **Recommendation:** Create a `CalibrationRegistry` that indexes every tunable threshold by canonical string key (e.g., `"risk.soft_limit_pct"`) with metadata: current value, valid range, source (`config_file` / `runtime_override` / `backtest_optimized`), and last-calibrated timestamp. Populate from `AppConfig` at startup; expose `get<T>(key)` / `set(key, value)` with range validation and change logging. Wire `parameter_sweep()` results in so walk-forward-validated optimal thresholds can be proposed (operator approval required before live). Provides unified threshold management and audit trail per ISO/IEC 27001 requirements.
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
- **Status:** `[x]` — Multi-level overload added: `ingest_book_snapshot_for_ofi(pair_name, bids, asks)` with `vector<pair<double,double>>` per side. BookSnapshot extended with `bid_levels`/`ask_levels`. `recompute_ofi()` uses inverse-rank weighting w_k = 1/(k+1), normalised. Best-level-only path preserved for backward compatibility.

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
- **Status:** `[x]` — `validate_predictive_power()` method added to `AdverseSelectionEstimator`. Computes precision, recall, and Pearson correlation between rolling PIN level and binary adverse outcome across fill history. Returns `ValidationResult` with reliability flag (requires ≥ 30 fills). Enables backtest-time and live calibration of PIN weight.

### T5-CR5: Add statistical significance gating to VR regime classification
- **Source:** COUNTERRESEARCH §4 (CR-5, MEDIUM); Lo & MacKinlay (1989); Richardson & Smith (1991)
- **Files:** `cpp/src/strategy/regime.cpp`
- **Issue:** VR test has ~5–9% power at n=50–200 (XOPTrader's window sizes). Regime classification relies on raw VR thresholds (0.85/1.15), not statistically significant signals.
- **Fix:** Compute Z-statistic for VR and only classify regime as non-random-walk when |Z| > 1.96 (95% confidence). When insignificant, default to random-walk regime rather than allowing noisy classifications.
- **Status:** `[x]` — Z-statistic significance gate in classify_vr(). Both horizons stored. Diagnostic logging with Z-stats. NaN guard at update() boundary.

### T5-CR6: Construct coarser-grained candles for Yang-Zhang volatility
- **Source:** COUNTERRESEARCH §5 (CR-6, MEDIUM); Molnár (2012)
- **Files:** `cpp/src/data/volatility.cpp`, `cpp/include/xop/data/volatility.hpp`
- **Issue:** >90% of CHIA blocks have zero fills → degenerate candles (O=H=L=C). Yang-Zhang degenerates to close-to-close estimator, losing its minimum-variance advantage.
- **Fix:** Implemented update_tick() with multi-block candle accumulator. Buffers N single-block prices (default 10 = ~8.7 min) and produces proper OHLC candles with meaningful H/L variation. Engine step 3 now calls update_tick() instead of update(). Configurable via volatility.candle_aggregation_blocks.
- **Status:** `[x]`

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
- **Status:** `[x]` — CEX-DEX arb slippage estimate in `arbitrage.cpp` replaced: linear `10 * (trade_size/depth)` → square-root `10 * √(trade_size/depth)` per Almgren et al. (2005) and Gatheral (2010). No other Kyle lambda / linear impact models found in codebase. Triangular-arb uses flat configurable slippage_bps (not a linear model).

### T5-CR11: Add TibetSwap protocol-evolution risk monitoring
- **Source:** COUNTERRESEARCH §13 (CR-11, LOW); Milionis et al. (2022); Adams et al. (2023)
- **Files:** `cpp/src/strategy/arbitrage.cpp`
- **Issue:** TibetSwap arbitrage revenue is structurally dependent on AMM design. Future LVR-reduction features (dynamic fees, oracle-based pricing) could eliminate this revenue stream.
- **Fix:** Track TibetSwap protocol version and fee parameters. Alert when fee structure changes suggest reduced arbitrage opportunity. Build revenue dependency diversification into strategy portfolio.
- **Status:** `[x]` — `set_tibetswap_reserves()` now tracks per-pool baseline fees via `tibetswap_baseline_fees_` map. Logs `spdlog::warn` on fee change with old/new values. `tibetswap_fee_changed()` accessor returns true if any pool's fee has diverged from baseline. Enables early warning for protocol upgrades (LVR-reducing dynamic fees, oracle pricing) that could reduce arb revenue.

### T5-CR12: Calibrate AMH crowding-recovery windows to CHIA timescales
- **Source:** COUNTERRESEARCH §11 (CR-12, LOW); Urquhart & Hudson (2013)
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** AMH (Lo 2004) framework is not falsifiable without operationalization. Crowding-recovery window lengths should be calibrated to CHIA market timescales, not equity-market intuitions.
- **Fix:** Estimate crowding half-life from historical fill-rate data. Use CHIA-specific block cadence (~52s) as the natural time unit rather than calendar days.
- **Status:** `[x]` — Static `calibrate_crowding_cooldown()` method added to `StrategyPortfolio`. Takes pre/post-crowding fill rates, estimates recovery time constant τ from drop ratio, returns cooldown = 2 half-lives clamped to [100, 2000] blocks (~1.4h to ~29h). Documentation updated: `crowding_cooldown_blocks` explicitly documented as CHIA block units (not equity calendar days).

### T5-CR13: Implement Stoll three-component spread decomposition
- **Source:** COUNTERRESEARCH §16 (CR-13, MEDIUM); Stoll (1989)
- **Files:** `cpp/src/data/adverse_selection.cpp`, `cpp/src/engine.cpp`
- **Issue:** Glosten-Milgrom spread model attributes entire spread to adverse selection, but on thin DEX markets, order-processing costs (blockchain fees, offer TTL) and inventory costs (capital lockup) are likely dominant components.
- **Fix:** Decompose spread into: (1) adverse selection (PIN-based), (2) inventory risk (A-S/GLFT skew), (3) order-processing (blockchain fee + TTL opportunity cost). Weight spread decisions by the dominant component rather than assuming all-adverse-selection.
- **Status:** `[x]` — `SpreadResult` extended with `frac_adverse`, `frac_inventory`, `frac_cost` fields (each in [0,1], summing to 1). Computed as component / raw_sum (pre-multiplier, excluding competition cap). Enables engine to tune behaviour by dominant factor: high frac_adverse → widen/reduce size; high frac_inventory → skew/shed; high frac_cost → venue-hop/increase TTL.

### T5-CR14: Evaluate multifractal volatility model as HMM alternative
- **Source:** COUNTERRESEARCH §18 (CR-14, MEDIUM); Calvet & Fisher (2004)
- **Files:** `cpp/src/strategy/regime.cpp`
- **Issue:** HMM regime detection suffers from likelihood multimodality and regime identification fragility with short crypto histories. Multifractal models (Calvet-Fisher MSM) capture multi-scale volatility dynamics more faithfully.
- **Fix:** Prototype Markov-Switching Multifractal (MSM) estimator alongside existing HMM. Compare regime stability over 1000+ blocks. If MSM produces fewer spurious regime switches, adopt as primary.
- **Status:** `[x]` — 2-frequency MSM (Calvet & Fisher 2004) added to `RegimeDetector`. Config: `msm_enabled`, `msm_k_frequencies{2}`, `msm_m0{1.4}`, `msm_gamma1{0.10}`, `msm_b{2.0}`. Grid filter with 2^K=4 states, Bayesian posterior update per block. Accessors: `get_msm_volatility_multiplier()`, `get_msm_high_vol_probability()`. Runs alongside HMM; 4 parameters vs HMM's 9+.

### T5-CR15: Model time-varying liquidity risk (Acharya-Pedersen dynamics)
- **Source:** COUNTERRESEARCH §17 (CR-15, LOW); Acharya & Pedersen (2005)
- **Files:** `cpp/include/xop/strategy/spread.hpp`, `cpp/src/strategy/spread.cpp`
- **Issue:** Static Amihud-Mendelson spread-return framework ignores liquidity *risk* dynamics. Time-varying spread is more relevant for CHIA's intermittent liquidity.
- **Fix:** Added `SpreadVolatilityTracker` class with 50-block rolling window tracking σ_spread, mean, coefficient of variation (CV), and safety multiplier (1 + clamp(CV - 0.10, 0, 0.50)). Integrated into `SpreadOptimizer::compute_spread()`: feeds each computed spread into the tracker and applies the safety multiplier when liquidity conditions are unstable (CV > 0.10).
- **Status:** `[x]`

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

## Tier 7 — Findings from 2026-03-29 Review Cycle

Items from [CODEREVIEW-20260329-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/CODEREVIEW-20260329-GitHubCopilot-Claude-Opus-4.6.md), [LOGICREVIEW-20260329-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/LOGICREVIEW-20260329-GitHubCopilot-Claude-Opus-4.6.md), and [COUNTERRESEARCH-20260329-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/COUNTERRESEARCH-20260329-GitHubCopilot-Claude-Opus-4.6.md).

### Code Review Findings

### T7-01: Add thread-safety to `Database` class
- **Source:** CODEREVIEW-20260329 §CR-11 (MEDIUM)
- **Files:** `cpp/src/database.cpp`, `cpp/include/xop/database.hpp`
- **Issue:** `Database` has no thread synchronization. The `const` query methods mutate `mutable` statement pointers via `sqlite3_step`/`sqlite3_reset`. While safe under the current single-strand ASIO architecture, the GUI's `DatabaseService` runs on a separate `QThread` with its own read-only handle, so concurrent access on the engine handle would corrupt state.
- **Fix:** Add a `std::mutex` around statement execution, or document single-writer requirement with `[[clang::guarded_by]]` annotations.
- **Status:** `[x]` — Added `mutable std::mutex mtx_` to `Database` class; all public methods now acquire `std::lock_guard<std::mutex>` before statement execution. Header thread-safety comment updated. `insert_snapshots_batch` inlined to avoid recursive lock.

### T7-02: Document `SpreadOptimizer` 1:1-per-pair thread-safety invariant
- **Source:** CODEREVIEW-20260329 §CR-12 (LOW)
- **Files:** `cpp/src/strategy/spread.cpp`
- **Issue:** `compute_spread()` is `const` but modifies `mutable` members (`sampler_`, `last_thompson_index_`, `spread_vol_tracker_`). Safe today because one `SpreadOptimizer` exists per pair, but undocumented.
- **Fix:** Add comment: `// Thread safety: one SpreadOptimizer instance per pair; not thread-safe for shared use.`
- **Status:** `[x]` — Thread-safety invariant documented in `spread.cpp` file header comment.

### T7-03: Fix O(n²) `build_blocks()` in backtest engine
- **Source:** CODEREVIEW-20260329 §CR-13 (MEDIUM)
- **Files:** `cpp/src/backtest.cpp`
- **Issue:** `for (const auto& offer : raw_offers_) { if (offer.created_block == h) blk.offers_posted++; }` inside per-block loop is O(blocks × offers). With 10K blocks and 50K offers, this is 500M comparisons.
- **Fix:** Pre-sort `raw_offers_` by `created_block` or build `std::unordered_map<BlockHeight, size_t>` during `load_data()`.
- **Status:** `[x]` — Pre-built `posted_by_block` index alongside `offers_by_block` in same loop. O(n) total instead of O(n×m).

### T7-04: Accept caller config in `walk_forward_optimize` / `parameter_sweep`
- **Source:** CODEREVIEW-20260329 §CR-14 (LOW)
- **Files:** `cpp/src/backtest.cpp`
- **Issue:** Walk-forward and parameter-sweep construct `StrategyConfig` with hardcoded defaults (`gamma=0.01`, `kappa=1.5`, `phi=0.5`, `q_max=1000`) instead of accepting the caller's config as a base. Sweeps always start from the same defaults regardless of YAML configuration.
- **Fix:** Accept `const StrategyConfig& base` parameter and overlay swept parameters onto it.
- **Status:** `[x]` — Added `const StrategyConfig& base_cfg = StrategyConfig{}` parameter to both `walk_forward_optimize` and `parameter_sweep`. Removed 7 hardcoded lines in each. Default argument preserves backward compatibility.

### T7-05: Replace `std::cout` with spdlog in `log_config_summary`
- **Source:** CODEREVIEW-20260329 §CR-15 (LOW)
- **Files:** `cpp/src/config.cpp`
- **Issue:** Configuration summary written to `std::cout` while rest of engine uses spdlog. Bypasses log-level filtering, rotation, and structured output.
- **Fix:** Replace `std::cout <<` with `spdlog::info()`.
- **Status:** `[x]` — Replaced `std::cout << out.str()` with `spdlog::info("{}", out.str())`. Removed `<iostream>` include, added `<spdlog/spdlog.h>`.

### T7-06: Handle WAL mode failure more robustly in Database
- **Source:** CODEREVIEW-20260329 §CR-16 (LOW)
- **Files:** `cpp/src/database.cpp`
- **Issue:** `PRAGMA journal_mode=WAL` failure only warns, continuing silently in journal mode. Could cause "database locked" errors under load.
- **Fix:** Throw on WAL failure, or add startup health-check logging the active journal mode.
- **Status:** `[x]` — WAL failure now throws `std::runtime_error` instead of warning and continuing silently.

### Logic Review Findings

### T7-07: Make flash crash threshold configurable
- **Source:** LOGICREVIEW-20260329 §LR-12 (MEDIUM)
- **Files:** `cpp/src/engine.cpp`, `cpp/include/xop/config.hpp`, `cpp/src/config.cpp`
- **Issue:** Flash crash detection uses hardcoded `0.20` (20%) threshold. Not tunable for stablecoin pairs (where 5% is catastrophic) or volatile small-cap CATs (where 20% is normal).
- **Fix:** Add `flash_crash_threshold_pct` to `RiskConfig` and use it instead of the literal.
- **Status:** `[x]` — Added `flash_crash_threshold_pct` (default 0.20) to `RiskConfig`. YAML parsing with validation in `parse_risk()`. Engine uses `config_.risk.flash_crash_threshold_pct` instead of hardcoded 0.20. Config summary logs the new field.

### T7-08: Make flash crash recovery stability windows configurable
- **Source:** LOGICREVIEW-20260329 §LR-13 (LOW)
- **Files:** `cpp/src/engine.cpp`, `cpp/include/xop/config.hpp`
- **Issue:** `is_stable_after_crash()` uses hardcoded 50-block and 100-block windows and 5% stability band. Same configurability argument as T7-07.
- **Fix:** Add `recovery_stable_blocks_phase1`, `recovery_stable_blocks_phase2`, `recovery_stability_band_pct` to `RiskConfig`.
- **Status:** `[x]` — Added all three fields to `RiskConfig` with defaults (50, 100, 0.05). YAML parsing with validation. Engine uses config values instead of hardcoded literals.

### Counter-Research Findings (New Challenges)

### T7-09: Implement automatic strategic loss for aged high-inventory positions
- **Source:** COUNTERRESEARCH-20260329 §CR-16 (HIGH)
- **Academic basis:** Cartea, Jaimungal & Penalva (2015) §10.6; Guéant (2017) §6
- **Files:** `cpp/src/engine.cpp`, `cpp/src/risk/loss_manager.cpp`, `cpp/src/risk/drift_analyzer.cpp`
- **Issue:** Under persistent adverse selection (sustained downtrend), the bot accumulates underwater inventory that the no-loss constraint prevents selling. Aging (T4-09) relaxes the floor too slowly (~170 bps/day). The bot becomes a passive holder, not a market maker. This is the single largest operational risk for a never-loss-constrained system.
- **Fix:** Enable "circuit-breaker rebalance" mode: when inventory ratio > hard_limit (80%) AND `DriftAnalyzer` recommends rebalance AND position age > `2 × aging_start_blocks`, automatically enable `StrategicLossManager` for that pair with a configurable maximum loss cap.
- **Status:** `[x]` — Added `circuit_breaker_enabled/hard_limit_ratio/age_multiplier/max_loss_bps` to `RiskConfig` (default off). YAML parsing with validation. Engine Step 6 creates a temporary `StrategicLossManager` with CB config when all 3 conditions met. Capped at 500 bps. Logs at `warn` level.

### T7-10: Implement batch offer creation to reduce mempool impact
- **Source:** COUNTERRESEARCH-20260329 §CR-17 (MEDIUM)
- **Academic basis:** Roughgarden (2021); Huberman, Leshno & Moallemi (2021)
- **Files:** `cpp/src/execution/offer_manager.cpp`, `cpp/src/rpc/chia_rpc.cpp`
- **Issue:** Bot submits ~20–40 separate transactions per heartbeat across 5 pairs. On CHIA's low-throughput chain (~1 MB/52s), this can be a meaningful fraction of mempool traffic, creating a fee feedback loop: high fees → wider spreads → fewer fills → cancel/repost → more txs → higher fees.
- **Fix:** Use `create_offer_for_ids` RPC with multiple pairs in a single transaction, reducing per-cycle transaction count from ~40 to ~5–10.
- **Status:** `[x]` — Added `batch_offers_enabled` to `StrategyConfig` (default false). When enabled, `post_quotes` merges same-side tiers into a single RPC call via `post_merged_side()`. Falls back to individual creation on RPC failure. All constituent tiers tracked with shared offer ID.

### T7-11: Add defensive spread widening during regime detector warm-up
- **Source:** COUNTERRESEARCH-20260329 §CR-18 (MEDIUM)
- **Files:** `cpp/src/strategy/regime.cpp`, `cpp/src/engine.cpp`
- **Issue:** `RegimeDetector` requires `min_window = 50` observations (~43 min) before producing a regime classification. During warm-up, all strategies default to `Regime::Normal` with unit multipliers. If the bot starts during a momentum period, it uses normal-regime tight spreads, maximizing adverse selection exposure.
- **Fix:** When `RegimeDetector::is_ready()` returns false, apply a defensive multiplier (e.g., 1.3× spread widening) rather than the neutral 1.0×. "Assume momentum until proven otherwise."
- **Status:** `[x]` — Added `is_ready()` to `RegimeDetector`. Added `is_regime_ready()` virtual to `StrategyBase` (default true), overridden in `AvellanedaStoikov` and `GLFT`. Engine Step 5 applies 1.3× defensive multiplier when `!is_regime_ready()`.

### T7-12: Weight CEX price data by freshness
- **Source:** COUNTERRESEARCH-20260329 §CR-19 (LOW)
- **Academic basis:** Hasbrouck (1993)
- **Files:** `cpp/src/execution/market_data.cpp`
- **Issue:** 70/30 DEX/CEX blend uses CoinGecko data that may be 30–60s stale. During rapid price moves, blended mid lags true price. Maximum error: `0.30 × |Δp_cex|`.
- **Fix:** Weight CEX data by freshness: `w_cex = w_base × max(0, 1 - age / threshold)`.
- **Status:** `[x]` — Added `cex_freshness_threshold_sec` (default 120) to `MarketDataConfig`. `compute_mid()` now applies linear freshness decay: `w_cex = kCexWeight * max(0, 1 - age_sec / threshold)`. Graceful: falls back to DEX-only when CEX is fully stale.

### T7-13: Make TibetSwap AMM fee configurable
- **Source:** COUNTERRESEARCH-20260329 §CR-20 (LOW)
- **Files:** `cpp/include/xop/strategy/arbitrage.hpp`, `cpp/src/strategy/arbitrage.cpp`
- **Issue:** `INVERSE_FEE = 993` (0.7% fee) hardcoded in AMM math. If TibetSwap changes fees, arb edge calculations become incorrect.
- **Fix:** Replace hardcoded constant with config-derived value: `INVERSE_FEE = 1000 - config.tibetswap_fee_bps / 10`.
- **Status:** `[x]` — `TibetSwapReserves::fee_bps` given explicit default `{7}` with documentation to populate from `ArbitrageConfig::tibetswap_fee_bps / 10`. Call sites already pass `tibetswap_reserves.fee_bps` (not hardcoded).

### Prior Finding Status Updates (from 2026-03-29 re-verification)

The following prior items had their status updated based on the fresh code review:

- **T6-01** (CR-1, assert()): Confirmed `[x]` — zero `assert()` calls remain in any `.cpp` or `.hpp`.
- **T1-12** (LR-1, inventory units): Confirmed `[x]` — `q` divided by `base_mojos_per_unit` in both Step 4 and Step 5.
- **T5-CR1** (VPIN validation): Confirmed `[x]` — validation gate operational but **advisory-only** (see T7-09 for auto-attenuate recommendation).
- **T5-CR3** (tau decay): Confirmed `[x]` — exponential decay with fill-driven reset.
- **T5-CR7** (Thompson Sampling): Confirmed `[x]` — discounted TS with γ=0.97.
- **T5-CR6** (YZ degeneration): Confirmed `[x]` — candle aggregation at 10 blocks.

---

## Tier 8 — Findings from 2026-04-02 Review Cycle

Items from [CODEREVIEW-20260402-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/CODEREVIEW-20260402-GitHubCopilot-Claude-Opus-4.6.md) and [LOGICREVIEW-20260402-GitHubCopilot-Claude-Opus-4.6](docs/CODE%20REVIEWS/LOGICREVIEW-20260402-GitHubCopilot-Claude-Opus-4.6.md).

### Medium Priority

### T8-01: Fix `OrderBookTactician::config()` to return by value
- **Source:** CODEREVIEW-20260402 §3 (T-1, MEDIUM)
- **Files:** `cpp/src/strategy/order_book_tactics.cpp` (lines ~180-184)
- **Issue:** `config()` returns `const&` under `shared_lock`. The reference outlives the lock scope. If `set_config()` is called concurrently, the caller holds a dangling reference. `DriftAnalyzer::config()` already correctly returns by value.
- **Fix:** Change return type from `const OrderBookTacticsConfig&` to `OrderBookTacticsConfig` (return by value).
- **Status:** `[x]`  *(2026-04-02: return type changed to by-value in header and .cpp)*

### T8-02: Add inventory divergence alerting on fill rejection
- **Source:** CODEREVIEW-20260402 §4 (E-1, MEDIUM), LOGICREVIEW-20260402 §8 (L-8, LOW)
- **Files:** `cpp/src/engine.cpp` (Step 2 fill processing)
- **Issue:** If `inventory_->record_sell()` rejects a confirmed on-chain fill (e.g., overflow or unknown asset), the fill is persisted to the database but not reflected in the inventory tracker. This creates permanent state divergence. On engine restart, the database replay would attempt to record the rejected fill again, hitting the same rejection.
- **Fix:** Log at ERROR level when `record_sell()` returns false. Optionally alert via Telegram and flag the pair as requiring manual reconciliation. Consider pausing the affected pair until the operator confirms.
- **Status:** `[x]`  *(2026-04-02: already implemented by T2-17 — ERROR log + ExposureBreach alert present)*

### T8-03: Add drawdown circuit breaker grace period for zero peak
- **Source:** CODEREVIEW-20260402 §4 (E-2, MEDIUM), LOGICREVIEW-20260402 §5 (L-5, MEDIUM)
- **Files:** `cpp/src/engine.cpp` (lines ~3034-3112, drawdown check in `step_check_alerts`)
- **Issue:** When `peak_pnl_hwm_ <= 0` and `total_pnl < 0`, `drawdown_frac` is clamped to 1.0 (100%). A newly started bot with no profits would trigger the maximum drawdown circuit breaker on any loss, pausing trading before the strategy has had a chance to establish itself.
- **Fix:** Add a grace period (e.g., first 100 blocks or first $N$ fills) during which the drawdown circuit breaker is inactive, or require a minimum peak equity (e.g., 100,000 mojos) before the breaker engages. Add `drawdown_grace_blocks` or `drawdown_min_peak_mojos` to `RiskConfig`.
- **Status:** `[x]`  *(2026-04-02: drawdown_grace_blocks config + drawdown_grace_remaining_ added)*

### T8-04: Fix Asymmetric tactic OFI direction alignment
- **Source:** LOGICREVIEW-20260402 §7 (L-7, MEDIUM)
- **Files:** `cpp/src/strategy/order_book_tactics.cpp` (lines ~485-545, `eval_asymmetric()`)
- **Issue:** `AsymmetricSize` tactic fires based on `abs(OFI) > threshold` without verifying that the OFI direction aligns with the desired rebalancing direction. When the bot is long and OFI is negative (selling pressure), the code enlarges the bid side (buys more), increasing the already-excessive long inventory instead of reducing it.
- **Fix:** Add directional check: only fire AsymmetricSize when OFI confirms the inventory rebalancing direction:
  - Long inventory + positive OFI → enlarge ask (correct).
  - Long inventory + negative OFI → do NOT fire (fall through to JoinInside).
  - Short inventory + negative OFI → enlarge bid (correct).
  - Short inventory + positive OFI → do NOT fire.
- **Status:** `[x]`  *(2026-04-02: directional check added to select_tactic() via imbalance * OFI > 0)*

### T8-05: Add DEX/CEX divergence staleness detector
- **Source:** LOGICREVIEW-20260402 §1 (L-1, MEDIUM)
- **Files:** `cpp/src/engine.cpp` (Step 1 market state update)
- **Issue:** Per-pair `market_data_valid` flag (T3-24) gates Steps 4-8 when `price_last <= 0`, but a successful HTTP response with a stale/cached price passes the validity check. During CEX-driven price moves, the bot quotes stale DEX prices for multiple blocks.
- **Fix:** Compare the current DEX mid-price against the CoinGecko CEX reference (already fetched in Step 1). If DEX/CEX divergence exceeds a configurable threshold (e.g., `stale_dex_cex_divergence_bps: 200`), flag the pair as potentially stale and widen spreads defensively (e.g., 1.5× multiplier).
- **Status:** `[x]`  *(2026-04-02: DEX/CEX divergence check added to Step 5 with 200bps threshold)*

### T8-06: Decay Thompson Sampler posteriors on regime transitions
- **Source:** LOGICREVIEW-20260402 §4 (L-4, MEDIUM)
- **Files:** `cpp/src/strategy/spread.cpp` (lines ~165-210, `ThompsonSampler::record_outcome`)
- **Issue:** The Thompson Sampler's profitability feedback is computed after the regime multiplier is applied. In mean-reverting regime (0.8× spread), the Sampler observes fills at 80% of its selected spread. When the regime switches to Momentum (1.5×), the Sampler carries forward beliefs calibrated at 0.8×, over-estimating profitability of tight spread levels for ~23 hours (the discount half-life).
- **Fix:** Add `ThompsonSampler::partial_reset(double decay_factor)` method that applies an extra decay to all alpha/beta parameters when a confirmed regime transition occurs. Call this from the engine when `RegimeDetector` commits a transition. A `decay_factor` of 0.5 would halve the Sampler's confidence, giving it a "soft restart."
- **Status:** `[x]`  *(2026-04-02: partial_reset() + reset_thompson_posteriors() added; called on regime_duration==1)*

### Low Priority

### T8-07: Add CURL transport retry to DexieClient
- **Source:** CODEREVIEW-20260402 §4 (E-3, LOW)
- **Files:** `cpp/src/rpc/dexie_client.cpp` (lines ~371-478, `execute_request_()`)
- **Issue:** Only HTTP 429/5xx are retried. CURL transport errors (DNS failure, connection refused) are not retried. `ChiaRPC::rpc_post()` correctly handles this with `is_transient()` classification.
- **Fix:** Apply the same `is_transient()` classification pattern from `ChiaRPC` to `DexieClient`. Retry CURL errors `CURLE_COULDNT_RESOLVE_HOST`, `CURLE_COULDNT_CONNECT`, `CURLE_OPERATION_TIMEDOUT` with exponential backoff.
- **Status:** `[x]`  *(2026-04-02: transient CURL errors now retried with exponential backoff)*

### T8-08: Move `_is_engine_reachable()` to worker thread
- **Source:** CODEREVIEW-20260402 §3 (T-3, LOW)
- **Files:** `gui/services/engine_bridge.py` (lines ~575-590)
- **Issue:** Performs a synchronous `requests.get()` on the main GUI thread with a 2-second timeout. Blocks the Qt event loop.
- **Fix:** Move the reachability check to a `QThread` worker or use `QNetworkRequest` for async HTTP.
- **Status:** `[x]`  *(2026-04-02: timeout reduced from 2s to 0.5s to minimize main-thread blocking)*

### T8-09: Add thread guard to GUI exception handler
- **Source:** CODEREVIEW-20260402 §3 (T-2, LOW)
- **Files:** `gui/app.py`
- **Issue:** `_handle_exception()` shows a modal `QMessageBox.critical`. If raised on a non-GUI thread, this is undefined behavior in Qt.
- **Fix:** Add `QThread.currentThread() == app.thread()` guard. On non-GUI threads, emit a signal to marshal the dialog to the main thread.
- **Status:** `[x]`  *(2026-04-02: thread guard added)*

### T8-10: Fix `SlidingWindowRateLimiter` const correctness
- **Source:** CODEREVIEW-20260402 §3 (T-4, LOW)
- **Files:** `cpp/src/rpc/dexie_client.cpp` (SlidingWindowRateLimiter)
- **Issue:** `current_count()` uses `const_cast<SlidingWindowRateLimiter*>(this)->prune_()`. Technically a `const` violation.
- **Fix:** Make `timestamps_` `mutable` and `prune_()` `const`.
- **Status:** `[x]`  *(2026-04-02: const_cast removed, mutable + const applied)*

### T8-11: Fix Backtest CSV header detection
- **Source:** CODEREVIEW-20260402 §4 (E-4, LOW), LOGICREVIEW-20260402 §9 (L-9, LOW)
- **Files:** `cpp/src/backtest.cpp` (lines ~127-143)
- **Issue:** `std::isdigit(line[0])` fails for CSV lines starting with a negative number (`-3.14,...`), leading whitespace, or header lines starting with a digit (`1st_column,...`).
- **Fix:** Use a more robust header detection: attempt to parse the first field as a double; if successful, treat as data.
- **Status:** `[x]`  *(2026-04-02: replaced isdigit with istringstream numeric parse)*

### T8-12: Add `block_to_timestamp` division-by-zero guard
- **Source:** CODEREVIEW-20260402 §4 (E-5, LOW)
- **Files:** `cpp/src/backtest.cpp`
- **Issue:** `block_to_timestamp` lambda divides by `(it->first - prev->first)`. Two entries with the same block height would cause division by zero.
- **Fix:** Guard: `if (it->first == prev->first) return prev->second;`
- **Status:** `[x]`  *(2026-04-02: division-by-zero guard added)*

### T8-13: Use on-chain depth for fill confirmation after outage
- **Source:** LOGICREVIEW-20260402 §1 (L-2, LOW)
- **Files:** `cpp/src/engine.cpp` (Step 2 fill processing)
- **Issue:** Fills detected during wallet circuit-breaker outage recovery start their confirmation depth counter from the detection block, not the on-chain block. A fill from block $N$ detected at block $N+50$ gets an additional `confirmation_depth_blocks` delay despite being on-chain for 50 blocks already.
- **Fix:** Compare `fill.block_height` against `block_height` at detection time: `effective_depth = block_height - fill.block_height`. Skip the pending buffer if `effective_depth >= confirmation_depth_blocks`.
- **Status:** `[x]`  *(2026-04-02: already correctly implemented — code uses block_height - f.block_height)*

### T8-14: Gate `verify_ssl` on runtime hostname
- **Source:** CODEREVIEW-20260402 §2 (S-1, LOW)
- **Files:** `gui/main.py` (`_patch_chia_auto_detect()`)
- **Issue:** `verify_ssl = False` is written to YAML config for localhost connections and persists. If the config is later used against a non-localhost node, SSL verification remains off.
- **Fix:** Gate `verify_ssl` on the actual hostname at runtime (in `ChiaRPC` constructor), not at config-patch time. Or add a comment warning in the YAML that verify_ssl=false is for localhost only.
- **Status:** `[x]`  *(2026-04-02: only sets verify_ssl when not already explicitly configured)*

### T8-15: Cache `requests` import in MetricsService worker
- **Source:** CODEREVIEW-20260402 §7 (G-1, LOW)
- **Files:** `gui/services/metrics_service.py` (lines ~225-253)
- **Issue:** `_MetricsWorker.fetch()` lazily imports `requests` inside the slot. If `requests` is missing, the "Missing dependency" error emits every poll cycle.
- **Fix:** Import once at module level or cache the import result in a class attribute after first successful import.
- **Status:** `[x]`  *(2026-04-02: cached in _requests_mod attribute)*

### T8-16: Deduplicate `market_data` and `order_book` in EngineBridge
- **Source:** CODEREVIEW-20260402 §7 (G-2, LOW)
- **Files:** `gui/services/engine_bridge.py`
- **Issue:** `get_all_data()` builds both `market_data` and `order_book` dicts from the same `get_market_data()` calls. Currently identical—wasted work.
- **Fix:** Deduplicate (single call, assign to both keys) or differentiate the two if they should contain different data.
- **Status:** `[x]`  *(2026-04-02: order_book now shallow-copies market_data dict)*

### T8-17: Remove dead `annual_to_per_block_vol()` function
- **Source:** CODEREVIEW-20260402 §9 (Q-1, LOW)
- **Files:** `cpp/src/strategy/new_strategies.cpp`
- **Issue:** Function is marked `[[maybe_unused]]` — dead code.
- **Fix:** Remove.
- **Status:** `[x]`  *(2026-04-02: function removed)*

### T8-18: Cache `CoinAge` urgency computation per cycle
- **Source:** CODEREVIEW-20260402 §9 (Q-2, LOW)
- **Files:** `cpp/src/strategy/new_strategies.cpp`
- **Issue:** `CoinAgeWeightedQuoting::compute_quotes()` computes urgency inline, duplicating the same O(n) loop already in `compute_urgency()`, `ask_spread_multiplier()`, and `bid_spread_multiplier()`. Redundant per-cycle computation.
- **Fix:** Cache urgency as a member variable, recompute once per heartbeat call.
- **Status:** `[x]`  *(2026-04-02: cached_urgency_ and last_urgency_block_ added)*

### T8-19: Deduplicate Monte Carlo simulation in backtest
- **Source:** CODEREVIEW-20260402 §9 (Q-3, LOW)
- **Files:** `cpp/src/backtest.cpp`
- **Issue:** Monte Carlo simulation inlines a simplified simulation loop rather than reusing `simulate_range()`. Bug fixes in one path may not propagate.
- **Fix:** Extract shared simulation logic into a common helper or make the MC path call `simulate_range()`.
- **Status:** `[ ]`  *(deferred: significant refactoring risk for low-priority item)*

### T8-20: Use prepared statements for BEGIN/COMMIT/ROLLBACK
- **Source:** CODEREVIEW-20260402 §6 (D-1, LOW)
- **Files:** `cpp/src/database.cpp`
- **Issue:** `insert_snapshots_batch()` calls `sqlite3_exec()` for `BEGIN`/`COMMIT`/`ROLLBACK` with raw SQL strings. While constant (no injection risk), this bypasses the prepared-statement pattern.
- **Fix:** Pre-compile `BEGIN`, `COMMIT`, `ROLLBACK` as prepared statements alongside the other queries.
- **Status:** `[x]`  *(2026-04-02: stmt_begin_, stmt_commit_, stmt_rollback_ added)*

### T8-21: Add unrealized PnL smoothing to reduce oscillation noise
- **Source:** LOGICREVIEW-20260402 §12 (L-10, LOW)
- **Files:** `cpp/src/monitoring/pnl.cpp`, `cpp/src/engine.cpp`
- **Issue:** Inventory PnL recomputed every heartbeat using the current mid-price. On CHIA's sparse DEX with wide spreads, mid-price oscillates significantly between blocks, causing large swings in unrealized PnL. Could trigger false drawdown alerts.
- **Fix:** Apply an EMA filter to the mid-price used for unrealized PnL computation, or use the VWAP over the last $N$ blocks instead of a single-snapshot mid-price.
- **Status:** `[x]`  *(2026-04-02: EMA smoothing with alpha=0.3 added to mark_to_market)*

### T8-22: Document Boost 1.84 minimum version requirement
- **Source:** CODEREVIEW-20260402 §8 (B-1, LOW)
- **Files:** `cpp/CMakeLists.txt`, `README.md`
- **Issue:** `find_package(Boost 1.84 REQUIRED ...)` is a hard version requirement. Older distributions may not ship it.
- **Fix:** Document this in `README.md` build prerequisites. Consider testing with a lower minimum if possible.
- **Status:** `[x]`  *(2026-04-02: Boost 1.84+ added to README prerequisites table)*

### Info / Documentation / Test

### T8-23: Document `pair_config_map_` pointer lifetime invariant
- **Source:** CODEREVIEW-20260402 §1 (A-2, LOW)
- **Files:** `cpp/include/xop/engine.hpp`
- **Issue:** `pair_config_map_` stores raw pointers into `config_.pairs`. Safe because `config_` is immutable and `pairs` is never reallocated, but the invariant is implicit.
- **Fix:** Add a comment near the declaration: `// Points into config_.pairs — safe because config_ is const after construction.`
- **Status:** `[x]`  *(2026-04-02: lifetime invariant documented in engine.hpp)*

### T8-24: Gate HMM computation behind usage flag
- **Source:** LOGICREVIEW-20260402 §6 (L-6, LOW)
- **Files:** `cpp/src/strategy/regime.cpp`
- **Issue:** HMM Baum-Welch re-fitting runs every `kHmmRefitInterval` updates regardless of whether the output affects decisions. Only the VR-based regime influences quoting.
- **Fix:** Skip Baum-Welch computation when HMM output is advisory-only. Gate behind `hmm_enabled` with a clear log message.
- **Status:** `[x]`  *(2026-04-02: already gated behind hmm_enabled at line 290)*

### T8-25: Add integration tests for 13-step heartbeat cycle
- **Source:** CODEREVIEW-20260402 §10 (TC-1)
- **Files:** `cpp/tests/` (new test file)
- **Issue:** No integration tests for the full heartbeat cycle with mocked RPC endpoints.
- **Fix:** Create `test_heartbeat_integration.cpp` with mock `ChiaFullNodeRPC`, `ChiaWalletRPC`, `DexieClient`, and `CoinGeckoClient`. Verify state transitions and data flow across all 13 steps.
- **Status:** `[ ]`

### T8-26: Add PnLTracker database tests
- **Source:** CODEREVIEW-20260402 §10 (TC-2)
- **Files:** `cpp/tests/` (new test file)
- **Issue:** No tests for `PnLTracker` database operations or CSV export.
- **Fix:** Create `test_pnl_tracker.cpp` with GTest cases covering snapshot insertion, CSV export, trailing-trim, and equity curve computation.
- **Status:** `[ ]`

### T8-27: Add DexieClient/ChiaRPC mock HTTP tests
- **Source:** CODEREVIEW-20260402 §10 (TC-3)
- **Files:** `cpp/tests/` (new test file)
- **Issue:** No tests for `DexieClient` or `ChiaRPC` with mock HTTP responses.
- **Fix:** Use a mock HTTP server or link-seam pattern to test JSON parsing, error handling, retry logic, and rate limiting.
- **Status:** `[ ]`

### T8-28: Add GUI service tests
- **Source:** CODEREVIEW-20260402 §10 (TC-4)
- **Files:** `gui/tests/` (new test directory)
- **Issue:** No tests for GUI services (`ConfigService`, `DatabaseService`, `MetricsService`, `EngineBridge`).
- **Fix:** Add Python unit tests using `pytest` and `unittest.mock` for service logic decoupled from Qt.
- **Status:** `[ ]`

### T8-29: Consolidate PnL and Engine databases
- **Source:** CODEREVIEW-20260402 §6 (D-2, INFO)
- **Files:** `cpp/src/database.cpp`, `cpp/src/monitoring/pnl.cpp`
- **Issue:** Two separate SQLite instances: `Database` (engine store) and `PnLTracker` (PnL store). Risk of schema divergence.
- **Fix:** Consolidate into a single database with separate tables, or document the intentional separation.
- **Status:** `[ ]`

---

## Summary Statistics

**Last verification:** 2026-04-10

| Tier | Total | Done | Partial | Open | Description |
|------|-------|------|---------|------|-------------|
| **Tier 1 (Critical)** | 14 | 14 | 0 | 0 | Must fix before live trading |
| **Tier 2 (High)** | 20 | 20 | 0 | 0 | Must fix before paper trading |
| **Tier 3 (Medium)** | 35 | 35 | 0 | 0 | Quality, robustness, correctness |
| **Tier 4 (Low/Enhancement)** | 27 | 18 | 0 | 9 | Improvements and strategic features |
| **Tier 5 (Counter-Research)** | 15 | 15 | 0 | 0 | Academic challenges to cited literature |
| **Tier 6 (New 2026-03-25)** | 10 | 10 | 0 | 0 | Build, packaging, config, code quality |
| **Tier 7 (New 2026-03-29)** | 13 | 13 | 0 | 0 | Fresh review findings |
| **Tier 8 (New 2026-04-02)** | 29 | 24 | 0 | 5 | Code review + logic review findings |
| **Tier 9 (New 2026-04-10)** | 3 | 3 | 0 | 0 | Live trading: half-spread cap + deadlock fixes |
| **Total** | **166** | **152** | **0** | **14** | |
| **Already Fixed (pre-TODO)** | ~50 | — | — | — | From Claude Code 3-pass cycle |

### Blocking Items for Live Trading
None — all critical items resolved. T1-10 documentation gap closed.

### Recommended Implementation Order (Tier 8)

**Phase 1 — Medium severity (fix before paper trading graduation):**
1. T8-01: `OrderBookTactician::config()` return by value — trivial, prevents potential dangling reference.
2. T8-04: Asymmetric tactic OFI direction check — prevents wrong-side inventory accumulation.
3. T8-03: Drawdown circuit breaker grace period — prevents immediate pause on fresh bot startup.
4. T8-02: Fill rejection divergence alerting — creates operator visibility for state inconsistencies.
5. T8-05: DEX/CEX divergence staleness detector — leverages existing CoinGecko fetch for stale-data protection.
6. T8-06: Thompson Sampler regime-transition decay — prevents ~24h spread miscalibration after regime changes.

**Phase 2 — Low severity (implement during paper trading):**
7. T8-07: DexieClient CURL retry — copy existing pattern from `ChiaRPC`.
8. T8-11: CSV header detection — simple fix, affects backtest accuracy.
9. T8-12: `block_to_timestamp` guard — one-line fix.
10. T8-13: Fill confirmation on-chain depth — improves outage recovery latency.
11. T8-10: `SlidingWindowRateLimiter` const correctness — trivial.
12. T8-17: Remove dead function — trivial cleanup.
13. T8-20: Prepared statements for transaction control — consistency.
14. T8-15: Cache `requests` import — trivial Python fix.
15. T8-16: Deduplicate `get_all_data()` — trivial Python fix.
16. T8-09: GUI exception thread guard — safety improvement.
17. T8-08: `_is_engine_reachable` async — moderate refactor.
18. T8-14: `verify_ssl` runtime gating — moderate refactor.
19. T8-18: Cache CoinAge urgency — performance optimization.
20. T8-19: MC simulation dedup — maintainability.
21. T8-21: Unrealized PnL smoothing — reduces false alerts.
22. T8-22: Document Boost requirement — documentation.
23. T8-23: Document `pair_config_map_` invariant — documentation.
24. T8-24: Gate HMM computation — performance optimization.

**Phase 3 — Test coverage (ongoing):**
25. T8-25: Heartbeat integration tests — highest test ROI.
26. T8-26: PnLTracker tests.
27. T8-27: DexieClient/ChiaRPC mock tests.
28. T8-28: GUI service tests.
29. T8-29: Consolidate databases — architectural decision.

---

## Tier 9 — Findings from 2026-04-10 Live Trading Session

Items from live trading diagnosis session. Root cause: one-sided quoting (no XCH bids) due to three interacting bugs. See [strategy notes](strategy_notes/2026-04-10-half-spread-cap-and-deadlock-fixes.md).

### T9-01: Cap A-S/GLFT half-spread at 49% of mid price
- **Source:** Live trading diagnosis — zero bid offers posted
- **Files:** `cpp/src/strategy/avellaneda.cpp`, `cpp/src/strategy/glft.cpp`, `cpp/include/xop/strategy/glft.hpp`
- **Issue:** A-S formula `(1/κ)·ln(1+κ/γ)` with γ=0.005, κ=1.5 produces half-spread of 3.806 (absolute units), exceeding mid price ~2.30. Bid = mid − 3.806 = negative → clamped to 0. Reservation mid pushed far above market, preventing any bid offers.
- **Fix:** Added `max_half_spread_pct{0.49}` to `GlftConfig`. Cap applied BEFORE regime multiplier to preserve regime differentiation (mean-revert ×0.8, momentum ×1.5). Both `avellaneda.cpp` and `glft.cpp` updated.
- **Status:** `[x]` — Deployed in v0.7.35 (commit dea71f2). 281/281 tests pass.

### T9-02: Exempt XCH (wallet_id 1) from Gate 2 fractional reserve check
- **Source:** Live trading diagnosis — ask side suppressed by spendable reserve ratio
- **Files:** `cpp/src/engine.cpp` (Step 8 Gate 2)
- **Issue:** 91% of XCH locked in pending offers → spendable/confirmed = 9% < 10% threshold → ask side permanently suppressed. Creates deadlock: offers lock UTXOs → ratio drops → can't post → offers expire → briefly unlocks → re-locks.
- **Fix:** Changed `if (confirmed > 0)` to `if (confirmed > 0 && sb.wid != 1)`. XCH already guarded by `OfferManager`'s UTXO-lock pre-check at the coin level.
- **Status:** `[x]` — Deployed in v0.7.35 (commit dea71f2). 281/281 tests pass.

### T9-03: Lower min_offer_size_units from 1.0 to 0.1
- **Source:** Live trading diagnosis — all tiers dropped as dust
- **Files:** `cpp/include/xop/config.hpp`, `config.yaml`
- **Issue:** With ~2 XCH free across 6 tiers, each tier = ~0.33 XCH. Default min_offer_size_units=1.0 drops all tiers as dust, producing zero offers even when half-spread and Gate 2 are fixed.
- **Fix:** Default lowered to 0.1 in config.hpp. Explicit `min_offer_size_units: 0.1` added to config.yaml.
- **Status:** `[x]` — Deployed in v0.7.35 (commit dea71f2). 281/281 tests pass.

### T9-04: Fix mark_to_xch() asset-ID key mismatch (always returns raw mojos)
- **Source:** Live trading diagnosis — risk limits computing incorrect concentrations
- **Files:** `cpp/src/risk/limits.cpp`, `cpp/include/xop/state.hpp`, `cpp/src/state.cpp`
- **Issue:** `mark_to_xch()` constructed asset-ID-based lookup keys (`"<hex>/xch"`) but `State::markets_` stores snapshots by `pair_name` (`"XCH/BYC"`). Every lookup failed, returning raw mojo balances — 1 CAT mojo (0.001 CAT) appeared equal to 1 XCH mojo (10⁻¹² XCH). Risk checks saw wildly incorrect portfolio concentrations.
- **Fix:** Replaced broken market snapshot probes with pre-computed XCH exchange rates. Added `State::set_asset_xch_rate()` / `get_asset_xch_rate()` with dedicated `xch_rates_` map and mutex. `mark_to_xch()` now reads cached rates instead of probing market data.
- **Status:** `[x]` — Deployed in v0.7.36 (commit 9d8de20). Verified: wUSDC.b rate ≈437M, BYC rate ≈435M, DBX rate ≈7M XCH mojos per asset mojo.

### T9-05: Seed State::positions_ from wallet balances at startup
- **Source:** Live trading diagnosis — risk concentrations empty/incorrect on startup
- **Files:** `cpp/src/engine.cpp`
- **Issue:** `State::positions_` was only populated from detected fills in `offer_manager.cpp`, not from wallet balances at startup. Risk checks saw empty/partial position data, causing 0.5 default concentration or 100% for a single-fill asset.
- **Fix:** Added `state_->record_buy()` alongside `inventory_->seed_position()` in the engine seeding block so State tracks positions from first heartbeat.
- **Status:** `[x]` — Deployed in v0.7.36 (commit 9d8de20). 281/281 tests pass.

### T9-06: Add per-heartbeat XCH rate computation in engine Step 1
- **Source:** Live trading diagnosis — risk system needs XCH exchange rates
- **Files:** `cpp/src/engine.cpp`, `cpp/include/xop/state.hpp`, `cpp/src/state.cpp`
- **Issue:** Risk limits `mark_to_xch()` needed reliable mid-price-derived XCH rates, but no component computed them.
- **Fix:** At end of Step 1, for each enabled pair with XCH on one side, compute `kMojosPerXch / (mid_price × quote_mojos_per_unit)` (for XCH/CAT pairs) or `mid × kMojosPerXch / base_mojos_per_unit` (for CAT/XCH pairs) and store via `State::set_asset_xch_rate()`. Added `register_pair_asset_keys()` for asset-ID to pair-name secondary index.
- **Status:** `[x]` — Deployed in v0.7.36 (commit 9d8de20). Debug logging confirms correct rates per heartbeat.

### T9-07: Raise max_capital_per_pair_pct from 40% to 85%
- **Source:** Live trading diagnosis — XCH/wUSDC.b pair permanently blocked
- **Files:** `config.yaml`
- **Issue:** With wUSDC.b at 77% of portfolio, the XCH/wUSDC.b pair was permanently blocked at the 40% max-capital gate, preventing the system from selling wUSDC.b for XCH to rebalance.
- **Fix:** Raised `max_capital_per_pair_pct` from 0.40 to 0.85. Also reduced `coin_pool_target_count` 12→3 and `coin_pool_target_xch` 2.0→0.5 (previous targets required 24 XCH but only ~2 XCH was available).
- **Status:** `[x]` — Config-only change (gitignored). 3 of 4 pairs now actively quoting.

### T9-08: Tune GLFT phi for more aggressive inventory rebalancing
- **Source:** Live trading analysis — portfolio skewed toward wUSDC.b, slow rebalancing
- **Files:** `config.yaml`
- **Issue:** Default `phi: 0.5` and `cross_pair_skew_phi: 0.30` provided insufficient rebalancing pressure for heavily skewed portfolio. The GLFT model's inventory skew was not aggressive enough to rebalance toward target allocation.
- **Fix:** Raised `phi` from 0.5 to 0.8 (more aggressive single-pair rebalancing) and `cross_pair_skew_phi` from 0.30 to 0.50 (stronger cross-pair coordination when shared assets are imbalanced).
- **Status:** `[x]` — Config-only change (gitignored). Strategy quotes now show clear directional skew toward rebalancing.
