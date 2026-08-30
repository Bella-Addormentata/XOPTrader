// ---------------------------------------------------------------------------
// [S24] The direct-lookup verdict routing -- the 6-XCH-incident guard.
//
// A wrong Stale here becomes State::remove_offer + a DB 'cancelled' write,
// and any later fill on that offer is lost forever. These tests pin the
// rule: only explicit terminal statuses may reap; everything unknown keeps
// the offer tracked.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include "xop/monitoring/reconcile_verdict.hpp"

using xop::monitoring::DirectLookupVerdict;
using xop::monitoring::classify_direct_lookup;

TEST(ReconcileVerdict, pending_statuses_are_live)
{
    EXPECT_EQ(classify_direct_lookup(0), DirectLookupVerdict::Live);
    EXPECT_EQ(classify_direct_lookup(1), DirectLookupVerdict::Live);
    EXPECT_EQ(classify_direct_lookup(2), DirectLookupVerdict::Live);
}

TEST(ReconcileVerdict, confirmed_defers_to_the_fill_detector)
{
    // A fill mislabelled 'cancelled' corrupts fill-rate analytics and,
    // worse, removes the offer before detect_fills can attribute it.
    EXPECT_EQ(classify_direct_lookup(4),
              DirectLookupVerdict::DeferToFillDetector);
}

TEST(ReconcileVerdict, only_explicit_terminals_reap)
{
    EXPECT_EQ(classify_direct_lookup(3), DirectLookupVerdict::Stale);
    EXPECT_EQ(classify_direct_lookup(5), DirectLookupVerdict::Stale);
}

TEST(ReconcileVerdict, unknown_codes_keep_the_offer_tracked)
{
    // [review] A future wallet extending TradeStatus (6 = some new
    // pending-like state) must NOT have its live offers reaped -- the
    // exact 2026-07-31 shape. Unknown means no evidence.
    EXPECT_EQ(classify_direct_lookup(6), DirectLookupVerdict::KeepTracked);
    EXPECT_EQ(classify_direct_lookup(42), DirectLookupVerdict::KeepTracked);
    EXPECT_EQ(classify_direct_lookup(-1), DirectLookupVerdict::KeepTracked);
    EXPECT_EQ(classify_direct_lookup(-7), DirectLookupVerdict::KeepTracked);
}
