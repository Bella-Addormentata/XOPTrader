/**
 * @file chia_rpc.hpp
 * @brief Chia blockchain RPC clients for Full Node and Wallet interactions.
 *
 * Provides async mTLS-authenticated JSON-RPC communication with the Chia
 * daemon (full node on port 8555, wallet on port 9256). Both client classes
 * inherit from ChiaRPCBase, which owns the SSL context, libcurl session
 * management, automatic retry with exponential backoff, and structured
 * error handling.
 *
 * Dependencies: libcurl (OpenSSL backend), nlohmann/json, spdlog,
 *               boost::asio (coroutines).
 *
 * Security: mTLS with Chia self-signed certificates; peer verification is
 *           disabled (CURLOPT_SSL_VERIFYPEER = 0) per Chia convention.
 *           Certificate file contents are never logged.
 *
 * ISO/IEC 27001:2022 -- sensitive material (cert paths) validated before use;
 *                       no secrets written to log sinks.
 * ISO/IEC 5055       -- defensive null/error checks on every external call.
 * ISO/IEC 25000      -- deterministic resource cleanup via RAII.
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
 * Owns a libcurl easy handle configured for mTLS with the Chia daemon.
 * Provides:
 *   - rpc_post()  : async JSON-RPC POST with automatic retry.
 *   - open()      : validate certs and initialise the CURL handle.
 *   - close()     : deterministic cleanup.
 *
 * Subclasses add endpoint-specific methods (full-node, wallet).
 *
 * Thread safety: NOT thread-safe.  Each instance must be used from a single
 * strand (boost::asio::strand) or serialised externally.
 */
class ChiaRPCBase {
public:
    virtual ~ChiaRPCBase();

    // Non-copyable, movable
    ChiaRPCBase(const ChiaRPCBase&)            = delete;
    ChiaRPCBase& operator=(const ChiaRPCBase&) = delete;
    ChiaRPCBase(ChiaRPCBase&&) noexcept;
    ChiaRPCBase& operator=(ChiaRPCBase&&) noexcept;

    /**
     * @brief Validate certificate paths and initialise the CURL handle.
     *
     * Must be called once before any RPC method.  Re-opening an already-open
     * client is safe (close + re-open).
     *
     * @throws ChiaRPCError if certificate files are missing or CURL init fails.
     */
    asio::awaitable<void> open();

    /**
     * @brief Release the CURL handle and cancel any pending timers.
     *
     * Idempotent — safe to call on a closed or never-opened client.
     */
    void close() noexcept;

    /**
     * @brief Check whether the client has an active, configured CURL handle.
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

    /// Endpoint configuration (host, port, TLS, timeouts, retry).
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
     * @brief Configure mTLS options on the CURL handle.
     *
     * Sets CURLOPT_SSLCERT, CURLOPT_SSLKEY, CURLOPT_CAINFO, and
     * disables peer verification (Chia self-signed certs).
     *
     * @throws ChiaRPCError on configuration failure.
     */
    void configure_tls();

    /**
     * @brief Perform a single synchronous CURL transfer (blocking).
     *
     * Called from rpc_post() after being dispatched to the thread pool so
     * that the coroutine does not block the io_context.
     *
     * @param url       Full request URL.
     * @param body      JSON-encoded request body string.
     * @param[out] response_body  Buffer for the response.
     * @param[out] http_code      HTTP status code from the server.
     * @return CURLcode indicating transport-level success / failure.
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

    /// libcurl easy handle — nullptr when closed.
    CURL* curl_{nullptr};

    /// Reusable HTTP header list for JSON Content-Type.
    curl_slist* headers_{nullptr};
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
        std::int64_t start         = 0,
        std::int64_t end           = 10,
        bool         file_contents = false);

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
};

} // namespace xop::rpc

#endif // XOP_RPC_CHIA_RPC_HPP
