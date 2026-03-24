// drift_analyzer.hpp -- Inventory drift analysis for the XOPTrader CHIA DEX
//                       market-making bot.
//
// PURPOSE
// -------
// This module answers Issue #9: "Would holding too long make the balances
// uneven?"  It provides a rigorous mathematical framework for predicting and
// monitoring inventory drift -- the tendency for holdings to skew away from
// 50/50 base:quote balance over time.
//
// MATHEMATICAL BACKGROUND
// -----------------------
//
// 1. RANDOM-WALK DRIFT (balanced market, no trend)
//
//    In a balanced market, each fill on the bid or ask side is equally likely.
//    The cumulative inventory q(n) after n fills follows a symmetric random
//    walk with step size equal to the average fill size delta_q:
//
//      q(n) = q(0) + sum_{i=1}^{n} X_i,   X_i ~ iid {+delta_q, -delta_q}
//
//    The standard deviation of the inventory position after n fills is:
//
//      sigma_q(n) = delta_q * sqrt(n)                              ... (1)
//
//    To convert from fill-count to block-count, let lambda be the average
//    fill rate (fills per block).  After B blocks:
//
//      n(B) = lambda * B
//      sigma_q(B) = delta_q * sqrt(lambda * B)                    ... (2)
//
//    The inventory ratio r = base_value / (base_value + quote_value) drifts
//    from 0.5 toward a limit at rate:
//
//      E[|r(B) - 0.5|] ~ delta_q * sqrt(lambda * B) / (2 * V_total)  ... (3)
//
//    where V_total is the total portfolio value.
//
//    TIME TO SOFT LIMIT (r = 0.60):
//      The inventory deviation needed is Delta_q_soft = 0.10 * V_total / price.
//      Setting sigma_q(B) = Delta_q_soft and solving for B:
//
//        B_soft = (Delta_q_soft)^2 / (delta_q^2 * lambda)         ... (4)
//
//    TIME TO HARD LIMIT (r = 0.80):
//      Delta_q_hard = 0.30 * V_total / price.
//        B_hard = (Delta_q_hard)^2 / (delta_q^2 * lambda)         ... (5)
//
//    These are EXPECTED FIRST-PASSAGE TIMES for a symmetric random walk to
//    reach an absorbing barrier.  For a symmetric random walk starting at
//    the origin with absorbing barriers at +/- L, the expected first-passage
//    time is:
//
//        E[T_fp] = L^2 / D                                        ... (6)
//
//    where D = delta_q^2 * lambda / 2 is the diffusion coefficient (factor
//    of 2 because the walk can move either direction each fill, giving
//    variance delta_q^2 per fill but the diffusion coefficient for a
//    unit-step walk is 1/2 per step).
//
//    Correction: for a one-sided barrier (we care about hitting +L OR -L,
//    whichever comes first), the expected time is L^2 / (delta_q^2 * lambda).
//    This is because the standard result for a symmetric random walk to hit
//    +L or -L starting from 0 gives E[T] = L^2 when variance per step is 1.
//    Scaling by step variance delta_q^2 and converting steps to time via lambda:
//
//        E[T_breach] = L^2 / (delta_q^2 * lambda)                 ... (7)
//
//    At typical CHIA parameters:
//      V_total = 10,000 USD, XCH price = 2.70 USD
//      delta_q = 50 XCH (typical fill size)
//      lambda  = 0.05 fills/block (one fill every 20 blocks ~ 17 min)
//
//      L_soft = 0.10 * 10000 / 2.70 = 370 XCH
//      B_soft = 370^2 / (50^2 * 0.05) = 136,900 / 125 = 1095 blocks ~ 15.8 hours
//
//      L_hard = 0.30 * 10000 / 2.70 = 1111 XCH
//      B_hard = 1111^2 / (50^2 * 0.05) = 1,234,321 / 125 = 9875 blocks ~ 5.9 days
//
// 2. TRENDING-MARKET DRIFT (directional move)
//
//    When XCH moves directionally at rate mu (fractional price change per
//    block), fills become asymmetric.  A rising price causes our bids to fill
//    more often (takers sell to us), accumulating base.  A falling price
//    causes our asks to fill more often, depleting base.
//
//    The fill-rate asymmetry under A-S / GLFT at a distance delta from mid is:
//      lambda(delta) = A * exp(-kappa * delta)
//
//    A price move of mu per block shifts the effective distance to our bid by
//    -mu and to our ask by +mu (our bid becomes relatively more attractive,
//    our ask less so).  The net inventory drift per block is approximately:
//
//      E[dq/dblock] ~ delta_q * [lambda(delta - mu) - lambda(delta + mu)]
//                   ~ delta_q * A * exp(-kappa * delta) * 2 * kappa * mu
//                     (first-order Taylor expansion for small mu)       ... (8)
//
//    This is O(mu), meaning drift is LINEAR in time:
//      E[q(B)] = q(0) + drift_rate * B                             ... (9)
//
//    For a 5%/day move:  mu_block = 0.05 / (3600*24/52) = 0.05/1662 = 3.0e-5
//    For a 10%/day move: mu_block = 0.10 / 1662 = 6.0e-5
//
//    With kappa = 1.5, delta = 0.003 (30 bps half-spread), A = 100:
//      fill_base = A * exp(-kappa * delta) = 100 * exp(-0.0045) ~ 99.6
//      drift_rate = 50 * 99.6 * 2 * 1.5 * 3.0e-5 = 0.449 XCH/block (5%/day)
//      drift_rate = 50 * 99.6 * 2 * 1.5 * 6.0e-5 = 0.898 XCH/block (10%/day)
//
//    Time to soft limit:
//      B_soft_5pct  = 370 / 0.449 = 824 blocks ~ 11.9 hours
//      B_soft_10pct = 370 / 0.898 = 412 blocks ~ 5.9 hours
//
//    Time to hard limit:
//      B_hard_5pct  = 1111 / 0.449 = 2474 blocks ~ 35.7 hours
//      B_hard_10pct = 1111 / 0.898 = 1237 blocks ~ 17.9 hours
//
// 3. A-S vs GLFT INVENTORY-REVERSION EFFECTIVENESS
//
//    Both models include mechanisms to push inventory back toward zero, but
//    they work differently:
//
//    A-S (Avellaneda-Stoikov):
//      The reservation price r = S - q * gamma * sigma^2 * tau shifts the
//      mid-price by an amount proportional to q * tau.  As tau -> 0 at the
//      end of the rolling horizon, the urgency to flatten increases.  The
//      reversion force is:
//        F_AS = gamma * sigma^2 * tau * q                          ... (10)
//
//      Steady-state variance (with rolling horizon reset every N blocks):
//        Var[q]_AS ~ delta_q^2 * lambda / (2 * gamma * sigma^2 * tau_avg)
//                                                                  ... (11)
//      where tau_avg = N * block_time / 2 (average tau over the horizon).
//
//    GLFT:
//      The skew = phi * q / q_max shifts both quotes by a fixed proportion
//      of inventory, INDEPENDENT of time remaining.  The reversion force is:
//        F_GLFT = phi * q / q_max                                  ... (12)
//
//      This creates an Ornstein-Uhlenbeck mean-reversion dynamic:
//        dq = -theta * q * dt + delta_q * dW
//      where theta = phi * lambda * kappa * delta_q / q_max (effective
//      reversion speed).
//
//      Steady-state variance:
//        Var[q]_GLFT = delta_q^2 * lambda / (2 * theta)
//                    = q_max / (2 * phi * kappa)                   ... (13)
//
//      The steady-state distribution is Normal:
//        q ~ N(0, Var[q]_GLFT)
//
//    COMPARISON:
//      GLFT is superior for 24/7 operation because:
//      - The reversion force is time-invariant (no tau dependence).
//      - The steady-state exists and is well-defined.
//      - A-S's reversion weakens right after each horizon reset (tau is large,
//        so the urgency is low), creating periodic windows of vulnerability.
//      - GLFT's running penalty means the probability of reaching the soft
//        limit is:
//          P(|q| > L_soft) = 2 * Phi(-L_soft / sqrt(Var[q]_GLFT))
//
// 4. COIN-SET (UTXO) FEEDBACK LOOP
//
//    CHIA's UTXO model means each offer locks specific coins.  When inventory
//    skews toward base, the available quote-side coins diminish, reducing the
//    number of bid offers we can post.  This creates a positive feedback loop:
//
//      - Inventory skews toward base
//      - Fewer quote coins available for bids
//      - Fewer bids posted => fewer buys of quote / sells of base
//      - Inventory skews further toward base
//
//    Model: let C_total be the total number of pre-split coin slots.
//    If inventory ratio is r, the available coin slots for each side are:
//      C_bid = C_total * (1 - r)  (quote-side coins available)
//      C_ask = C_total * r        (base-side coins available)
//
//    The effective fill rate on each side is proportional to the number of
//    active offers, which is min(desired_offers, available_coins):
//      lambda_bid(r) = lambda_0 * min(1, C_bid / C_desired)
//      lambda_ask(r) = lambda_0 * min(1, C_ask / C_desired)
//
//    When r > 0.5 (base-heavy), C_bid < C_ask, so bid fill rate drops,
//    exacerbating the skew.  The effective drift gets amplified by factor:
//
//      amplification(r) = 1 / (1 - |2r - 1| * utxo_sensitivity)   ... (14)
//
//    where utxo_sensitivity in [0, 1] captures how constrained the coin pool is.
//    At r = 0.80 (hard limit): amplification = 1 / (1 - 0.6 * s).
//    If s = 0.5 (moderate constraint): amplification = 1.43 (43% faster drift).
//
// 5. 24/7 vs SESSION-BASED EXTRA DRIFT
//
//    Equity markets force a session end (~6.5 hrs) which naturally caps the
//    maximum random-walk excursion.  CHIA runs 24/7, giving the random walk
//    approximately 3.7x more time (24 / 6.5) per "day" to evolve.
//
//    The extra drift risk from continuous operation vs session-based is:
//      sigma_q_24_7 / sigma_q_session = sqrt(24 / 6.5) = sqrt(3.69) = 1.92
//
//    This means the expected time-to-breach is about 3.7x shorter in
//    calendar hours (but the same in trading-hours terms).  More importantly,
//    there is no forced rebalancing event -- the walk can continue indefinitely.
//
//    Additional risks from 24/7 operation:
//      - No overnight position reduction
//      - Low-liquidity periods (Asian night) can produce wider excursions
//      - No opening auction to correct stale prices
//      - Maintenance windows require manual intervention rather than natural pauses
//
// IMPLEMENTATION
// -------------
// The InventoryDriftAnalyzer class:
//   1. Simulates inventory evolution under different market regimes.
//   2. Computes expected time-to-breach for soft/hard limits.
//   3. Monitors actual drift rate and raises alerts when anomalous.
//   4. Provides analyze_drift() -> DriftReport with actionable recommendations.
//
// Thread safety:
//   All public methods acquire at most one mutex.  No nesting.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- audit-ready metrics, controlled state
//   ISO/IEC 5055       -- no raw pointers, RAII locking, bounds checks
//   ISO/IEC 25000      -- comprehensive documentation, single-responsibility
//   ISO/IEC JTC 1/SC 22 -- C++20, no undefined behaviour

#ifndef XOP_RISK_DRIFT_ANALYZER_HPP
#define XOP_RISK_DRIFT_ANALYZER_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"
#include "xop/risk/inventory.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// MarketCondition -- regime classification for drift simulation.
// ---------------------------------------------------------------------------

enum class MarketCondition : std::uint8_t {
    RandomWalk    = 0,  // Balanced fills, drift ~ O(sqrt(T)).
    TrendingUp    = 1,  // Persistent bid fills, drift ~ O(T), base accumulation.
    TrendingDown  = 2,  // Persistent ask fills, drift ~ O(T), base depletion.
    MeanReverting = 3   // Fills oscillate, drift slower than random walk.
};

/// Human-readable label for logging and Prometheus export.
const char* to_string(MarketCondition c) noexcept;

// ---------------------------------------------------------------------------
// RecommendedAction -- operational guidance produced by drift analysis.
//
// Ordered by urgency so that comparison operators reflect escalation.
// ---------------------------------------------------------------------------

enum class RecommendedAction : std::uint8_t {
    NoAction          = 0,  // Drift within normal bounds.
    IncreaseSkew      = 1,  // Raise phi / gamma to accelerate reversion.
    WidenSpread       = 2,  // Widen spread on overweight side.
    ReduceOfferSize   = 3,  // Shrink offer sizes on overweight side.
    PullOverweight    = 4,  // Pull quotes entirely on overweight side.
    ManualRebalance   = 5   // Operator intervention recommended.
};

/// Human-readable label for logging.
const char* to_string(RecommendedAction a) noexcept;

// ---------------------------------------------------------------------------
// DriftReport -- comprehensive inventory drift assessment.
//
// Produced by InventoryDriftAnalyzer::analyze_drift().
// All block counts use CHIA's ~52-second block time.
// ---------------------------------------------------------------------------

struct DriftReport {
    // -- Current state -------------------------------------------------------

    double current_inventory_ratio;     // In [0, 1]; 0.5 = balanced.
    double current_drift_rate;          // Signed XCH per block (positive = accumulating base).
    double drift_rate_sigma;            // Uncertainty in drift rate estimate (XCH/block).

    // -- Time-to-breach forecasts --------------------------------------------

    double expected_blocks_to_soft_limit;   // Expected blocks until r hits 0.60 or 0.40.
    double expected_blocks_to_hard_limit;   // Expected blocks until r hits 0.80 or 0.20.

    double expected_hours_to_soft_limit;    // Convenience: blocks * 52 / 3600.
    double expected_hours_to_hard_limit;    // Convenience: blocks * 52 / 3600.

    // -- Regime-specific forecasts -------------------------------------------

    MarketCondition detected_condition;     // Inferred market regime.
    double trend_magnitude;                 // Estimated |mu| per block (0 for random walk).

    // -- Model comparison ----------------------------------------------------

    double as_steady_state_sigma;       // sqrt(Var[q]) under A-S with current params.
    double glft_steady_state_sigma;     // sqrt(Var[q]) under GLFT with current params.
    double as_breach_probability;       // P(|q| > soft_limit) under A-S steady state.
    double glft_breach_probability;     // P(|q| > soft_limit) under GLFT steady state.

    // -- UTXO feedback -------------------------------------------------------

    double utxo_amplification_factor;   // > 1.0 when coin scarcity accelerates drift.
    double effective_drift_rate;        // current_drift_rate * utxo_amplification.

    // -- 24/7 continuous-operation risk ---------------------------------------

    double continuous_vs_session_factor;  // sqrt(24 / 6.5) ~ 1.92.

    // -- Anomaly detection ---------------------------------------------------

    bool   drift_anomaly_detected;      // True if actual drift exceeds 2-sigma of expected.
    double anomaly_z_score;             // (actual - expected) / sigma.

    // -- Actionable output ---------------------------------------------------

    RecommendedAction recommended_action;
    std::string       action_detail;     // Human-readable explanation of the recommendation.
};

// ---------------------------------------------------------------------------
// DriftConfig -- parameters governing the drift analyzer.
// ---------------------------------------------------------------------------

struct DriftConfig {
    // -- Risk thresholds (match InventoryTracker's RiskConfig) ----------------

    double soft_limit_pct{0.60};        // Portfolio ratio triggering soft limit.
    double hard_limit_pct{0.80};        // Portfolio ratio triggering hard limit.

    // -- Market microstructure -----------------------------------------------

    double fill_rate_per_block{0.05};   // lambda: avg fills per block.
    double avg_fill_size_xch{50.0};     // delta_q: avg fill size in XCH.
    double total_portfolio_value_usd{10000.0};  // V_total for ratio calculations.
    double xch_price_usd{2.70};         // Current XCH price for conversions.

    // -- Strategy parameters (for model comparison) --------------------------

    double gamma{0.01};                 // A-S risk aversion.
    double kappa{1.5};                  // Fill-intensity decay.
    double phi{0.5};                    // GLFT skew strength.
    double q_max{1000.0};              // Maximum inventory in XCH.
    double sigma_annual{0.50};          // Annualised volatility (50% ~ typical crypto).
    double A_fill_base{100.0};          // Fill intensity base rate.

    // -- A-S horizon ---------------------------------------------------------

    uint32_t as_horizon_blocks{120};    // Rolling A-S horizon.

    // -- UTXO model ----------------------------------------------------------

    uint32_t total_coin_slots{20};      // Number of pre-split coin denominations.
    uint32_t desired_offers_per_side{4};// Desired concurrent offers per side.
    double utxo_sensitivity{0.50};      // How much coin scarcity amplifies drift [0, 1].

    // -- Block time ----------------------------------------------------------

    double block_time_seconds{52.0};    // Mean CHIA inter-block interval.

    // -- Session comparison --------------------------------------------------

    double equity_session_hours{6.5};   // Standard US equity session for comparison.

    // -- Anomaly detection ---------------------------------------------------

    double anomaly_z_threshold{2.0};    // Z-score above which drift is flagged as anomalous.

    // -- Drift history window ------------------------------------------------

    uint32_t drift_window_blocks{200};  // Rolling window for empirical drift estimation.
};

// ---------------------------------------------------------------------------
// DriftSimulationResult -- output of a Monte Carlo drift simulation.
// ---------------------------------------------------------------------------

struct DriftSimulationResult {
    double mean_time_to_soft_blocks;     // Mean first-passage time to soft limit (blocks).
    double mean_time_to_hard_blocks;     // Mean first-passage time to hard limit (blocks).
    double p05_time_to_soft_blocks;      // 5th percentile (worst case).
    double p95_time_to_soft_blocks;      // 95th percentile (best case).
    double p05_time_to_hard_blocks;      // 5th percentile.
    double p95_time_to_hard_blocks;      // 95th percentile.
    double steady_state_mean_ratio;      // Mean inventory ratio at convergence.
    double steady_state_sigma_ratio;     // Std dev of inventory ratio at convergence.
    uint32_t num_paths;                  // Number of Monte Carlo paths run.
};

// ---------------------------------------------------------------------------
// TimeToBreachTable -- precomputed table of time-to-breach across conditions.
// ---------------------------------------------------------------------------

struct TimeToBreachEntry {
    MarketCondition condition;
    double trend_pct_per_day;            // 0 for random walk, 5 or 10 for trending.
    double expected_blocks_to_soft;
    double expected_blocks_to_hard;
    double expected_hours_to_soft;
    double expected_hours_to_hard;
};

// ---------------------------------------------------------------------------
// InventoryDriftAnalyzer -- the main analysis engine.
//
// Typical usage:
//   1. Construct with a DriftConfig.
//   2. Call record_observation() each block with the current inventory ratio.
//   3. Call analyze_drift() to get a full DriftReport.
//   4. Optionally call simulate_drift() for Monte Carlo analysis.
//   5. Call compute_breach_table() for a precomputed reference table.
// ---------------------------------------------------------------------------

class InventoryDriftAnalyzer {
public:
    // -- Construction --------------------------------------------------------

    /// Construct with the given configuration.
    explicit InventoryDriftAnalyzer(const DriftConfig& cfg);

    /// Construct from existing inventory tracker and risk config.
    /// Extracts relevant parameters automatically.
    InventoryDriftAnalyzer(const RiskConfig& risk_cfg,
                           double fill_rate_per_block,
                           double avg_fill_size_xch,
                           double total_value_usd,
                           double xch_price_usd);

    // -- Observation recording -----------------------------------------------

    /// Record the current inventory ratio at the given block height.
    /// This feeds the rolling window used for empirical drift estimation.
    ///
    /// @param ratio         Current base-asset fraction in [0, 1].
    /// @param block_height  Block at which this observation was taken.
    void record_observation(double ratio, BlockHeight block_height);

    // -- Core analysis -------------------------------------------------------

    /// Produce a comprehensive drift report for the current state.
    ///
    /// @param current_ratio  Current inventory ratio in [0, 1].
    /// @param sigma          Annualised volatility (e.g. 0.50 for 50%).
    /// @param condition      Market regime classification.
    /// @param trend_pct_day  For trending regimes: daily price change as a
    ///                       fraction (e.g. 0.05 for 5%/day).  Ignored for
    ///                       RandomWalk and MeanReverting.
    ///
    /// @return DriftReport   Full analysis with forecasts and recommendations.
    DriftReport analyze_drift(double          current_ratio,
                              double          sigma,
                              MarketCondition condition,
                              double          trend_pct_day = 0.0) const;

    // -- Monte Carlo simulation ----------------------------------------------

    /// Run Monte Carlo simulation of inventory evolution.
    ///
    /// @param condition       Market regime to simulate.
    /// @param trend_pct_day   Daily trend magnitude (for trending regimes).
    /// @param current_ratio   Current inventory ratio in [0,1]; 0.5 = balanced.
    ///                        The simulation starts from this position rather
    ///                        than assuming a balanced portfolio.
    /// @param num_paths       Number of simulation paths.
    /// @param max_blocks      Maximum blocks to simulate per path.
    /// @param seed            RNG seed (0 = use random device).
    ///
    /// @return DriftSimulationResult  Statistical summary of the simulation.
    DriftSimulationResult simulate_drift(MarketCondition condition,
                                         double          trend_pct_day,
                                         double          current_ratio = 0.5,
                                         uint32_t        num_paths = 10000,
                                         uint32_t        max_blocks = 50000,
                                         uint64_t        seed = 0) const;

    // -- Precomputed reference table -----------------------------------------

    /// Compute the time-to-breach table for all standard market conditions.
    /// Returns entries for: RandomWalk, Trending 5%/day, Trending 10%/day,
    /// MeanReverting.
    ///
    /// @return Vector of TimeToBreachEntry, one per condition.
    std::vector<TimeToBreachEntry> compute_breach_table() const;

    // -- Real-time monitoring ------------------------------------------------

    /// Check whether the current drift rate is anomalous relative to the
    /// expected drift under the given market condition.
    ///
    /// @param condition  Current market regime.
    /// @return True if the z-score of the empirical drift exceeds the
    ///         configured threshold.
    bool is_drift_anomalous(MarketCondition condition) const;

    /// Get the empirical drift rate (change in inventory ratio per block)
    /// estimated from the rolling observation window.
    ///
    /// @return Pair of (drift_rate, sigma) or nullopt if insufficient data.
    std::optional<std::pair<double, double>> empirical_drift() const;

    // -- Accessors -----------------------------------------------------------

    /// Thread-safe copy of the current configuration.
    /// Returns by value to prevent data races with set_config().
    DriftConfig config() const;

    /// Update configuration (e.g. after market conditions change).
    void set_config(const DriftConfig& cfg);

    /// Number of observations currently in the rolling window.
    std::size_t observation_count() const;

    // -- Constants -----------------------------------------------------------

    /// Blocks per day at 52-second block time: 86400 / 52 = 1661.5.
    static constexpr double kBlocksPerDay = 86400.0 / 52.0;

    /// Continuous-vs-session drift ratio: sqrt(24 / 6.5).
    static constexpr double kContinuousVsSessionFactor = 1.9220;  // sqrt(3.6923)

    /// Seconds per year (365 days, non-leap).
    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;

private:
    // -- Internal computation helpers ----------------------------------------

    /// Convert annualised volatility to per-block volatility.
    /// sigma_block = sigma_annual * sqrt(block_time / seconds_per_year).
    double per_block_sigma(double sigma_annual) const;

    /// Compute the inventory deviation (in XCH) needed to reach a given ratio.
    /// Returns the absolute deviation from the balanced (0.5) position.
    ///
    /// @param target_ratio  The limit ratio (e.g. 0.60 for soft, 0.80 for hard).
    /// @param current_ratio Current inventory ratio.
    double ratio_to_xch_deviation(double target_ratio,
                                  double current_ratio) const;

    /// Compute the random-walk expected first-passage time (in blocks) to reach
    /// a deviation of L_xch from the current position.
    ///
    /// Uses formula: B = L^2 / (delta_q^2 * lambda).
    double random_walk_first_passage(double L_xch) const;

    /// Compute the trending-market expected time (in blocks) to reach a
    /// deviation of L_xch, given a net drift rate (XCH per block).
    ///
    /// For drift-dominated motion: B = L / |drift_rate|.
    /// For mixed (drift + diffusion): uses the exact first-passage time for
    /// a Brownian motion with drift.
    double trending_first_passage(double L_xch, double drift_rate) const;

    /// Compute the mean-reversion expected first-passage time for an
    /// Ornstein-Uhlenbeck process with reversion rate theta.
    ///
    /// Approximate: B = (1/theta) * ln(L / sigma_ss) for L >> sigma_ss.
    double mean_revert_first_passage(double L_xch, double theta,
                                     double sigma_steady) const;

    /// Compute the net drift rate (XCH per block) in a trending market.
    ///
    /// Uses the fill-intensity asymmetry formula (equation 8 in the header).
    double trending_drift_rate(double trend_pct_day) const;

    /// Compute the A-S steady-state inventory standard deviation.
    double as_steady_state_sigma(double sigma_annual) const;

    /// Compute the GLFT steady-state inventory standard deviation.
    double glft_steady_state_sigma() const;

    /// Compute the GLFT effective reversion rate theta.
    double glft_theta() const;

    /// Compute the UTXO amplification factor at a given inventory ratio.
    ///
    /// amplification = 1 / (1 - |2r - 1| * utxo_sensitivity).
    double utxo_amplification(double ratio) const;

    /// Compute the cumulative standard normal distribution function.
    /// Uses the Abramowitz-Stegun rational approximation (max error < 7.5e-8).
    static double normal_cdf(double x);

    /// Determine the recommended action given the current state.
    RecommendedAction determine_action(double current_ratio,
                                       double blocks_to_soft,
                                       double blocks_to_hard,
                                       bool   anomaly) const;

    /// Generate human-readable explanation for the recommended action.
    static std::string action_detail_text(RecommendedAction action,
                                          double blocks_to_soft,
                                          double blocks_to_hard,
                                          double current_ratio);

    /// Lock-free implementation of empirical_drift(); caller must hold mtx_.
    /// ISO/IEC 5055 -- CWE-362: separated to avoid re-entrant shared-lock UB.
    std::optional<std::pair<double, double>> empirical_drift_unlocked() const;

    // -- Observation history -------------------------------------------------

    struct Observation {
        double      ratio;
        BlockHeight block;
    };

    mutable std::shared_mutex mtx_;
    std::deque<Observation>   observations_;

    // -- Configuration -------------------------------------------------------

    DriftConfig cfg_;
};

}  // namespace xop

#endif  // XOP_RISK_DRIFT_ANALYZER_HPP
