// market_analyzer.hpp -- Startup market analysis for XOPTrader.
//
// MarketAnalyzer collects a configurable number of blocks of market
// observations before the engine enters active trading.  The goal is to
// characterise the current market micro-structure so that the engine can
// begin trading with an informed initial strategy configuration instead of
// relying entirely on the hard-coded YAML defaults.
//
// Data collected during analysis
// ================================
// For each enabled trading pair the analyzer tracks:
//   - Rolling mid-price observations (one per block).
//   - Rolling spread-in-bps observations.
//   - Rolling 24 h volume observations.
//   - Simple variance-ratio signal (mean-reverting / random / momentum).
//   - Order-book imbalance (bid-depth fraction relative to ask-depth).
//   - Price momentum (signed log-return over the analysis window).
//
// After the analysis window completes, the analyzer produces an
// AnalysisSummary with:
//   - Observed volatility (annualised, derived from log-return std-dev).
//   - Mean spread (bps) and its coefficient of variation.
//   - Regime signal (MeanReverting / Random / Momentum).
//   - Order-book imbalance fraction [0, 1].
//   - Recommended aggressiveness (Conservative / Normal / Aggressive).
//
// Scholarly foundations
// ======================
// The analysis phase follows the "observation-before-action" pattern
// studied in several market-microstructure papers:
//
//   Ho, T. & Stoll, H. (1981).
//     "Optimal dealer pricing under transactions and return uncertainty."
//     Journal of Financial Economics, 9(1), 47-73.
//     → Supports observing adverse-selection environment before quoting.
//
//   Madhavan, A. & Smidt, S. (1993).
//     "An analysis of changes in specialist inventories and quotations."
//     Journal of Finance, 48(5), 1595-1628.
//     → Documents specialist order-book monitoring period before price
//       discovery stabilises after market open.
//
//   Easley, D., Lopez de Prado, M. & O'Hara, M. (2012).
//     "The Volume Clock: Insights into the High-Frequency Paradigm."
//     Journal of Portfolio Management, 39(1), 19-29.
//     → VPIN requires a burn-in window before it becomes reliable.
//
//   Glosten, L. & Milgrom, P. (1985).
//     "Bid, ask and transaction prices in a specialist market with
//      heterogeneously informed traders."
//     Journal of Financial Economics, 14(1), 71-100.
//     → Adverse-selection component of spread requires initial observation.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets stored or logged
//   ISO/IEC 5055       -- no raw pointers, bounded containers
//   ISO/IEC 25000      -- documented interfaces and invariants

#ifndef XOP_DATA_MARKET_ANALYZER_HPP
#define XOP_DATA_MARKET_ANALYZER_HPP

#include <xop/strategy/base.hpp>   // MarketRegime

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// AnalysisAggressiveness -- recommended initial quoting aggressiveness.
//
// Derived from the observed regime and spread variability:
//   Conservative  -- high volatility OR momentum regime.  Use wider spreads
//                    and smaller position sizes during initial trading.
//   Normal        -- mean-reverting OR random-walk regime with stable spreads.
//                    Apply default strategy parameters as configured.
//   Aggressive    -- low volatility AND wide observed spreads (potential
//                    alpha from tighter quoting).  Compress initial spreads.
// ---------------------------------------------------------------------------
enum class AnalysisAggressiveness : std::uint8_t {
    Conservative = 0,
    Normal       = 1,
    Aggressive   = 2
};

/// Human-readable label.
inline const char* to_string(AnalysisAggressiveness a) noexcept {
    switch (a) {
        case AnalysisAggressiveness::Conservative: return "Conservative";
        case AnalysisAggressiveness::Normal:       return "Normal";
        case AnalysisAggressiveness::Aggressive:   return "Aggressive";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// PairAnalysisSummary -- analysis results for a single trading pair.
// ---------------------------------------------------------------------------
struct PairAnalysisSummary {
    std::string pair_name;

    // -- Volatility ---------------------------------------------------------
    double volatility_annual{0.0};    ///< Annualised log-return std-dev.
    double volatility_per_block{0.0}; ///< Per-block log-return std-dev.

    // -- Spread observations ------------------------------------------------
    double mean_spread_bps{0.0};      ///< Mean observed spread (bps).
    double spread_cv{0.0};            ///< Coefficient of variation (σ/μ).

    // -- Regime signal ------------------------------------------------------
    MarketRegime regime{MarketRegime::Random};  ///< Detected regime.
    double variance_ratio{1.0};                 ///< VR(5) estimate.

    // -- Order-book / depth -------------------------------------------------
    double book_imbalance{0.5}; ///< Bid fraction of total depth [0,1];
                                 ///  > 0.5 = more buy interest.

    // -- Momentum -----------------------------------------------------------
    double momentum{0.0};   ///< log(p_last / p_first) over analysis window.

    // -- Volume -------------------------------------------------------------
    double mean_volume_24h{0.0}; ///< Mean observed 24-h volume.

    // -- Recommendation -----------------------------------------------------
    AnalysisAggressiveness aggressiveness{AnalysisAggressiveness::Normal};

    // -- Completeness -------------------------------------------------------
    uint32_t blocks_collected{0}; ///< Actual observations ingested.
    bool     complete{false};     ///< True when analysis has ended (either
                                  ///  the full window was observed OR the
                                  ///  analysis was force-completed due to
                                  ///  a timeout).
    bool     window_filled{false};///< True only when the full observation
                                  ///  window was filled (blocks_collected
                                  ///  >= analysis_blocks).  False after
                                  ///  force_complete() with partial data.
};

// ---------------------------------------------------------------------------
// MarketAnalyzerConfig -- compile-time tunable parameters.
// ---------------------------------------------------------------------------
struct MarketAnalyzerConfig {
    /// Number of blocks to observe before declaring analysis complete.
    /// Default 20 blocks ≈ 17 minutes.  Range [3, 1440].
    uint32_t analysis_blocks{20};

    /// Seconds per block (used for annualisation).
    double block_time_seconds{52.0};

    /// Variance-ratio horizon (short lag, in blocks).
    uint32_t vr_short_lag{5};

    /// VR threshold below which the regime is considered mean-reverting.
    double vr_lower_threshold{0.85};

    /// VR threshold above which the regime is considered momentum.
    double vr_upper_threshold{1.15};

    /// Annualised volatility above which the recommendation is Conservative.
    double high_vol_threshold{0.40};   // 40% annualised.

    /// Spread CV above which the recommendation is Conservative (unstable).
    double high_spread_cv_threshold{0.80};

    /// Minimum mean spread (bps) to trigger Aggressive recommendation.
    double wide_spread_bps_threshold{80.0};

    /// Timeout multiplier: if total block polls exceed
    /// analysis_blocks * timeout_block_multiplier without all pairs
    /// completing, the engine forces completion.  Prevents hanging
    /// indefinitely when a pair has no market data.  Default 3x.
    uint32_t timeout_block_multiplier{3};
};

// ---------------------------------------------------------------------------
// MarketAnalyzer -- collects and analyses market data during startup.
//
// Usage pattern:
//   1. Construct with config and list of enabled pair names.
//   2. After each block, call ingest() for each pair with fresh snapshots.
//   3. Query is_complete() to check if the analysis window is filled.
//   4. On completion, get_summaries() returns per-pair AnalysisSummary.
//
// Thread safety: NOT thread-safe.  The engine calls this from a single
//               coroutine context; no concurrent access is expected.
// ---------------------------------------------------------------------------
class MarketAnalyzer {
public:
    // -- Construction ---------------------------------------------------------

    /// Construct with the given config and enabled pair names.
    ///
    /// @param cfg        Analysis configuration (window length, thresholds).
    /// @param pair_names List of enabled trading pair identifiers.
    explicit MarketAnalyzer(const MarketAnalyzerConfig& cfg,
                            const std::vector<std::string>& pair_names);

    /// Default constructor uses default config and no pairs.
    MarketAnalyzer();

    // -- Core interface -------------------------------------------------------

    /// Ingest a single block's observations for a trading pair.
    ///
    /// @param pair_name     The trading pair being observed.
    /// @param mid_price     Current mid-price (arbitrary unit, e.g. mojos).
    ///                      Must be > 0; ignored if <= 0.
    /// @param spread_bps    Observed spread in basis points.  Must be >= 0.
    /// @param volume_24h    24-hour volume (arbitrary unit).
    /// @param bid_depth     Total bid-side depth (arbitrary unit).
    /// @param ask_depth     Total ask-side depth (arbitrary unit).
    void ingest(const std::string& pair_name,
                double mid_price,
                double spread_bps,
                double volume_24h,
                double bid_depth,
                double ask_depth);

    // -- Queries --------------------------------------------------------------

    /// True when every enabled pair has collected analysis_blocks observations.
    [[nodiscard]] bool is_complete() const noexcept;

    /// Number of blocks collected for the given pair (0 if unknown pair).
    [[nodiscard]] uint32_t blocks_collected(const std::string& pair_name) const noexcept;

    /// Configured analysis window length (blocks).
    [[nodiscard]] uint32_t analysis_blocks() const noexcept;

    /// Configured timeout multiplier for analysis completion.
    [[nodiscard]] uint32_t timeout_block_multiplier() const noexcept;

    /// Per-pair analysis summaries.  Complete() must be true for the
    /// recommendations to be meaningful; calling before completion returns
    /// partial results with complete == false.
    [[nodiscard]] std::vector<PairAnalysisSummary> get_summaries() const;

    /// Summary for a single pair.  Returns a default-initialized summary if
    /// the pair name is not recognised.
    [[nodiscard]] PairAnalysisSummary get_summary(const std::string& pair_name) const;

    /// Reset all accumulated data (useful for re-analysis after reconnect).
    void reset();

    /// Force all pairs to be marked complete (``complete = true``), even
    /// if they have not collected enough blocks.  ``window_filled`` will
    /// remain false for pairs that did not reach ``analysis_blocks``,
    /// allowing callers to distinguish forced vs. fully-observed results.
    /// Used by the engine when the analysis timeout expires (e.g. a pair
    /// has no market data) to prevent the bot from hanging indefinitely
    /// in the Analyzing state.
    void force_complete();

    /// Overall aggressiveness recommendation across all pairs.
    /// Returns the most conservative recommendation among all pairs
    /// (i.e. if any pair recommends Conservative, the overall is
    /// Conservative).  Returns Normal if no pairs are tracked.
    [[nodiscard]] AnalysisAggressiveness overall_recommendation() const;

    /// Spread multiplier derived from the overall aggressiveness
    /// recommendation:
    ///   Conservative → 1.5  (50% wider initial spreads)
    ///   Normal       → 1.0  (no change)
    ///   Aggressive   → 0.8  (20% tighter initial spreads)
    [[nodiscard]] double recommended_spread_multiplier() const;

private:
    // -- Per-pair rolling storage ---------------------------------------------
    struct PairState {
        std::string pair_name;

        // Rolling price history (capped at analysis_blocks + vr_short_lag).
        std::deque<double> prices;      // mid_price per block.
        std::deque<double> spreads;     // spread_bps per block.
        std::deque<double> volumes;     // volume_24h per block.
        std::deque<double> bid_depths;  // bid depth per block.
        std::deque<double> ask_depths;  // ask depth per block.

        uint32_t blocks_collected{0};
        uint32_t total_poll_attempts{0};  ///< Total ingest calls (incl. invalid).
        bool     complete{false};         ///< Analysis ended (natural or forced).
    };

    // -- Helpers --------------------------------------------------------------

    /// Compute the analysis summary for one pair from its accumulated state.
    [[nodiscard]] PairAnalysisSummary compute_summary(const PairState& ps) const;

    /// Compute the variance ratio VR(lag) from the price series.
    /// Returns 1.0 if there are insufficient observations.
    [[nodiscard]] double compute_variance_ratio(const std::deque<double>& prices,
                                                 uint32_t lag) const;

    /// Derive the aggressiveness recommendation from summary statistics.
    [[nodiscard]] AnalysisAggressiveness recommend(
        double vol_annual,
        double spread_cv,
        double mean_spread_bps,
        MarketRegime regime) const;

    // -- Data members ---------------------------------------------------------
    MarketAnalyzerConfig cfg_;
    std::unordered_map<std::string, PairState> states_;
};

}  // namespace xop

#endif  // XOP_DATA_MARKET_ANALYZER_HPP
