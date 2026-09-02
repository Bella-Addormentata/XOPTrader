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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
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
    std::string tx_id{};       ///< Transaction (spend bundle) ID, if available.
                               ///  [CI, GCC] The {} is load-bearing: every
                               ///  designated-initializer site that skips this
                               ///  member trips -Wmissing-field-initializers,
                               ///  which -Wextra enables and MSVC has no
                               ///  equivalent for. Its siblings all carry one.

    /// [S41] True when the split was abandoned because the coin enumeration
    /// FAILED -- not because the wallet lacked funds. The two were previously
    /// indistinguishable: a failed read produced an empty coin list, which
    /// produced "insufficient balance ... Have 0 mojos total" about a wallet
    /// holding ~59 XCH. A function that never read the wallet is not entitled
    /// to make a claim about its balance.
    bool read_failed{false};
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

    /*
     * [S41 2026-09-01] get_balance_xch() and get_balance_mojos() were REMOVED
     * here. Both had zero callers repo-wide, and get_balance_mojos() failed
     * open twice -- co_return 0 on an unexpected response shape and co_return
     * 0 on ChiaRPCError -- i.e. it reported an empty wallet when the read had
     * failed, the identical defect being fixed below. Deleting a dormant trap
     * is smaller than porting it; shipping it unchanged alongside this fix
     * would have been shipping the twelfth instance in the same commit.
     */

    // -- Coin enumeration ---------------------------------------------------

    /**
     * @brief Enumerate spendable coins, excluding those locked by offers.
     *
     * Queries the wallet for all spendable coins, then filters out any
     * whose coin_name appears in the internal locked set.
     *
     * [S41] Returns std::nullopt when the enumeration DID NOT HAPPEN -- the
     * RPC threw, or a coin record could not be parsed. An engaged optional
     * holding an empty vector is a real answer: the wallet has no usable
     * unlocked coins. These two were previously the same value, which is how
     * a failed read came to authorise a split and to print "Have 0 mojos
     * total" about a wallet holding ~59 XCH.
     *
     * NO CALLER MAY SUBSTITUTE A NUMBER FOR A FAILED READ. In particular
     * .value_or({}) reinstates the bug; it is a single greppable token and it
     * must not appear.
     *
     * @param wallet_id  Wallet identifier.
     * @return Unlocked spendable coins, or nullopt if the read failed.
     */
    asio::awaitable<std::optional<std::vector<CoinInfo>>> get_spendable_coins(
        std::int64_t wallet_id);

    /**
     * @brief [S41] The whole of get_spendable_coins(), minus the wallet.
     *
     * WHY THIS EXISTS: THE GUARD HAD NO GUARD.
     *
     * The first attempt at S41 changed the return type to std::optional and
     * pinned that with static_asserts in test_coin_pool_verdict.cpp. A review
     * then reinstated the defect -- `co_return std::vector<CoinInfo>{};` in
     * place of `co_return std::nullopt;` inside the ChiaRPCError catch -- and
     * the entire 1109-test suite stayed GREEN. The type was never the bug.
     * The bug is a VALUE: an error path returning the same value as a valid
     * negative answer. static_asserts cannot see into a function body, and
     * ChiaWalletRPC is `final` with non-virtual methods and there is no gmock
     * in this repo, so no test could reach the catch at either end.
     *
     * So the RPC is now a parameter rather than a member read. `fetch` is any
     * callable taking a wallet id and returning an awaitable vector of coin
     * records; production passes one that calls the wallet, and a test passes
     * one that throws. Everything else -- the dust filter, the locked filter,
     * the sort, and BOTH catches -- is here, where a test can drive it.
     *
     * THE CONTRACT, which is the thing under test:
     *   - `fetch` throws                       -> std::nullopt
     *   - any record fails to parse            -> std::nullopt
     *   - `fetch` yields no records            -> ENGAGED, empty vector
     *   - every coin filtered out as dust or
     *     locked                               -> ENGAGED, empty vector
     *
     * The last two are why nullopt exists. "The wallet has nothing usable" is
     * a real answer and must stay expressible; it is not the same fact as
     * "the question was never answered", and treating them alike is what made
     * 68 failed reads into 34 phantom "Have 0 mojos total" claims about a
     * wallet holding ~59 XCH.
     *
     * All parameters are BY VALUE: this is a coroutine, and a reference
     * parameter would dangle across the first suspension.
     *
     * @param fetch           wallet_id -> awaitable<std::vector<json>>; may throw.
     * @param wallet_id       Wallet identifier, forwarded to `fetch` and logged.
     * @param dust_threshold  Coins strictly below this are dropped.
     * @param is_locked       coin_name -> bool; true drops the coin.
     * @param logger          May be null; used only for the two failure paths.
     * @return Unlocked spendable coins, or nullopt if the read did not happen.
     */
    template <class Fetch, class IsLocked>
    static asio::awaitable<std::optional<std::vector<CoinInfo>>>
    collect_spendable_coins(Fetch                           fetch,
                            std::int64_t                    wallet_id,
                            Mojo                            dust_threshold,
                            IsLocked                        is_locked,
                            std::shared_ptr<spdlog::logger> logger)
    {
        // `coins` is built INSIDE the try so there is no partially-filled
        // vector in scope that a later edit could return by accident on the
        // failure path.
        try {
            std::vector<CoinInfo> coins;

            auto raw_coins = co_await fetch(wallet_id);

            for (const auto& rc : raw_coins) {
                CoinInfo ci = parse_coin(rc);

                // Filter dust coins.
                if (ci.amount < dust_threshold) {
                    continue;
                }

                // Filter coins that are locked by pending offers.
                if (is_locked(ci.coin_name)) {
                    continue;
                }

                coins.push_back(std::move(ci));
            }

            // Sort by amount descending -- largest coins first for efficient
            // selection and splitting.
            std::sort(coins.begin(), coins.end(),
                      [](const CoinInfo& a, const CoinInfo& b) {
                          return a.amount > b.amount;
                      });

            co_return coins;

        } catch (const rpc::ChiaRPCError& e) {
            // The observed failure, 68 times: "Wallet needs to be fully
            // synced before getting all coins" -- an application error, not a
            // transport fault. NOT an empty wallet. Do not "simplify" this to
            // an empty vector; CoinPoolVerdict.AnRpcFailureIsNotAnEmptyWallet
            // exists to stop exactly that edit.
            if (logger) {
                logger->error("get_spendable_coins failed for wallet_id {}: {}",
                              wallet_id, e.what());
            }
            co_return std::nullopt;

        } catch (const std::exception& e) {
            // parse_coin() and compute_coin_name() throw json::type_error,
            // std::invalid_argument and std::runtime_error on a malformed
            // coin record. These used to escape this function and, at the two
            // live call sites, an ancestry of DETACHED coroutines whose
            // completion handler discards the exception_ptr -- so an escape
            // here would end the poll loop silently rather than terminate.
            // One non-throwing boundary, and nothing silent: it is logged at
            // critical.
            //
            // NOTE FOR THE ENGINE: because this catch is here, a malformed
            // record arrives at engine.cpp as a nullopt, NOT as an exception.
            // The engine's try/catch around count_free_coins() is a backstop
            // for a non-std::exception escape and is not the live path.
            if (logger) {
                logger->critical("get_spendable_coins: malformed coin record "
                                 "for wallet_id {}: {}", wallet_id, e.what());
            }
            co_return std::nullopt;
        }
    }

    /**
     * @brief Count of unlocked spendable coins for a wallet.
     *
      * Convenience wrapper around get_spendable_coins().size(). For wallet 1,
      * when the XCH coin pool is configured, this counts only pool-ready XCH
      * denominations so the engine does not treat tiny change or oversized
      * UTXOs as healthy trading inventory.
     *
     * [S41] nullopt propagates: "I could not count" is not the number 0.
     *
     * @param wallet_id  Wallet identifier.
     * @return Number of free (unlocked) coins, or nullopt if the read failed.
     */
    asio::awaitable<std::optional<int>> count_free_coins(std::int64_t wallet_id);

    /**
     * @brief Check whether a coin is already close to the target pool size.
     *
     * Coins below half the target denomination are too small to be useful
     * pre-split inventory. Coins above 2x the target denomination still
     * lock excess capital and should remain split candidates.
     *
     * @param coin_amount_mojos    Coin value in mojos.
     * @param target_amount_mojos  Desired pool denomination in mojos.
     * @return true if the coin is within the pool-ready size band.
     */
    static bool is_pool_ready_coin(Mojo coin_amount_mojos,
                                   Mojo target_amount_mojos);

    /**
     * @brief Count how many coins are already near the target pool size.
     *
     * @param coins                Candidate spendable coins.
     * @param target_amount_mojos  Desired pool denomination in mojos.
     * @return Number of pool-ready coins.
     */
    static std::size_t count_pool_ready_coins(
        const std::vector<CoinInfo>& coins,
        Mojo                         target_amount_mojos);

    /**
     * @brief Return true when a split would increase the number of pool-ready coins.
     *
     * This guards against no-op splits such as taking a coin that is already
     * at the target denomination and "splitting" it into one identical output,
     * which only creates pending_change without improving trading inventory.
     *
     * @param source_amount_mojos   Value of the source coin being split.
     * @param batch                 Number of target-sized outputs to create.
     * @param target_amount_mojos   Desired pool denomination in mojos.
     * @param fee                   Split transaction fee in mojos.
     * @return true if the resulting outputs contain more pool-ready coins than
     *         the original source coin.
     */
    static bool split_improves_pool_ready_count(Mojo source_amount_mojos,
                                                int  batch,
                                                Mojo target_amount_mojos,
                                                Mojo fee);

    /// One coin's viable split, chosen by plan_split_for_coin().
    /// batch == 0 means the coin has no improving split.
    struct SplitPlan {
        Mojo split_amount{0};  ///< denomination of each created coin
        int  batch{0};         ///< number of coins to create (0 = no plan)
    };

    /// Decide how (whether) to split one free coin to improve the pool.
    ///
    /// Preferred plan: `batch` coins of the pool target denomination, largest
    /// improving batch first (the historical behaviour).  Fallback
    /// [COIN-POOL-DEADLOCK 2026-08-23]: when the spendable value
    /// (amount - fee) lies in [target, 1.5*target), the only fundable
    /// target plan is batch 1, whose change (amount - fee - target) falls
    /// below the pool-ready band -- "no improvement", so such coins were
    /// refused forever.  On the live wallet every free XCH coin was ~2.0
    /// XCH against the 1.5 target, so ensure_split failed 76 times while
    /// offer whole-coin locking starved spendable XCH to zero.  For that
    /// window the planner splits into two in-band halves of
    /// (amount - fee) / 2 instead (batch=1 -- the wallet's change output
    /// IS the other half), so no dust is created and the ready count goes
    /// 1 -> 2.  Above the window the target path resumes because the
    /// post-fee change re-enters the band; the exact boundaries are the
    /// pool-ready predicates on the post-fee value, not literal source
    /// amounts.
    static SplitPlan plan_split_for_coin(Mojo amount_mojos,
                                         int  needed,
                                         Mojo target_amount_mojos,
                                         Mojo fee);


    /*
     * [S41 2026-09-01] The awaitable count_pool_ready_coins(wallet_id, target)
     * overload was REMOVED here: zero callers repo-wide, and it returned 0 on
     * a failed read like everything else in this family. The static
     * count_pool_ready_coins(coins, target) overload below is the live one and
     * is unchanged -- it takes coins that have already been read successfully,
     * so it cannot express this failure and does not need to.
     */

    // -- Coin splitting -----------------------------------------------------

    /**
     * @brief Pre-split large coins into target denominations for concurrency.
     *
     * If count_free_coins() < target_count, this method self-sends XCH
     * to create additional coins of the specified denomination.  The
     * transaction spends one or more large coins and produces target_count
     * outputs of target_amount_mojos each, plus a change output.
     *
     * The split uses the wallet's split_coins RPC, which divides a coin in
     * place and therefore needs no receive address.  A blockchain fee is
     * attached to incentivise prompt inclusion.
     *
     * @param wallet_id           Wallet to split coins in.
     * @param target_count        Desired total number of spendable coins.
     * @param target_amount_mojos Denomination of each new coin (mojos).
     * @param fee                 Transaction fee in mojos.
     * @return SplitResult with the number of coins created and success flag.
     */
    asio::awaitable<SplitResult> ensure_split(
        std::int64_t   wallet_id,
        int            target_count,
        Mojo           target_amount_mojos,
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

    /// Target XCH coin-pool denomination in mojos (wallet 1 only).
    Mojo xch_pool_target_mojos_{0};

    /// Mutex protecting the locked-coin set.
    mutable std::mutex mtx_locked_;

    /// Set of coin_name strings currently reserved by pending offers.
    std::unordered_set<std::string> locked_coins_;
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_COIN_MANAGER_HPP
