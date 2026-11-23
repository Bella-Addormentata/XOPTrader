// test_coin_fence.cpp -- [OPTIONS-FENCE] the options sub-account's fence.
//
// rpc/coin_fence.hpp decides; ChiaWalletRPC applies it on every call that
// makes the wallet select coins.  The golden puzzle hashes come from
// chia_wallet_sdk (the options book's own library): Address.decode for the
// options address, cat_puzzle_hash for its CAT wrappings.

#include "xop/execution/coin_manager.hpp"
#include "xop/rpc/chia_rpc.hpp"
#include "xop/rpc/coin_fence.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>

namespace rpc = xop::rpc;
namespace ex  = xop::execution;
using json    = nlohmann::json;

namespace {

// The live options address (config.yaml options.wallet.address).
constexpr const char* kAddress = "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2";
constexpr const char* kPuzzle  = "b18cdf6985b3a061e0c89b671d93f334aedf5be86ce1e79e2297d55f2528cc55";
constexpr const char* kUsdc    = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d";
constexpr const char* kByc     = "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac";

using Clock = rpc::CoinFence::Clock;

}  // namespace

// -- addresses and puzzle hashes ----------------------------------------------

TEST(CoinFenceAddress, DecodesTheOptionsAddressToItsPuzzleHash)
{
    EXPECT_EQ(rpc::decode_chia_address(kAddress, "xch").value_or(""), kPuzzle);
    std::string upper = kAddress;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    EXPECT_EQ(rpc::decode_chia_address(upper, "xch").value_or(""), kPuzzle);
}

TEST(CoinFenceAddress, RefusesWrongPrefixBadChecksumAndMixedCase)
{
    EXPECT_FALSE(rpc::decode_chia_address(kAddress, "txch"));
    std::string bad = kAddress;
    bad.back() = bad.back() == 'q' ? 'p' : 'q';
    EXPECT_FALSE(rpc::decode_chia_address(bad, "xch"));
    std::string mixed = kAddress;
    mixed[5] = static_cast<char>(std::toupper(static_cast<unsigned char>(mixed[5])));
    EXPECT_FALSE(rpc::decode_chia_address(mixed, "xch"));
    EXPECT_FALSE(rpc::decode_chia_address("", "xch"));
    EXPECT_FALSE(rpc::decode_chia_address("xch1", "xch"));
}

TEST(CoinFenceAddress, CatWrappingsOfTheOptionsAddressMatchTheSdk)
{
    EXPECT_EQ(ex::CoinManager::cat_puzzle_hash(kUsdc, kPuzzle).value_or(""),
              "a93eb5a3508a01554f6e6669c320fb72cb082295fd7f3c455e25bb5ac8d784f0");
    EXPECT_EQ(ex::CoinManager::cat_puzzle_hash(std::string{"0x"} + kByc, std::string{"0x"} + kPuzzle)
                  .value_or(""),
              "0b105798b69d34b73d98a56a7920cda12e968c5dfcf789953e71f60228b9ee44");
    EXPECT_FALSE(ex::CoinManager::cat_puzzle_hash("xch", kPuzzle));
    EXPECT_FALSE(ex::CoinManager::cat_puzzle_hash(kUsdc, "abc"));
    // Around OFFER_MOD it is still the settlement puzzle (test_fill_proof pins DBX).
    EXPECT_EQ(ex::CoinManager::settlement_puzzle_hash(kUsdc).value_or(""),
              "7dd46607faa82c963498f4a270823c9db4ca3c21d76ac4c8773c0b2cbfd8462a");
}

// -- exclusion lists ----------------------------------------------------------

TEST(CoinFenceExclusions, MergeKeepsTheCallersListAndAddsNoDuplicates)
{
    const auto merged = rpc::merge_exclusions({"0xAA", "bb"}, {"aa", "0xcc", "CC"});
    EXPECT_EQ(merged, (std::vector<std::string>{"0xAA", "bb", "0xcc"}));
}

TEST(CoinFenceExclusions, AddExclusionsWritesTheTopLevelCoinSelectionKey)
{
    json payload = {{"offer", "offer1..."}, {"fee", 5}};
    rpc::ChiaWalletRPC::add_exclusions(payload, {});
    EXPECT_FALSE(payload.contains("excluded_coin_ids")) << "nothing to exclude: payload unchanged";

    rpc::ChiaWalletRPC::add_exclusions(payload, {"aa", "0xbb"});
    EXPECT_EQ(payload["excluded_coin_ids"], json::array({"0xaa", "0xbb"}));

    // A caller's own exclusions (the GAS-RESERVE coins) stay.
    json offer = rpc::ChiaWalletRPC::build_create_offer_payload(
        json{{"1", -5}, {"2", 7}}, 0, false, std::nullopt, std::nullopt, {"0xcc"});
    rpc::ChiaWalletRPC::add_exclusions(offer, {"aa", "cc"});
    EXPECT_EQ(offer["excluded_coin_ids"], json::array({"0xcc", "0xaa"}));
}

TEST(CoinFenceExclusions, FindsFencedCoinsAnOfferLocked)
{
    const std::set<std::string> fenced{kPuzzle};
    const json clean = {{"trade_record",
                         {{"trade_id", "0x01"},
                          {"coins_of_interest",
                           json::array({{{"parent_coin_info", "0x11"},
                                         {"puzzle_hash", "0x3fd2"},
                                         {"amount", 5}}})}}}};
    EXPECT_EQ(rpc::fenced_coins_in_trade_record(clean, fenced), std::vector<std::string>{});

    json dirty = clean;
    dirty["trade_record"]["coins_of_interest"].push_back(
        {{"parent_coin_info", "0x22"}, {"puzzle_hash", std::string{"0x"} + kPuzzle}, {"amount", 7}});
    const auto hits = rpc::fenced_coins_in_trade_record(dirty, fenced);
    ASSERT_TRUE(hits);
    EXPECT_EQ(*hits, std::vector<std::string>{"0x22/7"});

    // A record that does not list its coins cannot be cleared.
    EXPECT_FALSE(rpc::fenced_coins_in_trade_record(json{{"trade_record", {{"trade_id", "0x01"}}}},
                                                   fenced));
    EXPECT_FALSE(rpc::fenced_coins_in_trade_record(json::object(), fenced));
}

// -- the fence's state ----------------------------------------------------------

TEST(CoinFenceState, OffFencesNothing)
{
    const rpc::CoinFence fence;
    EXPECT_FALSE(fence.enabled());
    ASSERT_TRUE(fence.exclusions());
    EXPECT_TRUE(fence.exclusions()->empty());
}

TEST(CoinFenceState, OnIsUnknownUntilRefreshedAndAgainWhenStale)
{
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    const auto t0 = Clock::now();
    EXPECT_FALSE(fence.exclusions(t0)) << "never refreshed: unknown, never empty";

    fence.update({{"aa", kPuzzle, 5}, {"bb", kPuzzle, 7}}, t0);
    ASSERT_TRUE(fence.exclusions(t0 + std::chrono::seconds(30)));
    EXPECT_EQ(*fence.exclusions(t0), (std::vector<std::string>{"aa", "bb"}));
    EXPECT_TRUE(fence.is_fenced_coin("0xAA"));
    EXPECT_FALSE(fence.is_fenced_coin("cc"));

    EXPECT_FALSE(fence.exclusions(t0 + std::chrono::seconds(31))) << "stale";
    const auto snap = fence.snapshot(t0 + std::chrono::seconds(31));
    EXPECT_TRUE(snap.enabled);
    EXPECT_FALSE(snap.fresh);
    EXPECT_EQ(snap.coin_count, 2U);
}

TEST(CoinFenceState, AFailedRefreshForgetsTheOldList)
{
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    fence.update({{"aa", kPuzzle, 5}});
    fence.mark_failed("node unreachable");
    EXPECT_FALSE(fence.exclusions());
    EXPECT_EQ(fence.snapshot().last_error, "node unreachable");
}

TEST(CoinFenceState, OnWithoutAnAddressRefusesEverything)
{
    rpc::CoinFence fence;
    fence.configure({}, std::chrono::seconds(30));
    fence.update({});
    EXPECT_TRUE(fence.enabled());
    EXPECT_FALSE(fence.exclusions()) << "an unreadable address must not read as nothing to fence";
}

TEST(CoinFenceState, EnsureFreshRefreshesOnlyWhenStaleAndNeverThrows)
{
    boost::asio::io_context ioc;
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    int calls = 0;
    fence.set_refresher([&calls]() -> boost::asio::awaitable<std::vector<rpc::FencedCoin>> {
        ++calls;
        co_return std::vector<rpc::FencedCoin>{{"aa", kPuzzle, 5}};
    });
    auto run = [&] {
        auto done = boost::asio::co_spawn(ioc, fence.ensure_fresh(), boost::asio::use_future);
        ioc.restart();
        ioc.run();
        done.get();
    };
    run();
    run();
    EXPECT_EQ(calls, 1) << "fresh: no second refresh";
    ASSERT_TRUE(fence.exclusions());

    fence.set_refresher([]() -> boost::asio::awaitable<std::vector<rpc::FencedCoin>> {
        throw std::runtime_error("node down");
        co_return std::vector<rpc::FencedCoin>{};
    });
    fence.mark_failed("forced");
    run();
    EXPECT_FALSE(fence.exclusions());
    EXPECT_EQ(fence.snapshot().last_error, "node down");
}

// -- every coin-selecting wallet call is fenced (source scan) -----------------

namespace {

std::string read_chia_rpc_cpp()
{
    std::ifstream in(XOP_TESTS_CHIA_RPC_CPP, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// The text of the ChiaWalletRPC member defined as `name(`, to its closing
/// brace at column 0.
std::string member(const std::string& src, const std::string& name)
{
    const std::size_t at = src.find("ChiaWalletRPC::" + name + "(");
    if (at == std::string::npos) return {};
    const std::size_t end = src.find("\n}", at);
    return src.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

}  // namespace

TEST(CoinFenceWiring, EveryCoinSelectingWalletCallAsksTheFence)
{
    const std::string src = read_chia_rpc_cpp();
    ASSERT_FALSE(src.empty());
    for (const char* name : {"create_offer", "take_offer", "cancel_offer", "cancel_offers",
                             "send_transaction", "split_coins", "select_coins"}) {
        const std::string body = member(src, name);
        ASSERT_FALSE(body.empty()) << name;
        EXPECT_NE(body.find("fence_exclusions("), std::string::npos)
            << "ChiaWalletRPC::" << name << " selects coins and must ask the fence";
    }
    // And the offer is read back for fenced coins after it is made.
    EXPECT_NE(member(src, "create_offer").find("fenced_coins_in_trade_record("), std::string::npos);
}

TEST(CoinFenceWiring, NoWalletPickedFeeCoinWhileTheFenceHoldsXch)
{
    // [FEE-COIN] The wallet ignores excluded_coin_ids for a CAT spend's fee
    // coin: these calls drop the fee instead, and say what they sent.
    const std::string src = read_chia_rpc_cpp();
    ASSERT_FALSE(src.empty());
    const std::string cancel = member(src, "cancel_offer");
    EXPECT_NE(cancel.find("fence_safe_cancel_fee("), std::string::npos);
    EXPECT_LT(cancel.find("fence_safe_cancel_fee("), cancel.find("fence_exclusions("))
        << "the fee is settled before the exclusions are asked for";
    for (const char* name : {"cancel_offer", "cancel_offers", "split_coins"}) {
        EXPECT_NE(member(src, name).find("kFeeUsedKey"), std::string::npos) << name;
    }
    EXPECT_NE(member(src, "cancel_offers").find("fence_up()"), std::string::npos);
    EXPECT_NE(member(src, "split_coins").find("fence_up()"), std::string::npos);
    EXPECT_NE(member(src, "fence_safe_cancel_fee").find("fence_up()"), std::string::npos)
        << "whenever the fence is up: holding XCH can change before the wallet chooses";
    EXPECT_NE(member(src, "fence_safe_cancel_fee").find("trade_record_offers_xch("),
              std::string::npos);
}

TEST(CoinFenceFeeCoin, AnOfferOfCatsAloneIsToldApartFromAnOfferOfXch)
{
    const auto offered = [](json what) { return json{{"summary", {{"offered", what}}}}; };
    EXPECT_EQ(rpc::trade_record_offers_xch(offered({{"xch", 500000000000}})), true);
    EXPECT_EQ(rpc::trade_record_offers_xch(offered({{"XCH", 1}})), true);
    EXPECT_EQ(rpc::trade_record_offers_xch(offered({{kUsdc, 8641}})), false);
    EXPECT_EQ(rpc::trade_record_offers_xch(offered(json::object())), false);
    EXPECT_FALSE(rpc::trade_record_offers_xch(json{{"trade_id", "0x01"}}).has_value());
    EXPECT_FALSE(rpc::trade_record_offers_xch(json{{"summary", "x"}}).has_value());
    EXPECT_FALSE(rpc::trade_record_offers_xch(json::array()).has_value());
}

// -- [stage 2] balances leave the sub-account out ---------------------------------

TEST(CoinFenceBalance, HeldIsCountedPerAssetAndUnknownWhenTheFenceIs)
{
    const std::string usd_ph = ex::CoinManager::cat_puzzle_hash(kUsdc, kPuzzle).value();
    rpc::CoinFence fence;
    EXPECT_EQ(fence.held("xch"), (std::pair<std::uint64_t, std::size_t>{0, 0})) << "off: nothing held";

    fence.configure({kPuzzle, usd_ph}, std::chrono::seconds(30));
    fence.set_assets({{kPuzzle, "xch"}, {usd_ph, std::string{"0x"} + kUsdc}});
    EXPECT_FALSE(fence.held("xch")) << "on but unknown";

    fence.update({{"aa", kPuzzle, 500}, {"bb", kPuzzle, 250}, {"cc", usd_ph, 1000}});
    EXPECT_EQ(fence.held("xch"), (std::pair<std::uint64_t, std::size_t>{750, 2}));
    EXPECT_EQ(fence.held(kUsdc), (std::pair<std::uint64_t, std::size_t>{1000, 1}));
    EXPECT_EQ(fence.held(kByc), (std::pair<std::uint64_t, std::size_t>{0, 0}));
}

TEST(CoinFenceBalance, SubtractsTheSubAccountFromEveryFigureNeverBelowZero)
{
    json balance = {{"wallet_id", 1},
                    {"asset_id", nullptr},
                    {"confirmed_wallet_balance", 301764857863909LL},
                    {"unconfirmed_wallet_balance", 301764857863909LL},
                    {"spendable_balance", 1000},
                    {"max_send_amount", 1000},
                    {"unspent_coin_count", 188},
                    {"pending_change", 0}};
    rpc::ChiaWalletRPC::subtract_fenced_balance(balance, 1500, 3);
    EXPECT_EQ(balance["confirmed_wallet_balance"].get<std::int64_t>(), 301764857862409LL);
    EXPECT_EQ(balance["unconfirmed_wallet_balance"].get<std::int64_t>(), 301764857862409LL);
    EXPECT_EQ(balance["spendable_balance"].get<std::int64_t>(), 0) << "clamped";
    EXPECT_EQ(balance["max_send_amount"].get<std::int64_t>(), 0);
    EXPECT_EQ(balance["unspent_coin_count"].get<std::int64_t>(), 185);
    EXPECT_EQ(balance["pending_change"].get<std::int64_t>(), 0) << "untouched";
}

TEST(CoinFenceWiring, BalancesAndSpendableCoinsAskTheFence)
{
    const std::string src = read_chia_rpc_cpp();
    EXPECT_NE(member(src, "get_wallet_balance").find("subtract_fenced_balance("), std::string::npos);
    EXPECT_NE(member(src, "get_wallet_balance").find("CoinFenceUnavailable"), std::string::npos);
    EXPECT_NE(member(src, "get_spendable_coins").find("puzzle_hashes()"), std::string::npos);
}

// -- review of #240 ------------------------------------------------------------------

TEST(CoinFenceWiring, TheRefresherReadsStrictlyAndSendsGetAListReadJustNow)
{
    std::ifstream in(XOP_TESTS_ENGINE_CPP, std::ios::binary);
    std::ostringstream engine;
    engine << in.rdbuf();
    const std::string setup = engine.str().substr(engine.str().find("void Engine::setup_coin_fence()"));
    EXPECT_NE(setup.substr(0, setup.find("\n}")).find("get_unspent_coin_records_strict("),
              std::string::npos)
        << "an unreadable inventory must throw, not read as no coins";

    const std::string src = read_chia_rpc_cpp();
    const std::string fx = member(src, "fence_exclusions");
    EXPECT_NE(fx.find("refresh_now()"), std::string::npos)
        << "non-offer spends cannot be checked afterwards: they need a list read just now";
}

TEST(CoinFenceState, RefreshNowIgnoresTheAgeAndRecordsTheWallClock)
{
    boost::asio::io_context ioc;
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    int calls = 0;
    fence.set_refresher([&calls]() -> boost::asio::awaitable<std::vector<rpc::FencedCoin>> {
        ++calls;
        co_return std::vector<rpc::FencedCoin>{{"aa", kPuzzle, 5}};
    });
    for (int i = 0; i < 2; ++i) {
        auto done = boost::asio::co_spawn(ioc, fence.refresh_now(), boost::asio::use_future);
        ioc.restart();
        ioc.run();
        done.get();
    }
    EXPECT_EQ(calls, 2);
    EXPECT_GT(fence.snapshot().refreshed_unix, 1.7e9);
}


// -- [AUTO-FENCE] engine_fence: auto -- the fence follows the options book --------

TEST(CoinFenceAuto, DownFencesNothingAndHoldsNothing)
{
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    fence.update({{"aa", kPuzzle, 5}});
    ASSERT_TRUE(fence.set_active(false));
    EXPECT_TRUE(fence.configured());
    EXPECT_FALSE(fence.enabled());
    ASSERT_TRUE(fence.exclusions()) << "down is known to fence nothing, not unknown";
    EXPECT_TRUE(fence.exclusions()->empty());
    const auto held = fence.held("xch");
    ASSERT_TRUE(held);
    EXPECT_EQ(held->first, 0U);
    EXPECT_FALSE(fence.is_fenced_coin("aa")) << "going down forgets the list";
    const auto snap = fence.snapshot();
    EXPECT_FALSE(snap.enabled);
    EXPECT_FALSE(snap.fresh) << "a fence that is down never reports itself holding";
}

TEST(CoinFenceAuto, GoingUpIsUnknownUntilRefreshedNeverAnOldList)
{
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    fence.update({{"aa", kPuzzle, 5}});
    fence.set_active(false);
    ASSERT_TRUE(fence.set_active(true));
    EXPECT_TRUE(fence.enabled());
    EXPECT_FALSE(fence.exclusions()) << "nothing may spend on a list from before it went down";
    EXPECT_FALSE(fence.held("xch"));
    fence.update({{"bb", kPuzzle, 7}});
    ASSERT_TRUE(fence.exclusions());
    EXPECT_EQ(*fence.exclusions(), (std::vector<std::string>{"bb"}));
}

TEST(CoinFenceAuto, SetActiveSaysWhetherItMovedAndUpIsTheDefault)
{
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    EXPECT_TRUE(fence.active()) << "engine_fence: true never calls set_active";
    EXPECT_FALSE(fence.set_active(true));
    EXPECT_TRUE(fence.set_active(false));
    EXPECT_FALSE(fence.set_active(false));
    EXPECT_FALSE(fence.active());
}

TEST(CoinFenceAuto, ADownFenceDoesNotRefresh)
{
    boost::asio::io_context ioc;
    rpc::CoinFence fence;
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    fence.set_active(false);
    int calls = 0;
    fence.set_refresher([&calls]() -> boost::asio::awaitable<std::vector<rpc::FencedCoin>> {
        ++calls;
        co_return std::vector<rpc::FencedCoin>{};
    });
    auto a = boost::asio::co_spawn(ioc, fence.ensure_fresh(), boost::asio::use_future);
    auto b = boost::asio::co_spawn(ioc, fence.refresh_now(), boost::asio::use_future);
    ioc.run();
    a.get();
    b.get();
    EXPECT_EQ(calls, 0) << "down, the full node is not asked";
}

TEST(CoinFenceWiring, TheEngineFollowsTheOptionsFenceFlag)
{
    std::ifstream in(XOP_TESTS_ENGINE_CPP, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string engine = buffer.str();

    const std::string setup = engine.substr(engine.find("void Engine::setup_coin_fence()"));
    const std::string setup_body = setup.substr(0, setup.find("\n}"));
    EXPECT_NE(setup_body.find("follow_book"), std::string::npos);
    EXPECT_NE(setup_body.find("set_active(false)"), std::string::npos)
        << "auto starts down until the flag is read";

    // (Line endings vary by checkout: look a little way past the assignment.)
    const std::size_t flag_at =
        engine.find("options_fence_flag_path_ = db_dir / \"options_fence.flag\";");
    ASSERT_NE(flag_at, std::string::npos);
    EXPECT_NE(engine.substr(flag_at, 160).find("sync_options_fence_flag();"), std::string::npos)
        << "the flag is read in the constructor, before anything can spend";

    const std::string poll = engine.substr(engine.find("asio::awaitable<void> Engine::poll_loop_coro()"));
    EXPECT_NE(poll.substr(0, poll.find("\n}")).find("sync_options_fence_flag()"), std::string::npos)
        << "the fast poll follows the flag";

    const std::string beat = engine.substr(engine.find("asio::awaitable<void> Engine::on_new_block_coro("));
    const std::string beat_head = beat.substr(0, beat.find("ensure_fresh()"));
    EXPECT_NE(beat_head.find("sync_options_fence_flag()"), std::string::npos)
        << "each heartbeat follows the flag before it refreshes the list";

    const std::string sync = engine.substr(engine.find("bool Engine::sync_options_fence_flag()"));
    EXPECT_NE(sync.substr(0, sync.find("\n}")).find("want = ec ? true : there;"), std::string::npos)
        << "a flag that cannot be read counts as there: the fence fails up";
}


// -- [OPTIONS-CAPITAL] what the fence tells the books ------------------------------

namespace {

double unix_now()
{
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

using Totals = rpc::CoinFence::Totals;

}  // namespace

TEST(CoinFenceCapital, TheFirstListAfterGoingUpIsWhatLeftTheTradingFunds)
{
    const std::string usd_ph = ex::CoinManager::cat_puzzle_hash(kUsdc, kPuzzle).value();
    rpc::CoinFence fence;
    fence.configure({kPuzzle, usd_ph}, std::chrono::seconds(30));
    fence.set_assets({{kPuzzle, "xch"}, {usd_ph, std::string{"0x"} + kUsdc}});
    EXPECT_FALSE(fence.raised()) << "up, and not listed yet";

    const double before = unix_now();
    fence.update({{"aa", kPuzzle, 500}, {"bb", kPuzzle, 250}, {"cc", usd_ph, 1000}});
    const auto raised = fence.raised();
    ASSERT_TRUE(raised);
    EXPECT_EQ(raised->totals, (Totals{{"xch", 750}, {kUsdc, 1000}}));
    EXPECT_EQ(raised->coins, (Totals{{"xch", 500}, {"xch", 250}, {kUsdc, 1000}}))
        << "each coin under its asset: a top-up's coin can be found among them";
    EXPECT_GE(raised->at_unix, before);
    EXPECT_LE(raised->at_unix, unix_now());

    // It stays as it was then: a top-up that arrives later is not in it, so
    // the event books what left with the fence however late it is written.
    fence.update({{"aa", kPuzzle, 500}, {"bb", kPuzzle, 250}, {"cc", usd_ph, 1000},
                  {"dd", kPuzzle, 2000}});
    ASSERT_TRUE(fence.raised());
    EXPECT_EQ(fence.raised()->totals, raised->totals);
    EXPECT_EQ(fence.raised()->at_unix, raised->at_unix);
    // A failed refresh does not unsay it either.
    fence.mark_failed("node away");
    ASSERT_TRUE(fence.raised());
    EXPECT_EQ(fence.raised()->totals, raised->totals);

    // Down, nothing is behind the fence; up again, the next first list counts.
    ASSERT_TRUE(fence.set_active(false));
    EXPECT_FALSE(fence.raised());
    ASSERT_TRUE(fence.set_active(true));
    EXPECT_FALSE(fence.raised()) << "unknown until listed, never the old count";
    fence.update({{"dd", kPuzzle, 2000}});
    ASSERT_TRUE(fence.raised());
    EXPECT_EQ(fence.raised()->totals, (Totals{{"xch", 2000}, {kUsdc, 0}}))
        << "an asset that is fenced and holds nothing is there, at nothing";
    EXPECT_EQ(fence.raised()->coins, (Totals{{"xch", 2000}}));

    // A list had while the fence is down is not one behind a fence.
    ASSERT_TRUE(fence.set_active(false));
    fence.update({{"ee", kPuzzle, 7}});
    EXPECT_FALSE(fence.raised());

    // Configured anew, it has no past.
    fence.configure({kPuzzle}, std::chrono::seconds(30));
    EXPECT_FALSE(fence.raised());
    EXPECT_FALSE(fence.lowered());
}

TEST(CoinFenceCapital, GoingDownKeepsWhatTheLastListHeld)
{
    const std::string usd_ph = ex::CoinManager::cat_puzzle_hash(kUsdc, kPuzzle).value();
    rpc::CoinFence fence;
    fence.configure({kPuzzle, usd_ph}, std::chrono::seconds(30));
    fence.set_assets({{kPuzzle, "xch"}, {usd_ph, kUsdc}});
    EXPECT_FALSE(fence.lowered()) << "it never went down";

    // Those coins become trading funds: the books need how much, and when.
    fence.update({{"aa", kPuzzle, 500}, {"bb", kPuzzle, 250}, {"cc", usd_ph, 1000}});
    fence.update({{"aa", kPuzzle, 500}, {"cc", usd_ph, 1000}});
    const double before = unix_now();
    ASSERT_TRUE(fence.set_active(false));
    const auto lowered = fence.lowered();
    ASSERT_TRUE(lowered);
    EXPECT_EQ(lowered->totals, (Totals{{"xch", 500}, {kUsdc, 1000}})) << "the last list";
    EXPECT_EQ(lowered->coins, (Totals{{"xch", 500}, {kUsdc, 1000}}));
    EXPECT_GE(lowered->at_unix, before) << "stamped when it went down, not when it was listed";
    EXPECT_LE(lowered->at_unix, unix_now());
    EXPECT_FALSE(fence.is_fenced_coin("aa")) << "the list itself is forgotten, as before";
    EXPECT_FALSE(fence.set_active(false)) << "already down";
    ASSERT_TRUE(fence.lowered());
    EXPECT_EQ(fence.lowered()->at_unix, lowered->at_unix);

    // Up again and down before any list: unknown, never the old count and
    // never zero.
    ASSERT_TRUE(fence.set_active(true));
    EXPECT_TRUE(fence.lowered()) << "the last lowering still stands";
    ASSERT_TRUE(fence.set_active(false));
    EXPECT_FALSE(fence.lowered());

    // A list whose last refresh failed, or that has gone stale, is still the
    // best count of what came back.
    ASSERT_TRUE(fence.set_active(true));
    fence.update({{"dd", kPuzzle, 40}});
    fence.mark_failed("node away");
    ASSERT_TRUE(fence.set_active(false));
    ASSERT_TRUE(fence.lowered());
    EXPECT_EQ(fence.lowered()->totals, (Totals{{"xch", 40}, {kUsdc, 0}}));
}

TEST(CoinFenceCapital, ListHoldingReadsTheAddressWhateverTheFenceIsDoing)
{
    const std::string usd_ph = ex::CoinManager::cat_puzzle_hash(kUsdc, kPuzzle).value();
    const auto run = [](rpc::CoinFence& fence) {
        boost::asio::io_context ioc;
        auto done = boost::asio::co_spawn(ioc, fence.list_holding(), boost::asio::use_future);
        ioc.run();
        return done.get();
    };
    rpc::CoinFence off;
    EXPECT_FALSE(run(off)) << "not configured: no address to list";

    rpc::CoinFence fence;
    fence.configure({kPuzzle, usd_ph}, std::chrono::seconds(30));
    fence.set_assets({{kPuzzle, "xch"}, {usd_ph, kUsdc}});
    EXPECT_FALSE(run(fence)) << "no refresher installed";

    int calls = 0;
    fence.set_refresher([&]() -> boost::asio::awaitable<std::vector<rpc::FencedCoin>> {
        ++calls;
        co_return std::vector<rpc::FencedCoin>{{"aa", kPuzzle, 500}, {"cc", usd_ph, 1000}};
    });
    fence.set_active(false);
    const double before = unix_now();
    const auto holding = run(fence);
    ASSERT_TRUE(holding) << "a fence that is down can still be asked what is at the address";
    EXPECT_EQ(holding->totals, (Totals{{"xch", 500}, {kUsdc, 1000}}));
    EXPECT_EQ(holding->coins, (Totals{{"xch", 500}, {kUsdc, 1000}}));
    EXPECT_GE(holding->at_unix, before);
    EXPECT_EQ(calls, 1);
    // It changed nothing: still down, still fencing nothing.
    EXPECT_FALSE(fence.enabled());
    ASSERT_TRUE(fence.exclusions());
    EXPECT_TRUE(fence.exclusions()->empty());
    EXPECT_FALSE(fence.lowered()) << "it went down with no list had";

    // Nor is asking a list behind the fence: up, the first refresh still is.
    fence.set_active(true);
    ASSERT_TRUE(run(fence));
    EXPECT_FALSE(fence.raised());
    EXPECT_FALSE(fence.exclusions()) << "still unknown to the spending calls";

    fence.set_refresher([]() -> boost::asio::awaitable<std::vector<rpc::FencedCoin>> {
        throw std::runtime_error("node away");
        co_return std::vector<rpc::FencedCoin>{};
    });
    EXPECT_FALSE(run(fence)) << "unreadable is unknown, not empty";
}
