// dexie_client.hpp -- Async REST client for the dexie.space v1 API.
//
// Responsibilities:
//   - HTTP session lifecycle (per-request CURL easy handles, thread pool)
//   - Sliding-window rate limiter (50 requests per 10-second window)
//   - Typed wrappers for every public dexie.space/v1 endpoint
//   - Automatic retry on 429 / 5xx; hard throw on 4xx client errors
//   - Non-blocking rate-limit waits via boost::asio::steady_timer
//   - Blocking CURL transfers dispatched to a thread pool so the
//     caller's io_context is never stalled.
//
// Thread safety: the DexieClient public interface may be called from
// coroutines on the same io_context.  The rate limiter uses internal
// mutex synchronisation; CURL handles are per-request (RAII).
//
// ISO/IEC 27001:2022 -- secrets (offer bech32m payloads) are truncated
// in all log output.  Full offer bodies never appear in logs.
//
// ISO/IEC 5055 -- no raw owning pointers; RAII via unique_ptr with
// custom deleters for every libcurl handle.

#ifndef XOP_RPC_DEXIE_CLIENT_HPP
#define XOP_RPC_DEXIE_CLIENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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

// -----------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------

/// Parameters that govern the HTTP transport and rate limiter.
struct DexieConfig {
    /// Base URL including version prefix (no trailing slash).
    std::string base_url = "https://api.dexie.space/v1";

    /// Maximum outstanding requests inside the sliding window.
    std::size_t rate_limit_max_requests = 50;

    /// Width of the sliding window.
    std::chrono::milliseconds rate_limit_window{10'000};

    /// Upper bound on a single HTTP request (including retries).
    std::chrono::milliseconds request_timeout{30'000};

    /// Time to wait for a TCP + TLS connection to establish.
    std::chrono::milliseconds connect_timeout{10'000};

    /// Maximum retries on 429 (Too Many Requests) or 5xx errors.
    std::size_t max_retries = 3;

    /// Base delay between retries; each retry doubles the wait.
    std::chrono::milliseconds retry_base_delay{500};

    // NOTE (MEDIUM-4): max_connections removed -- it was only consumed
    // by the CURLM multi-handle that has been deleted (dead code).
    // Per-request easy handles manage their own connections.

    /// Optional User-Agent header value.
    std::string user_agent = "XOPTrader-DexieClient/1.0";

    /// Number of threads in the CURL worker pool.  Blocking CURL
    /// transfers are dispatched here so the io_context is never stalled.
    std::uint32_t curl_thread_pool_size{4};
};

// -----------------------------------------------------------------------
// Response types -- thin wrappers around nlohmann::json
// -----------------------------------------------------------------------

/// A single asset reference inside an offer's offered/requested arrays.
struct AssetInfo {
    std::string id;     ///< "xch" or a 64-hex CAT asset ID.
    std::string code;   ///< Human-readable ticker, e.g. "XCH".
    std::string name;   ///< Full display name, e.g. "Chia".
    double      amount = 0.0;
};

/// Base/quote description of a trading pair.
struct PairInfo {
    struct Side {
        std::string id;
        std::string code;
        std::string name;
    };
    Side base;
    Side quote;
};

/// Compact representation of a single offer from the order book.
struct OfferRecord {
    std::string              id;
    int                      status = 0;
    std::string              offer_bech32;       ///< Empty when compact=true.
    std::vector<std::string> involved_coins;
    std::string              date_found;
    std::string              date_completed;
    std::string              date_pending;
    std::string              date_expiry;
    std::optional<uint64_t>  block_expiry;
    std::optional<uint64_t>  spent_block_index;
    double                   price = 0.0;
    std::vector<AssetInfo>   offered;
    std::vector<AssetInfo>   requested;
    uint64_t                 fees = 0;
    int                      mod_version = 0;
    std::string              trade_id;
};

/// Paginated response wrapper.
struct OffersPage {
    bool                     success = false;
    uint64_t                 count   = 0;        ///< Total matching offers.
    uint64_t                 page    = 0;
    uint64_t                 page_size = 0;
    std::vector<OfferRecord> offers;
};

/// 24-hour ticker data from /v1/markets (one market entry).
struct TickerData {
    std::string id;           ///< CAT asset ID of the quote token.
    std::string code;
    std::string name;
    std::string pair_id;
    bool        incentives = false;

    /// Daily volume denominated in XCH.
    double volume_xch_daily = 0.0;
    /// Daily volume denominated in the quote token.
    double volume_quote_daily = 0.0;

    /// Best buy / sell / last price at depth-0.
    double price_buy  = 0.0;
    double price_sell = 0.0;
    double price_last = 0.0;
    double price_high = 0.0;
    double price_low  = 0.0;
};

/// Result of POST /v1/offers (offer submission).
struct SubmitResult {
    bool        success = false;
    std::string offer_id;
    std::string error_message;
};

/// Result of GET /v1/offers/{id} (single-offer lookup).
struct OfferStatus {
    bool        success = false;
    OfferRecord offer;
    /// Additional fields present only when the offer is settled.
    nlohmann::json input_coins;
    nlohmann::json output_coins;
};

// -----------------------------------------------------------------------
// Error hierarchy
// -----------------------------------------------------------------------

/// Base class for all Dexie HTTP errors.
class DexieError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Non-retryable client error (HTTP 400-499 except 429).
class DexieClientError : public DexieError {
public:
    explicit DexieClientError(int status, std::string body)
        : DexieError("HTTP " + std::to_string(status) + ": " + body),
          status_code(status),
          response_body(std::move(body)) {}
    int         status_code;
    std::string response_body;
};

/// Rate-limit error (HTTP 429) that could not be resolved by retries.
class DexieRateLimitError : public DexieError {
public:
    DexieRateLimitError()
        : DexieError("Rate limit exceeded after maximum retries") {}
};

/// Server-side error (HTTP 5xx) that persisted after retries.
class DexieServerError : public DexieError {
public:
    explicit DexieServerError(int status, std::string body)
        : DexieError("HTTP " + std::to_string(status) + ": " + body),
          status_code(status),
          response_body(std::move(body)) {}
    int         status_code;
    std::string response_body;
};

// -----------------------------------------------------------------------
// Sliding-window rate limiter
// -----------------------------------------------------------------------

/// A token-bucket-style limiter that tracks individual request timestamps
/// inside a configurable sliding window.
///
/// Algorithm:
///   1. Prune every entry whose timestamp is older than (now - window).
///   2. If remaining count >= max_requests, compute the sleep duration
///      needed until the oldest entry expires from the window.
///   3. Otherwise, record the current time and allow the request.
///
/// This is a TRUE sliding window -- not a fixed-window counter that
/// resets on a cadence.  It cannot "burst" beyond max_requests in any
/// contiguous interval of length `window`.
class SlidingWindowRateLimiter {
public:
    /// Construct with capacity and window size.
    SlidingWindowRateLimiter(std::size_t max_requests,
                             std::chrono::milliseconds window);

    /// Non-copyable, non-movable (holds a mutex).
    SlidingWindowRateLimiter(const SlidingWindowRateLimiter&) = delete;
    SlidingWindowRateLimiter& operator=(const SlidingWindowRateLimiter&) = delete;

    /// Block the calling thread until a request slot is available, then
    /// record the current timestamp.  Returns the time spent waiting
    /// (zero if no wait was needed).
    ///
    /// NOTE: Prefer try_acquire() in coroutine contexts to avoid blocking
    /// the io_context event loop thread.
    std::chrono::milliseconds acquire();

    /// Non-blocking attempt to acquire a rate-limiter slot.
    ///
    /// If capacity is available, records the current timestamp and returns
    /// zero (no wait needed).  If the window is full, returns the duration
    /// the caller should wait before retrying (without recording a slot).
    /// The caller is responsible for sleeping asynchronously (e.g. via
    /// boost::asio::steady_timer) and calling try_acquire() again.
    ///
    /// @return Duration to wait (zero if a slot was acquired).
    std::chrono::milliseconds try_acquire();

    /// Number of requests currently inside the window (after pruning).
    /// [T8-10] Const-correct: prune_ is logically const (only removes
    /// stale entries), so timestamps_ is mutable and prune_ is const.
    std::size_t current_count() const;

    /// Reset internal state (useful in tests).
    void reset();

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Remove entries older than (now - window_).
    /// [T8-10] Marked const -- mutates only mutable members.
    void prune_(TimePoint now) const;

    std::size_t                    max_requests_;
    std::chrono::milliseconds      window_;
    mutable std::mutex             mu_;
    mutable std::deque<TimePoint>  timestamps_;
};

// -----------------------------------------------------------------------
// CURL handle RAII helpers
// -----------------------------------------------------------------------

/// Custom deleters for libcurl handles so unique_ptr cleans up correctly.
/// ISO/IEC 5055 -- RAII via unique_ptr with custom deleters; deterministic
/// resource release with no manual cleanup paths.
struct CurlEasyDeleter {
    void operator()(CURL* p) const noexcept {
        if (p) curl_easy_cleanup(p);
    }
};
// NOTE (MEDIUM-4): CurlMultiDeleter / CurlMultiPtr removed -- the CURLM
// multi-handle was allocated but never used (per-request easy handles were
// never attached to it).  Removed to eliminate dead code.
struct CurlSlistDeleter {
    void operator()(curl_slist* p) const noexcept {
        if (p) curl_slist_free_all(p);
    }
};

using CurlEasyPtr  = std::unique_ptr<CURL,       CurlEasyDeleter>;
using CurlSlistPtr = std::unique_ptr<curl_slist,  CurlSlistDeleter>;

// -----------------------------------------------------------------------
// DexieClient
// -----------------------------------------------------------------------

/// Async REST client for the dexie.space v1 public API.
///
/// Lifecycle:
///   DexieClient client(ioc, config);
///   client.open();          // initialises CURL thread pool
///   auto pairs = co_await client.get_pairs();
///   client.close();         // joins thread pool, releases state
///
/// Every public method that hits the network will:
///   1. Acquire a rate-limiter slot via async timer (never blocks io_context).
///   2. Dispatch the CURL transfer to the thread pool (non-blocking).
///   3. Retry automatically on 429 / 5xx (up to max_retries) with async
///      exponential backoff via steady_timer.
///   4. Parse JSON and return a typed result (or throw).
class DexieClient {
public:
    /// Construct an unopened client with the given configuration.
    ///
    /// @param ioc     The Boost.Asio io_context for async timers.
    /// @param config  Transport and rate-limiter configuration.
    explicit DexieClient(boost::asio::io_context& ioc,
                         DexieConfig              config);

    ~DexieClient();

    // -- Session lifecycle ------------------------------------------------

    /// Initialise the CURL thread pool for async request dispatch.
    /// Safe to call more than once (subsequent calls are no-ops).
    void open();

    /// Join the thread pool and release all state.
    void close();

    /// True after a successful open() and before close().
    [[nodiscard]] bool is_open() const noexcept;

    // -- Market data endpoints -------------------------------------------

    /// GET /v1/pairs
    /// Returns every known trading pair (base + quote asset descriptors).
    [[nodiscard]] boost::asio::awaitable<std::vector<PairInfo>> get_pairs();

    /// GET /v1/offers
    /// Returns a paginated, optionally filtered set of offers.
    ///
    /// @param pair_id    Filter by pair_id (e.g. "xch").  Empty = all.
    /// @param offered    Filter by offered asset id (e.g. "xch").
    /// @param requested  Filter by requested asset id.
    /// @param page       1-based page index.
    /// @param page_size  Results per page (max typically 100).
    /// @param sort       Sorting key: "price_asc", "price_desc",
    ///                   "date_found_asc", "date_found_desc", etc.
    /// @param compact    If true the response omits bech32 offer strings.
    /// @param status     Offer status filter (0=active, 4=completed, ...).
    [[nodiscard]] boost::asio::awaitable<OffersPage> get_offers(
        std::string_view pair_id    = {},
        std::string_view offered    = {},
        std::string_view requested  = {},
        uint32_t         page       = 1,
        uint32_t         page_size  = 20,
        std::string_view sort       = {},
        bool             compact    = false,
        std::optional<int> status   = std::nullopt);

    /// GET /v1/markets
    /// Returns 24-hour ticker data for every market grouped by base asset.
    [[nodiscard]] boost::asio::awaitable<std::vector<TickerData>> get_tickers();

    /// GET /v1/markets  (filtered client-side for a single asset pair)
    /// Convenience wrapper that returns only the ticker matching the
    /// configured base and quote asset IDs.
    [[nodiscard]] boost::asio::awaitable<std::optional<TickerData>> get_ticker(
        std::string_view base_asset_id,
        std::string_view quote_asset_id);

    /// GET /v1/markets  (returns the raw JSON for advanced consumers)
    /// Exposes the full depth/liquidity/price structure.
    [[nodiscard]] boost::asio::awaitable<nlohmann::json> get_prices();

    /// GET /v1/offers?status=4  (settled trades, most recent first)
    /// Convenience wrapper around get_offers with status=4 (completed).
    [[nodiscard]] boost::asio::awaitable<OffersPage> get_trades(
        std::string_view pair_id   = {},
        uint32_t         page      = 1,
        uint32_t         page_size = 20);

    // -- Order management ------------------------------------------------

    /// POST /v1/offers  { "offer": "<bech32m>", "claim_rewards": true }
    /// Submits a new offer to the dexie.space aggregator.
    /// When claim_rewards is true, dexie automatically claims DBX
    /// liquidity incentive rewards for qualifying offers.
    [[nodiscard]] boost::asio::awaitable<SubmitResult> submit_offer(
        std::string_view offer_bech32m,
        bool claim_rewards = true);

    /// GET /v1/offers/{offer_id}
    /// Retrieves the current status of a previously submitted offer.
    [[nodiscard]] boost::asio::awaitable<OfferStatus> get_offer_status(
        std::string_view offer_id);

    // -- Diagnostics -----------------------------------------------------

    /// Current rate-limiter occupancy (requests in the active window).
    [[nodiscard]] std::size_t rate_limiter_count() const;

    /// Immutable reference to the active configuration.
    [[nodiscard]] const DexieConfig& config() const noexcept;

private:
    // -- Internal HTTP helpers -------------------------------------------

    /// Low-level async GET that returns parsed JSON.  Handles async rate
    /// limiting, thread-pool dispatch, retries, and error classification.
    [[nodiscard]] boost::asio::awaitable<nlohmann::json> http_get_(
        const std::string& path);

    /// Low-level async POST (JSON body) with the same guarantees.
    [[nodiscard]] boost::asio::awaitable<nlohmann::json> http_post_(
        const std::string& path,
        const nlohmann::json& body);

    /// Shared async request execution with retry loop.
    /// Rate-limit waits use steady_timer (non-blocking); CURL transfers
    /// are dispatched to thread_pool_ via co_spawn.
    ///
    /// @param method  "GET" or "POST".
    /// @param url     Fully qualified URL.
    /// @param body    POST payload (empty string for GET).
    /// @return Parsed JSON response body.
    [[nodiscard]] boost::asio::awaitable<nlohmann::json> execute_request_(
        std::string_view method,
        const std::string& url,
        const std::string& body);

    /// Perform a single synchronous CURL transfer (blocking).
    /// Called from within the thread pool.  Returns parsed result via
    /// out-parameters.
    ///
    /// @param method       "GET" or "POST".
    /// @param url          Full request URL.
    /// @param body         POST payload (empty string for GET).
    /// @param[out] response_body  Raw response text.
    /// @param[out] http_status    HTTP status code.
    /// @return CURLcode indicating transport-level outcome.
    CURLcode perform_request_(std::string_view   method,
                              const std::string& url,
                              const std::string& body,
                              std::string&       response_body,
                              long&              http_status);

    /// Build a query-string from key-value pairs, URL-encoding values.
    [[nodiscard]] static std::string build_query_(
        const std::vector<std::pair<std::string, std::string>>& params);

    // -- JSON -> struct parsing helpers -----------------------------------

    [[nodiscard]] static AssetInfo   parse_asset_(const nlohmann::json& j);
    [[nodiscard]] static OfferRecord parse_offer_(const nlohmann::json& j);
    [[nodiscard]] static TickerData  parse_ticker_(const nlohmann::json& j,
                                                   std::string_view base_asset);

    // -- State ------------------------------------------------------------

    boost::asio::io_context&             ioc_;
    DexieConfig                          cfg_;
    bool                                 open_ = false;
    // NOTE (MEDIUM-4): CurlMultiPtr multi_ removed -- was dead code.
    std::shared_ptr<spdlog::logger>      log_;
    SlidingWindowRateLimiter             limiter_;

    /// Thread pool for offloading blocking CURL transfers.
    /// Created in open(), joined and destroyed in close().
    std::unique_ptr<boost::asio::thread_pool> thread_pool_;
};

} // namespace xop::rpc

#endif // XOP_RPC_DEXIE_CLIENT_HPP
