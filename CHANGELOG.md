# Changelog

All notable changes to XOPTrader are documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.6.9] — 2026-04-06

### Fixed

- **Offer churn: UTXO liberation age guard**: UTXO liberation now skips offers younger than 5 blocks (~2.5 minutes) instead of immediately cancelling freshly-posted offers. Previously, posting 3 tiers would lock enough XCH to drop spendable below the fee reserve, triggering liberation to cancel all 3 on the very next heartbeat — creating a perpetual create-cancel cycle every ~7 heartbeats. Fresh offers now survive until they can be filled or age out, while truly stale offers are still liberated normally

## [0.6.8] — 2026-04-06

### Added

- **GUI singleton enforcement**: On startup, the GUI now terminates any previously-running GUI and engine processes before launching, ensuring only one GUI and one engine run at a time. Prevents double-posting offers, port conflicts, and wallet RPC contention. Uses WMI process scanning on Windows and `/proc` enumeration on POSIX. Protects the venv launcher parent PID from self-termination

## [0.6.7] — 2026-04-05

### Fixed

- **Fee reserve recovery deadlock**: The v0.6.6 fee reserve fix blocked the engine's `xch_buy_only_mode` recovery path — the offer_manager's independent pre-check used the full 1.0 XCH threshold, overriding the engine's lower 0.01 XCH floor for recovery offers. Added `fee_reserve_override` parameter to `post_quotes()` so the engine can pass the recovery threshold when in `xch_buy_only_mode`, breaking the deadlock cycle

## [0.6.6] — 2026-04-05

### Fixed

- **Fee reserve enforcement for buy-XCH offers**: Removed the `tier_buys_xch` exemption that allowed buy-XCH offers to bypass spendable balance checks. All offers lock XCH UTXOs at creation time regardless of trade direction; 4 guard locations in offer_manager.cpp now enforce the reserve uniformly
- **Hard minimum spendable floor in xch_buy_only mode**: Engine Step 8 now checks `fee_min_spendable_xch` (0.01 XCH) before creating any offer in recovery mode, preventing the last dust UTXO from being locked
- **`xch_spendable_pre` scope**: Moved declaration to outer scope so it is accessible in the pair loop

### Added

- **Startup singleton enforcement**: New `kill_old_instances()` in main.cpp terminates any previously-running `xop_trader` processes before the engine starts, preventing port conflicts and double-posting. Uses `CreateToolhelp32Snapshot` on Windows, `/proc` enumeration on Linux
- **XCH currency symbols on dashboard**: Metric cards and per-pair PnL column now display XCH symbols
- **USD conversion on dashboard**: Live XCH/USD rate derived from XCH/wUSDC.b mid-price; PnL and fee cards show approximate USD equivalent

## [0.6.5] — 2026-04-05

### Added

- **Strategy analytics data collection**: Extended the `snapshots` table with 8 new strategy decision columns persisted every block: `reservation_price_mojos` (A-S reservation price), `half_spread_bps` (optimal half-spread), `kappa` (calibrated fill-intensity decay), `variance_ratio` (Lo-MacKinlay VR statistic), `adverse_rate` (fraction of adverse fills), `s_adverse_bps` / `s_inventory_bps` / `s_cost_bps` (Stoll three-component spread decomposition)
- **Per-tier quote persistence**: New `strategy_quotes` table stores every bid/ask quote at each tier level every block (`block_height`, `pair_name`, `tier`, `side`, `price_mojos`, `size_mojos`), enabling fill probability modelling, tier spacing optimization, and quoted-vs-filled spread analysis
- Forward-compatible ALTER TABLE migrations for existing databases

## [0.6.4] — 2026-04-05

### Added

- **Smart orphan management (CAOE)**: Cost-Aware Orphan Evaluation replaces the blind cancel-all-orphans startup logic with scholarly-grounded per-offer decision making. On startup, each orphaned wallet offer (PENDING_ACCEPT but not in the engine DB) is evaluated against the current Dexie mid-price to determine whether it should be **adopted** (re-tracked), **adopted-stale** (re-tracked but scheduled for immediate refresh), or **cancelled**
  - Academic basis: Guéant, Lehalle & Fernandez-Tapia (2013) "Dealing with the Inventory Risk" — cancel only when expected adverse selection loss exceeds cancellation cost; Gao & Wang (2020) "Optimal market making in the presence of latency" — the zero-offer gap during cancel→repost is the primary adverse selection cost for slow-chain market makers; Aït-Sahalia & Saglam (2017) — stale-quote risk scales with price deviation, remaining lifetime, and offer size
  - Parses wallet trade record `summary` field to extract pair, side, price, and size from each orphan's offered/requested asset maps
  - Fetches current Dexie ticker mid-prices for all enabled pairs during startup reconciliation
  - Computes signed price deviation and determines adverse direction (bid above mid = adverse, ask below mid = adverse)
  - Applies inventory-aware tolerance bonus: orphans that help reduce inventory imbalance get an extra `orphan_inventory_bonus` (default 1%) added to the adverse threshold
  - AdoptStale disposition sets the offer's `created_at_block` near the TTL boundary, triggering an immediate selective refresh on the next heartbeat — no stale offers linger
  - Adopted orphans are persisted to the DB so they survive the next restart without re-evaluation churn
  - New config params: `orphan_adopt_enabled` (default: true), `orphan_adverse_threshold` (default: 0.02 = 2%), `orphan_max_adopt_age_blocks` (default: 120 ≈ 104 min), `orphan_inventory_bonus` (default: 0.01 = 1%)
  - Comprehensive per-orphan logging with disposition, deviation %, and human-readable reasons

## [0.6.3] — 2026-04-05

### Fixed

- **UTXO liberation**: When all XCH spendable is consumed by offer UTXO locking (spendable=0 but confirmed balance is healthy), the engine now cancels the oldest pending offers at the start of Step 8 to free locked UTXOs. Cancels up to 3 offers per heartbeat, re-checking spendable after each. This breaks the deadlock where the engine was frozen for hours with 16+ XCH confirmed but 0 spendable
- **Secure cancel with fee=0**: `emergency_cancel` now attempts `secure=true, fee=0` before falling back to insecure (local-only) cancel. The offer's own locked coins serve as spend bundle inputs, allowing on-chain invalidation without requiring spendable XCH for fees. UTXO liberation uses `prefer_zero_fee` mode to try fee=0 FIRST (before descending fee tiers) to avoid burning spendable XCH on cancel fees
- **Anti-churn + cooldown**: After liberation cancels offers or finds none to cancel, the pair loop is skipped (`co_return`) to prevent creating offers that would get immediately liberated next heartbeat. A 5-heartbeat cooldown further suppresses the pair loop when spendable briefly recovers above reserve (1.0 XCH) but below 2× reserve (2.0 XCH), preventing the post→cancel residual churn cycle that wasted ~0.01 XCH/cycle
- **Recovery mode duplicate block removed**: Fixed corrupted duplicate code block in `step_xch_recovery()` that caused compilation errors

## [0.6.2] — 2026-04-04

### Fixed

- **Recovery oscillation**: Recovery mode now checks `confirmed_wallet_balance` in addition to `spendable_balance`. When confirmed XCH is healthy (>= threshold) but spendable is low due to UTXO locking from our own offers, recovery is no longer triggered — eliminating the 15-second cancel/re-post oscillation cycle
- **BYC trading enabled**: Lowered `min_trading_units` from 10.0 to 2.0 in config, unblocking XCH/BYC and BYC/wUSDC.b pairs that were suppressed because actual balances (BYC=3.0, wUSDC.b=7.5) were below the threshold

## [0.6.1] — 2026-04-04

### Fixed

- **Step 8 pre-gate uses full reserve**: The per-pair XCH balance gate before offer posting now checks against `fee_reserve_xch` (1.0 XCH) instead of `fee_min_spendable_xch` (0.01 XCH). Previously, the second pair could drain the entire reserve through UTXO locking because 0.01 was trivially satisfied
- **Per-offer pre-creation balance check**: `post_quotes()` now verifies XCH spendable >= `fee_reserve_xch` before each individual `create_offer()` call (both batch and non-batch modes), catching mid-cycle UTXO drain that the post-creation guard couldn't prevent
- **Recovery cancels via wallet RPC**: Recovery mode now calls `wallet.cancel_offers(fee=0, secure=false)` directly instead of `offer_mgr->cancel_all()`, which only checked engine state (empty after restart). This cancels ALL wallet offers including those from previous engine instances, and works with 0 XCH spendable
- **Recovery takes with zero fee**: When XCH spendable < 0.001, recovery mode uses fee=0 for `take_offer` calls, breaking the deadlock where taking XCH asks required XCH for fees

## [0.6.0] — 2026-04-04

### Added

- **Dynamic Market Allocator**: New `MarketAllocator` scores each trading pair across 5 dimensions (spread quality, volume, competition, fill rate, triangular arb edge) and dynamically shifts capital allocation toward the most attractive markets
- **Triangular arbitrage detection**: Computes forward and reverse cycle edges across the XCH/wUSDC ↔ XCH/BYC ↔ BYC/wUSDC triangle, net of per-leg fees, and factors the edge into allocation scoring
- **Allocation guardrails**: Configurable min/max per-pair allocation (default 10–50%), hysteresis threshold to prevent oscillation, and EMA smoothing for gradual capital shifts
- **`market_allocator` config section**: Full configuration for weights, intervals, fee assumptions, and allocation bounds
- **XCH Recovery Mode**: Automatic XCH acquisition when spendable balance drops below threshold (default 0.25 XCH). Cancels all offers, gates Steps 7-8, and scans Dexie order books for reasonably-priced XCH asks to take — resuming normal trading once balance recovers above target (default 1.0 XCH). Configurable via `recovery:` section
- **Split fee reserve**: `fee_reserve_xch` (inventory holdback) and `fee_min_spendable_xch` (fee gate) are now separate parameters, allowing fees to draw from the reserve without blocking trading

### Fixed

- **Dexie outlier price filter**: Reject bid/ask data from Dexie that is >10× or <0.1× the CEX reference price, preventing flash-crash triggers from garbage offers (e.g. $979M ask on a $2.38 asset)

## [0.5.9] — 2026-04-04

### Fixed

- **Emergency cancel fee cap**: Previous emergency cancel used `spendable - 1000 mojos` as the fee, burning up to ~1 XCH on a single cancel. Now capped at 2× the dynamic fee (~0.02 XCH max)
- **Emergency cancel fee retry cascade**: When wallet reports insufficient funds, emergency cancel now halves the fee and retries (2× dynamic → 1× → ½ → ¼ → ... → 1 mojo) before falling back to insecure local-only cancel. Enables cancellation even when spendable XCH is far below the configured minimum fee
- **Batch fallback fee reserve guard**: When batch offer creation fails and falls back to per-tier, now checks spendable balance after each successful tier and stops if below reserve

## [0.5.8] — 2026-04-04

### Fixed

- **UTXO-aware fee reserve enforcement**: Previous logical-deduction reserve was ineffective because Chia's UTXO model locks entire coins, not surgical amounts. Step 8 now queries actual XCH spendable balance before each pair and skips if below `fee_reserve_xch`. Post-creation guard in `post_quotes()` re-checks after each offer
- **Emergency cancel on insufficient funds**: When a cancel fails due to insufficient fee balance, automatically retries with a reduced fee (spendable minus dust margin) or falls back to local-only insecure cancel. Applied across all 6 cancel paths: `cancel_stale`, `cancel_all`, `selective_cancel`, asymmetric bid/ask cancel, and `startup_reconcile`
- **Broader fee-error detection**: Cancel error matching now catches both "insufficient funds" and "spendable balance" error strings from the Chia wallet RPC

### Changed

- **10× lower default fees**: Reduced `offer_fee_mojos` from 100M to 10M (0.00001 XCH), `min_fee_mojos` from 50M to 5M, `max_fee_mojos` from 500M to 100M. Based on blockchain research: Chia mempool is typically <1% full, fee estimate for instant inclusion is ~3.5M mojos, and most Dexie offers complete with zero blockchain fee

## [0.5.7] — 2026-04-05

### Added

- **DBX liquidity reward auto-claim**: Every offer submitted to Dexie now includes `claim_rewards: true`, automatically claiming DBX rewards from Dexie's Liquidity Incentive Program. Rewards are batched and sent daily. Toggle via `dexie.claim_rewards` config field or GUI checkbox
- **XCH/DBX pair template**: Added disabled XCH/DBX pair in `config.example.yaml` for users who want to farm high-APR DBX rewards (75–135% APR) on the thin DBX market
- **DBX rewards documentation**: New `docs/dbx-liquidity-rewards.md` covering reward rates, eligibility, claiming methods, and XCH/DBX market analysis

### Changed

- **Cancel-reduction: soft/hard TTL split**: The configured `offer_ttl_blocks` (default 60) is now a "soft" TTL. Offers past soft TTL are only expired if they show ≥0.2% adverse deviation — well-priced old offers stay live. Hard TTL at 2× soft (120 blocks ≈ 104 min) is the absolute safety cap
- **Cancel-reduction: tier-scaled threshold**: Outer tiers now tolerate more price movement before cancellation. Effective threshold scales by tier index: tier 0 → 0.50%, tier 1 → 0.75%, tier 2 → 1.00%, tier 3 → 1.25%. Inner tiers remain tightly monitored
- **Cancel-reduction: minimum age guard**: Offers younger than 3 blocks (~2.6 min) are protected from price-deviation cancellation. The round-trip fee for cancel+recreate exceeds adverse selection risk at small deviations. Crossed-mid cancellation still bypasses this guard
- **Cancel-reduction: all-stale branch fix**: When all tiers are classified as Stale (price deviation), the engine now uses `selective_cancel` instead of `cancel_stale(TTL)`. Previously, price-stale offers within TTL were missed by the TTL-only `cancel_stale` path, risking double-posting

## [0.5.6] — 2026-04-04

### Fixed

- **Critical — CAT offer size inflation (10⁹×)**: Step 6 converted GLFT display-unit sizes to mojos using `kMojosPerXch` (10¹²) instead of `pair_cfg->base_mojos_per_unit` (10³ for CAT tokens). BYC offers posted with ~2.38 billion units instead of ~2.38. Fixed by using correct per-pair mojos denominator
- **Critical — Reservation mid-price runaway**: Avellaneda-Stoikov formula produced absolute half-spread of ~3.35 price units regardless of price level. For BYC at $0.99, reservation_mid inflated to 2.17× market. Added 2% max-deviation clamp on reservation_mid from market mid
- **VPIN fill volume conversion**: Same `kMojosPerXch` vs `base_mojos_per_unit` bug in VPIN fill volume tracking inflated volume metrics by 10⁹× for CAT pairs
- **Steps 7/8 mid-price source**: Tier ladder and no-loss checks now use market mid directly instead of deriving from skewed risk_quote, preventing A-S inventory skew from distorting order placement

### Added

- **Crossed-book arbitrage taking**: Detects and takes profitable crossed-book opportunities on Dexie (peer-to-peer DEX with no matching engine). New `ArbitrageType::CrossedBook` with configurable min edge (bps) and max take size (XCH)
- **Dexie crossed-book data acceptance**: `ingest_dexie()` no longer discards order book data when bid > ask, which is normal for unmatched P2P offers

## [0.5.5] — 2026-04-03

### Added

- **AMM-aware mid-price blending**: 3-source mid-price computation (DEX 70% + CEX 30% + AMM 15%) with freshness-weighted re-normalisation. TibetSwap implied price feeds via `ingest_amm_mid()` so the engine tracks AMM fair value in real time
- **Order-book gap detection**: `analyse_order_book_gaps()` scans competing offers for underserved price ranges per-side, returning gaps sorted by width for dynamic tier placement
- **Dynamic gap-aware tier spacing**: new `compute_ladder()` overload shifts tier spacing toward detected gaps in the competing order book, with configurable blend factor and ascending-constraint enforcement
- **Adverse-selection-aware tier sizing**: inverse-decay weighting shrinks tier 0 (most vulnerable to informed traders on Chia's 52s blocks) and redistributes capital to outer tiers. Extra-conservative sizing under high volatility
- **Dexie price inversion fix**: Dexie API returns prices as "XCH per CAT" but the engine expected "CAT per XCH". Added inversion + bid/ask swap in `get_ticker()` to produce correct mid-price
- **16 new liquidity tests**: `test_liquidity.cpp` covering gap detection, adverse-selection sizing, gap-aware spacing, AMM mid-price blending, and edge cases
- 8 new config fields: `gap_aware_spacing`, `min_gap_bps`, `max_gap_scan_bps`, `gap_blend_factor`, `adverse_selection_sizing`, `adverse_selection_decay`, `adverse_selection_sigma_threshold`, `amm_blend_weight`

### Fixed

- **Critical — No-book guard bypass**: When Dexie returned no quotes (bid=0, ask=0), the order-book price guard silently skipped, allowing offers through without any market reference. Now clears the ladder entirely when no book reference exists
- **Critical — Missing final sanity check**: Tiers with non-positive prices could survive all adjustments. Added remove_if sweep dropping any tier with price ≤ 0
- **High — No-loss bypass after price guard clamp**: Price guard could clamp an ASK below cost basis, negating the `enforce_no_loss` from step 6. Added post-clamp re-check that drops ASK tiers violating the cost-basis + margin floor
- **High — Degenerate adverse-selection sizing**: Floating-point edge cases (NaN/Inf/underflow) in tier sizing normalization. Added `isfinite` validation with fallback to baseline config
- **Gap-aware spacing side-overwrite bug**: Per-side `for(side : {Bid, Ask})` loop mutated shared `tier_spacing_bps` — Ask overwrote Bid adjustments. Fixed by merging gap centers from both sides into one pass
- Order-book price guard added: clamps BID ≤ dex_best_ask and ASK ≥ dex_best_bid to prevent crossing the existing spread

## [0.5.4] — 2026-04-03

### Fixed

- **Critical**: Integer overflow in `build_offer_dict()` — the formula `tier.size * tier.price / quote_denom` overflowed int64 (product ~10²⁴), producing garbage amounts that the wallet rejected as "insufficient funds" for both BID and ASK offers. Fixed by decomposing into proper unit conversions: `(size/base_mojos_per_unit) × (price/kMojosPerXch) × quote_mojos_per_unit`, computed in double to avoid overflow
- **Chia wallet status field compatibility**: Newer Chia wallet versions return trade-record `status` as strings (`"PENDING_ACCEPT"`, `"CONFIRMED"`, etc.) instead of integers. Three call sites in `OfferManager` crashed with `json::type_error.302`. Added `trade_status::parse()` helper that accepts both formats

## [0.5.3] — 2026-04-03

### Added

- **Stuck transaction pruning**: new `OfferManager::prune_stuck_transactions()` detects wallet transactions with no spend bundle (stuck > 10 min) and clears them via `delete_unconfirmed_transactions` RPC. Runs automatically at startup
- **Pending-change gate**: Step 8 now queries live wallet balances before posting offers. If any wallet has `pending_change > 0` (coins in-flight from a prior transaction), offer creation is skipped until the pending transaction confirms on-chain (~1-2 blocks). Prevents the wallet daemon from reusing already-spent coins across concurrent offers
- `ChiaWalletRPC::delete_unconfirmed_transactions()` — clears stuck unconfirmed transactions from a wallet
- `ChiaWalletRPC::get_transactions()` — retrieves recent transactions for stuck-tx detection

### Fixed

- **Critical**: Wallet coin double-spend causing permanently stuck transactions. Rapid consecutive `create_offer` calls could select the same unspent coin, producing spend bundles that never broadcast. The pending-change gate and stuck-tx pruner prevent and recover from this condition
- Spendable reserve gate was previously a dead no-op (`cached_wallet_balances_` never populated). Now queries live wallet balances per-pair before posting

## [0.5.2] — 2026-04-03

### Added

- **Startup offer reconciliation**: on launch, engine queries the database for pending offers and scans the wallet for all PENDING_ACCEPT offers. Known offers are restored into State for tracking; unknown orphans are automatically cancelled to free locked capital
- `Database::query_pending_offers()` — retrieves all offer_log rows with status='pending' for startup recovery
- `OfferManager::startup_reconcile(known_ids)` — wallet-wide scan that cancels orphaned offers not tracked in the database

### Fixed

- Offers orphaned by engine restarts or crashes are now detected and cancelled automatically, preventing indefinite capital lockup (previously required manual cleanup)

## [0.5.1] — 2026-04-03

### Fixed

- **Critical**: Eliminate zero-offer gap during cancel→repost cycle via selective refresh (Gao & Wang 2020). New `classify_tier_staleness()` evaluates per-tier price deviation; only stale/expired tiers are cancelled while Fresh tiers remain live on the order book
- **Critical**: `cancel_stale()` now treats wallet cancel as authoritative — offer ID recorded as cancelled even if `state_->remove_offer()` returns false, preventing orphaned-offer re-cancel loops
- **Critical**: Shutdown DB persistence retries `update_offer_status()` up to 3 times, preventing ghost "pending" records that cause phantom offers on next startup
- **Critical**: `detect_fills()` position accounting failures no longer suppress fill emission — fills are always recorded and offers always removed regardless of `record_buy`/`record_sell` outcome
- **High**: Asymmetric ladder guard in batched `post_quotes()` — if one side (bid/ask) fails completely while the other succeeds, the posted side is cancelled to prevent one-sided book exposure
- **High**: Wallet ID cache (`wallet_ids_resolved_`) now invalidated on circuit-breaker recovery via new `invalidate_wallet_ids()` method, allowing runtime discovery of newly added CAT wallets

### Added

- `OfferManager::classify_tier_staleness()` — per-tier staleness classification (Fresh/Stale/Expired) based on price deviation from optimal ladder
- `OfferManager::selective_cancel()` — cancel only stale/expired tiers, leaving fresh tiers live
- `OfferManager::invalidate_wallet_ids()` — force wallet-ID cache rebuild on next `post_quotes()`
- `TierStaleness` enum and `TierClassification` struct for selective refresh decision-making
- Selective refresh filter in `Engine::step_manage_offers()` — posts replacement tiers only for cancelled slots, preventing double-exposure at fresh price levels
- `kSelectiveRefreshThreshold` constant (0.5%) for per-tier staleness classification

### Changed

- `Engine::step_manage_offers()` now uses 3-phase decision: classify → selective cancel → filtered repost (replaces blanket cancel_stale → post_quotes)
- Adaptive fees enabled by default in config

## [0.5.0] — 2026-04-03

### Added

- Spendable reserve gating: engine skips offer posting when any wallet's spendable/confirmed ratio falls below configurable threshold (`min_spendable_reserve_pct`, default 25%)
- Stuck offer detection and auto-cancellation: offers surviving beyond `offer_ttl_blocks + stuck_offer_age_blocks` are logged with fee info and cancelled
- Per-offer fee tracking: `fee_mojos` field stored on `PendingOffer`, `DbOfferRecord`, and `offer_log` DB table for fee-to-fill-time analytics
- Prometheus gauge `xop_spendable_reserve_pct{wallet}` — fraction of confirmed balance that is spendable (0–1)
- Prometheus gauge `xop_stuck_offers` — count of offers stuck beyond TTL + stuck-age threshold
- Config fields: `strategy.min_spendable_reserve_pct` (double, 0–1) and `strategy.stuck_offer_age_blocks` (uint32_t, default 30)
- GUI dashboard: wallet balance card shows reserve percentage with color-coded thresholds (red <10%, yellow <25%) and stuck-offer warning row
- GUI `MetricsService.get_spendable_reserve()` and `get_stuck_offers()` Prometheus parsers
- Forward-compatible DB migration: `ALTER TABLE offer_log ADD COLUMN fee_mojos INTEGER DEFAULT 0`
- Wallet balance Prometheus export (`xop_wallet_balance{wallet,field}`) for spendable, confirmed, unconfirmed, pending_change, pending_coin_removal, max_send
- Pre-flight balance check in `post_quotes()` — verifies spendable balance before tier loop
- GUI Dashboard "Wallet Balances" card with color-coded status

### Changed

- `resolve_wallet_id()` made public in OfferManager
- Engine `step_manage_offers()` extended with stuck-detection pass and reserve-gating logic

## [0.3.0] — 2026-04-03

### Fixed

- Fix QScrollArea wrapping hiding widget methods (dashboard, market analysis, settings)
- Fix startup analysis never displaying in GUI (`_create_page_widget` scroll wrapper)
- Fix per-side offer posting: bid-side insufficient funds no longer blocks ask-side offers
- Fix `compute_concentration()` returning 0.0 with empty positions (now returns 0.5 balanced)
- Fix GUI metric name mismatches (5 getters: pnl, health, market_data, offers, risk)
- Fix analysis data gating in EngineBridge (removed bot_status == Analyzing requirement)
- Fix null JSON crash in dexie_client.cpp with `json_number_or<T>()` helper

### Added

- Per-pair fault isolation in `step_update_market_state()` (try/catch per pair)
- `_unwrap()` helper for QScrollArea-wrapped page widgets in MainWindow

### Changed

- Default `q_max` guidance: must match actual wallet capacity (was 1000, realistic ~10)

## [0.2.2] — 2026-04-02

### Changed

- Pre-commit icons (icon.ico, icon.png) to repo; remove runtime generation
- Desktop shortcut enabled by default in Windows installer
- Single installer exe for Windows releases (standalone binaries removed)
- Release workflow deletes old assets before uploading new ones
- Harden macOS CI with Ninja generator

## [0.2.1] — 2026-04-02

### Fixed

- Fix constructor initializer order in Engine (Werror reorder)
- Add VolatilityEstimator::get_regime_duration_blocks() method
- Fix IndentationError in gui/widgets/chart.py
- Fix IndentationError in gui/widgets/main_window.py

## [0.2.0] — 2026-04-02

### Changed

- Version bump to 0.2.0

## [0.1.9] — 2026-04-01

### Changed

- Version bump to 0.1.9

## [0.1.7] — 2026-03-31

### Changed

- Version bump to 0.1.7

## [0.1.6] — 2026-03-31

### Changed

- Version bump to 0.1.6

## [0.1.5] — 2026-03-31

### Fixed

- Fixed `AttributeError` in `MetricsService`: use `Qt.TimerType.CoarseTimer` instead of `QTimer.TimerType`

### Changed

- Version bump to 0.1.5

## [0.1.4] — 2026-03-31

### Changed

- Version bump to 0.1.4

## [0.1.3] — 2026-03-28

### Changed

- Version bump to 0.1.3

## [0.1.2] — 2026-03-26

### Changed

- Version bump to 0.1.2

## [0.1.1] — 2026-03-26

### Fixed

- Fill-rate feedback loop: replaced hardcoded `fill_rate_24h = 0.30` and `fill_rate_per_block = 0.03` with DB-computed values from offer_log history
- Telegram alert HTML injection: added entity escaping (`&<>"`) in `post_telegram()` including unsafe fallback path
- SQLite diagnostic queries: `trade_count()`, `offer_count()`, `snapshot_count()` now check `sqlite3_step()` return values
- FetchContent supply-chain: pinned nlohmann_json, spdlog, yaml-cpp to commit SHAs instead of mutable tags
- Desktop file `Exec` path corrected for Linux packaging

### Added

- Configurable `offer_fee_mojos` in `StrategyConfig` (was hardcoded 100M mojos across 5 call sites)
- Link-Time Optimization for Release builds via `CheckIPOSupported`
- `ctest` step in CI workflow — tests now gate artifact upload
- Linux `uninstall.sh` with `--purge` option, bundled in release tarball
- `CHANGELOG.md`
- Release workflow triggers on GitHub UI release publish (+ concurrency guard)

### Changed

- TODO.md summary table updated (84/121 items complete)

## [0.1.0] — 2026-03-25

Initial release of the XOPTrader CHIA DEX market-making engine.

### Engine

- 13-step per-block heartbeat orchestration engine with Boost.Asio coroutines
- Avellaneda-Stoikov and GLFT market-making strategy implementations
- 4-component spread optimizer (adverse selection, inventory, cost basis, competition)
- Multi-tier offer ladder (configurable tiers, spacing, size allocation)
- Yang-Zhang volatility estimator with Bayesian PIN adverse-selection model
- HMM + variance-ratio regime detection (mean-reverting, random, momentum)
- Order book tactician with Thompson Sampling strategy selection
- Competitor detection and response (own-offer filtering, spread tracking, alerts)
- Whale trader detection with configurable thresholds
- CHIA structural edge multiplier (settlement speed, no-counterparty-risk, etc.)
- Strategic Loss Manager with EV-based rebalance decisions
- Arbitrage scanner (CEX-DEX, cross-DEX, triangular, cross-bridge)
- 7-layer hedging framework with Natural Hedge Efficiency tracking
- Backtesting framework with walk-forward window support

### Connectivity

- Chia full node RPC client (mTLS, port 8555) for block data and coin records
- Chia wallet RPC client (mTLS, port 9256) for offer lifecycle management
- dexie.space API client with rate-limited coroutine interface
- Per-request CURL handles with RAII wrappers for thread safety
- Configurable SSL verification with Chia CA cert support

### Risk Management

- Inventory tracking with mark-to-market concentration limits
- Soft/hard inventory limits with graduated proportional sizing
- Half-Kelly position sizing with division-by-zero guards
- Max-drawdown global circuit breaker (10% default)
- Flash-crash state machine (Normal → Crash → Recovery → Normal)
- Crowding recovery mechanism with cooldown and geometric decay
- Per-pair strategy instances (no shared mutable state)
- Configurable `max_half_spread_bps` cap preventing market withdrawal
- Configurable `offer_fee_mojos` for on-chain fee management

### Data Integrity

- SQLite persistence for trades, offers, and analytics snapshots
- `sqlite3_step()` return value checking on all diagnostic queries
- Fill-rate feedback loop computing rates from offer_log history
- Trade log timestamps populated from fill data
- Proper SHA-256 coin name computation via OpenSSL EVP
- `std::llround()` for all mojo price conversions (no truncation bias)
- Inventory units converted from mojos to base-asset display units
- Crossed-book data validation before ingestion

### Observability

- 24 Prometheus metrics with cardinality-guarded label sets
- 14 Telegram alert rules with HTML entity escaping
- Structured logging via spdlog with secrets redacted
- PnL attribution (spread / inventory / fee components)
- Tax CSV export with acquisition timestamps

### Build & Packaging

- CMake 3.24+ build system with vcpkg manifest mode
- C++20 with coroutine support (MSVC/GCC/Clang)
- Compiler hardening: `-Wall -Wextra -Werror`, stack protector, FORTIFY_SOURCE, RELRO
- Link-Time Optimization for Release builds via `CheckIPOSupported`
- FetchContent dependencies pinned to commit SHAs (nlohmann_json, spdlog, yaml-cpp)
- GitHub Actions CI/CD with 3-platform builds and artifact upload
- Tests run via `ctest` in CI before artifact creation
- Python GUI via PySide6 + pyqtgraph with PyInstaller packaging
- Windows Inno Setup installer with optional desktop shortcut
- Linux install bundle with `.desktop` file and uninstall script
- `pyproject.toml` with build backend, upper-bounded dependencies, `requires-python >=3.11,<4`

### Configuration

- YAML-based configuration with validation and error messages
- `config.example.yaml` reference with full 64-char asset ID placeholders
- GUI error handling for backend initialization failures

### Tests

- 81 unit tests across 8 test files (Google Test)
- Avellaneda-Stoikov math, spread optimizer, inventory/risk,
  volatility, regime detection, competitor detection, whale detection,
  and advanced trading methods

### Academic Rigor (Counter-Research Validation)

- VPIN validation gate with rolling-window precision tracking
- Exponential-decay tau for Avellaneda-Stoikov/GLFT
- Variance ratio Z-statistic significance gating
- Discounted Thompson Sampling for non-stationary rewards
- Sparse-fill correction for GLFT intensity estimation
- Fill-count dampening for Brock-Hommes heterogeneous agent model

### Known Limitations

- CEX reference prices not yet integrated (Phase 2)
- PreTradeCheck, GLFT, and config parsing test suites incomplete
- vcpkg baseline dated 2024-09-30
- No code signing for release binaries
