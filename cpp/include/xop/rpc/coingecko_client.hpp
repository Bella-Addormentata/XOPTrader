// coingecko_client.hpp -- Async REST client for the CoinGecko free API.
//
// Provides external CEX-grade price references for assets with CoinGecko
// listings.  Fetched prices are fed into MarketDataFeed::ingest_cex_reference()
// to activate the existing 70/30 DEX/CEX blending pipeline.
//
// Architecture mirrors DexieClient:
//   - Per-request CURL easy handles (RAII via CurlEasyPtr)
//   - Sliding-window rate limiter (10 req / 60 s for free tier)
//   - Blocking CURL transfers dispatched to a boost::asio::thread_pool
//   - Non-blocking rate-limit waits via boost::asio::steady_timer
//   - Automatic retry on 429 / 5xx; hard throw on 4xx
//
// Thread safety: public interface may be called from coroutines on the
// same io_context.  Rate limiter uses internal mutex; CURL handles are
// per-request.
//
// ISO/IEC 27001:2022 -- api_key (if set) is classified as a secret and
// is never logged.
// ISO/IEC 5055 -- no raw owning pointers; RAII via unique_ptr with
// custom deleters for every libcurl handle.

#ifndef XOP_RPC_COINGECKO_CLIENT_HPP
#define XOP_RPC_COINGECKO_CLIENT_HPP

#include "xop/config.hpp"
#include "xop/rpc/dexie_client.hpp"  // Reuse SlidingWindowRateLimiter, CurlEasyPtr, CurlSlistPtr

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace xop::rpc {

// -----------------------------------------------------------------------
// Error hierarchy
// -----------------------------------------------------------------------

/// Base class for all CoinGecko HTTP errors.
class CoinGeckoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Non-retryable client error (HTTP 400-499 except 429).
class CoinGeckoClientError : public CoinGeckoError {
public:
    explicit CoinGeckoClientError(int status, std::string body)
        : CoinGeckoError("HTTP " + std::to_string(status) + ": " + body),
          status_code(status),
          response_body(std::move(body)) {}
    int         status_code;
    std::string response_body;
};

/// Rate-limit error (HTTP 429) that could not be resolved by retries.
class CoinGeckoRateLimitError : public CoinGeckoError {
public:
    CoinGeckoRateLimitError()
        : CoinGeckoError("CoinGecko rate limit exceeded after maximum retries") {}
};

/// Server-side error (HTTP 5xx) that persisted after retries.
class CoinGeckoServerError : public CoinGeckoError {
public:
    explicit CoinGeckoServerError(int status, std::string body)
        : CoinGeckoError("HTTP " + std::to_string(status) + ": " + body),
          status_code(status),
          response_body(std::move(body)) {}
    int         status_code;
    std::string response_body;
};

// -----------------------------------------------------------------------
// CoinGeckoClient
// -----------------------------------------------------------------------

/// Async REST client for the CoinGecko /simple/price endpoint.
///
/// Lifecycle:
///   CoinGeckoClient client(ioc, config);
///   client.open();
///   auto prices = co_await client.fetch_prices();
///   client.close();
///
/// fetch_prices() returns a map from CoinGecko coin ID to USD price:
///   { "chia": 2.71, "ethereum": 2450.0, "usd-coin": 1.0001 }
class CoinGeckoClient {
public:
    /// Construct an unopened client.
    ///
    /// @param ioc     The Boost.Asio io_context for async timers.
    /// @param config  CoinGecko configuration from AppConfig.
    explicit CoinGeckoClient(boost::asio::io_context& ioc,
                             const CoinGeckoConfig&   config);

    ~CoinGeckoClient();

    // Non-copyable, non-movable.
    CoinGeckoClient(const CoinGeckoClient&)            = delete;
    CoinGeckoClient& operator=(const CoinGeckoClient&) = delete;
    CoinGeckoClient(CoinGeckoClient&&)                 = delete;
    CoinGeckoClient& operator=(CoinGeckoClient&&)      = delete;

    // -- Session lifecycle ------------------------------------------------

    /// Initialise the CURL thread pool.  Idempotent.
    void open();

    /// Join the thread pool and release state.
    void close();

    /// True after open() and before close().
    [[nodiscard]] bool is_open() const noexcept;

    // -- Price fetching ---------------------------------------------------

    /// Fetch current USD prices for all configured coin_ids.
    ///
    /// Calls GET /simple/price?ids=chia,ethereum,...&vs_currencies=usd
    ///
    /// @return Map from CoinGecko coin ID to USD price.
    ///         e.g. { "chia": 2.71, "ethereum": 2450.0 }
    [[nodiscard]] boost::asio::awaitable<std::map<std::string, double>>
    fetch_prices();

    // -- Diagnostics ------------------------------------------------------

    /// Current rate-limiter occupancy.
    [[nodiscard]] std::size_t rate_limiter_count() const;

    /// Immutable reference to the active configuration.
    [[nodiscard]] const CoinGeckoConfig& config() const noexcept;

private:
    // -- Internal HTTP helpers --------------------------------------------

    /// Shared async request execution with retry loop.
    [[nodiscard]] boost::asio::awaitable<nlohmann::json> execute_request_(
        const std::string& url);

    /// Perform a single synchronous CURL GET (blocking).
    /// Called from within the thread pool.
    CURLcode perform_request_(const std::string& url,
                              std::string&       response_body,
                              long&              http_status);

    // -- State ------------------------------------------------------------

    boost::asio::io_context&             ioc_;
    CoinGeckoConfig                      cfg_;
    bool                                 open_ = false;
    std::shared_ptr<spdlog::logger>      log_;
    SlidingWindowRateLimiter             limiter_;

    /// Thread pool for offloading blocking CURL transfers.
    std::unique_ptr<boost::asio::thread_pool> thread_pool_;
};

} // namespace xop::rpc

#endif // XOP_RPC_COINGECKO_CLIENT_HPP
