// test_bridge_ingest.cpp -- Unit tests for warp bridge-flow accounting
// ([S19 2026-08-23]).
//
// The constants below are the MEASURED production numbers from the first
// live bridge (2026-08-23, warp_jobs.db job 2):
//   - inbound mint: 4,985 CAT mojos of wUSDC.b (post_tip_mojos, after the
//     warp.green 0.3% tip on a 5,000-mojo deposit) at block 9189949
//   - wUSDC.b is valued at exactly $1.00 (the numeraire doctrine), so the
//     flow is $4.985 and the pseudo price is 1e12
//
// ISO/IEC 27001:2022 -- no secrets; pure numerical verification.
// ISO/IEC 5055       -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/accounting/bridge_ingest.hpp>
#include <xop/config.hpp>

#include <cmath>
#include <string>

namespace {

using namespace xop::accounting;

// Production-shaped constants (job 2, first live bridge).
constexpr xop::Mojo kJob2Minted = 4'985;   // post_tip_mojos
constexpr xop::Mojo kJob2Gross  = 5'000;   // amount_mojos (pre-tip)

BridgeJobRow inbound_job() {
    BridgeJobRow r;
    r.id             = 2;
    r.amount_mojos   = kJob2Gross;
    r.post_tip_mojos = kJob2Minted;
    r.flow_at        = "2026-08-23T19:28:03+00:00";  // first COMPLETED event
    r.created_at     = "2026-08-23T11:59:38+00:00";  // immutable, live job 2
    // Inbound jobs carry no "direction" key; that absence IS the marker.
    // The fingerprint is the live job 2 shape (v1:<erc20>:<dec>:<dec>:<id>).
    r.state_json     = R"({"phase": "done", "claim_block": 9189949,
        "asset_fingerprint": "v1:833589fcd6edb6e08f4c7c32d4f71b54bda02913:6:3:fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"})";
    return r;
}

BridgeJobRow outbound_job() {
    BridgeJobRow r;
    r.id             = 3;
    r.amount_mojos   = 20'000;             // burned CAT mojos
    r.post_tip_mojos = 0;
    r.flow_at        = "2026-08-24T02:00:00+00:00";
    r.created_at     = "2026-08-24T01:30:00+00:00";
    r.state_json     = R"({"direction": "out", "receiver_evm": "3b04",
        "asset_fingerprint": "v1:833589fcd6edb6e08f4c7c32d4f71b54bda02913:6:3:fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"})";
    return r;
}

// ============================================================================
// Classification
// ============================================================================

TEST(BridgeClassifyTest, FirstLiveBridgeBooksAsDeposit) {
    const auto f = classify_bridge_job(inbound_job());
    ASSERT_TRUE(f.valid);
    EXPECT_TRUE(f.inbound);
    EXPECT_EQ(f.delta_mojos, kJob2Minted);      // post-tip, not gross
    EXPECT_EQ(f.event_type, "bridge_deposit");
    // The immutable created_at is part of the identity: a recreated
    // jobs DB reusing AUTOINCREMENT id 2 cannot collide (round 3).
    EXPECT_EQ(f.event_id, "bridge:job:2:2026-08-23T11:59:38+00:00");
}

TEST(BridgeClassifyTest, UnwrapBooksAsWithdrawal) {
    const auto f = classify_bridge_job(outbound_job());
    ASSERT_TRUE(f.valid);
    EXPECT_FALSE(f.inbound);
    EXPECT_EQ(f.delta_mojos, -20'000);          // burn: negative delta
    EXPECT_EQ(f.event_type, "bridge_withdrawal");
    EXPECT_EQ(f.event_id, "bridge:job:3:2026-08-24T01:30:00+00:00");
}

TEST(BridgeClassifyTest, UnparseableStateIsSkippedNotGuessed) {
    // A corrupt state payload must not default to a direction: booking a
    // burn as a mint (or vice versa) is a signed error twice the flow.
    auto r = inbound_job();
    r.state_json = "{not json";
    EXPECT_FALSE(classify_bridge_job(r).valid);
}

TEST(BridgeClassifyTest, NonPositiveQuantitiesAreRefused) {
    auto in = inbound_job();
    in.post_tip_mojos = 0;                      // COMPLETED but no mint??
    EXPECT_FALSE(classify_bridge_job(in).valid);

    auto out = outbound_job();
    out.amount_mojos = 0;
    EXPECT_FALSE(classify_bridge_job(out).valid);

    auto bad_id = inbound_job();
    bad_id.id = 0;
    EXPECT_FALSE(classify_bridge_job(bad_id).valid);
}

TEST(BridgeClassifyTest, UnknownDirectionIsRefusedNotDefaulted) {
    // (review round 1) Any present direction other than exactly "out" is
    // unclassifiable -- a typo or future enum value must never default
    // to a signed booking.
    auto r = inbound_job();
    r.state_json = R"({"direction": "outt"})";
    EXPECT_FALSE(classify_bridge_job(r).valid);
    r.state_json = R"({"direction": "in"})";
    EXPECT_FALSE(classify_bridge_job(r).valid);
    r.state_json = R"({"direction": ""})";   // empty string == absent
    EXPECT_TRUE(classify_bridge_job(r).valid);
}

TEST(BridgeClassifyTest, FingerprintIsCapturedForTheCaller) {
    const auto f = classify_bridge_job(inbound_job());
    ASSERT_TRUE(f.valid);
    EXPECT_NE(f.asset_fingerprint.find("fa4a180a"), std::string::npos);
}

TEST(BridgeFingerprintTest, LiveJobFingerprintMatchesConfiguredAsset) {
    const xop::AccountingConfig acc{};
    const auto f = classify_bridge_job(inbound_job());
    ASSERT_TRUE(f.valid);
    EXPECT_TRUE(fingerprint_matches_asset(f.asset_fingerprint,
                                          acc.bridge_asset_id));
}

TEST(BridgeFingerprintTest, ForeignAssetDoesNotMatch) {
    // A milliETH job must never book at the $1 numeraire (review round 1).
    const std::string milli_fp =
        "v1:0000000000000000000000000000000000000000:18:3:"
        "b1a2c3d4e5f60718293a4b5c6d7e8f901a2b3c4d5e6f708192a3b4c5d6e7f809";
    const xop::AccountingConfig acc{};
    EXPECT_FALSE(fingerprint_matches_asset(milli_fp, acc.bridge_asset_id));
}

TEST(BridgeFingerprintTest, AbsentOrMalformedFingerprintNeverMatches) {
    const xop::AccountingConfig acc{};
    EXPECT_FALSE(fingerprint_matches_asset("", acc.bridge_asset_id));
    EXPECT_FALSE(fingerprint_matches_asset("no-colons-here",
                                           acc.bridge_asset_id));
    EXPECT_FALSE(fingerprint_matches_asset("v1:", acc.bridge_asset_id));
    EXPECT_FALSE(fingerprint_matches_asset("v1:abc:6:3:",
                                           acc.bridge_asset_id));
}

TEST(BridgeFingerprintTest, WrongShapeOrPrecisionFailsClosed) {
    // Review round 31: the ingester hard-codes 1e3 mojos per unit, so a
    // fingerprint stamped with any other CAT precision (or an unknown
    // version, or a field-count mismatch) must never book.
    const xop::AccountingConfig acc{};
    const std::string id = acc.bridge_asset_id;
    // Wrong CAT-decimals field (would misvalue by 1000x).
    EXPECT_FALSE(fingerprint_matches_asset(
        "v1:833589fcd6edb6e08f4c7c32d4f71b54bda02913:6:6:" + id, id));
    // Unknown version prefix.
    EXPECT_FALSE(fingerprint_matches_asset(
        "v2:833589fcd6edb6e08f4c7c32d4f71b54bda02913:6:3:" + id, id));
    // Field-count mismatches around an otherwise-matching tail.
    EXPECT_FALSE(fingerprint_matches_asset("v1:6:3:" + id, id));
    EXPECT_FALSE(fingerprint_matches_asset(
        "v1:x:y:833589fcd6edb6e08f4c7c32d4f71b54bda02913:6:3:" + id, id));
    // The live shape still matches.
    EXPECT_TRUE(fingerprint_matches_asset(
        "v1:833589fcd6edb6e08f4c7c32d4f71b54bda02913:6:3:" + id, id));
}

TEST(BridgeEventIdTest, SharedBuilderMatchesClassifierOutput) {
    // The engine's pre-fetch booked-candidate check must produce the
    // exact id the classifier books under (round 31).
    EXPECT_EQ(bridge_event_id(2, "2026-08-23 19:10:05"),
              "bridge:job:2:2026-08-23 19:10:05");
}

TEST(BridgeFingerprintTest, MatchIsCaseInsensitive) {
    EXPECT_TRUE(fingerprint_matches_asset("v1:x:6:3:ABCDEF12", "abcdef12"));
    EXPECT_TRUE(fingerprint_matches_asset("v1:x:6:3:abcdef12", "ABCDEF12"));
    EXPECT_FALSE(fingerprint_matches_asset("v1:x:6:3:abcdef13", "abcdef12"));
}

TEST(BridgeClassifyTest, InboundIgnoresGrossAmount) {
    // The gross deposit is 5,000 but only 4,985 arrived on-chain; booking
    // the gross would manufacture 15 mojos of phantom divergence.
    const auto f = classify_bridge_job(inbound_job());
    ASSERT_TRUE(f.valid);
    EXPECT_NE(f.delta_mojos, kJob2Gross);
}

// ============================================================================
// Valuation (the $1.00 numeraire)
// ============================================================================

TEST(BridgeValueTest, Job2ValuesAtFourNinetyEightFive) {
    const auto v = value_bridge_flow(kJob2Minted, 1e3, 1.0);
    EXPECT_EQ(v.fmv_pseudo_price, 1'000'000'000'000LL);   // $1.00 in 1e12
    EXPECT_NEAR(v.flow_usd, 4.985, 1e-9);
}

TEST(BridgeValueTest, WithdrawalValuesNegative) {
    const auto v = value_bridge_flow(-20'000, 1e3, 1.0);
    EXPECT_EQ(v.fmv_pseudo_price, 1'000'000'000'000LL);
    EXPECT_NEAR(v.flow_usd, -20.0, 1e-9);
}

TEST(BridgeValueTest, DegenerateInputsZeroOut) {
    EXPECT_EQ(value_bridge_flow(0, 1e3, 1.0).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, 0.0, 1.0).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, 1e3, 0.0).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, 1e3,
                                std::nan("")).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, std::nan(""),
                                1.0).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, 1e3, 1e13).fmv_pseudo_price, 0);
    // (review round 7) +Inf rates must zero out entirely -- an infinite
    // mojos_per_unit used to yield a nonzero pseudo-price with a zero
    // USD flow.
    EXPECT_EQ(value_bridge_flow(1'000, HUGE_VAL, 1.0).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, 1e3, HUGE_VAL).fmv_pseudo_price, 0);
    // Round 32: a quantity valuing at >= $1e12 must fail closed -- the
    // accumulator ignores it and the restart parser rehydrates zero, so
    // booking it would desynchronize the ledger from the PnL figure.
    EXPECT_EQ(value_bridge_flow(9'000'000'000'000'000'000LL, 1e3, 1.0)
                  .fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(-9'000'000'000'000'000'000LL, 1e3, 1.0)
                  .fmv_pseudo_price, 0);
}

TEST(BridgeValueTest, PseudoPriceScaleCannotOverflowMojo) {
    // (review round 1) The 1e12 scale overflows int64 above ~$9.2M/unit;
    // the guard must reject BEFORE the cast (which would be UB), while
    // legitimate large-but-safe prices still value.
    EXPECT_EQ(value_bridge_flow(1'000, 1e3, 1e7).fmv_pseudo_price, 0);
    EXPECT_EQ(value_bridge_flow(1'000, 1e3, 9.3e6).fmv_pseudo_price, 0);
    const auto ok = value_bridge_flow(1'000, 1e3, 9.0e6);
    EXPECT_GT(ok.fmv_pseudo_price, 0);
}

// ============================================================================
// Note round-trip (writer and parser side by side, so the format
// cannot drift -- same contract as reward_note/parse_reward_fmv_usd)
// ============================================================================

TEST(BridgeNoteTest, DepositRoundTrips) {
    const auto note = bridge_note(4.985, 1.0, 2);
    EXPECT_NE(note.find("deposit"), std::string::npos);
    EXPECT_NE(note.find("job=2"), std::string::npos);
    EXPECT_NEAR(parse_bridge_flow_usd(note), 4.985, 1e-9);
}

TEST(BridgeNoteTest, WithdrawalRoundTripsSigned) {
    // Withdrawals must survive the round trip NEGATIVE -- an unsigned
    // parse (the reward parser's contract) would flip a withdrawal into
    // a deposit on rehydration and double the error.
    const auto note = bridge_note(-20.0, 1.0, 3);
    EXPECT_NE(note.find("withdrawal"), std::string::npos);
    EXPECT_NEAR(parse_bridge_flow_usd(note), -20.0, 1e-9);
}

TEST(BridgeNoteTest, ForeignNotesParseToZero) {
    EXPECT_EQ(parse_bridge_flow_usd(""), 0.0);
    EXPECT_EQ(parse_bridge_flow_usd(
        "unexplained divergence reconciled to wallet"), 0.0);
    EXPECT_EQ(parse_bridge_flow_usd(
        "dexie liquidity reward; fmv_usd=0.0137520000; px_usd_per_unit="
        "0.0137520000; wallet_tx=abc"), 0.0);   // reward note: no flow_usd
    EXPECT_EQ(parse_bridge_flow_usd("flow_usd=garbage"), 0.0);
    // (round 15) Same magnitude bound as the live accumulator: a row
    // add_net_deposit_usd rejects must not reappear on rehydration.
    EXPECT_EQ(parse_bridge_flow_usd("flow_usd=2000000000000.0"), 0.0);
    EXPECT_EQ(parse_bridge_flow_usd("flow_usd=-2000000000000.0"), 0.0);
}

// ============================================================================
// Strict ISO ordering: used by the opening filter (pre-opening flows
// stay inside the opening balance) and by the process-start gate that
// decides whether a booked flow triggers the restart-style peak
// re-anchor (rounds 35+42) -- ties fail closed in both uses.
// ============================================================================

TEST(BridgePeakGuardTest, StrictOrderingBasics) {
    EXPECT_FALSE(iso_strictly_after(
        "2026-08-23T18:31:12+00:00", "2026-08-23T19:08:00+00:00"));
    EXPECT_TRUE(iso_strictly_after(
        "2026-08-23T19:31:12+00:00", "2026-08-23T19:08:00+00:00"));
    // (round 3) Equal second fails CLOSED: the GUI stamps whole seconds,
    // the engine start carries sub-second precision the compare discards,
    // so a same-second flow may already be inside the startup anchor.
    EXPECT_FALSE(iso_strictly_after(
        "2026-08-23T19:08:00+00:00", "2026-08-23T19:08:00.412Z"));
}

TEST(BridgePeakGuardTest, SuffixStylesCompareCorrectly) {
    // GUI writes "+00:00" offsets; the engine writes "Z".  The 19-char
    // prefix compare must be indifferent to the suffix style.
    EXPECT_TRUE(iso_strictly_after(
        "2026-08-23T19:31:12+00:00", "2026-08-23T19:08:00Z"));
    EXPECT_FALSE(iso_strictly_after(
        "2026-08-23T18:31:12Z", "2026-08-23T19:08:00+00:00"));
}

TEST(BridgePeakGuardTest, MalformedTimestampsFailClosed) {
    EXPECT_FALSE(iso_strictly_after("", "2026-08-23T19:08:00Z"));
    EXPECT_FALSE(iso_strictly_after("2026-08-23T19:31:12Z", ""));
    EXPECT_FALSE(iso_strictly_after("yesterday", "today"));
    // (review round 2) Length alone is not fail-closed: 19 chars of
    // garbage that sorts after any digit prefix must NOT shift the peak.
    EXPECT_FALSE(iso_strictly_after(
        "zzzzzzzzzzzzzzzzzzz", "2026-08-23T19:08:00Z"));
    EXPECT_FALSE(iso_strictly_after(
        "2026-08-23X19:31:12Z", "2026-08-23T19:08:00Z"));  // bad separator
    // (round 13) Shape-valid but semantically impossible fields must
    // fail closed too -- "month 99" sorts after any real timestamp.
    EXPECT_FALSE(iso_strictly_after(
        "2026-99-99T99:99:99Z", "2026-08-23T19:08:00Z"));
    EXPECT_FALSE(iso_strictly_after(
        "2026-13-01T00:00:00Z", "2026-08-01T00:00:00Z"));
    // (round 22) Calendar-exact: Feb 31 / Apr 31 are impossible even
    // though <= 31; leap-year Feb 29 is valid.
    EXPECT_FALSE(iso_strictly_after(
        "2026-02-31T00:00:00Z", "2026-01-01T00:00:00Z"));
    EXPECT_FALSE(iso_strictly_after(
        "2026-04-31T00:00:00Z", "2026-01-01T00:00:00Z"));
    EXPECT_TRUE(iso_strictly_after(
        "2028-02-29T00:00:00Z", "2026-01-01T00:00:00Z"));
    EXPECT_FALSE(iso_strictly_after(
        "2026-02-29T00:00:00Z", "2026-01-01T00:00:00Z"));
    EXPECT_FALSE(iso_strictly_after(
        "zzzzzzzzzzzzzzzzzzz", "2026-08-01T00:00:00Z"));
}

// ============================================================================
// Opening filter (review round 1: fresh/reset-ledger double-count guard)
// ============================================================================

TEST(BridgeOpeningFilterTest, JobAfterOpeningBooks) {
    EXPECT_TRUE(iso_strictly_after(
        "2026-08-23T19:31:12+00:00", "2026-08-01T00:00:00Z"));
}

TEST(BridgeOpeningFilterTest, JobAtOrBeforeOpeningSkips) {
    // A tie skips ON PURPOSE: under-booking degrades to the pre-S19
    // divergence-adjust behaviour, double-booking permanently overstates
    // net deposits.
    EXPECT_FALSE(iso_strictly_after(
        "2026-08-01T00:00:00Z", "2026-08-01T00:00:00+00:00"));
    EXPECT_FALSE(iso_strictly_after(
        "2026-07-31T23:59:59Z", "2026-08-01T00:00:00Z"));
}

TEST(BridgeOpeningFilterTest, MalformedTimestampsFailClosed) {
    EXPECT_FALSE(iso_strictly_after("", "2026-08-01T00:00:00Z"));
    EXPECT_FALSE(iso_strictly_after("2026-08-23T19:31:12Z", ""));
}

// ============================================================================
// Config wiring
// ============================================================================

TEST(BridgeConfigTest, DefaultsMatchTheLiveDeployment) {
    const xop::AccountingConfig acc{};
    EXPECT_TRUE(acc.bridge_ingest_enabled);
    EXPECT_EQ(acc.bridge_jobs_db_path, "data/warp_jobs.db");
    // wUSDC.b mainnet CAT id -- must match config.yaml's quote_asset_id
    // for the wUSDC.b pairs.
    EXPECT_EQ(acc.bridge_asset_id,
              "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901b"
              "aa6b7a99d");
}

}  // namespace
