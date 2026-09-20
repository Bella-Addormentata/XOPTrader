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
#include <vector>

namespace {

using xop::Mojo;
using xop::execution::decide_exposure;
using xop::execution::exposure_breaches_reserve;
using xop::execution::exposure_cancel_candidate;
using xop::execution::exposure_cancel_floor;
using xop::execution::ExposureInputs;
using xop::execution::ExposureVerdict;
using xop::execution::resting_spend_on_asset;
using xop::execution::RestingSpend;
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

// ===========================================================================
// [S71 2026-09-20] exposure_rule: one verdict for both Step 8 sites.
//
// The numbers below are the XCH/DBX ask loop of 2026-09-13 00:35-00:39 as
// engine.log recorded it (mojos; 1 XCH = 1e12): reserve 0.1 XCH, five 1-XCH
// asks resting, one more approved and posted, and the wallet funding it from
// a 1.2079 XCH coin that it then locks WHOLE.
// ===========================================================================

constexpr Mojo kXch             = 1'000'000'000'000LL;
constexpr Mojo kReserve         =   100'000'000'000LL;   // 0.1 XCH
constexpr Mojo kSpendableAfter  = 6'039'753'819'181LL;   // logged 00:36:47
constexpr Mojo kLockedCoin      = 1'207'936'840'697LL;   // the pending_change
constexpr Mojo kSpendableBefore = kSpendableAfter + kLockedCoin;   // 7.2477
constexpr Mojo kRestingBefore   = 5 * kXch;
constexpr Mojo kNewAsk          = 1 * kXch;
// What the wallet OWNED throughout: Step 7 logged "confirmed-minus-reserve
// 24.069719 XCH" at 00:36:13, and the fee reserve it subtracts is 0.5 XCH.
// (Confirmed balance; unconfirmed_wallet_balance differs from it only by the
// in-flight cancel fee of 20,030 mojos.)
constexpr Mojo kOwned           = 24'569'719'000'000LL;

ExposureInputs legacy_inputs(Mojo spendable, Mojo pending, Mojo planned) {
    ExposureInputs in;
    in.spendable_mojos     = spendable;
    in.resting_spend_mojos = pending;
    in.planned_spend_mojos = planned;
    in.reserve_mojos       = kReserve;
    return in;
}

ExposureInputs unified_inputs(Mojo owned, Mojo spendable, Mojo resting,
                              Mojo planned) {
    ExposureInputs in = legacy_inputs(spendable, resting, planned);
    in.owned_mojos = owned;
    return in;
}

// -- the recorded disagreement, in the legacy rule ---------------------------

TEST(ExposureRule, LegacyApprovesThePostThenCondemnsItOnceTheCoinIsLocked) {
    // BEFORE the post: spendable 7.2477, pending 5 + new 1 -> 1.2477 left.
    const auto pre = decide_exposure(
        /*unified=*/false,
        legacy_inputs(kSpendableBefore, kRestingBefore, kNewAsk), 0.25);
    EXPECT_EQ(pre.verdict, ExposureVerdict::Ok);

    // AFTER: nothing changed but the coin lock.  The same six XCH of asks now
    // project to 0.0398 XCH, and the rule cancels what it approved 24 s ago.
    const auto post = decide_exposure(
        /*unified=*/false,
        legacy_inputs(kSpendableAfter, kRestingBefore + kNewAsk, 0), 0.25);
    EXPECT_EQ(post.verdict, ExposureVerdict::CancelResting);
    EXPECT_EQ(post.projected_mojos, kSpendableAfter - 6 * kXch);
    EXPECT_EQ(post.need_to_free, kReserve - (kSpendableAfter - 6 * kXch));
}

TEST(ExposureRule, UnifiedGivesTheSameAnswerBeforeAndAfterTheLock) {
    const auto pre = decide_exposure(
        /*unified=*/true,
        unified_inputs(kOwned, kSpendableBefore, kRestingBefore, kNewAsk), 0.25);
    EXPECT_EQ(pre.verdict, ExposureVerdict::Ok);

    // The post moved 1 XCH from `planned` to `resting` and locked a coin.
    // owned did not move, so neither does the projection: 18.569 XCH.
    const auto post = decide_exposure(
        /*unified=*/true,
        unified_inputs(kOwned, kSpendableAfter, kRestingBefore + kNewAsk, 0),
        0.25);
    EXPECT_EQ(post.verdict, ExposureVerdict::Ok);
    EXPECT_EQ(post.projected_mojos, pre.projected_mojos);
    EXPECT_EQ(post.projected_mojos, kOwned - 6 * kXch);
}

TEST(ExposureRule, UnifiedIsBlindToPendingChangeToo) {
    // The other half of the loop: the cancel's change is in flight, so
    // spendable is the low figure while NOTHING is resting that was not
    // before.  Legacy suppressed the ask here (logged 00:35:09); unified does
    // not, because the change is still owned.
    EXPECT_EQ(decide_exposure(false,
                              legacy_inputs(kSpendableAfter, kRestingBefore,
                                            kNewAsk), 0.25).verdict,
              ExposureVerdict::SuppressNew);
    EXPECT_EQ(decide_exposure(true,
                              unified_inputs(kOwned, kSpendableAfter,
                                             kRestingBefore, kNewAsk),
                              0.25).verdict,
              ExposureVerdict::Ok);
}

// -- legacy is a true rollback ------------------------------------------------

TEST(ExposureRule, LegacyMatchesTheOldPredicateOnEveryInput) {
    // Both old call sites were exposure_breaches_reserve(spendable,
    // pending [+ new], 0, reserve).  Any non-Ok legacy verdict must coincide
    // with it exactly, including at the boundary and with hostile inputs.
    const Mojo vals[] = {kMojo(-5), kMojo(0), kMojo(1), kMojo(99), kMojo(100),
                         kMojo(101), kMojo(1000),
                         std::numeric_limits<Mojo>::max()};
    const Mojo reserves[] = {kMojo(-7), kMojo(0), kMojo(100)};
    for (const Mojo s : vals) {
        for (const Mojo p : vals) {
            for (const Mojo n : vals) {
                for (const Mojo r : reserves) {
                    ExposureInputs in;
                    in.owned_mojos         = kMojo(123456);  // must be ignored
                    in.spendable_mojos     = s;
                    in.resting_spend_mojos = p;
                    in.planned_spend_mojos = n;
                    in.reserve_mojos       = r;
                    const auto d = decide_exposure(false, in, 0.9);
                    EXPECT_EQ(d.verdict != ExposureVerdict::Ok,
                              exposure_breaches_reserve(s, p, n, r))
                        << s << ' ' << p << ' ' << n << ' ' << r;
                    EXPECT_EQ(d.projected_mojos,
                              projected_balance_after_fills(s, p, n));
                }
            }
        }
    }
}

TEST(ExposureRule, LegacyCancelsOnlyAtTheRestingSiteAndFreesToTheReserve) {
    // planned == 0 is the resting-offer site: a breach there is a cancel,
    // sized to bring the projection back to the reserve (the old
    // `reserve - projected_after_fill`).
    const auto resting = decide_exposure(
        false, legacy_inputs(kMojo(150), kMojo(100), kMojo(0)), 0.25);
    EXPECT_EQ(resting.verdict, ExposureVerdict::CancelResting);
    EXPECT_EQ(resting.need_to_free, kReserve - kMojo(50));
    // planned > 0 is the pre-post site: the same breach only suppresses.
    const auto prepost = decide_exposure(
        false, legacy_inputs(kMojo(150), kMojo(60), kMojo(40)), 0.25);
    EXPECT_EQ(prepost.verdict, ExposureVerdict::SuppressNew);
    EXPECT_EQ(prepost.need_to_free, kMojo(0));
}

// -- unified: hysteresis, funding, and what a real breach looks like ----------

TEST(ExposureRule, CancelFloorIsTheReserveScaledDownAndExactAtTheEnds) {
    EXPECT_EQ(exposure_cancel_floor(kReserve, 0.0), kReserve);
    EXPECT_EQ(exposure_cancel_floor(kReserve, 0.25), kReserve / 4 * 3);
    EXPECT_EQ(exposure_cancel_floor(kReserve, 1.0), kMojo(0));
    // Out of range and NaN fail toward the STRICTER end for a cancel: the
    // full reserve is the legacy threshold.
    EXPECT_EQ(exposure_cancel_floor(kReserve, -0.5), kReserve);
    EXPECT_EQ(exposure_cancel_floor(
                  kReserve, std::numeric_limits<double>::quiet_NaN()),
              kReserve);
    EXPECT_EQ(exposure_cancel_floor(kReserve, 7.0), kMojo(0));
    EXPECT_EQ(exposure_cancel_floor(kMojo(0), 0.25), kMojo(0));
    EXPECT_EQ(exposure_cancel_floor(kMojo(-9), 0.25), kMojo(0));
}

TEST(ExposureRule, CancelFloorNeverCastsOutOfRange) {
    // [review #164] (double)INT64_MAX rounds UP to exactly 2^63, and
    // 1.0 - 1e-20 rounds to 1.0, so the product is 2^63 -- which no Mojo
    // holds, and the unchecked cast is undefined behaviour.  The header also
    // pins this with a static_assert, so a compiler that evaluates it refuses
    // to BUILD the defect rather than run it.
    const Mojo big = std::numeric_limits<Mojo>::max();
    EXPECT_EQ(exposure_cancel_floor(big, 1e-20), big);
    EXPECT_EQ(exposure_cancel_floor(big, std::numeric_limits<double>::denorm_min()),
              big);
    EXPECT_EQ(exposure_cancel_floor(big - 1, 1e-20), big - 1);
    // A real hysteresis on the same reserve still scales, and stays a Mojo.
    const Mojo quarter_off = exposure_cancel_floor(big, 0.25);
    EXPECT_GT(quarter_off, big / 2);
    EXPECT_LT(quarter_off, big);
    // And the verdict built on it: nothing resting, everything owned -- Ok,
    // not a cancel reasoned from a negative floor or a wrapped one.
    ExposureInputs in;
    in.owned_mojos   = big;
    in.reserve_mojos = big;
    EXPECT_EQ(decide_exposure(true, in, 1e-20).verdict, ExposureVerdict::Ok);
}

TEST(ExposureRule, UnifiedSuppressesInsideTheBandAndCancelsOnlyBelowIt) {
    // Resting alone leaves 0.09 XCH against a 0.1 reserve and a 0.075 cancel
    // floor: short of the reserve, so no new exposure -- but not worth a fee.
    const Mojo owned = 6 * kXch + 90'000'000'000LL;
    auto in = unified_inputs(owned, owned, 6 * kXch, 0);
    EXPECT_EQ(decide_exposure(true, in, 0.25).verdict,
              ExposureVerdict::SuppressNew);

    // 0.074 XCH left: below the floor.  Free enough to regain the FULL
    // reserve, not merely the floor.
    in.owned_mojos = 6 * kXch + 74'000'000'000LL;
    const auto d = decide_exposure(true, in, 0.25);
    EXPECT_EQ(d.verdict, ExposureVerdict::CancelResting);
    EXPECT_EQ(d.need_to_free, kReserve - 74'000'000'000LL);

    // hysteresis 1.0 is "suppress only": even a projection of zero.
    in.owned_mojos = kMojo(0);
    EXPECT_EQ(decide_exposure(true, in, 1.0).verdict,
              ExposureVerdict::SuppressNew);
    // hysteresis 0 cancels at the reserve itself.
    in.owned_mojos = 6 * kXch + 99'999'999'999LL;
    EXPECT_EQ(decide_exposure(true, in, 0.0).verdict,
              ExposureVerdict::CancelResting);
}

TEST(ExposureRule, UnifiedRestingBreachOutranksThePlannedTiers) {
    // The resting book alone is a breach: the verdict is CancelResting with
    // or without planned tiers, and the pre-post site reads any non-Ok
    // verdict as "suppress".
    const auto in = unified_inputs(5 * kXch, 5 * kXch, 6 * kXch, kNewAsk);
    const auto d = decide_exposure(true, in, 0.25);
    EXPECT_EQ(d.verdict, ExposureVerdict::CancelResting);
    EXPECT_EQ(d.need_to_free, kReserve);   // projection clamps at 0
}

TEST(ExposureRule, UnifiedStillRefusesTiersTheFreeCoinsCannotFund) {
    // Plenty owned, almost nothing free: everything is locked in offers.
    // The projection is fine; the wallet simply cannot build the offer.
    auto in = unified_inputs(kOwned, kNewAsk + kReserve - 1, kRestingBefore,
                             kNewAsk);
    EXPECT_EQ(decide_exposure(true, in, 0.25).verdict,
              ExposureVerdict::SuppressNew);
    in.spendable_mojos = kNewAsk + kReserve;
    EXPECT_EQ(decide_exposure(true, in, 0.25).verdict, ExposureVerdict::Ok);
    // With nothing planned, spendable is not an input at all.
    in.spendable_mojos     = kMojo(0);
    in.planned_spend_mojos = kMojo(0);
    EXPECT_EQ(decide_exposure(true, in, 0.25).verdict, ExposureVerdict::Ok);
}

TEST(ExposureRule, UnifiedARealOutflowIsStillCaught) {
    // A fill on ANOTHER pair (or a take we made) really did reduce what is
    // owned.  That is the change the rule exists for.
    const auto in = unified_inputs(6 * kXch + 10'000'000'000LL, kMojo(0),
                                   6 * kXch, 0);
    EXPECT_EQ(decide_exposure(true, in, 0.25).verdict,
              ExposureVerdict::CancelResting);
}

// -- unified: the asset-wide resting sum ---------------------------------------

TEST(ExposureRule, RestingSpendSumsEveryPairThatSpendsTheAsset) {
    const std::vector<RestingSpend> offers = {
        {"xch", 1 * kXch, false},      // XCH/DBX ask
        {"xch", 2 * kXch, false},      // XCH/BYC ask
        {"dbx", 86'000, false},        // XCH/DBX bid spends DBX, not XCH
        {"xch", 4 * kXch, true},       // a cancel already sent: not exposure
        {"xch", kMojo(-5), false},     // hostile
        {"xch", kMojo(0), false},
    };
    EXPECT_EQ(resting_spend_on_asset(offers, "xch"), 3 * kXch);
    EXPECT_EQ(resting_spend_on_asset(offers, "dbx"), kMojo(86'000));
    EXPECT_EQ(resting_spend_on_asset(offers, "byc"), kMojo(0));
    EXPECT_EQ(resting_spend_on_asset({}, "xch"), kMojo(0));
}

TEST(ExposureRule, RestingSpendSaturatesInsteadOfOverflowing) {
    const Mojo big = std::numeric_limits<Mojo>::max();
    const std::vector<RestingSpend> offers = {
        {"xch", big, false}, {"xch", big, false}, {"xch", 1, false}};
    EXPECT_EQ(resting_spend_on_asset(offers, "xch"), big);
}

// -- unified: the minimum-age gate on candidates -------------------------------

TEST(ExposureRule, LegacyTakesEveryCandidateUnifiedSparesTheYoung) {
    // The 528 exposure cancels averaged 27 blocks old; the loop cancelled
    // offers one or two blocks after posting them.
    EXPECT_TRUE(exposure_cancel_candidate(false, 1000, 1001, 32));
    EXPECT_FALSE(exposure_cancel_candidate(true, 1000, 1001, 32));
    EXPECT_FALSE(exposure_cancel_candidate(true, 1000, 1031, 32));
    EXPECT_TRUE(exposure_cancel_candidate(true, 1000, 1032, 32));
    // min age 0 spares nothing; a created block in the future (a reorg, an
    // adopted offer) is never old enough rather than wrapping to "ancient".
    EXPECT_TRUE(exposure_cancel_candidate(true, 1000, 1000, 0));
    EXPECT_FALSE(exposure_cancel_candidate(true, 1005, 1000, 0));
    EXPECT_FALSE(exposure_cancel_candidate(true, 1005, 1000, 32));
}

}  // namespace
