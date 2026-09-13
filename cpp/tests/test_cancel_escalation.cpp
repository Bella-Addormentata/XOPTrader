// test_cancel_escalation.cpp -- [S14 2026-09-13] cancel_pending until the
// wallet says terminal, proof-gated escalation of cancels the chain proves
// never landed, and one set for stuck detection and its remedy.
//
// Every test calls the production functions in
// xop/execution/cancel_escalation.hpp -- and CoinManager::compute_coin_name
// for the chain vectors.  Nothing here re-implements a formula.

#include <gtest/gtest.h>

#include <xop/execution/cancel_escalation.hpp>
#include <xop/execution/coin_manager.hpp>
#include <xop/types.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace ex = xop::execution;

using Verdict = ex::CancelEscalationVerdict;
using Wallet  = ex::WalletCancelState;
using Coins   = ex::CoinProof;

// Three mainnet coins and their ids, checked against the node's coin_record
// table.  Vectors 2 and 3 hash correctly only WITH the CLVM 0x00 sign byte.
const std::string kParent1 =
    "a33370d3e7cca34cdf5d874fc9f8a1613c0bd1757488fc7f50e0782b53433c07";
const std::string kPuzzle1 =
    "9a26d88c6098b98d3e5e1f5edfaac87597b99c81fd04520a6e4681d70355333c";
constexpr std::uint64_t kAmount1 = 156'928'787'204ULL;
const std::string kName1 =
    "bcc82115cafeb2b1649a94535013311ab1f048f223a7cda23204df24fb17f489";

const std::string kParent2 =
    "e6a2346ad2e565ca854b3ea167be104b3221d14b2c6231aec32a0d8f25588161";
const std::string kPuzzle2 =
    "8eff4f6f186dd78d3533ed5e205f6c1f2d96a021dd074d8bed5b0da13a82a2e0";
constexpr std::uint64_t kAmount2 = 157ULL;
const std::string kName2 =
    "ba9098a89784f865992255f4ae60b8a4dd95fde9a7d4ab3de127c88c4b07418d";

const std::string kParent3 =
    "cef12f7f75e3f385c41c476d418581103dc3f2870e508b77c0e622c04e77edc3";
const std::string kPuzzle3 =
    "53f57209c44acd6f57d2f44ecfec412c9749d0a9aba48585444a54365cadfdc8";
constexpr std::uint64_t kAmount3 = 999'971'495'826ULL;
const std::string kName3 =
    "374accaec20e7d829f251323e2d729930fb2deebeb05f13935699c1037236941";

ex::CancelEscalationTrack track_at(std::uint64_t anchor,
                                   std::uint32_t escalations = 0,
                                   std::uint64_t retry_after = 0)
{
    ex::CancelEscalationTrack track;
    track.anchor_block      = anchor;
    track.escalations       = escalations;
    track.retry_after_block = retry_after;
    return track;
}

Verdict verdict(std::uint64_t                     block,
                const ex::CancelEscalationTrack&  track,
                Wallet                            wallet,
                Coins                             coins,
                const ex::CancelEscalationParams& params = ex::CancelEscalationParams{})
{
    ex::CancelEscalationInput in;
    in.current_block = block;
    in.track         = track;
    in.wallet        = wallet;
    in.coins         = coins;
    in.params        = params;
    return ex::decide_cancel_escalation(in);
}

// The same adapter shape the engine uses: any throw is "no name".
std::string chain_name(const ex::CoinRef& ref)
{
    try {
        return ex::CoinManager::compute_coin_name(
            ref.parent_hex, ref.puzzle_hash_hex,
            static_cast<xop::Mojo>(ref.amount));
    } catch (const std::exception&) {
        return {};
    }
}

std::string upper(std::string text)
{
    for (auto& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

nlohmann::json coin_json(const std::string& parent,
                         const std::string& puzzle,
                         std::uint64_t      amount)
{
    nlohmann::json coin = nlohmann::json::object();
    coin["parent_coin_info"] = parent;
    coin["puzzle_hash"]      = puzzle;
    coin["amount"]           = amount;
    return coin;
}

// The node's get_coin_records_by_names record shape.
nlohmann::json coin_record(const nlohmann::json& coin,
                           std::uint64_t         spent_block_index,
                           bool                  spent)
{
    nlohmann::json record = nlohmann::json::object();
    record["coin"]                  = coin;
    record["confirmed_block_index"] = 9'224'000U;
    record["spent_block_index"]     = spent_block_index;
    record["spent"]                 = spent;
    record["coinbase"]              = false;
    record["timestamp"]             = 1'756'600'000U;
    return record;
}

ex::UnresolvedAlertEntry alert_entry(const std::string&   offer_id,
                                     const std::string&   pair_name,
                                     std::uint32_t        escalations,
                                     ex::UnresolvedReason reason)
{
    ex::UnresolvedAlertEntry entry;
    entry.offer_id    = offer_id;
    entry.pair_name   = pair_name;
    entry.escalations = escalations;
    entry.reason      = reason;
    return entry;
}

}  // namespace

// ===========================================================================
// decide_cancel_escalation
// ===========================================================================

TEST(CancelEscalation, FirstSightingOnlyAnchors)
{
    EXPECT_EQ(verdict(9'285'386, ex::CancelEscalationTrack{},
                      Wallet::PendingCancel, Coins::AllUnspent),
              Verdict::AnchorNow);
}

TEST(CancelEscalation, WaitsUntilTheWindowHasElapsed)
{
    const auto track = track_at(1000);
    EXPECT_EQ(verdict(1095, track, Wallet::PendingCancel, Coins::AllUnspent),
              Verdict::Wait);
    EXPECT_EQ(verdict(1096, track, Wallet::PendingCancel, Coins::AllUnspent),
              Verdict::Escalate);
}

// The live XCH/BYC shape: submitted at 9224185, still PENDING_CANCEL at
// 9285386 with every maker coin unspent on-chain.
TEST(CancelEscalation, StrandedPendingCancelWithUnspentMakerCoinsEscalates)
{
    EXPECT_EQ(verdict(9'285'386, track_at(9'224'185), Wallet::PendingCancel,
                      Coins::AllUnspent),
              Verdict::Escalate);
}

TEST(CancelEscalation, LiveWalletRecordWithUnspentCoinsEscalates)
{
    EXPECT_EQ(verdict(1096, track_at(1000), Wallet::PendingAccept,
                      Coins::AllUnspent),
              Verdict::Escalate);
}

// [review] A cancel_pending row whose wallet record says PENDING_ACCEPT has
// nothing in flight to replace.  Waiting the full window would leave it
// takeable for 96 blocks where the pre-change engine re-cancelled it on the
// first heartbeat.
TEST(CancelEscalation, PendingAcceptEscalatesAfterTheShortWindow)
{
    const auto track = track_at(1000);
    EXPECT_EQ(verdict(1007, track, Wallet::PendingAccept, Coins::AllUnspent),
              Verdict::Wait);
    EXPECT_EQ(verdict(1008, track, Wallet::PendingAccept, Coins::AllUnspent),
              Verdict::Escalate);
    EXPECT_EQ(verdict(1008, track, Wallet::PendingCancel, Coins::AllUnspent),
              Verdict::Wait)
        << "a PENDING_CANCEL may still have a spend in flight";
}

TEST(CancelEscalation, ASpentMakerCoinMeansResolvingNotEscalate)
{
    const auto track = track_at(1000);
    EXPECT_EQ(verdict(1096, track, Wallet::PendingCancel, Coins::SomeSpent),
              Verdict::Resolving);
    EXPECT_EQ(verdict(1096, track, Wallet::PendingAccept, Coins::SomeSpent),
              Verdict::Resolving);
}

TEST(CancelEscalation, NoCoinEvidenceNeverPaysAFee)
{
    const auto track = track_at(1000);
    EXPECT_EQ(verdict(1096, track, Wallet::PendingCancel, Coins::Unknown),
              Verdict::NoProof);
    EXPECT_EQ(verdict(1096, track, Wallet::Unknown, Coins::AllUnspent),
              Verdict::NoProof);
}

TEST(CancelEscalation, WalletTerminalOrFillHandsOff)
{
    const auto track = track_at(1000);
    EXPECT_EQ(verdict(1096, track, Wallet::Cancelled, Coins::AllUnspent),
              Verdict::ResolvedByWallet);
    EXPECT_EQ(verdict(1096, track, Wallet::Failed, Coins::AllUnspent),
              Verdict::ResolvedByWallet);
    EXPECT_EQ(verdict(1096, track, Wallet::Confirmed, Coins::AllUnspent),
              Verdict::ResolvedByWallet);
    EXPECT_EQ(verdict(1096, track, Wallet::PendingConfirm, Coins::AllUnspent),
              Verdict::Resolving);
}

TEST(CancelEscalation, CapIsExhaustedNotAnotherFee)
{
    EXPECT_EQ(verdict(1096, track_at(1000, 2), Wallet::PendingAccept,
                      Coins::AllUnspent),
              Verdict::Escalate);
    EXPECT_EQ(verdict(1096, track_at(1000, 3), Wallet::PendingAccept,
                      Coins::AllUnspent),
              Verdict::Exhausted);
}

TEST(CancelEscalation, RetryBackoffDefersTheProbe)
{
    const auto track = track_at(1000, 0, 2008);
    EXPECT_FALSE(ex::escalation_probe_due(track, 2000));
    EXPECT_EQ(verdict(2000, track, Wallet::PendingAccept, Coins::AllUnspent),
              Verdict::Wait);
    EXPECT_TRUE(ex::escalation_probe_due(track, 2008));
    EXPECT_EQ(verdict(2008, track, Wallet::PendingAccept, Coins::AllUnspent),
              Verdict::Escalate);
}

// The engine reads the strategy.cancel_escalation_* keys; the decision must
// follow the configured values, not the defaults.
TEST(CancelEscalation, ConfiguredParametersAreHonoured)
{
    ex::CancelEscalationParams params;
    params.window_blocks   = 10;
    params.retry_blocks    = 2;
    params.max_escalations = 1;
    EXPECT_EQ(verdict(101, track_at(100), Wallet::PendingAccept,
                      Coins::AllUnspent, params),
              Verdict::Wait);
    EXPECT_EQ(verdict(102, track_at(100), Wallet::PendingAccept,
                      Coins::AllUnspent, params),
              Verdict::Escalate);
    EXPECT_EQ(verdict(109, track_at(100), Wallet::PendingCancel,
                      Coins::AllUnspent, params),
              Verdict::Wait);
    EXPECT_EQ(verdict(110, track_at(100), Wallet::PendingCancel,
                      Coins::AllUnspent, params),
              Verdict::Escalate);
    EXPECT_EQ(verdict(110, track_at(100, 1), Wallet::PendingCancel,
                      Coins::AllUnspent, params),
              Verdict::Exhausted);
}

TEST(CancelEscalation, UnresolvedAlertFiresOnceAfterTheFullLadder)
{
    auto track = track_at(1000);
    EXPECT_FALSE(ex::unresolved_alert_due(track, 1383));
    EXPECT_TRUE(ex::unresolved_alert_due(track, 1384));
    track.alerted = true;
    EXPECT_FALSE(ex::unresolved_alert_due(track, 1384));
}

// [review] A Resolving or NoProof offer used to be re-probed every 8 blocks
// forever.
TEST(CancelEscalation, IdleProbesBackOffUpToTheWindow)
{
    EXPECT_EQ(ex::escalation_backoff_blocks(0), 8u);
    EXPECT_EQ(ex::escalation_backoff_blocks(1), 16u);
    EXPECT_EQ(ex::escalation_backoff_blocks(2), 32u);
    EXPECT_EQ(ex::escalation_backoff_blocks(3), 64u);
    EXPECT_EQ(ex::escalation_backoff_blocks(4), 96u);
    EXPECT_EQ(ex::escalation_backoff_blocks(
                  std::numeric_limits<std::uint32_t>::max()),
              96u);
}

// ===========================================================================
// escalation_fee_mojos
// ===========================================================================

TEST(CancelEscalationFee, RaisesTheFloorByTheReplacementIncrementAndIsCapped)
{
    const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(ex::escalation_fee_mojos(9'719, 0, 0), 10'009'719u);
    EXPECT_EQ(ex::escalation_fee_mojos(0, 0, 0), 10'000'000u);
    // The cancel being replaced may have paid up to twice the dynamic fee
    // (emergency_cancel): outbid that, not the base.
    EXPECT_EQ(ex::escalation_fee_mojos(9'719, 0, 20'000'000), 30'000'000u);
    EXPECT_EQ(ex::escalation_fee_mojos(95'000'000, 0, 0), 100'000'000u);
    EXPECT_EQ(ex::escalation_fee_mojos(kMax, 0, 0), 100'000'000u);
    EXPECT_EQ(ex::escalation_fee_mojos(0, 0, kMax), 100'000'000u);
}

// [review] The adaptive base can FALL between attempts.  A ladder built on
// base + n x step would then bid below the previous attempt and be refused
// as MEMPOOL_CONFLICT while the RPC still reports success.
TEST(CancelEscalationFee, ABaseThatDropsBetweenAttemptsStillOutbidsThePreviousAttempt)
{
    const std::uint64_t first = ex::escalation_fee_mojos(9'719, 0, 0);
    ASSERT_EQ(first, 10'009'719u);
    const std::uint64_t second = ex::escalation_fee_mojos(5'000, first, 10'000);
    EXPECT_EQ(second, 20'009'719u);
    EXPECT_GE(second, first + ex::kMempoolMinFeeIncreaseMojos);
    const std::uint64_t third = ex::escalation_fee_mojos(5'000, second, 10'000);
    EXPECT_EQ(third, 30'009'719u);
}

// ===========================================================================
// Chain proof
// ===========================================================================

TEST(CoinProof, SpentBlockIndexOrSpentFlagMeansSpent)
{
    const auto c1 = coin_json(kParent1, kPuzzle1, kAmount1);
    const auto c2 = coin_json(kParent2, kPuzzle2, kAmount2);
    const std::vector<std::string> names{kName1, kName2};

    const std::vector<nlohmann::json> unspent{coin_record(c1, 0, false),
                                              coin_record(c2, 0, false)};
    EXPECT_EQ(ex::classify_coin_records(names, unspent, chain_name),
              Coins::AllUnspent);

    const std::vector<nlohmann::json> one_spent{coin_record(c1, 9'224'500, true),
                                                coin_record(c2, 0, false)};
    EXPECT_EQ(ex::classify_coin_records(names, one_spent, chain_name),
              Coins::SomeSpent);

    nlohmann::json flag_only = nlohmann::json::object();
    flag_only["coin"]  = c1;
    flag_only["spent"] = true;
    const std::vector<nlohmann::json> flagged{flag_only, coin_record(c2, 0, false)};
    EXPECT_EQ(ex::classify_coin_records(names, flagged, chain_name),
              Coins::SomeSpent);

    EXPECT_EQ(ex::classify_coin_records(names, std::vector<nlohmann::json>{},
                                        chain_name),
              Coins::Unknown);

    nlohmann::json no_coin = nlohmann::json::object();
    no_coin["spent_block_index"] = 0U;
    const std::vector<nlohmann::json> coinless{no_coin, no_coin};
    EXPECT_EQ(ex::classify_coin_records(names, coinless, chain_name),
              Coins::Unknown);
}

// [review] get_coin_records_by_names silently omits names the node does not
// find.  Two requested, one returned unspent, is NOT proof that both are.
TEST(CoinProof, APartialNodeAnswerIsUnknownNotAllUnspent)
{
    const auto c1 = coin_json(kParent1, kPuzzle1, kAmount1);
    const auto c3 = coin_json(kParent3, kPuzzle3, kAmount3);
    const std::vector<std::string> names{kName1, kName2};

    const std::vector<nlohmann::json> partial{coin_record(c1, 0, false)};
    EXPECT_EQ(ex::classify_coin_records(names, partial, chain_name),
              Coins::Unknown);

    // A record for a coin nobody asked about is no evidence about the ones
    // that were asked about.
    const std::vector<nlohmann::json> stranger{coin_record(c1, 0, false),
                                               coin_record(c3, 0, false)};
    EXPECT_EQ(ex::classify_coin_records(names, stranger, chain_name),
              Coins::Unknown);
}

TEST(CoinsOfInterest, ParsesTheWalletRecordAndFailsClosed)
{
    const auto valid_upper =
        coin_json("0x" + upper(kParent1), "0x" + upper(kPuzzle1), kAmount1);
    const auto valid2 = coin_json(kParent2, kPuzzle2, kAmount2);

    nlohmann::json record = nlohmann::json::object();
    record["trade_id"]          = "0x17840f2180f0ce45";
    record["status"]            = "PENDING_CANCEL";
    record["coins_of_interest"] = nlohmann::json::array({valid_upper, valid2});

    const auto refs = ex::parse_coins_of_interest(record);
    ASSERT_EQ(refs.size(), 2u);
    EXPECT_EQ(refs[0].parent_hex, kParent1);
    EXPECT_EQ(refs[0].puzzle_hash_hex, kPuzzle1);
    EXPECT_EQ(refs[0].amount, kAmount1);
    EXPECT_EQ(refs[1].parent_hex, kParent2);
    EXPECT_EQ(refs[1].amount, kAmount2);

    // [review] Each malformed shape sits BESIDE a valid coin.  Alone, a
    // parser that skipped bad elements would also return nothing, and this
    // test could not tell the two apart.
    const auto beside_valid = [&valid2](const nlohmann::json& bad) {
        nlohmann::json r = nlohmann::json::object();
        r["coins_of_interest"] = nlohmann::json::array({valid2, bad});
        return r;
    };

    auto bad_amount = coin_json(kParent1, kPuzzle1, kAmount1);
    bad_amount["amount"] = "abc";
    auto no_puzzle = coin_json(kParent1, kPuzzle1, kAmount1);
    no_puzzle.erase("puzzle_hash");
    const auto short_parent =
        coin_json(kParent1.substr(0, 62), kPuzzle1, kAmount1);
    auto no_amount = coin_json(kParent1, kPuzzle1, kAmount1);
    no_amount.erase("amount");
    auto negative = coin_json(kParent1, kPuzzle1, kAmount1);
    negative["amount"] = -1;

    EXPECT_TRUE(ex::parse_coins_of_interest(beside_valid(bad_amount)).empty());
    EXPECT_TRUE(ex::parse_coins_of_interest(beside_valid(no_puzzle)).empty());
    EXPECT_TRUE(ex::parse_coins_of_interest(beside_valid(short_parent)).empty());
    EXPECT_TRUE(ex::parse_coins_of_interest(beside_valid(no_amount)).empty());
    EXPECT_TRUE(ex::parse_coins_of_interest(beside_valid(negative)).empty());

    nlohmann::json not_an_array = nlohmann::json::object();
    not_an_array["coins_of_interest"] = "nope";
    EXPECT_TRUE(ex::parse_coins_of_interest(not_an_array).empty());
    EXPECT_TRUE(ex::parse_coins_of_interest(nlohmann::json::object()).empty());
}

// [review] compute_coin_name takes Mojo (int64).  A larger amount would be
// cast negative, hashed as the empty encoding, and name a different coin
// without throwing.  Kept a single-element record on purpose: this pins the
// range guard itself, not the fail-closed loop above.
TEST(CoinsOfInterest, AnAmountBeyondInt64IsRejected)
{
    auto huge = coin_json(kParent1, kPuzzle1, kAmount1);
    huge["amount"] = std::uint64_t{9'223'372'036'854'775'808ULL};
    nlohmann::json record = nlohmann::json::object();
    record["coins_of_interest"] = nlohmann::json::array({huge});
    EXPECT_TRUE(ex::parse_coins_of_interest(record).empty());

    auto largest = coin_json(kParent1, kPuzzle1, kAmount1);
    largest["amount"] = ex::kMaxHashableCoinAmount;
    nlohmann::json ok = nlohmann::json::object();
    ok["coins_of_interest"] = nlohmann::json::array({largest});
    EXPECT_EQ(ex::parse_coins_of_interest(ok).size(), 1u);
}

TEST(CoinsOfInterest, NamesMatchTheChain)
{
    nlohmann::json record = nlohmann::json::object();
    record["coins_of_interest"] = nlohmann::json::array({
        coin_json("0x" + upper(kParent1), "0x" + kPuzzle1, kAmount1),
        coin_json(kParent2, kPuzzle2, kAmount2),
        coin_json(kParent3, kPuzzle3, kAmount3)});

    const auto names =
        ex::coin_names_for(ex::parse_coins_of_interest(record), chain_name);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], kName1);
    EXPECT_EQ(names[1], kName2) << "157 needs the CLVM 0x00 sign byte";
    EXPECT_EQ(names[2], kName3) << "999971495826 needs the CLVM 0x00 sign byte";

    const auto fails_on_second = [](const ex::CoinRef& ref) {
        return ref.amount == kAmount2 ? std::string{} : chain_name(ref);
    };
    EXPECT_TRUE(ex::coin_names_for(ex::parse_coins_of_interest(record),
                                   fails_on_second).empty())
        << "one name that cannot be computed voids the proof";
}

TEST(WalletCancelState, FromIntOrName)
{
    EXPECT_EQ(ex::wallet_cancel_state_from_record(nlohmann::json{{"status", 2}}),
              Wallet::PendingCancel);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(
                  nlohmann::json::parse(R"({"status": 2})")),
              Wallet::PendingCancel);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(
                  nlohmann::json{{"status", "PENDING_CANCEL"}}),
              Wallet::PendingCancel);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(
                  nlohmann::json{{"status", "CANCELLED"}}),
              Wallet::Cancelled);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(nlohmann::json{{"status", 7}}),
              Wallet::Unknown);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(nlohmann::json{{"status", -1}}),
              Wallet::Unknown);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(nlohmann::json{{"status", 2.0}}),
              Wallet::Unknown);
    EXPECT_EQ(ex::wallet_cancel_state_from_record(nlohmann::json::object()),
              Wallet::Unknown);
}

// ===========================================================================
// One set for stuck detection and its remedy
// ===========================================================================

TEST(ForcedCancelCandidate, CancelPendingIsNeverAForcedCancelCandidate)
{
    EXPECT_FALSE(ex::is_forced_cancel_candidate(true, 9'284'141, 9'285'386, 830));
    EXPECT_TRUE(ex::is_forced_cancel_candidate(false, 9'284'141, 9'285'386, 830));
}

TEST(ForcedCancelCandidate, BoundaryMatchesCancelStale)
{
    EXPECT_TRUE(ex::is_forced_cancel_candidate(false, 100, 930, 830));
    EXPECT_FALSE(ex::is_forced_cancel_candidate(false, 100, 929, 830));
    EXPECT_FALSE(ex::is_forced_cancel_candidate(false, 1000, 999, 830));
    EXPECT_TRUE(ex::is_forced_cancel_candidate(false, 0, 830, 830));
}

TEST(StuckSummaryLog, LogsOnChangeOrIntervalNotEveryBlock)
{
    EXPECT_FALSE(ex::stuck_summary_log_due(5, 5, 100, 101));
    EXPECT_TRUE(ex::stuck_summary_log_due(5, 4, 100, 101));
    EXPECT_TRUE(ex::stuck_summary_log_due(5, 5, 100, 132));
    EXPECT_FALSE(ex::stuck_summary_log_due(0, 0, 0, 10'000));
    EXPECT_TRUE(ex::stuck_summary_log_due(0, 5, 0, 1));
}

// ===========================================================================
// Boot
// ===========================================================================

TEST(StartupPendingCancel, MislabelledCancelledRowIsReopened)
{
    using Action = ex::StartupPendingCancelAction;
    EXPECT_EQ(ex::startup_pending_cancel_action(std::optional<std::string>{"cancelled"}),
              Action::ReopenMislabelled);
    EXPECT_EQ(ex::startup_pending_cancel_action(std::nullopt),
              Action::IgnoreNotOurs);
    EXPECT_EQ(ex::startup_pending_cancel_action(std::optional<std::string>{"pending"}),
              Action::MarkCancelPending);
    EXPECT_EQ(ex::startup_pending_cancel_action(
                  std::optional<std::string>{"cancel_pending"}),
              Action::AlreadyCancelPending);
    EXPECT_EQ(ex::startup_pending_cancel_action(std::optional<std::string>{"filled"}),
              Action::Inconsistent);
}

// [review] The defect was the wallet scan keeping PENDING_ACCEPT only
// (offer_manager.cpp startup_reconcile Phase 1).  The decision is extracted
// so it can be pinned and mutated; startup_reconcile calls it.
TEST(StartupScanBucket, PendingCancelRecordsAreCollected)
{
    using Bucket = ex::StartupScanBucket;
    EXPECT_EQ(ex::startup_scan_bucket(2, false), Bucket::PendingCancelObserved);
    EXPECT_EQ(ex::startup_scan_bucket(2, true), Bucket::PendingCancelObserved);
    EXPECT_EQ(ex::startup_scan_bucket(0, true), Bucket::KnownLive);
    EXPECT_EQ(ex::startup_scan_bucket(0, false), Bucket::OrphanCandidate);
    EXPECT_EQ(ex::startup_scan_bucket(3, true), Bucket::Ignore);
    EXPECT_EQ(ex::startup_scan_bucket(-1, false), Bucket::Ignore);
}

// ===========================================================================
// Alerts
// ===========================================================================

// [review] Eight stranded offers reach Exhausted in two sweeps one heartbeat
// apart.  The second batch, sent inside AlertManager's 60 s CRITICAL
// cooldown, was dropped while its offers were already marked alerted.
TEST(CancelUnresolvedAlert, ASecondBatchInsideTheCooldownIsHeldNotDropped)
{
    ex::UnresolvedAlertQueue queue;
    ASSERT_TRUE(queue.enqueue(alert_entry("0xaaa", "XCH/BYC", 3,
                                          ex::UnresolvedReason::StillTakeable)));
    const auto first = queue.take_due(1'000'000, ex::kMaxIdsPerCancelUnresolvedAlert);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].offer_id, "0xaaa");

    ASSERT_TRUE(queue.enqueue(alert_entry("0xbbb", "XCH/DBX", 3,
                                          ex::UnresolvedReason::StillTakeable)));
    EXPECT_FALSE(queue.enqueue(alert_entry("0xbbb", "XCH/DBX", 3,
                                           ex::UnresolvedReason::StillTakeable)))
        << "an offer is queued once";
    EXPECT_TRUE(queue.take_due(1'005'000, ex::kMaxIdsPerCancelUnresolvedAlert).empty())
        << "a send inside the cooldown would be swallowed";
    EXPECT_EQ(queue.size(), 1u) << "held, not dropped";

    const auto second = queue.take_due(1'065'000, ex::kMaxIdsPerCancelUnresolvedAlert);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(second[0].offer_id, "0xbbb");
    EXPECT_EQ(queue.size(), 0u);
}

TEST(CancelUnresolvedAlert, TheMessageNamesEveryOfferAndSaysWhetherItIsTakeable)
{
    const std::string takeable =
        "0x17840f2180f0ce45aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const std::string unverifiable =
        "0x1bdf0090bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    const std::vector<ex::UnresolvedAlertEntry> batch{
        alert_entry(takeable, "XCH/BYC", 3, ex::UnresolvedReason::StillTakeable),
        alert_entry(unverifiable, "XCH/DBX", 0, ex::UnresolvedReason::Unverifiable)};
    const std::string msg = ex::format_cancel_unresolved_alert(batch, 2);
    EXPECT_NE(msg.find(takeable), std::string::npos) << msg;
    EXPECT_NE(msg.find(unverifiable), std::string::npos) << msg;
    EXPECT_NE(msg.find("XCH/BYC"), std::string::npos) << msg;
    EXPECT_NE(msg.find("STILL TAKEABLE"), std::string::npos) << msg;
    EXPECT_NE(msg.find("unverifiable"), std::string::npos) << msg;
    EXPECT_NE(msg.find("3 escalated"), std::string::npos) << msg;
    EXPECT_NE(msg.find("+2 more"), std::string::npos) << msg;
}

// ===========================================================================
// Gates the sweep re-reads while it is suspended
// ===========================================================================

// [review, round 2] The dead man's switch latches watchdog_fired_ on its own
// thread, then sends a wallet-wide zero-fee cancel.  The re-check before the
// fee-bearing call read shutdown and Cancel All but not the watchdog, so a
// sweep suspended in an RPC could still pay -- and an escalated bundle that
// reaches the mempool first gets the watchdog's conflicting batch refused.
TEST(CancelEscalationGates, AFiredWatchdogStopsTheFeeBearingCall)
{
    ex::EscalationAsyncGates gates;
    EXPECT_FALSE(ex::escalation_must_yield(gates)) << "every gate open: the sweep may pay";

    gates.watchdog_fired = true;
    EXPECT_TRUE(ex::escalation_must_yield(gates))
        << "the dead man's switch fired while the sweep was suspended";

    gates = ex::EscalationAsyncGates{};
    gates.graceful_cancel_active = true;
    EXPECT_TRUE(ex::escalation_must_yield(gates)) << "shutdown is walking the book";

    gates = ex::EscalationAsyncGates{};
    gates.cancel_all_inflight = true;
    EXPECT_TRUE(ex::escalation_must_yield(gates)) << "an operator Cancel All is in flight";
}
