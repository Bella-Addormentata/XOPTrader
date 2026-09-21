#ifndef XOP_EXECUTION_OFFER_MIN_INPUT_COIN_HPP
#define XOP_EXECUTION_OFFER_MIN_INPUT_COIN_HPP
// ---------------------------------------------------------------------------
// offer_min_input_coin.hpp -- keep reward dust out of the offers we create,
// on the FIRST create for each offer.  Not a guarantee about every offer:
// THE FALLBACK below re-sends the create with no floor at all after the
// wallet refuses the floored one, and the offer that retry builds is as
// exposed to dust as it was before this file existed.
//
// [MIN-INPUT-COIN 2026-09-19] Dexie pays liquidity rewards as one tiny coin
// per rewarded offer, so a CAT wallet that earns them fills with dust.  On
// 2026-09-19 the DBX wallet held 4,575 unspent coins; 4,389 of them were
// under 0.1 DBX and together came to 84.6 DBX.  The wallet's coin selection
// (chia/wallet/coin_selection.py, knapsack_coin_algorithm) minimises the
// overshoot above the target, and dust is what gets it closest, so an
// XCH/DBX bid paying 80.334 DBX came out as a 62,228-character offer --
// about 135 inputs at the 459 characters per input measured below -- that
// Dexie refused with HTTP 400 "Too many input coins".  The offer still
// existed in the wallet, locked its coins, and was listed nowhere.
// engine.log holds 13 such refusals, 2026-09-10 to 09-19.
//
// WHAT THE WALLET ACCEPTS (chia-blockchain 2.7.4, read from source)
// -----------------------------------------------------------------
// create_offer_for_ids is a tx_endpoint (chia/wallet/wallet_rpc_metadata.py),
// and tx_endpoint builds its coin-selection config from the TOP LEVEL of the
// request: TXConfigLoader.from_json_dict(request) in
// chia/wallet/wallet_rpc_api.py.  The keys are min_coin_amount,
// max_coin_amount, excluded_coin_amounts, excluded_coin_ids,
// included_coin_ids, primary_coin and reuse_puzhash
// (chia/wallet/util/tx_config.py).  min_coin_amount is a uint64 in MOJOS OF
// WHICHEVER WALLET IS SELECTING, and the filter is inclusive:
// `min_coin_amount <= coin.amount <= max_coin_amount`.
//
// ONE VALUE GOVERNS EVERY SELECTION IN THE REQUEST.  TradeManager passes the
// same action_scope.config.tx_config to the offered asset's
// get_coins_to_offer AND, for a CAT-funded offer that pays a fee, to
// CATWallet.create_tandem_xch_tx, which selects the XCH fee coin under it.
// So the number sent for a CAT leg is also a floor on the fee coin, read as
// XCH mojos.  It is small: a CAT has 1,000 mojos per unit and XCH has 10^12,
// so the 804-mojo floor of the offer above is 0.000000000804 XCH, and even a
// 1,000,000-unit CAT offer puts the fee-coin floor at 0.00001 XCH.
// test_offer_min_input_coin pins those magnitudes.
//
// SMALL IS NOT THE SAME AS INERT [review #162].  When the floor excludes the
// XCH coin the default selection would have taken, the wallet skips it and
// locks the next larger one -- typically a whole pool coin.  The XCH lock
// ledger therefore takes the same floor (CoinLockLedger::try_lock /
// try_lock_floor_only, ledger_min_coin_mojos below), so what it charges is
// what the wallet locks.
//
// WHEN THAT HAPPENS [review #162, round 3 -- CORRECTING THIS COMMENT].
// Earlier revisions said the effect "needs only floor > fee" and derived
// thresholds from fee / fraction.  That condition is neither necessary nor
// sufficient, and the true one is simpler:
//
//   The floor changes fee-coin selection exactly when the XCH wallet holds a
//   coin BELOW the floor.  The fee decides only whether excluding that coin
//   changes the answer -- never whether the exclusion happens.
//
// Why: chia filters the candidate set BEFORE it chooses a branch.
// select_coins builds valid_spendable_coins from
// coin_selection_config.filter_coins(...) as its first act
// (chia/wallet/coin_selection.py 2.7.4), and CoinSelectionConfig::filter_coins
// keeps a coin only when min_coin_amount <= coin.amount <= max_coin_amount
// (chia/wallet/util/tx_config.py).  Everything after that reads the filtered
// set: the exact-match probe, the smaller_coin_sum accumulation, the
// == / < / > branch choice, knapsack_coin_algorithm's input set,
// sum_largest_coins and select_smallest_coin_over_target.  Removing one coin
// can therefore flip the BRANCH, whatever that coin's size relative to the
// fee.
//
// NOT NECESSARY: cpp/tests/test_coin_lock_ledger.cpp,
// TheKnapsackUsesOnlyCoinsTheWalletCanSee, runs coins {5M, 5M, 6M, 1 XCH}
// against a 10M fee under a 5.5M floor -- a floor UNDER the fee.  Unfiltered,
// smaller_coins = {6M, 5M, 5M} sums to 16M > 10M and the knapsack finds the
// exact {5M, 5M}; filtered, smaller_coins = {6M} sums to 6M < 10M, so the
// wallet takes the smallest coin over the target: a whole 1 XCH.
// NOT SUFFICIENT: a floor of 100M over a pool whose every coin is at least
// 100M changes nothing, which AFloorNoCoinFallsUnderChangesNothing pins.
//
// WHAT THE LIVE DEPLOYMENT MAKES OF THAT, AS OF 2026-09-21.  The interaction
// is inert there -- because of the COIN SET, not because of the fee.
// Measured read-only on 2026-09-21 (chia rpc wallet get_spendable_coins and
// get_coin_records, wallet_id 1): the XCH wallet held 54 unspent coins, the
// smallest 13,494,209,440 mojos, and of the 30 spendable ones the smallest
// was 20,757,615,448 mojos.  The largest floor this bot can emit is bounded
// by the CAT balance one offer could spend -- that day 1,844,501 DBX mojos,
// so 18,446 at the shipped 0.01 and 1,844,501 even at a fraction near 1.
// Both are more than five orders of magnitude under the smallest XCH coin,
// so nothing is filtered out and no selection changes.
//
// STATE, NOT POLICY.  That is a property of today's coin set and it moves:
// the same smallest spendable coin read 20,787,615,448 mojos in the
// 2026-09-20 snapshot and 20,757,615,448 on 2026-09-21, exactly 30,000,000
// lower -- what two 15,000,000-mojo fee spends would do, though only the two
// readings are measured and the attribution is an inference.  Any spend
// leaving small change can put a coin under a floor.  fees.min_fee_mojos
// does NOT govern reachability --
// lowering the live 15,000,000 back to 5,000 would not by itself make the
// interaction reachable, and raising it would not prevent it.  The floor is
// modelled in the ledger for exactly that reason: a coin set is not a knob
// anyone sets, and the modelling costs one parameter.
//
// THE LEDGER SEES THE SAME COINS THE WALLET DOES.  Its pool is seeded
// directly from wallet_->get_spendable_coins(1) in
// OfferManager::begin_xch_lock_cycle() (cpp/src/execution/offer_manager.cpp),
// NOT from CoinManager's pool, so CoinManager's 1,000,000-mojo dust
// threshold does not bound it and sub-1,000,000-mojo XCH coins are visible
// to the ledger and to the wallet alike.
//
// THE RULE
// --------
// min input coin = ceil(offered mojos x strategy.offer_min_input_coin_frac),
// applied to the asset the offer SPENDS.  Relative, so it needs no per-asset
// constant, and it bounds the input count: every selected coin is at least
// `frac` of the target, so ceil(1 / frac) of them always reach it -- 100 at
// the default 0.01, plus one fee coin.  Dexie does not publish its limit;
// measured from this bot's own submissions it accepted 125 inputs (57,382
// characters, about 459 per input) and refused everything from 60,612
// characters up, which puts the limit between 125 and about 132.
//
// XCH-FUNDED OFFERS ARE LEFT ALONE.  XCH coins are already shaped by
// CoinManager's pool (ensure_split) and budgeted by the XCH lock ledger,
// which models whole-coin locking under the wallet's default selection.
// The dust observed is CAT reward payouts, and changing which XCH coin the
// wallet picks would change what that ledger assumes.
//
// THE FALLBACK, AND WHY IT CANNOT DOUBLE-POST
// -------------------------------------------
// When the coins at or above the floor cannot cover the amount, the wallet
// refuses: "Transaction for N is greater than max spendable balance in a
// block of M. There may be other transactions pending or our minimum coin
// amount is too high." (coin_selection.py).  The create is then sent ONCE
// more without the floor.  Only that ANSWER triggers it -- a
// ChiaRPCApplicationError, i.e. a parsed success=false.  A timeout or any
// other transport failure is not an answer (rpc/rpc_retry_policy.hpp) and is
// never followed by a second create from here.
//
// [review #162, round 2] A refusal proves the wallet built nothing ONLY IF
// it answers the one request the wallet saw.  rpc_post returns just its last
// attempt, and it used to re-send this endpoint after a timeout: the refusal
// could then answer a copy whose original had already built the offer and
// lost its reply -- the original's coin locks being the very reason the copy
// was refused.  create_offer_for_ids is therefore NeverResend, and the
// static_assert beside the fallback makes reverting that a build error.
//
// Pure apart from the one coroutine template, which takes the wallet call as
// a parameter so cpp/tests/test_offer_min_input_coin.cpp can drive a refusing
// wallet (ChiaWalletRPC is final and this repo has no gmock; same seam as
// CoinManager::collect_spendable_coins).
// ---------------------------------------------------------------------------

#include <boost/asio/awaitable.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "xop/rpc/chia_rpc.hpp"
#include "xop/rpc/rpc_retry_policy.hpp"

namespace xop::execution {

/// The fraction is applied in parts per billion, ROUNDED UP, so the
/// arithmetic below is exact integer arithmetic and gives one answer on
/// every compiler.
inline constexpr std::uint64_t kMinInputCoinFracScale = 1'000'000'000ULL;

/// Wallet id 1 is the XCH wallet; offer_dict keys are wallet ids as strings.
inline constexpr std::string_view kXchWalletIdKey = "1";

/// The configured fraction in parts per billion; 0 means "send no floor".
///
/// 0 for anything outside (0, 1), NaN included (both comparisons are false
/// for NaN).  config.cpp rejects those values at load, so this is the second
/// line: a bad fraction degrades to the pre-existing request, never to a
/// floor nobody chose.
///
/// ROUNDED UP, NEVER TO NEAREST [review #162].  The input bound needs the
/// applied fraction to be at least the configured one: k = ceil(1 / frac)
/// coins reach the amount only if k x applied >= 1.  Rounding to nearest
/// rounds half of all fractions DOWN, and it broke the bound for ordinary
/// values, not exotic ones: 1/3 became 333,333,333 ppb, and three floor-sized
/// coins of a 1,000,000,000-mojo offer then totalled 999,999,999.  Rounded up,
/// the applied fraction can exceed the configured one by under 1 ppb, which
/// only raises the floor.  An operator-entered decimal can land one ppb high
/// for the same reason (its double is a hair above it); 0.01 does not.
///
/// The result is in [1, scale]: a positive fraction below the resolution is
/// still "on" (1 ppb), and one just under 1 reaches the full scale, where
/// the floor equals the amount offered -- never more.
[[nodiscard]] constexpr std::uint64_t min_input_coin_frac_ppb(
    double frac) noexcept
{
    if (!(frac > 0.0) || !(frac < 1.0)) return 0;
    // In (0, 10^9], so the cast is defined, and truncation is floor.
    const double scaled = frac * static_cast<double>(kMinInputCoinFracScale);
    auto ppb = static_cast<std::uint64_t>(scaled);
    if (static_cast<double>(ppb) < scaled) ++ppb;
    return ppb;
}

/// ceil(offered_mojos x frac), or nullopt when no floor should be sent
/// (fraction off or invalid, or nothing offered).
///
/// CEIL IS LOAD-BEARING: with every input at least amount x frac,
/// ceil(1 / frac) inputs always reach the amount.  Rounding down would admit
/// coins just under that and lose the bound.
///
/// Always in [1, offered_mojos], so a coin exactly the size of the offer is
/// never excluded.  Split at the scale so neither product can wrap:
/// r x ppb < 10^18 and q x ppb <= offered_mojos for any uint64 amount, the
/// full scale included.
[[nodiscard]] constexpr std::optional<std::uint64_t> min_input_coin_mojos(
    std::uint64_t offered_mojos, double frac) noexcept
{
    const std::uint64_t ppb = min_input_coin_frac_ppb(frac);
    if (ppb == 0 || offered_mojos == 0) return std::nullopt;
    const std::uint64_t q    = offered_mojos / kMinInputCoinFracScale;
    const std::uint64_t r    = offered_mojos % kMinInputCoinFracScale;
    const std::uint64_t part = r * ppb;
    const std::uint64_t round_up =
        (part % kMinInputCoinFracScale != 0) ? 1u : 0u;
    return q * ppb + part / kMinInputCoinFracScale + round_up;
}

// Evaluated by the compiler, so GCC and MSVC agree or the build fails.
static_assert(min_input_coin_mojos(80'334, 0.01) == 804u,
              "the 2026-09-19 XCH/DBX bid: 80.334 DBX -> 0.804 DBX floor");
static_assert(min_input_coin_mojos(100'000, 0.01) == 1'000u);
static_assert(min_input_coin_mojos(100'001, 0.01) == 1'001u, "ceil, not floor");
static_assert(min_input_coin_mojos(1, 0.01) == 1u);
static_assert(!min_input_coin_mojos(100'000, 0.0).has_value(), "0 disables");
static_assert(!min_input_coin_mojos(100'000, 1.0).has_value());
static_assert(!min_input_coin_mojos(0, 0.01).has_value());
static_assert(min_input_coin_mojos(UINT64_MAX, 0.01)
              == 184'467'440'737'095'517ULL);
// [review #162] The fraction is rounded UP to ppb.  To nearest, the first is
// 10,101,010 and 99 such coins total 999,999,990; the second is 333,333,333.
static_assert(min_input_coin_mojos(1'000'000'000, 0.0101010102)
              == 10'101'011u);
static_assert(min_input_coin_mojos(1'000'000'000, 1.0 / 3.0) == 333'333'334u);

/// The floor to send for the offer this offer_dict describes, or nullopt.
///
/// offer_dict is what the wallet receives: wallet id (string) -> signed
/// mojos, negative = we spend.  A floor is returned only for the one shape
/// the engine builds and this rule was designed for: EXACTLY ONE spend leg,
/// and that leg not XCH.  Anything else -- an XCH-funded offer, no spend
/// leg, two spend legs, a non-integer amount -- gets nullopt, i.e. the
/// request the engine sent before this existed.  A merged batch is still one
/// spend leg (the tiers are summed per wallet id), so its floor scales with
/// the merged amount.
[[nodiscard]] inline std::optional<std::uint64_t> offer_min_input_coin(
    const nlohmann::json& offer_dict, double frac)
{
    if (!offer_dict.is_object()) return std::nullopt;
    std::optional<std::uint64_t> spent;
    for (const auto& [wallet_id, amount] : offer_dict.items()) {
        if (!amount.is_number_integer()) return std::nullopt;
        // An unsigned value is a receive leg, and reading one above
        // INT64_MAX as int64 would wrap it into a spend.
        if (amount.is_number_unsigned()) continue;
        const auto v = amount.get<std::int64_t>();
        if (v >= 0) continue;
        if (spent.has_value()) return std::nullopt;
        if (wallet_id == kXchWalletIdKey) return std::nullopt;
        // -(v + 1) + 1 rather than -v: defined for INT64_MIN too.
        spent = static_cast<std::uint64_t>(-(v + 1)) + 1u;
    }
    if (!spent.has_value()) return std::nullopt;
    return min_input_coin_mojos(*spent, frac);
}

/// offer_min_input_coin() in the form the XCH lock ledger takes: signed
/// mojos, 0 when no floor is sent.
///
/// [review #162] The wallet applies the one min_coin_amount to the XCH fee
/// coin as well, so the ledger has to admit against the same filtered pool or
/// it charges a coin the wallet will skip.  Deriving both numbers from the
/// one offer_dict is what keeps them equal.  A floor above INT64_MAX (only a
/// spend of INT64_MIN at a fraction near 1 gets there) saturates; wrapped
/// negative it would read as "no floor".
[[nodiscard]] inline std::int64_t ledger_min_coin_mojos(
    const nlohmann::json& offer_dict, double frac)
{
    constexpr std::uint64_t kMax = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    const std::uint64_t coin_floor =
        offer_min_input_coin(offer_dict, frac).value_or(0);
    return static_cast<std::int64_t>(coin_floor > kMax ? kMax : coin_floor);
}

/// Did the wallet refuse a create because the coins at or above
/// min_coin_amount cannot cover the amount?
///
/// Matches the sentence the wallet itself uses to name that cause
/// (coin_selection.py, the `sum_spendable_coins < amount` branch).  The same
/// text is raised when the shortfall comes from pending transactions, which
/// is indistinguishable here; the one retry it triggers is then refused for
/// the same reason and costs one round trip.
[[nodiscard]] inline bool wallet_refused_for_min_coin(
    std::string_view wallet_error) noexcept
{
    return wallet_error.find("minimum coin amount")
        != std::string_view::npos;
}

/// Is this Dexie rejection body the "Too many input coins" refusal?
/// ASCII case-insensitive, so a recapitalised message still matches.
[[nodiscard]] inline bool dexie_rejected_too_many_inputs(
    std::string_view body) noexcept
{
    constexpr std::string_view needle = "too many input coins";
    if (body.size() < needle.size()) return false;
    const auto lower = [](char c) noexcept {
        return (c >= 'A' && c <= 'Z')
            ? static_cast<char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i + needle.size() <= body.size(); ++i) {
        std::size_t k = 0;
        while (k < needle.size() && lower(body[i + k]) == needle[k]) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

/// Create an offer with the floor, falling back ONCE to no floor when the
/// wallet answers that the floor left too little to spend.
///
/// @param create       optional<uint64_t> min_coin_amount -> awaitable<json>;
///                     production passes ChiaWalletRPC::create_offer.
/// @param min_coin     offer_min_input_coin(); nullopt sends one plain create.
/// @param on_fallback  Called with the wallet's refusal text just before the
///                     second create, so the caller can log pair/side/tier.
///
/// At most two creates are ever sent, and the second only after the wallet
/// ANSWERED the first with a refusal, so at most one offer can exist --
/// given that `create` never re-sends a request the wallet may have received
/// (the static_assert below).  Every
/// other failure -- ChiaRPCTransportError above all: a timeout says nothing
/// about whether the wallet built the offer -- propagates untouched from
/// whichever create raised it, to the handling the call site already had.
///
/// All parameters BY VALUE: this is a coroutine, and a reference parameter
/// would dangle across the first suspension.
// The premise of the fallback below, checked where it is relied on: if this
// endpoint is ever re-sent after a possibly-delivered failure again, a refusal
// stops proving that no offer exists, and the retry can create a second one.
static_assert(rpc::retry_policy_for_endpoint("create_offer_for_ids")
                  == rpc::RpcRetryPolicy::NeverResend,
              "create_offer_with_min_coin_fallback needs create_offer_for_ids "
              "to be NeverResend");

template <class Create, class OnFallback>
boost::asio::awaitable<nlohmann::json> create_offer_with_min_coin_fallback(
    Create                       create,
    std::optional<std::uint64_t> min_coin,
    OnFallback                   on_fallback)
{
    if (!min_coin.has_value()) {
        co_return co_await create(std::optional<std::uint64_t>{});
    }
    // co_await cannot appear inside a catch handler, so the refusal is
    // carried out of it and the retry happens below.
    std::string refusal;
    try {
        co_return co_await create(min_coin);
    } catch (const rpc::ChiaRPCApplicationError& e) {
        if (!wallet_refused_for_min_coin(e.what())) throw;
        refusal = e.what();
    }
    on_fallback(refusal);
    co_return co_await create(std::optional<std::uint64_t>{});
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_OFFER_MIN_INPUT_COIN_HPP
