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

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

CoinManager::CoinManager(asio::io_context&                    ioc,
                         std::shared_ptr<rpc::ChiaWalletRPC>  wallet,
                         const AppConfig&                     config)
    : ioc_(ioc)
    , wallet_(std::move(wallet))
    , logger_(spdlog::default_logger()->clone("CoinMgr"))
{
    default_split_fee_ = static_cast<Mojo>(config.strategy.offer_fee_mojos);

    // Dust threshold: coins smaller than 1,000,000 mojos (0.000001 XCH)
    // are ignored to avoid unnecessary UTXO bloat.
    dust_threshold_ = 1'000'000LL;

    logger_->info("CoinManager initialised: split_fee={} mojos, "
                  "dust_threshold={} mojos",
                  default_split_fee_, dust_threshold_);
}

// ---------------------------------------------------------------------------
// get_balance_xch -- confirmed spendable balance in whole XCH (display)
// ---------------------------------------------------------------------------

asio::awaitable<double> CoinManager::get_balance_xch(std::int64_t wallet_id)
{
    Mojo balance_mojos = co_await get_balance_mojos(wallet_id);
    double balance_xch = static_cast<double>(balance_mojos)
                         / static_cast<double>(kMojosPerXch);
    co_return balance_xch;
}

// ---------------------------------------------------------------------------
// get_balance_mojos -- confirmed spendable balance in raw mojos
// ---------------------------------------------------------------------------

asio::awaitable<Mojo> CoinManager::get_balance_mojos(std::int64_t wallet_id)
{
    try {
        json result = co_await wallet_->get_wallet_balance(wallet_id);

        // MEDIUM-5 FIX: The wallet RPC already unwraps the "wallet_balance"
        // envelope, so `result` IS the balance object.  Access the balance
        // fields directly from the unwrapped object; the nested
        // "wallet_balance" path was dead code (always false).
        // ISO/IEC 25000 -- remove unreachable branch to improve maintainability.
        // ISO/IEC 5055 -- CWE-561 (dead code) elimination.
        if (result.contains("spendable_balance")) {
            co_return result["spendable_balance"].get<Mojo>();
        }

        logger_->warn("get_balance_mojos: unexpected response structure "
                      "for wallet_id {}", wallet_id);
        co_return 0;

    } catch (const rpc::ChiaRPCError& e) {
        logger_->error("get_balance_mojos failed for wallet_id {}: {}",
                       wallet_id, e.what());
        co_return 0;
    }
}

// ---------------------------------------------------------------------------
// get_spendable_coins -- enumerate unlocked spendable coins
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<CoinInfo>> CoinManager::get_spendable_coins(
    std::int64_t wallet_id)
{
    std::vector<CoinInfo> coins;

    try {
        auto raw_coins = co_await wallet_->get_spendable_coins(wallet_id);

        for (const auto& rc : raw_coins) {
            CoinInfo ci = parse_coin(rc);

            // Filter dust coins.
            if (ci.amount < dust_threshold_) {
                continue;
            }

            // Filter coins that are locked by pending offers.
            {
                std::lock_guard<std::mutex> lock(mtx_locked_);
                if (locked_coins_.count(ci.coin_name) > 0) {
                    continue;
                }
            }

            coins.push_back(std::move(ci));
        }

        // Sort by amount descending -- largest coins first for efficient
        // selection and splitting.
        std::sort(coins.begin(), coins.end(),
                  [](const CoinInfo& a, const CoinInfo& b) {
                      return a.amount > b.amount;
                  });

    } catch (const rpc::ChiaRPCError& e) {
        logger_->error("get_spendable_coins failed for wallet_id {}: {}",
                       wallet_id, e.what());
    }

    co_return coins;
}

// ---------------------------------------------------------------------------
// count_free_coins -- number of unlocked spendable coins
// ---------------------------------------------------------------------------

asio::awaitable<int> CoinManager::count_free_coins(std::int64_t wallet_id)
{
    auto coins = co_await get_spendable_coins(wallet_id);
    co_return static_cast<int>(coins.size());
}

// ---------------------------------------------------------------------------
// ensure_split -- pre-split large coins into target denominations
// ---------------------------------------------------------------------------

asio::awaitable<SplitResult> CoinManager::ensure_split(
    std::int64_t       wallet_id,
    int                target_count,
    Mojo               target_amount_mojos,
    const std::string& address,
    Mojo               fee)
{
    SplitResult result;

    // Step 1: Count current free coins.
    auto free_coins = co_await get_spendable_coins(wallet_id);
    int current_count = static_cast<int>(free_coins.size());

    if (current_count >= target_count) {
        logger_->info("ensure_split: already have {} free coins (target {}), "
                      "no split needed", current_count, target_count);
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
        co_return SplitResult{false, "overflow in total_needed calculation"};
    }
    Mojo total_needed = static_cast<Mojo>(needed) * target_amount_mojos + fee;

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
            co_return SplitResult{false, "balance overflow"};
        }
        total_available += c.amount;
    }

    if (total_available < total_needed) {
        logger_->error("ensure_split: insufficient balance for split. "
                       "Need {} mojos, have {} mojos",
                       total_needed, total_available);
        result.success = false;
        co_return result;
    }

    // Step 4: Execute the split as a series of send_transaction calls.
    //
    // The Chia wallet RPC send_transaction endpoint sends XCH to a target
    // address.  By sending target_amount_mojos to our own address multiple
    // times in separate transactions, we create individual coins of the
    // desired denomination.
    //
    // Batching strategy: send at most 10 coins per transaction to avoid
    // mempool congestion and ensure prompt confirmation.
    constexpr int kBatchSize = 10;
    int coins_created = 0;
    Mojo total_fee = 0;

    while (coins_created < needed) {
        int batch = std::min(kBatchSize, needed - coins_created);

        for (int j = 0; j < batch; ++j) {
            try {
                // Build the send_transaction request per the Chia wallet
                // RPC specification.  Each call creates one output coin
                // of the target denomination at our own receive address.
                json send_params;
                send_params["wallet_id"] = wallet_id;
                send_params["amount"]    = target_amount_mojos;
                send_params["address"]   = address;
                send_params["fee"]       = fee;

                // Execute the send_transaction RPC call.
                // The Chia wallet daemon creates a spend bundle that
                // consumes one or more input coins and produces:
                //   1. An output of target_amount_mojos to our address.
                //   2. A change output for any remainder.
                // The transaction enters the mempool and confirms within
                // ~52 seconds (one block time) under normal conditions.
                //
                // ISO/IEC 5055 -- exceptions from send_transaction are
                // caught below; partial results are still returned.
                json tx_resp = co_await wallet_->send_transaction(send_params);

                ++coins_created;
                total_fee += fee;

                // Extract and log the transaction ID if present.
                std::string tx_id = "(unknown)";
                if (tx_resp.contains("transaction_id")) {
                    tx_id = tx_resp["transaction_id"].get<std::string>();
                }

                logger_->debug("Split coin {}/{}: {} mojos to {} [tx={}]",
                               coins_created, needed, target_amount_mojos,
                               address.substr(0, 20), tx_id.substr(0, 16));

                // Capture the transaction ID from the last successful send
                // into the result for caller inspection.
                if (tx_resp.contains("transaction_id")) {
                    result.tx_id = tx_id;
                }

            } catch (const rpc::ChiaRPCError& e) {
                // RPC-level error (transport, application rejection, etc.).
                // Log and return partial results -- partial splits are
                // still useful for concurrency.
                logger_->error("ensure_split: send_transaction RPC error at "
                               "coin {}/{}: {}",
                               coins_created + 1, needed, e.what());
                result.coins_created = coins_created;
                result.fee_paid      = total_fee;
                result.success       = (coins_created > 0);
                co_return result;

            } catch (const std::exception& e) {
                // Unexpected non-RPC error (JSON parse failure, etc.).
                logger_->error("ensure_split: unexpected error at "
                               "coin {}/{}: {}",
                               coins_created + 1, needed, e.what());
                result.coins_created = coins_created;
                result.fee_paid      = total_fee;
                result.success       = (coins_created > 0);
                co_return result;
            }
        }
    }

    result.coins_created = coins_created;
    result.fee_paid      = total_fee;
    result.success       = true;

    logger_->info("ensure_split: created {} coins of {} mojos each "
                  "(fee {} mojos total)",
                  coins_created, target_amount_mojos, total_fee);

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
