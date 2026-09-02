/**
 * @file coin_manager.cpp
 * @brief Implementation of the CHIA UTXO (coin-set) manager.
 *
 * See coin_manager.hpp for the full interface contract and design rationale.
 *
 * Key implementation notes:
 *   - All coin queries go through the wallet RPC (get_spendable_coins,
 *     get_wallet_balance).  The wallet tracks confirmed vs. pending state.
 *   - The locked-coin set is purely application-level bookkeeping: coins
 *     locked by create_offer() are flagged here so they are excluded from
 *     subsequent get_spendable_coins() results, preventing double-spend
 *     attempts across concurrent offers.
 *   - Coin splitting uses the wallet's send_transaction RPC to self-send
 *     XCH, producing multiple outputs of the target denomination.  This
 *     is the standard approach recommended by the Chia documentation for
 *     pre-splitting coins before offer creation.
 *   - coin_name derivation: SHA-256(parent_coin_info || puzzle_hash || amount)
 *     where amount is encoded as a CLVM-style big-endian integer with minimal
 *     bytes (no leading zeros for positive values, except when the high bit
 *     is set to preserve sign).  Implemented via OpenSSL EVP.
 *
 * ISO/IEC 27001:2022 -- coin_name hashes are not secrets; logged freely.
 * ISO/IEC 5055       -- all arithmetic checked; no UB on mojo conversions.
 * ISO/IEC 25000      -- structured logging; single-responsibility helpers.
 */

#include <xop/execution/coin_manager.hpp>

#include <algorithm>
#include <mutex>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>         // std::numeric_limits -- overflow guards (HIGH-1, HIGH-3)
#include <sstream>
#include <stdexcept>
#include <iomanip>

// OpenSSL EVP interface for SHA-256 (T1-04: proper coin name computation).
// EVP is the recommended API as of OpenSSL 3.x; the legacy SHA256_*
// functions are deprecated.  Linked via OpenSSL::Crypto in CMake.
#include <openssl/evp.h>

namespace xop::execution {

namespace {

constexpr Mojo kPoolReadyMinDivisor = 2;
constexpr Mojo kPoolReadyMaxMultiple = 2;
constexpr int  kMaxCoinsPerSplit     = 500;  // Chia wallet per-tx limit

}  // namespace

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

CoinManager::CoinManager(asio::io_context&                    /*ioc*/,
                         std::shared_ptr<rpc::ChiaWalletRPC>  wallet,
                         const AppConfig&                     config)
    : wallet_(std::move(wallet))
    , logger_(spdlog::default_logger()->clone("CoinMgr"))
{
    default_split_fee_ = static_cast<Mojo>(config.strategy.offer_fee_mojos);
    if (config.strategy.coin_pool_target_count > 0
        && config.strategy.coin_pool_target_xch > 0.0) {
        xch_pool_target_mojos_ = static_cast<Mojo>(std::llround(
            config.strategy.coin_pool_target_xch
            * static_cast<double>(kMojosPerXch)));
    }

    // Dust threshold: coins smaller than 1,000,000 mojos (0.000001 XCH)
    // are ignored to avoid unnecessary UTXO bloat.
    dust_threshold_ = 1'000'000LL;

    logger_->info("CoinManager initialised: split_fee={} mojos, "
                  "dust_threshold={} mojos",
                  default_split_fee_, dust_threshold_);
}

// ---------------------------------------------------------------------------
// [S41 2026-09-01] get_balance_xch() and get_balance_mojos() were DELETED
// here. Both had zero callers repo-wide. get_balance_mojos() returned 0 on an
// unexpected response shape and 0 again on ChiaRPCError -- reporting an empty
// wallet when the read had failed, which is the exact defect this commit
// fixes in get_spendable_coins(). Leaving it in place while fixing its twin
// would have shipped the next instance of the family in the same commit.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// get_spendable_coins -- enumerate unlocked spendable coins
// ---------------------------------------------------------------------------

// [S41 2026-09-01] nullopt means THE READ DID NOT HAPPEN. An engaged optional
// holding an empty vector means the wallet genuinely has nothing usable. This
// function used to return the same empty vector for both, so 68 failed reads
// became 34 phantom "Have 0 mojos total" claims about a wallet that held ~59
// XCH, and each one reached ensure_split(). See coin_pool_verdict.hpp.
//
// EVERYTHING THAT CAN GO WRONG HERE LIVES IN collect_spendable_coins(), a
// static template in coin_manager.hpp taking the wallet call as a parameter.
// That is not decoration: the first version of this fix put the two catches
// in this file, where no test could reach them, and a review demonstrated the
// consequence by restoring the fail-open inside the ChiaRPCError catch and
// watching all 1109 tests stay green. The seam exists so that
// cpp/tests/test_coin_pool_verdict.cpp can drive a throwing fetch and a
// malformed record and observe the RETURNED VALUE, which is where the bug
// was. This function is now only the wiring: which wallet, which dust
// threshold, which locked set.
asio::awaitable<std::optional<std::vector<CoinInfo>>>
CoinManager::get_spendable_coins(std::int64_t wallet_id)
{
    // The locked set is consulted through a predicate rather than copied, so
    // the mutex is still taken per coin AFTER the RPC returns -- a snapshot
    // taken before the await would miss a coin locked while it was in flight
    // and could hand out a coin an offer had claimed.
    co_return co_await collect_spendable_coins(
        [wallet = wallet_](std::int64_t wid)
            -> asio::awaitable<std::vector<json>> {
            return wallet->get_spendable_coins(wid);
        },
        wallet_id,
        dust_threshold_,
        [this](const std::string& coin_name) {
            std::lock_guard<std::mutex> lock(mtx_locked_);
            return locked_coins_.count(coin_name) > 0;
        },
        logger_);
}

// ---------------------------------------------------------------------------
// count_free_coins -- number of unlocked spendable coins
// ---------------------------------------------------------------------------

asio::awaitable<std::optional<int>> CoinManager::count_free_coins(
    std::int64_t wallet_id)
{
    // [S41] Propagate "I could not count". Note there is deliberately no
    // .value_or(0) here and there must never be one -- that single token is
    // the whole bug, relocated.
    auto coins = co_await get_spendable_coins(wallet_id);
    if (!coins.has_value()) {
        co_return std::nullopt;
    }

    if (wallet_id == 1 && xch_pool_target_mojos_ > 0) {
        co_return static_cast<int>(count_pool_ready_coins(
            *coins, xch_pool_target_mojos_));
    }
    co_return static_cast<int>(coins->size());
}

bool CoinManager::is_pool_ready_coin(Mojo coin_amount_mojos,
                                     Mojo target_amount_mojos)
{
    if (coin_amount_mojos <= 0 || target_amount_mojos <= 0) {
        return false;
    }

    const Mojo min_amount = target_amount_mojos / kPoolReadyMinDivisor
                          + (target_amount_mojos % kPoolReadyMinDivisor);
    const Mojo max_amount =
        target_amount_mojos > std::numeric_limits<Mojo>::max()
                                / kPoolReadyMaxMultiple
            ? std::numeric_limits<Mojo>::max()
            : target_amount_mojos * kPoolReadyMaxMultiple;

    return coin_amount_mojos >= min_amount && coin_amount_mojos <= max_amount;
}

std::size_t CoinManager::count_pool_ready_coins(
    const std::vector<CoinInfo>& coins,
    Mojo                         target_amount_mojos)
{
    return static_cast<std::size_t>(std::count_if(
        coins.begin(), coins.end(),
        [target_amount_mojos](const CoinInfo& coin) {
            return is_pool_ready_coin(coin.amount, target_amount_mojos);
        }));
}

CoinManager::SplitPlan CoinManager::plan_split_for_coin(
    Mojo amount_mojos, int needed, Mojo target_amount_mojos, Mojo fee)
{
    SplitPlan plan;
    // The funding gate subtracts instead of adding: target + fee could
    // overflow signed Mojo for arguments near the type limit (review).
    if (amount_mojos <= 0 || needed <= 0 || target_amount_mojos <= 0
        || fee < 0 || fee >= amount_mojos
        || amount_mojos - fee < target_amount_mojos) {
        return plan;
    }

    // Preferred: as many target-denomination coins as still improve the pool.
    // Cap the quotient in the Mojo domain BEFORE narrowing: a small target
    // against a large coin overflows int, and that conversion is
    // implementation-defined (review).
    const Mojo raw_from_coin = (amount_mojos - fee) / target_amount_mojos;
    const int  max_from_coin =
        raw_from_coin > static_cast<Mojo>(kMaxCoinsPerSplit)
            ? kMaxCoinsPerSplit
            : static_cast<int>(raw_from_coin);
    int candidate = std::min(needed, max_from_coin);
    while (candidate > 0
           && !split_improves_pool_ready_count(
               amount_mojos, candidate, target_amount_mojos, fee)) {
        --candidate;
    }
    if (candidate >= 1) {
        plan.split_amount = target_amount_mojos;
        plan.batch        = candidate;
        return plan;
    }

    // [COIN-POOL-DEADLOCK 2026-08-23] Fallback for the window where the
    // post-fee value (amount - fee) lies in [target, 1.5*target): the
    // batch-1 target split's change (amount - fee - target) lands below
    // the band, so no target plan improves -- but two equal halves of
    // (amount - fee) both land IN the band.  batch=1 of half: the wallet's
    // change output is the other half (same size, +1 mojo when odd), so
    // nothing dusty is created and the pool-ready count goes 1 -> 2.  The
    // is_pool_ready_coin checks below are the authoritative boundary,
    // exact to integer rounding (review: with fee > 0 the literal
    // source-amount interval is shifted by the fee).
    const Mojo half   = (amount_mojos - fee) / 2;
    const Mojo change = amount_mojos - fee - half;
    const int  current_ready =
        is_pool_ready_coin(amount_mojos, target_amount_mojos) ? 1 : 0;
    if (half > 0
        && is_pool_ready_coin(half, target_amount_mojos)
        && is_pool_ready_coin(change, target_amount_mojos)
        && 2 > current_ready) {
        plan.split_amount = half;
        plan.batch        = 1;
    }
    return plan;
}

bool CoinManager::split_improves_pool_ready_count(Mojo source_amount_mojos,
                                                  int  batch,
                                                  Mojo target_amount_mojos,
                                                  Mojo fee)
{
    if (source_amount_mojos <= 0 || batch <= 0
        || target_amount_mojos <= 0 || fee < 0) {
        return false;
    }

    if (static_cast<Mojo>(batch) >
        (std::numeric_limits<std::int64_t>::max() - fee) / target_amount_mojos) {
        return false;
    }

    const Mojo spend = static_cast<Mojo>(batch) * target_amount_mojos + fee;
    if (spend > source_amount_mojos) {
        return false;
    }

    const Mojo remainder = source_amount_mojos - spend;
    const int current_ready =
        is_pool_ready_coin(source_amount_mojos, target_amount_mojos) ? 1 : 0;

    int future_ready = batch;
    if (remainder > 0 && is_pool_ready_coin(remainder, target_amount_mojos)) {
        ++future_ready;
    }

    return future_ready > current_ready;
}

// [S41 2026-09-01] The awaitable count_pool_ready_coins(wallet_id, target)
// overload was DELETED here: zero callers repo-wide, and it returned 0 on a
// failed read. The static (coins, target) overload above is the live one.

// ---------------------------------------------------------------------------
// ensure_split -- pre-split large coins into target denominations
// ---------------------------------------------------------------------------

asio::awaitable<SplitResult> CoinManager::ensure_split(
    std::int64_t       wallet_id,
    int                target_count,
    Mojo               target_amount_mojos,
    Mojo               fee)
{
    SplitResult result;

    // Step 1: Count current pool-ready coins.
    //
    // [S41 2026-09-01] The read is checked BEFORE anything derived from it is
    // computed. This function is a SPEND, and every downstream step -- the
    // needed count, the balance sum, the candidate search -- is meaningless if
    // the enumeration never happened. It used to proceed on an empty vector
    // and reach the insufficient-balance branch, which logged "Have 0 mojos
    // total" about a wallet the function had never successfully read. That it
    // did not also spend was arithmetic luck (0 < target + fee), not a guard.
    auto maybe_free_coins = co_await get_spendable_coins(wallet_id);
    if (!maybe_free_coins.has_value()) {
        logger_->error("ensure_split: coin enumeration FAILED for wallet_id "
                       "{} -- the pool state is unknown, so no split is "
                       "authorised. This is NOT an insufficient balance.",
                       wallet_id);
        co_return SplitResult{.success = false, .read_failed = true};
    }
    auto free_coins = std::move(*maybe_free_coins);

    const int current_count = static_cast<int>(count_pool_ready_coins(
        free_coins, target_amount_mojos));

    if (current_count >= target_count) {
        logger_->info("ensure_split: already have {} pool-ready coins for "
                      "target {} mojos ({} free total), no split needed",
                      current_count, target_amount_mojos,
                      free_coins.size());
        result.success = true;
        result.coins_created = 0;
        co_return result;
    }

    int needed = target_count - current_count;

    // Step 2: Calculate total mojos required for the split.
    // Each new coin will be target_amount_mojos, plus the fee.
    //
    // HIGH-1 FIX: Overflow guard -- needed * target_amount_mojos can exceed
    // int64_t range for large coin counts or denominations.  Validate before
    // the multiplication to prevent undefined behaviour (signed overflow).
    // ISO/IEC 5055 -- CWE-190 (integer overflow) prevention.
    // ISO/IEC 27001:2022 -- deterministic error reporting on arithmetic fault.
    if (needed > 0 && target_amount_mojos > 0 &&
        static_cast<Mojo>(needed) >
            (std::numeric_limits<std::int64_t>::max() - fee)
            / target_amount_mojos) {
        logger_->error("coin_manager: split total_needed overflows int64_t "
                       "(needed={}, target={}, fee={})",
                       needed, target_amount_mojos, fee);
        co_return SplitResult{.success = false, .tx_id = "overflow in total_needed calculation"};
    }
    // Step 3: Verify we have sufficient balance.
    //
    // HIGH-3 FIX: Overflow guard on cumulative summation -- if a wallet
    // holds many large coins, total_available may exceed int64_t range.
    // ISO/IEC 5055 -- CWE-190 (integer overflow) prevention.
    // ISO/IEC 27001:2022 -- deterministic error reporting on arithmetic fault.
    Mojo total_available = 0;
    for (const auto& c : free_coins) {
        if (total_available > std::numeric_limits<std::int64_t>::max() - c.amount) {
            logger_->error("coin_manager: balance summation overflow");
            co_return SplitResult{.success = false, .tx_id = "balance overflow"};
        }
        total_available += c.amount;
    }

    if (total_available < target_amount_mojos + fee) {
        logger_->error("ensure_split: insufficient balance to create even 1 "
                       "coin of {} mojos (+ {} fee). Have {} mojos total",
                       target_amount_mojos, fee, total_available);
        result.success = false;
        co_return result;
    }

    // Step 4: Execute the split as a single atomic transaction.
    //
    // Uses the Chia wallet RPC "split_coins" endpoint to create coins
    // in one spend bundle -- one block confirmation, one fee.
    // This replaces the previous sequential send_transaction approach which
    // suffered from change-coin locking (each send locks the change output,
    // so only ~1 coin could be created per block).
    //
    // Strategy: sort free coins by amount descending, pick the largest one,
    // and take its best improving split (target denominations, or the
    // half-split fallback -- see plan_split_for_coin).

    // Sort free coins by amount descending to find best candidate.
    std::sort(free_coins.begin(), free_coins.end(),
              [](const CoinInfo& a, const CoinInfo& b) {
                  return a.amount > b.amount;
              });

    // Find the largest coin with an improving split plan.
    const CoinInfo* source_coin = nullptr;
    SplitPlan plan;
    for (const auto& c : free_coins) {
        if (c.coin_name.empty()) {
            continue;
        }
        plan = plan_split_for_coin(c.amount, needed, target_amount_mojos, fee);
        if (plan.batch < 1) {
            continue;
        }
        source_coin = &c;
        break;  // Largest coin first -- best candidate.
    }

    if (!source_coin || plan.batch < 1) {
        logger_->error("ensure_split: no coin can improve the pool-ready "
                       "count for target {} mojos (+ {} fee). "
                       "Largest free coin: {} mojos",
                       target_amount_mojos, fee,
                       free_coins.empty() ? 0 : free_coins[0].amount);
        result.success = false;
        co_return result;
    }

    logger_->info("ensure_split: splitting coin {} ({} mojos) into {} "
                  "coins of {} mojos each (need {}, fee={})",
                  source_coin->coin_name.substr(0, 16),
                  source_coin->amount,
                  plan.batch, plan.split_amount, needed, fee);

    try {
        json split_resp = co_await wallet_->split_coins(
            wallet_id,
            source_coin->coin_name,
            plan.batch,
            plan.split_amount,
            fee);

        result.coins_created = plan.batch;
        result.fee_paid      = fee;
        result.success       = true;

        // Extract transaction ID if available.
        if (split_resp.contains("transaction") &&
            split_resp["transaction"].contains("name")) {
            result.tx_id = split_resp["transaction"]["name"]
                           .get<std::string>();
        }

        logger_->info("ensure_split: created {} coins of {} mojos each "
                      "(fee {} mojos, tx={})",
                      plan.batch, plan.split_amount, fee,
                      result.tx_id.empty() ? "(none)" :
                      result.tx_id.substr(0, 16));

    } catch (const rpc::ChiaRPCError& e) {
        logger_->error("ensure_split: split_coins RPC error: {}", e.what());
        result.success = false;
        co_return result;

    } catch (const std::exception& e) {
        logger_->error("ensure_split: unexpected error: {}", e.what());
        result.success = false;
        co_return result;
    }

    co_return result;
}

// ---------------------------------------------------------------------------
// lock_coin -- mark a coin as reserved by a pending offer
// ---------------------------------------------------------------------------

void CoinManager::lock_coin(const std::string& coin_name)
{
    std::lock_guard<std::mutex> lock(mtx_locked_);
    auto [it, inserted] = locked_coins_.insert(coin_name);
    if (inserted) {
        logger_->debug("Locked coin {}", coin_name.substr(0, 16));
    }
    // Duplicate lock is idempotent -- no warning needed.
}

// ---------------------------------------------------------------------------
// unlock_coin -- release a previously locked coin
// ---------------------------------------------------------------------------

bool CoinManager::unlock_coin(const std::string& coin_name)
{
    std::lock_guard<std::mutex> lock(mtx_locked_);
    std::size_t erased = locked_coins_.erase(coin_name);
    if (erased > 0) {
        logger_->debug("Unlocked coin {}", coin_name.substr(0, 16));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// unlock_all -- release all locked coins (shutdown path)
// ---------------------------------------------------------------------------

void CoinManager::unlock_all()
{
    std::lock_guard<std::mutex> lock(mtx_locked_);
    std::size_t count = locked_coins_.size();
    locked_coins_.clear();
    logger_->info("unlock_all: released {} coins", count);
}

// ---------------------------------------------------------------------------
// is_locked -- check if a specific coin is currently locked
// ---------------------------------------------------------------------------

bool CoinManager::is_locked(const std::string& coin_name) const
{
    std::lock_guard<std::mutex> lock(mtx_locked_);
    return locked_coins_.count(coin_name) > 0;
}

// ---------------------------------------------------------------------------
// locked_count -- number of coins currently in the locked set
// ---------------------------------------------------------------------------

std::size_t CoinManager::locked_count() const
{
    std::lock_guard<std::mutex> lock(mtx_locked_);
    return locked_coins_.size();
}

// ---------------------------------------------------------------------------
// log_coin_summary -- diagnostic dump of coin state
// ---------------------------------------------------------------------------

void CoinManager::log_coin_summary(std::int64_t wallet_id) const
{
    std::lock_guard<std::mutex> lock(mtx_locked_);
    logger_->info("CoinManager summary: wallet_id={}, locked_coins={}",
                  wallet_id, locked_coins_.size());
}

// ---------------------------------------------------------------------------
// parse_coin -- convert wallet RPC JSON to CoinInfo struct
// ---------------------------------------------------------------------------

CoinInfo CoinManager::parse_coin(const json& coin_json)
{
    CoinInfo ci;

    // The Chia wallet RPC returns coin records with a nested "coin" object.
    // Structure:
    // {
    //   "coin": {
    //     "parent_coin_info": "0x...",
    //     "puzzle_hash": "0x...",
    //     "amount": <int64>
    //   },
    //   "confirmed_block_index": <uint32>,
    //   "coin_name": "0x..."           // may or may not be present
    // }

    const json& coin_obj = coin_json.contains("coin")
                           ? coin_json["coin"]
                           : coin_json;

    // Parent coin info.
    if (coin_obj.contains("parent_coin_info")) {
        ci.parent_id = coin_obj["parent_coin_info"].get<std::string>();
        // Strip the "0x" prefix if present for consistent hex representation.
        if (ci.parent_id.size() > 2 && ci.parent_id.substr(0, 2) == "0x") {
            ci.parent_id = ci.parent_id.substr(2);
        }
    }

    // Puzzle hash.
    if (coin_obj.contains("puzzle_hash")) {
        ci.puzzle_hash = coin_obj["puzzle_hash"].get<std::string>();
        if (ci.puzzle_hash.size() > 2 &&
            ci.puzzle_hash.substr(0, 2) == "0x") {
            ci.puzzle_hash = ci.puzzle_hash.substr(2);
        }
    }

    // Amount.
    if (coin_obj.contains("amount")) {
        ci.amount = coin_obj["amount"].get<Mojo>();
    }

    // Confirmed block height.
    if (coin_json.contains("confirmed_block_index")) {
        ci.confirmed_at = static_cast<BlockHeight>(
            coin_json["confirmed_block_index"].get<std::int64_t>());
    }

    // Coin name: prefer the explicit field if present; otherwise derive it.
    if (coin_json.contains("coin_name")) {
        ci.coin_name = coin_json["coin_name"].get<std::string>();
        if (ci.coin_name.size() > 2 && ci.coin_name.substr(0, 2) == "0x") {
            ci.coin_name = ci.coin_name.substr(2);
        }
    } else if (!ci.parent_id.empty() && !ci.puzzle_hash.empty()) {
        ci.coin_name = compute_coin_name(ci.parent_id, ci.puzzle_hash,
                                         ci.amount);
    }

    return ci;
}

// ---------------------------------------------------------------------------
// hex_to_bytes -- decode a hex string to a byte vector
// ---------------------------------------------------------------------------
// Utility for compute_coin_name: converts 64-hex-character strings
// (parent_coin_info, puzzle_hash) into their 32-byte binary representations.
//
// ISO/IEC 5055 -- validates input length is even; throws on malformed hex.
// ---------------------------------------------------------------------------

static std::vector<std::uint8_t> hex_to_bytes(const std::string& hex)
{
    if (hex.size() % 2 != 0) {
        throw std::invalid_argument(
            "hex_to_bytes: odd-length hex string");
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);

    for (std::size_t i = 0; i < hex.size(); i += 2) {
        // Convert each pair of hex characters to a byte.
        const unsigned int byte_val = static_cast<unsigned int>(
            std::stoul(hex.substr(i, 2), nullptr, 16));
        bytes.push_back(static_cast<std::uint8_t>(byte_val));
    }

    return bytes;
}

// ---------------------------------------------------------------------------
// encode_clvm_int -- encode an integer as a CLVM-style big-endian byte string
// ---------------------------------------------------------------------------
// CLVM integer serialisation rules (Chia specification):
//   - Zero is encoded as an empty byte string (0 bytes).
//   - Positive integers use the minimum number of big-endian bytes.
//   - If the most significant bit of the leading byte is set (i.e. the value
//     would appear negative in two's-complement), a leading 0x00 byte is
//     prepended to preserve the sign.
//   - Negative integers use two's-complement with minimal bytes (not needed
//     here since Mojo amounts are always non-negative).
//
// This encoding is critical for coin_name correctness: using a fixed 8-byte
// big-endian representation (as many implementations assume) produces
// incorrect hashes for amounts that fit in fewer bytes.
//
// Reference: https://chialisp.com/docs/ref/clvm#atoms
// ISO/IEC 5055 -- no UB; all shifts on unsigned types.
// ---------------------------------------------------------------------------

static std::vector<std::uint8_t> encode_clvm_int(Mojo amount)
{
    // HIGH-2 FIX: Guard against negative input.  Mojo coin amounts are
    // strictly non-negative; casting a negative int64_t to uint64_t would
    // produce a large spurious value, corrupting the CLVM encoding and
    // yielding an incorrect coin_name hash.  Return empty bytes (encodes
    // as zero) and log the error for diagnostics.
    // ISO/IEC 5055 -- CWE-681 (incorrect conversion) prevention.
    // ISO/IEC 27001:2022 -- audit-quality error logging.
    if (amount < 0) {
        spdlog::error("encode_clvm_int: negative amount {} is invalid "
                      "for coin amounts", amount);
        return {};  // Empty bytes encodes as zero per CLVM spec.
    }

    // Zero encodes as empty byte string per CLVM spec.
    if (amount == 0) {
        return {};
    }

    // Work with unsigned representation for bit manipulation.
    // Mojo (int64_t) amounts are non-negative in valid coin records.
    auto uval = static_cast<std::uint64_t>(amount);

    // Extract big-endian bytes, most significant first.
    std::vector<std::uint8_t> bytes;
    bytes.reserve(8);  // At most 8 bytes for a 64-bit value.

    while (uval > 0) {
        bytes.push_back(static_cast<std::uint8_t>(uval & 0xFF));
        uval >>= 8;
    }

    // Reverse to big-endian order.
    std::reverse(bytes.begin(), bytes.end());

    // CLVM sign-bit rule: if the high bit of the leading byte is set,
    // prepend 0x00 so the value is not misinterpreted as negative.
    if (!bytes.empty() && (bytes.front() & 0x80) != 0) {
        bytes.insert(bytes.begin(), 0x00);
    }

    return bytes;
}

// ---------------------------------------------------------------------------
// compute_coin_name -- derive the unique coin identifier via SHA-256
// ---------------------------------------------------------------------------
// Chia coin name specification:
//   coin_name = SHA-256(parent_coin_info || puzzle_hash || amount_clvm)
//
// Where:
//   - parent_coin_info: 32 bytes (the coin_name of the parent coin)
//   - puzzle_hash:      32 bytes (hash of the CLVM puzzle that locks this coin)
//   - amount_clvm:      variable-length CLVM-encoded big-endian integer
//
// The result is a 32-byte (256-bit) digest, returned as a 64-character
// lowercase hex string.
//
// Reference: https://docs.chia.net/coin-set-model/#coin-id
//
// Uses the OpenSSL EVP interface (recommended over deprecated SHA256_*).
// ISO/IEC 5055 -- RAII via unique_ptr with custom deleter for EVP_MD_CTX.
// ISO/IEC 27001:2022 -- coin names are public identifiers, not secrets.
// ---------------------------------------------------------------------------

std::string CoinManager::compute_coin_name(const std::string& parent_id,
                                           const std::string& puzzle_hash,
                                           Mojo               amount)
{
    // Decode hex inputs to raw bytes.
    const std::vector<std::uint8_t> parent_bytes  = hex_to_bytes(parent_id);
    const std::vector<std::uint8_t> puzzle_bytes  = hex_to_bytes(puzzle_hash);
    const std::vector<std::uint8_t> amount_bytes  = encode_clvm_int(amount);

    // Validate expected sizes for parent_coin_info and puzzle_hash.
    if (parent_bytes.size() != 32 || puzzle_bytes.size() != 32) {
        throw std::invalid_argument(
            "compute_coin_name: parent_id and puzzle_hash must each be "
            "64 hex characters (32 bytes)");
    }

    // Allocate EVP digest context with RAII cleanup.
    // ISO/IEC 5055 -- deterministic resource release via unique_ptr.
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);

    if (!ctx) {
        throw std::runtime_error(
            "compute_coin_name: EVP_MD_CTX_new() failed");
    }

    // Initialise SHA-256 digest computation.
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error(
            "compute_coin_name: EVP_DigestInit_ex(SHA-256) failed");
    }

    // Feed the three components in order: parent || puzzle || amount.
    if (EVP_DigestUpdate(ctx.get(), parent_bytes.data(),
                         parent_bytes.size()) != 1) {
        throw std::runtime_error(
            "compute_coin_name: EVP_DigestUpdate(parent) failed");
    }

    if (EVP_DigestUpdate(ctx.get(), puzzle_bytes.data(),
                         puzzle_bytes.size()) != 1) {
        throw std::runtime_error(
            "compute_coin_name: EVP_DigestUpdate(puzzle) failed");
    }

    // Amount may be zero-length (for amount == 0); EVP_DigestUpdate
    // handles empty input correctly.
    if (!amount_bytes.empty()) {
        if (EVP_DigestUpdate(ctx.get(), amount_bytes.data(),
                             amount_bytes.size()) != 1) {
            throw std::runtime_error(
                "compute_coin_name: EVP_DigestUpdate(amount) failed");
        }
    }

    // Finalise and extract the 32-byte digest.
    std::array<unsigned char, 32> digest{};
    unsigned int digest_len = 0;

    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digest_len) != 1 ||
        digest_len != 32) {
        throw std::runtime_error(
            "compute_coin_name: EVP_DigestFinal_ex() failed");
    }

    // Encode the digest as a 64-character lowercase hex string.
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digest_len; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }

    return oss.str();
}

}  // namespace xop::execution
