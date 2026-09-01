#ifndef XOP_STRATEGY_TIER_GAIN_HPP
#define XOP_STRATEGY_TIER_GAIN_HPP
// ---------------------------------------------------------------------------
// tier_gain.hpp -- how much edge a tier carries, and against what.
//
// [FEEGAIN 2026-09-01] SHADOW ONLY on arrival. Step 8's fee-to-gain gate
// still decides on the published-mid frame exactly as before; this computes
// the other frames alongside and logs only the disagreement.
//
// WHY THE REFERENCE IS THE WHOLE QUESTION
// ---------------------------------------
// Tier prices are built as centre * (1 +/- spacing_bps/1e4). Scoring them
// against a DIFFERENT centre is a frame error, and Step 7 produces three
// candidate centres in sequence:
//
//   (1) the PUBLISHED MID          -- what the gate uses today
//   (2) the FAIR-VALUE CENTRE      -- after the peg anchor and the
//                                     fair-value blend, BEFORE the A-S shift
//   (3) the POST-A-S CENTRE        -- (2) displaced to manage inventory,
//                                     persisted as pcs.quote_mid_mojos
//
// (3) is wrong for the reason the gate's own comment gives: tier price is
// C'(1 +/- s), so |P - C'|/C' is identically s on BOTH sides. The A-S shift
// deliberately SPENDS edge on the inventory-reducing side -- true edge is
// (s - r) on asks and (s + r) on bids when the centre shifts down by r --
// and measuring from C' erases that cost exactly where it lands. It also
// blinds the gate to r > s, where an inner tier is genuinely priced through
// fair value.
//
// (1) is wrong on DIRECTION, which is the part the comment misses. This gate
// only ever DROPS tiers, so a reference that is farther from the ladder is
// more PERMISSIVE, not more conservative. The published mid is systematically
// farther, because the ladder is not centred on it. That is not robustness;
// it is noise in a filter. Measured on XCH/DBX: the frame error |(1)-(2)| is
// mean 143.2 bps and max 959 bps, while the A-S skew |(2)-(3)| the comment
// warns about is mean 11.8 bps and max 29.9 bps -- the comment names the
// small term correctly and then admits one about twelve times larger.
//
// So (2) is the reference, and it is why quote_fair_centre_mojos exists.
//
// THE SIGN TRAP -- READ THIS BEFORE "TIDYING" THE CALLER
// ------------------------------------------------------
// The live gate takes std::abs of the price difference, which makes its own
// std::max(0.0, ...) dead code and hands a wrong-side tier (an ask priced
// BELOW fair value) full credit for its distance. The obvious cleanup is to
// make the edge signed.
//
// DO NOT DO THAT ALONE. Under frame (1), bid tiers sit ABOVE the published
// mid whenever the centre shift exceeds the tier spacing -- measured on 81.4%
// of cycles -- so a signed edge against the published mid clamps to zero and
// drops very nearly every bid. The sign fix is only correct once the frame is
// also correct. They ship together or not at all, which is precisely why this
// header computes both and the caller changes neither yet.
//
// Pure header, no engine types, so every frame is driven directly by
// cpp/tests/test_tier_gain.cpp rather than through an Engine -- nothing in
// cpp/tests constructs one, which is how a regression in this same Step 8
// family survived four review rounds.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xop::strategy {

struct TierGain {
    /// false => the caller has no usable reference and must SKIP the gate
    /// rather than score the tier against nothing.
    bool          usable{false};
    /// SIGNED edge as a fraction of the centre: (P - C)/C for an ask,
    /// (C - P)/C for a bid. Negative means the tier is priced THROUGH fair
    /// value -- it carries no edge at all, which is a fact the absolute
    /// value hides.
    double        edge_fraction{0.0};
    /// Expected gain in XCH-equivalent mojos; 0 whenever edge_fraction <= 0.
    std::uint64_t expected_gain_mojos{0};
};

/// @param tier_price          tier price, mojo-scaled.
/// @param tier_size           tier size in BASE-asset mojos.
/// @param is_ask              side of the tier.
/// @param centre              reference centre, mojo-scaled. Which centre is
///                            the caller's decision and the whole question.
/// @param base_mojos_per_unit base-asset mojos per whole unit (1e12 for XCH,
///                            1e3 for a typical CAT). Scales the gain into
///                            XCH-equivalent mojos so it is comparable with
///                            an XCH-denominated fee.
[[nodiscard]] inline TierGain tier_expected_gain(
    double        tier_price,
    double        tier_size,
    bool          is_ask,
    double        centre,
    double        base_mojos_per_unit) noexcept
{
    TierGain g{};
    if (!(centre > 0.0) || !std::isfinite(centre)
        || !(tier_price > 0.0) || !std::isfinite(tier_price)
        || !(tier_size > 0.0) || !std::isfinite(tier_size)
        || !(base_mojos_per_unit > 0.0)
        || !std::isfinite(base_mojos_per_unit)) {
        return g;   // usable == false: skip, do not score against nothing
    }
    g.usable = true;
    // SIGNED, deliberately. An ask below the centre or a bid above it has
    // negative edge; it is not "distance from fair value worth paying a fee
    // to capture", it is a quote that gives edge away.
    g.edge_fraction = is_ask ? (tier_price - centre) / centre
                             : (centre - tier_price) / centre;
    if (!(g.edge_fraction > 0.0)) {
        return g;   // usable, but worth nothing
    }
    const double mojos = g.edge_fraction * tier_size
                       * 1'000'000'000'000.0 / base_mojos_per_unit;
    if (!std::isfinite(mojos) || mojos <= 0.0) {
        return g;
    }
    // Clamp rather than wrap: the product is a fraction times a mojo size
    // times a scale factor, and neither operand alone bounds it.
    //
    // 2^63, NOT a value near UINT64_MAX. [caught by CI on GCC, 2026-09-01]
    // An earlier revision clamped at 18'446'744'073'709'551'000.0, reasoning
    // that it sits below UINT64_MAX. It does as an integer; it does not as a
    // DOUBLE. Doubles are spaced 2048 apart up there, so that literal rounds
    // to exactly 2^64 = 18'446'744'073'709'551'616 -- one greater than
    // UINT64_MAX -- and the cast is undefined. MSVC happened to yield a
    // large value and the test passed locally; GCC yielded 0, which is the
    // precise failure the clamp exists to prevent, introduced by the clamp.
    //
    // 2^63 is exactly representable, comfortably below UINT64_MAX, and far
    // beyond any real gain (a fill worth 9.2 billion XCH). Saturating there
    // is indistinguishable from saturating higher for every caller.
    constexpr double kMaxSafeGain = 9'223'372'036'854'775'808.0;  // 2^63
    g.expected_gain_mojos = static_cast<std::uint64_t>(
        std::min(mojos, kMaxSafeGain));
    return g;
}

/// The live gate's arithmetic, preserved verbatim so the shadow measures a
/// real disagreement rather than an artifact of two nearly-similar formulas.
/// Note the absolute value and the consequently-dead max().
[[nodiscard]] inline TierGain tier_expected_gain_legacy(
    double tier_price,
    double tier_size,
    double published_mid,
    double base_mojos_per_unit) noexcept
{
    TierGain g{};
    if (!(published_mid > 0.0) || !std::isfinite(published_mid)
        || !(base_mojos_per_unit > 0.0)
        || !std::isfinite(base_mojos_per_unit)
        || !std::isfinite(tier_price) || !std::isfinite(tier_size)) {
        return g;
    }
    g.usable = true;
    g.edge_fraction = std::abs(tier_price - published_mid) / published_mid;
    const double mojos = g.edge_fraction * tier_size
                       * 1'000'000'000'000.0 / base_mojos_per_unit;
    g.expected_gain_mojos = static_cast<std::uint64_t>(
        std::max(0.0, std::isfinite(mojos) ? mojos : 0.0));
    return g;
}

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_TIER_GAIN_HPP
