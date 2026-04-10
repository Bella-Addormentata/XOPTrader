# Changelog

All notable changes to XOPTrader are documented in this file.

Format based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.7.33] — 2026-04-09

### Fixed

- **Depleted-side offer cancellation** (engine.cpp Step 8): When an asset's balance drops below the reserve threshold, existing offers that sell that asset are now cancelled immediately — even if they are Fresh.  Previously, the anti-churn logic kept all fresh offers alive when both sides were suppressed, reasoning that they would expire naturally via TTL.  However, counterparties could fill those asks before expiry, draining the asset to zero (BYC went from 0.878 → 0.000 while the engine logged "keeping live to prevent churn").  The fix introduces `base_depleted` / `quote_depleted` tracking at Gate 2 (reserve-ratio) and Gate 3 (min-balance) suppression points, then:
  - **Both sides suppressed**: cancels fresh offers on the depleted side (asks for depleted base, bids for depleted quote) while preserving anti-churn behavior for the non-depleted side.
  - **Single side suppressed**: cancels existing offers on the depleted side before the early-continue, allowing the active side to repost normally.
  - XCH UTXO-lock anti-churn is preserved when neither side is truly depleted (the scenario it was designed for).

## [0.7.31] — 2026-04-09

### Changed

- **Offer TTL increased** (`offer_ttl_blocks` 180 → 600): Reduces cancel/repost churn from every ~90 min to every ~5 hours. Fewer blockchain transactions, lower fee spend, and less pending_change downtime between cycles.

- **Fee floor lowered** (`min_fee_mojos` 50000 → 5000): Previous 50K floor was 20× the mempool estimate. 5K is still ~2× the current mempool rate, saving ~45K mojos per on-chain transaction.

- **CAT coin pool disabled** (`cat_coin_pool_target_count` 10 → 0): BYC and wUSDC.b have near-zero spendable balances (most locked in offers). Splitting was failing every cycle, wasting logs. Disabled until CAT inventory recovers.

- **Spendable reserve threshold lowered** (`min_spendable_reserve_pct` 0.25 → 0.10): The 25% spendable/confirmed ratio gate was permanently suppressing both sides of BYC/wUSDC.b (only 11.9% free). Lowered to 10% to allow quoting when most inventory is locked in offers.

### Added

- **CEX-DEX data wiring for ArbitrageDetector**: Step 9b now builds `DexieBookSnapshot` vectors from MarketDataFeed mid/spread and `CexPrice` vectors from CoinGecko-derived references, then calls `set_dex_snapshots()` + `set_cex_prices()` before `scan_all()`. The CEX-DEX and Cross-DEX scans that were previously blind (empty caches) now receive data and can detect divergences.

- **CEX-DEX confidence cap** (`cex_dex_confidence_cap`, default 0.25): Hard ceiling on confidence assigned to CEX-DEX arbitrage opportunities. CoinGecko prices are aggregated, delayed, and vulnerable to manipulation — capping at 0.25 (below the 0.40 min_confidence_threshold) means CEX-DEX signals are logged for visibility but not acted upon unless the operator explicitly lowers the confidence threshold. Prevents the engine from making aggressive trades based on potentially manipulated CEX data.

## [0.7.30] — 2026-04-08

### Added

- **Dry-powder arbitrage reserve** (`arb_reserve_coins`, default 2): Step 7's XCH UTXO headroom calculation now deducts N coins from the available budget, ensuring they remain unallocated and instantly available for opportunistic trades (crossed-book takes, peg arb). Prevents the tier ladder from locking every spendable coin.

- **Cancel-worst-to-free for arb** (`cancel_worst_to_free`, default true): Step 9c now checks XCH spendable before the crossed-book scan. If below 0.25 XCH and no free coins exist, the engine cancels the least competitive pending offer (highest tier, oldest) via `emergency_cancel` to liberate a UTXO, then proceeds with the arb take. Works in tandem with dry-powder reserve as a fallback when all reserved coins have been consumed.

## [0.7.29] — 2026-04-08

### Added

- **Universal coin pool management (XCH + CAT tokens)**: The existing `step_maintain_coin_pool()` was declared but only handled XCH (wallet 1). Rewrote to support all asset types. Phase 1 splits XCH coins as before. Phase 2 automatically discovers every unique CAT wallet referenced by enabled pairs (BYC, wUSDC.b, wmilliETH.b, etc.), resolves wallet IDs via `resolve_wallet_id()`, converts the target denomination using each asset's `mojos_per_unit`, and calls `ensure_split()` for each. New config fields `cat_coin_pool_target_count` (default 10) and `cat_coin_pool_target_units` (default 50.0) control CAT splitting independently from XCH. Each wallet's `pending_change` is checked before splitting to avoid overlapping transactions. Startup and heartbeat triggers updated to fire when either XCH or CAT pool is configured.

### Fixed

- **Coin pool heartbeat activation**: The heartbeat and startup coin pool triggers only checked `coin_pool_target_count > 0` (XCH). Updated both conditions to also trigger when `cat_coin_pool_target_count > 0`, ensuring CAT splitting runs even if XCH splitting is disabled.

## [0.7.28] — 2026-04-08

### Added

- **Dust-filtered mid-price from competing offers**: The Dexie ticker API reports top-of-book prices regardless of offer size, so a 5-mojo dust offer can set the "best bid" or "best ask" and poison the mid-price used by the Avellaneda-Stoikov model. After ingesting competing offers (already filtered by `min_competitor_offer_size`), the engine now recomputes BBO from only non-dust offers and overrides the ticker-derived `dex_best_bid`/`dex_best_ask`. This ensures mid-price calculations reflect meaningful liquidity, not spam.

- **Pre-balance check in Step 9e peg-crossing taker**: Before calling `take_offer()`, the engine now queries the spendable balance of the relevant wallet (quote wallet for ASK takes, base wallet for BID takes) and skips the take with a warning if funds are insufficient. Prevents doomed RPCs that produce "Can't select amount higher than our spendable balance" errors and the associated "peg-arb failed" log noise.

- **Wallet sync gating before startup inventory seeding**: Added a sync-wait loop before the startup inventory seeding block. The engine polls `get_sync_status()` up to 30 times (~5 minutes) waiting for the wallet to report fully synced before querying balances. Prevents unreliable balance data from a partially-synced wallet from seeding incorrect inventory positions and causing the A-S model to generate bad quotes on the first tick.

## [0.7.27] — 2026-04-08

### Added

- **Inventory ratio guard for Step 9e peg-crossing taker**: New config field `peg_arb_max_inventory_ratio` (default 0.70) caps how far inventory can skew before Step 9e suppresses takes. When base holdings exceed 70% of portfolio value, ASK takes (buying more base) are suppressed. When base holdings drop below 30%, BID takes (selling base) are suppressed. Prevents the engine from accumulating unlimited BYC from the ~2,500 artificially cheap sub-peg offers on the Dexie BYC/wUSDC.b book while still allowing opportunistic takes within the allowed balance range.

## [0.7.26] — 2026-04-08

### Fixed

- **Inventory seeding from wallet balances at startup**: The `InventoryTracker` started at zero for all assets and only updated from recorded fills. With no fill history, `inventory_ratio()` permanently returned 0.5 (perfectly balanced), causing the Avellaneda-Stoikov model to place symmetric quotes regardless of actual wallet composition. This led to buying BYC and selling wUSDC.b without any rebalancing pressure. Fix: at engine startup (after wallet connection and offer reconciliation), query on-chain spendable balances for each configured pair's assets and call the new `seed_position()` method. The inventory skew model now reflects real holdings from the first tick, generating appropriate bid/ask asymmetry to rebalance towards target allocation.
- **`ensure_wallet_ids()` public method on OfferManager**: The asset-to-wallet-ID cache (`init_wallet_id_map()`) was only populated lazily inside `post_quotes()`. Inventory seeding at startup needed wallet IDs before any quotes were posted. Added a public `ensure_wallet_ids()` coroutine that populates the cache on demand — safe to call multiple times (no-op after first initialization).

## [0.7.25] — 2026-04-08

### Fixed

- **Competing offer fetch for CAT/CAT pairs**: The Dexie API `pair_id` parameter for non-XCH denomination tokens (e.g. wUSDC.b) returns ALL 584K+ offers involving that token — not just the target pair. With `page_size=100`, BYC/wUSDC.b offers were never found. Fix: for CAT/CAT pairs, fetch each direction separately using `offered`/`requested` asset ID parameters, yielding precise per-pair results. XCH/CAT pairs continue using the faster `pair_id` path unchanged.
- **Dust threshold scaling for CAT pairs**: `min_competitor_offer_size` (default 1 trillion mojos = 1 XCH) rejected all BYC offers because BYC uses 1000 mojos/unit. Fix: scale the threshold proportionally by `base_mojos_per_unit / kMojosPerXch`, with a floor of 1 unit. For BYC (1000 mpu), effective threshold = 1000 mojos (1 BYC). For XCH, unchanged.
- **BID-side competing offer price normalization**: Dexie API always reports `price = requested/offered`. For ASK offers (offered=base) this matches market convention. For BID offers (offered=quote) it's the reciprocal. Fix: invert BID prices to market convention at ingestion, fixing Step 9e peg comparison, competitive cap (Step 7), and spread analysis for all pairs.

## [0.7.24] — 2026-04-08

### Added

- **Step 9e — Peg-crossing offer taker**: Automatically takes competing offers that cross the $1 peg on stablecoin pairs when the depeg detector reports Normal status (peg trusted). BIDs above peg are free premium to sell into; ASKs below peg are discounted buys. Guarded by depeg detector — will not fire during Warning, Bailed, or SuspectedFailure states, preventing the engine from buying into a real depeg event. New config fields: `peg_arb_enabled`, `peg_arb_min_edge_bps` (default 5 bps), `peg_arb_max_take_units` (default 50 units).

## [0.7.23] — 2026-04-08

### Fixed

- **Stablecoin peg guard for BID prices**: Added hard cap in Step 7 that drops any BID tier priced at or above `peg_target` on stablecoin pairs. Prevents the engine from ever bidding >= $1.00 on a stablecoin — even when a crossed or noisy order-book pushes the computed mid above peg. ASK tiers are also floored at `peg_target × (1 + margin_bps)` using per-pair `min_profit_margin_bps_override` (or global fallback). Fixes issue where BYC/wUSDC.b BID tiers were placed above market mid due to crossed-book normalization.

## [0.7.22] — 2026-04-08

### Added

- **Automatic coin pool maintenance**: New `step_maintain_coin_pool()` runs at engine startup and every `coin_pool_interval_blocks` (default 50) to self-send XCH and maintain a pool of pre-split coins. Prevents the large-coin-locking problem where a single UTXO (e.g. 135 XCH) gets locked by a small offer, starving all other offers of capital.
- **`get_next_address` RPC method**: Added to `ChiaWalletRPC` for obtaining wallet receive addresses (used by coin splitting).
- **Coin pool config**: New `strategy` fields: `coin_pool_target_count` (default 20), `coin_pool_target_xch` (default 5.0), `coin_pool_interval_blocks` (default 50). Set `coin_pool_target_count: 0` to disable.
- **Zero-fee coin splitting**: Splits use fee=0 by default since Chia mainnet mempool is rarely congested.

## [0.7.21] — 2026-04-08

### Changed

- **Proportional DEX/CEX divergence response**: Replaced the binary 1.5× spread multiplier (triggered at >200 bps divergence) with a linear ramp: `mult = 1.0 + min(divergence_bps / 1000, 0.5)`. At 200 bps divergence the multiplier is now 1.2× instead of 1.5×, scaling up to max 1.5× at 500+ bps. Eliminates the cliff-edge that caused uncompetitive pricing.
- **Reservation mid clamp tightened 2% → 1%**: Bids and asks now stay within 1% of market mid instead of 2%, keeping quotes competitive when the Avellaneda-Stoikov reservation price diverges.
- **`high_vol_multiplier` now configurable**: Exposed the high-volatility regime multiplier (previously hardcoded at 1.80) as a YAML parameter in `strategy.high_vol_multiplier`. Set to 1.3 (30% widen vs 80%).
- **Config tuning for competitiveness**: `gamma` 0.01→0.005 (halves adverse selection base), `max_half_spread_bps` 250→150 (300 bps round-trip cap), `wall_size_threshold_xch` 20→50, `wall_niche_premium_pct` 15%→5%.
- **Circuit breakers tightened**: `max_drawdown_pct` 10%→5%, `loss_window_blocks` 1152→576 (16h→8h), `max_window_loss_bps` 500→250 (5%→2.5%). Engine pauses earlier if trades are losing money.

## [0.7.20] — 2026-04-07

### Fixed

- **Stablecoin ask price floor (peg guard)**: Added a final safety net in Step 7 that floors all ASK prices at `peg_target × (1 + min_margin_bps)` and caps all BID prices at `peg_target × (1 − min_margin_bps)` for stablecoin pairs. Previously, when the market mid dipped below peg (e.g. BYC trading at 0.98 wUSDC), the engine would place asks below $1.00 — effectively selling a $1-pegged asset at a loss. The peg guard prevents this regardless of what upstream pricing stages compute.
- **Stablecoin undercut bounds respect peg**: The competitive undercut logic (penny-ahead for tier 0) now enforces peg-based bounds in addition to mid-based bounds. Asks cannot undercut below `peg × (1 + margin)` and bids cannot overbid above `peg × (1 − margin)`.
- **Peg-anchor threshold now inclusive**: Changed `dev < threshold` to `dev <= threshold` so that deviations exactly at the configured `peg_anchor_threshold_pct` (e.g. 2.0%) trigger blending rather than being skipped.
- **GUI engine binary search order**: `engine_bridge.py` now checks `cpp/build/Release/xop_trader.exe` before the project-root copy, preventing stale root-level binaries from being launched over freshly-built ones.

## [0.7.19] — 2026-04-07

### Added

- **Configurable minimum offer size (`min_offer_size_units`)**: New global strategy field (default 1.0) and per-pair override (`min_offer_size_units_override`) control the minimum tier size in base-asset units. Tiers below this threshold are dropped in Step 7 before posting. Prevents dust-sized offers (e.g. 2-3 BYC ≈ $2-3) that waste XCH fees, fragment wallet UTXOs, and clutter the DEX with economically insignificant offers.

### Fixed

- **BYC/wUSDC.b dust offers**: Set `min_offer_size_units_override: 10` for BYC/wUSDC.b, requiring at least 10 BYC (~$10) per tier. Previously the 1-unit (1 BYC) minimum allowed tiers as small as 2-3 BYC through, with all tiers clamped to the same price after the order-book guard — posting redundant identical-price offers worth $2-3 each.

## [0.7.18] — 2026-04-07

### Fixed

- **Offer churn from overly aggressive soft TTL threshold**: The `kSoftTtlAdverseThreshold` was 0.2%, far tighter than the normal-zone threshold (0.5%) and well below the 2% reservation_mid clamp range. Normal Avellaneda-Stoikov model fluctuations within the 2% clamp produced >0.2% "adverse" deviation, causing offers to expire every 60 blocks (~18 min), get cancelled, then immediately recreated — wasting fees and blocking all offer posting during the pending_change confirmation window. Raised threshold from 0.2% to 2.0% to match the reservation_mid clamp range.
- **Increased default offer TTL from 60 to 180 blocks** (~56 min): 60 blocks (~18 min) was too aggressive for Chia's slow DEX market, causing unnecessary churn. Hard TTL (2× soft) is now 360 blocks (~112 min).

## [0.7.17] — 2026-04-07

### Added

- **Cross-stablecoin arbitrage (Step 9d)**: New engine step detects and takes cross-market arbitrage between XCH/BYC and XCH/wUSDC.b order books. Since both BYC and wUSDC.b are USD-pegged stablecoins (~$1), XCH should be priced equivalently on both markets after adjusting for the BYC/wUSDC.b cross-rate. When one market's ask is cheaper than the other's bid by more than `cross_stable_min_edge_bps` (default 15 bps), the engine takes the cheap ask. Based on Shleifer & Vishny (1997) limits-to-arbitrage theory and Makarov & Schoar (2020) cross-venue crypto spreads.
- **Spread-aware triangular arbitrage**: `scan_triangular` now uses bid/ask prices when available instead of mid prices. Selling legs use bid prices; buying legs use 1/ask. This eliminates phantom profits from ignoring half-spreads across the 3-leg route (Kozhan & Tham 2012). Falls back to mid prices when bid/ask data is unavailable.
- **New config fields**: `cross_stable_arb_enabled`, `cross_stable_min_edge_bps`, `cross_stable_max_take_xch` under the `arbitrage:` section.
- **`PairBidAskMap` type and `set_pair_bid_asks()` setter** for providing bid/ask data to the arbitrage detector.

## [0.7.16] — 2026-04-07

### Fixed

- **Budget-starved offers falsely cancelled as price_adverse(100%)**: When low XCH balance causes all new tiers to be dropped by the sub-unit minimum filter or dynamic tier limiter, `classify_tier_staleness` could not find the pending offer's tier index in the (now empty) new ladder and unconditionally classified it as `Stale` with 100% adverse price deviation. This cancelled perfectly healthy offers that could not be replaced, creating a pointless cancel→0-offers→post→cancel cycle wasting fees. Fixed by falling back to a mid-price sanity check when the tier is absent from the new ladder: offers that have not crossed the mid-price are classified as `Fresh` and kept alive; only crossed-mid offers (immediate adverse selection risk) are cancelled. Hard TTL expiration is unaffected.

## [0.7.15] — 2026-04-07

### Fixed

- **Reconciler falsely cancels all live offers (string status regression)**: The Chia wallet (newer versions) returns offer status as string names (`"PENDING_ACCEPT"`, `"CANCELLED"`, `"CONFIRMED"`, etc.) instead of integer codes. The v0.7.12 fix handled this with `std::stoi()`, which works for `"3"` but throws on `"CANCELLED"`. The catch block skipped the offer entirely, so `PENDING_ACCEPT` offers were missing from the wallet map, falsely marked "NOT FOUND", and cancelled via `on_chain_reconcile`. Fixed by adding a proper string-name-to-integer-code lookup for all six Chia offer states.

## [0.7.14] — 2026-04-07

### Fixed

- **XCH-buy-only deadlock: dynamic tier limiter blocks recovery bids**: When spendable XCH is marginally above the fee reserve (e.g. 1.208 XCH with 1.0 reserve), the Step 8 dynamic tier limiter trimmed ALL tiers to zero because the `xch_budget` (0.008 XCH) couldn't cover the 0.25 XCH UTXO overhead per offer. This created a permanent deadlock: no XCH-buy bids could be posted, so XCH could never recover. Fixed by skipping the Step 8 XCH budget limiter when `xch_buy_only_mode` is active for pairs involving XCH. The offer_manager's per-tier UTXO-lock recovery zone check already handles safety correctly, allowing XCH-buy bids when spendable ≥ 1× reserve.

## [0.7.13] — 2026-04-07

### Added

- **Cross-pair correlated inventory skewing (Guéant 2019)**: When multiple pairs share a common asset (XCH↔BYC↔wUSDC.b triangle), each pair's inventory skew now accounts for inventory pressure from other pairs. If XCH/BYC is short BYC, BYC/wUSDC.b automatically skews its bids to acquire more BYC. The adjustment is weighted by the market allocator's allocation fractions and clamped to prevent runaway skew. Configurable via `cross_pair_skew_enabled` (default: off) and `cross_pair_skew_phi` (default: 0.30).

### Changed

- **Inventory aging enabled** with conservative settings: positions begin relaxing the no-loss constraint after 500 blocks (~7 hours), up to 25 bps max loss, preventing capital from getting permanently locked in underwater positions.

- **Circuit-breaker auto-rebalance enabled**: When inventory ratio exceeds 80% and the position has aged past the threshold, the loss manager is automatically engaged with a 100 bps loss budget to free locked capital.

- **Triangular arbitrage threshold lowered** from 30 bps to 12 bps (just above the fee floor), enabling more frequent marginal rebalancing trades through the XCH↔wUSDC↔BYC triangle.

## [0.7.12] — 2026-04-07

### Fixed

- **Offers cancelled immediately after creation by on-chain reconciler**: `verify_pending_offer_coins` crashed with `json.exception.type_error.302` when the Chia wallet returned offer `status` as a string instead of an integer. After the error, the wallet offer map was empty, causing ALL pending offers to be falsely marked "NOT FOUND in wallet" and cancelled via `on_chain_reconcile`. Fixed by (1) handling `status` as either int or string, and (2) aborting stale detection entirely when the wallet query fails completely, preserving all pending offers.

- **Sub-unit BYC offers wasting fee coins**: The engine created BYC/wUSDC.b offers with sizes as small as 970 mojos (0.97 BYC, < 1 full unit), which are economically insignificant and waste XCH on chain fees. Added a minimum offer size filter in Step 7 that drops any tier where `size < base_mojos_per_unit` (1000 mojos for CAT tokens, ensuring at least 1 full unit per offer). Logged as `[Engine] Step 7: dropped N sub-unit tiers`.

## [0.7.11] — 2026-04-07

### Fixed

- **UTXO-lock deadlock when XCH balance drops between 1× and 2× reserve**: The 2× reserve UTXO-lock guard blocked ALL offers — including buy-XCH bids — when spendable XCH fell below `2× fee_reserve_xch`. This created an unrecoverable deadlock: the engine wanted to buy XCH to restore balance, but couldn't create the offers needed to do so. Added a "recovery zone" (1× ≤ spendable < 2× reserve) that allows buy-XCH offers through while still blocking sell-XCH and non-XCH offers. Applied to all three UTXO-lock checks in offer_manager (batch pre-check, per-tier pre-check, and post-creation guard)

## [0.7.10] — 2026-04-06

### Added

- **Warning log when min_fee floor significantly exceeds mempool estimate**: `FeeTracker::get_recommended_fee()` now emits a `[warn]` when `min_fee_mojos` clamps the fee up by more than 10x over the mempool estimate, making silent overpaying immediately visible in logs
- **FeeTracker unit tests** (18 tests): Covers fee selection, min/max clamping, adaptive mempool usage, budget enforcement, fee-vs-gain gating, and documents the overpay scenario that prompted v0.7.9

## [0.7.9] — 2026-04-06

### Changed

- **Reduced fee estimate target from 60s to 300s**: The `get_fee_estimate` RPC was requesting fees for 60-second inclusion urgency, producing ~9.3M mojo fees on a near-empty mempool. Market-making offers are long-lived (60-block TTL) and don't need next-block priority. New configurable `fee_estimate_target_seconds` (default 300s) drops the estimate to ~5,661 mojos when the mempool is quiet
- **Lowered `min_fee_mojos` in config.yaml from 5M to 50K**: The previous 5M floor was 880x higher than the blockchain required. Reduced to 50,000 mojos to let the adaptive fee tracker use naturally low fees during quiet periods

### Added

- **`fee_estimate_target_seconds` config field**: New `FeeConfig` setting controlling the target inclusion time passed to the Chia full node's `get_fee_estimate` RPC. Configurable per deployment; higher values = lower fees, lower urgency

## [0.7.8] — 2026-04-06

### Fixed

- **Fee-gain formula inverted for CAT/CAT pairs**: The fee gating calculation used `base_mojos_per_unit / kMojosPerXch` instead of `kMojosPerXch / base_mojos_per_unit`, causing expected gain to evaluate to 0 for CAT pairs (1e3/1e12 = ~0) and blocking all BYC/wUSDC.b offers. Corrected factor restores proper scaling (1e12/1e3 = 1e9) so gain correctly exceeds fee threshold

## [0.7.7] — 2026-04-06

### Fixed

- **Critical mojo scale bug for CAT/CAT pairs**: `base_mojos_per_unit` defaulted to 1e12 (XCH scale) for all pairs, but CAT tokens use 1e3 mojos/unit. For BYC/wUSDC.b this caused a **billion-fold pricing error** — offers requested 1.9 billion BYC for 1.9 wUSDC.b instead of correct ~1:1 ratio
- **Auto-detect mojos-per-unit from asset type**: Config parser now sets `base_mojos_per_unit` and `quote_mojos_per_unit` based on whether asset_id is "xch" (1e12) or a CAT hex ID (1e3), eliminating the need for manual configuration

## [0.7.6] — 2026-04-06

### Added

- **Stablecoin dynamic tier system**: New per-pair PairConfig fields for competitive stablecoin trading:
  - `max_half_spread_bps_override` — per-pair spread cap (replaces global 250 bps with e.g. 75 bps for stablecoins)
  - `peg_anchor_threshold_pct` — configurable deviation threshold for peg-anchor blending (raised from hardcoded 1% to 2%)
  - `peg_anchor_weight` — configurable peg weight in blend (raised from hardcoded 50% to 60%)
  - `stablecoin_exempt_buyonly` — exempts non-XCH stablecoin pairs from XCH-buy-only mode skip
  - `stablecoin_undercut_all_tiers` — enables competitive undercutting on all tiers (not just tier 0)
  - `stablecoin_flat_sizing` — bypasses adverse-selection sizing that crushes inner tiers
  - `stablecoin_skip_gap_aware` — bypasses gap-aware spacing that widens configured tight tiers

### Fixed

- **BYC/wUSDC.b not posting offers**: Pair was entirely skipped during XCH-buy-only mode because it has no XCH component. Now exempt via `stablecoin_exempt_buyonly`
- **A-S model producing 3000+ bps spreads for stablecoins**: Capped to 500 bps by global limit, still 10x too wide. Per-pair `max_half_spread_bps_override: 75` now produces ~45 bps spreads
- **Peg-anchor not activating**: BYC at 0.989 was 1.12% from peg, just outside hardcoded 1% threshold. Configurable `peg_anchor_threshold_pct: 2.0` now activates correctly
- **Gap-aware spacing counterproductive for stablecoins**: Was widening configured tight tiers [8,15,25,40,60,80] → [40,50,60,70,80,90]. Now skipped via `stablecoin_skip_gap_aware`
- **Adverse-selection crushing inner tiers**: Tier 0 was getting only 0.3% of capital. Now bypassed for stablecoins via `stablecoin_flat_sizing`

## [0.7.5] — 2026-04-06

### Fixed

- **Both-sides-suppressed churn fix**: When both bid and ask sides are suppressed (e.g. BYC balance is 0 and XCH spendable ratio is low after UTXO locking), the engine no longer cancels *all* pending offers. Fresh offers (recently created) are now kept live to avoid wasting creation fees and triggering a create→suppress→cancel→create churn cycle. Only stale/expired offers are cancelled to free locked capital

## [0.7.4] — 2026-04-06

### Fixed

- **UTXO-lock safety margin (2× reserve floor)**: All offer creation now requires `xch_spendable >= 2 × fee_reserve_xch` (default 2.0 XCH) before any offer is created. Previously, the engine would create offers when spendable was barely above the 1× reserve (e.g. 1.29 XCH with 1.0 reserve), but Chia's UTXO model locks the *entire* coin used for fee payment — a 0.005 XCH fee can lock a 1.3 XCH UTXO, instantly draining spendable to zero. This caused a create→drain→cancel→create churn cycle that kept spendable at zero indefinitely
- **Buy-only mode fee override raised**: In XCH-buy-only mode, the fee reserve override passed to `offer_manager::post_quotes` is now `fee_reserve_xch` (1.0) instead of `fee_min_spendable_xch` (0.01). The old 0.01 XCH threshold made offer_manager's per-offer guards ineffective, allowing offers that would lock entire UTXOs
- **Preemptive UTXO danger zone detection**: When spendable XCH is between 1× and 2× reserve, the engine now enters XCH-buy-only mode preemptively (before UTXO liberation triggers). This prevents the scenario where cooldown expires, the engine exits buy-only mode, immediately creates offers that drain spendable to zero, and re-enters the churn cycle

## [0.7.3] — 2026-04-06

### Fixed

- **Wallet sync gate**: `step_manage_offers` now checks `get_sync_status()` on every heartbeat and skips ALL offer management when the wallet is not fully synced. Previously, the engine would create/cancel/verify offers against an unsynced wallet, causing `verify_pending_offer_coins` to falsely mark live offers as NOT FOUND (wallet returns incomplete `get_all_offers` during sync), `cancel_offer` to fail with "Wallet needs to be fully synced", and the engine to create duplicate offers that drain XCH to zero
- **Creation grace period in `verify_pending_offer_coins`**: Offers younger than 120 seconds are no longer marked as stale when NOT FOUND in the wallet. The Chia wallet may not immediately surface newly-created offers in `get_all_offers`, and this previously caused the engine to lose track of 15-second-old offers and re-create duplicates
- **Startup reconcile wallet sync pre-check**: `startup_reconcile` now checks wallet sync status before attempting orphan cancellation. When wallet is not synced, orphans are immediately force-adopted instead of attempting cancel RPCs that will fail, preventing uncancellable orphan deadlock

## [0.7.2] — 2026-04-06

### Fixed

- **Prevent orphan-offer XCH deadlock**: Wallet offers that the engine loses track of (due to wallet desync, reconciliation race conditions, or restart) now get force-adopted back into State instead of being silently dropped. Two fixes:
  - `startup_reconcile`: when an orphan offer cannot be cancelled (e.g., 0 spendable XCH), it is now adopted into State rather than left in limbo. Previously, uncancellable orphans locked coins indefinitely with no engine awareness
  - `reconcile_offers`: periodic reconciliation now detects PENDING_ACCEPT wallet offers that are not tracked in engine State and adopts them. This catches mid-session orphans created when `verify_pending_offer_coins` incorrectly removes an offer during wallet desync
- Adds `try_parse_wallet_offer()` helper that extracts pair/side/price/size from a wallet trade record for offer adoption

## [0.7.1] — 2026-04-06

### Fixed

- **GUI shows wallet/node connected during analysis**: The engine now publishes system health metrics (block height, node synced, wallet connected) during the startup analysis phase, not only after analysis completes. Previously the GUI showed "not connected" for the entire 5+ minute analysis window even though the wallet and full node were reachable

## [0.7.0] — 2026-04-06

### Added

- **Dynamic tier limiting**: Capital-budget tier limiter that automatically reduces the number of active offer tiers when XCH spendable balance cannot support the full tier ladder. Each create_offer locks ~0.25 XCH in fee UTXOs; when posting 6 tiers (3 bid + 3 ask) would exceed the spendable headroom above the fee reserve, outer tiers are pruned from the outside in (highest tier_index first) to prevent UTXO liberation churn. Based on Gueant-Lehalle-Fernandez-Tapia (2013) capital-constrained market making: fewer, well-capitalised tiers outperform many thin tiers when capital is scarce. Includes 20% safety margin above fee reserve and logs tier trimming decisions

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
