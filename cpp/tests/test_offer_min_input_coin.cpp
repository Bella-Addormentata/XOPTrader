// ---------------------------------------------------------------------------
// [MIN-INPUT-COIN] The floor on the coins an offer may be funded from.
//
// On 2026-09-19 an XCH/DBX bid paying 80.334 DBX was built from reward dust
// and Dexie refused it for having too many input coins.  These tests pin the
// five ways the fix could quietly stop protecting, or start harming:
//
//   1. The floor is ceil(offered x frac).  Rounded DOWN it admits coins just
//      under the fraction and loses the ceil(1 / frac) input bound.
//   2. 0 disables, and an invalid fraction degrades to "no floor" -- never to
//      a floor nobody chose.
//   3. The floor is sent for CAT-funded offers only, as ONE top-level key,
//      and an unconfigured request is byte-identical to the old one.
//   4. The same number also governs the XCH fee coin, so it has to stay tiny
//      in XCH terms.
//   5. The fallback sends a second create ONLY after the wallet ANSWERED the
//      first with the min-coin refusal.  A timeout is not an answer, and a
//      second create after one could be a second offer for the same tier.
//
// Nothing here constructs an OfferManager: the decisions are pure, and the
// fallback takes the wallet call as a parameter.
// tests/test_offer_min_input_coin_wiring.py pins that OfferManager uses them.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xop/config.hpp"
#include "xop/execution/offer_min_input_coin.hpp"
#include "xop/rpc/chia_rpc.hpp"
#include "xop/types.hpp"

namespace asio = boost::asio;
using nlohmann::json;
using xop::execution::create_offer_with_min_coin_fallback;
using xop::execution::dexie_rejected_too_many_inputs;
using xop::execution::ledger_min_coin_mojos;
using xop::execution::min_input_coin_frac_ppb;
using xop::execution::min_input_coin_mojos;
using xop::execution::offer_min_input_coin;
using xop::execution::wallet_refused_for_min_coin;
using xop::rpc::ChiaWalletRPC;

namespace {

using Floor = std::optional<std::uint64_t>;

// The refusal chia 2.7.4 returns when the coins at or above min_coin_amount
// cannot cover the amount (coin_selection.py), with the prefix TradeManager
// adds on the way out.
const char* const kMinCoinRefusal =
    "Error creating offer: Transaction for 80334 is greater than max "
    "spendable balance in a block of 61200. There may be other transactions "
    "pending or our minimum coin amount is too high.";

// The body Dexie answered with on 2026-09-19 14:42:50, copied from engine.log.
const char* const kDexieTooManyInputs =
    R"({"success":false,"error_message":"Too many input coins, use smaller )"
    R"(offers or merge the coins by sending the offered amount to yourself."})";

// An XCH/DBX bid: spends DBX (wallet 5), receives XCH (wallet 1).
json cat_funded_bid(std::int64_t cat_mojos) {
    return json{{"5", -cat_mojos}, {"1", std::int64_t{1'000'000'000'000}}};
}

// Run one awaitable to completion on a private io_context.  Rethrows, so a
// test expecting an exception sees the real one.
template <class T>
T run_awaitable(asio::awaitable<T> a)
{
    asio::io_context   ioc;
    std::optional<T>   out;
    std::exception_ptr err;
    asio::co_spawn(ioc, std::move(a),
                   [&](std::exception_ptr ep, T value) {
                       err = ep;
                       if (!ep) out = std::move(value);
                   });
    ioc.run();
    if (err) std::rethrow_exception(err);
    return std::move(*out);
}

// A scripted stand-in for ChiaWalletRPC::create_offer.  Each create consumes
// one step; a create beyond the script throws std::out_of_range, so an
// unexpected extra request fails the test instead of being answered.
enum class StepKind { Ok, ApplicationError, TransportError, BaseError };

struct Step {
    StepKind    kind{StepKind::Ok};
    std::string text{};
};

struct FakeWallet {
    std::vector<Step>  script{};
    std::vector<Floor> floors_seen{};  // the floor passed to each create
};

auto creator(FakeWallet& w)
{
    return [&w](Floor coin_floor) -> asio::awaitable<json> {
        const std::size_t i = w.floors_seen.size();
        w.floors_seen.push_back(coin_floor);
        const Step& s = w.script.at(i);
        switch (s.kind) {
            case StepKind::ApplicationError:
                throw xop::rpc::ChiaRPCApplicationError(
                    s.text, json{{"success", false}, {"error", s.text}});
            case StepKind::TransportError:
                throw xop::rpc::ChiaRPCTransportError(
                    s.text, 0, CURLE_OPERATION_TIMEDOUT);
            case StepKind::BaseError:
                throw xop::rpc::ChiaRPCError(s.text);
            case StepKind::Ok:
                break;
        }
        co_return json{{"offer", "offer1abc"},
                       {"trade_record", {{"trade_id", s.text}}}};
    };
}

struct Fallbacks {
    std::vector<std::string> refusals{};
};

auto recorder(Fallbacks& f)
{
    return [&f](const std::string& refusal) { f.refusals.push_back(refusal); };
}

}  // namespace

// ===========================================================================
// 1. The floor itself
// ===========================================================================

TEST(OfferMinInputCoin, TheIncidentOfferGetsAFloorAboveTheRewardDust) {
    // 80.334 DBX at the default 1%: 803.34 mojos, rounded UP.  The dust that
    // built the refused offer was under 0.1 DBX = 100 mojos per coin.
    const auto coin_floor = min_input_coin_mojos(80'334, 0.01);
    ASSERT_TRUE(coin_floor.has_value());
    EXPECT_EQ(*coin_floor, std::uint64_t{804});
    EXPECT_GT(*coin_floor, std::uint64_t{100});
}

TEST(OfferMinInputCoin, RoundsUpNeverDown) {
    EXPECT_EQ(*min_input_coin_mojos(100'000, 0.01), std::uint64_t{1'000});
    // One mojo more than a whole multiple must lift the floor: floor() here
    // would return 1000 and admit a coin below 1% of the amount.
    EXPECT_EQ(*min_input_coin_mojos(100'001, 0.01), std::uint64_t{1'001});
    EXPECT_EQ(*min_input_coin_mojos(100'099, 0.01), std::uint64_t{1'001});
    EXPECT_EQ(*min_input_coin_mojos(100'100, 0.01), std::uint64_t{1'001});
    EXPECT_EQ(*min_input_coin_mojos(100'101, 0.01), std::uint64_t{1'002});
}

TEST(OfferMinInputCoin, TinyOffersGetAFloorOfOneMojo) {
    // ceil of anything positive is at least 1; a floor of 1 excludes nothing
    // a wallet can hold, which is the right answer for an offer this small.
    for (std::uint64_t offered : {std::uint64_t{1}, std::uint64_t{2},
                                  std::uint64_t{99}, std::uint64_t{100}}) {
        const auto coin_floor = min_input_coin_mojos(offered, 0.01);
        ASSERT_TRUE(coin_floor.has_value()) << offered;
        EXPECT_EQ(*coin_floor, std::uint64_t{1}) << offered;
    }
}

TEST(OfferMinInputCoin, ZeroDisables) {
    EXPECT_FALSE(min_input_coin_mojos(80'334, 0.0).has_value());
    EXPECT_FALSE(min_input_coin_mojos(80'334, -0.0).has_value());
    EXPECT_EQ(min_input_coin_frac_ppb(0.0), std::uint64_t{0});
}

TEST(OfferMinInputCoin, NothingOfferedMeansNoFloor) {
    EXPECT_FALSE(min_input_coin_mojos(0, 0.01).has_value());
}

TEST(OfferMinInputCoin, AnInvalidFractionDegradesToNoFloor) {
    // config.cpp rejects all of these at load; this is the second line.  The
    // failure direction matters: "no floor" is the request the engine sent
    // for months, whereas a floor from a garbage fraction could refuse every
    // coin in the wallet.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double bad : {-0.01, -1.0, 1.0, 1.5, nan, inf, -inf}) {
        EXPECT_FALSE(min_input_coin_mojos(80'334, bad).has_value())
            << "fraction " << bad << " produced a floor";
    }
}

TEST(OfferMinInputCoin, TheFractionIsReadInPartsPerBillionRoundedUp) {
    // Exact in ppb: unchanged by the direction of rounding.
    EXPECT_EQ(min_input_coin_frac_ppb(0.01), std::uint64_t{10'000'000});
    EXPECT_EQ(min_input_coin_frac_ppb(0.5), std::uint64_t{500'000'000});
    // Positive but below the resolution still means "on".
    EXPECT_EQ(min_input_coin_frac_ppb(1e-12), std::uint64_t{1});
    // [review #162] Not exact in ppb: UP, never to nearest.  Both of these
    // have a fractional part under one half, so nearest rounds them DOWN
    // (10,101,010 and 333,333,333) and the applied fraction falls below the
    // configured one.
    EXPECT_EQ(min_input_coin_frac_ppb(0.0101010102), std::uint64_t{10'101'011});
    EXPECT_EQ(min_input_coin_frac_ppb(1.0 / 3.0), std::uint64_t{333'333'334});
    // Just under 1 reaches the full scale.  Pulling it back to scale - 1
    // would be the one place the fraction is still rounded down.
    EXPECT_EQ(min_input_coin_frac_ppb(std::nextafter(1.0, 0.0)),
              std::uint64_t{1'000'000'000});
}

TEST(OfferMinInputCoin, AFractionJustUnderOneMakesTheFloorTheWholeOffer) {
    // The full scale: only a coin at least as large as the offer may fund it.
    // Equal to the amount, never above it -- and no wrap at the top of uint64,
    // where q x ppb is the whole multiple of 10^9 below the amount.
    const double almost_one = std::nextafter(1.0, 0.0);
    for (std::uint64_t offered :
         {std::uint64_t{1}, std::uint64_t{12'345}, std::uint64_t{1'000'000'000},
          std::uint64_t{1'000'000'001},
          std::numeric_limits<std::uint64_t>::max()}) {
        const auto coin_floor = min_input_coin_mojos(offered, almost_one);
        ASSERT_TRUE(coin_floor.has_value()) << offered;
        EXPECT_EQ(*coin_floor, offered);
    }
}

TEST(OfferMinInputCoin, TheReviewExampleReachesTheAmountInNinetyNineCoins) {
    // [review #162] 0.0101010102 is a hair above 1/99, so ceil(1 / frac) is
    // 99.  Rounded to nearest it became 10,101,010 ppb and 99 floor-sized
    // coins of a 1,000,000,000-mojo offer totalled 999,999,990.
    const auto coin_floor = min_input_coin_mojos(1'000'000'000, 0.0101010102);
    ASSERT_TRUE(coin_floor.has_value());
    EXPECT_EQ(*coin_floor, std::uint64_t{10'101'011});
    EXPECT_EQ(static_cast<std::uint64_t>(std::ceil(1.0 / 0.0101010102)),
              std::uint64_t{99});
    EXPECT_GE(*coin_floor * 99u, std::uint64_t{1'000'000'000});
}

TEST(OfferMinInputCoin, TheInputBoundHoldsAtEveryReciprocalBoundary) {
    // The documented bound, as a property: ceil(1 / frac) coins of floor size
    // always reach the amount.  Reciprocals are where it is tightest -- k x
    // frac is 1 with nothing to spare -- and rounding the fraction to nearest
    // broke it for 901 of the 1,999 values of 1/k below (k = 3 first).
    // Counted rather than asserted per case, so a regression prints one line.
    std::uint64_t broken = 0;
    std::string   first_broken;
    for (std::uint64_t k = 2; k <= 2000; ++k) {
        const double exact = 1.0 / static_cast<double>(k);
        for (double frac : {exact, std::nextafter(exact, 1.0)}) {
            const auto coins =
                static_cast<std::uint64_t>(std::ceil(1.0 / frac));
            for (std::uint64_t offered :
                 {std::uint64_t{80'334}, std::uint64_t{999'999'937},
                  std::uint64_t{1'000'000'000}, std::uint64_t{1'000'000'001},
                  std::uint64_t{123'456'789'012}}) {
                const auto coin_floor = min_input_coin_mojos(offered, frac);
                if (!coin_floor.has_value() || *coin_floor * coins < offered) {
                    if (broken == 0) {
                        first_broken = "k=" + std::to_string(k) + " offered="
                                     + std::to_string(offered) + " floor="
                                     + std::to_string(coin_floor.value_or(0))
                                     + " coins=" + std::to_string(coins);
                    }
                    ++broken;
                }
            }
        }
    }
    EXPECT_EQ(broken, std::uint64_t{0}) << "first: " << first_broken;
}

TEST(OfferMinInputCoin, TheFloorNeverExceedsTheAmountOffered) {
    // A coin exactly the size of the offer must always stay selectable.
    const double almost_one = std::nextafter(1.0, 0.0);
    for (std::uint64_t offered :
         {std::uint64_t{1}, std::uint64_t{7}, std::uint64_t{80'334},
          std::uint64_t{999'999'999}, std::uint64_t{1'000'000'000},
          std::uint64_t{1'000'000'001},
          std::numeric_limits<std::uint64_t>::max()}) {
        for (double frac : {1e-12, 0.01, 0.5, 0.999999999, almost_one}) {
            const auto coin_floor = min_input_coin_mojos(offered, frac);
            ASSERT_TRUE(coin_floor.has_value()) << offered << " x " << frac;
            EXPECT_GE(*coin_floor, std::uint64_t{1}) << offered << " x " << frac;
            EXPECT_LE(*coin_floor, offered) << offered << " x " << frac;
        }
    }
}

TEST(OfferMinInputCoin, HugeAmountsAreExactAndDoNotWrap) {
    // 18446744073709551615 x 0.01 = 184467440737095516.15 -> ...517.  A
    // double cannot hold this amount, and both 64-bit products sit within
    // 2^64; the split at 10^9 is what keeps them there.
    EXPECT_EQ(*min_input_coin_mojos(std::numeric_limits<std::uint64_t>::max(),
                                    0.01),
              std::uint64_t{184'467'440'737'095'517ULL});
    // INT64_MAX, the largest spend an offer_dict can carry.
    EXPECT_EQ(*min_input_coin_mojos(std::uint64_t{9'223'372'036'854'775'807ULL},
                                    0.01),
              std::uint64_t{92'233'720'368'547'759ULL});
    // 2^53 + 1 is the first integer a double silently rounds.
    EXPECT_EQ(*min_input_coin_mojos(std::uint64_t{9'007'199'254'740'993ULL},
                                    0.5),
              std::uint64_t{4'503'599'627'370'497ULL});
}

TEST(OfferMinInputCoin, InputsAtTheFloorAlwaysReachTheAmountWithinTheBound) {
    // The property the default is chosen for: with every coin >= the floor,
    // ceil(1 / frac) coins cover the amount.  100 at 1%, against a Dexie
    // limit measured between 125 and about 132 inputs.  [review #162,
    // round 6] That is the CAT LEG's count.  The XCH fee coin is chosen by
    // create_tandem_xch_tx in a separate selection that this CAT-scaled
    // floor does not constrain, so the offer carries these inputs plus
    // however many XCH coins the fee takes -- see offer_min_input_coin.hpp.
    for (std::uint64_t offered : {std::uint64_t{80'334}, std::uint64_t{99'999},
                                  std::uint64_t{100'001},
                                  std::uint64_t{123'456'789}}) {
        const std::uint64_t coin_floor = *min_input_coin_mojos(offered, 0.01);
        EXPECT_GE(coin_floor * 100u, offered) << offered;
    }
}

// ===========================================================================
// 2. Which offers get one
// ===========================================================================

TEST(OfferMinInputCoin, ACatFundedBidGetsAFloorFromItsQuoteLeg) {
    EXPECT_EQ(offer_min_input_coin(cat_funded_bid(80'334), 0.01),
              Floor{804});
}

TEST(OfferMinInputCoin, ACatFundedAskGetsAFloorFromItsBaseLeg) {
    // DBX/wUSDC.b style: spends the base CAT (wallet 5), receives wallet 9.
    const json ask{{"5", std::int64_t{-250'000}}, {"9", std::int64_t{40'000}}};
    EXPECT_EQ(offer_min_input_coin(ask, 0.01), Floor{2'500});
}

TEST(OfferMinInputCoin, AMergedBatchScalesWithTheMergedAmount) {
    // post_merged_side sums the tiers per wallet id: still one spend leg.
    const json merged{{"5", std::int64_t{-(80'334 + 120'000 + 200'000)}},
                      {"1", std::int64_t{5'000'000'000'000}}};
    EXPECT_EQ(offer_min_input_coin(merged, 0.01), Floor{4'004});
}

TEST(OfferMinInputCoin, AnXchFundedOfferIsLeftToTheCoinPool) {
    // XCH/DBX ask: spends XCH.  CoinManager shapes those coins and the XCH
    // lock ledger budgets them under the wallet's default selection.
    const json ask{{"1", std::int64_t{-1'000'000'000'000}},
                   {"5", std::int64_t{80'334}}};
    EXPECT_FALSE(offer_min_input_coin(ask, 0.01).has_value());
    // The same for an XCH-quoted bid.
    const json bid{{"1", std::int64_t{-2'500'000'000'000}},
                   {"7", std::int64_t{1'000'000}}};
    EXPECT_FALSE(offer_min_input_coin(bid, 0.5).has_value());
}

TEST(OfferMinInputCoin, ADisabledFractionSendsNoFloorForAnyOffer) {
    EXPECT_FALSE(offer_min_input_coin(cat_funded_bid(80'334), 0.0).has_value());
}

TEST(OfferMinInputCoin, ShapesTheRuleWasNotDesignedForGetNoFloor) {
    // Two spend legs: one floor would have to suit both assets.
    const json two{{"5", std::int64_t{-100'000}}, {"7", std::int64_t{-5'000}},
                   {"1", std::int64_t{1'000}}};
    EXPECT_FALSE(offer_min_input_coin(two, 0.01).has_value());
    // The XCH leg second in key order must not slip through either.
    const json cat_and_xch{{"1", std::int64_t{-100}},
                           {"5", std::int64_t{-100'000}}};
    EXPECT_FALSE(offer_min_input_coin(cat_and_xch, 0.01).has_value());
    // No spend leg, an empty dict, and things that are not dicts at all.
    EXPECT_FALSE(offer_min_input_coin(json{{"5", std::int64_t{100'000}}}, 0.01)
                     .has_value());
    EXPECT_FALSE(offer_min_input_coin(json::object(), 0.01).has_value());
    EXPECT_FALSE(offer_min_input_coin(json::array({-5}), 0.01).has_value());
    EXPECT_FALSE(offer_min_input_coin(json(nullptr), 0.01).has_value());
    // A non-integer amount is not something to do arithmetic on.
    EXPECT_FALSE(offer_min_input_coin(json{{"5", -100000.5}, {"1", 1}}, 0.01)
                     .has_value());
    EXPECT_FALSE(offer_min_input_coin(json{{"5", "-100000"}, {"1", 1}}, 0.01)
                     .has_value());
}

TEST(OfferMinInputCoin, ExtremeAmountsAreReadWithoutWrapping) {
    // INT64_MIN has no positive int64 counterpart; -v would be undefined.
    const json min_leg{{"5", std::numeric_limits<std::int64_t>::min()},
                       {"1", std::int64_t{1}}};
    EXPECT_EQ(offer_min_input_coin(min_leg, 0.5),
              Floor{4'611'686'018'427'387'904ULL});
    // A receive leg above INT64_MAX read as int64 would wrap NEGATIVE and be
    // mistaken for a second spend leg.
    const json big_receive{{"5", std::int64_t{-100'000}},
                           {"1", std::numeric_limits<std::uint64_t>::max()}};
    EXPECT_EQ(offer_min_input_coin(big_receive, 0.01), Floor{1'000});
}

TEST(OfferMinInputCoin, TheLedgerIsGivenTheSameFloorTheWalletIs) {
    // [review #162] The wallet applies the floor to the XCH fee coin too, so
    // the XCH lock ledger admits against the same number.  Same offer_dict,
    // same fraction, same answer -- as signed mojos, 0 for "none".
    for (std::int64_t cat_mojos : {std::int64_t{1}, std::int64_t{80'334},
                                   std::int64_t{10'000'000'000}}) {
        const json dict = cat_funded_bid(cat_mojos);
        const Floor sent = offer_min_input_coin(dict, 0.01);
        ASSERT_TRUE(sent.has_value());
        EXPECT_EQ(static_cast<std::uint64_t>(ledger_min_coin_mojos(dict, 0.01)),
                  *sent);
    }
    EXPECT_EQ(ledger_min_coin_mojos(cat_funded_bid(80'334), 0.01), 804);
    // No floor sent, no floor modelled: XCH-funded, disabled, unknown shape.
    const json xch_ask{{"1", std::int64_t{-1'000'000'000'000}},
                       {"5", std::int64_t{80'334}}};
    EXPECT_EQ(ledger_min_coin_mojos(xch_ask, 0.01), 0);
    EXPECT_EQ(ledger_min_coin_mojos(cat_funded_bid(80'334), 0.0), 0);
    EXPECT_EQ(ledger_min_coin_mojos(json::object(), 0.01), 0);
}

TEST(OfferMinInputCoin, ALedgerFloorAboveInt64Saturates) {
    // A spend of INT64_MIN at the full scale is a floor of 2^63, one past
    // what a Mojo holds.  Wrapped, it is negative and the ledger would read
    // it as "no floor"; saturated, it still filters every coin.
    const json min_leg{{"5", std::numeric_limits<std::int64_t>::min()},
                       {"1", std::int64_t{1}}};
    ASSERT_EQ(offer_min_input_coin(min_leg, std::nextafter(1.0, 0.0)),
              Floor{9'223'372'036'854'775'808ULL});
    EXPECT_EQ(ledger_min_coin_mojos(min_leg, std::nextafter(1.0, 0.0)),
              std::numeric_limits<std::int64_t>::max());
}

// ===========================================================================
// 3. The XCH fee coin lives under the same floor
// ===========================================================================

TEST(OfferMinInputCoin, ACatScaledFloorIsTinyInXchTerms) {
    // chia 2.7.4 applies ONE min_coin_amount to every selection in the
    // request, so for a CAT-funded offer the floor also filters the XCH fee
    // coin -- read as XCH mojos.  A CAT has 10^3 mojos per unit and XCH has
    // 10^12, which keeps it small.  Small is not inert: it changes which coin
    // pays whenever the XCH wallet holds a coin BELOW the floor -- not only
    // when the floor exceeds the fee -- and test_coin_lock_ledger pins that
    // the ledger models it.
    const std::uint64_t xch = static_cast<std::uint64_t>(xop::kMojosPerXch);
    // The incident offer: 804 mojos is under a billionth of an XCH.
    const Floor incident = offer_min_input_coin(cat_funded_bid(80'334), 0.01);
    ASSERT_TRUE(incident.has_value());
    EXPECT_LT(*incident, xch / 1'000'000'000u);
    // 100,000 CAT units: numerically CoinManager's XCH dust threshold
    // (1,000,000 mojos).  That is a coincidence of scale and nothing more --
    // the XCH lock ledger is seeded from get_spendable_coins, not from
    // CoinManager's pool, so a floor under 1,000,000 mojos is NOT a floor
    // "the engine ignores anyway" [review #162, round 3].
    EXPECT_EQ(offer_min_input_coin(cat_funded_bid(100'000'000), 0.01),
              Floor{1'000'000});
    // 1,000,000 CAT units -- far beyond any tier this bot posts: the fee
    // coin must be at least 0.00001 XCH.
    EXPECT_EQ(offer_min_input_coin(cat_funded_bid(1'000'000'000), 0.01),
              Floor{xch / 100'000u});
}

// ===========================================================================
// 4. What is sent to the wallet
// ===========================================================================

TEST(OfferMinInputCoin, TheFloorIsOneTopLevelKey) {
    const json dict = cat_funded_bid(80'334);
    const json p = ChiaWalletRPC::build_create_offer_payload(
        dict, 5000, false, std::nullopt, Floor{804});
    ASSERT_TRUE(p.contains("min_coin_amount"));
    EXPECT_TRUE(p["min_coin_amount"].is_number_unsigned());
    EXPECT_EQ(p["min_coin_amount"].get<std::uint64_t>(), std::uint64_t{804});
    // The wallet reads its coin-selection config from the top level of the
    // request; inside "offer" the key would be parsed as a wallet id.
    EXPECT_EQ(p["offer"], dict);
    EXPECT_FALSE(p["offer"].contains("min_coin_amount"));
}

TEST(OfferMinInputCoin, NoFloorLeavesThePayloadByteIdentical) {
    const json dict = cat_funded_bid(80'334);
    const json before = ChiaWalletRPC::build_create_offer_payload(
        dict, 5000, false, std::nullopt);
    const json after = ChiaWalletRPC::build_create_offer_payload(
        dict, 5000, false, std::nullopt, std::nullopt);
    EXPECT_FALSE(after.contains("min_coin_amount"));
    EXPECT_EQ(after.dump(), before.dump());
    EXPECT_EQ(after.size(), std::size_t{3});  // offer, fee, validate_only
}

TEST(OfferMinInputCoin, AZeroFloorIsNotSent) {
    // 0 is the wallet's own default; sending it would only make an
    // unconfigured request differ from the old one.
    const json p = ChiaWalletRPC::build_create_offer_payload(
        cat_funded_bid(80'334), 5000, false, std::nullopt, Floor{0});
    EXPECT_FALSE(p.contains("min_coin_amount"));
}

TEST(OfferMinInputCoin, NoOtherCoinSelectionKeyIsEverSent) {
    // The rule is a floor and nothing else.  max_coin_amount in particular
    // would cap the XCH fee coin at a CAT-scaled number and refuse them all.
    const json p = ChiaWalletRPC::build_create_offer_payload(
        cat_funded_bid(80'334), 5000, false, std::uint64_t{1'757'086'400},
        Floor{804});
    for (const char* key : {"max_coin_amount", "excluded_coin_amounts",
                            "exclude_coin_amounts", "excluded_coin_ids",
                            "exclude_coin_ids", "excluded_coins",
                            "exclude_coins", "included_coin_ids",
                            "primary_coin", "reuse_puzhash"}) {
        EXPECT_FALSE(p.contains(key)) << key;
    }
    // And it coexists with the expiry rather than displacing it.
    EXPECT_EQ(p["max_time"].get<std::uint64_t>(), std::uint64_t{1'757'086'400});
    EXPECT_EQ(p.size(), std::size_t{5});
}

TEST(OfferMinInputCoin, TheShippedDefaultIsOnePercent) {
    EXPECT_DOUBLE_EQ(xop::StrategyConfig{}.offer_min_input_coin_frac, 0.01);
}

// ===========================================================================
// 5. The fallback
// ===========================================================================

TEST(OfferMinInputCoinFallback, NoFloorIsOnePlainCreate) {
    FakeWallet w;
    w.script = {{StepKind::Ok, "0xaaa"}};
    Fallbacks f;
    const json r = run_awaitable(create_offer_with_min_coin_fallback(
        creator(w), Floor{}, recorder(f)));
    EXPECT_EQ(r["trade_record"]["trade_id"], "0xaaa");
    ASSERT_EQ(w.floors_seen.size(), std::size_t{1});
    EXPECT_FALSE(w.floors_seen[0].has_value());
    EXPECT_TRUE(f.refusals.empty());
}

TEST(OfferMinInputCoinFallback, AnAcceptedFloorIsOneCreateCarryingIt) {
    FakeWallet w;
    w.script = {{StepKind::Ok, "0xaaa"}};
    Fallbacks f;
    const json r = run_awaitable(create_offer_with_min_coin_fallback(
        creator(w), Floor{804}, recorder(f)));
    EXPECT_EQ(r["trade_record"]["trade_id"], "0xaaa");
    ASSERT_EQ(w.floors_seen.size(), std::size_t{1});
    EXPECT_EQ(w.floors_seen[0], Floor{804});
    EXPECT_TRUE(f.refusals.empty());
}

TEST(OfferMinInputCoinFallback, TheMinCoinRefusalIsRetriedOnceWithoutTheFloor) {
    FakeWallet w;
    w.script = {{StepKind::ApplicationError, kMinCoinRefusal},
                {StepKind::Ok, "0xbbb"}};
    Fallbacks f;
    const json r = run_awaitable(create_offer_with_min_coin_fallback(
        creator(w), Floor{804}, recorder(f)));
    EXPECT_EQ(r["trade_record"]["trade_id"], "0xbbb");
    ASSERT_EQ(w.floors_seen.size(), std::size_t{2});
    EXPECT_EQ(w.floors_seen[0], Floor{804});
    EXPECT_FALSE(w.floors_seen[1].has_value())
        << "the retry still carried a floor, so it would be refused again";
    // The caller is told, once, with the wallet's own words -- that is what
    // lets OfferManager name the pair, side and tier in its warning.
    ASSERT_EQ(f.refusals.size(), std::size_t{1});
    EXPECT_EQ(f.refusals[0], kMinCoinRefusal);
}

TEST(OfferMinInputCoinFallback, ATimeoutIsNeverFollowedByASecondCreate) {
    // THE ONE THAT MATTERS.  A timeout proves only that no answer arrived;
    // the wallet may have built the offer.  A second create would then be a
    // second offer for the same tier.  The script holds a success the code
    // must never reach.
    FakeWallet w;
    w.script = {{StepKind::TransportError,
                 "CURL transport failure: Timeout was reached"},
                {StepKind::Ok, "0xduplicate"}};
    Fallbacks f;
    EXPECT_THROW(run_awaitable(create_offer_with_min_coin_fallback(
                     creator(w), Floor{804}, recorder(f))),
                 xop::rpc::ChiaRPCTransportError);
    EXPECT_EQ(w.floors_seen.size(), std::size_t{1});
    EXPECT_TRUE(f.refusals.empty());
}

TEST(OfferMinInputCoinFallback, ATimeoutQuotingTheRefusalIsStillNotAnAnswer) {
    // The TYPE decides, not the text: only a parsed success=false
    // (ChiaRPCApplicationError) proves the wallet created nothing.
    for (StepKind kind : {StepKind::TransportError, StepKind::BaseError}) {
        FakeWallet w;
        w.script = {{kind, kMinCoinRefusal}, {StepKind::Ok, "0xduplicate"}};
        Fallbacks f;
        EXPECT_THROW(run_awaitable(create_offer_with_min_coin_fallback(
                         creator(w), Floor{804}, recorder(f))),
                     xop::rpc::ChiaRPCError);
        EXPECT_EQ(w.floors_seen.size(), std::size_t{1});
        EXPECT_TRUE(f.refusals.empty());
    }
}

TEST(OfferMinInputCoinFallback, AnyOtherRefusalPropagatesUntouched) {
    // Removing the floor cannot cure these, and the per-tier call site reads
    // the text ("insufficient funds", "spendable balance") to stop the side.
    for (const char* refusal :
         {"Error creating offer: insufficient funds in wallet 5",
          "Error creating offer: Can't select amount higher than our "
          "spendable balance.  Amount: 80334, spendable: 61200",
          "Wallet needs to be fully synced before making transactions."}) {
        FakeWallet w;
        w.script = {{StepKind::ApplicationError, refusal},
                    {StepKind::Ok, "0xduplicate"}};
        Fallbacks f;
        try {
            run_awaitable(create_offer_with_min_coin_fallback(
                creator(w), Floor{804}, recorder(f)));
            ADD_FAILURE() << "no exception for: " << refusal;
        } catch (const xop::rpc::ChiaRPCApplicationError& e) {
            EXPECT_STREQ(e.what(), refusal);
        }
        EXPECT_EQ(w.floors_seen.size(), std::size_t{1}) << refusal;
        EXPECT_TRUE(f.refusals.empty()) << refusal;
    }
}

TEST(OfferMinInputCoinFallback, AFailedRetryPropagatesAndThereIsNoThird) {
    FakeWallet w;
    w.script = {{StepKind::ApplicationError, kMinCoinRefusal},
                {StepKind::ApplicationError, kMinCoinRefusal},
                {StepKind::Ok, "0xthird"}};
    Fallbacks f;
    EXPECT_THROW(run_awaitable(create_offer_with_min_coin_fallback(
                     creator(w), Floor{804}, recorder(f))),
                 xop::rpc::ChiaRPCApplicationError);
    EXPECT_EQ(w.floors_seen.size(), std::size_t{2});
    EXPECT_EQ(f.refusals.size(), std::size_t{1});
}

TEST(OfferMinInputCoinFallback, WithoutAFloorTheSameRefusalIsNotRetried) {
    // The wallet raises the same sentence when pending transactions are the
    // cause.  With no floor sent there is nothing to remove, so a retry
    // would be the identical request.
    FakeWallet w;
    w.script = {{StepKind::ApplicationError, kMinCoinRefusal},
                {StepKind::Ok, "0xduplicate"}};
    Fallbacks f;
    EXPECT_THROW(run_awaitable(create_offer_with_min_coin_fallback(
                     creator(w), Floor{}, recorder(f))),
                 xop::rpc::ChiaRPCApplicationError);
    EXPECT_EQ(w.floors_seen.size(), std::size_t{1});
    EXPECT_TRUE(f.refusals.empty());
}

// ===========================================================================
// 6. Reading the two refusals
// ===========================================================================

TEST(OfferMinInputCoin, RecognisesTheWalletsMinCoinRefusal) {
    EXPECT_TRUE(wallet_refused_for_min_coin(kMinCoinRefusal));
    EXPECT_FALSE(wallet_refused_for_min_coin(
        "Error creating offer: insufficient funds in wallet 5"));
    EXPECT_FALSE(wallet_refused_for_min_coin(
        "Can't select amount higher than our spendable balance.  Amount: "
        "80334, spendable: 61200"));
    EXPECT_FALSE(wallet_refused_for_min_coin(
        "Transaction of 80334 mojo would use more than 500 coins. Try "
        "sending a smaller amount"));
    EXPECT_FALSE(wallet_refused_for_min_coin(""));
}

TEST(OfferMinInputCoin, RecognisesDexiesTooManyInputsRefusal) {
    EXPECT_TRUE(dexie_rejected_too_many_inputs(kDexieTooManyInputs));
    EXPECT_TRUE(dexie_rejected_too_many_inputs("Too many input coins"));
    EXPECT_TRUE(dexie_rejected_too_many_inputs("TOO MANY INPUT COINS."));
    EXPECT_TRUE(dexie_rejected_too_many_inputs("x too many input coins"));
    // Other refusals Dexie sends for an offer must not raise this alarm.
    EXPECT_FALSE(dexie_rejected_too_many_inputs(
        R"({"success":false,"error_message":"Invalid Offer"})"));
    EXPECT_FALSE(dexie_rejected_too_many_inputs("too many input coin"));
    EXPECT_FALSE(dexie_rejected_too_many_inputs("too many requests"));
    EXPECT_FALSE(dexie_rejected_too_many_inputs(""));
}
