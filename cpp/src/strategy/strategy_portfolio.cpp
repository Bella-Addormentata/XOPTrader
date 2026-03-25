// strategy_portfolio.cpp -- Strategy portfolio blending and dynamic weight
//                           rebalancing implementation.
//
// See strategy_portfolio.hpp for the full scholarly basis and interface
// documentation.
//
// ISO/IEC 27001:2022 -- no secrets handled; pure algorithmic computation.
// ISO/IEC 5055       -- no unchecked arithmetic; all edge cases guarded.
// ISO/IEC 25000      -- comprehensive inline documentation.
// ISO/IEC JTC 1/SC 22 -- standard-conforming C++20; no UB.

#include <xop/strategy/strategy_portfolio.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <stdexcept>
#include <limits>
#include <numeric>

namespace xop {

// ===========================================================================
// Free function: to_string(StrategyComponent)
// ===========================================================================

const char* to_string(StrategyComponent c) noexcept
{
    switch (c) {
        case StrategyComponent::AvellanedaStoikov: return "AvellanedaStoikov";
        case StrategyComponent::GLFT:              return "GLFT";
        case StrategyComponent::SpreadOptimizer:   return "SpreadOptimizer";
        case StrategyComponent::ThompsonSampling:  return "ThompsonSampling";
        case StrategyComponent::ChiaEdge:          return "ChiaEdge";
        case StrategyComponent::CoinAgeWeighted:   return "CoinAgeWeighted";
        case StrategyComponent::BlockCadence:      return "BlockCadence";
        case StrategyComponent::MempoolSentinel:   return "MempoolSentinel";
    }
    return "Unknown";
}

// ===========================================================================
// Construction
// ===========================================================================

StrategyPortfolio::StrategyPortfolio(const PortfolioConfig& cfg)
    : cfg_(cfg)
{
    // Validate config invariants.  These parameters come from the config layer
    // and may contain user-supplied values, so throw on invalid input rather
    // than assert (which is stripped in Release builds).
    // ISO/IEC 5055: fail-fast on invalid configuration.
    if (!(cfg_.beta_intensity >= 0.0)) {
        throw std::invalid_argument("PortfolioConfig: beta_intensity must be non-negative");
    }
    if (!(cfg_.min_weight >= 0.0)) {
        throw std::invalid_argument("PortfolioConfig: min_weight must be non-negative");
    }
    if (!(cfg_.max_weight <= 1.0)) {
        throw std::invalid_argument("PortfolioConfig: max_weight must be <= 1.0");
    }
    if (!(cfg_.min_weight < cfg_.max_weight)) {
        throw std::invalid_argument("PortfolioConfig: min_weight must be < max_weight");
    }
    if (!(cfg_.switching_cost_bps >= 0.0)) {
        throw std::invalid_argument("PortfolioConfig: switching_cost_bps must be non-negative");
    }
    if (!(cfg_.crowding_threshold > 0.0)) {
        throw std::invalid_argument("PortfolioConfig: crowding_threshold must be positive");
    }
    if (!(cfg_.crowding_threshold <= 1.0)) {
        throw std::invalid_argument("PortfolioConfig: crowding_threshold must be <= 1.0");
    }
    if (!(cfg_.crowding_decay_factor > 0.0 && cfg_.crowding_decay_factor < 1.0)) {
        throw std::invalid_argument(
            "PortfolioConfig: crowding_decay_factor must be in (0.0, 1.0)");
    }
    if (cfg_.crowding_cooldown_blocks == 0) {
        throw std::invalid_argument(
            "PortfolioConfig: crowding_cooldown_blocks must be > 0");
    }
    if (cfg_.min_fills_for_full_weight == 0) {
        throw std::invalid_argument(
            "PortfolioConfig: min_fills_for_full_weight must be > 0");
    }

    // Enumerate every defined component and initialise with uniform weight.
    static constexpr StrategyComponent kAllComponents[] = {
        StrategyComponent::AvellanedaStoikov,
        StrategyComponent::GLFT,
        StrategyComponent::SpreadOptimizer,
        StrategyComponent::ThompsonSampling,
        StrategyComponent::ChiaEdge,
        StrategyComponent::CoinAgeWeighted,
        StrategyComponent::BlockCadence,
        StrategyComponent::MempoolSentinel
    };

    static constexpr std::size_t kNumComponents =
        sizeof(kAllComponents) / sizeof(kAllComponents[0]);

    // T3-34: Validate that the min_weight constraint is satisfiable.
    // If N * min_weight > 1.0, the iterative clamp/normalize loop in
    // clamp_weights() can never converge because clamping every component
    // to at least min_weight already exceeds the simplex budget of 1.0.
    // Example: 8 strategies * 0.15 = 1.2 > 1.0 -- impossible to satisfy.
    // ISO/IEC 5055: fail-fast on unsatisfiable constraint.
    if (static_cast<double>(kNumComponents) * cfg_.min_weight > 1.0) {
        throw std::invalid_argument(
            "PortfolioConfig: num_components (" + std::to_string(kNumComponents)
            + ") * min_weight (" + std::to_string(cfg_.min_weight)
            + ") = " + std::to_string(
                static_cast<double>(kNumComponents) * cfg_.min_weight)
            + " > 1.0; clamp_weights cannot converge");
    }

    // Uniform initialisation: 1/N for each of the N components.
    const double uniform = 1.0 / static_cast<double>(kNumComponents);

    for (auto comp : kAllComponents) {
        ComponentState state{};
        state.weight          = uniform;
        state.previous_weight = uniform;
        components_[comp] = state;
    }

    spdlog::info("StrategyPortfolio initialised: {} components, uniform weight={:.4f}, "
                 "beta={:.2f}, lookback={} blocks",
                 kNumComponents, uniform, cfg_.beta_intensity,
                 cfg_.pnl_lookback_blocks);
}

// ===========================================================================
// PnL attribution
// ===========================================================================

void StrategyPortfolio::record_pnl(StrategyComponent component,
                                    double realized_pnl_bps,
                                    double adverse_selection_bps,
                                    BlockHeight block)
{
    // T2-02: Exclusive lock -- record_pnl mutates components_ (pnl_history,
    // trailing totals, fill counts).
    std::unique_lock lock(mtx_);

    // Look up the component state.  All valid components are initialised in
    // the constructor; unknown components must be rejected to prevent
    // operator[] from default-constructing a spurious entry in the map.
    auto it = components_.find(component);
    if (it == components_.end()) {
        spdlog::warn("[StrategyPortfolio] Unknown component {}, ignoring PnL record",
                     static_cast<int>(component));
        return;
    }
    auto& state = it->second;

    // Build and store the PnL record.
    StrategyPnLRecord record{};
    record.component            = component;
    record.realized_pnl_bps     = realized_pnl_bps;
    record.adverse_selection_bps = adverse_selection_bps;
    record.block                = block;

    state.pnl_history.push_back(record);

    // Update running totals.
    state.trailing_pnl_bps     += realized_pnl_bps;
    state.trailing_adverse_bps += adverse_selection_bps;
    state.fill_count           += 1;
    state.historical_fill_count += 1;

    spdlog::debug("StrategyPortfolio::record_pnl -- component={}, pnl={:.2f} bps, "
                  "adverse={:.2f} bps, block={}",
                  to_string(component), realized_pnl_bps,
                  adverse_selection_bps, block);
}

// ===========================================================================
// Weight recomputation (called once per block)
// ===========================================================================

void StrategyPortfolio::recompute_weights(MarketRegime regime,
                                           BlockHeight current_block)
{
    // T2-02: Exclusive lock -- recompute_weights mutates components_ (weights,
    // previous_weight, intensity_score, pnl_history, trailing totals,
    // crowding state) and current_regime_.
    std::unique_lock lock(mtx_);

    // Log regime transitions for operational visibility.
    if (regime != current_regime_) {
        spdlog::info("StrategyPortfolio::recompute_weights -- regime changed: {} -> {}",
                     to_string(current_regime_), to_string(regime));
    }
    current_regime_ = regime;

    // Step 1: Trim stale PnL history outside the lookback window.
    trim_pnl_history(current_block);

    // Step 2: Snapshot previous weights for switching-cost computation.
    for (auto& [comp, state] : components_) {
        state.previous_weight = state.weight;
    }

    // Step 3: Compute Brock-Hommes intensity scores incorporating regime priors.
    compute_intensity_scores(regime);

    // Step 4: Softmax normalisation -- convert intensities to weights summing to 1.
    normalize_weights();

    // Step 5: Enforce min/max constraints and renormalise.
    clamp_weights();

    spdlog::debug("StrategyPortfolio::recompute_weights -- block={}, regime={}",
                  current_block, to_string(regime));
}

// ===========================================================================
// Weight accessors
// ===========================================================================

double StrategyPortfolio::weight(StrategyComponent component) const
{
    // T2-02: Shared lock -- read-only access to components_.
    std::shared_lock lock(mtx_);

    auto it = components_.find(component);
    if (it == components_.end()) {
        return 0.0;
    }
    return it->second.weight;
}

std::vector<StrategyWeight> StrategyPortfolio::all_weights() const
{
    // T2-02: Shared lock -- read-only access to components_.
    std::shared_lock lock(mtx_);

    std::vector<StrategyWeight> result;
    result.reserve(components_.size());

    for (const auto& [comp, state] : components_) {
        StrategyWeight sw{};
        sw.component       = comp;
        sw.weight          = state.weight;
        sw.recent_pnl_bps  = state.trailing_pnl_bps - state.trailing_adverse_bps;
        sw.intensity_score = state.intensity_score;
        result.push_back(sw);
    }

    // Sort by component enum value for deterministic ordering.
    std::sort(result.begin(), result.end(),
              [](const StrategyWeight& a, const StrategyWeight& b) {
                  return static_cast<std::uint8_t>(a.component)
                       < static_cast<std::uint8_t>(b.component);
              });

    return result;
}

// ===========================================================================
// Quote blending
// ===========================================================================

StrategyPortfolio::BlendedQuote StrategyPortfolio::blend(
    double mid_price,
    const std::unordered_map<StrategyComponent, QuoteResult,
                             StrategyComponentHash>& component_quotes) const
{
    // T2-02: Shared lock -- read-only access to components_.
    // NOTE: weight() lookup is inlined to avoid re-entrant shared lock
    // acquisition (std::shared_mutex is non-recursive on Windows SRWLOCK).
    std::shared_lock lock(mtx_);

    // Accumulate weighted sums.  Only components present in the quote map
    // contribute; their weights are renormalised proportionally so the blend
    // always reflects a full allocation.
    double total_weight = 0.0;
    double w_bid_price  = 0.0;
    double w_ask_price  = 0.0;
    double w_bid_size   = 0.0;
    double w_ask_size   = 0.0;

    for (const auto& [comp, quote] : component_quotes) {
        // Inlined from weight(): look up directly to avoid re-entrant lock.
        double w = 0.0;
        auto wit = components_.find(comp);
        if (wit != components_.end()) {
            w = wit->second.weight;
        }
        if (w <= 0.0) {
            continue;  // Component has no allocation; skip.
        }
        total_weight += w;
        w_bid_price  += w * quote.bid_price;
        w_ask_price  += w * quote.ask_price;
        w_bid_size   += w * quote.bid_size;
        w_ask_size   += w * quote.ask_size;
    }

    BlendedQuote result{};

    if (total_weight < 1e-12) {
        // No contributing components -- return a degenerate quote at mid.
        spdlog::warn("StrategyPortfolio::blend -- no components with positive weight; "
                     "returning degenerate quote at mid={:.6f}", mid_price);
        result.bid_price  = mid_price;
        result.ask_price  = mid_price;
        result.bid_size   = 0.0;
        result.ask_size   = 0.0;
        result.spread_bps = 0.0;
        return result;
    }

    // Normalise by total_weight so that the blend accounts for missing components.
    const double inv_tw = 1.0 / total_weight;
    result.bid_price = w_bid_price * inv_tw;
    result.ask_price = w_ask_price * inv_tw;
    result.bid_size  = w_bid_size  * inv_tw;
    result.ask_size  = w_ask_size  * inv_tw;

    // Defensive: ask must always exceed bid.
    if (result.ask_price <= result.bid_price) {
        result.ask_price = result.bid_price + 1e-12;
    }

    // Compute spread in basis points: 10000 * (ask - bid) / mid.
    result.spread_bps = (mid_price > 0.0)
        ? 10000.0 * (result.ask_price - result.bid_price) / mid_price
        : 0.0;

    return result;
}

// ===========================================================================
// Crowding detection (Farmer & Joshi 2002)
// ===========================================================================

bool StrategyPortfolio::is_crowded(StrategyComponent component) const
{
    // T2-02: Shared lock -- read-only access to cfg_ and components_.
    // NOTE: When called from compute_intensity_scores() (which runs under
    // the exclusive lock held by recompute_weights()), this shared lock
    // would deadlock because std::shared_mutex is non-recursive on Windows.
    // To handle that case, compute_intensity_scores() inlines this logic
    // directly.  This public method is safe for external callers only.
    std::shared_lock lock(mtx_);

    if (!cfg_.enable_crowding_detection) {
        return false;
    }

    auto it = components_.find(component);
    if (it == components_.end()) {
        return false;
    }

    const auto& state = it->second;

    // Need at least one complete lookback window of history to establish
    // a historical baseline fill rate.
    if (state.historical_windows == 0) {
        return false;
    }

    // Component must have meaningful allocation to be crowding-detectable.
    if (state.weight <= cfg_.min_weight) {
        return false;
    }

    // Historical average fills per lookback window.
    const double avg_fills_per_window =
        static_cast<double>(state.historical_fill_count)
        / static_cast<double>(state.historical_windows);

    // Guard: if no fills historically, cannot detect a decline.
    if (avg_fills_per_window < 1e-6) {
        return false;
    }

    // Current fill rate in this window.
    const double current_fills = static_cast<double>(state.fill_count);

    // Fraction of historical average achieved.
    const double fill_ratio = current_fills / avg_fills_per_window;

    // Crowded if fill rate has dropped by more than the threshold.
    // A fill_ratio of 0.7 means 30% decline; threshold default = 0.30.
    const bool crowded = (fill_ratio < (1.0 - cfg_.crowding_threshold));

    if (crowded) {
        spdlog::info("StrategyPortfolio::is_crowded -- {} flagged as crowded: "
                     "current_fills={:.0f}, avg={:.1f}, ratio={:.3f}",
                     to_string(component), current_fills, avg_fills_per_window,
                     fill_ratio);
    }

    return crowded;
}

// ===========================================================================
// Priority ranking
// ===========================================================================

std::vector<StrategyComponent> StrategyPortfolio::priority_ranking() const
{
    // T2-02: Shared lock -- read-only access to components_.
    std::shared_lock lock(mtx_);

    // Build a vector of (component, net_pnl) pairs.
    struct Ranked {
        StrategyComponent component;
        double            net_pnl;
    };

    std::vector<Ranked> ranking;
    ranking.reserve(components_.size());

    for (const auto& [comp, state] : components_) {
        const double net = state.trailing_pnl_bps - state.trailing_adverse_bps;
        ranking.push_back(Ranked{comp, net});
    }

    // Sort descending by net PnL (highest expected value first).
    std::sort(ranking.begin(), ranking.end(),
              [](const Ranked& a, const Ranked& b) {
                  return a.net_pnl > b.net_pnl;
              });

    // Extract component identifiers in sorted order.
    std::vector<StrategyComponent> result;
    result.reserve(ranking.size());
    for (const auto& r : ranking) {
        result.push_back(r.component);
    }

    return result;
}

// ===========================================================================
// Brock-Hommes intensity-of-choice computation
// ===========================================================================

void StrategyPortfolio::compute_intensity_scores(MarketRegime regime)
{
    // For each component c:
    //   net_pnl_c    = trailing_pnl_bps_c - trailing_adverse_bps_c
    //   base_w_c     = regime_prior_weight(c, regime)
    //   switch_cost  = switching_cost_bps * |weight_c - previous_weight_c|
    //   intensity_c  = base_w_c * exp( beta * net_pnl_c - switch_cost )
    //
    // The base weight acts as a Bayesian prior: the regime detector's opinion
    // about which strategies should be emphasised is baked into the prior,
    // while the PnL history provides the data-driven update.  The switching
    // cost penalises rapid reallocation, damping oscillation.
    //
    // Crowding integration (Farmer & Joshi 2002):
    //   When a component is detected as crowded, its intensity is reduced by
    //   a geometric decay factor (default 0.90 = 10% per evaluation) rather
    //   than the original binary halving that caused a death spiral.  If the
    //   component reaches min_weight and stays there for crowding_cooldown_blocks
    //   consecutive blocks, the crowding flag is cleared to allow re-evaluation.
    //   This prevents permanent strategy extinction per Lo (2004).
    //
    // Numerical guard: cap the exponent argument to avoid overflow in exp().
    // On IEEE 754 double, exp(709) ~ 8.2e307 which is near DBL_MAX.
    // We cap at 500.0 which gives exp(500) ~ 1.4e217 -- large but safe.
    static constexpr double kMaxExponentArg = 500.0;

    // Tolerance for "at min_weight" comparison.  Accounts for floating-point
    // drift from normalization arithmetic.
    static constexpr double kMinWeightTolerance = 1e-6;

    for (auto& [comp, state] : components_) {
        // --- Crowding cooldown recovery ---
        // Track how long this component has been sitting at or near min_weight.
        // After crowding_cooldown_blocks consecutive blocks at the floor, clear
        // the crowding flag so the component can compete fairly again.
        // ISO/IEC 25000: prevents permanent strategy extinction (Lo 2004).
        if (state.weight <= cfg_.min_weight + kMinWeightTolerance) {
            ++state.blocks_at_min_weight;
        } else {
            state.blocks_at_min_weight = 0;
        }

        if (state.crowding_flagged &&
            state.blocks_at_min_weight >= cfg_.crowding_cooldown_blocks)
        {
            spdlog::info("StrategyPortfolio -- {} crowding cooldown expired "
                         "after {} blocks at min_weight; clearing flag",
                         to_string(comp), state.blocks_at_min_weight);
            state.crowding_flagged     = false;
            state.blocks_at_min_weight = 0;
        }

        // --- Crowding detection ---
        // Re-evaluate crowding status on each call.
        // T2-02: is_crowded() logic is inlined here to avoid re-entrant lock
        // acquisition.  This method runs under the exclusive lock held by
        // recompute_weights(); calling the public is_crowded() would deadlock
        // because std::shared_mutex is non-recursive on Windows SRWLOCK.
        {
            bool crowded_now = false;
            if (cfg_.enable_crowding_detection
                && state.historical_windows > 0
                && state.weight > cfg_.min_weight)
            {
                const double avg_fills_per_window =
                    static_cast<double>(state.historical_fill_count)
                    / static_cast<double>(state.historical_windows);
                if (avg_fills_per_window >= 1e-6) {
                    const double current_fills_d = static_cast<double>(state.fill_count);
                    const double fill_ratio_d = current_fills_d / avg_fills_per_window;
                    crowded_now = (fill_ratio_d < (1.0 - cfg_.crowding_threshold));
                    if (crowded_now) {
                        spdlog::info("StrategyPortfolio -- {} crowded: "
                                     "current_fills={:.0f}, avg={:.1f}, ratio={:.3f}",
                                     to_string(comp), current_fills_d,
                                     avg_fills_per_window, fill_ratio_d);
                    }
                }
            }
            if (crowded_now) {
                state.crowding_flagged = true;
            }
        }

        // Net PnL: realised profit minus adverse-selection cost.
        const double net_pnl = state.trailing_pnl_bps - state.trailing_adverse_bps;

        // T3-26: Dampen beta by fill count to stabilise the Brock-Hommes
        // discrete-choice update under sparse data.  With only 2-3 fills in
        // a lookback window, a single fortunate fill can cause 10%+ weight
        // reallocation.  We scale beta linearly with fill density:
        //   effective_beta = beta * min(1.0, fill_count / min_fills)
        // so that below the minimum fill threshold, the intensity-of-choice
        // signal is attenuated proportionally.
        // Brock & Hommes (1998): the discrete-choice model assumes sufficient
        // observations; this corrects for the small-sample regime.
        const double fill_ratio_for_beta = std::min(
            1.0,
            static_cast<double>(state.fill_count)
                / static_cast<double>(cfg_.min_fills_for_full_weight));
        const double effective_beta = cfg_.beta_intensity * fill_ratio_for_beta;

        // Regime-dependent base weight (prior).
        const double base_w = regime_prior_weight(comp, regime);

        // Switching cost penalty: proportional to the absolute change in weight
        // since the previous rebalance.
        const double switch_penalty =
            cfg_.switching_cost_bps * std::abs(state.weight - state.previous_weight);

        // Exponent argument with overflow protection.
        // Uses effective_beta (fill-count-dampened) instead of raw beta_intensity.
        const double exponent = std::clamp(
            effective_beta * net_pnl - switch_penalty,
            -kMaxExponentArg,
            kMaxExponentArg);

        // Intensity = prior * exp(beta * net_pnl - switch_cost).
        // The prior must be strictly positive to prevent permanent extinction
        // of a component.  We floor it at a small epsilon.
        const double safe_base = std::max(base_w, 1e-8);
        double intensity = safe_base * std::exp(exponent);

        // Apply geometric crowding decay instead of binary halving.
        // The decay factor (default 0.90) reduces the intensity by 10% per
        // evaluation, converging gradually rather than collapsing in 3-4 steps.
        // This avoids the death spiral where reduced weight -> fewer fills ->
        // still crowded -> halved again -> permanent extinction at min_weight.
        if (state.crowding_flagged) {
            intensity *= cfg_.crowding_decay_factor;
            spdlog::debug("StrategyPortfolio -- {} intensity reduced by crowding "
                          "decay ({:.2f}x), blocks_at_min={}",
                          to_string(comp), cfg_.crowding_decay_factor,
                          state.blocks_at_min_weight);
        }

        state.intensity_score = intensity;
    }
}

// ===========================================================================
// Regime prior weight lookup
// ===========================================================================

double StrategyPortfolio::regime_prior_weight(StrategyComponent component,
                                               MarketRegime regime) const
{
    // Select the appropriate prior weight table for the current regime.
    const PortfolioConfig::RegimeWeights* rw = nullptr;

    switch (regime) {
        case MarketRegime::MeanReverting:
            rw = &cfg_.mean_reverting_weights;
            break;
        case MarketRegime::Momentum:
            rw = &cfg_.trending_weights;
            break;
        case MarketRegime::Random:
            [[fallthrough]];
        default:
            rw = &cfg_.random_walk_weights;
            break;
    }

    // Map the component to its prior weight.  Components not explicitly
    // listed in RegimeWeights receive a residual weight computed so that the
    // four listed priors plus the residual sum to 1.0.
    switch (component) {
        case StrategyComponent::AvellanedaStoikov:
            return rw->as_weight;
        case StrategyComponent::GLFT:
            return rw->glft_weight;
        case StrategyComponent::SpreadOptimizer:
            return rw->spread_opt_weight;
        case StrategyComponent::ChiaEdge:
            return rw->chia_edge_weight;
        default: {
            // Remaining components (ThompsonSampling, CoinAgeWeighted,
            // BlockCadence, MempoolSentinel) share the residual weight
            // equally.  Residual = 1.0 - sum(four named priors).
            const double named_sum = rw->as_weight + rw->glft_weight
                                   + rw->spread_opt_weight + rw->chia_edge_weight;
            // Clamp residual to non-negative in case named priors already sum >= 1.
            const double residual = std::max(0.0, 1.0 - named_sum);
            // Four unnamed components share the residual equally.
            static constexpr double kNumResidual = 4.0;
            return residual / kNumResidual;
        }
    }
}

// ===========================================================================
// Softmax normalisation
// ===========================================================================

void StrategyPortfolio::normalize_weights()
{
    // Sum all intensity scores.
    double total_intensity = 0.0;
    for (const auto& [comp, state] : components_) {
        total_intensity += state.intensity_score;
    }

    // Guard: if total intensity is effectively zero, fall back to uniform weights.
    if (total_intensity < 1e-30) {
        spdlog::warn("StrategyPortfolio::normalize_weights -- total intensity ~= 0; "
                     "falling back to uniform weights");
        const double uniform = 1.0 / static_cast<double>(components_.size());
        for (auto& [comp, state] : components_) {
            state.weight = uniform;
        }
        return;
    }

    // Normalise: weight_c = intensity_c / sum(intensity_j).
    const double inv_total = 1.0 / total_intensity;
    for (auto& [comp, state] : components_) {
        state.weight = state.intensity_score * inv_total;
    }
}

// ===========================================================================
// Weight clamping with renormalisation
// ===========================================================================

void StrategyPortfolio::clamp_weights()
{
    // Phase 1: Clamp each weight to [min_weight, max_weight].
    // Track the excess/deficit from clamping so we can renormalise.
    // We use an iterative approach: clamp, then distribute the
    // surplus/deficit among the unclamped components.  Repeat until stable.
    //
    // Convergence is guaranteed because each iteration fixes at least one
    // component at a bound, and there are finitely many components.

    // T3-34: Maximum iteration count for the clamp/normalize loop.
    // The constructor validates that N * min_weight <= 1.0, which guarantees
    // theoretical convergence.  This safety net catches any unforeseen edge
    // case (e.g., floating-point pathology) and logs a warning rather than
    // spinning indefinitely.
    // ISO/IEC 5055: bounded iteration prevents unbounded resource consumption.
    static constexpr int kMaxIterations = 20;
    bool converged = false;

    for (int iter = 0; iter < kMaxIterations; ++iter) {
        double sum_clamped   = 0.0;   // Sum of weights that hit a bound.
        double sum_free      = 0.0;   // Sum of weights that are within bounds.
        int    num_free      = 0;

        for (const auto& [comp, state] : components_) {
            if (state.weight <= cfg_.min_weight || state.weight >= cfg_.max_weight) {
                sum_clamped += std::clamp(state.weight, cfg_.min_weight, cfg_.max_weight);
            } else {
                sum_free += state.weight;
                ++num_free;
            }
        }

        // Target for the free components: 1.0 - sum_clamped.
        const double target_free = 1.0 - sum_clamped;

        // If no free components or free sum is zero, we cannot redistribute.
        // Just clamp and accept the rounding error.
        if (num_free == 0 || sum_free < 1e-30) {
            for (auto& [comp, state] : components_) {
                state.weight = std::clamp(state.weight, cfg_.min_weight, cfg_.max_weight);
            }
            converged = true;
            break;
        }

        // Scale the free components to fill the remaining allocation.
        const double scale = target_free / sum_free;
        bool any_newly_clamped = false;

        for (auto& [comp, state] : components_) {
            if (state.weight <= cfg_.min_weight) {
                state.weight = cfg_.min_weight;
            } else if (state.weight >= cfg_.max_weight) {
                state.weight = cfg_.max_weight;
            } else {
                state.weight *= scale;
                // Check if scaling pushed it into a bound.
                if (state.weight <= cfg_.min_weight || state.weight >= cfg_.max_weight) {
                    state.weight = std::clamp(state.weight, cfg_.min_weight, cfg_.max_weight);
                    any_newly_clamped = true;
                }
            }
        }

        // If no new clamps happened this iteration, we are stable.
        if (!any_newly_clamped) {
            converged = true;
            break;
        }
    }

    // T3-34: Log a warning if the loop exhausted its iteration budget.
    // This should never happen when N * min_weight <= 1.0 holds, but
    // the safety net ensures operational visibility if it does.
    if (!converged) {
        spdlog::warn("StrategyPortfolio::clamp_weights -- clamp/normalize loop "
                     "did not converge within {} iterations; proceeding with "
                     "best-effort weights", kMaxIterations);
    }

    // Final renormalisation pass: ensure weights sum to exactly 1.0.
    // Floating-point drift from iterative clamping can leave a small residual.
    double total = 0.0;
    for (const auto& [comp, state] : components_) {
        total += state.weight;
    }

    if (total > 1e-12) {
        const double inv_total = 1.0 / total;
        for (auto& [comp, state] : components_) {
            state.weight *= inv_total;
        }
    }

    // Post-normalization clamp: ensure min/max bounds still hold after
    // the final division.  Without this, weights can drift below min_weight
    // or above max_weight due to floating-point renormalization.
    // ISO/IEC 5055: defensive re-enforcement of invariant after arithmetic.
    for (auto& [comp, state] : components_) {
        state.weight = std::clamp(state.weight, cfg_.min_weight, cfg_.max_weight);
    }

    // Second normalization pass: the post-normalization clamp above can break
    // the simplex constraint (sum == 1.0) by pushing weights inward from both
    // bounds.  Without this pass, the portfolio operates on a weight vector
    // that does not sum to 1.0, causing the blended quote to under- or
    // over-allocate capital.
    // ISO/IEC 5055: re-establish the simplex invariant after every mutation.
    double final_sum = 0.0;
    for (const auto& [comp, state] : components_) {
        final_sum += state.weight;
    }
    if (final_sum > 0.0) {
        const double inv_final = 1.0 / final_sum;
        for (auto& [comp, state] : components_) {
            state.weight *= inv_final;
        }
    }
}

// ===========================================================================
// PnL history trimming
// ===========================================================================

void StrategyPortfolio::trim_pnl_history(BlockHeight current_block)
{
    // Determine the oldest block that falls within the lookback window.
    // BlockHeight is uint32_t, so we guard against underflow.
    const BlockHeight oldest_allowed =
        (current_block > static_cast<BlockHeight>(cfg_.pnl_lookback_blocks))
        ? (current_block - static_cast<BlockHeight>(cfg_.pnl_lookback_blocks))
        : 0;

    for (auto& [comp, state] : components_) {
        // Remove records older than the lookback window.
        while (!state.pnl_history.empty()
               && state.pnl_history.front().block < oldest_allowed)
        {
            state.pnl_history.pop_front();
        }

        // Recompute trailing totals from the remaining history.
        // This avoids drift from incremental additions and removals.
        double pnl_sum     = 0.0;
        double adverse_sum = 0.0;
        std::size_t fills  = 0;

        for (const auto& rec : state.pnl_history) {
            pnl_sum     += rec.realized_pnl_bps;
            adverse_sum += rec.adverse_selection_bps;
            ++fills;
        }

        state.trailing_pnl_bps     = pnl_sum;
        state.trailing_adverse_bps = adverse_sum;
        state.fill_count           = fills;

        // Track how many complete lookback windows have elapsed.
        // We increment once per trim call (called once per block), and the
        // window is "complete" after pnl_lookback_blocks blocks.
        // We use a simple block-count approach: increment on every call
        // and check if we have passed at least one full window.
        if (current_block >= static_cast<BlockHeight>(cfg_.pnl_lookback_blocks)) {
            // At least one full window has elapsed since genesis.
            // Update historical_windows to reflect the number of complete windows.
            const auto windows =
                static_cast<std::size_t>(current_block / cfg_.pnl_lookback_blocks);
            if (windows > state.historical_windows) {
                state.historical_windows = windows;
            }
        }
    }
}

}  // namespace xop
