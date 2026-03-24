// strategy_portfolio.hpp -- Strategy portfolio blending and dynamic weight
//                           rebalancing for the XOPTrader market-making engine.
//
// Rather than switching FROM one strategy TO another, this module runs multiple
// strategy components simultaneously with dynamically weighted blending.  The
// regime detector controls the blend: more A-S weight in mean-reverting
// regimes, more GLFT weight in trending (momentum) regimes.
//
// Scholarly basis:
//   - Brock, W. & Hommes, C. (1998). "Heterogeneous beliefs and routes to
//     chaos in a simple asset pricing model." Journal of Economic Dynamics &
//     Control, 22, 1235-1274.
//     (Intensity-of-choice model for strategy switching.)
//   - Lo, A.W. (2004). "The Adaptive Markets Hypothesis." Journal of
//     Portfolio Management, 30(5), 15-29.
//     (No strategy should be permanently ruled out -- adaptive allocation.)
//   - Farmer, J.D. & Joshi, S. (2002). "The price dynamics of common
//     trading strategies." Journal of Economic Behavior & Organization, 49(2),
//     149-171.
//     (Profitable strategies attract imitators -- crowding detection.)
//   - Menkveld, A.J. (2013). "High frequency trading and the new market
//     makers." Journal of Financial Markets, 16(4), 712-740.
//     (Diversified portfolios outperform monolithic strategies.)
//
// ISO/IEC 27001:2022 -- no secrets handled; pure algorithmic computation.
// ISO/IEC 5055       -- no raw pointers; bounds-checked containers.
// ISO/IEC 25000      -- clear naming; single-responsibility interface.
// ISO/IEC JTC 1/SC 22 -- standard-conforming C++20; no UB.

#ifndef XOP_STRATEGY_STRATEGY_PORTFOLIO_HPP
#define XOP_STRATEGY_STRATEGY_PORTFOLIO_HPP

#include <xop/strategy/base.hpp>
#include <xop/types.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

// ===========================================================================
// StrategyComponent -- identifies each blendable strategy module.
//
// Each enumerator maps to one of the strategy classes that can independently
// produce a QuoteResult.  The portfolio holds per-component state (weight,
// trailing PnL, fill count) and blends their outputs each block.
// ===========================================================================

enum class StrategyComponent : std::uint8_t {
    AvellanedaStoikov  = 0,   // S1.1 -- inventory-penalized reservation price
    GLFT               = 1,   // S1.2 -- running inventory penalty
    SpreadOptimizer    = 2,   // S1.3 -- four-component spread
    ThompsonSampling   = 3,   // S1.12 -- spread learning
    ChiaEdge           = 4,   // 5-factor CHIA edge multiplier
    CoinAgeWeighted    = 5,   // Coin-age weighted quoting
    BlockCadence       = 6,   // Block-cadence adaptive spread
    MempoolSentinel    = 7    // Mempool sentinel strategy
};

/// Human-readable label for logging and metrics.
const char* to_string(StrategyComponent c) noexcept;

/// Hash support for std::unordered_map keyed by StrategyComponent.
struct StrategyComponentHash {
    std::size_t operator()(StrategyComponent c) const noexcept {
        return static_cast<std::size_t>(c);
    }
};

// ===========================================================================
// StrategyPnLRecord -- one PnL attribution record for a single component
//                      within a given evaluation window.
// ===========================================================================

struct StrategyPnLRecord {
    StrategyComponent component;         // Which component produced this fill.
    double            realized_pnl_bps;  // Realised PnL attributed (bps).
    double            adverse_selection_bps; // Adverse-selection cost (bps).
    BlockHeight       block;             // Block at which the fill occurred.
};

// ===========================================================================
// StrategyWeight -- snapshot of a component's current allocation and scoring.
// ===========================================================================

struct StrategyWeight {
    StrategyComponent component;
    double            weight;            // Current weight in [0, 1]; all sum to 1.
    double            recent_pnl_bps;    // Trailing net PnL for this component.
    double            intensity_score;   // Brock-Hommes intensity-of-choice score.
};

// ===========================================================================
// PortfolioConfig -- tuning parameters for the strategy portfolio.
// ===========================================================================

struct PortfolioConfig {
    // -- Brock-Hommes intensity-of-choice parameter beta -----------------------
    // Higher beta = more aggressive switching toward profitable strategies.
    // Lower beta  = more stable allocation, less oscillation.
    // Recommended: 1.0 - 5.0 for Chia DEX conditions.
    double beta_intensity{2.0};

    // -- PnL lookback window (in blocks) ---------------------------------------
    // ~200 blocks * 52 s/block = 10,400 s ~= 2.9 hours.
    std::size_t pnl_lookback_blocks{200};

    // -- Min / max per-component weight bounds ---------------------------------
    // Lo (2004): no strategy should be permanently ruled out (min > 0).
    // Menkveld (2013): diversified portfolios outperform monolithic ones (max < 1).
    double min_weight{0.05};
    double max_weight{0.60};

    // -- Switching cost penalty (bps) ------------------------------------------
    // Penalises rapid reallocation; prevents oscillation and herding.
    // Brock & Hommes: too-low switching cost causes destabilising herding.
    double switching_cost_bps{5.0};

    // -- Crowding detection (Farmer & Joshi 2002) ------------------------------
    // Enable fill-rate-based crowding detection.
    bool   enable_crowding_detection{true};

    // Fill-rate drop threshold (fraction, e.g. 0.30 = 30% decline).
    // If a component's recent fill rate drops by more than this fraction
    // relative to its historical average while weight > min_weight, the
    // component is flagged as crowded.
    double crowding_threshold{0.30};

    // Crowding cooldown: number of consecutive blocks a component must spend
    // at or near min_weight before the crowding flag is cleared and the
    // component is allowed to re-enter fair evaluation.  500 blocks at ~52s
    // each equals approximately 7.2 hours -- long enough for the competitive
    // landscape to shift, short enough to recover within a trading day.
    // ISO/IEC 25000: prevents permanent strategy extinction (Lo 2004).
    std::size_t crowding_cooldown_blocks{500};

    // Crowding decay factor applied per evaluation instead of binary halving.
    // Geometric decay 0.90 means the weight is reduced by 10% each time
    // crowding is detected, converging gradually rather than collapsing to
    // min_weight in 3-4 iterations (as 0.5 would).
    // Farmer & Joshi (2002): gradual withdrawal is more stable than binary.
    double crowding_decay_factor{0.90};

    // -- Regime-dependent base (prior) weights ---------------------------------
    // These weights are used as priors before PnL-driven adjustment.
    // MeanReverting regime -> heavier A-S, lighter GLFT.
    // Momentum regime      -> heavier GLFT, lighter A-S.
    struct RegimeWeights {
        double as_weight{0.40};           // AvellanedaStoikov prior
        double glft_weight{0.30};         // GLFT prior
        double spread_opt_weight{0.15};   // SpreadOptimizer prior
        double chia_edge_weight{0.15};    // ChiaEdge prior
    };

    RegimeWeights mean_reverting_weights{0.45, 0.20, 0.15, 0.20};
    RegimeWeights trending_weights      {0.20, 0.45, 0.15, 0.20};
    RegimeWeights random_walk_weights   {0.35, 0.35, 0.15, 0.15};
};

// ===========================================================================
// StrategyPortfolio -- blends multiple strategy outputs with dynamically
//                      rebalanced weights driven by intensity-of-choice.
//
// Lifecycle (called once per block by the engine):
//   1. record_pnl()       -- for each fill in this block.
//   2. recompute_weights() -- after all PnL records are pushed.
//   3. blend()            -- to obtain the weighted-average quotation.
//
// Thread safety: not thread-safe.  All methods are called from the single
// engine thread (one block at a time).
// ===========================================================================

class StrategyPortfolio {
public:
    /// Construct with a fully populated configuration.
    /// Initialises all eight components with uniform weight = 1/8.
    /// @throws std::invalid_argument if any config invariant is violated.
    explicit StrategyPortfolio(const PortfolioConfig& cfg);

    // -- PnL attribution -------------------------------------------------------

    /// Record a single PnL attribution event for a strategy component.
    /// Called once per fill (possibly multiple fills per block).
    ///
    /// @param component             Which strategy produced the fill.
    /// @param realized_pnl_bps      Realised PnL in basis points.
    /// @param adverse_selection_bps  Adverse-selection cost in basis points.
    /// @param block                  Block height at which the fill occurred.
    void record_pnl(StrategyComponent component,
                    double realized_pnl_bps,
                    double adverse_selection_bps,
                    BlockHeight block);

    // -- Weight recomputation ---------------------------------------------------

    /// Recompute all component weights using the Brock-Hommes
    /// intensity-of-choice model.  Should be called once per block
    /// after all PnL records for the block have been pushed.
    ///
    /// @param regime        Current market regime from the regime detector.
    /// @param current_block Current block height (for history trimming).
    void recompute_weights(MarketRegime regime, BlockHeight current_block);

    // -- Weight accessors -------------------------------------------------------

    /// Get the current weight for a single component.
    /// Returns 0.0 if the component has never been registered.
    double weight(StrategyComponent component) const;

    /// Get a snapshot of all current weights, sorted by component enum value.
    std::vector<StrategyWeight> all_weights() const;

    // -- Quote blending --------------------------------------------------------

    /// Blended quotation produced by combining component outputs.
    struct BlendedQuote {
        double bid_price;       // Weighted-average bid price.
        double ask_price;       // Weighted-average ask price.
        double bid_size;        // Weighted-average bid size.
        double ask_size;        // Weighted-average ask size.
        double spread_bps;      // Full spread in basis points.
    };

    /// Blend strategy outputs using current weights.
    ///
    /// For each component present in @p component_quotes, its bid/ask prices
    /// and sizes are weighted by the component's current allocation.  Missing
    /// components are skipped (their weight redistributed proportionally among
    /// the present components).
    ///
    /// @param mid_price        Current mid-price for spread-bps computation.
    /// @param component_quotes Map from component to its raw QuoteResult.
    /// @return BlendedQuote    The weighted combination.
    BlendedQuote blend(
        double mid_price,
        const std::unordered_map<StrategyComponent, QuoteResult,
                                 StrategyComponentHash>& component_quotes) const;

    // -- Crowding detection -----------------------------------------------------

    /// Returns true if the given component appears crowded (fill rate has
    /// dropped more than crowding_threshold from its historical average while
    /// weight exceeds min_weight).
    bool is_crowded(StrategyComponent component) const;

    // -- Priority ranking -------------------------------------------------------

    /// Returns components sorted by expected value (highest first), using
    /// the trailing net PnL as the ranking signal.
    std::vector<StrategyComponent> priority_ranking() const;

    // -- Configuration access ---------------------------------------------------

    const PortfolioConfig& config() const noexcept { return cfg_; }

private:
    // -- Internal state per component ------------------------------------------

    struct ComponentState {
        double      weight{0.0};             // Current allocation [0, 1].
        double      previous_weight{0.0};    // Weight at prior recompute (for switch cost).
        double      intensity_score{0.0};    // Brock-Hommes raw intensity.
        double      trailing_pnl_bps{0.0};   // Sum of realised PnL in window.
        double      trailing_adverse_bps{0.0}; // Sum of adverse-selection cost in window.
        std::size_t fill_count{0};           // Fills in the current window.
        std::size_t historical_fill_count{0};// Lifetime fill count (for crowding baseline).
        std::size_t historical_windows{0};   // Number of complete lookback windows observed.
        std::deque<StrategyPnLRecord> pnl_history; // Per-fill records within lookback.

        // Crowding recovery state.
        // Tracks consecutive blocks at or near min_weight so that the
        // crowding flag can be cleared after a cooldown period, preventing
        // permanent strategy extinction (Lo 2004 Adaptive Markets Hypothesis).
        bool        crowding_flagged{false};    // Currently flagged as crowded.
        std::size_t blocks_at_min_weight{0};    // Consecutive blocks at min_weight.
    };

    // -- Brock-Hommes intensity-of-choice computation --------------------------

    /// Compute the raw intensity score for each component:
    ///   intensity_c = base_weight_c(regime) *
    ///                 exp( beta * net_pnl_c - switching_cost * |w_c - w_c_prev| )
    void compute_intensity_scores(MarketRegime regime);

    /// Get the regime-dependent base (prior) weight for a component.
    double regime_prior_weight(StrategyComponent component,
                               MarketRegime regime) const;

    // -- Normalisation ---------------------------------------------------------

    /// Softmax normalisation: weight_c = intensity_c / sum(intensity_j).
    void normalize_weights();

    /// Enforce min_weight / max_weight constraints, then renormalise so
    /// weights still sum to 1.0.
    void clamp_weights();

    // -- History management ----------------------------------------------------

    /// Remove PnL records older than pnl_lookback_blocks from every component
    /// and recompute trailing totals.
    void trim_pnl_history(BlockHeight current_block);

    // -- Data members ----------------------------------------------------------

    PortfolioConfig cfg_;

    /// Per-component bookkeeping, keyed by StrategyComponent.
    std::unordered_map<StrategyComponent, ComponentState,
                       StrategyComponentHash> components_;

    /// Most recently observed regime (for logging deltas).
    MarketRegime current_regime_{MarketRegime::Random};
};

}  // namespace xop

#endif  // XOP_STRATEGY_STRATEGY_PORTFOLIO_HPP
