// depeg_detector.hpp -- Stablecoin peg-failure detection for XOPTrader.
//
// Monitors stablecoin pairs for sustained price deviations from their
// expected peg target.  Two severity levels:
//   WARN  -- deviation exceeds warn threshold; log + optional Telegram alert.
//   BAIL  -- deviation exceeds bail threshold for N consecutive blocks;
//            flag pair as suspected-failed, pull all quotes, optionally
//            auto-disable the pair in config.
//
// Inspired by the Stably USD (USDST) depeg event on Chia: the token lost
// its peg and never recovered.  This detector prevents holding a failing
// stablecoin while it spirals to zero.
//
// ISO/IEC 5055 -- bounded containers, no raw pointers, deterministic state.

#ifndef XOP_STRATEGY_DEPEG_DETECTOR_HPP
#define XOP_STRATEGY_DEPEG_DETECTOR_HPP

#include "xop/config.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace xop {

// ---------------------------------------------------------------------------
// Depeg severity levels.
// ---------------------------------------------------------------------------
enum class DepegStatus {
    Normal,          // Price within tolerance of peg target.
    Warning,         // Deviation > warn_pct but < bail_pct.
    Bailed,          // Deviation > bail_pct for sustained_blocks; quotes pulled.
    SuspectedFailure // Manually or automatically flagged as failed.
};

inline const char* to_string(DepegStatus s)
{
    switch (s) {
        case DepegStatus::Normal:           return "Normal";
        case DepegStatus::Warning:          return "Warning";
        case DepegStatus::Bailed:           return "Bailed";
        case DepegStatus::SuspectedFailure: return "SuspectedFailure";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Per-pair depeg tracking state.
// ---------------------------------------------------------------------------
struct DepegPairState {
    DepegStatus status{DepegStatus::Normal};
    double      last_price{0.0};
    double      last_deviation_pct{0.0};
    uint32_t    blocks_above_bail{0};     // Consecutive blocks above bail threshold.
    uint32_t    block_entered_warn{0};    // Block when warning was first triggered.
    bool        manually_flagged{false};  // Operator flagged as suspected failure.
};

// ---------------------------------------------------------------------------
// DepegDetector -- monitors stablecoin pairs for peg failures.
//
// Usage:
//   1. Construct with global DepegConfig.
//   2. Call register_pair() for each stablecoin pair (from PairConfig).
//   3. Call update() every block with the current mid-price.
//   4. Check should_bail() before quoting; skip the pair if true.
//   5. Call flag_suspected_failure() to manually mark a coin as dead.
// ---------------------------------------------------------------------------
class DepegDetector {
public:
    explicit DepegDetector(const DepegConfig& global_cfg)
        : global_cfg_(global_cfg) {}

    // Register a stablecoin pair for monitoring.  Uses per-pair thresholds
    // from PairConfig, falling back to global defaults.
    void register_pair(const PairConfig& pair)
    {
        if (!pair.is_stablecoin) return;

        PairThresholds t;
        t.peg_target         = pair.peg_target;
        t.warn_pct           = pair.depeg_warn_pct;
        t.bail_pct           = pair.depeg_bail_pct;
        t.sustained_blocks   = pair.depeg_sustained_blocks;

        thresholds_[pair.name] = t;
        state_[pair.name] = DepegPairState{};
    }

    // Update with the latest mid-price for a pair.  Call once per block.
    // Returns the new DepegStatus for the pair.
    DepegStatus update(const std::string& pair_name,
                       double mid_price,
                       uint32_t block_height)
    {
        if (!global_cfg_.enabled) return DepegStatus::Normal;

        auto t_it = thresholds_.find(pair_name);
        if (t_it == thresholds_.end()) return DepegStatus::Normal;

        auto& t = t_it->second;
        auto& s = state_[pair_name];

        // Already manually flagged — stay in SuspectedFailure.
        if (s.manually_flagged) {
            s.status = DepegStatus::SuspectedFailure;
            return s.status;
        }

        s.last_price = mid_price;

        // Compute % deviation from peg target.
        if (t.peg_target <= 0.0) {
            return DepegStatus::Normal;
        }
        double deviation_pct = std::abs(mid_price - t.peg_target)
                               / t.peg_target * 100.0;
        s.last_deviation_pct = deviation_pct;

        if (deviation_pct >= t.bail_pct) {
            ++s.blocks_above_bail;
            if (s.blocks_above_bail >= t.sustained_blocks) {
                s.status = DepegStatus::Bailed;
            } else {
                s.status = DepegStatus::Warning;
                if (s.block_entered_warn == 0) {
                    s.block_entered_warn = block_height;
                }
            }
        } else if (deviation_pct >= t.warn_pct) {
            s.blocks_above_bail = 0;  // Reset bail counter.
            s.status = DepegStatus::Warning;
            if (s.block_entered_warn == 0) {
                s.block_entered_warn = block_height;
            }
        } else {
            // Back within tolerance — reset all counters.
            s.blocks_above_bail  = 0;
            s.block_entered_warn = 0;
            s.status = DepegStatus::Normal;
        }

        return s.status;
    }

    // Should the engine skip quoting for this pair?
    [[nodiscard]] bool should_bail(const std::string& pair_name) const
    {
        auto it = state_.find(pair_name);
        if (it == state_.end()) return false;
        return it->second.status == DepegStatus::Bailed
            || it->second.status == DepegStatus::SuspectedFailure;
    }

    // Is the pair in a warning state (still quoting, but alert-worthy)?
    [[nodiscard]] bool is_warning(const std::string& pair_name) const
    {
        auto it = state_.find(pair_name);
        if (it == state_.end()) return false;
        return it->second.status == DepegStatus::Warning;
    }

    // Manually flag a coin as suspected failure (e.g., operator observes
    // the project has collapsed, like Stably).  Immediately bails out.
    void flag_suspected_failure(const std::string& pair_name)
    {
        auto it = state_.find(pair_name);
        if (it == state_.end()) return;
        it->second.manually_flagged = true;
        it->second.status = DepegStatus::SuspectedFailure;
    }

    // Clear a suspected failure flag (e.g., peg restored, false alarm).
    void clear_suspected_failure(const std::string& pair_name)
    {
        auto it = state_.find(pair_name);
        if (it == state_.end()) return;
        it->second.manually_flagged = false;
        it->second.blocks_above_bail = 0;
        it->second.block_entered_warn = 0;
        it->second.status = DepegStatus::Normal;
    }

    // Get the current state for a pair (for logging / monitoring).
    [[nodiscard]] const DepegPairState* get_state(
        const std::string& pair_name) const
    {
        auto it = state_.find(pair_name);
        return (it != state_.end()) ? &it->second : nullptr;
    }

    // Get the global config.
    [[nodiscard]] const DepegConfig& config() const { return global_cfg_; }

private:
    struct PairThresholds {
        double   peg_target{1.0};
        double   warn_pct{2.0};
        double   bail_pct{10.0};
        uint32_t sustained_blocks{30};
    };

    DepegConfig global_cfg_;
    std::unordered_map<std::string, PairThresholds> thresholds_;
    std::unordered_map<std::string, DepegPairState> state_;
};

} // namespace xop

#endif // XOP_STRATEGY_DEPEG_DETECTOR_HPP
