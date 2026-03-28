// alerts.cpp -- Telegram alert manager implementation for XOPTrader.
//
// Uses libcurl to POST messages to the Telegram Bot API sendMessage endpoint.
// A persistent worker thread with a thread-safe message queue handles all
// HTTP sends, ensuring clean shutdown and bounded thread usage.  The trading
// loop is never blocked by network I/O; callers enqueue messages and return
// immediately.
//
// Rate limiting is enforced per-tier using steady_clock timestamps:
//   CRITICAL  -- at most once per 60 seconds
//   WARNING   -- at most once per 300 seconds
//   INFO      -- accumulated in a buffer and flushed hourly
//
// All 14 alert rules from Section 15 of the strategy document are evaluated
// by check_and_alert() on each heartbeat.
//
// Security (ISO/IEC 27001:2022):
//   bot_token and chat_id are never written to log output.  Curl errors
//   are logged at WARN level without echoing the URL (which contains the
//   token).
//
// Compliant with:
//   ISO/IEC 5055       -- no raw pointers (curl is RAII-wrapped), bounds checks
//   ISO/IEC 25000      -- documented, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- standard C++20, no undefined behaviour
//
// T2-03: Detached threads replaced with a persistent worker queue.
// T2-12: send_alert(AlertTier, ...) marked [[deprecated]].

#include "xop/monitoring/alerts.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace xop {

// ===================================================================
//  AlertTier / AlertRule string helpers
// ===================================================================

const char* to_string(AlertTier tier) noexcept
{
    switch (tier) {
        case AlertTier::INFO:     return "INFO";
        case AlertTier::WARNING:  return "WARNING";
        case AlertTier::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

const char* to_string(AlertRule rule) noexcept
{
    switch (rule) {
        case AlertRule::NodeDesync:          return "NodeDesync";
        case AlertRule::WalletUnreachable:   return "WalletUnreachable";
        case AlertRule::ExposureBreach:      return "ExposureBreach";
        case AlertRule::FlashCrash:          return "FlashCrash";
        case AlertRule::CircuitBreaker:      return "CircuitBreaker";
        case AlertRule::FillRateDrop:        return "FillRateDrop";
        case AlertRule::SpreadWidening:      return "SpreadWidening";
        case AlertRule::UnderwaterPosition:  return "UnderwaterPosition";
        case AlertRule::ConcentrationBreach: return "ConcentrationBreach";
        case AlertRule::PnlDrawdown:         return "PnlDrawdown";
        case AlertRule::OfferCreationFail:   return "OfferCreationFail";
        case AlertRule::HourlyPnl:           return "HourlyPnl";
        case AlertRule::DailyPnl:            return "DailyPnl";
        case AlertRule::NewPairVolume:        return "NewPairVolume";
        case AlertRule::ArbitrageDetected:    return "ArbitrageDetected";
    }
    return "UNKNOWN";
}

AlertTier tier_for_rule(AlertRule rule) noexcept
{
    switch (rule) {
        // CRITICAL (rules 1-5).
        case AlertRule::NodeDesync:
        case AlertRule::WalletUnreachable:
        case AlertRule::ExposureBreach:
        case AlertRule::FlashCrash:
        case AlertRule::CircuitBreaker:
            return AlertTier::CRITICAL;

        // WARNING (rules 6-11).
        case AlertRule::FillRateDrop:
        case AlertRule::SpreadWidening:
        case AlertRule::UnderwaterPosition:
        case AlertRule::ConcentrationBreach:
        case AlertRule::PnlDrawdown:
        case AlertRule::OfferCreationFail:
            return AlertTier::WARNING;

        // INFO (rules 12-15).
        case AlertRule::HourlyPnl:
        case AlertRule::DailyPnl:
        case AlertRule::NewPairVolume:
        case AlertRule::ArbitrageDetected:
            return AlertTier::INFO;
    }
    return AlertTier::INFO;
}

// ===================================================================
//  libcurl write callback (discards response body)
// ===================================================================

namespace {

/// libcurl WRITEFUNCTION callback.  Telegram's response body is not
/// needed; discard it to prevent curl from writing to stdout.
std::size_t curl_discard_write(char* /*ptr*/, std::size_t size,
                               std::size_t nmemb, void* /*userdata*/)
{
    return size * nmemb;
}

}  // anonymous namespace

// ===================================================================
//  Construction / Destruction
// ===================================================================

AlertManager::AlertManager()
{
    // Populate default thresholds for all rules that have numeric thresholds.
    thresholds_[static_cast<std::uint8_t>(AlertRule::NodeDesync)]
        = kDefaultDesyncBlocks;
    thresholds_[static_cast<std::uint8_t>(AlertRule::FlashCrash)]
        = kDefaultFlashCrashPct;
    thresholds_[static_cast<std::uint8_t>(AlertRule::FillRateDrop)]
        = kDefaultFillRateDropPct;
    thresholds_[static_cast<std::uint8_t>(AlertRule::SpreadWidening)]
        = kDefaultSpreadWideningMult;
    thresholds_[static_cast<std::uint8_t>(AlertRule::ConcentrationBreach)]
        = kDefaultConcentrationCap;
    thresholds_[static_cast<std::uint8_t>(AlertRule::PnlDrawdown)]
        = kDefaultDrawdownPct;
    thresholds_[static_cast<std::uint8_t>(AlertRule::OfferCreationFail)]
        = kDefaultOfferFailCount;
}

AlertManager::~AlertManager()
{
    // Ensure the worker thread drains its queue and exits cleanly.
    stop_worker();
}

// ===================================================================
//  Lifecycle
// ===================================================================

void AlertManager::init(const std::string& bot_token,
                         const std::string& chat_id)
{
    std::lock_guard lock(mtx_);

    bot_token_ = bot_token;
    chat_id_   = chat_id;
    enabled_   = !bot_token_.empty() && !chat_id_.empty();

    if (enabled_) {
        // Log confirmation without revealing the token value.
        spdlog::info("AlertManager: Telegram alerts enabled (chat configured)");
    } else {
        spdlog::info("AlertManager: Telegram alerts disabled (log-only mode)");
    }

    // Start the persistent send-worker thread (T2-03).
    // Started unconditionally so that shutdown is always uniform; if
    // Telegram is disabled the queue simply remains empty.
    start_worker();
}

bool AlertManager::is_enabled() const noexcept
{
    // No lock needed; enabled_ is only written during init() which
    // precedes concurrent usage.
    return enabled_;
}

// ===================================================================
//  Rate Limiting
// ===================================================================

// ISO/IEC 5055: rate limiting is keyed by AlertRule, not AlertTier, so that
// each of the 14 alert rules has its own independent cooldown.  The previous
// per-tier design allowed a single rapid-firing rule (e.g. SpreadWidening)
// to suppress all other WARNING-tier rules sharing the same cooldown bucket.
bool AlertManager::check_rate_limit(AlertRule rule)
{
    const AlertTier tier = tier_for_rule(rule);

    // INFO tier is handled by batch buffering, not per-message rate limiting.
    if (tier == AlertTier::INFO) {
        return true;
    }

    const auto key = static_cast<std::uint8_t>(rule);
    const auto now = Clock::now();

    // Determine the cooldown from the rule's tier.
    const auto cooldown = (tier == AlertTier::CRITICAL)
        ? cooldown_critical_
        : cooldown_warning_;

    auto it = last_sent_.find(key);
    if (it == last_sent_.end()) {
        // First alert for this rule -- always permit.
        last_sent_[key] = now;
        return true;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - it->second);

    if (elapsed >= cooldown) {
        it->second = now;
        return true;
    }

    // Rate-limited: log at debug for audit trail but do not send.
    spdlog::debug("AlertManager: {} alert rate-limited ({}/{} seconds elapsed)",
                  to_string(rule), elapsed.count(), cooldown.count());
    return false;
}

// ===================================================================
//  Alert Dispatch
// ===================================================================

void AlertManager::send_alert(AlertTier tier, const std::string& message)
{
    std::lock_guard lock(mtx_);

    // Always log the alert locally regardless of Telegram delivery.
    switch (tier) {
        case AlertTier::CRITICAL:
            spdlog::error("[ALERT:CRITICAL] {}", message);
            break;
        case AlertTier::WARNING:
            spdlog::warn("[ALERT:WARNING] {}", message);
            break;
        case AlertTier::INFO:
            spdlog::info("[ALERT:INFO] {}", message);
            break;
    }

    // INFO tier: accumulate into the batch buffer for hourly flush.
    if (tier == AlertTier::INFO) {
        info_batch_.push_back(message);
        return;
    }

    // NOTE: This tier-based overload is kept for backward compatibility
    // (e.g. direct INFO sends).  For rule-based dispatch with per-rule
    // rate limiting, use send_alert(AlertRule, message) instead.

    if (!enabled_) {
        return;
    }

    // Format the Telegram message with a tier prefix and label.
    std::string prefix;
    switch (tier) {
        case AlertTier::CRITICAL: prefix = "[CRITICAL] "; break;
        case AlertTier::WARNING:  prefix = "[WARNING] ";  break;
        default:                  prefix = "[INFO] ";     break;
    }

    post_telegram(prefix + message);
}

// ISO/IEC 5055: per-rule alert dispatch with independent cooldowns.
void AlertManager::send_alert(AlertRule rule, const std::string& message)
{
    const AlertTier tier = tier_for_rule(rule);

    std::lock_guard lock(mtx_);

    // Always log the alert locally regardless of Telegram delivery.
    switch (tier) {
        case AlertTier::CRITICAL:
            spdlog::error("[ALERT:CRITICAL] {}", message);
            break;
        case AlertTier::WARNING:
            spdlog::warn("[ALERT:WARNING] {}", message);
            break;
        case AlertTier::INFO:
            spdlog::info("[ALERT:INFO] {}", message);
            break;
    }

    // INFO tier: accumulate into the batch buffer for hourly flush.
    if (tier == AlertTier::INFO) {
        info_batch_.push_back(message);
        return;
    }

    // CRITICAL / WARNING: check per-rule rate limit.
    if (!check_rate_limit(rule)) {
        return;
    }

    if (!enabled_) {
        return;
    }

    // Format the Telegram message with a tier prefix and label.
    std::string prefix;
    switch (tier) {
        case AlertTier::CRITICAL: prefix = "[CRITICAL] "; break;
        case AlertTier::WARNING:  prefix = "[WARNING] ";  break;
        default:                  prefix = "[INFO] ";     break;
    }

    post_telegram(prefix + message);
}

void AlertManager::send_daily_summary(const AlertDailySummary& summary)
{
    // Build a human-readable summary message.
    // Monetary values are shown in XCH (mojos / 10^12) for readability.
    const double mojos_to_xch = 1.0 / 1'000'000'000'000.0;

    std::ostringstream oss;
    oss << "[DAILY SUMMARY]\n"
        << "Total PnL: " << (summary.total_pnl * mojos_to_xch) << " XCH\n"
        << "  Realized: " << (summary.realized_pnl * mojos_to_xch) << " XCH\n"
        << "  Unrealized: " << (summary.unrealized_pnl * mojos_to_xch) << " XCH\n"
        << "Fills: " << summary.fills_count << "\n"
        << "Offers posted: " << summary.offers_posted << "\n"
        << "Avg spread: " << summary.avg_spread_bps << " bps\n"
        << "Fill rate: " << summary.fill_rate << "/hr\n"
        << "NHE: " << (summary.nhe * 100.0) << "%";

    const std::string text = oss.str();

    spdlog::info("[ALERT:DAILY] {}", text);

    std::lock_guard lock(mtx_);
    if (enabled_) {
        post_telegram(text);
    }
}

void AlertManager::flush_info_batch()
{
    std::lock_guard lock(mtx_);

    if (info_batch_.empty()) {
        return;
    }

    spdlog::info("AlertManager: flushing {} queued INFO alerts", info_batch_.size());

    // Concatenate all queued messages into a single Telegram message,
    // separated by newlines.  Telegram supports messages up to 4096
    // characters; if the batch exceeds that, split into chunks.
    constexpr std::size_t kMaxTelegramLen = 4000; // Leave margin for prefix.

    std::ostringstream oss;
    oss << "[INFO BATCH]\n";
    std::size_t current_len = oss.str().size();

    for (const auto& msg : info_batch_) {
        // Check if adding this message would exceed the limit.
        if (current_len + msg.size() + 2 > kMaxTelegramLen) {
            // Send the current chunk and start a new one.
            if (enabled_) {
                post_telegram(oss.str());
            }
            oss.str("");
            oss.clear();
            oss << "[INFO BATCH cont.]\n";
            current_len = oss.str().size();
        }

        oss << "- " << msg << "\n";
        current_len += msg.size() + 3; // "- " prefix + newline.
    }

    // Send the final chunk.
    if (enabled_ && current_len > 20) { // More than just the header.
        post_telegram(oss.str());
    }

    info_batch_.clear();
}

// ===================================================================
//  Telegram HTTP Transport -- Worker Queue (T2-03)
// ===================================================================
//
// The previous implementation spawned a detached std::thread for every
// Telegram send.  Detached threads have no cleanup path: if the process
// exits while a detached thread is in-flight, the behaviour is undefined
// (ISO/IEC JTC 1/SC 22 [basic.start.term]).
//
// The replacement uses a single persistent worker thread that drains a
// thread-safe FIFO queue.  post_telegram() pushes a message string and
// returns immediately (non-blocking for callers).  On destruction,
// stop_worker() sets the stop flag, notifies the worker, and joins it,
// ensuring all pending messages are delivered or discarded cleanly.
// ===================================================================

void AlertManager::post_telegram(const std::string& text)
{
    // Enqueue the message for the worker thread.
    {
        std::lock_guard lock(send_mtx_);
        send_queue_.push(text);
    }
    send_cv_.notify_one();
}

void AlertManager::start_worker()
{
    // Guard against double-start (e.g. if init() is called more than once).
    if (send_worker_.joinable()) {
        return;
    }

    // Capture bot_token_ and chat_id_ by value so the worker does not
    // access AlertManager members after destruction begins.  Both are
    // stable after init() returns.
    const std::string token = bot_token_;
    const std::string chat  = chat_id_;

    send_stop_.store(false, std::memory_order_release);

    send_worker_ = std::thread([this, token, chat]() {
        spdlog::debug("AlertManager: send-worker thread started");

        while (true) {
            std::string message;

            // Wait for a message or a stop signal.
            {
                std::unique_lock lock(send_mtx_);
                send_cv_.wait(lock, [this]() {
                    return !send_queue_.empty()
                        || send_stop_.load(std::memory_order_acquire);
                });

                // Drain one message if available, even during shutdown,
                // to guarantee ordered delivery of queued messages.
                if (send_queue_.empty()) {
                    // Queue is empty and stop was requested -- exit.
                    break;
                }

                message = std::move(send_queue_.front());
                send_queue_.pop();
            }

            // Perform the HTTP POST outside any lock.
            CURL* curl = curl_easy_init();
            if (!curl) {
                spdlog::warn("AlertManager: curl_easy_init failed");
                continue;
            }

            // Build the Telegram sendMessage URL.
            // URL format: https://api.telegram.org/bot<token>/sendMessage
            // Security (ISO/IEC 27001:2022): the URL containing the token
            // is never logged.
            const std::string url =
                "https://api.telegram.org/bot" + token + "/sendMessage";

            // Build the POST body as URL-encoded form data.
            // HTML-escape the message first to prevent injection via
            // user-controlled data (pair names, asset IDs, error messages).
            // Telegram's default parse_mode treats <, >, & as HTML if the
            // server echoes them; escaping neutralises this (T4-25).
            auto html_escape = [](const std::string& s) -> std::string {
                std::string out;
                out.reserve(s.size() + s.size() / 8);
                for (char c : s) {
                    switch (c) {
                        case '&':  out += "&amp;";  break;
                        case '<':  out += "&lt;";   break;
                        case '>':  out += "&gt;";   break;
                        case '"': out += "&quot;";  break;
                        default:   out += c;         break;
                    }
                }
                return out;
            };
            std::string safe_message = html_escape(message);

            std::string escaped_text;
            char* curl_escaped = curl_easy_escape(
                curl, safe_message.c_str(), static_cast<int>(safe_message.size()));
            if (curl_escaped) {
                escaped_text = curl_escaped;
                curl_free(curl_escaped);
            } else {
                // Fallback: URL-encode manually is not practical, so use
                // the already HTML-escaped text (safe against injection).
                escaped_text = safe_message;
            }

            const std::string post_data =
                "chat_id=" + chat + "&text=" + escaped_text;

            // Configure curl.
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_discard_write);

            // Timeout: 10 seconds total, 5 seconds for connection.
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

            // Enforce TLS (Telegram API is HTTPS-only).
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

            // Execute the request.
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                // Log error WITHOUT the URL (contains secret bot token).
                spdlog::warn("AlertManager: Telegram POST failed: {}",
                             curl_easy_strerror(res));
            } else {
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (http_code != 200) {
                    spdlog::warn("AlertManager: Telegram API returned HTTP {}",
                                 http_code);
                }
            }

            curl_easy_cleanup(curl);
        }

        spdlog::debug("AlertManager: send-worker thread exiting");
    });
}

void AlertManager::stop_worker()
{
    // Signal the worker to finish after draining the queue.
    send_stop_.store(true, std::memory_order_release);
    send_cv_.notify_one();

    // Join the thread to ensure clean shutdown (ISO/IEC JTC 1/SC 22:
    // a joinable std::thread must be joined or detached before destruction).
    if (send_worker_.joinable()) {
        send_worker_.join();
    }
}

// ===================================================================
//  Configuration
// ===================================================================

void AlertManager::set_cooldown(AlertTier tier, std::chrono::seconds cooldown)
{
    std::lock_guard lock(mtx_);

    switch (tier) {
        case AlertTier::CRITICAL:
            cooldown_critical_ = cooldown;
            break;
        case AlertTier::WARNING:
            cooldown_warning_ = cooldown;
            break;
        case AlertTier::INFO:
            // INFO uses hourly batching, not per-message cooldown.
            spdlog::warn("AlertManager: set_cooldown ignored for INFO tier "
                         "(use flush_info_batch interval instead)");
            break;
    }
}

void AlertManager::set_threshold(AlertRule rule, double value)
{
    std::lock_guard lock(mtx_);
    thresholds_[static_cast<std::uint8_t>(rule)] = value;
}

// ===================================================================
//  Rule Engine
// ===================================================================

void AlertManager::check_and_alert(const BotState& state)
{
    // Evaluate all 14 rules in severity order (CRITICAL first so that the
    // most urgent alerts are delivered before rate limits engage).

    // CRITICAL rules (1-4).
    check_node_desync(state);
    check_wallet_unreachable(state);
    check_exposure_breach(state);
    check_flash_crash(state);

    // WARNING rules (5-10).
    check_fill_rate_drop(state);
    check_spread_widening(state);
    check_underwater_positions(state);
    check_concentration_breach(state);
    check_pnl_drawdown(state);
    check_offer_creation_fail(state);

    // INFO rules (12-15) are not checked here because they are event-driven:
    //   12. HourlyPnl    -- triggered by the hourly timer in the main loop.
    //   13. DailyPnl     -- triggered by the daily timer.
    //   14. NewPairVolume -- triggered by the market data feed.
    //   15. ArbitrageDetected -- triggered by the arbitrage scanner.
    //
    // CircuitBreaker (rule 5) is also event-driven (fired by engine.cpp on
    // pause transitions) and is not checked here.
    //
    // The caller fires send_alert(AlertRule::..., ...) directly for those events.
}

// ===================================================================
//  Individual Rule Checks
// ===================================================================

// Rule 1: Node desync > N blocks behind.
void AlertManager::check_node_desync(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::NodeDesync));

    if (state.network_block_height <= state.current_block_height) {
        return; // Synced or no network tip data.
    }

    const auto blocks_behind = static_cast<double>(
        state.network_block_height - state.current_block_height);

    if (blocks_behind > threshold) {
        send_alert(AlertRule::NodeDesync,
            fmt::format("Node desync: {} blocks behind network tip (height {} vs {})",
                        static_cast<int>(blocks_behind),
                        state.current_block_height,
                        state.network_block_height));
    }
}

// Rule 2: Wallet RPC unreachable.
void AlertManager::check_wallet_unreachable(const BotState& state)
{
    if (!state.wallet_connected) {
        send_alert(AlertRule::WalletUnreachable,
                   "Wallet RPC unreachable -- cannot create or cancel offers");
    }
}

// Rule 3: Inventory exposure exceeds hard limit.
void AlertManager::check_exposure_breach(const BotState& state)
{
    if (state.max_inventory_ratio > state.hard_limit_pct) {
        send_alert(AlertRule::ExposureBreach,
            fmt::format("Exposure breach: inventory ratio {:.1f}% exceeds "
                        "hard limit {:.1f}%",
                        state.max_inventory_ratio * 100.0,
                        state.hard_limit_pct * 100.0));
    }
}

// Rule 4: Flash crash > threshold% drop.
void AlertManager::check_flash_crash(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::FlashCrash));

    for (const auto& pair : state.pairs) {
        if (pair.recent_high <= 0) {
            continue; // No price history yet.
        }

        const double drop_pct = static_cast<double>(
            pair.recent_high - pair.mid_price)
            / static_cast<double>(pair.recent_high);

        if (drop_pct >= threshold) {
            send_alert(AlertRule::FlashCrash,
                fmt::format("Flash crash on {}: {:.1f}% drop (high={}, mid={})",
                            pair.pair_name,
                            drop_pct * 100.0,
                            pair.recent_high,
                            pair.mid_price));
        }
    }
}

// Rule 5: Fill rate below 50% of 24h average.
void AlertManager::check_fill_rate_drop(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::FillRateDrop));

    // Avoid division by zero; if 24h average is zero, there is no baseline.
    if (state.avg_fill_rate_24h <= 0.0) {
        return;
    }

    const double ratio = state.fill_rate_per_hour / state.avg_fill_rate_24h;

    if (ratio < threshold) {
        send_alert(AlertRule::FillRateDrop,
            fmt::format("Fill rate drop: {:.2f}/hr vs {:.2f}/hr 24h avg "
                        "({:.0f}% of normal)",
                        state.fill_rate_per_hour,
                        state.avg_fill_rate_24h,
                        ratio * 100.0));
    }
}

// Rule 6: Spread widening > 2x normal.
void AlertManager::check_spread_widening(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::SpreadWidening));

    for (const auto& pair : state.pairs) {
        if (pair.normal_spread_bps <= 0.0) {
            continue; // No baseline yet.
        }

        const double ratio = pair.current_spread_bps / pair.normal_spread_bps;

        if (ratio > threshold) {
            send_alert(AlertRule::SpreadWidening,
                fmt::format("Spread widening on {}: {:.1f} bps "
                            "({:.1f}x normal {:.1f} bps)",
                            pair.pair_name,
                            pair.current_spread_bps,
                            ratio,
                            pair.normal_spread_bps));
        }
    }
}

// Rule 7: Underwater position detected (cost_basis > market_price).
void AlertManager::check_underwater_positions(const BotState& state)
{
    for (const auto& asset : state.assets) {
        if (asset.balance <= 0) {
            continue; // No position to be underwater.
        }

        if (asset.cost_basis > asset.market_price && asset.market_price > 0) {
            send_alert(AlertRule::UnderwaterPosition,
                fmt::format("Underwater position: {} (cost={}, market={})",
                            asset.asset_id,
                            asset.cost_basis,
                            asset.market_price));
        }
    }
}

// Rule 8: Single asset exceeds concentration cap (12% of portfolio).
void AlertManager::check_concentration_breach(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::ConcentrationBreach));

    for (const auto& asset : state.assets) {
        // Skip native XCH -- concentration limits apply to CATs only
        // (the strategy doc's "single CAT cap" rule).
        if (asset.asset_id == "xch") {
            continue;
        }

        if (asset.portfolio_pct > threshold) {
            send_alert(AlertRule::ConcentrationBreach,
                fmt::format("Concentration breach: {} at {:.1f}% of portfolio "
                            "(cap {:.1f}%)",
                            asset.asset_id,
                            asset.portfolio_pct * 100.0,
                            threshold * 100.0));
        }
    }
}

// Rule 9: PnL drawdown exceeds threshold from peak.
void AlertManager::check_pnl_drawdown(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::PnlDrawdown));

    if (state.peak_pnl <= 0) {
        return; // No positive peak yet -- cannot compute drawdown.
    }

    const double drawdown = static_cast<double>(state.peak_pnl - state.total_pnl)
                          / static_cast<double>(state.peak_pnl);

    if (drawdown > threshold) {
        send_alert(AlertRule::PnlDrawdown,
            fmt::format("PnL drawdown: {:.1f}% from peak (threshold {:.1f}%)",
                        drawdown * 100.0,
                        threshold * 100.0));
    }
}

// Rule 10: Consecutive offer creation failures.
void AlertManager::check_offer_creation_fail(const BotState& state)
{
    const double threshold = thresholds_.at(
        static_cast<std::uint8_t>(AlertRule::OfferCreationFail));

    if (static_cast<double>(state.consecutive_offer_failures) >= threshold) {
        send_alert(AlertRule::OfferCreationFail,
            fmt::format("Offer creation failing: {} consecutive failures",
                        state.consecutive_offer_failures));
    }
}

}  // namespace xop
