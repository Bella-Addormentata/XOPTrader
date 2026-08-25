// market_analyzer.cpp -- Implementation of the MarketAnalyzer startup
//                        analysis module.
//
// All numerical routines are derived from established market-microstructure
// literature and are documented inline.  See market_analyzer.hpp for the full
// scholarly citations.
//
// ISO/IEC 27001:2022  (no secrets)
// ISO/IEC 5055        (bounds-checked, no UB, no raw owning pointers)
// ISO/IEC 25000       (documented computations with mathematical derivation)

#include <xop/data/market_analyzer.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace xop {

namespace {

// Annualisation factor: blocks per year under 52-second block times.
// Used by compute_summary to convert per-block sigma to annualised.
double annualisation_factor(double block_time_seconds) noexcept {
    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;
    if (block_time_seconds <= 0.0) return 1.0;
    return std::sqrt(kSecondsPerYear / block_time_seconds);
}

// Sample standard deviation of a sequence.
// Returns 0.0 if the sequence has fewer than 2 elements.
// Uses the sample standard deviation formula (denominator n-1) for
// unbiased estimation from a finite sample, consistent with the
// volatility calculation convention used elsewhere in this codebase.
double stddev(const std::deque<double>& v) noexcept {
    if (v.size() < 2) return 0.0;
    const double n   = static_cast<double>(v.size());
    const double sum = std::accumulate(v.begin(), v.end(), 0.0);
    const double mu  = sum / n;
    double sq_sum = 0.0;
    for (double x : v) {
        const double d = x - mu;
        sq_sum += d * d;
    }
    return std::sqrt(sq_sum / (n - 1.0));
}

// Mean of a sequence; returns 0.0 if empty.
double mean(const std::deque<double>& v) noexcept {
    if (v.empty()) return 0.0;
    return std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}

}  // anonymous namespace

// ===========================================================================
// Construction
// ===========================================================================

MarketAnalyzer::MarketAnalyzer(const MarketAnalyzerConfig& cfg,
                               const std::vector<std::string>& pair_names)
    : cfg_(cfg)
{
    if (cfg_.analysis_blocks < 3) {
        spdlog::warn("[MarketAnalyzer] analysis_blocks={} < 3; clamping to 3",
                     cfg_.analysis_blocks);
        cfg_.analysis_blocks = 3;
    }

    for (const auto& name : pair_names) {
        states_[name].pair_name = name;
    }
}

MarketAnalyzer::MarketAnalyzer()
    : MarketAnalyzer(MarketAnalyzerConfig{}, {})
{
}

// ===========================================================================
// Core interface
// ===========================================================================

void MarketAnalyzer::ingest(const std::string& pair_name,
                             double mid_price,
                             double spread_bps,
                             double volume_24h,
                             double bid_depth,
                             double ask_depth)
{
    auto it = states_.find(pair_name);
    if (it == states_.end()) {
        // Unknown pair -- add it on-the-fly.
        states_[pair_name].pair_name = pair_name;
        it = states_.find(pair_name);
    }

    PairState& ps = it->second;

    // Track total poll attempts (including invalid data) for timeout
    // and data-quality reporting.
    ++ps.total_poll_attempts;

    if (ps.complete) {
        return;  // Window already full; ignore further observations.
    }

    // Validate inputs; skip bad values.
    if (mid_price <= 0.0) {
        spdlog::debug("[MarketAnalyzer] {} skipping invalid mid_price={}",
                      pair_name, mid_price);
        return;
    }
    if (spread_bps < 0.0) spread_bps = 0.0;
    if (volume_24h < 0.0) volume_24h = 0.0;
    if (bid_depth  < 0.0) bid_depth  = 0.0;
    if (ask_depth  < 0.0) ask_depth  = 0.0;

    // Limit rolling window to analysis_blocks + vr_short_lag so that the
    // variance-ratio computation has sufficient history without unbounded growth.
    const std::size_t max_window =
        static_cast<std::size_t>(cfg_.analysis_blocks + cfg_.vr_short_lag + 1);

    auto push = [&max_window](std::deque<double>& dq, double val) {
        dq.push_back(val);
        while (dq.size() > max_window) dq.pop_front();
    };

    push(ps.prices,     mid_price);
    push(ps.spreads,    spread_bps);
    push(ps.volumes,    volume_24h);
    push(ps.bid_depths, bid_depth);
    push(ps.ask_depths, ask_depth);

    ++ps.blocks_collected;

    if (ps.blocks_collected >= cfg_.analysis_blocks) {
        ps.complete = true;
        spdlog::info("[MarketAnalyzer] {} analysis complete ({} blocks)",
                     pair_name, ps.blocks_collected);
    }
}

// ===========================================================================
// Queries
// ===========================================================================

// [S23 2026-08-24] A pair that has been polled its full share and produced
// NOTHING.  ingest() rejects any non-positive mid, and since the S20
// plausibility gate a pair whose junk mid is refused publishes no mid at
// all, so such a pair never accumulates an observation.
//
// Deliberately does NOT consider `complete`.  force_complete() sets that
// flag on every state, so folding it in here would make this predicate
// stop recognising zero-observation pairs the moment the analysis timeout
// fires -- handing them back their vote on the spread multiplier on
// exactly the path where nobody has good data.  Having no observations is
// a property of the data, not of whether someone declared the phase over.
bool MarketAnalyzer::has_no_observations(const PairState& ps) const noexcept
{
    return ps.blocks_collected == 0
        && ps.total_poll_attempts >= cfg_.analysis_blocks;
}

// The completion test adds the `complete` guard on top: force_complete()
// must satisfy is_complete() even when every pair collected nothing, so a
// state someone has explicitly completed is no longer "still waiting".
bool MarketAnalyzer::is_structurally_unpriced(const PairState& ps) const noexcept
{
    return !ps.complete && has_no_observations(ps);
}

bool MarketAnalyzer::is_complete() const noexcept {
    if (states_.empty()) return true;  // No pairs to analyse.

    // [S23 2026-08-24] A structurally unpriced pair cannot hold the phase
    // open.
    //
    // ingest() rejects any observation with a non-positive mid, and since
    // the S20 plausibility gate a pair whose junk mid is refused publishes
    // no mid at all -- so blocks_collected never leaves 0, `complete`
    // never becomes true, and this function would return false forever.
    // The startup loop polls on that answer, so one such pair costs every
    // restart the full timeout before the phase force-completes on partial
    // data.  A pair that has been polled its full share and produced
    // NOTHING has been given its chance; the healthy pairs decide.
    //
    // The distinction is between "not polled yet" (still counts, so early
    // progress is never skipped) and "polled repeatedly, produced nothing".
    // Only an INCOMPLETE state can be structurally unpriced.
    // The `!ps.complete` term inside is_structurally_unpriced() is what
    // makes force_complete() work.  That method sets `complete` without
    // touching the counters, so WITHOUT that term a zero-observation pair
    // would still match here; with every pair at zero observations --
    // exactly what the analysis timeout produces -- all of them would be
    // skipped, any_priceable would stay false, and this function would
    // return false even after a forced completion, breaking that method's
    // contract.  The guard is why that cannot happen.
    bool any_priceable = false;
    for (const auto& [name, ps] : states_) {
        if (is_structurally_unpriced(ps)) {
            continue;   // structurally unpriced
        }
        any_priceable = true;
        if (!ps.complete) return false;
    }

    // Every pair unpriced: deliberately NOT complete.  Declaring success on
    // zero observations would hand the trading loop an empty analysis and
    // hide a total feed failure; the existing timeout owns that case and
    // says so in the log.
    return any_priceable;
}

uint32_t MarketAnalyzer::blocks_collected(const std::string& pair_name) const noexcept {
    auto it = states_.find(pair_name);
    return (it != states_.end()) ? it->second.blocks_collected : 0u;
}

uint32_t MarketAnalyzer::poll_attempts(const std::string& pair_name) const noexcept {
    auto it = states_.find(pair_name);
    return (it != states_.end()) ? it->second.total_poll_attempts : 0u;
}

uint32_t MarketAnalyzer::analysis_blocks() const noexcept {
    return cfg_.analysis_blocks;
}

uint32_t MarketAnalyzer::timeout_block_multiplier() const noexcept {
    return cfg_.timeout_block_multiplier;
}

std::vector<PairAnalysisSummary> MarketAnalyzer::get_summaries() const {
    std::vector<PairAnalysisSummary> result;
    result.reserve(states_.size());
    for (const auto& [name, ps] : states_) {
        result.push_back(compute_summary(ps));
    }
    return result;
}

PairAnalysisSummary MarketAnalyzer::get_summary(const std::string& pair_name) const {
    auto it = states_.find(pair_name);
    if (it == states_.end()) {
        PairAnalysisSummary s;
        s.pair_name = pair_name;
        return s;
    }
    return compute_summary(it->second);
}

void MarketAnalyzer::reset() {
    for (auto& [name, ps] : states_) {
        ps.prices.clear();
        ps.spreads.clear();
        ps.volumes.clear();
        ps.bid_depths.clear();
        ps.ask_depths.clear();
        ps.blocks_collected     = 0;
        ps.total_poll_attempts  = 0;
        ps.complete             = false;
    }
}

void MarketAnalyzer::force_complete() {
    for (auto& [name, ps] : states_) {
        if (!ps.complete) {
            ps.complete = true;
            spdlog::warn("[MarketAnalyzer] {} force-completed after {} valid / {} total polls",
                         name, ps.blocks_collected, ps.total_poll_attempts);
        }
    }
}

AnalysisAggressiveness MarketAnalyzer::overall_recommendation() const {
    if (states_.empty()) return AnalysisAggressiveness::Normal;

    // Return the most conservative recommendation across all pairs.
    // Conservative(0) < Normal(1) < Aggressive(2) — lower is more conservative.
    //
    // [S23 2026-08-24] Structurally unpriced pairs do not vote.  Their
    // summary is computed from zero observations and defaults to Normal,
    // so leaving them in meant an unpriceable pair could veto an
    // Aggressive consensus and move the applied spread multiplier from
    // 0.8 to 1.0 -- a pair with NO data quietly setting policy for the
    // pairs that have it.
    //
    // Uses has_no_observations(), NOT the is_structurally_unpriced() that
    // completion uses.  The two DELIBERATELY differ: completion adds a
    // `complete` guard so force_complete() can end the phase, but folding
    // that guard in here would hand a zero-observation pair its vote back
    // the moment the timeout fired -- on precisely the path where nobody
    // has good data.  Having no observations is a property of the data;
    // being complete is a statement about the phase.
    auto most_conservative = AnalysisAggressiveness::Aggressive;
    bool any_priceable = false;
    for (const auto& [name, ps] : states_) {
        // has_no_observations, NOT is_structurally_unpriced: the vote must
        // stay withdrawn after force_complete() too.
        if (has_no_observations(ps)) continue;
        any_priceable = true;
        const auto summary = compute_summary(ps);
        if (static_cast<uint8_t>(summary.aggressiveness) <
            static_cast<uint8_t>(most_conservative)) {
            most_conservative = summary.aggressiveness;
        }
    }
    // Nothing priceable at all: fall back to Normal rather than inheriting
    // the Aggressive seed above.
    if (!any_priceable) return AnalysisAggressiveness::Normal;
    return most_conservative;
}

double MarketAnalyzer::recommended_spread_multiplier() const {
    switch (overall_recommendation()) {
        case AnalysisAggressiveness::Conservative: return 1.5;
        case AnalysisAggressiveness::Aggressive:   return 0.8;
        default:                                   return 1.0;
    }
}

// ===========================================================================
// Private helpers
// ===========================================================================

PairAnalysisSummary MarketAnalyzer::compute_summary(const PairState& ps) const {
    PairAnalysisSummary s;
    s.pair_name        = ps.pair_name;
    s.blocks_collected = ps.blocks_collected;
    s.complete         = ps.complete;
    s.window_filled    = ps.complete && (ps.blocks_collected >= cfg_.analysis_blocks);

    const std::size_t n = ps.prices.size();

    // -----------------------------------------------------------------------
    // 1. Volatility -- per-block log-return std-dev, then annualised.
    //
    //   sigma_block = stddev( ln(p_{t+1} / p_t) )
    //   sigma_annual = sigma_block * sqrt(blocks_per_year)
    //
    // This is equivalent to close-to-close volatility (Parkinson 1980 would
    // require OHLC candles, but we only have mid-prices here).
    // -----------------------------------------------------------------------
    if (n >= 2) {
        std::deque<double> log_returns;
        for (std::size_t i = 1; i < n; ++i) {
            const double prev = ps.prices[i - 1];
            const double curr = ps.prices[i];
            if (prev > 0.0 && curr > 0.0) {
                log_returns.push_back(std::log(curr / prev));
            }
        }
        s.volatility_per_block = stddev(log_returns);
        s.volatility_annual    =
            s.volatility_per_block * annualisation_factor(cfg_.block_time_seconds);

        // Momentum: signed cumulative log-return over the whole window.
        if (ps.prices.front() > 0.0) {
            s.momentum = std::log(ps.prices.back() / ps.prices.front());
        }
    }

    // -----------------------------------------------------------------------
    // 2. Spread statistics.
    // -----------------------------------------------------------------------
    if (!ps.spreads.empty()) {
        s.mean_spread_bps = mean(ps.spreads);
        const double sd   = stddev(ps.spreads);
        s.spread_cv       = (s.mean_spread_bps > 1e-9) ? (sd / s.mean_spread_bps) : 0.0;
    }

    // -----------------------------------------------------------------------
    // 3. Volume (simple mean).
    // -----------------------------------------------------------------------
    s.mean_volume_24h = mean(ps.volumes);

    // -----------------------------------------------------------------------
    // 4. Variance Ratio -- Lo & MacKinlay (1988).
    //
    //   VR(q) = Var(r_q) / (q * Var(r_1))
    //
    //   where r_1 = single-period log return
    //         r_q = q-period log return (sum of q consecutive r_1)
    //
    // Under a pure random walk, VR = 1.  VR < 1 implies mean reversion,
    // VR > 1 implies momentum.
    // -----------------------------------------------------------------------
    s.variance_ratio = compute_variance_ratio(ps.prices, cfg_.vr_short_lag);

    if (s.variance_ratio < cfg_.vr_lower_threshold) {
        s.regime = MarketRegime::MeanReverting;
    } else if (s.variance_ratio > cfg_.vr_upper_threshold) {
        s.regime = MarketRegime::Momentum;
    } else {
        s.regime = MarketRegime::Random;
    }

    // -----------------------------------------------------------------------
    // 5. Order-book imbalance.
    //
    //   imbalance = bid_depth / (bid_depth + ask_depth)
    //
    // 0.5 = perfectly balanced; > 0.5 = net buy interest.
    // -----------------------------------------------------------------------
    {
        const double total_bid = std::accumulate(ps.bid_depths.begin(),
                                                  ps.bid_depths.end(), 0.0);
        const double total_ask = std::accumulate(ps.ask_depths.begin(),
                                                  ps.ask_depths.end(), 0.0);
        const double total     = total_bid + total_ask;
        s.book_imbalance = (total > 1e-9) ? (total_bid / total) : 0.5;
    }

    // -----------------------------------------------------------------------
    // 6. Recommendation.
    // -----------------------------------------------------------------------
    s.aggressiveness = recommend(s.volatility_annual,
                                  s.spread_cv,
                                  s.mean_spread_bps,
                                  s.regime);

    spdlog::debug("[MarketAnalyzer] {} summary: vol_ann={:.2f}% spread={:.1f}bps "
                  "cv={:.2f} VR={:.3f} regime={} agg={}",
                  ps.pair_name,
                  s.volatility_annual * 100.0,
                  s.mean_spread_bps,
                  s.spread_cv,
                  s.variance_ratio,
                  to_string(s.regime),
                  to_string(s.aggressiveness));

    return s;
}

double MarketAnalyzer::compute_variance_ratio(const std::deque<double>& prices,
                                               uint32_t lag) const {
    // Need at least lag+1 price points to compute q-period returns.
    if (lag < 2 || prices.size() < static_cast<std::size_t>(lag + 2)) {
        return 1.0;  // Not enough data; return neutral estimate.
    }

    const std::size_t m = prices.size();

    // Compute single-period (lag-1) log-returns.
    std::vector<double> r1;
    r1.reserve(m - 1);
    for (std::size_t i = 1; i < m; ++i) {
        if (prices[i - 1] > 0.0 && prices[i] > 0.0) {
            r1.push_back(std::log(prices[i] / prices[i - 1]));
        }
    }

    if (r1.size() < static_cast<std::size_t>(lag + 1)) {
        return 1.0;
    }

    // Variance of single-period returns.
    const double var1 = [&r1]() -> double {
        const double n   = static_cast<double>(r1.size());
        const double mu  = std::accumulate(r1.begin(), r1.end(), 0.0) / n;
        double sq = 0.0;
        for (double x : r1) { const double d = x - mu; sq += d * d; }
        return (n > 1.0) ? sq / (n - 1.0) : 0.0;
    }();

    if (var1 < 1e-16) return 1.0;  // All returns are zero.

    // Compute overlapping q-period returns and their variance.
    const std::size_t q = static_cast<std::size_t>(lag);
    std::vector<double> rq;
    rq.reserve(m - q);
    for (std::size_t i = 0; i + q < m; ++i) {
        if (prices[i] > 0.0 && prices[i + q] > 0.0) {
            rq.push_back(std::log(prices[i + q] / prices[i]));
        }
    }

    if (rq.empty()) return 1.0;

    const double varq = [&rq]() -> double {
        const double n   = static_cast<double>(rq.size());
        const double mu  = std::accumulate(rq.begin(), rq.end(), 0.0) / n;
        double sq = 0.0;
        for (double x : rq) { const double d = x - mu; sq += d * d; }
        return (n > 1.0) ? sq / (n - 1.0) : 0.0;
    }();

    return varq / (static_cast<double>(q) * var1);
}

AnalysisAggressiveness MarketAnalyzer::recommend(
    double vol_annual,
    double spread_cv,
    double mean_spread_bps,
    MarketRegime regime) const
{
    // High-volatility or momentum regime: always Conservative.
    if (vol_annual >= cfg_.high_vol_threshold ||
        regime == MarketRegime::Momentum) {
        return AnalysisAggressiveness::Conservative;
    }

    // Unstable spreads: Conservative (adverse selection risk).
    if (spread_cv >= cfg_.high_spread_cv_threshold) {
        return AnalysisAggressiveness::Conservative;
    }

    // Low vol, mean-reverting, wide observed spread: opportunity to quote
    // tighter and capture more spread.
    if (vol_annual < cfg_.high_vol_threshold * 0.5 &&
        mean_spread_bps >= cfg_.wide_spread_bps_threshold &&
        regime == MarketRegime::MeanReverting) {
        return AnalysisAggressiveness::Aggressive;
    }

    return AnalysisAggressiveness::Normal;
}

}  // namespace xop
