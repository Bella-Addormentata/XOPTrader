#ifndef XOP_STRATEGY_REVIVE_HPP
#define XOP_STRATEGY_REVIVE_HPP
// ---------------------------------------------------------------------------
// revive.hpp -- [REVIVE2 2026-10-22] decisions for revive_market pairs in
// thin markets.
//
// revive_market (PairConfig) used to do one thing: keep a ladder alive when
// the filtered book was EMPTY and a fresh external estimate anchored the
// centre. On the live thin pairs that almost never applied: a one-sided book
// or a pair with no CEX leg publishes no mid, Step 4 drops the quote before
// Step 7's revive branch is reached, and any OTHER pair's flash crash halts
// every pair's posting. So a revive pair now:
//
//   * gets a CEX reference leg from its feed listings in Step 1
//     (listed_cross_price in xop/feed_listings.hpp), so its published mid
//     falls back to the external price on an empty or one-sided book;
//   * is judged by its OWN flash-crash state machine, and neither trips nor
//     holds the global one (next_crash_state; Engine wiring);
//
// and, to bound the extra risk, while its third-party book is thin
// (book_is_thin):
//
//   * quotes no tighter than a revive half-spread floor (half_spread_floor_bps);
//   * posts no tier larger than a revive size cap (capped_tier_size);
//
// and at all times:
//
//   * stops quoting once its P&L has fallen more than a daily loss budget
//     below its peak in the last 24 hours (LossBudget).
//
// Everything that protects against REAL loss still applies unchanged: the
// external estimate must come from a fresh feed (load_config already
// requires both legs to be CoinGecko-listed for revive_market), peg
// suspension and the depeg bail-out, the rolling-window and drawdown
// breakers, the wallet-sync and balance gates.
//
// Pure header: no engine or feed types, so tests drive every branch directly.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace xop::revive {

/// True when the third-party book cannot price the pair by itself: either
/// side missing, crossed or locked, or the touches may include our own
/// offers (`third_party` false -- MarketSnapshot::touches_third_party).
[[nodiscard]] inline bool book_is_thin(double best_bid, double best_ask,
                                       bool third_party) noexcept
{
    if (!third_party) return true;
    if (!(best_bid > 0.0) || !std::isfinite(best_bid)) return true;
    if (!(best_ask > best_bid) || !std::isfinite(best_ask)) return true;
    return false;
}

/// The ladder's minimum half-spread for a revive pair. `thin` raises it to
/// at least `revive_floor_bps`; otherwise, or with the floor at 0, the
/// normal floor is returned unchanged.
[[nodiscard]] inline double half_spread_floor_bps(double floor_bps, bool thin,
                                                  double revive_floor_bps) noexcept
{
    if (!thin || !(revive_floor_bps > 0.0) || !std::isfinite(revive_floor_bps)) {
        return floor_bps;
    }
    return std::max(floor_bps, revive_floor_bps);
}

/// A tier's size under the revive cap. The cap never goes below the pair's
/// minimum offer size (which would turn every tier into dust the up-scale
/// just removed); `cap` <= 0 means no cap.
[[nodiscard]] inline std::int64_t capped_tier_size(std::int64_t size,
                                                   std::int64_t cap,
                                                   std::int64_t min_size) noexcept
{
    if (cap <= 0) return size;
    return std::min(size, std::max(cap, min_size));
}

/// One step of a flash-crash state machine, the same transitions the global
/// one makes (Engine::on_new_block_coro), applied to ONE pair's signals.
/// `State` needs Normal, Crash and Recovery enumerators.
template <typename State>
[[nodiscard]] constexpr State next_crash_state(State s, bool crashing,
                                               bool stable_phase1,
                                               bool stable_phase2) noexcept
{
    switch (s) {
        case State::Normal:
            return crashing ? State::Crash : State::Normal;
        case State::Crash:
            if (crashing) return State::Crash;
            return stable_phase1 ? State::Recovery : State::Crash;
        case State::Recovery:
            if (crashing) return State::Crash;
            return stable_phase2 ? State::Normal : State::Recovery;
    }
    return s;
}

/// Per-pair daily loss budget over a rolling window of P&L samples.
///
/// The drawdown is the window's PEAK total P&L minus the LATEST sample, so a
/// gain earlier in the day is not spent before the budget counts, and a
/// loss that has been earned back stops counting once it leaves the window.
/// Total P&L includes the mark-to-market of inventory: a revive pair holding
/// an asset whose price falls is losing money whether or not it traded.
class LossBudget {
public:
    using Clock     = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    explicit LossBudget(std::chrono::seconds window = std::chrono::hours(24),
                        std::chrono::seconds min_spacing = std::chrono::seconds(60))
        : window_(window), min_spacing_(min_spacing) {}

    /// Record `total_pnl_usd` for `pair` at `now`. Samples closer together
    /// than `min_spacing` replace the latest one rather than adding a new
    /// entry, so the window holds at most ~1,440 samples per pair. Non-finite
    /// values are ignored.
    void record(const std::string& pair, TimePoint now, double total_pnl_usd)
    {
        if (!std::isfinite(total_pnl_usd)) return;
        auto& d = samples_[pair];
        if (!d.empty() && now - d.back().first < min_spacing_) {
            d.back().second = total_pnl_usd;
        } else {
            d.emplace_back(now, total_pnl_usd);
        }
        while (!d.empty() && now - d.front().first > window_) d.pop_front();
    }

    /// Peak-to-latest drawdown in USD for `pair` (0 with no samples).
    [[nodiscard]] double drawdown_usd(const std::string& pair) const
    {
        const auto it = samples_.find(pair);
        if (it == samples_.end() || it->second.empty()) return 0.0;
        double peak = it->second.front().second;
        for (const auto& s : it->second) peak = std::max(peak, s.second);
        return std::max(0.0, peak - it->second.back().second);
    }

    /// True when the drawdown exceeds `budget_usd`. A budget <= 0 (or not
    /// finite) disables the check.
    [[nodiscard]] bool exceeded(const std::string& pair, double budget_usd) const
    {
        if (!(budget_usd > 0.0) || !std::isfinite(budget_usd)) return false;
        return drawdown_usd(pair) > budget_usd;
    }

private:
    std::chrono::seconds window_;
    std::chrono::seconds min_spacing_;
    std::unordered_map<std::string,
                       std::deque<std::pair<TimePoint, double>>> samples_;
};

}  // namespace xop::revive

#endif  // XOP_STRATEGY_REVIVE_HPP
