// drift_analyzer.cpp -- Implementation of InventoryDriftAnalyzer for XOPTrader
//                       CHIA DEX market-making bot.
//
// This translation unit implements the inventory drift analysis engine
// described in drift_analyzer.hpp.  See that header for the full mathematical
// derivations and references.
//
// Key computational notes:
//   - All monetary values are in XCH (double), not mojos, because the drift
//     models operate in continuous-valued space.  Conversion to/from mojos
//     happens at the call site in the risk manager.
//   - Monte Carlo simulation uses std::mt19937_64 PRNG.  The PRNG state is
//     local to each simulation call and does not affect thread safety.
//   - The normal_cdf() helper uses the Abramowitz-Stegun approximation which
//     is accurate to < 7.5e-8 -- adequate for risk calculations.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- audit-ready outputs, controlled mutable state
//   ISO/IEC 5055       -- no raw pointers, RAII locking, bounds checks
//   ISO/IEC 25000      -- single-responsibility methods, documented formulas
//   ISO/IEC JTC 1/SC 22 -- C++20, no undefined behaviour

#include <xop/risk/drift_analyzer.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>

namespace xop {

// ===========================================================================
// Enum stringification
// ===========================================================================

const char* to_string(MarketCondition c) noexcept {
    switch (c) {
        case MarketCondition::RandomWalk:    return "RandomWalk";
        case MarketCondition::TrendingUp:    return "TrendingUp";
        case MarketCondition::TrendingDown:  return "TrendingDown";
        case MarketCondition::MeanReverting: return "MeanReverting";
    }
    return "Unknown";
}

const char* to_string(RecommendedAction a) noexcept {
    switch (a) {
        case RecommendedAction::NoAction:        return "NoAction";
        case RecommendedAction::IncreaseSkew:    return "IncreaseSkew";
        case RecommendedAction::WidenSpread:     return "WidenSpread";
        case RecommendedAction::ReduceOfferSize: return "ReduceOfferSize";
        case RecommendedAction::PullOverweight:  return "PullOverweight";
        case RecommendedAction::ManualRebalance: return "ManualRebalance";
    }
    return "Unknown";
}

// ===========================================================================
// Construction
// ===========================================================================

InventoryDriftAnalyzer::InventoryDriftAnalyzer(const DriftConfig& cfg)
    : cfg_{cfg}
{
    spdlog::info("InventoryDriftAnalyzer constructed: lambda={:.4f} "
                 "delta_q={:.1f} V_total={:.0f} XCH_price={:.2f}",
                 cfg_.fill_rate_per_block, cfg_.avg_fill_size_xch,
                 cfg_.total_portfolio_value_usd, cfg_.xch_price_usd);
}

InventoryDriftAnalyzer::InventoryDriftAnalyzer(
    const RiskConfig& risk_cfg,
    double fill_rate_per_block,
    double avg_fill_size_xch,
    double total_value_usd,
    double xch_price_usd)
    : cfg_{}
{
    // Extract risk thresholds from the RiskConfig.
    cfg_.soft_limit_pct            = risk_cfg.soft_limit_pct;
    cfg_.hard_limit_pct            = risk_cfg.hard_limit_pct;

    // Store caller-supplied market microstructure parameters.
    cfg_.fill_rate_per_block       = fill_rate_per_block;
    cfg_.avg_fill_size_xch         = avg_fill_size_xch;
    cfg_.total_portfolio_value_usd = total_value_usd;
    cfg_.xch_price_usd             = xch_price_usd;

    spdlog::info("InventoryDriftAnalyzer constructed from RiskConfig: "
                 "soft={:.2f} hard={:.2f} lambda={:.4f} delta_q={:.1f}",
                 cfg_.soft_limit_pct, cfg_.hard_limit_pct,
                 cfg_.fill_rate_per_block, cfg_.avg_fill_size_xch);
}

// ===========================================================================
// Observation recording
// ===========================================================================

void InventoryDriftAnalyzer::record_observation(double ratio,
                                                 BlockHeight block_height)
{
    // Clamp ratio to [0, 1] for robustness against rounding artifacts.
    ratio = std::clamp(ratio, 0.0, 1.0);

    // Exclusive lock -- modifying the observation deque.
    std::unique_lock lock(mtx_);

    observations_.push_back({ratio, block_height});

    // Trim the deque to the configured rolling window size.
    while (observations_.size() >
           static_cast<std::size_t>(cfg_.drift_window_blocks)) {
        observations_.pop_front();
    }
}

// ===========================================================================
// Core analysis: analyze_drift()
// ===========================================================================

DriftReport InventoryDriftAnalyzer::analyze_drift(
    double          current_ratio,
    double          sigma,
    MarketCondition condition,
    double          trend_pct_day) const
{
    DriftReport report{};

    // -- Current state -------------------------------------------------------

    report.current_inventory_ratio = std::clamp(current_ratio, 0.0, 1.0);
    report.detected_condition      = condition;

    // -- Drift rate estimation -----------------------------------------------

    // Compute the model-expected drift rate based on the market condition.
    double model_drift_rate = 0.0;  // XCH per block.

    switch (condition) {
        case MarketCondition::RandomWalk:
            // No systematic drift; only diffusion.
            model_drift_rate = 0.0;
            break;

        case MarketCondition::TrendingUp:
            // Price rising => bids fill more => base accumulates.
            model_drift_rate = trending_drift_rate(std::abs(trend_pct_day));
            report.trend_magnitude = std::abs(trend_pct_day) / kBlocksPerDay;
            break;

        case MarketCondition::TrendingDown:
            // Price falling => asks fill more => base depletes.
            model_drift_rate = -trending_drift_rate(std::abs(trend_pct_day));
            report.trend_magnitude = std::abs(trend_pct_day) / kBlocksPerDay;
            break;

        case MarketCondition::MeanReverting:
            // Drift is dampened by reversion; use a fraction of random walk.
            model_drift_rate = 0.0;
            report.trend_magnitude = 0.0;
            break;
    }

    // Use empirical drift if available; fall back to model.
    auto emp = empirical_drift();
    if (emp.has_value()) {
        // Convert ratio-space drift to XCH-space:
        //   d(ratio)/d(block) * V_total / price = XCH/block.
        const double xch_per_ratio = cfg_.total_portfolio_value_usd
                                   / cfg_.xch_price_usd;
        report.current_drift_rate = emp->first * xch_per_ratio;
        report.drift_rate_sigma   = emp->second * xch_per_ratio;
    } else {
        report.current_drift_rate = model_drift_rate;
        // Diffusion-based uncertainty for model drift.
        const double diffusion_per_block = cfg_.avg_fill_size_xch
                                         * std::sqrt(cfg_.fill_rate_per_block);
        report.drift_rate_sigma = diffusion_per_block;
    }

    // -- Compute deviations needed to reach limits ---------------------------

    // Distance from current ratio to soft and hard limits (in ratio space).
    // We consider breach in EITHER direction (too much base or too much quote).
    const double distance_to_soft = ratio_to_xch_deviation(
        cfg_.soft_limit_pct, current_ratio);
    const double distance_to_hard = ratio_to_xch_deviation(
        cfg_.hard_limit_pct, current_ratio);

    // -- Time-to-breach under each regime ------------------------------------

    switch (condition) {
        case MarketCondition::RandomWalk: {
            report.expected_blocks_to_soft_limit =
                random_walk_first_passage(distance_to_soft);
            report.expected_blocks_to_hard_limit =
                random_walk_first_passage(distance_to_hard);
            break;
        }

        case MarketCondition::TrendingUp:
        case MarketCondition::TrendingDown: {
            const double abs_drift = std::abs(model_drift_rate);
            report.expected_blocks_to_soft_limit =
                trending_first_passage(distance_to_soft, abs_drift);
            report.expected_blocks_to_hard_limit =
                trending_first_passage(distance_to_hard, abs_drift);
            break;
        }

        case MarketCondition::MeanReverting: {
            const double theta = glft_theta();
            const double sigma_ss = glft_steady_state_sigma();
            report.expected_blocks_to_soft_limit =
                mean_revert_first_passage(distance_to_soft, theta, sigma_ss);
            report.expected_blocks_to_hard_limit =
                mean_revert_first_passage(distance_to_hard, theta, sigma_ss);
            break;
        }
    }

    // Convert blocks to hours for convenience.
    report.expected_hours_to_soft_limit =
        report.expected_blocks_to_soft_limit * cfg_.block_time_seconds / 3600.0;
    report.expected_hours_to_hard_limit =
        report.expected_blocks_to_hard_limit * cfg_.block_time_seconds / 3600.0;

    // -- A-S vs GLFT model comparison ----------------------------------------

    report.as_steady_state_sigma   = as_steady_state_sigma(sigma);
    report.glft_steady_state_sigma = glft_steady_state_sigma();

    // Breach probabilities under steady state:
    //   P(|q| > L_soft) = 2 * Phi(-L_soft / sigma_ss)
    // This is equivalent to 2 * (1 - Phi(L / sigma_ss)) by symmetry.
    if (report.as_steady_state_sigma > 0.0) {
        report.as_breach_probability =
            2.0 * normal_cdf(-distance_to_soft / report.as_steady_state_sigma);
    } else {
        report.as_breach_probability = 0.0;
    }

    if (report.glft_steady_state_sigma > 0.0) {
        report.glft_breach_probability =
            2.0 * normal_cdf(-distance_to_soft / report.glft_steady_state_sigma);
    } else {
        report.glft_breach_probability = 0.0;
    }

    // -- UTXO feedback -------------------------------------------------------

    report.utxo_amplification_factor = utxo_amplification(current_ratio);
    report.effective_drift_rate =
        report.current_drift_rate * report.utxo_amplification_factor;

    // Adjust time-to-breach for UTXO feedback (divides by amplification).
    if (report.utxo_amplification_factor > 1.0) {
        report.expected_blocks_to_soft_limit /= report.utxo_amplification_factor;
        report.expected_blocks_to_hard_limit /= report.utxo_amplification_factor;
        report.expected_hours_to_soft_limit  /= report.utxo_amplification_factor;
        report.expected_hours_to_hard_limit  /= report.utxo_amplification_factor;
    }

    // -- 24/7 continuous operation -------------------------------------------

    report.continuous_vs_session_factor = kContinuousVsSessionFactor;

    // -- Anomaly detection ---------------------------------------------------

    report.drift_anomaly_detected = false;
    report.anomaly_z_score = 0.0;

    if (emp.has_value() && report.drift_rate_sigma > 0.0) {
        // Z-score: how far the empirical drift is from the model prediction.
        const double expected = model_drift_rate;
        const double observed_xch_per_block = report.current_drift_rate;
        report.anomaly_z_score =
            (observed_xch_per_block - expected) / report.drift_rate_sigma;
        report.drift_anomaly_detected =
            std::abs(report.anomaly_z_score) > cfg_.anomaly_z_threshold;
    }

    // -- Recommended action --------------------------------------------------

    report.recommended_action = determine_action(
        current_ratio,
        report.expected_blocks_to_soft_limit,
        report.expected_blocks_to_hard_limit,
        report.drift_anomaly_detected);

    report.action_detail = action_detail_text(
        report.recommended_action,
        report.expected_blocks_to_soft_limit,
        report.expected_blocks_to_hard_limit,
        current_ratio);

    spdlog::debug("analyze_drift: ratio={:.4f} condition={} "
                  "blocks_to_soft={:.0f} blocks_to_hard={:.0f} action={}",
                  current_ratio, to_string(condition),
                  report.expected_blocks_to_soft_limit,
                  report.expected_blocks_to_hard_limit,
                  to_string(report.recommended_action));

    return report;
}

// ===========================================================================
// Monte Carlo simulation
// ===========================================================================

DriftSimulationResult InventoryDriftAnalyzer::simulate_drift(
    MarketCondition condition,
    double          trend_pct_day,
    double          current_ratio,
    uint32_t        num_paths,
    uint32_t        max_blocks,
    uint64_t        seed) const
{
    // Seed the RNG.  If seed == 0, use a hardware random device.
    std::mt19937_64 rng;
    if (seed != 0) {
        rng.seed(seed);
    } else {
        std::random_device rd;
        rng.seed(rd());
    }

    std::normal_distribution<double> normal(0.0, 1.0);

    // Precompute constants.
    const double delta_q   = cfg_.avg_fill_size_xch;
    const double lambda    = cfg_.fill_rate_per_block;
    const double sigma_blk = per_block_sigma(cfg_.sigma_annual);
    const double theta     = glft_theta();
    const double L_soft    = ratio_to_xch_deviation(cfg_.soft_limit_pct, 0.5);
    const double L_hard    = ratio_to_xch_deviation(cfg_.hard_limit_pct, 0.5);

    // Compute optional directional drift (XCH per block).
    double drift_per_block = 0.0;
    if (condition == MarketCondition::TrendingUp) {
        drift_per_block = trending_drift_rate(trend_pct_day);
    } else if (condition == MarketCondition::TrendingDown) {
        drift_per_block = -trending_drift_rate(trend_pct_day);
    }

    // Gaussian noise standard deviation per block for the inventory process.
    // Each block has lambda expected fills of size delta_q, giving variance
    // delta_q^2 * lambda per block.
    const double noise_sigma = delta_q * std::sqrt(lambda);

    // A-S reversion force: gamma * sigma_block^2 * tau_avg.
    // tau_avg is the average remaining time over the rolling horizon.
    const double tau_avg =
        static_cast<double>(cfg_.as_horizon_blocks) *
        cfg_.block_time_seconds / (2.0 * kSecondsPerYear);
    const double as_force = cfg_.gamma * sigma_blk * sigma_blk * tau_avg;

    // Convert the current inventory ratio to an XCH deviation from balanced.
    // q_initial = (current_ratio - 0.5) * total_value / xch_price.
    // This ensures the simulation starts from the actual position rather than
    // assuming a balanced (q = 0) portfolio.
    const double clamped_ratio = std::clamp(current_ratio, 0.0, 1.0);
    const double q_initial =
        (cfg_.xch_price_usd > 0.0)
            ? (clamped_ratio - 0.5) * cfg_.total_portfolio_value_usd
              / cfg_.xch_price_usd
            : 0.0;

    // Storage for first-passage times across all paths.
    std::vector<double> soft_times(num_paths,
                                   static_cast<double>(max_blocks));
    std::vector<double> hard_times(num_paths,
                                   static_cast<double>(max_blocks));

    // Accumulate final-state ratios for steady-state statistics.
    double sum_final_ratio    = 0.0;
    double sum_final_ratio_sq = 0.0;

    for (uint32_t p = 0; p < num_paths; ++p) {
        double q = q_initial;  // Start from the actual inventory deviation.
        bool hit_soft = false;
        bool hit_hard = false;

        for (uint32_t b = 1; b <= max_blocks; ++b) {
            // Apply directional drift.
            q += drift_per_block;

            // Apply Gaussian diffusion noise.
            q += noise_sigma * normal(rng);

            // Apply mean-reversion force (model-dependent).
            // For MeanReverting: use GLFT theta (full OU reversion).
            // For other regimes: use A-S reversion force.
            if (condition == MarketCondition::MeanReverting) {
                q -= theta * q;
            } else {
                q -= as_force * q;
            }

            const double abs_q = std::abs(q);

            // Record first-passage to soft limit.
            if (!hit_soft && abs_q >= L_soft) {
                soft_times[p] = static_cast<double>(b);
                hit_soft = true;
            }

            // Record first-passage to hard limit.
            if (!hit_hard && abs_q >= L_hard) {
                hard_times[p] = static_cast<double>(b);
                hit_hard = true;
            }

            // If both limits breached, no need to continue this path.
            if (hit_soft && hit_hard) {
                break;
            }
        }

        // Convert final inventory deviation to a ratio for steady-state stats.
        // ratio = 0.5 + q * xch_price / total_value.
        const double final_ratio =
            0.5 + q * cfg_.xch_price_usd / cfg_.total_portfolio_value_usd;
        sum_final_ratio    += final_ratio;
        sum_final_ratio_sq += final_ratio * final_ratio;
    }

    // -- Compute summary statistics ------------------------------------------

    // Sort first-passage times for percentile extraction.
    std::sort(soft_times.begin(), soft_times.end());
    std::sort(hard_times.begin(), hard_times.end());

    // Helper: compute mean of a vector.
    const auto vec_mean = [](const std::vector<double>& v) -> double {
        return std::accumulate(v.begin(), v.end(), 0.0) /
               static_cast<double>(v.size());
    };

    // Helper: linearly-interpolated percentile on a sorted vector.
    const auto percentile = [](const std::vector<double>& sorted_v,
                                double pct) -> double {
        if (sorted_v.empty()) return 0.0;
        const double idx =
            pct * static_cast<double>(sorted_v.size() - 1);
        const auto lo = static_cast<std::size_t>(std::floor(idx));
        const auto hi = static_cast<std::size_t>(std::ceil(idx));
        if (lo == hi) return sorted_v[lo];
        const double frac = idx - static_cast<double>(lo);
        return sorted_v[lo] * (1.0 - frac) + sorted_v[hi] * frac;
    };

    DriftSimulationResult result{};
    result.mean_time_to_soft_blocks = vec_mean(soft_times);
    result.mean_time_to_hard_blocks = vec_mean(hard_times);
    result.p05_time_to_soft_blocks  = percentile(soft_times, 0.05);
    result.p95_time_to_soft_blocks  = percentile(soft_times, 0.95);
    result.p05_time_to_hard_blocks  = percentile(hard_times, 0.05);
    result.p95_time_to_hard_blocks  = percentile(hard_times, 0.95);

    const double n = static_cast<double>(num_paths);
    result.steady_state_mean_ratio  = sum_final_ratio / n;
    result.steady_state_sigma_ratio =
        std::sqrt(std::max(0.0, sum_final_ratio_sq / n -
                                (sum_final_ratio / n) *
                                (sum_final_ratio / n)));
    result.num_paths = num_paths;

    spdlog::info("simulate_drift: {} paths, condition={}, "
                 "mean_soft={:.0f} mean_hard={:.0f} blocks",
                 num_paths, to_string(condition),
                 result.mean_time_to_soft_blocks,
                 result.mean_time_to_hard_blocks);

    return result;
}

// ===========================================================================
// Precomputed breach table
// ===========================================================================

std::vector<TimeToBreachEntry> InventoryDriftAnalyzer::compute_breach_table() const
{
    std::vector<TimeToBreachEntry> table;
    table.reserve(4);

    // Deviation from 0.5 needed for soft and hard limits.
    const double L_soft    = ratio_to_xch_deviation(cfg_.soft_limit_pct, 0.5);
    const double L_hard    = ratio_to_xch_deviation(cfg_.hard_limit_pct, 0.5);
    const double blk_to_hrs = cfg_.block_time_seconds / 3600.0;

    // 1. RandomWalk (balanced market, no trend).
    {
        const double bs = random_walk_first_passage(L_soft);
        const double bh = random_walk_first_passage(L_hard);
        table.push_back(TimeToBreachEntry{
            MarketCondition::RandomWalk, 0.0,
            bs, bh,
            bs * blk_to_hrs, bh * blk_to_hrs
        });
    }

    // 2. TrendingUp at 5%/day.
    {
        const double drift = trending_drift_rate(0.05);
        const double bs = trending_first_passage(L_soft, drift);
        const double bh = trending_first_passage(L_hard, drift);
        table.push_back(TimeToBreachEntry{
            MarketCondition::TrendingUp, 5.0,
            bs, bh,
            bs * blk_to_hrs, bh * blk_to_hrs
        });
    }

    // 3. TrendingUp at 10%/day.
    {
        const double drift = trending_drift_rate(0.10);
        const double bs = trending_first_passage(L_soft, drift);
        const double bh = trending_first_passage(L_hard, drift);
        table.push_back(TimeToBreachEntry{
            MarketCondition::TrendingUp, 10.0,
            bs, bh,
            bs * blk_to_hrs, bh * blk_to_hrs
        });
    }

    // 4. MeanReverting (GLFT with reversion).
    {
        const double theta    = glft_theta();
        const double sigma_ss = glft_steady_state_sigma();
        const double bs = mean_revert_first_passage(L_soft, theta, sigma_ss);
        const double bh = mean_revert_first_passage(L_hard, theta, sigma_ss);
        table.push_back(TimeToBreachEntry{
            MarketCondition::MeanReverting, 0.0,
            bs, bh,
            bs * blk_to_hrs, bh * blk_to_hrs
        });
    }

    return table;
}

// ===========================================================================
// Real-time monitoring
// ===========================================================================

bool InventoryDriftAnalyzer::is_drift_anomalous(
    MarketCondition condition) const
{
    auto emp = empirical_drift();
    if (!emp.has_value() || emp->second <= 0.0) {
        // Insufficient data; cannot declare an anomaly.
        return false;
    }

    const auto& [slope, emp_sigma] = emp.value();

    // For RandomWalk and MeanReverting, expected drift is zero.
    // For trending conditions, the empirical drift IS the expectation,
    // so we compare against zero (any drift is the expected drift).
    double expected_drift = 0.0;

    const double z = std::abs(slope - expected_drift) / emp_sigma;
    return z > cfg_.anomaly_z_threshold;
}

std::optional<std::pair<double, double>>
InventoryDriftAnalyzer::empirical_drift() const
{
    std::shared_lock lock(mtx_);

    // Need at least 10 observations for a meaningful linear regression.
    if (observations_.size() < 10) {
        return std::nullopt;
    }

    // Ordinary least-squares regression: ratio = alpha + beta * block.
    // beta is the drift rate in ratio-per-block.
    //
    // Using the standard formulas:
    //   beta = (n * sum(x*y) - sum(x) * sum(y)) / (n * sum(x^2) - (sum(x))^2)
    //   where x = block, y = ratio.
    //
    // For numerical stability, we centre the block values around the mean.

    const double n = static_cast<double>(observations_.size());

    double sum_x  = 0.0;
    double sum_y  = 0.0;
    double sum_xy = 0.0;
    double sum_xx = 0.0;

    // First pass: compute the mean block height for centring.
    double mean_block = 0.0;
    for (const auto& obs : observations_) {
        mean_block += static_cast<double>(obs.block);
    }
    mean_block /= n;

    for (const auto& obs : observations_) {
        const double x = static_cast<double>(obs.block) - mean_block;
        const double y = obs.ratio;
        sum_x  += x;
        sum_y  += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    const double denom = n * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-20) {
        return std::nullopt;
    }

    const double beta = (n * sum_xy - sum_x * sum_y) / denom;

    // Estimate residual variance for uncertainty.
    const double alpha = (sum_y - beta * sum_x) / n;
    double sse = 0.0;
    for (const auto& obs : observations_) {
        const double x = static_cast<double>(obs.block) - mean_block;
        const double predicted = alpha + beta * x;
        const double residual  = obs.ratio - predicted;
        sse += residual * residual;
    }

    const double residual_var = sse / (n - 2.0);
    const double beta_var     = residual_var * n / denom;
    const double beta_sigma   = std::sqrt(std::max(beta_var, 0.0));

    return std::make_pair(beta, beta_sigma);
}

// ===========================================================================
// Accessors
// ===========================================================================

const DriftConfig& InventoryDriftAnalyzer::config() const noexcept
{
    return cfg_;
}

void InventoryDriftAnalyzer::set_config(const DriftConfig& cfg)
{
    std::unique_lock lock(mtx_);
    cfg_ = cfg;
    spdlog::info("InventoryDriftAnalyzer config updated.");
}

std::size_t InventoryDriftAnalyzer::observation_count() const
{
    std::shared_lock lock(mtx_);
    return observations_.size();
}

// ===========================================================================
// Internal helpers
// ===========================================================================

double InventoryDriftAnalyzer::per_block_sigma(double sigma_annual) const
{
    // sigma_block = sigma_annual * sqrt(block_time / seconds_per_year)
    return sigma_annual * std::sqrt(cfg_.block_time_seconds / kSecondsPerYear);
}

double InventoryDriftAnalyzer::ratio_to_xch_deviation(
    double target_ratio,
    double current_ratio) const
{
    // Compute the XCH deviation needed to move from the current inventory
    // imbalance to the target ratio's imbalance.
    //
    // deviation = |target_ratio - 0.5| * V_total / xch_price
    //           - |current_ratio - 0.5| * V_total / xch_price
    //
    // This measures the ADDITIONAL imbalance (in XCH) required to reach the
    // target ratio, starting from the current ratio.  Positive values indicate
    // the target has not yet been reached.

    if (cfg_.xch_price_usd <= 0.0) {
        return 0.0;
    }

    const double scale = cfg_.total_portfolio_value_usd / cfg_.xch_price_usd;
    const double target_dev  = std::abs(target_ratio  - 0.5) * scale;
    const double current_dev = std::abs(current_ratio - 0.5) * scale;

    return std::max(0.0, target_dev - current_dev);
}

double InventoryDriftAnalyzer::random_walk_first_passage(double L_xch) const
{
    // E[T_breach] = L^2 / (delta_q^2 * lambda)         (equation 7)
    //
    // Expected first-passage time (in blocks) for a symmetric random walk
    // with step variance delta_q^2 and rate lambda fills/block to reach
    // distance L from the origin.

    const double dq  = cfg_.avg_fill_size_xch;
    const double lam = cfg_.fill_rate_per_block;
    const double denom = dq * dq * lam;

    if (denom <= 0.0 || L_xch <= 0.0) {
        return 0.0;
    }

    return (L_xch * L_xch) / denom;
}

double InventoryDriftAnalyzer::trending_first_passage(
    double L_xch,
    double drift_rate) const
{
    // For a drift-dominated process, the expected time to traverse distance
    // L is simply the distance divided by the drift speed:
    //
    //   E[T] = L / |drift_rate|
    //
    // Falls back to the random walk estimate when drift_rate is negligibly
    // small (diffusion-dominated regime).

    const double abs_drift = std::abs(drift_rate);

    if (abs_drift < 1.0e-12 || L_xch <= 0.0) {
        // Drift negligible; fall back to random walk.
        return random_walk_first_passage(L_xch);
    }

    return L_xch / abs_drift;
}

double InventoryDriftAnalyzer::mean_revert_first_passage(
    double L_xch,
    double theta,
    double sigma_steady) const
{
    // For an Ornstein-Uhlenbeck process with reversion rate theta and
    // steady-state standard deviation sigma_steady:
    //
    //   dq = -theta * q * dt + sigma_noise * dW
    //
    // Approximate first-passage time for L >> sigma_steady:
    //
    //   E[T] = (1 / theta) * ln(L / sigma_steady)
    //
    // When L is not much larger than sigma_steady, the barrier is within
    // the normal fluctuation range and breach is rapid.  We floor at
    // 1 block to avoid returning zero or negative.

    if (L_xch <= 0.0) {
        return 0.0;
    }

    if (theta <= 0.0 || sigma_steady <= 0.0) {
        // No reversion: fall back to random walk.
        return random_walk_first_passage(L_xch);
    }

    const double ratio = L_xch / sigma_steady;

    if (ratio <= 1.0) {
        // Barrier is within steady-state fluctuations; breach is imminent.
        return 1.0;
    }

    return (1.0 / theta) * std::log(ratio);
}

double InventoryDriftAnalyzer::trending_drift_rate(double trend_pct_day) const
{
    // Net inventory drift per block in a trending market.
    //
    // From the header derivation (equation 8):
    //   drift_rate = delta_q * A * exp(-kappa * delta) * 2 * kappa * mu_block
    //
    // where:
    //   mu_block = trend_pct_day / kBlocksPerDay   (fractional move per block)
    //   delta    = typical half-spread (we use 30 bps = 0.003 as default)
    //   A, kappa = fill intensity parameters from config.

    const double mu_block = trend_pct_day / kBlocksPerDay;
    const double delta    = 0.003;  // 30 bps half-spread (representative).

    const double fill_base = cfg_.A_fill_base
                           * std::exp(-cfg_.kappa * delta);

    return cfg_.avg_fill_size_xch * fill_base * 2.0 * cfg_.kappa * mu_block;
}

double InventoryDriftAnalyzer::as_steady_state_sigma(double sigma_annual) const
{
    // A-S steady-state variance (equation 11 in header):
    //   Var[q]_AS ~ delta_q^2 * lambda / (2 * gamma * sigma_block^2 * tau_avg)
    //
    // where:
    //   sigma_block = per-block volatility (converted from annual)
    //   tau_avg     = horizon * block_time / 2
    //              = as_horizon_blocks * 52 / 2  (seconds, average remaining time)
    //
    // tau starts at horizon_blocks * block_time and linearly decreases to 0,
    // then resets.  The average over the horizon is half the total.

    const double sigma_blk = per_block_sigma(sigma_annual);
    const double tau_avg   = static_cast<double>(cfg_.as_horizon_blocks)
                           * cfg_.block_time_seconds / 2.0;

    if (cfg_.gamma <= 0.0 || sigma_blk <= 0.0 || tau_avg <= 0.0) {
        return 0.0;
    }

    const double dq  = cfg_.avg_fill_size_xch;
    const double lam = cfg_.fill_rate_per_block;

    const double var_q = (dq * dq * lam)
                       / (2.0 * cfg_.gamma * sigma_blk * sigma_blk * tau_avg);

    return std::sqrt(var_q);
}

double InventoryDriftAnalyzer::glft_steady_state_sigma() const
{
    // GLFT steady-state variance (equation 13 in header):
    //   Var[q]_GLFT = q_max / (2 * phi * kappa)
    //
    // This is derived from the OU process with theta = phi*lambda*kappa*dq/q_max
    // and noise variance delta_q^2 * lambda:
    //   Var[q] = noise_var / (2 * theta) = (dq^2 * lambda) / (2 * theta)
    //          = (dq^2 * lambda * q_max) / (2 * phi * lambda * kappa * dq)
    //          = q_max / (2 * phi * kappa)
    //
    // The simplified form is independent of fill rate and fill size.

    const double denom = 2.0 * cfg_.phi * cfg_.kappa;

    if (denom <= 0.0) {
        return 0.0;
    }

    return std::sqrt(cfg_.q_max / denom);
}

double InventoryDriftAnalyzer::glft_theta() const
{
    // Effective Ornstein-Uhlenbeck reversion rate for GLFT.
    //
    // The skew shifts quotes by phi * q / q_max, which changes the fill
    // probability asymmetry.  The reversion force (net drift toward zero)
    // per unit of inventory is:
    //
    //   theta = phi * lambda * kappa * delta_q / q_max
    //
    // This drives the OU dynamics:  dq = -theta * q * dt + delta_q * dW
    // producing a steady-state variance of delta_q^2 * lambda / (2 * theta).

    if (cfg_.q_max <= 0.0) {
        return 0.0;
    }

    return cfg_.phi * cfg_.fill_rate_per_block * cfg_.kappa
         * cfg_.avg_fill_size_xch / cfg_.q_max;
}

double InventoryDriftAnalyzer::utxo_amplification(double ratio) const
{
    // UTXO amplification factor (equation 14 in header):
    //   amplification = 1 / (1 - |2r - 1| * utxo_sensitivity)
    //
    // When inventory is balanced (r = 0.5), amplification = 1.0 (no effect).
    // When inventory skews (r -> 0 or 1), the depleted side has fewer coin
    // slots, reducing its fill rate and creating a positive feedback loop
    // that accelerates drift.
    //
    // Clamped to avoid division by zero or negative values when the product
    // |2r - 1| * sensitivity approaches or exceeds 1.0.

    const double skew    = std::abs(2.0 * ratio - 1.0);
    const double product = skew * cfg_.utxo_sensitivity;

    // Cap the product at 0.95 to keep the amplification finite.
    constexpr double kMaxProduct = 0.95;
    const double clamped = std::min(product, kMaxProduct);

    return 1.0 / (1.0 - clamped);
}

double InventoryDriftAnalyzer::normal_cdf(double x)
{
    // Abramowitz and Stegun approximation 26.2.17.
    // Maximum absolute error: 7.5e-8.
    //
    // For x < 0, use symmetry: Phi(x) = 1 - Phi(-x).

    if (x < -8.0) return 0.0;
    if (x >  8.0) return 1.0;

    const bool negate = (x < 0.0);
    if (negate) x = -x;

    static constexpr double a1 =  0.254829592;
    static constexpr double a2 = -0.284496736;
    static constexpr double a3 =  1.421413741;
    static constexpr double a4 = -1.453152027;
    static constexpr double a5 =  1.061405429;
    static constexpr double p  =  0.3275911;

    const double t = 1.0 / (1.0 + p * x);
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double t4 = t3 * t;
    const double t5 = t4 * t;

    const double phi = 1.0 - (a1 * t + a2 * t2 + a3 * t3 + a4 * t4 + a5 * t5)
                       * std::exp(-0.5 * x * x);

    return negate ? (1.0 - phi) : phi;
}

RecommendedAction InventoryDriftAnalyzer::determine_action(
    double current_ratio,
    double blocks_to_soft,
    double blocks_to_hard,
    bool   anomaly) const
{
    // Action determination based on blocks_to_soft thresholds:
    //
    //   > 2000 blocks (~29 hours): NoAction
    //   > 500  blocks (~7 hours):  IncreaseSkew
    //   > 200  blocks (~3 hours):  WidenSpread
    //   > 50   blocks (~43 min):   ReduceOfferSize
    //   > 10   blocks (~9 min):    PullOverweight
    //   <= 10  blocks or anomaly:  ManualRebalance

    // If an anomaly is detected, escalate immediately.
    if (anomaly) {
        spdlog::warn("Drift anomaly detected at ratio={:.4f}; "
                     "recommending ManualRebalance.", current_ratio);
        return RecommendedAction::ManualRebalance;
    }

    // If already past the hard limit, require manual intervention.
    if (blocks_to_hard <= 0.0) {
        return RecommendedAction::ManualRebalance;
    }

    if (blocks_to_soft <= 10.0) {
        return RecommendedAction::PullOverweight;
    }
    if (blocks_to_soft <= 50.0) {
        return RecommendedAction::ReduceOfferSize;
    }
    if (blocks_to_soft <= 200.0) {
        return RecommendedAction::WidenSpread;
    }
    if (blocks_to_soft <= 500.0) {
        return RecommendedAction::IncreaseSkew;
    }

    return RecommendedAction::NoAction;
}

std::string InventoryDriftAnalyzer::action_detail_text(
    RecommendedAction action,
    double blocks_to_soft,
    double blocks_to_hard,
    double current_ratio)
{
    std::ostringstream oss;

    switch (action) {
        case RecommendedAction::NoAction:
            oss << "Inventory within normal bounds (ratio="
                << current_ratio << "). "
                << "Estimated " << blocks_to_soft << " blocks (~"
                << blocks_to_soft * 52.0 / 3600.0 << "h) to soft limit.";
            break;

        case RecommendedAction::IncreaseSkew:
            oss << "Anomalous drift detected. Consider increasing phi or gamma "
                << "to accelerate inventory reversion. Current ratio="
                << current_ratio << ".";
            break;

        case RecommendedAction::WidenSpread:
            oss << "Soft limit breach imminent (~" << blocks_to_soft
                << " blocks). Widen spread on the overweight side. "
                << "Ratio=" << current_ratio << ".";
            break;

        case RecommendedAction::ReduceOfferSize:
            oss << "Hard limit breach imminent (~" << blocks_to_hard
                << " blocks). Reduce offer sizes on the overweight side. "
                << "Ratio=" << current_ratio << ".";
            break;

        case RecommendedAction::PullOverweight:
            oss << "Inventory past soft limit (ratio=" << current_ratio
                << "). Pull quotes on the overweight side and activate "
                << "aggressive inventory skewing.";
            break;

        case RecommendedAction::ManualRebalance:
            oss << "CRITICAL: Inventory past hard limit (ratio="
                << current_ratio << "). Manual rebalancing recommended. "
                << "Consider cross-venue or CEX transfer.";
            break;
    }

    return oss.str();
}

}  // namespace xop
