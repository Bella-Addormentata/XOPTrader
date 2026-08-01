// test_exposure_gate.cpp -- Unit tests for the Step 8 side-suppression
// decision (xop/execution/exposure_gate.hpp).
//
// Added 2026-08-01 with the pending-change gating fix.  The old Step 8
// Gate 1 suppressed a side on ANY nonzero pending_change, which created a
// self-sustaining bid-only loop: a bid fill buys base -> pending change on
// the base wallet -> ask side suppressed -> only bids post -> the next bid
// fill refreshes the pending change (measured 10 bids / 0 asks overnight
// on XCH/BYC).  The replacement decision suppresses a side only when the
// wallet's SPENDABLE balance genuinely cannot cover the side's committed
// exposure (live offers + planned ladder) plus the configured reserve.
// These tests lock in that decision.

#include <gtest/gtest.h>

#include <xop/execution/exposure_gate.hpp>
#include <xop/types.hpp>

#include <limits>

namespace {

using xop::Mojo;
using xop::execution::exposure_breaches_reserve;
using xop::execution::projected_balance_after_fills;

constexpr Mojo kMojo(long long v) { return static_cast<Mojo>(v); }

// ---------------------------------------------------------------------------
// The regression scenario: pending change exists (a bid fill just bought
// base), but spendable comfortably covers the planned ask ladder plus the
// reserve.  The side must NOT be suppressed -- pending change is not an
// input to the decision at all.
// ---------------------------------------------------------------------------
TEST(ExposureGate, AmpleSpendableIsNotSuppressedRegardlessOfPendingChange) {
    // 30 XCH spendable, 5 XCH already committed in live asks,
    // 5 XCH planned new ladder, 10 XCH reserve -> 20 XCH left >= reserve.
    EXPECT_FALSE(exposure_breaches_reserve(
        kMojo(30'000'000'000'000), kMojo(5'000'000'000'000),
        kMojo(5'000'000'000'000), kMojo(10'000'000'000'000)));
}

// ---------------------------------------------------------------------------
// Genuine shortfall: spendable cannot fund ladder + reserve -> suppress.
// ---------------------------------------------------------------------------
TEST(ExposureGate, SuppressesWhenLadderPlusReserveExceedsSpendable) {
    // 10 spendable, 4 live + 3 planned = 7 committed -> 3 left < 5 reserve.
    EXPECT_TRUE(exposure_breaches_reserve(
        kMojo(10), kMojo(4), kMojo(3), kMojo(5)));

    // Committed alone exceeds spendable -> projected 0 < any positive reserve.
    EXPECT_TRUE(exposure_breaches_reserve(
        kMojo(10), kMojo(12), kMojo(0), kMojo(1)));
}

// ---------------------------------------------------------------------------
// Boundary: projected balance exactly equal to the reserve is allowed
// (mirrors the strict `<` the engine used at both projection sites).
// ---------------------------------------------------------------------------
TEST(ExposureGate, ExactReserveBoundaryIsNotSuppressed) {
    EXPECT_FALSE(exposure_breaches_reserve(
        kMojo(10), kMojo(3), kMojo(2), kMojo(5)));  // 10-5 == 5 reserve
    EXPECT_TRUE(exposure_breaches_reserve(
        kMojo(10), kMojo(3), kMojo(3), kMojo(5)));  // 10-6 == 4 < 5
}

// ---------------------------------------------------------------------------
// Zero reserve: only a total wipe-out below zero projects a breach; with
// reserve 0 nothing is ever below it.
// ---------------------------------------------------------------------------
TEST(ExposureGate, ZeroReserveNeverBreaches) {
    EXPECT_FALSE(exposure_breaches_reserve(kMojo(1), kMojo(100), kMojo(100),
                                           kMojo(0)));
    EXPECT_FALSE(exposure_breaches_reserve(kMojo(0), kMojo(0), kMojo(0),
                                           kMojo(0)));
}

// ---------------------------------------------------------------------------
// Empty side: no live offers and no planned ladder -- suppress only when
// the balance is already below reserve.
// ---------------------------------------------------------------------------
TEST(ExposureGate, NoExposureFallsBackToPlainReserveCheck) {
    EXPECT_FALSE(exposure_breaches_reserve(kMojo(6), kMojo(0), kMojo(0),
                                           kMojo(5)));
    EXPECT_TRUE(exposure_breaches_reserve(kMojo(4), kMojo(0), kMojo(0),
                                          kMojo(5)));
}

// ---------------------------------------------------------------------------
// Projected balance helper: clamping and saturation.
// ---------------------------------------------------------------------------
TEST(ExposureGate, ProjectedBalanceClampsAndSaturates) {
    // Simple subtraction.
    EXPECT_EQ(projected_balance_after_fills(kMojo(10), kMojo(3), kMojo(2)),
              kMojo(5));

    // Over-committed clamps at zero, never negative.
    EXPECT_EQ(projected_balance_after_fills(kMojo(10), kMojo(20), kMojo(20)),
              kMojo(0));

    // Negative inputs (defensive) are treated as zero.
    EXPECT_EQ(projected_balance_after_fills(kMojo(-5), kMojo(-1), kMojo(-1)),
              kMojo(0));
    EXPECT_EQ(projected_balance_after_fills(kMojo(10), kMojo(-1), kMojo(4)),
              kMojo(6));

    // Committed sum saturates instead of overflowing int64.
    constexpr Mojo big = std::numeric_limits<Mojo>::max() - 1;
    EXPECT_EQ(projected_balance_after_fills(kMojo(100), big, big), kMojo(0));
    EXPECT_TRUE(exposure_breaches_reserve(kMojo(100), big, big, kMojo(1)));
}

// ---------------------------------------------------------------------------
// Negative reserve (defensive) behaves as reserve 0.
// ---------------------------------------------------------------------------
TEST(ExposureGate, NegativeReserveTreatedAsZero) {
    EXPECT_FALSE(exposure_breaches_reserve(kMojo(0), kMojo(10), kMojo(10),
                                           kMojo(-7)));
}

}  // namespace
