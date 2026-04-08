/**
 * @file chia_rpc.hpp
 * @brief Chia blockchain RPC clients for Full Node and Wallet interactions.
 *
 * Provides async mTLS-authenticated JSON-RPC communication with the Chia
 * daemon (full node on port 8555, wallet on port 9256). Both client classes
 * inherit from ChiaRPCBase, which owns the SSL context, automatic retry
 * with exponential backoff, and structured error handling.
 *
 * Blocking CURL transfers are dispatched to a dedicated thread pool so that
 * the caller's io_context event loop is never stalled.  Each RPC call creates
 * a per-request CURL easy handle (RAII) for thread safety.
 *
 * Dependencies: libcurl (OpenSSL backend), nlohmann/json, spdlog,
 *               boost::asio (coroutines, thread_pool).
 *
 * Security: mTLS with Chia self-signed certificates.  SSL peer verification
 *           is ON by default when a CA cert path is available, but can be
 *           disabled via config for explicit localhost overrides.
 *           Certificate file contents are never logged.
 *
 * ISO/IEC 27001:2022 -- sensitive material (cert paths) validated before use;
 *                       no secrets written to log sinks.  SSL verification
 *                       enabled by default to prevent MITM on wallet RPC.
 * ISO/IEC 5055       -- defensive null/error checks on every external call;
 *                       per-request CURL handles eliminate shared-state races.
 * ISO/IEC 25000      -- deterministic resource cleanup via RAII; thread pool
 *                       joined on destruction.
 */

#ifndef XOP_RPC_CHIA_RPC_HPP
#define XOP_RPC_CHIA_RPC_HPP

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace xop::rpc {

// ---------------------------------------------------------------------------
// Forward declarations / type aliases
// ---------------------------------------------------------------------------
using json = nlohmann::json;
namespace asio = boost::asio;
namespace fs   = std::filesystem;

// ---------------------------------------------------------------------------
// Configuration structures
// ---------------------------------------------------------------------------

/**
 * @brief TLS certificate paths for Chia mTLS authentication.
 *
 * Chia generates self-signed certs during node setup.  Typical locations:
 *   ~/.chia/mainnet/config/ssl/full_node/private_full_node.crt
 *   ~/.chia/mainnet/config/ssl/full_node/private_full_node.key
 *   ~/.chia/mainnet/config/ssl/ca/chia_ca.crt
 *
 * All three paths are validated for existence before the CURL handle is
 * configured.  File *contents* are never read into memory by this layer
 * and are never logged.
 */
struct ChiaTLSConfig {
    fs::path cert_path;   ///< Client certificate (.crt / .pem)
    fs::path key_path;    ///< Client private key  (.key / .pem)
    fs::path ca_cert_path; ///< CA certificate for the Chia daemon

    /**
     * @brief Verify that all three files exist on disk.
     * @return true if every path resolves to a regular file.
     */
    [[nodiscard]] bool validate() const noexcept;
};

/**
 * @brief Connection parameters for a single Chia RPC endpoint.
 */
struct ChiaRPCConfig {
    std::string   host{"localhost"};       ///< Hostname / IP address
    std::uint16_t port{0};                 ///< TCP port (8555 / 9256)
    ChiaTLSConfig tls;                     ///< mTLS certificate paths

    /// Maximum duration to wait for a single HTTP round-trip (default 30 s).
    std::chrono::milliseconds request_timeout{30'000};

    /// Number of automatic retries on transient (network / 5xx) errors.
    std::uint32_t max_retries{3};

    /// Initial backoff delay — doubled after each retry (exponential).
    std::chrono::milliseconds retry_base_delay{500};

    /// Enable SSL peer verification (default: true).
    ///
    /// When true and a CA cert path is available in ChiaTLSConfig, the CURL
    /// handle will verify the server certificate against the CA.  This
    /// prevents MITM attacks on the wallet RPC channel.
    ///
    /// Set to false only for explicit localhost/loopback testing scenarios
    /// where the Chia daemon uses self-signed certs without a matching CA.
    ///
    /// ISO/IEC 27001:2022 -- SSL verification is ON by default; disabling
    /// requires an explicit configuration decision logged at warning level.
    bool verify_ssl{true};

    /// Number of threads in the CURL worker pool.  Blocking CURL transfers
    /// are dispatched here so the io_context event loop is never stalled.
    /// Default of 4 handles typical full-node + wallet + retry concurrency.
    std::uint32_t curl_thread_pool_size{4};
};

// ---------------------------------------------------------------------------
// Custom exception hierarchy
// ---------------------------------------------------------------------------

/**
 * @brief Base exception for all Chia RPC errors.
 */
class ChiaRPCError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief The Chia daemon returned {"success": false} (application-level).
 */
class ChiaRPCApplicationError : public ChiaRPCError {
public:
    explicit ChiaRPCApplicationError(const std::string& msg,
                                      json               response);
    [[nodiscard]] const json& response() const noexcept { return response_; }
private:
    json response_;
};

/**
 * @brief Transport-level failure (DNS, TLS handshake, timeout, HTTP 5xx).
 */
class ChiaRPCTransportError : public ChiaRPCError {
public:
    explicit ChiaRPCTransportError(const std::string& msg,
                                    long               http_code,
                                    CURLcode           curl_code);
    [[nodiscard]] long     http_code()  const noexcept { return http_code_; }
    [[nodiscard]] CURLcode curl_code()  const noexcept { return curl_code_; }
private:
    long     http_code_;
    CURLcode curl_code_;
};

// ---------------------------------------------------------------------------
// Base class -- shared mTLS, HTTP POST, retry logic
// ---------------------------------------------------------------------------

/**
 * @brief Common foundation for all Chia RPC clients.
 *
 * Provides:
 *   - rpc_post()  : async JSON-RPC POST with automatic retry.
 *   - open()      : validate certs and start the CURL thread pool.
 *   - close()     : deterministic cleanup (joins all pool threads).
 *
 * Per-request CURL handles are created and destroyed within each RPC call,
 * providing full thread safety without external synchronisation.  All
 * blocking CURL transfers are dispatched to an internal thread pool via
 * boost::asio::co_spawn so the caller's io_context is never stalled.
 *
 * SSL verification is enabled by default when a CA cert is configured,
 * preventing MITM attacks on the wallet RPC channel.
 *
 * Subclasses add endpoint-specific methods (full-node, wallet).
 *
 * Thread safety: the public interface (rpc_post and derived methods) is
 * safe to call concurrently from multiple coroutines on the same
 * io_context -- each call gets its own CURL handle and response buffer.
 */
class ChiaRPCBase {
public:
    virtual ~ChiaRPCBase();

    // Non-copyable, movable.
    ChiaRPCBase(const ChiaRPCBase&)            = delete;
    ChiaRPCBase& operator=(const ChiaRPCBase&) = delete;
    ChiaRPCBase(ChiaRPCBase&&) noexcept;
    ChiaRPCBase& operator=(ChiaRPCBase&&) noexcept;

    /**
     * @brief Validate certificate paths and start the CURL thread pool.
     *
     * Must be called once before any RPC method.  Re-opening an already-open
     * client is safe (close + re-open).
     *
     * @throws ChiaRPCError if certificate files are missing.
     */
    asio::awaitable<void> open();

    /**
     * @brief Join the thread pool and mark the client as closed.
     *
     * Idempotent -- safe to call on a closed or never-opened client.
     */
    void close() noexcept;

    /**
     * @brief Check whether the client has been opened and not yet closed.
     */
    [[nodiscard]] bool is_open() const noexcept;

protected:
    /**
     * @brief Construct with an io_context reference and endpoint config.
     * @param ioc  The Boost.Asio io_context that drives async timers.
     * @param cfg  Connection parameters (host, port, TLS, timeouts).
     * @param logger_name  Name for the spdlog logger instance.
     */
    ChiaRPCBase(asio::io_context& ioc,
                ChiaRPCConfig     cfg,
                std::string_view  logger_name);

    /**
     * @brief Execute an async JSON-RPC POST with automatic retry.
     *
     * Serialises @p payload as JSON, POSTs to
     *   https://<host>:<port>/<endpoint>
     * and returns the parsed JSON response body.
     *
     * The blocking CURL transfer is dispatched to the internal thread pool
     * via co_spawn, so the caller's io_context is never blocked.  Each call
     * creates a fresh CURL easy handle (RAII) for thread safety.
     *
     * Retry policy (applied only for transient errors):
     *   - Network errors (CURL codes indicating connection / timeout)
     *   - HTTP 429, 500, 502, 503, 504
     *   - Up to max_retries attempts with exponential backoff.
     *
     * On a non-retryable error, or when retries are exhausted, the
     * appropriate ChiaRPC*Error exception is thrown.
     *
     * @param endpoint  RPC path, e.g. "get_blockchain_state".
     * @param payload   JSON body (may be empty object {}).
     * @return Parsed JSON response.
     */
    asio::awaitable<json> rpc_post(std::string_view endpoint,
                                   const json&      payload = json::object());

    /// Reference to the driving io_context (for timer creation).
    asio::io_context& ioc_;

    /// Endpoint configuration (host, port, TLS, timeouts, retry, SSL).
    ChiaRPCConfig config_;

    /// Per-client spdlog logger.
    std::shared_ptr<spdlog::logger> logger_;

private:
    /**
     * @brief Build the full URL for an RPC endpoint.
     * @param endpoint  Relative path, e.g. "get_blockchain_state".
     * @return Fully-qualified URL including scheme, host, port.
     */
    [[nodiscard]] std::string build_url(std::string_view endpoint) const;

    /**
     * @brief Configure mTLS and SSL verification on a CURL handle.
     *
     * Sets CURLOPT_SSLCERT, CURLOPT_SSLKEY, CURLOPT_CAINFO.
     * SSL peer/host verification is controlled by config_.verify_ssl:
     *   - When true (default): VERIFYPEER=1, VERIFYHOST=2, CAINFO set.
     *   - When false: VERIFYPEER=0, VERIFYHOST=0 (logged at warn level).
     *
     * The caller must materialise the path strings and pass them by
     * const reference so their lifetime spans curl_easy_perform().
     *
     * @param curl      The CURL easy handle to configure.
     * @param cert_str  Materialised cert_path  (must outlive CURL transfer).
     * @param key_str   Materialised key_path   (must outlive CURL transfer).
     * @param ca_str    Materialised ca_cert_path (must outlive CURL transfer).
     *
     * ISO/IEC 27001:2022 -- SSL verification default-on; disabling
     *                       triggers a warning log entry.
     * ISO/IEC 5055       -- CWE-416 use-after-free prevention: path strings
     *                       are accepted by const& from the caller whose
     *                       scope covers the entire CURL transfer.
     */
    void configure_tls(CURL*              curl,
                       const std::string& cert_str,
                       const std::string& key_str,
                       const std::string& ca_str);

    /**
     * @brief Perform a single synchronous CURL transfer (blocking).
     *
     * Creates a per-request CURL easy handle with full mTLS and timeout
     * configuration, executes the transfer, and cleans up the handle.
     * Called from rpc_post() inside the thread pool so the io_context
     * is never blocked.
     *
     * @param url       Full request URL.
     * @param body      JSON-encoded request body string.
     * @param[out] response_body  Buffer for the response.
     * @param[out] http_code      HTTP status code from the server.
     * @return CURLcode indicating transport-level success / failure.
     *
     * ISO/IEC 5055 -- RAII for the CURL handle; all CURL calls checked.
     */
    CURLcode perform_request(const std::string& url,
                             const std::string& body,
                             std::string&       response_body,
                             long&              http_code);

    /**
     * @brief Determine whether a failed request is eligible for retry.
     * @param curl_code  libcurl result code.
     * @param http_code  HTTP status code (0 if transport failed).
     */
    [[nodiscard]] static bool is_transient(CURLcode curl_code,
                                           long     http_code) noexcept;

    /// Thread pool for offloading blocking CURL transfers.
    /// Created in open(), joined and destroyed in close().
    std::unique_ptr<asio::thread_pool> thread_pool_;

    /// True after successful open(), false after close().
    bool open_{false};
};

// ---------------------------------------------------------------------------
// ChiaFullNodeRPC
// ---------------------------------------------------------------------------

/**
 * @brief Async RPC client for the Chia Full Node (default port 8555).
 *
 * Provides coroutine methods for blockchain state queries and coin
 * record lookups required by the market-making engine.
 *
 * All methods require a prior successful call to open().
 */
class ChiaFullNodeRPC final : public ChiaRPCBase {
public:
    /**
     * @param ioc  Boost.Asio io_context.
     * @param cfg  Connection config (port should be 8555 for mainnet).
     */
    ChiaFullNodeRPC(asio::io_context& ioc, ChiaRPCConfig cfg);

    /**
     * @brief Retrieve the peak (highest known) block height.
     * @return Block height as a non-negative integer.
     */
    asio::awaitable<std::int64_t> get_block_height();

    /**
     * @brief Retrieve full blockchain state (sync status, difficulty, etc.).
     * @return Raw JSON response from the node.
     */
    asio::awaitable<json> get_blockchain_state();

    /**
     * @brief Look up coin records matching a puzzle hash.
     *
     * @param puzzle_hash    Hex-encoded puzzle hash (0x prefix optional).
     * @param include_spent  If true, include already-spent coins.
     * @param start_height   Optional lower height bound (inclusive).
     * @param end_height     Optional upper height bound (inclusive).
     * @return Vector of coin-record JSON objects.
     */
    asio::awaitable<std::vector<json>> get_coin_records_by_puzzle_hash(
        const std::string& puzzle_hash,
        bool               include_spent = false,
        std::int64_t       start_height  = 0,
        std::int64_t       end_height    = 0);

    /**
     * @brief Estimate the recommended transaction fee based on mempool state.
     *
     * Calls the Chia full-node `get_fee_estimate` RPC endpoint (available
     * since Chia 1.6).  Returns the estimated fee (in mojos per cost unit)
     * for the requested target inclusion time.
     *
     * @param target_time_seconds  Desired inclusion time in seconds
     *                             (e.g. 60 for next ~1 block).
     * @return Estimated fee in mojos.  Returns 0 if the RPC call fails or
     *         the endpoint is not supported by the connected node.
     */
    asio::awaitable<std::uint64_t> get_fee_estimate(
        std::uint64_t target_time_seconds = 60);

    /**
     * @brief Look up coin records by their coin names (IDs).
     *
     * Batch-validates whether specific coins are spent or unspent on-chain.
     * Useful for on-chain reconciliation: verify that coins backing pending
     * offers have not been spent externally.
     *
     * @param names          Vector of hex-encoded coin names (without 0x prefix).
     * @param include_spent  If true, include already-spent coins.
     * @return Vector of coin-record JSON objects.
     */
    asio::awaitable<std::vector<json>> get_coin_records_by_names(
        const std::vector<std::string>& names,
        bool                            include_spent = true);

    /**
     * @brief Retrieve a block record by height.
     *
     * Returns the block record (including header_hash) for the given height.
     * Required to obtain the header_hash for get_additions_and_removals().
     *
     * @param height  Block height to query.
     * @return JSON block_record object, or empty JSON on failure.
     */
    asio::awaitable<json> get_block_record_by_height(std::int64_t height);

    /**
     * @brief Retrieve additions and removals for a specific block.
     *
     * Returns the coins created (additions) and coins spent (removals) in
     * the block identified by header_hash.  Core primitive for on-chain
     * reconciliation: enables detection of fills and fee extraction by
     * examining which coins were consumed and produced.
     *
     * @param header_hash  Hex-encoded header hash (from block record).
     * @return JSON with "additions" and "removals" arrays of coin records.
     */
    asio::awaitable<json> get_additions_and_removals(
        const std::string& header_hash);
};

// ---------------------------------------------------------------------------
// ChiaWalletRPC
// ---------------------------------------------------------------------------

/**
 * @brief Async RPC client for the Chia Wallet (default port 9256).
 *
 * Covers all offer-system endpoints (create, take, cancel, list, validate)
 * plus balance and coin-selection helpers needed by the market maker.
 *
 * All methods require a prior successful call to open().
 */
class ChiaWalletRPC final : public ChiaRPCBase {
public:
    /**
     * @param ioc  Boost.Asio io_context.
     * @param cfg  Connection config (port should be 9256 for mainnet).
     */
    ChiaWalletRPC(asio::io_context& ioc, ChiaRPCConfig cfg);

    // -- Wallet info --------------------------------------------------------

    /**
     * @brief Get the balance for a specific wallet.
     * @param wallet_id  Integer wallet identifier (1 = main XCH wallet).
     * @return JSON with confirmed, spendable, pending balances.
     */
    asio::awaitable<json> get_wallet_balance(std::int64_t wallet_id);

    /**
     * @brief Enumerate all wallets known to the running wallet service.
     * @return Vector of wallet-info JSON objects (id, name, type).
     */
    asio::awaitable<std::vector<json>> get_wallets();

    // -- Offer lifecycle ----------------------------------------------------

    /**
     * @brief Create a new offer.
     *
     * @param offer_dict     Map of wallet_id -> mojos (positive = offering,
     *                       negative = requesting). e.g. {1: -100, 2: 50}
     * @param fee            Transaction fee in mojos (default 0).
     * @param validate_only  If true, validate without creating (dry run).
     * @return JSON containing "offer" (bech32 text) and "trade_record".
     */
    asio::awaitable<json> create_offer(
        const json&    offer_dict,
        std::uint64_t  fee           = 0,
        bool           validate_only = false);

    /**
     * @brief Accept (take) an existing offer.
     *
     * @param offer_text  Bech32-encoded offer string.
     * @param fee         Transaction fee in mojos.
     * @return JSON with the resulting trade_record.
     */
    asio::awaitable<json> take_offer(const std::string& offer_text,
                                     std::uint64_t      fee = 0);

    /**
     * @brief Cancel a single offer by trade ID.
     *
     * @param trade_id  Hex-encoded trade identifier.
     * @param fee       Transaction fee in mojos.
     * @param secure    If true, spend the locked coins on-chain (costs a tx);
     *                  if false, local-only cancellation (free but the offer
     *                  could still be taken by a counterparty who already has
     *                  the offer file).
     * @return JSON confirmation.
     */
    asio::awaitable<json> cancel_offer(const std::string& trade_id,
                                       std::uint64_t      fee    = 0,
                                       bool               secure = true);

    /**
     * @brief Cancel ALL outstanding offers.
     *
     * @param fee     Transaction fee in mojos (applied per cancellation).
     * @param secure  On-chain vs local-only cancellation.
     * @return JSON confirmation.
     */
    asio::awaitable<json> cancel_offers(std::uint64_t fee    = 0,
                                        bool          secure = true);

    /**
     * @brief Retrieve offers with pagination.
     *
     * @param start          Starting index (0-based).
     * @param end            Ending index (exclusive).
     * @param file_contents  If true, include the raw offer bech32 text.
     * @return Vector of offer/trade-record JSON objects.
     */
    asio::awaitable<std::vector<json>> get_all_offers(
        std::int64_t start             = 0,
        std::int64_t end               = 10,
        bool         file_contents     = false,
        bool         include_completed = true);

    /**
     * @brief Retrieve the wallet's synced block height.
     *
     * Calls the Chia wallet RPC "get_height_info" endpoint.  In wallet-only
     * mode this replaces the full-node's get_block_height() call.  The wallet
     * service tracks block height from its peer connections even when no
     * local full node is running.
     *
     * @return The wallet's current synced block height.
     * @throws ChiaRPCError on transport or application-level failure.
     */
    asio::awaitable<std::int64_t> get_height_info();

    /**
     * @brief Retrieve the wallet's sync status.
     *
     * Calls the Chia wallet RPC "get_sync_status" endpoint.  Returns the
     * raw JSON containing "syncing" and "synced" boolean fields.
     *
     * @return JSON with sync status information.
     */
    asio::awaitable<json> get_sync_status();

    /**
     * @brief Check whether an offer is still valid (coins unspent).
     *
     * @param offer  Bech32-encoded offer string.
     * @return true if the offer can still be taken.
     */
    asio::awaitable<bool> check_offer_validity(const std::string& offer);

    // -- Coin selection -----------------------------------------------------

    /**
     * @brief Select a set of coins summing to at least @p amount.
     *
     * @param wallet_id        Target wallet.
     * @param amount           Minimum total in mojos.
     * @param min_coin_amount  Ignore coins smaller than this (mojos).
     * @return Vector of coin JSON objects.
     */
    asio::awaitable<std::vector<json>> select_coins(
        std::int64_t  wallet_id,
        std::uint64_t amount,
        std::uint64_t min_coin_amount = 0);

    /**
     * @brief List all spendable (unconfirmed-excluded) coins for a wallet.
     *
     * @param wallet_id  Target wallet.
     * @return Vector of coin JSON objects.
     */
    asio::awaitable<std::vector<json>> get_spendable_coins(
        std::int64_t wallet_id);

    // -- Transaction sending -------------------------------------------------

    /**
     * @brief Send XCH to a target address (self-send for coin splitting).
     *
     * Wraps the Chia wallet RPC "send_transaction" endpoint.  The caller
     * provides pre-built parameters including wallet_id, amount, address,
     * and fee.  Returns the full JSON response containing "transaction_id"
     * and "transaction" on success.
     *
     * @param params  JSON with keys: wallet_id, amount, address, fee.
     * @return JSON response from the Chia wallet daemon.
     * @throws ChiaRPCError on transport or application-level failure.
     *
     * ISO/IEC 5055 -- all error paths throw; caller must handle exceptions.
     */
    asio::awaitable<json> send_transaction(const json& params);

    /**
     * @brief Split a single coin into multiple smaller coins atomically.
     *
     * Wraps the Chia wallet RPC "split_coins" endpoint.  Creates
     * number_of_coins new outputs of amount_per_coin mojos each from
     * the specified target coin, all in a single spend bundle (one block,
     * one fee).  Any remainder is returned as a change coin.
     *
     * @param wallet_id       Wallet identifier (1 = main XCH wallet).
     * @param target_coin_id  Hex coin ID (with or without 0x prefix) to split.
     * @param number_of_coins Number of new coins to create (max 500).
     * @param amount_per_coin Denomination of each new coin in mojos.
     * @param fee             Transaction fee in mojos.
     * @return JSON response from the Chia wallet daemon.
     * @throws ChiaRPCError on transport or application-level failure.
     */
    asio::awaitable<json> split_coins(
        std::int64_t       wallet_id,
        const std::string& target_coin_id,
        int                number_of_coins,
        std::int64_t       amount_per_coin,
        std::int64_t       fee);

    // -- Address management -------------------------------------------------

    /**
     * @brief Get a receive address for a wallet (for self-send splits).
     *
     * Wraps the Chia wallet RPC "get_next_address" endpoint.  When
     * new_address is false, returns the current unused address without
     * generating a new derivation.
     *
     * @param wallet_id    Target wallet ID.
     * @param new_address  If true, derive a fresh address.
     * @return bech32m address string.
     * @throws ChiaRPCError on transport or application-level failure.
     */
    asio::awaitable<std::string> get_next_address(
        std::int64_t wallet_id,
        bool         new_address = false);

    // -- Stuck transaction management ---------------------------------------

    /**
     * @brief Delete all unconfirmed (stuck) transactions for a wallet.
     *
     * Wraps the Chia wallet RPC "delete_unconfirmed_transactions" endpoint.
     * This clears transactions that were created locally but never broadcast
     * or confirmed on-chain (e.g. due to coin double-spend conflicts).
     *
     * @param wallet_id  Target wallet ID.
     * @return JSON confirmation from the wallet daemon.
     * @throws ChiaRPCError on transport or application-level failure.
     */
    asio::awaitable<json> delete_unconfirmed_transactions(
        std::int64_t wallet_id);

    /**
     * @brief Retrieve recent transactions for a wallet.
     *
     * Wraps the Chia wallet RPC "get_transactions" endpoint with
     * descending time order.  Used to detect stuck transactions
     * that lack a spend bundle.
     *
     * @param wallet_id  Target wallet ID.
     * @param start      Starting index (0-based).
     * @param end        Ending index (exclusive).
     * @return Vector of transaction JSON objects.
     * @throws ChiaRPCError on transport or application-level failure.
     */
    asio::awaitable<std::vector<json>> get_transactions(
        std::int64_t wallet_id,
        std::int64_t start = 0,
        std::int64_t end   = 50);
};

} // namespace xop::rpc

#endif // XOP_RPC_CHIA_RPC_HPP
