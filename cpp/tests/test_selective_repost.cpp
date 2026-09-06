// ---------------------------------------------------------------------------
// [S33 2026-09-05] Step 8's selective-refresh whitelist -- a cancel that
// failed is not a cancel.
//
// The T5-01 filter used to derive its repost whitelist from tier STALENESS.
// OfferManager::selective_cancel returns only the offers it actually
// cancelled: on a ChiaRPCError that is not an insufficient-funds error it logs
// and moves on WITHOUT marking the offer cancel_pending.  A tier could
// therefore classify Stale, keep its old offer resting on the book, and still
// have its replacement posted -- two live offers at the same price level, the
// exact double exposure the filter exists to prevent.
//
// (An earlier draft of this note also blamed selective_cancel's "skip offers
// already awaiting cancel confirmation" branch.  It cannot produce this state:
// classify_tier_staleness does `if (po.cancel_pending) continue;`, so such an
// offer never reaches a TierClassification at all.  The swallowed RPC error is
// the whole of it.)
//
// select_repost_keys is that rule and select_postable_tiers is the whole
// decision it sits in -- GATE included, because the gate was the other half of
// the defect: the filter ran only when some tier was still Fresh, so the
// sibling full-cancel branch never consulted it.  Both are lifted out of the
// coroutine so they are reachable from ctest.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "xop/engine.hpp"

using xop::Side;
using xop::TierQuote;
using xop::execution::TierClassification;
using xop::execution::TierStaleness;
using xop::select_postable_tiers;
using xop::select_repost_keys;
using xop::select_resting_keys;

namespace {

TierClassification tier(const std::string& offer_id,
                        TierStaleness      staleness,
                        std::uint8_t       tier_index,
                        Side               side = Side::Bid)
{
    TierClassification tc{};
    tc.offer_id   = offer_id;
    tc.staleness  = staleness;
    tc.tier_index = tier_index;
    tc.side       = side;
    return tc;
}

std::string key(Side side, std::uint8_t tier_index)
{
    return std::to_string(static_cast<int>(side)) + "_"
         + std::to_string(tier_index);
}

TierQuote candidate(std::uint8_t tier_index, Side side = Side::Bid)
{
    TierQuote tq{};
    tq.tier_index = tier_index;
    tq.side       = side;
    tq.price      = 1'000'000;
    tq.size       = 1'000'000;
    return tq;
}

std::set<std::string> keys_of(const std::vector<TierQuote>& tiers)
{
    std::set<std::string> out;
    for (const auto& tq : tiers) out.insert(key(tq.side, tq.tier_index));
    return out;
}

// The dynamic XCH budget limiter, reduced to the only thing that matters
// here: which candidate tiers it CHARGES.  Mirrors the `if
// (resting_keys.count(resting_key) > 0) continue;` skip in step_manage_offers.
std::set<std::string> charged_keys(
    const std::vector<TierQuote>&                    candidates,
    const std::vector<TierClassification>&           tier_classes,
    const std::vector<std::string>&                  cancelled_ids)
{
    const auto resting = select_resting_keys(tier_classes, cancelled_ids);
    std::set<std::string> out;
    for (const auto& tq : candidates) {
        const std::string k = key(tq.side, tq.tier_index);
        if (resting.count(k) > 0) continue;
        out.insert(k);
    }
    return out;
}

}  // namespace

// The whole point of the finding: tier 1's cancel RPC failed, so its old
// offer is still on the book and its replacement must NOT be posted.
TEST(SelectiveRepost, FailedCancelYieldsNoRepostKey)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale,   0),
        tier("b", TierStaleness::Stale,   1),
        tier("c", TierStaleness::Fresh,   2),
    };
    const std::vector<std::string> cancelled_ids{"a"};

    const auto keys = select_repost_keys(classes, cancelled_ids);

    EXPECT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.count(key(Side::Bid, 0)), 1u)
        << "tier 0 was actually cancelled -- its replacement may post";
    EXPECT_EQ(keys.count(key(Side::Bid, 1)), 0u)
        << "tier 1's cancel failed: its offer is still live, so reposting "
           "would double the exposure at that price level";
    EXPECT_EQ(keys.count(key(Side::Bid, 2)), 0u)
        << "a Fresh tier was never asked to be cancelled";
}

// An Expired tier is cancelled through the same path and must behave the
// same way -- success is what matters, not which non-Fresh class it was.
TEST(SelectiveRepost, ExpiredTierFollowsCancelSuccessToo)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Expired, 0, Side::Ask),
        tier("b", TierStaleness::Expired, 1, Side::Ask),
    };
    const std::vector<std::string> cancelled_ids{"b"};

    const auto keys = select_repost_keys(classes, cancelled_ids);

    EXPECT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys.count(key(Side::Ask, 1)), 1u);
    EXPECT_EQ(keys.count(key(Side::Ask, 0)), 0u);
}

// b62bade's fix must survive: with every tier Fresh nothing was cancelled, so
// the whitelist is empty and only brand-new tiers (handled by the caller's
// pending_keys branch, not here) post.
TEST(SelectiveRepost, AllFreshYieldsEmptySet)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Fresh, 0),
        tier("b", TierStaleness::Fresh, 1),
    };

    EXPECT_TRUE(select_repost_keys(classes, {}).empty());
}

// The ordinary path: every cancel succeeded, so every stale tier reposts.
TEST(SelectiveRepost, AllCancelsSucceed)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale, 0),
        tier("b", TierStaleness::Stale, 1, Side::Ask),
    };
    const std::vector<std::string> cancelled_ids{"a", "b"};

    const auto keys = select_repost_keys(classes, cancelled_ids);

    EXPECT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys.count(key(Side::Bid, 0)), 1u);
    EXPECT_EQ(keys.count(key(Side::Ask, 1)), 1u);
}

// A total cancel failure must whitelist nothing at all.  This branch dropped
// main's `&& !cancelled_ids.empty()` guard, so the filter now runs in this
// case; it must repost nothing rather than everything.
TEST(SelectiveRepost, EveryCancelFailedYieldsEmptySet)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale, 0),
        tier("b", TierStaleness::Expired, 1),
    };

    EXPECT_TRUE(select_repost_keys(classes, {}).empty());
}

// Bid tier 0 and ask tier 0 are distinct slots; a cancelled bid must not
// unlock the ask's replacement.
TEST(SelectiveRepost, KeysAreSideQualified)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale, 0, Side::Bid),
        tier("b", TierStaleness::Stale, 0, Side::Ask),
    };
    const std::vector<std::string> cancelled_ids{"a"};

    const auto keys = select_repost_keys(classes, cancelled_ids);

    EXPECT_EQ(keys.count(key(Side::Bid, 0)), 1u);
    EXPECT_EQ(keys.count(key(Side::Ask, 0)), 0u);
}

// ---------------------------------------------------------------------------
// [S33-LIMITER 2026-09-05] select_resting_keys -- the other half of the same
// accounting, for the dynamic XCH budget limiter.
//
// The limiter measures the whole ladder against a budget derived from the
// wallet's spendable_balance, but spendable_balance has already had the coins
// locked by resting offers taken out of it.  Charging a still-resting tier
// against that budget counts the same XCH twice, and near the fee reserve it
// trims the side below the tier the refresh is trying to replace.
//
// Mutation check for these tests: make select_resting_keys return an empty set
// (the pre-fix behaviour, where every tier was charged) and
// AFreshTierIsRestingAndMustNotBeCharged goes red.  Invert the membership test
// to `> 0` and CancelledTiersStayChargedUntilTheCancelConfirms goes red.
// ---------------------------------------------------------------------------

// The finding itself: a Fresh tier's offer is still on the book, so its coins
// are already outside the budget and it must not be charged again.
TEST(RestingKeys, AFreshTierIsRestingAndMustNotBeCharged)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Fresh, 0),
        tier("b", TierStaleness::Fresh, 1),
    };

    const auto keys = select_resting_keys(classes, {});

    EXPECT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys.count(key(Side::Bid, 0)), 1u);
    EXPECT_EQ(keys.count(key(Side::Bid, 1)), 1u);
}

// The conservative half, and the reason this is not simply "not reposting".
// A submitted cancel has NOT confirmed on-chain, so those coins are not back
// in spendable_balance yet.  They keep being charged.
TEST(RestingKeys, CancelledTiersStayChargedUntilTheCancelConfirms)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale, 0),
        tier("b", TierStaleness::Stale, 1),
    };
    const std::vector<std::string> cancelled_ids{"a", "b"};

    EXPECT_TRUE(select_resting_keys(classes, cancelled_ids).empty())
        << "a cancel is submitted, not confirmed -- the coins are still gone "
           "from spendable_balance, so the limiter must keep charging them";
}

// A cancel whose RPC failed leaves the offer resting, so it is NOT charged --
// and (via select_repost_keys) it is not reposted either.  The two functions
// must agree about that tier, or the limiter frees budget for a replacement
// the filter will never post.
TEST(RestingKeys, AFailedCancelIsRestingAndIsAlsoNotReposted)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale, 0),
        tier("b", TierStaleness::Stale, 1),
    };
    const std::vector<std::string> cancelled_ids{"a"};  // "b" failed

    const auto resting = select_resting_keys(classes, cancelled_ids);
    const auto repost  = select_repost_keys(classes, cancelled_ids);

    EXPECT_EQ(resting.count(key(Side::Bid, 1)), 1u) << "b is still on the book";
    EXPECT_EQ(repost.count(key(Side::Bid, 1)), 0u)  << "so b must not repost";
    EXPECT_EQ(resting.count(key(Side::Bid, 0)), 0u);
    EXPECT_EQ(repost.count(key(Side::Bid, 0)), 1u);
}

// Brand-new tiers have no classification entry at all, so they are absent from
// the resting set and stay fully charged -- they really do lock new coins.
TEST(RestingKeys, BrandNewTiersAreNotRestingAndStayCharged)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Fresh, 0),
    };

    const auto keys = select_resting_keys(classes, {});

    EXPECT_EQ(keys.count(key(Side::Bid, 0)), 1u);
    EXPECT_EQ(keys.count(key(Side::Bid, 1)), 0u)
        << "tier 1 has no live offer -- posting it locks new XCH";
    EXPECT_EQ(keys.count(key(Side::Ask, 0)), 0u)
        << "keys are side-qualified: the ask side of tier 0 is unposted";
}

// One (side, tier) slot holding TWO pending offers is possible after any
// earlier double post.  If their cancels disagree, select_repost_keys
// whitelists the slot for repost -- so the limiter must CHARGE it, or the
// replacement is posted against a budget that never reserved for it.
TEST(RestingKeys, ADuplicatedSlotWithOneCancelledLegIsCharged)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale, 0),
        tier("b", TierStaleness::Stale, 0),   // same slot as "a"
    };
    const std::vector<std::string> cancelled_ids{"a"};  // "b" failed

    const auto resting = select_resting_keys(classes, cancelled_ids);
    const auto repost  = select_repost_keys(classes, cancelled_ids);

    EXPECT_EQ(repost.count(key(Side::Bid, 0)), 1u)
        << "one leg was cancelled, so the whitelist admits the slot";
    EXPECT_EQ(resting.count(key(Side::Bid, 0)), 0u)
        << "a slot that may be reposted must never be excluded from the "
           "budget charge -- posted must stay a subset of charged";
}

// ---------------------------------------------------------------------------
// [S33 2026-09-05] select_postable_tiers -- the GATE, which is where the
// double exposure actually survived.
//
// The call site ran the whitelist only when `has_pending && fresh_count > 0`.
// The sibling branch -- has_pending && fresh_count == 0, i.e. EVERY tier
// stale, cancelled through one selective_cancel(all_ids) call -- skipped it
// entirely, so a cancel that failed there left its offer resting AND had a
// replacement posted over it.  The limiter made that strictly worse: it
// excludes resting tiers from the XCH budget unconditionally, so those
// duplicates were posted unreserved against a budget built from
// spendable_balance (the 2026-08-23 zero-spendable shape).
//
// MUTATION CHECK for this group: reinstate the gate inside
// select_postable_tiers, i.e. add
//
//     int fresh = 0;
//     for (const auto& tc : tier_classes)
//         if (tc.staleness == execution::TierStaleness::Fresh) ++fresh;
//     if (fresh == 0) return candidate_tiers;
//
// after the `tier_classes.empty()` early return.  AllStaleWithAFailedCancel*
// and ChargedSetEqualsPostedSetWhenEveryTierIsStale must go red.
// ---------------------------------------------------------------------------

// THE finding: every tier stale, one cancel failed, so that slot is still
// occupied and must not be reposted.
TEST(PostableTiers, AllStaleWithAFailedCancelDoesNotRepostThatTier)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale,   0),
        tier("b", TierStaleness::Stale,   1),
        tier("c", TierStaleness::Expired, 0, Side::Ask),
        tier("d", TierStaleness::Expired, 1, Side::Ask),
    };
    // ask tier 1's cancel RPC threw and was swallowed.
    const std::vector<std::string> cancelled_ids{"a", "b", "c"};

    const std::vector<TierQuote> candidates{
        candidate(0), candidate(1),
        candidate(0, Side::Ask), candidate(1, Side::Ask),
    };

    const auto postable = select_postable_tiers(candidates, classes,
                                                cancelled_ids);

    EXPECT_EQ(keys_of(postable),
              (std::set<std::string>{key(Side::Bid, 0), key(Side::Bid, 1),
                                     key(Side::Ask, 0)}))
        << "ask tier 1 is still resting on the book -- posting its "
           "replacement is the double exposure this filter exists to prevent";
}

// BLOCKING 1: the limiter and the filter must agree about that same tier, or
// the budget reserves for a different set than the one that gets posted.
TEST(PostableTiers, ChargedSetEqualsPostedSetWhenEveryTierIsStale)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale,   0),
        tier("b", TierStaleness::Stale,   1),
        tier("c", TierStaleness::Expired, 0, Side::Ask),
        tier("d", TierStaleness::Expired, 1, Side::Ask),
    };
    const std::vector<std::string> cancelled_ids{"a", "b", "c"};

    const std::vector<TierQuote> candidates{
        candidate(0), candidate(1),
        candidate(0, Side::Ask), candidate(1, Side::Ask),
    };

    EXPECT_EQ(keys_of(select_postable_tiers(candidates, classes,
                                            cancelled_ids)),
              charged_keys(candidates, classes, cancelled_ids))
        << "the limiter skipped charging the failed-cancel tier because its "
           "coins never left the wallet locked set; if the filter then posts "
           "it anyway, that XCH lock was never reserved for";
}

// The same agreement on the selective-refresh branch (some tiers Fresh),
// which is the only branch the filter used to run in.
TEST(PostableTiers, ChargedSetEqualsPostedSetOnASelectiveRefresh)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Fresh, 0),
        tier("b", TierStaleness::Stale, 1),
        tier("c", TierStaleness::Stale, 2),
    };
    const std::vector<std::string> cancelled_ids{"b"};   // "c" failed

    const std::vector<TierQuote> candidates{
        candidate(0), candidate(1), candidate(2), candidate(3),
    };

    const auto postable = select_postable_tiers(candidates, classes,
                                                cancelled_ids);
    EXPECT_EQ(keys_of(postable),
              (std::set<std::string>{key(Side::Bid, 1), key(Side::Bid, 3)}))
        << "tier 0 is fresh, tier 2 cancel failed, tier 3 is brand new";
    EXPECT_EQ(keys_of(postable),
              charged_keys(candidates, classes, cancelled_ids));
}

// The wiring the extracted whitelist could not see: dropping the old
// `&& !cancelled_ids.empty()` guard must not stop the ladder from growing.
TEST(PostableTiers, ABrandNewTierStillPostsWhenEveryCancelFailed)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale,   0),
        tier("b", TierStaleness::Expired, 1),
    };

    const std::vector<TierQuote> candidates{
        candidate(0), candidate(1), candidate(2),
    };

    EXPECT_EQ(keys_of(select_postable_tiers(candidates, classes, {})),
              (std::set<std::string>{key(Side::Bid, 2)}))
        << "tier 2 had no pending offer at all, so it cannot duplicate one";
}

// ...and with nothing new to post, the caller `continue` is the right answer:
// post nothing rather than everything.
TEST(PostableTiers, EveryCancelFailedAndNoNewTiersPostsNothing)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Stale,   0),
        tier("b", TierStaleness::Expired, 1),
    };
    const std::vector<TierQuote> candidates{candidate(0), candidate(1)};

    EXPECT_TRUE(select_postable_tiers(candidates, classes, {}).empty());
}

// A Fresh tier is never reposted, whatever else happened this heartbeat.
// (b62bade fix, at the level that decides posting.)
TEST(PostableTiers, FreshTiersAreNeverReposted)
{
    const std::vector<TierClassification> classes{
        tier("a", TierStaleness::Fresh, 0),
        tier("b", TierStaleness::Fresh, 1),
    };
    const std::vector<TierQuote> candidates{candidate(0), candidate(1)};

    EXPECT_TRUE(select_postable_tiers(candidates, classes, {}).empty());
}

// Nothing pending: every candidate is postable and the order is preserved.
// This is the "post from scratch" path, which must not be filtered at all.
TEST(PostableTiers, NothingPendingPostsEveryCandidate)
{
    const std::vector<TierQuote> candidates{
        candidate(0), candidate(1), candidate(0, Side::Ask),
    };

    const auto postable = select_postable_tiers(candidates, {}, {});
    ASSERT_EQ(postable.size(), 3u);
    EXPECT_EQ(postable[0].tier_index, 0);
    EXPECT_EQ(postable[1].tier_index, 1);
    EXPECT_EQ(postable[2].side, Side::Ask);
}
