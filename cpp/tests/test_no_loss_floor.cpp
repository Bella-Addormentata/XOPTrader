// ---------------------------------------------------------------------------
// [FLOOR] The ask-floor mode dial -- realizing losses is a policy choice,
// not an accident. Strict never quotes below basis+margin; aging decays
// the floor on the operator's schedule (max_loss_relax_bps finally does
// something); off cedes pricing to the market.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/strategy/no_loss_floor.hpp"

using xop::strategy::FloorMode;
using xop::strategy::compute_ask_floor;

namespace {
constexpr std::int64_t kBasis = 11'680'000'000'000;  // the live BYC case
constexpr double kMargin = 50.0;                     // bps
constexpr std::uint32_t kStart = 500;                // aging_start_blocks
constexpr double kRate = 0.05;                       // bps per block
constexpr double kMaxRelax = 25.0;                   // bps
}  // namespace

TEST(NoLossFloor, off_means_no_floor)
{
    EXPECT_EQ(compute_ask_floor(FloorMode::Off, kBasis, kMargin,
                                10'000, kStart, kRate, kMaxRelax), 0);
}

TEST(NoLossFloor, strict_is_basis_plus_margin_regardless_of_age)
{
    const auto f = compute_ask_floor(FloorMode::Strict, kBasis, kMargin,
                                     1'000'000, kStart, kRate, kMaxRelax);
    EXPECT_EQ(f, static_cast<std::int64_t>(
        std::llround(kBasis * (1.0 + kMargin / 10'000.0))));
}

TEST(NoLossFloor, aging_matches_strict_before_the_start_block)
{
    const auto young = compute_ask_floor(FloorMode::Aging, kBasis, kMargin,
                                         499, kStart, kRate, kMaxRelax);
    const auto strict = compute_ask_floor(FloorMode::Strict, kBasis,
                                          kMargin, 499, kStart, kRate,
                                          kMaxRelax);
    EXPECT_EQ(young, strict);
}

TEST(NoLossFloor, aging_decays_linearly_and_caps_at_max_relax)
{
    // 700 blocks old: 200 past start * 0.05 = 10 bps discount.
    const auto mid_age = compute_ask_floor(FloorMode::Aging, kBasis,
                                           kMargin, 700, kStart, kRate,
                                           kMaxRelax);
    EXPECT_EQ(mid_age, static_cast<std::int64_t>(
        std::llround(kBasis * (1.0 + (kMargin - 10.0) / 10'000.0))));

    // Very old: discount capped at 25 bps -> floor = basis + 25 bps.
    const auto old_age = compute_ask_floor(FloorMode::Aging, kBasis,
                                           kMargin, 1'000'000, kStart,
                                           kRate, kMaxRelax);
    EXPECT_EQ(old_age, static_cast<std::int64_t>(
        std::llround(kBasis * (1.0 + (kMargin - kMaxRelax) / 10'000.0))));
}

TEST(NoLossFloor, aging_effective_margin_never_below_negative_max_relax)
{
    // Margin 10, discount capped at 200: effective = 10 - 200 = -190,
    // and the -max_loss_relax clamp does NOT bite (-190 > -200). The
    // contract bounds the LOSS, not margin-minus-discount: "never accept
    // a loss larger than the configured cap" (config.hpp).
    const auto f = compute_ask_floor(FloorMode::Aging, kBasis, 10.0,
                                     1'000'000, kStart, 10.0, 200.0);
    EXPECT_EQ(f, static_cast<std::int64_t>(
        std::llround(kBasis * (1.0 - 190.0 / 10'000.0))));

    // And when margin - discount WOULD undershoot the cap (margin 10,
    // discount 500 via a huge rate against max 500): clamp at -500? No:
    // discount itself caps at max_loss_relax (500), effective = -490,
    // still above -500. The clamp is unreachable by construction unless
    // margin is negative -- pin that the guard holds anyway.
    const auto g = compute_ask_floor(FloorMode::Aging, kBasis, -100.0,
                                     1'000'000, kStart, 10.0, 200.0);
    EXPECT_EQ(g, static_cast<std::int64_t>(
        std::llround(kBasis * (1.0 - 200.0 / 10'000.0))));
}

TEST(NoLossFloor, unknown_basis_is_no_floor_in_every_mode)
{
    for (auto mode : {FloorMode::Strict, FloorMode::Aging, FloorMode::Off}) {
        EXPECT_EQ(compute_ask_floor(mode, 0, kMargin, 1000, kStart, kRate,
                                    kMaxRelax), 0);
        EXPECT_EQ(compute_ask_floor(mode, -5, kMargin, 1000, kStart, kRate,
                                    kMaxRelax), 0);
    }
}
