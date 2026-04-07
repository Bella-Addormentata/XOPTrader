// config.hpp -- Configuration data structures for XOPTrader CHIA DEX market-maker.
//
// All configuration is loaded from a YAML file via yaml-cpp. Structures are
// plain-data aggregates with no heap indirection, facilitating value semantics
// and straightforward serialisation. Field names mirror the YAML schema
// defined in config.example.yaml so that the mapping is unsurprising.
//
// Security: SSL certificate paths, wallet fingerprints, and Telegram tokens
//           are classified as secrets and are excluded from any log output.
//
// ISO/IEC 27001:2022 -- secrets handling, least-privilege logging.
// ISO/IEC 5055       -- no raw pointers, bounds-checked containers.
// ISO/IEC 25000      -- clear naming, complete documentation.

#ifndef XOP_CONFIG_HPP
#define XOP_CONFIG_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <stdexcept>

namespace xop {

// ---------------------------------------------------------------------------
// Exception thrown when a configuration file is missing, malformed, or
// contains values outside their valid domain.
// ---------------------------------------------------------------------------
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// Chia node operating mode.
//   Auto:       Attempt full_node first; fall back to wallet_only if the
//               full node is unreachable at startup (recommended).
//   FullNode:   Require a running Chia full node — abort if unavailable.
//   WalletOnly: Run with the Chia wallet service only — no full node
//               required.  Block height is obtained from the wallet's
//               synced view (get_height_info), and fee estimation falls
//               back to static fees.
// ---------------------------------------------------------------------------
enum class ChiaMode : std::uint8_t {
    Auto       = 0,
    FullNode   = 1,
    WalletOnly = 2
};

/// Human-readable label for logging.
inline const char* to_string(ChiaMode m) noexcept {
    switch (m) {
        case ChiaMode::Auto:       return "auto";
        case ChiaMode::FullNode:   return "full_node";
        case ChiaMode::WalletOnly: return "wallet_only";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Chia blockchain RPC connectivity and authentication.
// Covers both the full-node and wallet daemon endpoints.
// ---------------------------------------------------------------------------
struct ChiaConfig {
    ChiaMode    mode{ChiaMode::Auto};   // Node operating mode (auto/full_node/wallet_only).
    std::string full_node_host;         // Full-node RPC hostname.
    uint16_t    full_node_port{8555};   // Full-node RPC port (default 8555).
    std::string wallet_host;            // Wallet RPC hostname.
    uint16_t    wallet_port{9256};      // Wallet RPC port (default 9256).

    // SSL mutual-auth certificate/key for the full-node endpoint.
    std::string ssl_cert_path;          // SECRET -- never log.
    std::string ssl_key_path;           // SECRET -- never log.

    // SSL mutual-auth certificate/key for the wallet endpoint.
    std::string wallet_cert_path;       // SECRET -- never log.
    std::string wallet_key_path;        // SECRET -- never log.

    // CA certificate used to verify the Chia daemon's server certificate.
    // Required when verify_ssl is true (the default).
    std::string ca_cert_path;           // SECRET -- never log.

    // SSL certificate verification policy for Chia RPC.
    // Keep true in normal operation; set false only for trusted localhost
    // troubleshooting when certificate validation is failing.
    bool        verify_ssl{true};

    // Wallet fingerprint that identifies the key to use.
    uint32_t    wallet_fingerprint{0};  // SECRET -- never log.
};

// ---------------------------------------------------------------------------
// Dexie aggregator API settings.
// Rate limiting is enforced per the published ceiling (50 req / 10 s).
// ---------------------------------------------------------------------------
struct DexieConfig {
    std::string api_base{"https://api.dexie.space/v1"};
    uint32_t    max_requests_per_10s{50};  // Must be >= 1.
    bool        claim_rewards{true};       // Auto-claim DBX liquidity rewards on offer submission.
};

// ---------------------------------------------------------------------------
// A single trading pair the bot may market-make.
// asset IDs are 64-character lower-hex strings except for native XCH which
// uses the literal "xch".
// ---------------------------------------------------------------------------
struct PairConfig {
    std::string base_asset_id;   // e.g. "xch".
    std::string quote_asset_id;  // e.g. 64-hex CAT asset ID.
    std::string name;            // Human-readable label, e.g. "XCH/wUSDC".
    bool        enabled{true};   // Inactive pairs are loaded but skipped.

    /// Mojos-per-displayable-unit for the base asset.
    /// XCH: 10^12 (1 XCH = 1 000 000 000 000 mojos).
    /// CAT: 10^3  (1 CAT unit = 1 000 mojos).
    /// ISO/IEC 5055: explicit denomination prevents silent truncation when
    /// a CAT amount is divided by the XCH constant (off by 10^9).
    std::int64_t base_mojos_per_unit{1'000'000'000'000LL};

    /// Mojos-per-displayable-unit for the quote asset.
    /// Same convention as base_mojos_per_unit.
    std::int64_t quote_mojos_per_unit{1'000LL};

    // -- Per-pair strategy overrides ----------------------------------------
    // When set, these override the global StrategyConfig values for this
    // pair only.  Allows stablecoin pairs (e.g. BYC/wUSDC.b) to use
    // tighter spreads and lower risk aversion than volatile pairs.
    std::optional<double>   gamma_override;
    std::optional<double>   kappa_override;
    std::optional<double>   phi_override;
    std::optional<double>   q_max_override;
    std::optional<double>   min_profit_margin_bps_override;
    std::optional<std::vector<double>> tier_spacing_bps_override;
    std::optional<std::vector<double>> tier_size_pct_override;
    std::optional<double>   max_half_spread_bps_override;

    // -- Stablecoin peg configuration ---------------------------------------
    // When is_stablecoin is true, the depeg detector monitors this pair
    // and can flag it as suspected-failed, pulling all quotes.
    bool   is_stablecoin{false};
    double peg_target{1.0};               // Expected trading price.
    double depeg_warn_pct{2.0};           // Warn when >2% off peg.
    double depeg_bail_pct{10.0};          // Bail out (pull quotes) when >10% off peg.
    uint32_t depeg_sustained_blocks{30};  // Must persist N blocks before bail (~26 min).

    // -- Stablecoin trading overrides ---------------------------------------
    double peg_anchor_threshold_pct{1.0};   // Dev pct for peg-anchor blending.
    double peg_anchor_weight{0.50};         // Weight of peg in blend (0-1).
    bool   stablecoin_exempt_buyonly{false}; // Exempt from XCH-buy-only skip.
    bool   stablecoin_undercut_all_tiers{false}; // Competitive undercut on all tiers.
    bool   stablecoin_flat_sizing{false};    // Skip adverse-selection sizing.
    bool   stablecoin_skip_gap_aware{false}; // Skip gap-aware spacing.
};

// ---------------------------------------------------------------------------
// Core Avellaneda-Stoikov / GLFT market-making algorithm parameters.
//
// gamma  -- risk aversion coefficient (controls spread width).
// kappa  -- fill intensity decay (higher = less impact of spread on fills).
// phi    -- GLFT inventory skew strength (skew = phi * q / q_max).
// q_max  -- maximum tolerated inventory in base-asset units.
//
// Multi-tier offer ladder:
//   num_tiers         -- how many price levels per side.
//   tier_spacing_bps  -- spread from mid-price for each tier, in basis points.
//   tier_size_pct     -- fraction of allocated capital placed at each tier.
//                        Values must sum to approximately 1.0.
// ---------------------------------------------------------------------------
struct StrategyConfig {
    double   gamma{0.01};
    double   kappa{1.5};
    double   phi{0.5};
    double   q_max{1000.0};
    double   min_profit_margin_bps{35.0};   // Never ask below cost + this.
    uint32_t offer_ttl_blocks{60};          // Cancel stale offers after N blocks.
    uint32_t num_tiers{4};                  // Tier count per side.
    std::vector<double> tier_spacing_bps;   // Length == num_tiers.
    std::vector<double> tier_size_pct;      // Length == num_tiers, sum ~= 1.0.

    /// Global cap on half-spread (bps) after all compounding multipliers.
    /// Prevents the multiplicative chain (regime * whale * VPIN * OFI * tactic)
    /// from producing effective market withdrawal.
    /// Default 250 bps half-spread = 500 bps round-trip = 5% total.
    double   max_half_spread_bps{250.0};

    /// On-chain fee per offer/cancel (mojos).  Default 0.00001 XCH.
    std::uint64_t offer_fee_mojos{10'000'000ULL};

    /// Number of blocks to observe in startup market-analysis mode before
    /// entering active trading.  0 = skip analysis.  Range [0, 1440].
    /// Example: 20 blocks ≈ 17 minutes at 52 s/block.
    uint32_t startup_analysis_blocks{0};

    /// [T4-02] Reorg protection: number of confirmations required before a
    /// fill is treated as final.  Fills detected at confirmed_at_index are
    /// held in a pending buffer until current_block - fill_block >= this
    /// value.  Default 6 blocks (~5 min at 52 s/block).  0 = instant.
    uint32_t confirmation_depth_blocks{6};

    /// [T4-11] How often (in blocks) to run full offer-state reconciliation
    /// between wallet RPC state and in-memory pending-offers map.
    /// Default 20 blocks (~17 min).  0 = disabled.
    uint32_t reconciliation_interval_blocks{20};

    /// [T7-10] Batch offer creation: merge same-side tiers for a pair into
    /// a single RPC call, reducing per-heartbeat transaction count from ~40
    /// to ~10 (one offer per side per pair).  The merged offer sums the
    /// mojo amounts across tiers.  All constituent tiers share the same
    /// offer ID for lifecycle tracking.
    /// false = current behavior (one offer per tier).
    /// true  = merge same-side tiers.
    bool     batch_offers_enabled{false};

    /// Minimum fraction of confirmed balance that must remain spendable
    /// before the engine will post new offers.  Range [0, 1].  Default 0.25.
    double   min_spendable_reserve_pct{0.25};

    /// Extra blocks beyond offer_ttl_blocks before an offer is considered
    /// "stuck" and eligible for forced cancellation + alerting.
    uint32_t stuck_offer_age_blocks{30};

    // -- Minimum balance management -----------------------------------------

    /// XCH to hold back from offer allocation for paying on-chain fees
    /// (offer cancellation / creation).  Deducted from the available
    /// capital pool in Step 7 before the tier ladder is built, so offers
    /// never lock the last `fee_reserve_xch` of spendable XCH.
    /// Default 1.0 XCH.
    double   fee_reserve_xch{1.0};

    /// Minimum spendable XCH required before posting offers (Step 8 gate).
    /// Unlike fee_reserve_xch (which protects trading inventory), this is
    /// the absolute minimum XCH the wallet must have to pay on-chain fees.
    /// If spendable XCH drops below this, offer posting is skipped.
    /// Set lower than fee_reserve_xch so fees can draw from the reserve
    /// without blocking trading.  Default 0.01 XCH (~3× typical fee).
    double   fee_min_spendable_xch{0.01};

    /// Minimum units of each asset to keep as reserve.  Offers on the
    /// side that would deplete an asset below this level are suppressed.
    /// Uses the pair's mojos_per_unit for conversion.  Default 1.0.
    double   min_reserve_units{1.0};

    /// Minimum units of each asset desired for active trading.  When an
    /// asset is below this level, the engine biases toward acquiring it
    /// by posting only buy-side offers.  Default 10.0.
    double   min_trading_units{10.0};

    /// When true, automatically post one-sided offers to acquire
    /// depleted assets when below min_trading_units.  Default true.
    bool     auto_rebalance_enabled{true};

    // -- Gap-aware dynamic tier spacing -------------------------------------

    /// Enable gap-aware dynamic tier spacing.
    bool     gap_aware_spacing{true};

    /// Minimum gap width (bps) in competing order book to target.
    double   min_gap_bps{50.0};

    /// Maximum distance from mid (bps) to scan for gaps.
    double   max_gap_scan_bps{1500.0};

    /// Blend factor for gap-directed spacing [0, 1].
    double   gap_blend_factor{0.6};

    // -- Adverse-selection-aware tier sizing ---------------------------------

    /// Enable adverse-selection-aware tier sizing.
    bool     adverse_selection_sizing{true};

    /// Decay factor for adverse-selection sizing (lower = more outer-heavy).
    double   adverse_selection_decay{0.7};

    /// Volatility threshold above which decay is halved.
    double   adverse_selection_sigma_threshold{0.05};

    // -- Fill-rate-weighted adaptive tier sizing ----------------------------

    /// Enable fill-rate-adaptive tier sizing.
    bool     fill_rate_sizing{true};

    /// Blend factor for fill-rate sizing [0, 1].
    double   fill_rate_blend{0.30};

    /// Lookback window in hours for fill-rate computation.
    int      fill_rate_lookback_hours{24};

    /// Minimum allocation fraction per tier when fill-rate sizing is active.
    double   fill_rate_min_pct{0.05};

    // -- AMM blend weight for market data feed ------------------------------

    /// Weight of TibetSwap AMM implied price in mid-price blend.
    double   amm_blend_weight{0.15};

    // -- Wall-aware retail niche pricing ------------------------------------

    /// Competing offers larger than this threshold (XCH) are classified as
    /// "walls".  The engine will not undercut walls in the competitive cap
    /// (Step 7) and will widen spreads to capture a retail niche premium
    /// (Step 5).  On Chia DEX, offers are atomic — small traders cannot
    /// take wall-sized offers and must use our smaller, accessible ones.
    /// Default 20.0 XCH.
    double   wall_size_threshold_xch{20.0};

    /// Spread widening factor when walls are detected.  Applied as a
    /// multiplier on total_spread_bps in Step 5.  Default 0.15 = 15%
    /// wider spreads targeting the captive retail market segment.
    double   wall_niche_premium_pct{0.15};

    // -- Cost-aware orphan evaluation (startup reconciliation) ---------------
    //
    // Scholarly basis:
    //   Guéant, Lehalle & Fernandez-Tapia (2013) — inventory-risk-aware
    //     cancellation: cancel cost vs. expected adverse selection loss.
    //   Gao & Wang (2020) — zero-offer gap during cancel→repost is the
    //     primary adverse selection cost for latent market makers.
    //   Aït-Sahalia & Saglam (2017) — stale-quote risk scales with price
    //     deviation, remaining lifetime, and offer size.
    //
    // When the engine restarts and discovers wallet offers it doesn't
    // track ("orphans"), the default behavior was to cancel them all.
    // This wastes fees and creates a zero-offer gap.  When enabled, the
    // engine evaluates each orphan's current market attractiveness and
    // adopts well-priced orphans instead of cancelling them.

    /// Master switch for cost-aware orphan evaluation.  When false,
    /// startup reconciliation cancels all orphans (legacy behavior).
    bool     orphan_adopt_enabled{true};

    /// Maximum adverse price deviation (fraction) to adopt an orphan.
    /// An orphan whose price has drifted adversely beyond this threshold
    /// is cancelled.  Default 0.02 (2%).  Adverse means: bid too high
    /// relative to current mid (overpaying) or ask too low (underselling).
    double   orphan_adverse_threshold{0.02};

    /// Maximum age in blocks for an adoptable orphan.  Offers older than
    /// this are cancelled regardless of price accuracy.  Default 120
    /// blocks (~104 minutes).  Prevents adopting offers with very old
    /// coin references that may fail on-chain.
    uint32_t orphan_max_adopt_age_blocks{120};

    /// Extra adverse-deviation tolerance (fraction) granted to orphans
    /// that would reduce the current inventory imbalance.  For example,
    /// if we are long and the orphan is an ask (sell), it helps rebalance
    /// inventory and gets this bonus before the threshold check.
    /// Default 0.01 (1% additional tolerance → effective 3% for helpers).
    double   orphan_inventory_bonus{0.01};
};

// ---------------------------------------------------------------------------
// Risk / inventory management thresholds.
//
// Percentages are expressed as fractions in [0, 1].
//   soft_limit_pct          -- begin aggressive quote skewing.
//   hard_limit_pct          -- pull quotes on overweight side.
//   single_cat_cap_pct      -- max portfolio fraction in any one CAT.
//   kelly_fraction          -- fraction of full Kelly to use (Half-Kelly = 0.5).
//   max_capital_per_pair_pct-- upper bound on capital allocated to one pair.
//
// Circuit breakers (ISO/IEC 27001:2022 §8.20 -- continuous risk monitoring):
//   max_drawdown_pct     -- peak-to-trough drawdown fraction that pauses the
//                           engine.  Default 10% (0.10).  Measures the drop
//                           from the all-time PnL high-water mark.
//   loss_window_blocks   -- rolling window size in blocks for the time-window
//                           loss circuit breaker.  Default 1152 blocks ≈ 10 h
//                           at the Chia mean block time of 52 s.
//   max_window_loss_bps  -- maximum loss (in basis points, i.e. 0.01 % per bp)
//                           permitted within the rolling window before the
//                           engine is paused.  Default 500 bps = 5 %.
//                           A value of 0 disables the window circuit breaker.
// ---------------------------------------------------------------------------
struct RiskConfig {
    double   soft_limit_pct{0.60};
    double   hard_limit_pct{0.80};
    double   single_cat_cap_pct{0.12};
    double   kelly_fraction{0.50};
    double   max_capital_per_pair_pct{0.20};

    // -- Circuit breakers ---------------------------------------------------
    double   max_drawdown_pct{0.10};        ///< HWM drawdown threshold (0,1].
    uint32_t drawdown_grace_blocks{100};    ///< Blocks to skip drawdown check at startup.
    uint32_t loss_window_blocks{1152};      ///< Rolling window size in blocks.
    double   max_window_loss_bps{500.0};    ///< Max loss in window (bps; 0=disabled).

    // -- Flash crash detection (T7-07, T7-08) --------------------------------
    double   flash_crash_threshold_pct{0.20};      ///< Drop % to trigger crash (0,1].
    uint32_t recovery_stable_blocks_phase1{50};     ///< Blocks stable for Crash→Recovery.
    uint32_t recovery_stable_blocks_phase2{100};    ///< Blocks stable for Recovery→Normal.
    double   recovery_stability_band_pct{0.05};     ///< Max price deviation in recovery.

    // -- Circuit-breaker rebalance (T7-09) -----------------------------------
    // Automatically enables StrategicLossManager for a pair when all of:
    //   1. inventory_ratio > circuit_breaker_hard_limit_ratio (one-sided)
    //   2. DriftAnalyzer recommends ManualRebalance or PullOverweight
    //   3. position_age > aging_start_blocks * circuit_breaker_age_multiplier
    // The loss is capped at circuit_breaker_max_loss_bps.
    bool     circuit_breaker_enabled{false};         ///< Master switch (opt-in).
    double   circuit_breaker_hard_limit_ratio{0.80}; ///< Inventory ratio trigger (0,1].
    double   circuit_breaker_age_multiplier{2.0};    ///< age >= aging_start * this.
    double   circuit_breaker_max_loss_bps{100.0};    ///< Max loss cap per rebalance [0,500].
};

// ---------------------------------------------------------------------------
// Yang-Zhang hybrid volatility estimator settings.
//
// lookback_blocks -- rolling window length in blocks (~52 s each).
// yz_alpha        -- blending weight for the YZ estimator (0, 1).
// ---------------------------------------------------------------------------
struct VolatilityConfig {
    uint32_t lookback_blocks{200};
    double   yz_alpha{0.34};

    /// [T5-CR6] Number of blocks to aggregate into a single OHLC candle
    /// before feeding the Yang-Zhang estimator.  With >90% of blocks
    /// producing degenerate (O=H=L=C) candles, aggregating N blocks into
    /// one proper candle dramatically improves the Rogers-Satchell component.
    /// Default 10 blocks (~8.7 min).  1 = no aggregation (legacy).
    uint32_t candle_aggregation_blocks{10};
};

// ---------------------------------------------------------------------------
// Observability: Prometheus metrics exporter and Telegram alert bot.
//
// telegram_bot_token and telegram_chat_id are SECRET and must not be logged.
// ---------------------------------------------------------------------------
struct MonitoringConfig {
    uint16_t    prometheus_port{9090};
    std::string telegram_bot_token;   // SECRET -- never log.
    std::string telegram_chat_id;     // SECRET -- never log.
};

// ---------------------------------------------------------------------------
// Persistent storage path (SQLite in Phase 1, PostgreSQL URI later).
// ---------------------------------------------------------------------------
struct DatabaseConfig {
    std::string path{"data/xop_trader.db"};
};

// ---------------------------------------------------------------------------
// Depeg detector configuration (applies to all stablecoin pairs globally).
// Individual thresholds are set per-pair in PairConfig.
// ---------------------------------------------------------------------------
struct DepegConfig {
    bool   enabled{true};                 // Master switch for depeg detection.
    double default_warn_pct{2.0};         // Default warn threshold (%).
    double default_bail_pct{10.0};        // Default bail threshold (%).
    uint32_t default_sustained_blocks{30};// Default sustained-blocks window.
    bool   auto_disable_pair{true};       // Automatically disable pair on bail.
    bool   alert_on_warn{true};           // Send Telegram alert on warning.
    bool   alert_on_bail{true};           // Send Telegram alert on bail.
};

// ---------------------------------------------------------------------------
// ArbitrageSettings -- YAML-configurable parameters for arbitrage detection.
//
// Maps 1:1 to the `arbitrage:` section of config.yaml.  These values are
// copied into the ArbitrageConfig struct (strategy/arbitrage.hpp) at engine
// construction time.  Keeping the YAML parsing separate from the strategy
// struct avoids a circular dependency between config.hpp and arbitrage.hpp.
// ---------------------------------------------------------------------------
struct ArbitrageSettings {
    bool     enabled{true};                 // Master switch for arb scanning.

    // -- Triangular arbitrage ------------------------------------------------
    double   triangular_min_profit_bps{30.0};
    double   triangular_slippage_bps{10.0};
    double   triangular_per_leg_fee_bps{5.0};
    uint32_t triangular_max_legs{3};

    // -- CEX-DEX arbitrage ---------------------------------------------------
    double   cex_dex_min_edge_bps{50.0};
    double   cex_dex_max_edge_bps{500.0};
    double   cex_fee_bps{10.0};
    double   bridge_fee_bps{0.0};

    // -- Cross-DEX arbitrage -------------------------------------------------
    double   cross_dex_min_edge_bps{15.0};
    double   tibetswap_fee_bps{70.0};
    double   dexie_fee_bps{0.0};

    // -- Cross-Bridge arbitrage ----------------------------------------------
    double   cross_bridge_min_edge_bps{20.0};
    double   bridge_cost_bps{15.0};

    // -- Crossed-book arbitrage (intra-DEX, Dexie has no matching engine) ----
    bool     crossed_book_enabled{true};
    double   crossed_book_min_edge_bps{10.0};
    double   crossed_book_max_take_xch{5.0};

    // -- General parameters --------------------------------------------------
    double   max_position_size{100.0};
    double   default_confidence{0.75};
    double   min_confidence_threshold{0.40};
    uint32_t default_urgency_blocks{5};
};

// ---------------------------------------------------------------------------
// CoinGecko external price reference -- free-tier API configuration.
//
// Provides CEX-grade mid-prices for assets that have CoinGecko listings.
// The free tier allows ~10-30 calls/min with no API key.  An optional
// api_key field supports the "Demo" plan (30 calls/min guaranteed).
//
// Asset mapping:
//   XCH          -> coingecko id "chia"
//   wmilliETH.b  -> coingecko id "ethereum" (price / 1000)
//   wmilliETH    -> coingecko id "ethereum" (price / 1000)
//   wUSDC.b      -> coingecko id "usd-coin" (~1.0)
//   BYC          -> no CoinGecko listing (DEX-only)
// ---------------------------------------------------------------------------
struct CoinGeckoConfig {
    bool        enabled{false};              // Master switch.

    /// Base URL for the CoinGecko API (no trailing slash).
    std::string base_url{"https://api.coingecko.com/api/v3"};

    /// CoinGecko coin IDs to fetch (e.g. "chia", "ethereum", "usd-coin").
    std::vector<std::string> coin_ids;

    /// How often to poll CoinGecko (milliseconds).  Free tier: 30-60 s.
    uint32_t    polling_interval_ms{30'000};

    /// HTTP request timeout.
    uint32_t    request_timeout_ms{15'000};

    /// TCP + TLS connect timeout.
    uint32_t    connect_timeout_ms{10'000};

    /// Maximum retries on 429 / 5xx.
    uint32_t    max_retries{3};

    /// Base delay between retries (exponential backoff).
    uint32_t    retry_base_delay_ms{1'000};

    /// Rate limiter: max requests per window.
    uint32_t    rate_limit_max_requests{10};

    /// Rate limiter: sliding window width (milliseconds).
    uint32_t    rate_limit_window_ms{60'000};

    /// Number of threads in the CURL worker pool.
    uint32_t    curl_thread_pool_size{2};

    /// Optional API key (CoinGecko Demo plan).  Empty = free tier.
    std::string api_key;

    /// User-Agent header.
    std::string user_agent{"XOPTrader-CoinGecko/1.0"};
};

// ---------------------------------------------------------------------------
// Fee budget tracking and dynamic fee selection.
//
// Controls two behaviours:
//   1. Fee-vs-gain gating: skip posting an offer when the blockchain fee
//      exceeds a configurable fraction of the expected gain from the trade.
//   2. Adaptive fee selection: track observed on-chain fees and try to pay
//      the minimum fee that achieves timely inclusion.
//
// When disabled (enabled == false), the static offer_fee_mojos from
// StrategyConfig is used in all fee sites (backward-compatible).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Inventory aging configuration (T4-09).
//
// Controls gradual relaxation of the no-loss constraint for positions that
// have been held for an extended period.  The rationale is that capital
// locked in a permanently-underwater position has an opportunity cost;
// accepting a small controlled loss to free it up can be net-positive.
//
// The effective margin discount grows linearly from 0 at aging_start_blocks
// to max_loss_relax_bps at the maximum aging horizon:
//   discount_bps = min(max_loss_relax_bps,
//                      (age - aging_start_blocks) * relax_rate_bps_per_block)
//   effective_margin = min_profit_margin_bps - discount_bps
//
// The effective margin is never allowed to go below -max_loss_relax_bps
// (i.e. the bot will never accept a loss larger than the configured cap).
// ---------------------------------------------------------------------------
struct InventoryAgingConfig {
    bool     enabled{false};                   // Master switch.

    /// Number of blocks an underwater position must age before relaxation
    /// begins.  Default 1000 blocks (~14.4 hours at 52 s/block).
    uint32_t aging_start_blocks{1000};

    /// Maximum allowed loss (in basis points) for aged positions.
    /// Default 50 bps (0.50%).  The effective margin will never go below
    /// -max_loss_relax_bps.
    double   max_loss_relax_bps{50.0};

    /// Rate at which the no-loss floor relaxes, in bps per block, once
    /// the position age exceeds aging_start_blocks.
    /// Default 0.05 bps/block => 50 bps max loss reached after ~1000 extra
    /// blocks (~14.4 hours after aging begins).
    double   relax_rate_bps_per_block{0.05};
};

struct FeeConfig {
    bool     enabled{false};                    // Master switch.

    /// Maximum total blockchain fees the bot may spend in a rolling 24 h
    /// window.  Default 10 000 000 000 mojos (0.01 XCH/day).
    std::uint64_t daily_budget_mojos{10'000'000'000ULL};

    /// Maximum acceptable ratio of fee-to-expected-gain per offer tier.
    /// If fee / expected_gain > this value, the tier is skipped.
    /// Default 0.30 (30%).  0.0 disables fee-vs-gain gating.
    double   fee_to_gain_max_ratio{0.30};

    /// Multiplier applied to the fee in the fee-vs-gain ratio check to
    /// account for the round-trip cost of posting + cancelling an offer.
    /// A value of 2.0 means the gate checks (2×fee)/gain, reflecting that
    /// every offer that doesn't fill will also incur a cancellation fee.
    /// With adaptive fees the cancel may cost more than the post, so
    /// values > 2.0 provide additional margin.  Default 2.0.
    double   cancel_cost_multiplier{2.0};

    /// Absolute fee floor (mojos).  The tracker will never recommend a fee
    /// below this value.  Default 5 000 000 (0.000005 XCH).
    std::uint64_t min_fee_mojos{5'000'000ULL};

    /// Absolute fee ceiling (mojos).  The tracker will never recommend a
    /// fee above this value.  Default 100 000 000 (0.0001 XCH).
    std::uint64_t max_fee_mojos{100'000'000ULL};

    /// When true, query the full node's get_fee_estimate RPC to adapt the
    /// fee dynamically based on mempool congestion.
    bool     adaptive_enabled{true};

    /// Rolling window (in blocks) over which cumulative fees are tracked
    /// for daily budget enforcement.  Default 1662 ≈ 24 h at 52 s/block.
    uint32_t fee_window_blocks{1662};

    /// Target inclusion time (seconds) passed to the full node's
    /// get_fee_estimate RPC.  Lower values request higher fees for faster
    /// inclusion; higher values allow the node to return cheaper estimates.
    /// Market-making offers are long-lived (offer_ttl_blocks ~60), so
    /// urgency is low.  Default 300 s (5 min).
    uint32_t fee_estimate_target_seconds{300};
};

// ---------------------------------------------------------------------------
// Market data aggregation configuration (T4-05).
//
// Exposes VPIN, OFI, whale detection, and competitor detection parameters
// that were previously only code-configurable.  All fields have sensible
// defaults matching MarketDataConfig in execution/market_data.hpp.
// ---------------------------------------------------------------------------
struct MarketDataSettings {
    // -- Whale detection ---------------------------------------------------
    /// Minimum trade size (mojos) to classify as a whale trade.
    /// Default: 50 XCH = 50e12 mojos.
    std::int64_t whale_trade_threshold{50LL * 1'000'000'000'000LL};

    /// Fraction of rolling 24h volume that triggers whale classification.
    double whale_volume_fraction{0.05};

    /// Blocks over which whale events are counted.
    uint32_t whale_window_blocks{10};

    /// Maximum spread multiplier during whale activity.
    double whale_max_spread_multiplier{3.0};

    // -- VPIN (flow toxicity) -----------------------------------------------
    /// Volume per VPIN bucket (base-asset units, e.g. XCH).
    double vpin_bucket_size{10.0};

    /// Number of completed buckets in the rolling VPIN window.
    uint32_t vpin_window_buckets{50};

    // -- OFI (order flow imbalance) ----------------------------------------
    /// Number of order-book snapshots for OFI computation.
    uint32_t ofi_window_size{20};

    // -- Competitor detection -----------------------------------------------
    /// Enable competitor tracking from order book data.
    bool enable_competitor_tracking{true};

    /// Minimum offer size (mojos) to consider as competitor.
    std::int64_t min_competitor_offer_size{1'000'000'000'000LL};

    /// Spread threshold (bps) that triggers a competitor alert.
    double competitor_alert_threshold_bps{50.0};

    // -- Asymmetric spread --------------------------------------------------
    /// Skew factor controlling whale-side asymmetry (0.0–1.0).
    double asymmetric_skew_factor{0.5};

    // -- CEX freshness ------------------------------------------------------
    /// Seconds before CEX data weight decays to zero.
    double cex_freshness_threshold_sec{120.0};
};

// ---------------------------------------------------------------------------
// Adverse selection (PIN model) configuration (T4-05).
// ---------------------------------------------------------------------------
struct AdverseSelectionSettings {
    double prior_alpha{2.0};         ///< Adverse fill pseudo-count.
    double prior_beta{8.0};          ///< Non-adverse fill pseudo-count.
    uint32_t observation_blocks{10}; ///< Post-fill observation window.
    double adverse_threshold{0.003}; ///< 30 bps adverse classification.
    uint32_t max_history{500};       ///< Rolling fill window.
    double decay_factor{0.0};        ///< Exponential decay in posterior.
};

// ---------------------------------------------------------------------------
// Dynamic market allocator configuration.
//
// Scores each enabled pair on five dimensions (spread, volume, competition,
// fill-rate, triangular-arb) and computes a target capital allocation
// fraction per pair.  Hysteresis and EMA smoothing prevent oscillation.
// ---------------------------------------------------------------------------
struct MarketAllocatorConfig {
    bool     enabled{false};                // Master switch.
    uint32_t eval_interval_blocks{50};      // Re-score every N blocks (~43 min).
    double   min_alloc_pct{0.10};           // Minimum per-pair (10%).
    double   max_alloc_pct{0.50};           // Maximum per-pair (50%).
    double   hysteresis_bps{50.0};          // Score change threshold to act.
    double   smooth_alpha{0.20};            // EMA smoothing (0,1].

    // Dimension weights (normalised internally).
    double   weight_spread{1.0};
    double   weight_volume{1.0};
    double   weight_competition{1.0};
    double   weight_fill_rate{1.0};
    double   weight_tri_arb{1.0};

    // Triangular arbitrage detection.
    double   tri_arb_fee_bps{15.0};         // Per-leg fee for arb calc.
    double   tri_arb_min_edge_bps{5.0};     // Minimum edge to score > 0.
};

// ---------------------------------------------------------------------------
// XCH Recovery Mode -- automatic XCH acquisition when balance critically low.
//
// When XCH spendable drops below `xch_low_threshold`, the engine enters
// recovery mode:
//   1. Cancels all outstanding offers (freeing locked coins).
//   2. Skips Steps 7-8 (no new market-making offers posted).
//   3. Monitors Dexie order books for reasonable XCH-selling asks on
//      XCH-base pairs (e.g. XCH/wUSDC.b) and takes them to acquire XCH.
//   4. Resumes normal trading once XCH spendable > `xch_recovery_target`.
//
// Fees are conserved: only cancellation + recovery takes consume fees.
// ---------------------------------------------------------------------------
struct RecoveryConfig {
    bool     enabled{true};                 // Master switch.
    double   xch_low_threshold{0.25};       // Enter recovery below this (XCH).
    double   xch_recovery_target{1.0};      // Exit recovery above this (XCH).
    double   max_take_per_block_xch{0.5};   // Max XCH to acquire per block.
    double   max_premium_bps{100.0};        // Max premium over CEX price to pay.
    bool     cancel_on_enter{true};         // Cancel all offers on entry.
};

// ---------------------------------------------------------------------------
// Top-level application configuration aggregating every section.
// ---------------------------------------------------------------------------
struct AppConfig {
    ChiaConfig       chia;
    DexieConfig      dexie;
    std::vector<PairConfig> pairs;
    StrategyConfig   strategy;
    RiskConfig       risk;
    VolatilityConfig volatility;
    MonitoringConfig monitoring;
    DatabaseConfig   database;
    DepegConfig      depeg;
    ArbitrageSettings arbitrage;
    CoinGeckoConfig  coingecko;
    FeeConfig        fees;
    InventoryAgingConfig inventory_aging;
    MarketDataSettings market_data;
    AdverseSelectionSettings adverse_selection;
    MarketAllocatorConfig market_allocator;
    RecoveryConfig   recovery;
};

// ---------------------------------------------------------------------------
// Load and fully validate a YAML configuration file, returning a populated
// AppConfig.  Throws xop::ConfigError on any structural or domain error.
//
// Tilde (~) prefixes in filesystem paths are expanded to the user's HOME.
// ---------------------------------------------------------------------------
AppConfig load_config(const std::string& path);

} // namespace xop

#endif // XOP_CONFIG_HPP
