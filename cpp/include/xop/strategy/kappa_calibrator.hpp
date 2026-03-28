// kappa_calibrator.hpp -- Online κ (fill-intensity decay) calibrator.
//
// Implements T4-16: Rolling MLE estimator for the Poisson fill-intensity
// parameter κ in the model λ(δ) = A · exp(−κ · δ), where δ is the
// half-spread and λ is the fill arrival rate.
//
// The calibrator maintains a set of discrete spread buckets.  Each bucket
// records fill counts and exposure time (blocks quoted at that spread).
// Periodically, the engine calls calibrate() to fit the exponential decay
// and produce an updated κ estimate.
//
// Thread safety: thread-safe via std::shared_mutex (T2-02).
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets, audit-friendly numeric parameters.
//   ISO/IEC 5055       -- bounds-checked, no undefined behaviour.

#ifndef XOP_STRATEGY_KAPPA_CALIBRATOR_HPP
#define XOP_STRATEGY_KAPPA_CALIBRATOR_HPP

#include "xop/types.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <shared_mutex>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// KappaCalibrator configuration.
// ---------------------------------------------------------------------------

struct KappaCalibratorConfig {
    /// Number of spread buckets for binning fill observations.
    /// Spreads are bucketed into [0, bucket_width_bps), [bucket_width_bps,
    /// 2*bucket_width_bps), etc.  Default 10 buckets.
    std::uint32_t num_buckets{10};

    /// Width of each spread bucket in basis points.  Default 10 bps.
    double bucket_width_bps{10.0};

    /// Minimum number of total fills required before calibration produces
    /// a non-default κ.  Default 20 fills.
    std::uint32_t min_fills_for_calibration{20};

    /// Exponential decay weight for aging old observations.
    /// Applied every calibrate() call.  0.95 => half-life ~14 calls.
    double decay_factor{0.95};

    /// Default κ to return when insufficient data is available.
    double default_kappa{1.5};

    /// Bounds for the fitted κ (prevents runaway estimates).
    double kappa_min{0.1};
    double kappa_max{10.0};

    /// How often (in blocks) the engine should call calibrate().
    /// Default 50 blocks (~43 min).
    std::uint32_t calibration_interval_blocks{50};
};

// ---------------------------------------------------------------------------
// KappaCalibrator
// ---------------------------------------------------------------------------

class KappaCalibrator {
public:
    explicit KappaCalibrator(const KappaCalibratorConfig& cfg);

    /// Record that a fill occurred at the given half-spread (bps).
    /// Called by the engine in step_process_fills for each detected fill.
    void record_fill(double half_spread_bps);

    /// Record that we quoted at the given half-spread for one block
    /// without being filled.  Called every heartbeat for each active tier.
    void record_exposure(double half_spread_bps);

    /// Run the exponential-decay fit and return the calibrated κ.
    /// Applies the decay_factor to all buckets (aging).
    /// Returns default_kappa if insufficient data.
    double calibrate();

    /// Get the most recent calibrated κ (or default_kappa if never calibrated).
    double current_kappa() const noexcept;

    /// Total number of fills recorded across all buckets.
    std::uint32_t total_fills() const noexcept;

    /// Read-only access to the config.
    const KappaCalibratorConfig& config() const noexcept;

private:
    /// Map a half-spread (bps) to a bucket index (clamped).
    std::size_t bucket_index(double half_spread_bps) const noexcept;

    mutable std::shared_mutex mtx_;

    KappaCalibratorConfig cfg_;

    /// Per-bucket fill counts (decayed).
    std::vector<double> bucket_fills_;

    /// Per-bucket exposure counts (blocks quoted; decayed).
    std::vector<double> bucket_exposure_;

    /// Latest calibrated kappa.
    double kappa_;
};

}  // namespace xop

#endif  // XOP_STRATEGY_KAPPA_CALIBRATOR_HPP
