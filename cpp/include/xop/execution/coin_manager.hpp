/**
 * @file coin_manager.hpp
 * @brief UTXO (coin-set) manager for the XOPTrader CHIA market-making bot.
 *
 * CHIA uses a coin-set (UTXO) model -- every on-chain value is an immutable
 * "coin" identified by its parent_coin_info + puzzle_hash + amount triple,
 * hashed to a unique coin_name (32 bytes, hex-encoded).
 *
 * Market-making implications (Section 4 of strategy document):
 *   - Each offer locks the specific coins it references.
 *   - A single coin cannot back multiple concurrent offers.
 *   - Pre-splitting large coins into trading denominations is essential for
 *     concurrency: with N tiers per side and M pairs, up to 2*N*M coins are
 *     needed simultaneously.
 *   - Cancellation requires spending a locked coin (~52 s confirmation).
 *
 * CoinManager responsibilities:
 *   1. Query wallet for spendable coins (excluding locked ones).
 *   2. Track which coins are locked by pending offers.
 *   3. Pre-split large coins into target denominations via self-send.
 *   4. Provide balance and free-coin-count queries for the strategy engine.
 *
 * Thread safety:
 *   The locked-coin set is protected by a std::mutex.  All other operations
 *   are async wallet RPC calls that must run on a single strand.
 *
 * ISO/IEC 27001:2022 -- coin names are hex hashes (not secrets), logged freely.
 * ISO/IEC 5055       -- no raw pointers; RAII locking on the mutex.
 * ISO/IEC 25000      -- clear naming, single-responsibility, documented API.
 */

#ifndef XOP_EXECUTION_COIN_MANAGER_HPP
#define XOP_EXECUTION_COIN_MANAGER_HPP

#include <xop/config.hpp>
#include <xop/types.hpp>
#include <xop/rpc/chia_rpc.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace xop::execution {

namespace asio = boost::asio;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// CoinInfo -- lightweight representation of a single unspent coin.
// ---------------------------------------------------------------------------

struct CoinInfo {
    std::string coin_name;     ///< Unique 64-hex identifier (sha256 of record).
    std::string parent_id;     ///< Parent coin's identifier (64-hex).
    std::string puzzle_hash;   ///< Puzzle hash that locks this coin (64-hex).
    Mojo        amount{0};     ///< Coin value in mojos.
    BlockHeight confirmed_at{0}; ///< Block height at which the coin was confirmed.
};

// ---------------------------------------------------------------------------
// SplitResult -- outcome of a coin-splitting operation.
// ---------------------------------------------------------------------------

struct SplitResult {
    int  coins_created{0};     ///< Number of new coins produced by the split.
    Mojo fee_paid{0};          ///< Transaction fee paid in mojos.
    bool success{false};       ///< True if the split transaction was accepted.
    std::string tx_id;         ///< Transaction (spend bundle) ID, if available.
};

// ---------------------------------------------------------------------------
// CoinManager
// ---------------------------------------------------------------------------

/**
 * @brief Manages the CHIA coin pool for concurrent multi-tier offer creation.
 *
 * The coin-set model means that every outstanding offer ties up specific
 * coins.  CoinManager ensures the bot always has enough pre-split coins of
 * appropriate denomination to post the configured number of tiers on all
 * active pairs, without double-spending.
 *
 * Key operations:
 *   - get_balance_xch()     : confirmed + spendable balance.
 *   - get_spendable_coins() : unspent coins minus those locked by offers.
 *   - count_free_coins()    : how many unlocked coins are available.
 *   - ensure_split()        : split large coins into target denominations.
 *   - lock_coin() / unlock_coin() / unlock_all() : manual lock management.
 *
 * Locking discipline:
 *   - lock_coin() is called by OfferManager after a successful create_offer.
 *   - unlock_coin() is called when an offer is cancelled or settled.
 *   - unlock_all() is called during graceful shutdown after cancel_all.
 *   - The locked set is protected by a std::mutex for thread safety.
 */
class CoinManager {
public:
    /**
     * @brief Construct a CoinManager.
     *
     * @param ioc     Boost.Asio io_context (accepted for API stability;
     *                not stored — async work runs on the caller's strand).
     * @param wallet  Shared pointer to an open ChiaWalletRPC client.
     * @param config  Application configuration (for default fee, etc.).
     */
    CoinManager(asio::io_context&                    ioc,
                std::shared_ptr<rpc::ChiaWalletRPC>  wallet,
                const AppConfig&                     config);

    ~CoinManager() = default;

    // Non-copyable, non-movable -- owned by the engine via unique_ptr.
    CoinManager(const CoinManager&)            = delete;
    CoinManager& operator=(const CoinManager&) = delete;
    CoinManager(CoinManager&&)                 = delete;
    CoinManager& operator=(CoinManager&&)      = delete;

    // -- Balance queries ----------------------------------------------------

    /**
     * @brief Get the confirmed spendable XCH balance for a wallet.
     *
     * Queries get_wallet_balance() and returns the "spendable_balance" field
     * converted from mojos to XCH (double, for informational display).
     *
     * @param wallet_id  Wallet identifier (1 = main XCH wallet).
     * @return Balance in whole XCH (double, precision limited by display).
     */
    asio::awaitable<double> get_balance_xch(std::int64_t wallet_id);

    /**
     * @brief Get the confirmed spendable balance in mojos.
     *
     * More precise than get_balance_xch() -- returns the raw mojo count
     * without floating-point conversion.
     *
     * @param wallet_id  Wallet identifier.
     * @return Balance in mojos.
     */
    asio::awaitable<Mojo> get_balance_mojos(std::int64_t wallet_id);

    // -- Coin enumeration ---------------------------------------------------

    /**
     * @brief Enumerate spendable coins, excluding those locked by offers.
     *
     * Queries the wallet for all spendable coins, then filters out any
     * whose coin_name appears in the internal locked set.
     *
     * @param wallet_id  Wallet identifier.
     * @return Vector of CoinInfo for unlocked spendable coins.
     */
    asio::awaitable<std::vector<CoinInfo>> get_spendable_coins(
        std::int64_t wallet_id);

    /**
     * @brief Count of unlocked spendable coins for a wallet.
     *
     * Convenience wrapper around get_spendable_coins().size().
     *
     * @param wallet_id  Wallet identifier.
     * @return Number of free (unlocked) coins.
     */
    asio::awaitable<int> count_free_coins(std::int64_t wallet_id);

    // -- Coin splitting -----------------------------------------------------

    /**
     * @brief Pre-split large coins into target denominations for concurrency.
     *
     * If count_free_coins() < target_count, this method self-sends XCH
     * to create additional coins of the specified denomination.  The
     * transaction spends one or more large coins and produces target_count
     * outputs of target_amount_mojos each, plus a change output.
     *
     * The split is a standard XCH send-to-self using the provided receive
     * address.  A blockchain fee is attached to incentivise prompt inclusion.
     *
     * @param wallet_id           Wallet to split coins in.
     * @param target_count        Desired total number of spendable coins.
     * @param target_amount_mojos Denomination of each new coin (mojos).
     * @param address             Receive address (bech32m, own wallet).
     * @param fee                 Transaction fee in mojos.
     * @return SplitResult with the number of coins created and success flag.
     */
    asio::awaitable<SplitResult> ensure_split(
        std::int64_t   wallet_id,
        int            target_count,
        Mojo           target_amount_mojos,
        const std::string& address,
        Mojo           fee);

    // -- Coin locking -------------------------------------------------------

    /**
     * @brief Mark a coin as locked (reserved by a pending offer).
     *
     * Thread-safe.  Duplicate locks on the same coin_name are idempotent.
     *
     * @param coin_name  64-hex unique coin identifier.
     */
    void lock_coin(const std::string& coin_name);

    /**
     * @brief Release a previously locked coin (offer cancelled or settled).
     *
     * Thread-safe.  Unlocking a coin that is not locked is a safe no-op.
     *
     * @param coin_name  64-hex unique coin identifier.
     * @return true if the coin was found and unlocked, false if not found.
     */
    bool unlock_coin(const std::string& coin_name);

    /**
     * @brief Release all locked coins.  Called during graceful shutdown
     *        after OfferManager::cancel_all() has completed.
     *
     * Thread-safe.
     */
    void unlock_all();

    /**
     * @brief Check whether a specific coin is currently locked.
     *
     * Thread-safe.
     *
     * @param coin_name  64-hex unique coin identifier.
     * @return true if the coin is in the locked set.
     */
    bool is_locked(const std::string& coin_name) const;

    /**
     * @brief Number of coins currently in the locked set.
     *
     * Thread-safe.
     */
    std::size_t locked_count() const;

    // -- Diagnostics --------------------------------------------------------

    /**
     * @brief Log a summary of coin state (total, locked, free, balance).
     *
     * Non-async.  Reads only the locked set (sync) -- balance queries
     * require the async overloads.
     *
     * @param wallet_id  Wallet to summarise.
     */
    void log_coin_summary(std::int64_t wallet_id) const;

private:
    // -- Internal helpers ---------------------------------------------------

    /**
     * @brief Parse a wallet RPC coin JSON object into a CoinInfo struct.
     *
     * Expected JSON structure (from get_spendable_coins response):
     * {
     *   "coin": {
     *     "parent_coin_info": "0x...",
     *     "puzzle_hash": "0x...",
     *     "amount": 100000000000
     *   },
     *   "confirmed_block_index": 12345
     * }
     *
     * @param coin_json  Single coin record from the wallet RPC.
     * @return Populated CoinInfo.
     */
    static CoinInfo parse_coin(const json& coin_json);

    /**
     * @brief Compute the sha256-based coin_name from its components.
     *
     * coin_name = sha256(parent_coin_info || puzzle_hash || amount_bytes)
     *
     * This is used when the RPC response does not include the coin_name
     * directly and it must be derived.
     *
     * @param parent_id    Parent coin identifier (32 bytes, hex).
     * @param puzzle_hash  Puzzle hash (32 bytes, hex).
     * @param amount       Coin amount in mojos.
     * @return 64-character hex-encoded coin_name.
     */
    static std::string compute_coin_name(const std::string& parent_id,
                                         const std::string& puzzle_hash,
                                         Mojo               amount);

    // -- Member data --------------------------------------------------------

    /// Wallet RPC client (shared with OfferManager).
    std::shared_ptr<rpc::ChiaWalletRPC> wallet_;

    /// Per-component logger (spdlog).
    std::shared_ptr<spdlog::logger> logger_;

    /// Default transaction fee for coin splits (mojos).  From config.
    Mojo default_split_fee_{100'000'000};  // 0.0001 XCH

    /// Minimum coin size to consider useful (filters dust).  Mojos.
    Mojo dust_threshold_{1'000'000};  // 0.000001 XCH

    /// Mutex protecting the locked-coin set.
    mutable std::mutex mtx_locked_;

    /// Set of coin_name strings currently reserved by pending offers.
    std::unordered_set<std::string> locked_coins_;
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_COIN_MANAGER_HPP
