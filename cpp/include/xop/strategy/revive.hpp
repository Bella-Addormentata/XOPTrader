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
//   * stops quoting once its own trading P&L (PairFlows) has fallen more
//     than a daily loss budget below its peak in the last 24 hours
//     (LossBudget).
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
#include <vector>

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

/// A revive pair's own trading P&L: the base and quote it has bought and
/// sold through its fills, plus the XCH fees those offers cost, valued at
/// the current mid. Starts at zero, so a pair that has not traded has lost
/// nothing; inventory held before revive, or shared with another pair, is
/// not attributed to it. Both legs count: a bid that bought base with quote
/// is worth (base x mid - quote paid) whatever asset carries the loss.
struct PairFlows {
    double base_units{0.0};    ///< net base bought (+) / sold (-)
    double quote_units{0.0};   ///< net quote received (+) / paid (-)
    double fee_xch{0.0};       ///< offer-creation fees paid, in XCH

    /// True until the first fill is booked.
    [[nodiscard]] bool is_empty() const noexcept
    {
        return base_units == 0.0 && quote_units == 0.0 && fee_xch == 0.0;
    }

    /// Book one fill. `bid` true = we bought base. `price` is quote per
    /// base. Non-finite or non-positive inputs are ignored.
    void apply_fill(bool bid, double base_units_filled, double price,
                    double fee_xch_paid) noexcept
    {
        if (!(base_units_filled > 0.0) || !std::isfinite(base_units_filled)
            || !(price > 0.0) || !std::isfinite(price)) {
            return;
        }
        const double quote = base_units_filled * price;
        base_units  += bid ? base_units_filled : -base_units_filled;
        quote_units += bid ? -quote : quote;
        if (fee_xch_paid > 0.0 && std::isfinite(fee_xch_paid)) {
            fee_xch += fee_xch_paid;
        }
    }

    /// The flows marked at `mid` (quote per base) in USD, given the quote
    /// asset's and XCH's USD prices. nullopt when any input it needs is
    /// unknown, so a missing price is never read as a loss.
    [[nodiscard]] std::optional<double> value_usd(double mid, double quote_usd,
                                                  double xch_usd) const noexcept
    {
        if (!(quote_usd > 0.0) || !std::isfinite(quote_usd)) return std::nullopt;
        if (base_units != 0.0 && (!(mid > 0.0) || !std::isfinite(mid))) {
            return std::nullopt;
        }
        if (fee_xch > 0.0 && (!(xch_usd > 0.0) || !std::isfinite(xch_usd))) {
            return std::nullopt;
        }
        const double base_value = base_units != 0.0 ? base_units * mid : 0.0;
        const double fees = fee_xch > 0.0 ? fee_xch * xch_usd : 0.0;
        return (quote_units + base_value) * quote_usd - fees;
    }

    /// [SYNTH 2026-10-22] The flows marked LEG BY LEG: base at `base_usd`,
    /// quote at `quote_usd` (USD per unit of each). A leg's mark is needed
    /// only while that leg is held, so quote-only exposure needs no base
    /// mark and base-only exposure no quote mark. nullopt when a mark the
    /// flows need is unknown, so a missing price is never read as a loss.
    [[nodiscard]] std::optional<double> value_usd_by_leg(
        double base_usd, double quote_usd, double xch_usd) const noexcept
    {
        auto ok = [](double v) { return v > 0.0 && std::isfinite(v); };
        if (base_units != 0.0 && !ok(base_usd)) return std::nullopt;
        if (quote_units != 0.0 && !ok(quote_usd)) return std::nullopt;
        if (fee_xch > 0.0 && !ok(xch_usd)) return std::nullopt;
        const double b = base_units != 0.0 ? base_units * base_usd : 0.0;
        const double q = quote_units != 0.0 ? quote_units * quote_usd : 0.0;
        const double f = fee_xch > 0.0 ? fee_xch * xch_usd : 0.0;
        return b + q - f;
    }
};

/// The quote leg's USD price for PairFlows::value_usd: XCH's price for an
/// XCH quote, else the leg's listed (feed) price, else the pair's carried
/// accounting factor; 0 when none is known. The accounting factor alone is
/// 0 for a non-par, non-XCH quote (wmilliETH.b/DBX), which would leave the
/// loss budget blind, so the listed price comes first.
[[nodiscard]] inline double quote_usd_price(bool quote_is_xch, double xch_usd,
                                            double listed_usd,
                                            double carried_factor) noexcept
{
    auto ok = [](double v) { return v > 0.0 && std::isfinite(v); };
    if (quote_is_xch) return ok(xch_usd) ? xch_usd : 0.0;
    if (ok(listed_usd)) return listed_usd;
    return ok(carried_factor) ? carried_factor : 0.0;
}

/// Per-pair daily loss budget over a rolling window of P&L samples.
///
/// The drawdown is the window's PEAK P&L minus the LATEST sample, so a gain
/// earlier in the day is not spent before the budget counts, and a loss that
/// has been earned back stops counting once it leaves the window.
///
/// Samples closer together than `min_spacing` share one bucket. A bucket
/// keeps its own highest value as well as its latest, so coalescing never
/// erases a peak: a P&L of 0 then -20 inside one bucket is a 20 drawdown.
class LossBudget {
public:
    using Clock     = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    explicit LossBudget(std::chrono::seconds window = std::chrono::hours(24),
                        std::chrono::seconds min_spacing = std::chrono::seconds(60))
        : window_(window), min_spacing_(min_spacing) {}

    /// Record `pnl_usd` for `pair` at `now`. The window holds at most about
    /// window / min_spacing buckets per pair. Non-finite values are ignored.
    void record(const std::string& pair, TimePoint now, double pnl_usd)
    {
        if (!std::isfinite(pnl_usd)) return;
        auto& d = samples_[pair];
        if (!d.empty() && now - d.back().start < min_spacing_) {
            d.back().latest = pnl_usd;
            d.back().peak   = std::max(d.back().peak, pnl_usd);
        } else {
            d.push_back(Bucket{now, pnl_usd, pnl_usd});
        }
        while (!d.empty() && now - d.front().start > window_) d.pop_front();
    }

    /// True once anything has been recorded for `pair` (and not all aged out).
    [[nodiscard]] bool has_samples(const std::string& pair) const
    {
        const auto it = samples_.find(pair);
        return it != samples_.end() && !it->second.empty();
    }

    /// Peak-to-latest drawdown in USD for `pair` (0 with no samples).
    [[nodiscard]] double drawdown_usd(const std::string& pair) const
    {
        const auto it = samples_.find(pair);
        if (it == samples_.end() || it->second.empty()) return 0.0;
        double peak = it->second.front().peak;
        for (const auto& b : it->second) peak = std::max(peak, b.peak);
        return std::max(0.0, peak - it->second.back().latest);
    }

    /// True when the drawdown exceeds `budget_usd`. A budget <= 0 (or not
    /// finite) disables the check.
    [[nodiscard]] bool exceeded(const std::string& pair, double budget_usd) const
    {
        if (!(budget_usd > 0.0) || !std::isfinite(budget_usd)) return false;
        return drawdown_usd(pair) > budget_usd;
    }

private:
    struct Bucket {
        TimePoint start;
        double    latest;
        double    peak;
    };
    std::chrono::seconds window_;
    std::chrono::seconds min_spacing_;
    std::unordered_map<std::string, std::deque<Bucket>> samples_;
};

/// The revive size cap in base mojos, or 0 (no cap) when `cap_units` is not
/// a positive finite number. Saturates instead of overflowing, and a positive
/// cap is never rounded down to 0 -- that is the "no cap" sentinel, so a tiny
/// cap would silently disable itself.
[[nodiscard]] inline std::int64_t cap_units_to_mojos(double cap_units,
                                                     double mojos_per_unit) noexcept
{
    if (!(cap_units > 0.0) || !std::isfinite(cap_units)) return 0;
    if (!(mojos_per_unit > 0.0) || !std::isfinite(mojos_per_unit)) return 0;
    const double v = cap_units * mojos_per_unit;
    constexpr double kMax = 9.0e18;   // below INT64_MAX (~9.22e18)
    if (!(v < kMax)) return static_cast<std::int64_t>(kMax);
    return std::max<std::int64_t>(1, std::llround(v));
}

/// [REVIVE2 2026-10-22] What revive maintenance may cancel on one pair:
/// everything when it is over its loss budget, else offers above
/// `size_limit` (base mojos; 0 = no limit) while its book is thin.
struct RestingRule {
    bool         over_budget{false};
    std::int64_t size_limit{0};
};

/// A resting offer, as revive maintenance sees it.
struct RestingOffer {
    std::string  offer_id;
    std::string  pair_name;
    std::int64_t size{0};
    bool         cancel_pending{false};
};

/// One offer to cancel, and why.
struct RestingCancel {
    std::string offer_id;
    bool        over_budget{false};   ///< false = above the size limit
};

/// The resting offers revive maintenance cancels: on a pair with a rule,
/// every offer not already cancelling when the pair is over budget, else
/// those above its size limit. Pairs without a rule are never touched.
[[nodiscard]] inline std::vector<RestingCancel> select_resting_cancels(
    const std::vector<RestingOffer>& offers,
    const std::unordered_map<std::string, RestingRule>& rules)
{
    std::vector<RestingCancel> out;
    for (const auto& o : offers) {
        if (o.cancel_pending) continue;
        const auto r = rules.find(o.pair_name);
        if (r == rules.end()) continue;
        if (r->second.over_budget) {
            out.push_back(RestingCancel{o.offer_id, true});
        } else if (r->second.size_limit > 0 && o.size > r->second.size_limit) {
            out.push_back(RestingCancel{o.offer_id, false});
        }
    }
    return out;
}

}  // namespace xop::revive

#endif  // XOP_STRATEGY_REVIVE_HPP
