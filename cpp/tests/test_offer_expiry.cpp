// ---------------------------------------------------------------------------
// [OFFER-EXPIRY] The on-chain offer-expiry decisions.
//
// These pin four things, each of which is a way the feature could quietly
// become harmful rather than protective:
//
//   1. A per-pair 0 means "no expiry HERE" and must bind, not inherit.
//   2. A broken host clock must produce no timelock, never a born-expired
//      offer.
//   3. The wallet's echo must be verified, and every not-honoured shape --
//      absent, null, wrong type, mismatched -- must read as failure.  A
//      wallet that ignores max_time returns SUCCESS with the flag missing,
//      so a permissive check here would rest offers we wrongly believe
//      self-retire.
//   4. An expiry must outlast our own hard TTL, or the chain retires offers
//      the engine still tracks as live.
//
// The functions are pure and clock-free precisely so this file can drive
// them; nothing here constructs an OfferManager.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "xop/execution/offer_expiry.hpp"
#include "xop/rpc/chia_rpc.hpp"

using nlohmann::json;
using xop::execution::effective_offer_expiry_secs;
using xop::execution::expiry_echo_ok;
using xop::execution::expiry_max_time_from;
using xop::execution::expiry_outlasts_hard_ttl;
using xop::execution::age_limit_cancel_applies;
using xop::execution::chain_clock_trust;
using xop::execution::chain_clock_trust_reason;
using xop::execution::ChainClockTrust;
using xop::execution::decide_expired_retire;
using xop::execution::expired_at_depth;
using xop::execution::expired_retire_clock_height;
using xop::execution::ExpiredRetire;
using xop::execution::expiry_warn_due;
using xop::execution::expiry_worth_checking;
using xop::execution::hard_ttl_seconds;
using xop::execution::kExpiredRetireDepthBlocks;
using xop::execution::kExpiryWarnIntervalBlocks;
using xop::execution::kMinPlausibleUnixTime;
using xop::execution::trade_record_max_time;
using xop::rpc::ChiaWalletRPC;

namespace {

// A response shaped like the one chia 2.7.3 actually returns; the
// valid_times block below was captured from a live get_all_offers.
json response_with_max_time(const json& max_time_value) {
    json vt = {
        {"max_blocks_after_created", nullptr},
        {"max_height",               nullptr},
        {"max_secs_after_created",   nullptr},
        {"max_time",                 max_time_value},
        {"min_blocks_since_created", nullptr},
        {"min_height",               nullptr},
        {"min_secs_since_created",   nullptr},
        {"min_time",                 nullptr},
    };
    return json{{"offer", "offer1abc"},
                {"trade_record", {{"trade_id", "0xdeadbeef"},
                                  {"valid_times", vt}}}};
}

constexpr std::uint32_t kTtlBlocks = 60;
constexpr std::uint32_t kHardMult  = 2;
// The repo's CONFIGURED mean inter-block interval
// (StrategyConfig::block_time_seconds), not the 18.75s this code used to
// invent for itself -- see TheFloorTracksTheConfiguredBlockTimeNotAConstant.
constexpr double        kSecsBlock = 52.0;

}  // namespace

// ===========================================================================
// 1. Which expiry applies to a pair
// ===========================================================================

TEST(OfferExpiry, AbsentOverrideInheritsTheGlobal) {
    EXPECT_EQ(effective_offer_expiry_secs(std::nullopt, 86400u), 86400u);
    EXPECT_EQ(effective_offer_expiry_secs(std::nullopt, 0u), 0u);
}

TEST(OfferExpiry, PresentOverrideWins) {
    EXPECT_EQ(effective_offer_expiry_secs(std::optional<std::uint32_t>{7200u},
                                          86400u),
              7200u);
}

TEST(OfferExpiry, ZeroOverrideBindsAndDoesNotInherit) {
    // The whole point of the optional: 0 is "never expire THIS pair's
    // offers".  If this ever returns the global, a pair deliberately opted
    // out of expiry starts attaching timelocks anyway.
    EXPECT_EQ(effective_offer_expiry_secs(std::optional<std::uint32_t>{0u},
                                          86400u),
              0u);
}

// ===========================================================================
// 2. Turning that into an absolute max_time
// ===========================================================================

TEST(OfferExpiry, DisabledProducesNoTimelock) {
    EXPECT_FALSE(expiry_max_time_from(1'757'000'000, 0u).has_value());
}

TEST(OfferExpiry, NormalClockProducesNowPlusExpiry) {
    const auto t = expiry_max_time_from(1'757'000'000, 86400u);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, 1'757'000'000ull + 86400ull);
}

TEST(OfferExpiry, StalePositiveClockAlsoProducesNoTimelock) {
    // [review #150] The guard rejected only epoch-and-before, so a host clock
    // reading 1 minted max_time 86401 -- an offer born expired.  An honest
    // wallet echoes that back EXACTLY, so expiry_echo_ok CONFIRMS it and the
    // posting path publishes an unfillable offer.
    EXPECT_FALSE(expiry_max_time_from(1, 86400u).has_value());
    EXPECT_FALSE(expiry_max_time_from(1'000'000'000, 86400u).has_value());
    // A floor, not a window: genesis itself still yields a timelock.
    EXPECT_TRUE(expiry_max_time_from(1'616'162'400, 86400u).has_value());
}

TEST(OfferExpiry, BrokenClockProducesNoTimelockRatherThanABornExpiredOffer) {
    // A host clock at or before the epoch would otherwise mint a max_time in
    // the past: the offer would be dead on arrival while we believed it live.
    for (std::int64_t now : {std::int64_t{0}, std::int64_t{-1},
                             std::numeric_limits<std::int64_t>::min()}) {
        EXPECT_FALSE(expiry_max_time_from(now, 86400u).has_value())
            << "clock " << now << " must produce no timelock";
    }
}

// ===========================================================================
// 3. Verifying the wallet's echo -- every check fails closed
// ===========================================================================

TEST(OfferExpiry, EchoAcceptedOnlyWhenItMatchesExactly) {
    const std::uint64_t want = 1'757'086'400ull;
    EXPECT_TRUE(expiry_echo_ok(response_with_max_time(want), want));
}

TEST(OfferExpiry, EchoRejectedWhenTheWalletSilentlyDroppedTheFlag) {
    // This is the shape a wallet that does not support max_time returns:
    // a successful create whose valid_times carries a null max_time.
    const std::uint64_t want = 1'757'086'400ull;
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time(nullptr), want));
}

TEST(OfferExpiry, EchoRejectedOnMismatch) {
    const std::uint64_t want = 1'757'086'400ull;
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time(want - 1), want));
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time(want + 1), want));
}

TEST(OfferExpiry, EchoRejectedOnWrongType) {
    // A string that happens to spell the right number is not an echo.
    const std::uint64_t want = 1'757'086'400ull;
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time("1757086400"), want));
}

TEST(OfferExpiry, EchoRejectedOnAFloatThatTruncatesToTheExpectedValue) {
    // [review #150] The check used is_number(), which admits number_float,
    // and get<uint64_t>() TRUNCATES -- so this passed an exact-echo test.
    // Chia's max_time is a uint64; a float is not that wallet's answer.
    const std::uint64_t want = 1'757'086'400ull;
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time(1757086400.5), want));
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time(1757086400.0), want));
    // A signed value cannot be a uint64 echo either.
    EXPECT_FALSE(expiry_echo_ok(response_with_max_time(-1), want));
}

TEST(OfferExpiry, EchoRejectedOnMalformedResponses) {
    const std::uint64_t want = 1'757'086'400ull;
    // No trade_record at all.
    EXPECT_FALSE(expiry_echo_ok(json{{"offer", "offer1abc"}}, want));
    // trade_record present but not an object.
    EXPECT_FALSE(expiry_echo_ok(json{{"trade_record", "nope"}}, want));
    // trade_record without valid_times -- the pre-2.x response shape.
    EXPECT_FALSE(expiry_echo_ok(
        json{{"trade_record", {{"trade_id", "0xabc"}}}}, want));
    // valid_times present but not an object.
    EXPECT_FALSE(expiry_echo_ok(
        json{{"trade_record", {{"valid_times", 5}}}}, want));
    // Entirely empty.
    EXPECT_FALSE(expiry_echo_ok(json::object(), want));
}

// ===========================================================================
// 4. The startup floor: expiry must outlast our own hard TTL
// ===========================================================================

TEST(OfferExpiry, HardTtlSecondsIsSoftTtlTimesMultiplier) {
    // 60 blocks x 2 x 52s = 6240s at the configured rate.
    EXPECT_DOUBLE_EQ(hard_ttl_seconds(kTtlBlocks, kHardMult, kSecsBlock),
                     6240.0);
    // And it is a pure product -- the rate is an INPUT, never a constant.
    EXPECT_DOUBLE_EQ(hard_ttl_seconds(60u, 2u, 18.75), 2250.0);
}

TEST(OfferExpiry, DisabledExpiryIsTriviallySafe) {
    EXPECT_TRUE(expiry_outlasts_hard_ttl(0u, kTtlBlocks, kHardMult,
                                         kSecsBlock));
}

TEST(OfferExpiry, ExpiryInsideTheHardTtlIsRejected) {
    // Hard TTL is 6240s at the configured rate; the floor is that times
    // kExpiryFloorMargin.  Anything inside the raw TTL is plainly unsafe:
    // the chain would retire offers the canceller still considers live,
    // with no cancel ever recorded.
    EXPECT_FALSE(expiry_outlasts_hard_ttl(1800u, kTtlBlocks, kHardMult,
                                          kSecsBlock));
    EXPECT_FALSE(expiry_outlasts_hard_ttl(6240u, kTtlBlocks, kHardMult,
                                          kSecsBlock));
}

TEST(OfferExpiry, ExpiryInsideTheSafetyMarginIsAlsoRejected) {
    // [review #150] THE REGRESSION THIS PINS.  6241s outlasts the RAW hard
    // TTL but not the margin.  The TTL is counted in BLOCKS and the expiry
    // fires on WALL CLOCK: when blocks arrive slower than the configured
    // mean the TTL stretches in seconds while a bare floor does not, and a
    // value in this band expires while the engine still tracks the offer as
    // live.  A tie is likewise a race not worth entering.
    EXPECT_FALSE(expiry_outlasts_hard_ttl(6241u, kTtlBlocks, kHardMult,
                                          kSecsBlock));
    EXPECT_FALSE(expiry_outlasts_hard_ttl(12480u, kTtlBlocks, kHardMult,
                                          kSecsBlock));
}

TEST(OfferExpiry, ExpiryBeyondTheMarginIsAccepted) {
    EXPECT_TRUE(expiry_outlasts_hard_ttl(12481u, kTtlBlocks, kHardMult,
                                         kSecsBlock));
    EXPECT_TRUE(expiry_outlasts_hard_ttl(86400u, kTtlBlocks, kHardMult,
                                         kSecsBlock));
}

TEST(OfferExpiry, TheFloorTracksTheConfiguredBlockTimeNotAConstant) {
    // [review #150] The floor used an invented 18.75s/block while this
    // repo's configured mean is 52s -- making it ~2.8x too LOW.  Passing
    // the rate in is what lets the operator's real configuration bind: the
    // SAME expiry that clears at 18.75s/block is refused at 52s/block.
    EXPECT_TRUE(expiry_outlasts_hard_ttl(5000u, kTtlBlocks, kHardMult, 18.75));
    EXPECT_FALSE(expiry_outlasts_hard_ttl(5000u, kTtlBlocks, kHardMult, 52.0));
}

// ===========================================================================
// 5. The RPC payload -- what we send, and more importantly what we never send
// ===========================================================================

TEST(OfferExpiryPayload, NoTimelockKeysAtAllWhenExpiryIsDisabled) {
    const auto p = ChiaWalletRPC::build_create_offer_payload(
        json{{"1", -1000}}, 10000000ull, false, std::nullopt);
    // Byte-identical to the pre-feature payload: three keys, nothing else.
    EXPECT_EQ(p.size(), 3u);
    EXPECT_TRUE(p.contains("offer"));
    EXPECT_TRUE(p.contains("fee"));
    EXPECT_TRUE(p.contains("validate_only"));
    EXPECT_FALSE(p.contains("max_time"));
}

TEST(OfferExpiryPayload, MaxTimeIsSentWhenRequested) {
    const auto p = ChiaWalletRPC::build_create_offer_payload(
        json{{"1", -1000}}, 10000000ull, false,
        std::optional<std::uint64_t>{1'757'086'400ull});
    ASSERT_TRUE(p.contains("max_time"));
    EXPECT_EQ(p["max_time"].get<std::uint64_t>(), 1'757'086'400ull);
}

TEST(OfferExpiryPayload, NeverSendsTheFlagsThatBreakTakers) {
    // THE load-bearing test of this feature.  max_height, min_height and
    // min_time are enforced on-chain but a reference-wallet taker fails on
    // them, so an offer carrying one is visible and unfillable.  If this
    // ever goes red, offers stop being takeable by most of the market.
    for (const auto& mt : {std::optional<std::uint64_t>{},
                           std::optional<std::uint64_t>{1'757'086'400ull}}) {
        const auto p = ChiaWalletRPC::build_create_offer_payload(
            json{{"1", -1000}}, 10000000ull, false, mt);
        EXPECT_FALSE(p.contains("max_height"));
        EXPECT_FALSE(p.contains("min_height"));
        EXPECT_FALSE(p.contains("min_time"));
        // The relative flags are not supported by this API at all.
        EXPECT_FALSE(p.contains("max_blocks_after_created"));
        EXPECT_FALSE(p.contains("max_secs_after_created"));
        EXPECT_FALSE(p.contains("min_blocks_since_created"));
        EXPECT_FALSE(p.contains("min_secs_since_created"));
    }
}

TEST(OfferExpiryPayload, BaseFieldsAreUnchangedByExpiry) {
    const json dict{{"1", -1000}, {"2", 50}};
    const auto without = ChiaWalletRPC::build_create_offer_payload(
        dict, 12345ull, true, std::nullopt);
    auto with = ChiaWalletRPC::build_create_offer_payload(
        dict, 12345ull, true, std::optional<std::uint64_t>{999ull});
    with.erase("max_time");
    // Adding an expiry must change NOTHING else about the request.
    EXPECT_EQ(without, with);
}

TEST(OfferExpiry, TheFloorTracksTheConfiguredTtlRatherThanAFixedNumber) {
    // The example config ships offer_ttl_blocks: 600, so at the configured
    // rate the hard TTL is 62400s and the floor 124800s.  A value generous
    // at TTL 60 is therefore REJECTED at TTL 600 -- which is why the bound
    // is computed from config rather than hardcoded.
    EXPECT_TRUE(expiry_outlasts_hard_ttl(86400u, 60u, kHardMult, kSecsBlock));
    EXPECT_FALSE(expiry_outlasts_hard_ttl(86400u, 600u, kHardMult,
                                          kSecsBlock));
    EXPECT_TRUE(expiry_outlasts_hard_ttl(200000u, 600u, kHardMult,
                                         kSecsBlock));
}

// ===========================================================================
// [S70 2026-09-20] ttl_cancel_mode: expire
//
// Five ways this mode could turn from "fewer cancels" into a loss:
//
//   5. It must spare ONLY an offer with a verified expiry.  Sparing one with
//      none leaves an offer nothing bounds.
//   6. The expiry read back from a wallet record must fail closed exactly as
//      the creation echo does.
//   7. "Expired" is a statement about the CHAIN clock, read at a confirmation
//      DEPTH (a seconds margin is not one -- [review #164]); the host clock
//      may only ever decide to look.
//   8. A trade the wallet no longer reports PENDING_ACCEPT is never retired
//      locally -- CONFIRMED is a fill, and `filled` always wins.
//   9. A record that does not repeat the tracked expiry is never retired
//      locally: that would be the insecure cancel on an unverified timelock.
//  10. [review #164] The clock must come from a peer this host runs.  It is
//      not a local fact and nothing about its VALUE can be checked, so the
//      only defence is a check on WHOSE it is.
// ===========================================================================

namespace {

// A get_offer trade_record as the live chia 2.7.4 wallet returned it on
// 2026-09-20 for an offer posted with offer_expiry_secs: 86400 (key set and
// valid_times block verbatim; ids shortened).
json live_trade_record(const json& max_time_value,
                       const char* status = "PENDING_ACCEPT") {
    return json{
        {"accepted_at_time", nullptr},
        {"confirmed_at_index", 0},
        {"created_at_time", 1789916833},
        {"is_my_offer", true},
        {"sent", 0},
        {"sent_to", json::array()},
        {"status", status},
        {"taken_offer", nullptr},
        {"trade_id", "0x77e25517"},
        {"valid_times", {
            {"max_blocks_after_created", nullptr},
            {"max_height",               nullptr},
            {"max_secs_after_created",   nullptr},
            {"max_time",                 max_time_value},
            {"min_blocks_since_created", nullptr},
            {"min_height",               nullptr},
            {"min_secs_since_created",   nullptr},
            {"min_time",                 nullptr},
        }},
    };
}

constexpr std::uint64_t kMaxTime = 1'790'003'230ull;  // the live record's

}  // namespace

// -- 5. which offers the hard TTL still owns --------------------------------

TEST(OfferExpireMode, DefaultModeKeepsTheHardTtlForEveryOffer) {
    EXPECT_TRUE(age_limit_cancel_applies(/*expire_mode=*/false, 0u));
    EXPECT_TRUE(age_limit_cancel_applies(/*expire_mode=*/false, kMaxTime));
}

TEST(OfferExpireMode, ExpireModeSparesOnlyAVerifiedExpiry) {
    EXPECT_FALSE(age_limit_cancel_applies(/*expire_mode=*/true, kMaxTime));
    // No verified expiry: nothing else bounds this offer's life, so the
    // hard TTL keeps it -- an offer from before the feature, a failed echo,
    // or one restored from offer_log and not yet read back from the wallet.
    EXPECT_TRUE(age_limit_cancel_applies(/*expire_mode=*/true, 0u));
}

// -- 6. the expiry a wallet RECORD carries ----------------------------------

TEST(OfferExpireMode, RecordMaxTimeIsReadFromTheLiveShape) {
    EXPECT_EQ(trade_record_max_time(live_trade_record(kMaxTime)), kMaxTime);
    // And through TEXT, as rpc_post parses the wire -- which is what makes
    // a non-negative integer number_unsigned in the first place.
    const json wire = json::parse(live_trade_record(kMaxTime).dump());
    ASSERT_TRUE(wire["valid_times"]["max_time"].is_number_unsigned());
    EXPECT_EQ(trade_record_max_time(wire), kMaxTime);
}

TEST(OfferExpireMode, RecordMaxTimeFailsClosedOnEveryNotHonouredShape) {
    EXPECT_EQ(trade_record_max_time(live_trade_record(nullptr)), 0u);
    EXPECT_EQ(trade_record_max_time(live_trade_record("1790003230")), 0u);
    EXPECT_EQ(trade_record_max_time(live_trade_record(1790003230.0)), 0u);
    EXPECT_EQ(trade_record_max_time(live_trade_record(1790003230.5)), 0u);
    EXPECT_EQ(trade_record_max_time(live_trade_record(-1)), 0u);
    EXPECT_EQ(trade_record_max_time(json::object()), 0u);
    EXPECT_EQ(trade_record_max_time(json{{"valid_times", nullptr}}), 0u);
    EXPECT_EQ(trade_record_max_time(json{{"valid_times", "nope"}}), 0u);
    EXPECT_EQ(trade_record_max_time(json{{"valid_times", json::object()}}), 0u);
    EXPECT_EQ(trade_record_max_time(json::array()), 0u);
    EXPECT_EQ(trade_record_max_time(json(nullptr)), 0u);
}

TEST(OfferExpireMode, RecordMaxTimeBeforeGenesisIsNotOurs) {
    // Same plausibility floor as expiry_max_time_from: a timelock before
    // mainnet genesis is not one this bot minted, and with chain time far
    // past it every such offer would read as "expired long ago".
    const auto genesis = static_cast<std::uint64_t>(kMinPlausibleUnixTime);
    EXPECT_EQ(trade_record_max_time(live_trade_record(genesis - 1)), 0u);
    EXPECT_EQ(trade_record_max_time(live_trade_record(std::uint64_t{86401})),
              0u);
    EXPECT_EQ(trade_record_max_time(live_trade_record(genesis)), genesis);
}

TEST(OfferExpireMode, TheGetOfferWrapperIsNotWhatIsParsed) {
    // ChiaWalletRPC::get_offer returns the trade_record OBJECT.  Handing the
    // parser the whole response instead must read as "no expiry", not throw.
    const json whole{{"success", true},
                     {"trade_record", live_trade_record(kMaxTime)}};
    EXPECT_EQ(trade_record_max_time(whole), 0u);
    EXPECT_EQ(trade_record_max_time(whole["trade_record"]), kMaxTime);
}

// -- 7. "expired" is the chain's word ----------------------------------------

TEST(OfferExpireMode, TheReorgAllowanceIsADepthInBlocks) {
    // 32 peak-height blocks (~10 min at 18.75 s), over five times the 6 this
    // engine asks of a fill.  Pinned because the header, the PR and the
    // operator procedure all quote it.
    EXPECT_EQ(kExpiredRetireDepthBlocks, 32);
    // The clock is read that far BELOW the wallet's synced height...
    EXPECT_EQ(expired_retire_clock_height(9'319'422), 9'319'390);
    EXPECT_EQ(expired_retire_clock_height(33), 1);
    // ...and a wallet not yet that deep into the chain has no clock at all,
    // rather than a clamped height that would quietly read the tip.
    EXPECT_EQ(expired_retire_clock_height(32), 0);
    EXPECT_EQ(expired_retire_clock_height(1), 0);
    EXPECT_EQ(expired_retire_clock_height(0), 0);
    EXPECT_EQ(expired_retire_clock_height(-7), 0);
}

TEST(OfferExpireMode, ExpiredOnceTheChainAtDepthIsAtOrPastMaxTime) {
    EXPECT_FALSE(expired_at_depth(kMaxTime, kMaxTime - 1));
    // AT max_time counts, as it does in consensus: a spend asserting BEFORE
    // max_time fails once the previous transaction block is stamped >= it.
    EXPECT_TRUE(expired_at_depth(kMaxTime, kMaxTime));
    EXPECT_TRUE(expired_at_depth(kMaxTime, kMaxTime + 86400));
}

TEST(OfferExpireMode, UnknownClockOrUnknownExpiryIsNeverExpired) {
    EXPECT_FALSE(expired_at_depth(0u, kMaxTime + 86400));
    EXPECT_FALSE(expired_at_depth(kMaxTime, 0u));
    EXPECT_FALSE(expired_at_depth(0u, 0u));
}

TEST(OfferExpireMode, ASecondsMarginIsNotAConfirmationDepth) {
    // [review #164] The scenario the first revision got wrong.  The last
    // transaction block before the expiry was stamped max_time - 5; then the
    // chain went quiet, and the NEXT transaction block -- the wallet's tip --
    // is stamped max_time + 700.  A "600 s past max_time" margin read at the
    // tip is satisfied at a confirmation depth of ONE; reorg that block and
    // the offer is takeable again while its trade says CANCELLED.
    //
    // Read 32 blocks back instead, the chain clock is still max_time - 5, and
    // the offer waits.  (That the pass ASKS for the clock at that height, not
    // at the tip, is pinned in tests/test_cancel_reduction_wiring.py.)
    const std::uint64_t clock_at_depth = kMaxTime - 5;
    EXPECT_FALSE(expired_at_depth(kMaxTime, clock_at_depth));
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, clock_at_depth),
              ExpiredRetire::NotExpired);
    // Thirty-two blocks later that same +700 block is what the at-depth read
    // returns, and the offer retires.
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, kMaxTime + 700),
              ExpiredRetire::RetireLocal);
}

TEST(OfferExpireMode, AHugeMaxTimeHasNoArithmeticToWrap) {
    // The decision compares; it never adds to or subtracts from max_time, so a
    // hostile value cannot wrap into "long expired".
    const auto huge = std::numeric_limits<std::uint64_t>::max() - 10;
    EXPECT_FALSE(expired_at_depth(huge, kMaxTime));
    EXPECT_TRUE(expired_at_depth(huge, huge));
}

TEST(OfferExpireMode, TheHostClockOnlyDecidesWhetherToLook) {
    EXPECT_FALSE(expiry_worth_checking(kMaxTime,
                                       static_cast<std::int64_t>(kMaxTime) - 1));
    EXPECT_TRUE(expiry_worth_checking(kMaxTime,
                                      static_cast<std::int64_t>(kMaxTime)));
    // No expiry, or a dead host clock: nothing to look at.
    EXPECT_FALSE(expiry_worth_checking(0u, static_cast<std::int64_t>(kMaxTime)));
    EXPECT_FALSE(expiry_worth_checking(kMaxTime, 0));
    EXPECT_FALSE(expiry_worth_checking(kMaxTime, -5));
}

TEST(OfferExpireMode, AFastHostClockCannotRetireAnything) {
    // The host says the offer expired a day ago; the chain, at depth, says it
    // has two minutes left.  The pre-filter looks -- and the decision says no.
    const std::uint64_t chain_now = kMaxTime - 120;
    EXPECT_TRUE(expiry_worth_checking(
        kMaxTime, static_cast<std::int64_t>(kMaxTime) + 86400));
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, chain_now),
              ExpiredRetire::NotExpired);
}

// -- 8 and 9. the retire decision ---------------------------------------------

TEST(OfferExpireMode, RetiresOnlyAnExpiredVerifiedPendingAcceptOffer) {
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, kMaxTime),
              ExpiredRetire::RetireLocal);
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, kMaxTime + 600),
              ExpiredRetire::RetireLocal);
}

TEST(OfferExpireMode, AnyOtherWalletStatusIsLeftToTheWallet) {
    // CONFIRMED is a fill; PENDING_CANCEL / CANCELLED belong to the path that
    // sent the cancel.  None is retired locally, however long ago it expired
    // -- and the status outranks every other input.
    EXPECT_EQ(decide_expired_retire(false, kMaxTime, kMaxTime,
                                    kMaxTime + 86400),
              ExpiredRetire::LeaveToWallet);
    EXPECT_EQ(decide_expired_retire(false, 0u, 0u, 0u),
              ExpiredRetire::LeaveToWallet);
    EXPECT_EQ(decide_expired_retire(false, kMaxTime, kMaxTime - 1,
                                    kMaxTime + 86400),
              ExpiredRetire::LeaveToWallet);
}

TEST(OfferExpireMode, ARecordThatDoesNotRepeatTheTrackedExpiryIsUnverified) {
    // Dropped (0), earlier, later: each means the timelock we are about to
    // rely on is not the one the wallet signed.
    for (const std::uint64_t rec : {std::uint64_t{0}, kMaxTime - 1,
                                    kMaxTime + 1}) {
        EXPECT_EQ(decide_expired_retire(true, kMaxTime, rec, kMaxTime + 86400),
                  ExpiredRetire::Unverified) << rec;
    }
    // And an offer we never tracked an expiry for is not retired because the
    // record happens to carry one.
    EXPECT_EQ(decide_expired_retire(true, 0u, kMaxTime, kMaxTime + 86400),
              ExpiredRetire::Unverified);
    EXPECT_EQ(decide_expired_retire(true, 0u, 0u, kMaxTime + 86400),
              ExpiredRetire::Unverified);
}

TEST(OfferExpireMode, VerifiedButNotYetExpiredAtDepthWaits) {
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, kMaxTime - 1),
              ExpiredRetire::NotExpired);
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, 0u),
              ExpiredRetire::NotExpired);
}

TEST(OfferExpireMode, ARepeatedRetireWarningIsLoggedOncePerHalfHour) {
    // The retire pass runs every heartbeat while any offer waits past its
    // expiry, and a wallet that cannot supply the chain clock fails the same
    // way each time.  96 peak-height blocks is ~30 min at 18.75 s.
    EXPECT_EQ(kExpiryWarnIntervalBlocks, 96u);
    EXPECT_TRUE(expiry_warn_due(0u, 9'319'000u)) << "never warned";
    EXPECT_FALSE(expiry_warn_due(9'319'000u, 9'319'000u));
    EXPECT_FALSE(expiry_warn_due(9'319'000u, 9'319'095u));
    EXPECT_TRUE(expiry_warn_due(9'319'000u, 9'319'096u));
    // A height that went backwards (a reorg, a height-source switch) must
    // warn rather than wrap into a huge "age" -- or stay silent for good.
    EXPECT_TRUE(expiry_warn_due(9'319'000u, 9'318'990u));
}

// ===========================================================================
// 10. [review #164 2026-09-21] WHOSE clock it is.
//
// Copilot's finding, verified against the chia 2.7.4 source: the timestamp is
// not anchored to the wallet's processed chain.  WalletNode.get_timestamp_for_
// height walks its full-node peers and returns the first non-None answer, with
// expected_header_hash left at None, so on a cache miss the only validation is
// "one block came back" and "its height matches" -- no signature, PoSpace, VDF
// or consensus.  Acting falsely on it frees a still-takeable offer's coins
// with no spend and no undo, and the take is then invisible forever.
//
// The tests below pin the two halves of the honest answer: the arithmetic has
// NO defence of its own (so nobody deletes the gate believing one is in here),
// and the gate is a statement about the peer SET.
// ===========================================================================

TEST(OfferExpireMode, TheClockArithmeticHasNoDefenceAgainstAForgedTimestamp) {
    // A peer answering the largest uint64 expires every offer that will ever
    // exist.  Nothing in this header objects -- expired_at_depth tests only
    // `>=`, and there is deliberately no plausibility ceiling.
    const auto forged_far = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(expired_at_depth(kMaxTime, forged_far));
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, forged_far),
              ExpiredRetire::RetireLocal);

    // And the crude lie is not the one to design against.  A patient peer
    // answers max_time EXACTLY -- a second's worth of lie, which no ceiling
    // and no "is this plausible" test could ever separate from the truth --
    // and gets the same irreversible cancel.
    EXPECT_EQ(decide_expired_retire(true, kMaxTime, kMaxTime, kMaxTime),
              ExpiredRetire::RetireLocal);

    // Nor would a monotonicity rule help: read an honest clock first, then a
    // forged one, and the sequence is still strictly increasing, so a "the
    // chain clock must never go backwards" gate passes both readings.
    const std::uint64_t honest_earlier = kMaxTime - 600;
    EXPECT_FALSE(expired_at_depth(kMaxTime, honest_earlier));
    EXPECT_GT(forged_far, honest_earlier);
    EXPECT_GT(kMaxTime, honest_earlier);

    // Which is the whole argument for gating on WHO answered.
}

TEST(OfferExpireMode, TheClockIsTrustedOnlyWhenEveryFullNodePeerIsOnThisHost) {
    EXPECT_EQ(chain_clock_trust(true, 1, 0), ChainClockTrust::Trusted);
    EXPECT_EQ(chain_clock_trust(true, 4, 0), ChainClockTrust::Trusted);
    // ONE stranger among four is enough.  The RPC never reports which peer
    // answered and get_full_node_peers_in_order() shuffles within its
    // buckets, so the candidate set is the only thing that can be gated.
    EXPECT_EQ(chain_clock_trust(true, 4, 1), ChainClockTrust::UntrustedPeer);
    EXPECT_EQ(chain_clock_trust(true, 3, 3), ChainClockTrust::UntrustedPeer);
    // "Nothing could have answered" and "we could not read the answer" are
    // different operator problems from "a stranger could have"; all three
    // refuse, and each says which it was.
    EXPECT_EQ(chain_clock_trust(true, 0, 0), ChainClockTrust::NoFullNodePeer);
    EXPECT_EQ(chain_clock_trust(false, 1, 0), ChainClockTrust::Unreadable);
    EXPECT_EQ(chain_clock_trust(false, 0, 0), ChainClockTrust::Unreadable);
    // An unreadable census outranks whatever it might have contained: those
    // counts were never established.
    EXPECT_EQ(chain_clock_trust(false, 9, 0), ChainClockTrust::Unreadable);
    EXPECT_EQ(chain_clock_trust(false, 9, 9), ChainClockTrust::Unreadable);
}

TEST(OfferExpireMode, TheTwoStatesThisOperatorHasBeenInBothRefuseTheClock) {
    // (a) THE LOCAL NODE IS DOWN.  wallet_node.py:838-841 on_disconnect clears
    //     local_node_synced and re-runs initialize_wallet_peers; discovery
    //     connects to strangers and the wallet untrusted-syncs to them
    //     (:1481-1499).  The node RPC was unreachable for hours on
    //     2026-09-14 -- this is not a hypothetical.
    EXPECT_EQ(chain_clock_trust(true, 3, 3), ChainClockTrust::UntrustedPeer);
    // (b) THE TRANSIENT AFTER IT RETURNS.  The localhost peer is connected
    //     again but sits in the `trusted` bucket (3rd) until its long_sync
    //     finishes, while already-synced strangers sit in `synced` (2nd) --
    //     so a stranger is asked FIRST for the whole window (:1203-1228).
    EXPECT_EQ(chain_clock_trust(true, 4, 3), ChainClockTrust::UntrustedPeer);
    // Neither is closed by the engine's wallet sync gate (get_sync_status):
    // a wallet synced to strangers reports synced = true.  Only the steady
    // state retires anything.
    EXPECT_EQ(chain_clock_trust(true, 1, 0), ChainClockTrust::Trusted);
}

TEST(OfferExpireMode, EachRefusalNamesItselfToTheOperator) {
    const std::string unreadable =
        chain_clock_trust_reason(ChainClockTrust::Unreadable);
    const std::string no_peer =
        chain_clock_trust_reason(ChainClockTrust::NoFullNodePeer);
    const std::string stranger =
        chain_clock_trust_reason(ChainClockTrust::UntrustedPeer);
    const std::string trusted =
        chain_clock_trust_reason(ChainClockTrust::Trusted);
    for (const std::string* s : {&unreadable, &no_peer, &stranger, &trusted}) {
        EXPECT_FALSE(s->empty());
    }
    // A dead node, a wallet that would not say, and a wallet talking to
    // strangers are three different things to go and fix.
    EXPECT_NE(unreadable, no_peer);
    EXPECT_NE(no_peer, stranger);
    EXPECT_NE(unreadable, stranger);
    // The one that matters most must say why the answer is worthless, not
    // merely that a peer is foreign.
    EXPECT_NE(stranger.find("this host does not run"), std::string::npos);
    EXPECT_NE(stranger.find("whatever a peer says"), std::string::npos);
}
