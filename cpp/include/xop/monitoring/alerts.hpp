// alerts.hpp -- Telegram alert manager for XOPTrader CHIA DEX market-maker.
//
// Implements the three-tier alert system specified in Section 15 of the
// strategy document:
//
//   CRITICAL  -- Telegram DM, immediate delivery (node desync, wallet down,
//                exposure breach, flash crash).
//   WARNING   -- Telegram, throttled (spread widening, fill rate drop,
//                underwater position, concentration breach, drawdown,
//                offer creation failure).
//   INFO      -- Batched hourly (hourly PnL, daily summary, volume anomaly,
//                arbitrage opportunity).
//
// Rate limiting (prevents Telegram API abuse and operator alert fatigue):
//   CRITICAL  -- max 1 per 60 seconds
//   WARNING   -- max 1 per 300 seconds (5 minutes)
//   INFO      -- batched and flushed once per hour
//
// HTTP transport uses libcurl (async via a detached std::thread per send to
// avoid blocking the main trading loop).  The Telegram Bot API sendMessage
// endpoint is used for all message delivery.
//
// Security:
//   The bot_token is classified SECRET (ISO/IEC 27001:2022) and is never
//   written to log output.  The chat_id is also treated as SECRET.
//
// Thread safety:
//   All public methods are safe to call from any thread.  Internal state is
//   guarded by a single mutex; no method nests locks.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- secret classification, audit logging
//   ISO/IEC 5055       -- no raw pointers, RAII locking, bounds checks
//   ISO/IEC 25000      -- documented interfaces, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++17, no undefined behaviour

#ifndef XOP_MONITORING_ALERTS_HPP
#define XOP_MONITORING_ALERTS_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// AlertTier -- severity classification for alert messages.
//
// Ordered by severity so that numeric comparison is meaningful.
// ---------------------------------------------------------------------------

enum class AlertTier : std::uint8_t {
    INFO     = 0, // Batched hourly (daily PnL, volume anomaly).
    WARNING  = 1, // Throttled per-tier (spread widening, fill rate drop).
    CRITICAL = 2  // Immediate, rate-limited (node desync, wallet down).
};

/// Human-readable label for logging and message formatting.
const char* to_string(AlertTier tier) noexcept;

// ---------------------------------------------------------------------------
// AlertRule -- unique identifier for each of the 14 alert rules.
//
// Enum values are stable (used as map keys and in rate-limit tracking).
// ---------------------------------------------------------------------------

enum class AlertRule : std::uint8_t {
    // CRITICAL (rules 1-4)
    NodeDesync          = 1,  // Node > 5 blocks behind chain tip.
    WalletUnreachable   = 2,  // Wallet RPC connection failed.
    ExposureBreach      = 3,  // Inventory exceeds hard_limit_pct.
    FlashCrash          = 4,  // Price drop > 20% detected.

    // WARNING (rules 5-10)
    FillRateDrop        = 5,  // Fill rate < 50% of 24h average.
    SpreadWidening      = 6,  // Spread > 2x normal for the pair.
    UnderwaterPosition  = 7,  // Cost basis > market price on held asset.
    ConcentrationBreach = 8,  // Single asset > 12% of portfolio.
    PnlDrawdown         = 9,  // PnL drawdown exceeds 5% of peak.
    OfferCreationFail   = 10, // Consecutive offer creation failures.

    // INFO (rules 11-14)
    HourlyPnl           = 11, // Hourly PnL summary.
    DailyPnl            = 12, // Daily PnL summary.
    NewPairVolume        = 13, // Volume detected on a newly listed pair.
    ArbitrageDetected    = 14  // Cross-venue or cross-bridge arb opportunity.
};

/// Human-readable label for logging.
const char* to_string(AlertRule rule) noexcept;

/// Map each rule to its severity tier.
AlertTier tier_for_rule(AlertRule rule) noexcept;

// ---------------------------------------------------------------------------
// BotState -- aggregated runtime state snapshot consumed by check_and_alert().
//
// The caller assembles this from xop::State, xop::InventoryTracker, and
// system health probes, then passes it to the alert manager for rule
// evaluation.
// ---------------------------------------------------------------------------

struct BotState {
    // System health (Dashboard 4).
    BlockHeight current_block_height;   // Our node's latest block.
    BlockHeight network_block_height;   // Network tip (from peer info).
    bool        wallet_connected;       // True if wallet RPC responded.

    // Offer lifecycle (Dashboard 5).
    double fill_rate_per_hour;          // Rolling fills per hour.
    double avg_fill_rate_24h;           // 24-hour average fill rate.
    std::uint32_t consecutive_offer_failures; // Consecutive create_offer failures.

    // Market data (Dashboard 3).
    struct PairState {
        std::string pair_name;
        double      current_spread_bps; // Current spread in basis points.
        double      normal_spread_bps;  // Rolling average spread (baseline).
        Mojo        mid_price;          // Current mid-price.
        Mojo        recent_high;        // Highest mid-price in lookback window.
    };
    std::vector<PairState> pairs;

    // Inventory / risk (Dashboards 2 & 6).
    struct AssetState {
        AssetId asset_id;
        Mojo    balance;
        Mojo    cost_basis;
        Mojo    market_price;        // Current market price for this asset.
        double  portfolio_pct;       // Fraction of total portfolio [0, 1].
    };
    std::vector<AssetState> assets;

    // PnL (Dashboard 1).
    Mojo   total_pnl;          // Current total PnL.
    Mojo   peak_pnl;           // High-water mark PnL.

    // Inventory exposure.
    double max_inventory_ratio; // Highest inventory_ratio across all pairs.
    double hard_limit_pct;      // Hard limit threshold from RiskConfig.
};

// ---------------------------------------------------------------------------
// DailySummary -- data for the daily summary alert.
// ---------------------------------------------------------------------------

struct DailySummary {
    Mojo   total_pnl;           // Net PnL for the day.
    Mojo   realized_pnl;        // Realized component.
    Mojo   unrealized_pnl;      // Unrealized component.
    std::uint64_t fills_count;  // Total fills in the period.
    std::uint64_t offers_posted;// Total offers posted.
    double avg_spread_bps;      // Average spread across all pairs.
    double fill_rate;           // Average fills per hour.
    double nhe;                 // Natural Hedge Efficiency.
};

// ---------------------------------------------------------------------------
// AlertManager -- Telegram-based alert dispatcher with rate limiting.
//
// Lifecycle:
//   1. Construct (no-op).
//   2. Call init(bot_token, chat_id) to configure the Telegram endpoint.
//   3. Call check_and_alert(state) from the main loop on each heartbeat.
//   4. Call flush_info_batch() hourly to deliver batched INFO alerts.
//   5. Call send_daily_summary() once per day.
//   6. No explicit shutdown needed; destructor is safe.
//
// If init() is called with empty credentials, all send operations become
// no-ops (alerts are logged via spdlog but not delivered).
// ---------------------------------------------------------------------------

class AlertManager {
public:
    AlertManager();
    ~AlertManager();

    // Non-copyable, non-movable.
    AlertManager(const AlertManager&) = delete;
    AlertManager& operator=(const AlertManager&) = delete;
    AlertManager(AlertManager&&) = delete;
    AlertManager& operator=(AlertManager&&) = delete;

    // -- Lifecycle -----------------------------------------------------------

    /// Configure the Telegram Bot API endpoint.
    /// If bot_token or chat_id is empty, alerts are disabled (log-only mode).
    void init(const std::string& bot_token, const std::string& chat_id);

    /// True if Telegram delivery is enabled (both token and chat_id are set).
    bool is_enabled() const noexcept;

    // -- Alert Dispatch ------------------------------------------------------

    /// Send a single alert message at the given severity tier.
    /// Respects per-tier rate limits:
    ///   CRITICAL -- max 1 per 60 seconds.
    ///   WARNING  -- max 1 per 300 seconds.
    ///   INFO     -- queued into the hourly batch buffer.
    ///
    /// If the rate limit has not elapsed, the message is silently dropped
    /// (but logged at debug level for audit).
    void send_alert(AlertTier tier, const std::string& message);

    /// Send a pre-formatted daily summary.
    /// Bypasses rate limiting (it is itself a once-per-day event).
    void send_daily_summary(const DailySummary& summary);

    /// Flush all queued INFO-tier messages as a single batched Telegram
    /// message.  Should be called once per hour by the main loop.
    void flush_info_batch();

    // -- Rule Engine ---------------------------------------------------------

    /// Evaluate all 14 alert rules against the current bot state and fire
    /// any alerts whose conditions are met (subject to rate limiting).
    ///
    /// This is the primary entry point for the per-block heartbeat.
    void check_and_alert(const BotState& state);

    // -- Configuration -------------------------------------------------------

    /// Override the default rate-limit cooldown for a tier (for testing).
    void set_cooldown(AlertTier tier, std::chrono::seconds cooldown);

    /// Override the threshold for a specific rule (for testing or tuning).
    /// Thresholds are rule-specific; see the implementation for semantics.
    void set_threshold(AlertRule rule, double value);

private:
    // -- Telegram HTTP transport ---------------------------------------------

    /// Post a message to the Telegram Bot API (sendMessage endpoint).
    /// Executed asynchronously on a detached thread.  Failures are logged
    /// but do not propagate exceptions to the caller.
    ///
    /// @param text  UTF-8 message body (Telegram MarkdownV2 or plain text).
    void post_telegram(const std::string& text);

    // -- Rate limiting -------------------------------------------------------

    /// Return true if enough time has elapsed since the last alert of this
    /// tier to permit sending.  Updates the timestamp if permitted.
    bool check_rate_limit(AlertTier tier);

    // -- Rule evaluation helpers (one per rule) ------------------------------

    void check_node_desync(const BotState& state);
    void check_wallet_unreachable(const BotState& state);
    void check_exposure_breach(const BotState& state);
    void check_flash_crash(const BotState& state);
    void check_fill_rate_drop(const BotState& state);
    void check_spread_widening(const BotState& state);
    void check_underwater_positions(const BotState& state);
    void check_concentration_breach(const BotState& state);
    void check_pnl_drawdown(const BotState& state);
    void check_offer_creation_fail(const BotState& state);

    // -- Internal data -------------------------------------------------------

    mutable std::mutex mtx_;

    // Telegram credentials (SECRET -- never logged).
    std::string bot_token_;
    std::string chat_id_;
    bool        enabled_{false};

    // Per-tier rate-limit cooldowns.
    std::chrono::seconds cooldown_critical_{60};
    std::chrono::seconds cooldown_warning_{300};
    // INFO tier uses hourly batching, not per-message cooldown.

    // Timestamps of the last successfully sent alert per tier.
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    std::unordered_map<std::uint8_t, TimePoint> last_sent_;

    // INFO-tier batch buffer.
    std::vector<std::string> info_batch_;

    // Per-rule configurable thresholds (indexed by AlertRule cast to uint8_t).
    // Defaults are set in the constructor.
    std::unordered_map<std::uint8_t, double> thresholds_;

    // -- Default threshold constants -----------------------------------------

    // Rule 1: Node desync -- blocks behind before CRITICAL.
    static constexpr double kDefaultDesyncBlocks        = 5.0;
    // Rule 4: Flash crash -- fractional price drop threshold.
    static constexpr double kDefaultFlashCrashPct       = 0.20;
    // Rule 5: Fill rate -- fraction of 24h average below which WARNING fires.
    static constexpr double kDefaultFillRateDropPct     = 0.50;
    // Rule 6: Spread widening -- multiplier of normal spread.
    static constexpr double kDefaultSpreadWideningMult  = 2.0;
    // Rule 8: Concentration -- maximum portfolio fraction per single asset.
    static constexpr double kDefaultConcentrationCap    = 0.12;
    // Rule 9: PnL drawdown -- fractional drawdown from peak.
    static constexpr double kDefaultDrawdownPct         = 0.05;
    // Rule 10: Consecutive offer failures before WARNING.
    static constexpr double kDefaultOfferFailCount      = 3.0;
};

}  // namespace xop

#endif  // XOP_MONITORING_ALERTS_HPP
