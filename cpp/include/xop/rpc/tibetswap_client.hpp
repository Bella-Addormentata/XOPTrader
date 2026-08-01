// tibetswap_client.hpp -- Async REST client for the TibetSwap v2 AMM API.
//
// TibetSwap is the on-chain constant-product AMM on Chia.  Every pool pairs
// native XCH against a single CAT, so its marginal price is an independent,
// arbitrage-anchored reference for any pair we quote that has an XCH leg.
//
// Until this client existed, ArbitrageDetector::set_tibetswap_reserves() had
// zero call sites: cached_tibetswap_reserves_ was permanently empty, so both
// ArbitrageDetector::scan_cross_dex() and MarketDataFeed::ingest_amm_mid()
// were unreachable dead code.  This client is the missing producer.
//
// Architecture mirrors CoinGeckoClient / DexieClient exactly:
//   - Per-request CURL easy handles (RAII via CurlEasyPtr)
//   - Sliding-window rate limiter
//   - Blocking CURL transfers dispatched to a boost::asio::thread_pool
//   - Non-blocking rate-limit waits via boost::asio::steady_timer
//   - Automatic retry on 429 / 5xx; hard throw on 4xx
//
// Endpoints used (all plain GET, JSON, no authentication):
//   GET {base_url}/pairs?skip={n}&limit={n}   -- pool directory (all pools)
//   GET {base_url}/pair/{pair_id}             -- live reserves for one pool
//
// Fetch strategy: the pool directory is large (~370 pools) and its
// asset_id -> pair_id mapping is effectively static, so it is resolved once
// and cached for directory_refresh_ms.  Each poll then issues one small
// GET /pair/{pair_id} per configured asset.
//
// Units (confirmed against the live API 2026-08-01):
//   xch_reserve   -- XCH mojos          (1 XCH  = 10^12 mojos)
//   token_reserve -- CAT mojos          (1 unit = 10^3  mojos)
//   inverse_fee   -- 993 => 0.7% fee    (fee per-mille numerator = 1000 - 993)
// The mojos-per-unit denominations are carried through into TibetSwapReserves
// (da85235) so tibet::get_implied_price() is finally called with the rescale
// it requires; without it XCH/DBX reports 1.018e-7 instead of 101.83.
//
// Thread safety: public interface may be called from coroutines on the same
// io_context.  Rate limiter uses an internal mutex; CURL handles are
// per-request.
//
// ISO/IEC 27001:2022 -- no credentials are involved; the API is unauthenticated.
// ISO/IEC 5055 -- no raw owning pointers; RAII via unique_ptr with custom
// deleters for every libcurl handle.

#ifndef XOP_RPC_TIBETSWAP_CLIENT_HPP
#define XOP_RPC_TIBETSWAP_CLIENT_HPP

#include "xop/config.hpp"
#include "xop/rpc/dexie_client.hpp"  // SlidingWindowRateLimiter, CurlEasyPtr, CurlSlistPtr
#include "xop/strategy/arbitrage.hpp"  // TibetSwapReserves

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/thread_pool.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace xop::rpc {

// -----------------------------------------------------------------------
// Error hierarchy (mirrors CoinGeckoError / DexieError)
// -----------------------------------------------------------------------

/// Base class for all TibetSwap HTTP errors.
class TibetSwapError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Non-retryable client error (HTTP 400-499 except 429).
class TibetSwapClientError : public TibetSwapError {
public:
    explicit TibetSwapClientError(int status, std::string body)
        : TibetSwapError("HTTP " + std::to_string(status) + ": " + body),
          status_code(status),
          response_body(std::move(body)) {}
    int         status_code;
    std::string response_body;
};

/// Rate-limit error (HTTP 429) that could not be resolved by retries.
class TibetSwapRateLimitError : public TibetSwapError {
public:
    TibetSwapRateLimitError()
        : TibetSwapError("TibetSwap rate limit exceeded after maximum retries") {}
};

/// Server-side error (HTTP 5xx) that persisted after retries.
class TibetSwapServerError : public TibetSwapError {
public:
    explicit TibetSwapServerError(int status, std::string body)
        : TibetSwapError("HTTP " + std::to_string(status) + ": " + body),
          status_code(status),
          response_body(std::move(body)) {}
    int         status_code;
    std::string response_body;
};

// -----------------------------------------------------------------------
// TibetSwapPool -- one parsed pool record from the API
// -----------------------------------------------------------------------

/// A single TibetSwap liquidity pool (always XCH vs one CAT).
///
/// Field names match the API payload exactly so the parser stays auditable.
/// Reserves are raw mojos on both sides -- see the unit note at the top of
/// this file.
struct TibetSwapPool {
    std::string   pair_id;        // Pool launcher ID (64 hex).
    std::string   asset_id;       // CAT tail / asset ID (64 hex), lower-cased.
    std::string   short_name;     // e.g. "BYC".
    std::string   name;           // e.g. "Bytecash".
    std::int64_t  xch_reserve{0};    // XCH-side reserve, mojos (10^12 / XCH).
    std::int64_t  token_reserve{0};  // CAT-side reserve, mojos (10^3 / unit).
    std::uint32_t inverse_fee{993};  // 993 => 0.7% swap fee.
    std::int64_t  liquidity{0};      // LP token supply (diagnostics only).

    /// Pool fee expressed as the per-mille numerator used by
    /// TibetSwapReserves::fee_bps (993 -> 7, i.e. 0.7%).
    [[nodiscard]] std::uint32_t fee_per_mille() const noexcept {
        return (inverse_fee > 0 && inverse_fee < 1000)
             ? (1000u - inverse_fee)
             : 0u;
    }
};

// -----------------------------------------------------------------------
// Pure parsing helpers -- no network, no client state.
//
// Exposed so response handling can be unit-tested against captured payloads
// without any live network access.
// -----------------------------------------------------------------------

/// Parse a single pool object.
///
/// Returns std::nullopt when the JSON is not an object, is missing a required
/// field, has a field of the wrong type, or carries a non-positive reserve.
/// Never throws.
[[nodiscard]] std::optional<TibetSwapPool>
parse_pool_json(const nlohmann::json& node);

/// Parse an array of pool objects (the /pairs payload).
///
/// Malformed entries are skipped with a warning rather than failing the whole
/// batch -- one bad pool in a 370-pool directory must not blind us to the
/// other 369.  A non-array input yields an empty vector.  Never throws.
[[nodiscard]] std::vector<TibetSwapPool>
parse_pools_json(const nlohmann::json& node);

/// Map fetched pools onto the configured trading pairs, producing the
/// TibetSwapReserves vector that ArbitrageDetector::set_tibetswap_reserves()
/// consumes.
///
/// A pair qualifies when exactly one of its legs is native XCH and a pool
/// exists for the other leg's asset ID.  CAT/CAT pairs (e.g. "BYC/wUSDC.b")
/// have no direct TibetSwap pool and are skipped.
///
/// The mojos-per-unit denominations are taken from the pair's own config and
/// assigned to the XCH / token side according to which leg is which, so
/// callers of tibet::get_implied_price() get the correct rescale regardless of
/// pair orientation.
///
/// @param pools             Pools fetched from the API.
/// @param pairs             Configured pairs (disabled pairs are skipped).
/// @param fallback_fee_bps  Per-mille fee to use when a pool reports an
///                          out-of-range inverse_fee (default 7 = 0.7%).
[[nodiscard]] std::vector<TibetSwapReserves>
build_tibetswap_reserves(const std::vector<TibetSwapPool>& pools,
                         const std::vector<PairConfig>&    pairs,
                         std::uint32_t                     fallback_fee_bps = 7);

// -----------------------------------------------------------------------
// TibetSwapClient
// -----------------------------------------------------------------------

/// Async REST client for the TibetSwap v2 AMM API.
///
/// Lifecycle:
///   TibetSwapClient client(ioc, config);
///   client.open();
///   auto pools = co_await client.fetch_pools_for_assets(asset_ids);
///   client.close();
class TibetSwapClient {
public:
    /// Construct an unopened client.
    ///
    /// @param ioc     The Boost.Asio io_context for async timers.
    /// @param config  TibetSwap configuration from AppConfig.
    explicit TibetSwapClient(boost::asio::io_context& ioc,
                             const TibetSwapConfig&   config);

    ~TibetSwapClient();

    // Non-copyable, non-movable.
    TibetSwapClient(const TibetSwapClient&)            = delete;
    TibetSwapClient& operator=(const TibetSwapClient&) = delete;
    TibetSwapClient(TibetSwapClient&&)                 = delete;
    TibetSwapClient& operator=(TibetSwapClient&&)      = delete;

    // -- Session lifecycle ------------------------------------------------

    /// Initialise the CURL thread pool.  Idempotent.
    void open();

    /// Join the thread pool and release state.
    void close();

    /// True after open() and before close().
    [[nodiscard]] bool is_open() const noexcept;

    // -- Pool fetching ----------------------------------------------------

    /// Fetch the complete pool directory: GET /pairs?skip=&limit=
    ///
    /// Pages until the API returns fewer than page_limit records or
    /// max_pools is reached.
    [[nodiscard]] boost::asio::awaitable<std::vector<TibetSwapPool>>
    fetch_all_pools();

    /// Fetch live reserves for one pool: GET /pair/{pair_id}
    ///
    /// @throws TibetSwapError on transport failure, HTTP error, or a payload
    ///         that does not parse into a usable pool.
    [[nodiscard]] boost::asio::awaitable<TibetSwapPool>
    fetch_pool(const std::string& pair_id);

    /// Fetch live reserves for the given CAT asset IDs.
    ///
    /// Resolves asset_id -> pair_id from the cached pool directory, refreshing
    /// it when it is stale or when a requested asset is unknown.  Issues one
    /// GET /pair/{pair_id} per resolved asset.
    ///
    /// Per-asset isolation: an asset that cannot be resolved or whose fetch
    /// fails is logged and omitted from the result; the remaining assets are
    /// still returned.  Only a failure to obtain the directory at all (when it
    /// has never been populated) propagates as an exception.
    [[nodiscard]] boost::asio::awaitable<std::vector<TibetSwapPool>>
    fetch_pools_for_assets(const std::vector<std::string>& asset_ids);

    // -- Diagnostics ------------------------------------------------------

    /// Number of pools currently held in the asset_id -> pair_id directory.
    [[nodiscard]] std::size_t directory_size() const noexcept;

    /// Current rate-limiter occupancy.
    [[nodiscard]] std::size_t rate_limiter_count() const;

    /// Immutable reference to the active configuration.
    [[nodiscard]] const TibetSwapConfig& config() const noexcept;

    /// Seed the asset_id -> pair_id directory directly.  Test seam: lets the
    /// resolution logic be exercised without any network access.
    void seed_directory(const std::vector<TibetSwapPool>& pools);

private:
    // -- Internal HTTP helpers --------------------------------------------

    /// Shared async request execution with rate limiting + retry loop.
    [[nodiscard]] boost::asio::awaitable<nlohmann::json> execute_request_(
        const std::string& url);

    /// Perform a single synchronous CURL GET (blocking).
    /// Called from within the thread pool.
    CURLcode perform_request_(const std::string& url,
                              std::string&       response_body,
                              long&              http_status);

    /// Refresh the asset_id -> pair_id directory from GET /pairs.
    [[nodiscard]] boost::asio::awaitable<void> refresh_directory_();

    // -- State ------------------------------------------------------------

    boost::asio::io_context&        ioc_;
    TibetSwapConfig                 cfg_;
    bool                            open_ = false;
    std::shared_ptr<spdlog::logger> log_;
    SlidingWindowRateLimiter        limiter_;

    /// asset_id (lower-case hex) -> pair_id.  Effectively static per pool.
    std::unordered_map<std::string, std::string> asset_to_pair_id_;

    /// When the directory was last successfully refreshed.
    std::chrono::steady_clock::time_point directory_refreshed_at_{};
    bool                                  directory_populated_{false};

    /// Thread pool for offloading blocking CURL transfers.
    std::unique_ptr<boost::asio::thread_pool> thread_pool_;
};

} // namespace xop::rpc

#endif // XOP_RPC_TIBETSWAP_CLIENT_HPP
