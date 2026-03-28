// kappa_calibrator.cpp -- Online κ calibrator implementation.
//
// Fits λ(δ) = A · exp(−κ · δ) via weighted least-squares on
// log(fill_rate) vs half-spread.  Uses exponential decay to weight
// recent observations over stale ones.
//
// ISO/IEC 27001:2022 -- no secrets, deterministic numeric computation.
// ISO/IEC 5055       -- bounds-checked, no undefined behaviour.

#include "xop/strategy/kappa_calibrator.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

namespace xop {

KappaCalibrator::KappaCalibrator(const KappaCalibratorConfig& cfg)
    : cfg_(cfg)
    , bucket_fills_(cfg.num_buckets, 0.0)
    , bucket_exposure_(cfg.num_buckets, 0.0)
    , kappa_(cfg.default_kappa)
{
}

std::size_t KappaCalibrator::bucket_index(double half_spread_bps) const noexcept
{
    if (half_spread_bps <= 0.0 || cfg_.bucket_width_bps <= 0.0) {
        return 0;
    }
    const auto idx = static_cast<std::size_t>(half_spread_bps / cfg_.bucket_width_bps);
    return std::min(idx, static_cast<std::size_t>(cfg_.num_buckets - 1));
}

void KappaCalibrator::record_fill(double half_spread_bps)
{
    std::unique_lock lock(mtx_);
    const auto idx = bucket_index(half_spread_bps);
    bucket_fills_[idx] += 1.0;
}

void KappaCalibrator::record_exposure(double half_spread_bps)
{
    std::unique_lock lock(mtx_);
    const auto idx = bucket_index(half_spread_bps);
    bucket_exposure_[idx] += 1.0;
}

double KappaCalibrator::calibrate()
{
    std::unique_lock lock(mtx_);

    // Count total fills for minimum-data check.
    const double total = std::accumulate(bucket_fills_.begin(),
                                          bucket_fills_.end(), 0.0);

    if (total < static_cast<double>(cfg_.min_fills_for_calibration)) {
        // Not enough data; apply decay and return default.
        for (std::size_t i = 0; i < cfg_.num_buckets; ++i) {
            bucket_fills_[i]    *= cfg_.decay_factor;
            bucket_exposure_[i] *= cfg_.decay_factor;
        }
        return kappa_;
    }

    // Compute fill rates per bucket and fit log(rate) = log(A) - κ·δ
    // via simple linear regression: y = a + b·x where y = log(rate),
    // x = bucket midpoint, b = -κ.
    //
    // Only include buckets with both fills > 0 and exposure > 0.
    double sum_x   = 0.0;
    double sum_y   = 0.0;
    double sum_xx  = 0.0;
    double sum_xy  = 0.0;
    double n_valid = 0.0;

    for (std::size_t i = 0; i < cfg_.num_buckets; ++i) {
        if (bucket_fills_[i] < 0.5 || bucket_exposure_[i] < 0.5) {
            continue;  // Skip empty buckets.
        }

        const double rate = bucket_fills_[i] / bucket_exposure_[i];
        if (rate <= 0.0) continue;

        // Bucket midpoint in bps.
        const double x = (static_cast<double>(i) + 0.5) * cfg_.bucket_width_bps;
        const double y = std::log(rate);

        sum_x  += x;
        sum_y  += y;
        sum_xx += x * x;
        sum_xy += x * y;
        n_valid += 1.0;
    }

    if (n_valid < 2.0) {
        // Need at least 2 points for linear regression.
        for (std::size_t i = 0; i < cfg_.num_buckets; ++i) {
            bucket_fills_[i]    *= cfg_.decay_factor;
            bucket_exposure_[i] *= cfg_.decay_factor;
        }
        return kappa_;
    }

    // Linear regression: b = (n·Σxy - Σx·Σy) / (n·Σxx - (Σx)²)
    const double denom = n_valid * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-12) {
        // Degenerate; keep current kappa.
        for (std::size_t i = 0; i < cfg_.num_buckets; ++i) {
            bucket_fills_[i]    *= cfg_.decay_factor;
            bucket_exposure_[i] *= cfg_.decay_factor;
        }
        return kappa_;
    }

    const double slope = (n_valid * sum_xy - sum_x * sum_y) / denom;
    // κ = -slope (since log(rate) = log(A) - κ·δ => slope = -κ).
    const double fitted_kappa = -slope;

    // Clamp to configured bounds.
    kappa_ = std::clamp(fitted_kappa, cfg_.kappa_min, cfg_.kappa_max);

    // Apply exponential decay to age out old observations.
    for (std::size_t i = 0; i < cfg_.num_buckets; ++i) {
        bucket_fills_[i]    *= cfg_.decay_factor;
        bucket_exposure_[i] *= cfg_.decay_factor;
    }

    return kappa_;
}

double KappaCalibrator::current_kappa() const noexcept
{
    std::shared_lock lock(mtx_);
    return kappa_;
}

std::uint32_t KappaCalibrator::total_fills() const noexcept
{
    std::shared_lock lock(mtx_);
    return static_cast<std::uint32_t>(
        std::accumulate(bucket_fills_.begin(), bucket_fills_.end(), 0.0));
}

const KappaCalibratorConfig& KappaCalibrator::config() const noexcept
{
    std::shared_lock lock(mtx_);
    return cfg_;
}

}  // namespace xop
