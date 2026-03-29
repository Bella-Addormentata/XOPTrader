// regime.hpp -- Regime detection module for XOPTrader CHIA DEX market-making bot.
//
// Classifies the current market micro-structure into one of three regimes
// (mean-reverting, random walk, momentum) using the variance ratio test of
// Lo & MacKinlay (1988).  An optional Hidden Markov Model (HMM) with three
// volatility states provides a complementary signal via the Baum-Welch /
// forward algorithm.
//
// The regime classification drives multiplicative adjustments to the spread,
// position size, inventory shedding rate, and offer TTL in the quote engine.
//
// Statistical foundation:
//
//   Variance Ratio Test (Lo & MacKinlay, 1988):
//
//     VR(q) = Var(r_q) / (q * Var(r_1))
//
//   where r_q are overlapping q-block log returns and r_1 are single-block
//   log returns.  Under a random walk (i.i.d. returns) VR = 1.
//
//     VR < 1  =>  negative autocorrelation (mean-reversion)
//     VR > 1  =>  positive autocorrelation (momentum / trending)
//
//   Asymptotic Z-statistic under heteroskedasticity-robust null (Lo-MacKinlay):
//
//     Z = (VR(q) - 1) / sqrt( 2*(2q-1)*(q-1) / (3*q*n) )
//
//   where n = number of single-block returns.  The test rejects the random-walk
//   null at 95% confidence when |Z| > 1.96.
//
//   We run the test at two horizons (q=5 and q=10 blocks) and combine the
//   signals; only when both periods agree do we trigger a regime change.
//
// Hysteresis:
//   A regime transition requires N consecutive blocks (default 5) confirming
//   the new regime before the switch takes effect.  This prevents whipsawing
//   between regimes when the VR hovers near a threshold boundary.
//
// Hidden Markov Model (optional, advanced):
//   Three hidden states (low-vol, normal-vol, high-vol) with Gaussian emission
//   distributions.  Parameters are estimated via Baum-Welch on a rolling window
//   of log returns.  The forward algorithm computes real-time posterior state
//   probabilities.  The Viterbi algorithm recovers the most likely state
//   sequence for diagnostics.
//
// References:
//   Lo, A.W. & MacKinlay, A.C. (1988). "Stock market prices do not follow
//     random walks: Evidence from a simple specification test." Review of
//     Financial Studies, 1(1), 41-66.
//   Rabiner, L.R. (1989). "A tutorial on hidden Markov models and selected
//     applications in speech recognition." Proceedings of the IEEE, 77(2).
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets; pure numerical computation)
//   ISO/IEC 5055        (no raw pointers; bounds-checked containers)
//   ISO/IEC 25000       (comprehensive formulae documentation; clear naming)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++20)

#ifndef XOP_STRATEGY_REGIME_HPP
#define XOP_STRATEGY_REGIME_HPP

#include <xop/strategy/base.hpp>
#include <xop/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// Regime -- market micro-structure classification.
//
//   MeanReverting : VR(q) < lower_threshold  (default 0.85) AND |Z| > vr_z_threshold
//                   Negative return autocorrelation favours tighter spreads
//                   and reduced inventory shedding -- ideal for market making.
//
//   Normal        : VR within thresholds, OR |Z| <= vr_z_threshold (insignificant)
//                   Returns are approximately independent; use default params.
//                   T5-CR5: any VR deviation that is not statistically
//                   significant falls here regardless of raw VR value.
//
//   Momentum      : VR(q) > upper_threshold  (default 1.15) AND |Z| > vr_z_threshold
//                   Positive autocorrelation; widen spreads, shed inventory
//                   aggressively, reduce sizes to limit adverse selection.
// ---------------------------------------------------------------------------

enum class Regime : std::uint8_t {
    MeanReverting = 0,
    Normal        = 1,
    Momentum      = 2
};

/// Human-readable label for logging and Prometheus metrics.
inline const char* to_string(Regime r) noexcept {
    switch (r) {
        case Regime::MeanReverting: return "MeanReverting";
        case Regime::Normal:        return "Normal";
        case Regime::Momentum:      return "Momentum";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// RegimeMultipliers -- multiplicative adjustments applied by the quote engine
//                      based on the current regime.
//
// Each field scales a base parameter from the strategy configuration:
//   spread_mult   -- applied to the base half-spread.
//   size_mult     -- applied to position sizes (order quantity).
//   shedding_mult -- applied to the inventory shedding rate (phi in GLFT).
//   ttl_mult      -- applied to offer TTL (time-to-live in blocks).
//
// Strategy document Section 5:
//   MeanReverting: {0.8, 1.2, 0.5, 1.5}
//   Normal:        {1.0, 1.0, 1.0, 1.0}
//   Momentum:      {1.5, 0.6, 2.0, 0.5}
// ---------------------------------------------------------------------------

struct RegimeMultipliers {
    double spread_mult;     // Applied to base spread.
    double size_mult;       // Applied to position sizes.
    double shedding_mult;   // Applied to inventory shedding rate.
    double ttl_mult;        // Applied to offer TTL.
};

// ---------------------------------------------------------------------------
// RegimeDetectorConfig -- tuning parameters for the variance-ratio detector.
// ---------------------------------------------------------------------------

struct RegimeDetectorConfig {
    // -- Variance ratio test parameters --------------------------------------

    /// Minimum number of single-block log returns required before the VR test
    /// is considered valid.  The window must contain at least this many
    /// observations.  Default 50 blocks (~43 minutes at 52 s/block).
    std::uint32_t min_window_size{50};

    /// Maximum rolling window of single-block log returns to retain.
    /// Older returns are discarded.  Default 200 blocks (~2.9 hours).
    std::uint32_t max_window_size{200};

    /// Short-horizon VR period.  VR is computed for q-block returns.
    /// Default q = 5 blocks (~4.3 minutes).
    std::uint32_t vr_q_short{5};

    /// Long-horizon VR period.  Default q = 10 blocks (~8.7 minutes).
    std::uint32_t vr_q_long{10};

    // -- Regime thresholds ---------------------------------------------------

    /// VR below this value signals mean-reversion.
    double vr_lower_threshold{0.85};

    /// VR above this value signals momentum.
    double vr_upper_threshold{1.15};

    /// Z-statistic significance threshold for the VR test (Lo & MacKinlay
    /// 1988).  A regime is classified as non-random-walk only when the
    /// absolute Z-statistic exceeds this value, i.e. |Z| > vr_z_threshold.
    /// Default 1.96 corresponds to a two-sided 95% confidence level.
    ///
    /// T5-CR5: Without this gate, the raw VR thresholds (0.85 / 1.15)
    /// produce classifications that are not statistically significant at
    /// typical window sizes n=50-200 (Lo & MacKinlay 1989, power ~5-9%).
    /// ISO/IEC 27001:2022 -- deterministic, auditable threshold; no secrets.
    double vr_z_threshold{1.96};

    // -- Hysteresis ----------------------------------------------------------

    /// Number of consecutive blocks that must confirm the new regime
    /// before the detector switches.  Default 5 blocks (~4.3 minutes).
    /// Set to 1 for immediate switching (not recommended).
    std::uint32_t hysteresis_blocks{5};

    // -- Regime multiplier table ---------------------------------------------

    /// Multipliers for the mean-reverting regime.
    RegimeMultipliers mr_multipliers{0.8, 1.2, 0.5, 1.5};

    /// Multipliers for the normal (random walk) regime.
    RegimeMultipliers normal_multipliers{1.0, 1.0, 1.0, 1.0};

    /// Multipliers for the momentum (trending) regime.
    RegimeMultipliers momentum_multipliers{1.5, 0.6, 2.0, 0.5};

    // -- HMM (optional, advanced) --------------------------------------------

    /// Enable the Hidden Markov Model for supplementary regime detection.
    /// When enabled, the HMM posterior probability can override or blend
    /// with the VR test result.
    bool hmm_enabled{false};

    /// Number of Baum-Welch EM iterations per update.  Default 20.
    std::uint32_t hmm_em_iterations{20};

    /// Convergence tolerance for Baum-Welch log-likelihood improvement.
    double hmm_convergence_tol{1e-6};

    // -- MSM (optional, advanced — T5-CR14) -----------------------------------

    /// Enable the Markov-Switching Multifractal model (Calvet & Fisher 2004)
    /// as a complement to or replacement for the HMM.  MSM captures multi-
    /// scale volatility dynamics (short bursts within long trends) with fewer
    /// parameters and better regime stability than single-scale HMMs.
    bool msm_enabled{false};

    /// Number of frequency components (K).  K=2 gives 4 states (2^K),
    /// sufficient for CHIA's low-frequency data.  Higher K requires more
    /// data to identify.
    std::uint32_t msm_k_frequencies{2};

    /// Low-volatility multiplier m_0 ∈ (1, 2).  Each component switches
    /// between m_0 (low) and 2-m_0 (high).  Default 1.4 gives a 3.5:1
    /// high-to-low ratio: 0.6 vs 1.4.
    double msm_m0{1.4};

    /// Short-term transition probability γ_1 (probability the fastest
    /// component flips per block).  Default 0.10.
    double msm_gamma1{0.10};

    /// Frequency spacing ratio b.  Component k transitions with
    /// probability γ_k = 1 - (1 - γ_1)^(b^(k-1)).  Default 2.0.
    double msm_b{2.0};

    /// Base volatility σ̄ (unconditional per-block vol).  If 0, estimated
    /// from the return window.
    double msm_sigma_bar{0.0};
};

// ---------------------------------------------------------------------------
// HmmState -- internal state of the three-state Gaussian HMM.
//
// Hidden states:
//   0 = low-vol   (quiet, mean-reverting market)
//   1 = normal-vol (standard conditions)
//   2 = high-vol   (volatile, trending market)
//
// Observable: per-block log return.
// Emission: Gaussian(mean[s], stddev[s]) for each hidden state s.
// ---------------------------------------------------------------------------

struct HmmState {
    static constexpr std::size_t kNumStates = 3;

    /// Transition matrix A[i][j] = P(state_j at t+1 | state_i at t).
    /// Row-stochastic: each row sums to 1.
    std::array<std::array<double, kNumStates>, kNumStates> transition;

    /// Initial state probabilities pi[s] = P(state_s at t=0).
    std::array<double, kNumStates> initial_prob;

    /// Gaussian emission means per state.
    std::array<double, kNumStates> emission_mean;

    /// Gaussian emission standard deviations per state (must be > 0).
    std::array<double, kNumStates> emission_stddev;

    /// Forward probabilities alpha[s] = P(state_s at current t | observations).
    /// Updated incrementally by the forward algorithm.
    std::array<double, kNumStates> forward_prob;

    /// Most recent log-likelihood from Baum-Welch.
    double log_likelihood{0.0};

    /// Whether the HMM has been initialised via Baum-Welch at least once.
    bool fitted{false};
};

// ---------------------------------------------------------------------------
// MsmState -- internal state of the Markov-Switching Multifractal model.
//
// Calvet, L. & Fisher, A. (2004). "How to Forecast Long-Run Volatility:
//   Regime Switching and the Estimation of Multifractal Processes."
//   Journal of Financial Econometrics, 2(1), 49-83.
//
// The MSM with K frequency components generates 2^K possible volatility
// states.  Each state s = (M_1, ..., M_K) where M_k ∈ {m_0, 2 - m_0}.
// Instantaneous volatility: σ_t = σ̄ · √(∏ M_k,t).
//
// For K=2: states are {(m0,m0), (m0,2-m0), (2-m0,m0), (2-m0,2-m0)},
// giving 4 volatility levels.  The slowest component captures persistent
// regime shifts; the fastest captures short bursts.
// ---------------------------------------------------------------------------

struct MsmState {
    /// Number of frequency components (K).
    std::uint32_t k{2};

    /// Number of states = 2^K.
    std::size_t num_states{4};

    /// Posterior probabilities for each of the 2^K states.
    std::vector<double> state_probs;

    /// Volatility multiplier for each state: √(∏ M_k) for that state.
    std::vector<double> state_vol_multipliers;

    /// Transition probability for each frequency component k.
    std::vector<double> gamma;

    /// Base volatility σ̄.
    double sigma_bar{0.003};

    /// Whether the MSM has been initialised.
    bool initialised{false};
};

// ---------------------------------------------------------------------------
// RegimeDetector -- the primary regime detection class.
//
// Usage (per-block heartbeat):
//
//     RegimeDetector detector(config);
//
//     // On each new block:
//     double log_return = std::log(new_mid / old_mid);
//     detector.update(log_return);
//
//     Regime       r    = detector.get_regime();
//     auto         mult = detector.get_multipliers();
//     double       vr5  = detector.get_variance_ratio(5);
//     double       conf = detector.get_confidence();
//     int          dur  = detector.get_regime_duration_blocks();
//
// Thread safety: NOT thread-safe.  The caller must serialise access.
// ---------------------------------------------------------------------------

class RegimeDetector {
public:
    /// Construct with the given configuration.
    /// Validates config invariants; throws std::invalid_argument on bad input.
    explicit RegimeDetector(const RegimeDetectorConfig& cfg);

    /// Default constructor uses default config.
    RegimeDetector();

    // -- Core interface ------------------------------------------------------

    /// Ingest a new single-block log return.
    ///
    /// This is called once per block (~52 seconds) with:
    ///   log_return = ln(mid_price_new / mid_price_old)
    ///
    /// After ingestion the detector:
    ///   1. Appends the return to the rolling window.
    ///   2. Recomputes the variance ratio at both horizons (q_short, q_long).
    ///   3. Evaluates the Z-statistic for significance.
    ///   4. Updates the hysteresis counter for potential regime transitions.
    ///   5. If HMM is enabled, runs one forward-algorithm step and
    ///      periodically re-fits via Baum-Welch.
    void update(double log_return);

    /// Return the current regime classification.
    /// Until enough data has been collected (< min_window_size returns),
    /// the detector returns Regime::Normal.
    Regime get_regime() const noexcept;

    /// Return the multiplicative adjustments for the current regime.
    RegimeMultipliers get_multipliers() const noexcept;

    /// Compute the variance ratio VR(q) using the current rolling window.
    ///
    /// @param q  The aggregation period (number of single-block returns per
    ///           multi-block return).  Must be >= 2.  Returns 1.0 if data
    ///           is insufficient or q is invalid.
    ///
    /// The calculation uses overlapping returns for efficiency:
    ///   r_q(t) = sum_{i=0}^{q-1} r_1(t-i)
    ///
    /// VR(q) = Var(r_q) / (q * Var(r_1))
    ///
    /// where Var is the sample variance.  Under a random walk, VR(q) = 1.
    double get_variance_ratio(std::uint32_t q) const;

    /// Return the confidence level of the current regime classification.
    ///
    /// This is the absolute value of the Z-statistic from the most recent
    /// variance ratio test at the short horizon.  Values > 1.96 indicate
    /// 95% significance; values > 2.576 indicate 99% significance.
    ///
    /// Returns 0.0 if insufficient data.
    double get_confidence() const noexcept;

    /// Return how many consecutive blocks the current regime has persisted.
    /// Minimum value is 1 (the block at which the current regime was confirmed).
    int get_regime_duration_blocks() const noexcept;

    /// Return whether the detector has enough data to produce a meaningful
    /// regime classification.  Returns false during warm-up (< min_window_size
    /// observations), during which get_regime() returns Regime::Normal by
    /// default.  Callers should widen spreads defensively when !is_ready().
    /// [T7-11]
    bool is_ready() const noexcept;

    // -- Diagnostic accessors ------------------------------------------------

    /// Return the Z-statistic for VR(q) using the Lo-MacKinlay formula.
    ///
    ///   Z = (VR(q) - 1) / sqrt( 2*(2q-1)*(q-1) / (3*q*n) )
    ///
    /// where n is the number of single-block returns in the window.
    /// Returns 0.0 if insufficient data.
    double get_z_statistic(std::uint32_t q) const;

    /// Return the raw regime suggested by VR thresholds (before hysteresis).
    /// Useful for diagnostics to see what the detector "wants" to switch to.
    Regime get_raw_signal() const noexcept;

    /// Return the number of consecutive blocks the raw signal has persisted.
    /// When this reaches hysteresis_blocks, the confirmed regime switches.
    int get_pending_confirmation_count() const noexcept;

    /// Return the number of single-block log returns currently stored.
    std::size_t window_size() const noexcept;

    /// Return a const reference to the configuration.
    const RegimeDetectorConfig& config() const noexcept;

    // -- HMM accessors (meaningful only when hmm_enabled) --------------------

    /// Return the HMM posterior probability of each hidden state at the
    /// current time step.  Returns {0, 0, 0} if HMM is disabled.
    std::array<double, 3> get_hmm_state_probabilities() const noexcept;

    /// Return the most likely HMM state index (0=low-vol, 1=normal, 2=high-vol).
    /// Returns 1 (normal) if HMM is disabled or not yet fitted.
    std::size_t get_hmm_most_likely_state() const noexcept;

    /// Return the most likely state sequence (Viterbi) for the entire
    /// observation window.  Returns an empty vector if HMM is disabled.
    std::vector<std::size_t> get_hmm_viterbi_path() const;

    // -- MSM accessors (T5-CR14, meaningful only when msm_enabled) -----------

    /// Return the MSM-estimated current volatility multiplier (relative to
    /// σ̄).  Values < 1 indicate low-vol, > 1 indicate high-vol.
    /// Returns 1.0 if MSM is disabled or not initialised.
    double get_msm_volatility_multiplier() const noexcept;

    /// Return the MSM posterior probability of being in a high-volatility
    /// state (any state where ∏ M_k > 1).  Returns 0.5 if MSM is disabled.
    double get_msm_high_vol_probability() const noexcept;

private:
    // -- Variance ratio helpers ----------------------------------------------

    /// Compute the VR(q) statistic from the returns window.
    /// Requires returns_.size() >= q + 1.
    double compute_vr(std::uint32_t q) const;

    /// Compute the Z-statistic for a given VR(q) and window size n.
    /// Uses the Lo-MacKinlay heteroskedasticity-consistent formula:
    ///   Z = (VR - 1) / sqrt( 2*(2q-1)*(q-1) / (3*q*n) )
    static double compute_z(double vr, std::uint32_t q, std::size_t n);

    /// Classify a VR value into a Regime using the configured thresholds.
    /// Does NOT apply hysteresis; this is the "raw" signal.
    Regime classify_vr(double vr_short, double vr_long,
                       double z_short, double z_long) const;

    // -- HMM helpers ---------------------------------------------------------

    /// Initialise HMM parameters with reasonable defaults derived from the
    /// return distribution in the current window.
    void hmm_initialise();

    /// Run one pass of the Baum-Welch EM algorithm to re-estimate HMM
    /// parameters from the current observation window.
    void hmm_baum_welch();

    /// Evaluate the Gaussian emission probability N(x | mu, sigma) for
    /// observation x under state s.
    static double hmm_emission_prob(double x, double mean, double stddev);

    /// Execute one forward-algorithm step for a new observation.
    void hmm_forward_step(double observation);

    // -- MSM helpers (T5-CR14) -----------------------------------------------

    /// Initialise MSM state probabilities and volatility multipliers.
    void msm_initialise();

    /// Execute one MSM grid-filter step for a new observation.
    void msm_filter_step(double observation);

    /// Run the Viterbi algorithm over the full observation window.
    std::vector<std::size_t> hmm_viterbi() const;

    /// Sort HMM states by emission standard deviation (ascending) after
    /// Baum-Welch convergence.  Resolves the label-switching ambiguity
    /// inherent in EM so that state indices are deterministic:
    ///   state 0 = lowest stddev  (low-vol regime)
    ///   state 1 = middle stddev  (normal-vol regime)
    ///   state 2 = highest stddev (high-vol regime)
    /// Permutes emission_mean, emission_stddev, initial_prob, transition,
    /// and forward_prob consistently.
    void hmm_sort_states_by_stddev();

    // -- Data members --------------------------------------------------------

    RegimeDetectorConfig cfg_;

    /// Rolling buffer of single-block log returns.
    /// Front = oldest, back = newest.  Capped at max_window_size.
    std::deque<double> returns_;

    /// Current confirmed regime (post-hysteresis).
    Regime confirmed_regime_{Regime::Normal};

    /// Raw (pre-hysteresis) regime signal from the most recent VR test.
    Regime raw_signal_{Regime::Normal};

    /// The regime that the detector is attempting to transition toward.
    /// Only meaningful when pending_count_ > 0.
    Regime pending_regime_{Regime::Normal};

    /// Number of consecutive blocks the raw signal has consistently
    /// indicated pending_regime_ (which differs from confirmed_regime_).
    /// Once this reaches hysteresis_blocks, the confirmed regime switches.
    std::uint32_t pending_count_{0};

    /// Number of blocks the confirmed regime has persisted.
    std::uint32_t regime_duration_{0};

    /// Most recently computed Z-statistics for confidence and diagnostics.
    /// T5-CR5: both horizons stored so that diagnostic logging can emit
    /// the Z-statistic alongside VR for each horizon independently.
    double last_z_short_{0.0};
    double last_z_long_{0.0};

    /// Most recently computed VR values for diagnostics.
    double last_vr_short_{1.0};
    double last_vr_long_{1.0};

    /// Total number of update() calls received.
    std::uint64_t total_updates_{0};

    /// HMM internal state (allocated only when hmm_enabled).
    HmmState hmm_;

    /// MSM internal state (T5-CR14, allocated only when msm_enabled).
    MsmState msm_;

    /// Block counter for periodic HMM re-fitting (every N blocks).
    static constexpr std::uint32_t kHmmRefitInterval = 50;
};

// ---------------------------------------------------------------------------
// RegimeDetector-to-RegimeInfo bridge
//
// The canonical RegimeDetector uses the Regime enum and RegimeMultipliers
// struct, while the strategy consumers (AvellanedaStoikov, GlftStrategy,
// CoinAgeWeightedQuoting, etc.) use the MarketRegime enum and RegimeInfo
// struct defined in base.hpp.
//
// This free function converts the canonical detector state into the
// consumer-facing RegimeInfo, bridging the two type systems so that all
// strategies can delegate regime detection to the single canonical
// RegimeDetector without changing their public interfaces.
//
// T3-01: consolidate 4 variance ratio implementations into shared
// RegimeDetector.
// ---------------------------------------------------------------------------

/// Convert the current state of a RegimeDetector into a RegimeInfo struct.
///
/// Maps:
///   Regime::MeanReverting -> MarketRegime::MeanReverting
///   Regime::Normal        -> MarketRegime::Random
///   Regime::Momentum      -> MarketRegime::Momentum
///
/// The variance_ratio field is populated from the short-horizon VR value.
/// The spread_mult and skew_mult fields are populated from the detector's
/// configured regime multipliers.
///
/// @param detector  The canonical regime detector to query.
/// @return RegimeInfo suitable for consumption by strategy classes.
RegimeInfo to_regime_info(const RegimeDetector& detector);

}  // namespace xop

#endif  // XOP_STRATEGY_REGIME_HPP
