#ifndef XOP_STRATEGY_SYNTHETIC_PRICE_HPP
#define XOP_STRATEGY_SYNTHETIC_PRICE_HPP
// ---------------------------------------------------------------------------
// synthetic_price.hpp -- [SYNTH 2026-10-22] a quoting price for a
// revive_market pair that has no external feed, built from whatever evidence
// exists, with an honest error bar.
//
// WHY. revive_market used to need a CoinGecko listing for both legs. BYC has
// none, so neither BYC pair could be revived, and a pair with no listing and
// a thin book had no price at all. This builds one from:
//
//   * per-asset XCH prices -- each CAT's price in XCH from its CoinGecko
//     listing (when it has one) and/or from Dexie's per-trade history of its
//     <asset>_xch market (tape_source), carried forward with a growing error
//     bar when the evidence goes quiet (aged_source); a pair's price is the
//     cross of its two legs (cross_source);
//   * the engine's fair-value estimate for the pair, which already includes
//     cross-pair books and declared pars;
//
// blended in log space by inverse variance (blend_sources). When the sources
// DISAGREE the blended error bar widens to their spread, so conflicting
// evidence (par says $1, the tape says $0.78) quotes wide instead of
// confidently in the middle.
//
// WHAT IT IS NOT. docs/price-discovery-from-trade-history.md (2026-09-01)
// refused the trade tape as a fair-value input, for reasons that still hold
// for VALUATION: a polled ticker cannot bound a print's age, and a tape-fed
// graph edge would displace the governed par fallback with a smaller error
// bar. So this price never enters the fair-value solve, valuation or the
// mid gate's anchors (MarketDataFeed marks it synthetic and withholds
// valuation grade). It only lets a revive pair QUOTE, and it answers the
// note's two objections for that use: Dexie's v2 trade history carries a
// per-trade timestamp, so age is measured, not inferred; and its error bar
// is never smaller than the disagreement between sources.
//
// Pure header: no engine, feed or RPC types.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace xop::synth {

/// A price and its one-sigma uncertainty, in basis points.
struct PriceSource {
    double price{0.0};
    double sigma_bps{0.0};
};

/// True when `s` is usable: positive finite price, finite sigma >= 0.
[[nodiscard]] inline bool usable(const PriceSource& s) noexcept
{
    return s.price > 0.0 && std::isfinite(s.price)
        && s.sigma_bps >= 0.0 && std::isfinite(s.sigma_bps);
}

/// The largest error bar any synthetic price may report. A source that
/// would exceed it is clipped to it rather than dropped, so a stale price
/// still quotes -- wide.
inline constexpr double kMaxSigmaBps = 5000.0;

/// How fast a remembered synthetic price's error bar grows, per hour, once
/// no fresh evidence supports it (aged_source's usual rate).
inline constexpr double kAgedSigmaBpsPerHour = 25.0;

/// Inverse-variance blend in log space. The reported sigma is the larger of
/// the formal combined error, sqrt(1 / sum w), and the weighted spread of the
/// sources around the result, so two confident sources that disagree do not
/// produce a confident answer. A source with sigma 0 is treated as
/// `min_sigma_bps`. nullopt when no source is usable.
[[nodiscard]] inline std::optional<PriceSource> blend_sources(
    const std::vector<PriceSource>& sources, double min_sigma_bps = 50.0)
{
    double sum_w = 0.0, sum_wx = 0.0;
    for (const auto& s : sources) {
        if (!usable(s)) continue;
        const double sig = std::max(s.sigma_bps, min_sigma_bps) / 1e4;
        const double w = 1.0 / (sig * sig);
        sum_w  += w;
        sum_wx += w * std::log(s.price);
    }
    if (!(sum_w > 0.0)) return std::nullopt;
    const double centre = sum_wx / sum_w;
    double sum_wd = 0.0;
    for (const auto& s : sources) {
        if (!usable(s)) continue;
        const double sig = std::max(s.sigma_bps, min_sigma_bps) / 1e4;
        const double d = std::log(s.price) - centre;
        sum_wd += (1.0 / (sig * sig)) * d * d;
    }
    const double formal = std::sqrt(1.0 / sum_w) * 1e4;
    const double spread = std::sqrt(sum_wd / sum_w) * 1e4;
    PriceSource out;
    out.price     = std::exp(centre);
    out.sigma_bps = std::min(kMaxSigmaBps, std::max(formal, spread));
    return out;
}

/// The price of `base` in `quote` from each one's price in a common unit
/// (XCH): base / quote, errors added in quadrature. nullopt if either leg is
/// unusable.
[[nodiscard]] inline std::optional<PriceSource> cross_source(
    const PriceSource& base_in_xch, const PriceSource& quote_in_xch)
{
    if (!usable(base_in_xch) || !usable(quote_in_xch)) return std::nullopt;
    PriceSource out;
    out.price     = base_in_xch.price / quote_in_xch.price;
    out.sigma_bps = std::min(kMaxSigmaBps,
                             std::hypot(base_in_xch.sigma_bps,
                                        quote_in_xch.sigma_bps));
    if (!usable(out)) return std::nullopt;
    return out;
}

/// A remembered price `age_hours` old: its sigma grows linearly with age.
[[nodiscard]] inline PriceSource aged_source(const PriceSource& last,
                                             double age_hours,
                                             double bps_per_hour)
{
    PriceSource out = last;
    const double age = (age_hours > 0.0 && std::isfinite(age_hours)) ? age_hours : 0.0;
    out.sigma_bps = std::min(kMaxSigmaBps, last.sigma_bps + age * bps_per_hour);
    return out;
}

/// One settled trade on a <asset>_xch market: XCH per asset, the asset
/// amount, and when it settled.
struct TapeTrade {
    double price{0.0};
    double volume{0.0};
    std::chrono::system_clock::time_point at{};
};

/// A trade's epoch-milliseconds timestamp as a time point, or nullopt when
/// it is not positive or lies beyond what system_clock can represent (its
/// duration is nanoseconds on Linux, so ~year 2262 -- an int64 of
/// milliseconds reaches much further, and converting it would overflow).
[[nodiscard]] inline std::optional<std::chrono::system_clock::time_point>
time_from_epoch_ms(std::int64_t ms) noexcept
{
    using namespace std::chrono;
    constexpr std::int64_t kMaxMs =
        duration_cast<milliseconds>(system_clock::duration::max()).count();
    if (ms <= 0 || ms > kMaxMs) return std::nullopt;
    return system_clock::time_point(
        duration_cast<system_clock::duration>(milliseconds(ms)));
}

/// Parameters of tape_source. Defaults are the shipped values.
struct TapeParams {
    std::chrono::hours window{72};   ///< trades older than this are ignored
    double base_sigma_bps{200.0};    ///< floor: a thin tape is never precise
    double bps_per_hour{25.0};       ///< added per hour since the newest trade
};

/// The asset's price in XCH from its recent trades: the volume-weighted mean
/// of log prices, with a sigma of base + the trades' own weighted dispersion
/// (in quadrature) + bps_per_hour times the newest trade's age. Trades with a
/// non-positive price or volume, or outside the window, are ignored. nullopt
/// when none remain.
[[nodiscard]] inline std::optional<PriceSource> tape_source(
    const std::vector<TapeTrade>& trades,
    std::chrono::system_clock::time_point now,
    const TapeParams& p = TapeParams{})
{
    double sum_w = 0.0, sum_wx = 0.0;
    std::chrono::system_clock::time_point newest{};
    for (const auto& t : trades) {
        if (!(t.price > 0.0) || !std::isfinite(t.price)) continue;
        if (!(t.volume > 0.0) || !std::isfinite(t.volume)) continue;
        if (t.at > now || now - t.at > p.window) continue;
        sum_w  += t.volume;
        sum_wx += t.volume * std::log(t.price);
        newest = std::max(newest, t.at);
    }
    if (!(sum_w > 0.0)) return std::nullopt;
    const double centre = sum_wx / sum_w;
    double sum_wd = 0.0;
    for (const auto& t : trades) {
        if (!(t.price > 0.0) || !std::isfinite(t.price)) continue;
        if (!(t.volume > 0.0) || !std::isfinite(t.volume)) continue;
        if (t.at > now || now - t.at > p.window) continue;
        const double d = std::log(t.price) - centre;
        sum_wd += t.volume * d * d;
    }
    const double dispersion = std::sqrt(sum_wd / sum_w) * 1e4;
    const double age_h =
        std::chrono::duration<double, std::ratio<3600>>(now - newest).count();
    PriceSource out;
    out.price     = std::exp(centre);
    out.sigma_bps = std::min(kMaxSigmaBps,
                             std::hypot(p.base_sigma_bps, dispersion)
                                 + std::max(0.0, age_h) * p.bps_per_hour);
    return out;
}

/// A mark for a loss budget from a synthetic price: one error bar against
/// the position (the lower edge when long base, the upper when short; the
/// price itself when flat), so the budget can only over-count a loss. 0 when
/// the source is unusable.
[[nodiscard]] inline double conservative_mark(const PriceSource& p,
                                              double net_base) noexcept
{
    if (!usable(p)) return 0.0;
    const double frac = std::min(p.sigma_bps, kMaxSigmaBps) / 1e4;
    if (net_base > 0.0) return p.price * (1.0 - frac);
    if (net_base < 0.0) return p.price * (1.0 + frac);
    return p.price;
}

/// The UNCERTAINTY-derived part of a synthetic-priced pair's quoting
/// half-spread: the centre's own width (k_sigma times the combined centre
/// sigma) and, on a thin book, `sigma_mult` of the synthetic price's error
/// bars -- capped at `cap_bps`, since past ~50% a ladder stops meaning
/// anything and the bids approach zero. Explicit minimums (profit margin,
/// fee, the revive floor) are applied by the caller on top, uncapped: they
/// are configuration, not uncertainty.
[[nodiscard]] inline double uncertainty_width_bps(double centre_width_bps,
                                                  double synth_sigma_bps,
                                                  bool   thin,
                                                  double sigma_mult = 2.0,
                                                  double cap_bps = kMaxSigmaBps) noexcept
{
    double want = (centre_width_bps > 0.0 && std::isfinite(centre_width_bps))
        ? centre_width_bps : 0.0;
    if (thin && synth_sigma_bps > 0.0 && std::isfinite(synth_sigma_bps)) {
        want = std::max(want, sigma_mult * synth_sigma_bps);
    }
    return std::min(want, cap_bps);
}

/// Whether a remembered synthetic PAIR price may still stand in, given each
/// leg's peg suspension now and when the price was made. A leg suspended
/// since then invalidates it: the price may rest on that wrapper's listing
/// (its underlying coin) or on an estimate anchored there. A leg suspended
/// then had both excluded, so its suspension now is no reason to refuse.
[[nodiscard]] inline bool remembered_price_usable(bool base_suspended_now,
                                                  bool quote_suspended_now,
                                                  bool base_suspended_then,
                                                  bool quote_suspended_then) noexcept
{
    return (!base_suspended_now || base_suspended_then)
        && (!quote_suspended_now || quote_suspended_then);
}

/// How far from its centre a ladder may price before the engine's mojo
/// arithmetic (1e12-scaled, int64) can overflow: a synthetic price is used
/// only if `kLadderHeadroom` times it still fits, which covers the widest
/// ladder (a 50% half-spread plus any tier schedule) many times over.
inline constexpr double kLadderHeadroom = 16.0;

/// True when `price` (quote per base) leaves the ladder that headroom in
/// `scale`-scaled int64 units.
[[nodiscard]] inline bool fits_with_ladder_headroom(double price,
                                                    double scale) noexcept
{
    if (!(price > 0.0) || !std::isfinite(price)) return false;
    constexpr double kMax = 9.2233720368547748e18;
    const double scaled = price * scale;
    return scaled >= 1.0 && scaled * kLadderHeadroom < kMax;
}

}  // namespace xop::synth

#endif  // XOP_STRATEGY_SYNTHETIC_PRICE_HPP
