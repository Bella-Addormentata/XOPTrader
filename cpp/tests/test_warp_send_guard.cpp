// test_warp_send_guard.cpp -- Which wallets warp has a Chia send in flight on
// ([WARP-LOST-SEND 2026-10-25], execution/warp_send_guard.hpp).
//
// The rows are job 4 of 2026-10-24 (the claim whose funding send the
// engine's force-delete removed) and the unwrap shapes the GUI writes.
//
// ISO/IEC 5055 -- deterministic tests; no undefined behaviour.

#include <gtest/gtest.h>

#include <xop/execution/warp_send_guard.hpp>

#include <string>

namespace {

using namespace xop::execution;
using nlohmann::json;

const std::string kUsdc =
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";
const std::string kMilli =
    "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6";

TEST(WarpSendGuardTest, AClaimFundingSendProtectsTheXchWallet) {
    // Job 4, as warp_jobs.db held it at 16:13 CDT.
    const json state = {
        {"funding_tx_id", "0xa7f5f64aae1c886e9eb47382aebc47c790e093f385049dc0d7c9601bdd83ef70"},
        {"funding_amount", 100004985},
    };
    const auto s = warp_job_chia_send("FUNDING_CLAIM", state, kUsdc);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->xch);
    EXPECT_TRUE(s->cat_asset_id.empty());
}

TEST(WarpSendGuardTest, AJobStillWaitingToFundProtectsNothing) {
    EXPECT_FALSE(warp_job_chia_send("FUNDING_CLAIM", json::object(), kUsdc)
                     .has_value());
    EXPECT_FALSE(warp_job_chia_send("FUNDING_CLAIM", json{{"funding_tx_id", ""}},
                                    kUsdc).has_value());
    EXPECT_FALSE(warp_job_chia_send("FUNDING_CLAIM", json{{"funding_tx_id", 7}},
                                    kUsdc).has_value());
}

TEST(WarpSendGuardTest, TheTollFundingSendProtectsTheXchWallet) {
    const json state = {{"direction", "out"}, {"funding_tx_id", "toll-tx"}};
    const auto s = warp_job_chia_send("BURNING", state, kUsdc);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->xch);
    EXPECT_TRUE(s->cat_asset_id.empty());
    // Once the toll coin is pinned the send has confirmed; the bundle it
    // then pushes never passes through the wallet.
    EXPECT_FALSE(warp_job_chia_send("BURNING", json{{"direction", "out"}},
                                    kUsdc).has_value());
}

TEST(WarpSendGuardTest, ACatSpendProtectsItsCatWalletAndXch) {
    const json state = {
        {"burn_tx_id", "burn-tx-1"},
        {"asset", "milliETH"},
        {"asset_fingerprint",
         "v1:0xf2d5d8ec69e2faed5eb4de90749c87ee314a4b12:3:3:" + kMilli},
    };
    const auto s = warp_job_chia_send("BURN_SENT", state, kUsdc);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->xch);                 // a configured fee is paid in XCH
    EXPECT_EQ(s->cat_asset_id, kMilli);
}

TEST(WarpSendGuardTest, AnUnstampedCatSpendIsTheBridgeAsset) {
    const auto s = warp_job_chia_send("BURN_SENT", json{{"burn_tx_id", "x"}},
                                      kUsdc);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->cat_asset_id, kUsdc);
    // A malformed fingerprint falls back the same way.
    const auto t = warp_job_chia_send(
        "BURN_SENT", json{{"burn_tx_id", "x"}, {"asset_fingerprint", "v1:oops"}},
        kUsdc);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->cat_asset_id, kUsdc);
}

TEST(WarpSendGuardTest, AFailedJobIsReadByWhereItFailedFrom) {
    // Job 4 after its two-hour timeout: FAILED, marker kept, slot kept --
    // and the send it recorded is still whatever it was.
    const json state = {{"failed_from", "FUNDING_CLAIM"},
                        {"funding_tx_id", "0xa7f5"}};
    const auto s = warp_job_chia_send("FAILED", state, kUsdc);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->xch);
    const json burn = {{"failed_from", "BURN_SENT"}, {"burn_tx_id", "x"}};
    const auto b = warp_job_chia_send("FAILED", burn, kUsdc);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->cat_asset_id, kUsdc);
    // Failed from a state with no wallet send, or with no record of where.
    EXPECT_FALSE(warp_job_chia_send(
        "FAILED", json{{"failed_from", "BRIDGING"}, {"funding_tx_id", "x"}},
        kUsdc).has_value());
    EXPECT_FALSE(warp_job_chia_send("FAILED", json{{"funding_tx_id", "x"}},
                                    kUsdc).has_value());
}

TEST(WarpSendGuardTest, AReplacedSendKeepsProtectionInEveryActiveStatus) {
    // Orphan funding A replaced by B; A landed first and the job moved on
    // while B is still confirming in the wallet.
    const json state = {{"funding_tx_id", "b"},
                        {"funding_tx_id_prior", json::array({"a"})}};
    for (const char* st : {"CLAIM_FUNDED", "COLLECTING_SIGS", "CLAIMING",
                           "FAILED"}) {
        const auto s = warp_job_chia_send(st, state, kUsdc);
        ASSERT_TRUE(s.has_value()) << st;
        EXPECT_TRUE(s->xch) << st;
    }
    // A replaced cat_spend protects its CAT wallet too, in any status.
    const json burn = {{"burn_tx_id", "b"},
                       {"burn_tx_id_prior", json::array({"a"})}};
    const auto b = warp_job_chia_send("BURNING", burn, kUsdc);
    ASSERT_TRUE(b.has_value());
    EXPECT_TRUE(b->xch);
    EXPECT_EQ(b->cat_asset_id, kUsdc);
    // A retained funding attempt protects XCH even with no current marker
    // (Retry cleared it; a gate or a refusal left no replacement yet).
    const auto r = warp_job_chia_send(
        "FUNDING_CLAIM", json{{"funding_tx_id_prior", json::array({"a"})}}, kUsdc);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->xch);
    EXPECT_TRUE(warp_job_chia_send(
        "CLAIMING", json{{"funding_tx_id_prior", json::array({"a"})}}, kUsdc)
                    .has_value());
    // No replacement, or an empty prior list: the status rules.
    EXPECT_FALSE(warp_job_chia_send(
        "CLAIMING", json{{"funding_tx_id", "b"}}, kUsdc).has_value());
    EXPECT_FALSE(warp_job_chia_send(
        "CLAIMING", json{{"funding_tx_id", "b"},
                         {"funding_tx_id_prior", json::array()}}, kUsdc)
                     .has_value());
}

TEST(WarpSendGuardTest, RetiredAttemptsProvenDeadProtectNothing) {
    // Every retired attempt proven dead: nothing can still land, so the
    // wallet is free for the engine's cleanup again.
    const json all_dead = {{"funding_tx_id_prior", json::array({"a", "b"})},
                           {"funding_tx_id_prior_dead", json::array({"a", "b"})}};
    EXPECT_FALSE(warp_job_chia_send("CLAIMING", all_dead, kUsdc).has_value());
    // One of them not proven dead: protected.
    const json one_live = {{"funding_tx_id_prior", json::array({"a", "b"})},
                           {"funding_tx_id_prior_dead", json::array({"a"})}};
    const auto s = warp_job_chia_send("CLAIMING", one_live, kUsdc);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->xch);
    // The current marker still rules in its own status.
    const json dead_with_marker = {{"funding_tx_id", "c"},
                                   {"funding_tx_id_prior", json::array({"a"})},
                                   {"funding_tx_id_prior_dead", json::array({"a"})}};
    EXPECT_TRUE(warp_job_chia_send("FUNDING_CLAIM", dead_with_marker, kUsdc)
                    .has_value());
    EXPECT_FALSE(warp_job_chia_send("CLAIMING", dead_with_marker, kUsdc)
                     .has_value());
}

TEST(WarpSendGuardTest, ASendTheWalletDoesNotHoldProtectsNothing) {
    // The GUI's last judgement found no wallet record: nothing to protect.
    // The flag follows the record (an ORPHAN then SHALLOW with the record
    // back reads false again), and only a boolean counts.
    const json absent = {{"funding_tx_id", "x"}, {"funding_tx_id_wallet_absent", true}};
    EXPECT_FALSE(warp_job_chia_send("FUNDING_CLAIM", absent, kUsdc).has_value());
    const json failed = {{"failed_from", "FUNDING_CLAIM"}, {"funding_tx_id", "x"},
                         {"funding_tx_id_wallet_absent", true}};
    EXPECT_FALSE(warp_job_chia_send("FAILED", failed, kUsdc).has_value());
    const json held = {{"funding_tx_id", "x"}, {"funding_tx_id_wallet_absent", false}};
    EXPECT_TRUE(warp_job_chia_send("FUNDING_CLAIM", held, kUsdc).has_value());
    const json unknown = {{"funding_tx_id", "x"}, {"funding_tx_id_wallet_absent", nullptr}};
    EXPECT_TRUE(warp_job_chia_send("FUNDING_CLAIM", unknown, kUsdc).has_value());
    const json burn = {{"burn_tx_id", "y"}, {"burn_tx_id_wallet_absent", true}};
    EXPECT_FALSE(warp_job_chia_send("BURN_SENT", burn, kUsdc).has_value());
    const json odd = {{"funding_tx_id", "x"}, {"funding_tx_id_wallet_absent", "true"}};
    EXPECT_TRUE(warp_job_chia_send("FUNDING_CLAIM", odd, kUsdc).has_value())
        << "a non-boolean is not evidence";
    // A retained live prior still protects regardless.
    const json prior = {{"funding_tx_id", "x"}, {"funding_tx_id_wallet_absent", true},
                        {"funding_tx_id_prior", json::array({"a"})}};
    EXPECT_TRUE(warp_job_chia_send("FUNDING_CLAIM", prior, kUsdc).has_value());
}

TEST(WarpSendGuardTest, OtherStatusesHoldNoWalletSend) {
    const json state = {{"funding_tx_id", "x"}, {"burn_tx_id", "y"}};
    for (const char* st : {"AWAITING_DEPOSIT", "APPROVING", "BRIDGING",
                           "CLAIM_FUNDED", "COLLECTING_SIGS", "CLAIMING",
                           "UNWRAP_CHECKS", "COLLECTING_EVM_SIGS", "RELAYING",
                           "AWAITING_EXTERNAL_RELAY", "COMPLETED", "FAILED"}) {
        EXPECT_FALSE(warp_job_chia_send(st, state, kUsdc).has_value()) << st;
    }
}

TEST(WarpSendGuardTest, AFingerprintIsReadCaseInsensitively) {
    std::string upper = kMilli;
    for (auto& c : upper) c = static_cast<char>(std::toupper(c));
    EXPECT_EQ(fingerprint_asset_id("v1:0xabc:3:3:" + upper), kMilli);
    EXPECT_EQ(fingerprint_asset_id(""), "");
    EXPECT_EQ(fingerprint_asset_id("v1:0xabc:3:3:" + kMilli.substr(1)), "");
}

TEST(WarpSendGuardTest, ASubmittedTakeProtectsXchAndTheBoughtCat) {
    const auto s = warp_take_chia_send("submitted", kMilli);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->xch);
    EXPECT_EQ(s->cat_asset_id, kMilli);
    EXPECT_TRUE(warp_take_chia_send("submitting", kMilli).has_value());
    EXPECT_TRUE(warp_take_chia_send("unknown", kMilli).has_value());
    EXPECT_FALSE(warp_take_chia_send("confirmed", kMilli).has_value());
    EXPECT_FALSE(warp_take_chia_send("failed", kMilli).has_value());
}

TEST(WarpSendGuardTest, TheGuardAccumulatesAndFailsClosed) {
    WarpSendGuard g;
    EXPECT_FALSE(g.any());
    g.add(*warp_take_chia_send("submitted", kMilli));
    g.add(*warp_job_chia_send("BURN_SENT", json{{"burn_tx_id", "x"}}, kUsdc));
    EXPECT_TRUE(g.any());
    EXPECT_TRUE(g.xch);
    EXPECT_EQ(g.cat_asset_ids.size(), 2u);
    EXPECT_TRUE(g.cat_asset_ids.count(kMilli) && g.cat_asset_ids.count(kUsdc));

    WarpSendGuard unreadable;
    unreadable.readable = false;
    EXPECT_TRUE(unreadable.any());
}

}  // namespace
