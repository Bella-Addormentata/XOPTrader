// ---------------------------------------------------------------------------
// test_valuation_authority -- [S20 2026-08-24] pins for the peak-authority
// state machine and the carry-TTL expiry (xop/risk/valuation_authority.hpp).
//
// These semantics are safety-critical and easy to reverse by accident: an
// off-by-one in the debounce, an inverted gate, or an unguarded unsigned
// subtraction each fail silently in a direction that only shows up as a
// spurious pause (or a missed one) hours later on live data.  Step 13 has no
// test harness of its own, which is why the logic lives in a pure header.
// ---------------------------------------------------------------------------

#include "xop/risk/valuation_authority.hpp"

#include <gtest/gtest.h>

using xop::risk::ValuationAuthorityGate;
using xop::risk::carry_expired;

// ---------------------------------------------------------------------------
// carry_expired
// ---------------------------------------------------------------------------

TEST(CarryExpiryTest, BoundaryIsExclusiveAtExactlyTheTtl) {
    // Exactly TTL blocks old is still authoritative; one more is not.
    EXPECT_FALSE(carry_expired(/*current=*/1720, /*last_live=*/1000, /*ttl=*/720));
    EXPECT_TRUE (carry_expired(/*current=*/1721, /*last_live=*/1000, /*ttl=*/720));
}

TEST(CarryExpiryTest, ZeroTtlDisablesExpiry) {
    // Documented config semantics: 0 means "never expire".
    EXPECT_FALSE(carry_expired(1'000'000, 1, /*ttl=*/0));
}

TEST(CarryExpiryTest, RegressedBlockHeightDoesNotExpire) {
    // A reorg or a node swap can serve a LOWER tip.  BlockHeight is
    // unsigned, so an unguarded (current - last) would wrap to a huge age
    // and instantly declare every held asset degraded -- freezing the peak
    // across the whole portfolio on a transient node hiccup.
    EXPECT_FALSE(carry_expired(/*current=*/900, /*last_live=*/1000, /*ttl=*/720));
    EXPECT_FALSE(carry_expired(/*current=*/0,   /*last_live=*/1000, /*ttl=*/720));
    EXPECT_FALSE(carry_expired(/*current=*/1000, /*last_live=*/1000, /*ttl=*/720));
}

// ---------------------------------------------------------------------------
// ValuationAuthorityGate
// ---------------------------------------------------------------------------

TEST(ValuationAuthorityTest, FreshProcessIsArmedImmediately) {
    // A process that has never degraded must seed its peak from the first
    // valued cycle, exactly as before S20 -- otherwise the drawdown breaker
    // would have no peak for its first ten heartbeats.
    ValuationAuthorityGate gate;
    const auto s = gate.step(/*degraded=*/false);
    EXPECT_TRUE(s.may_update_peak);
    EXPECT_FALSE(s.entered_degraded);
    EXPECT_FALSE(s.recovered);
}

TEST(ValuationAuthorityTest, DegradedCycleFreezesThePeakAtOnce) {
    ValuationAuthorityGate gate;
    const auto s = gate.step(/*degraded=*/true);
    EXPECT_FALSE(s.may_update_peak);
    EXPECT_TRUE(s.entered_degraded) << "first degraded cycle must warn";
}

TEST(ValuationAuthorityTest, RecoveryTakesExactlyTenCleanCycles) {
    ValuationAuthorityGate gate;
    gate.step(/*degraded=*/true);

    // Nine clean cycles are not enough.
    for (int i = 0; i < ValuationAuthorityGate::kRearmCleanCycles - 1; ++i) {
        const auto s = gate.step(false);
        EXPECT_FALSE(s.may_update_peak)
            << "peak re-armed early, after " << (i + 1) << " clean cycles";
        EXPECT_FALSE(s.recovered);
    }

    // The tenth re-arms, and says so exactly once.
    const auto s = gate.step(false);
    EXPECT_TRUE(s.may_update_peak);
    EXPECT_TRUE(s.recovered);

    const auto s2 = gate.step(false);
    EXPECT_TRUE(s2.may_update_peak);
    EXPECT_FALSE(s2.recovered) << "recovery must be logged once, not every cycle";
}

TEST(ValuationAuthorityTest, AlternatingJunkCannotRatchetThePeak) {
    // The reason the debounce exists: a feed flapping between junk and
    // honest cycles must never accumulate peak-update permission one
    // accepted cycle at a time.
    ValuationAuthorityGate gate;
    for (int i = 0; i < 50; ++i) {
        gate.step(/*degraded=*/true);
        const auto s = gate.step(/*degraded=*/false);
        ASSERT_FALSE(s.may_update_peak)
            << "alternating cycles granted peak authority at iteration " << i;
    }
}

TEST(ValuationAuthorityTest, DegradedWarnsOncePerEpisodeNotEveryCycle) {
    ValuationAuthorityGate gate;
    EXPECT_TRUE(gate.step(true).entered_degraded);
    for (int i = 0; i < 20; ++i) {
        EXPECT_FALSE(gate.step(true).entered_degraded)
            << "re-warned mid-episode at cycle " << i;
    }

    // A completed recovery re-arms the warning for the NEXT episode.
    for (int i = 0; i < ValuationAuthorityGate::kRearmCleanCycles; ++i) {
        gate.step(false);
    }
    EXPECT_TRUE(gate.step(true).entered_degraded)
        << "a new degraded episode after recovery must warn again";
}
