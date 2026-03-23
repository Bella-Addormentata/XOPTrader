// dexie_client.hpp -- Async REST client for the dexie.space v1 API.
//
// Responsibilities:
//   - HTTP session lifecycle (libcurl multi-handle connection pool)
//   - Sliding-window rate limiter (50 requests per 10-second window)
//   - Typed wrappers for every public dexie.space/v1 endpoint
//   - Automatic retry on 429 / 5xx; hard throw on 4xx client errors
//
// Thread safety: the DexieClient instance is NOT thread-safe.  Drive it
// from a single boost::asio strand and post completions back to the
// caller's executor.
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

    /// libcurl connection-cache size (keep-alive pool).
    long max_connections = 8;

    /// Optional User-Agent header value.
    std::string user_agent = "XOPTrader-DexieClient/1.0";
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
    std::chrono::milliseconds acquire();

    /// Number of requests currently inside the window (after pruning).
    std::size_t current_count() const;

    /// Reset internal state (useful in tests).
    void reset();

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    /// Remove entries older than (now - window_).
    void prune_(TimePoint now);

    std::size_t                    max_requests_;
    std::chrono::milliseconds      window_;
    mutable std::mutex             mu_;
    std::deque<TimePoint>          timestamps_;
};

// -----------------------------------------------------------------------
// CURL handle RAII helpers
// -----------------------------------------------------------------------

/// Custom deleters for libcurl handles so unique_ptr cleans up correctly.
struct CurlEasyDeleter {
    void operator()(CURL* p) const noexcept {
        if (p) curl_easy_cleanup(p);
    }
};
struct CurlMultiDeleter {
    void operator()(CURLM* p) const noexcept {
        if (p) curl_multi_cleanup(p);
    }
};
struct CurlSlistDeleter {
    void operator()(curl_slist* p) const noexcept {
        if (p) curl_slist_free_all(p);
    }
};

using CurlEasyPtr  = std::unique_ptr<CURL,       CurlEasyDeleter>;
using CurlMultiPtr = std::unique_ptr<CURLM,      CurlMultiDeleter>;
using CurlSlistPtr = std::unique_ptr<curl_slist,  CurlSlistDeleter>;

// -----------------------------------------------------------------------
// DexieClient
// -----------------------------------------------------------------------

/// Async-ready REST client for the dexie.space v1 public API.
///
/// Lifecycle:
///   DexieClient client(config);
///   client.open();          // initialises CURL multi-handle pool
///   auto pairs = client.get_pairs();
///   client.close();         // tears down all handles
///
/// Every public method that hits the network will:
///   1. Acquire a rate-limiter slot (may block).
///   2. Execute the HTTP request via the keep-alive connection pool.
///   3. Retry automatically on 429 / 5xx (up to max_retries).
///   4. Parse JSON and return a typed result (or throw).
class DexieClient {
public:
    /// Construct an unopened client with the given configuration.
    explicit DexieClient(DexieConfig config);

    ~DexieClient();

    // -- Session lifecycle ------------------------------------------------

    /// Initialise the libcurl multi-handle and connection pool.
    /// Safe to call more than once (subsequent calls are no-ops).
    void open();

    /// Tear down the connection pool and release all curl handles.
    void close();

    /// True after a successful open() and before close().
    [[nodiscard]] bool is_open() const noexcept;

    // -- Market data endpoints -------------------------------------------

    /// GET /v1/pairs
    /// Returns every known trading pair (base + quote asset descriptors).
    [[nodiscard]] std::vector<PairInfo> get_pairs();

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
    [[nodiscard]] OffersPage get_offers(
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
    [[nodiscard]] std::vector<TickerData> get_tickers();

    /// GET /v1/markets  (filtered client-side for a single pair_id)
    /// Convenience wrapper that returns only the ticker matching pair_id.
    [[nodiscard]] std::optional<TickerData> get_ticker(
        std::string_view pair_id);

    /// GET /v1/markets  (returns the raw JSON for advanced consumers)
    /// Exposes the full depth/liquidity/price structure.
    [[nodiscard]] nlohmann::json get_prices();

    /// GET /v1/offers?status=4  (settled trades, most recent first)
    /// Convenience wrapper around get_offers with status=4 (completed).
    [[nodiscard]] OffersPage get_trades(
        std::string_view pair_id   = {},
        uint32_t         page      = 1,
        uint32_t         page_size = 20);

    // -- Order management ------------------------------------------------

    /// POST /v1/offers  { "offer": "<bech32m>" }
    /// Submits a new offer to the dexie.space aggregator.
    [[nodiscard]] SubmitResult submit_offer(std::string_view offer_bech32m);

    /// GET /v1/offers/{offer_id}
    /// Retrieves the current status of a previously submitted offer.
    [[nodiscard]] OfferStatus get_offer_status(std::string_view offer_id);

    // -- Diagnostics -----------------------------------------------------

    /// Current rate-limiter occupancy (requests in the active window).
    [[nodiscard]] std::size_t rate_limiter_count() const;

    /// Immutable reference to the active configuration.
    [[nodiscard]] const DexieConfig& config() const noexcept;

private:
    // -- Internal HTTP helpers -------------------------------------------

    /// Low-level GET that returns parsed JSON.  Handles rate limiting,
    /// retries, and error classification internally.
    [[nodiscard]] nlohmann::json http_get_(const std::string& path);

    /// Low-level POST (JSON body) with the same guarantees as http_get_.
    [[nodiscard]] nlohmann::json http_post_(const std::string& path,
                                            const nlohmann::json& body);

    /// Shared request execution with retry loop.
    /// @param method  "GET" or "POST".
    /// @param url     Fully qualified URL.
    /// @param body    POST payload (empty string for GET).
    /// @return Parsed JSON response body.
    [[nodiscard]] nlohmann::json execute_request_(
        std::string_view method,
        const std::string& url,
        const std::string& body);

    /// Build a query-string from key-value pairs, URL-encoding values.
    [[nodiscard]] static std::string build_query_(
        const std::vector<std::pair<std::string, std::string>>& params);

    // -- JSON -> struct parsing helpers -----------------------------------

    [[nodiscard]] static AssetInfo   parse_asset_(const nlohmann::json& j);
    [[nodiscard]] static OfferRecord parse_offer_(const nlohmann::json& j);
    [[nodiscard]] static TickerData  parse_ticker_(const nlohmann::json& j,
                                                   std::string_view base_asset);

    // -- State ------------------------------------------------------------

    DexieConfig                          cfg_;
    bool                                 open_ = false;
    CurlMultiPtr                         multi_;
    std::shared_ptr<spdlog::logger>      log_;
    SlidingWindowRateLimiter             limiter_;
};

} // namespace xop::rpc

#endif // XOP_RPC_DEXIE_CLIENT_HPP
