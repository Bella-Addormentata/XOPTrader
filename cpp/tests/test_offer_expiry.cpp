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

#include "xop/execution/offer_expiry.hpp"
#include "xop/rpc/chia_rpc.hpp"

using nlohmann::json;
using xop::execution::effective_offer_expiry_secs;
using xop::execution::expiry_echo_ok;
using xop::execution::expiry_max_time_from;
using xop::execution::expiry_outlasts_hard_ttl;
using xop::execution::hard_ttl_seconds;
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
