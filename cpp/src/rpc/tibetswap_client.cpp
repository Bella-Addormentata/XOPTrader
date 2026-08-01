// tibetswap_client.cpp -- Implementation of the TibetSwap v2 AMM REST client.
//
// Transport   : libcurl easy handles (per-request, RAII) dispatched to a
//               dedicated thread pool so the io_context is never blocked.
// Rate control: true sliding-window limiter with non-blocking async waits
//               via boost::asio::steady_timer.
// Retry policy: exponential back-off on HTTP 429 / 5xx via async timer;
//               immediate throw on 4xx client errors.
//
// ISO/IEC 5055 -- no raw owning pointers; RAII throughout; per-request CURL
//                 handles eliminate shared-state data races.

#include "xop/rpc/tibetswap_client.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace xop::rpc {

namespace asio = boost::asio;

// =======================================================================
// Local helpers
// =======================================================================

namespace {

/// libcurl invokes this for every chunk of the response body.
std::size_t tibetswap_write_cb(char*       data,
                               std::size_t /*size -- always 1*/,
                               std::size_t nmemb,
                               void*       userdata) {
    if (!userdata) {
        return 0;
    }
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(data, nmemb);
    return nmemb;
}

/// Lower-case an ASCII hex string so asset IDs compare case-insensitively.
std::string to_lower_hex(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

/// Shared logger for the free parsing helpers (which have no client state).
std::shared_ptr<spdlog::logger> parse_log() {
    auto log = spdlog::get("tibetswap");
    if (!log) {
        log = spdlog::stdout_color_mt("tibetswap");
    }
    return log;
}

/// Extract a JSON field as int64, accepting integer, unsigned, float, or
/// decimal-string encodings.  Returns false when the field is absent, null,
/// of an unusable type, or out of int64 range.
bool json_to_int64(const nlohmann::json& node,
                   const char*           key,
                   std::int64_t&         out) {
    const auto it = node.find(key);
    if (it == node.end() || it->is_null()) {
        return false;
    }

    if (it->is_number_integer()) {
        out = it->get<std::int64_t>();
        return true;
    }
    if (it->is_number_unsigned()) {
        const auto u = it->get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(u);
        return true;
    }
    if (it->is_number_float()) {
        const double d = it->get<double>();
        if (!std::isfinite(d) ||
            d < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
            d > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(d);
        return true;
    }
    if (it->is_string()) {
        try {
            std::size_t consumed = 0;
            const std::string s = it->get<std::string>();
            const long long v = std::stoll(s, &consumed);
            if (consumed != s.size()) {
                return false;
            }
            out = static_cast<std::int64_t>(v);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

/// Extract a JSON field as a string.  Returns false when absent/null/wrong type.
bool json_to_string(const nlohmann::json& node,
                    const char*           key,
                    std::string&          out) {
    const auto it = node.find(key);
    if (it == node.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

} // anonymous namespace

// =======================================================================
// Pure parsing helpers
// =======================================================================

std::optional<TibetSwapPool>
parse_pool_json(const nlohmann::json& node) {
    if (!node.is_object()) {
        return std::nullopt;
    }

    TibetSwapPool pool;

    if (!json_to_string(node, "pair_id", pool.pair_id) ||
        pool.pair_id.empty()) {
        return std::nullopt;
    }
    if (!json_to_string(node, "asset_id", pool.asset_id) ||
        pool.asset_id.empty()) {
        return std::nullopt;
    }
    pool.asset_id = to_lower_hex(std::move(pool.asset_id));
    pool.pair_id  = to_lower_hex(std::move(pool.pair_id));

    if (!json_to_int64(node, "xch_reserve", pool.xch_reserve) ||
        !json_to_int64(node, "token_reserve", pool.token_reserve)) {
        return std::nullopt;
    }

    // A pool with a non-positive reserve has no meaningful marginal price and
    // would divide by zero downstream.  Reject it here so it can never reach
    // tibet::get_implied_price().
    if (pool.xch_reserve <= 0 || pool.token_reserve <= 0) {
        return std::nullopt;
    }

    // Optional / cosmetic fields: absent values keep their defaults.
    json_to_string(node, "asset_short_name", pool.short_name);
    json_to_string(node, "asset_name",       pool.name);
    json_to_int64 (node, "liquidity",        pool.liquidity);

    std::int64_t inverse_fee = 0;
    if (json_to_int64(node, "inverse_fee", inverse_fee) &&
        inverse_fee > 0 && inverse_fee < 1000) {
        pool.inverse_fee = static_cast<std::uint32_t>(inverse_fee);
    }
    // else: keep the 993 default (0.7%), the value every live pool reports.

    return pool;
}

std::vector<TibetSwapPool>
parse_pools_json(const nlohmann::json& node) {
    std::vector<TibetSwapPool> pools;

    if (!node.is_array()) {
        parse_log()->warn("TibetSwap /pairs payload is not an array "
                          "(type={}) -- treating as empty",
                          node.type_name());
        return pools;
    }

    pools.reserve(node.size());
    std::size_t skipped = 0;

    for (const auto& item : node) {
        auto parsed = parse_pool_json(item);
        if (!parsed) {
            ++skipped;
            continue;
        }
        pools.push_back(std::move(*parsed));
    }

    if (skipped > 0) {
        parse_log()->warn("TibetSwap /pairs: skipped {} malformed pool "
                          "record(s) of {}", skipped, node.size());
    }

    return pools;
}

std::vector<TibetSwapReserves>
build_tibetswap_reserves(const std::vector<TibetSwapPool>& pools,
                         const std::vector<PairConfig>&    pairs,
                         std::uint32_t                     fallback_fee_bps) {

    // Index the pools by CAT asset ID for O(1) lookup.
    std::unordered_map<std::string, const TibetSwapPool*> by_asset;
    by_asset.reserve(pools.size());
    for (const auto& pool : pools) {
        by_asset.emplace(to_lower_hex(pool.asset_id), &pool);
    }

    std::vector<TibetSwapReserves> out;
    out.reserve(pairs.size());

    for (const auto& pair : pairs) {
        if (!pair.enabled) {
            continue;
        }

        const std::string base  = to_lower_hex(pair.base_asset_id);
        const std::string quote = to_lower_hex(pair.quote_asset_id);

        const bool base_is_xch  = (base  == "xch");
        const bool quote_is_xch = (quote == "xch");

        // TibetSwap pools are always XCH vs exactly one CAT.  A CAT/CAT pair
        // (e.g. "BYC/wUSDC.b") has no direct pool; XCH/XCH is nonsense.
        if (base_is_xch == quote_is_xch) {
            continue;
        }

        const std::string& cat_asset = base_is_xch ? quote : base;

        const auto it = by_asset.find(cat_asset);
        if (it == by_asset.end()) {
            continue;
        }
        const TibetSwapPool& pool = *it->second;

        TibetSwapReserves reserves;
        reserves.pair_name     = pair.name;
        reserves.xch_reserve   = pool.xch_reserve;
        reserves.token_reserve = pool.token_reserve;

        const std::uint32_t fee = pool.fee_per_mille();
        reserves.fee_bps = (fee > 0) ? fee : fallback_fee_bps;

        // da85235: the denominations must follow the assets, not the reserve
        // slots.  The XCH-side reserve is scaled by whichever leg of the pair
        // is XCH; the token-side reserve by the other leg.  Getting this
        // backwards is a 10^9 error on any CAT pair.
        reserves.xch_mojos_per_unit   = base_is_xch ? pair.base_mojos_per_unit
                                                    : pair.quote_mojos_per_unit;
        reserves.token_mojos_per_unit = base_is_xch ? pair.quote_mojos_per_unit
                                                    : pair.base_mojos_per_unit;

        out.push_back(std::move(reserves));
    }

    return out;
}

// =======================================================================
// TibetSwapClient -- construction / destruction / session lifecycle
// =======================================================================

TibetSwapClient::TibetSwapClient(asio::io_context&      ioc,
                                 const TibetSwapConfig& config)
    : ioc_(ioc),
      cfg_(config),
      limiter_(cfg_.rate_limit_max_requests,
               std::chrono::milliseconds{cfg_.rate_limit_window_ms}) {

    log_ = spdlog::get("tibetswap");
    if (!log_) {
        log_ = spdlog::stdout_color_mt("tibetswap");
    }
}

TibetSwapClient::~TibetSwapClient() {
    close();
}

void TibetSwapClient::open() {
    if (open_) {
        return;
    }

    const auto pool_size = std::max(1u, cfg_.curl_thread_pool_size);
    thread_pool_ = std::make_unique<asio::thread_pool>(pool_size);

    open_ = true;
    log_->info("TibetSwapClient opened (base_url={}, pool_size={})",
               cfg_.base_url, pool_size);
}

void TibetSwapClient::close() {
    if (!open_) {
        return;
    }

    if (thread_pool_) {
        thread_pool_->join();
        thread_pool_.reset();
    }

    open_ = false;
    log_->info("TibetSwapClient closed");
}

bool TibetSwapClient::is_open() const noexcept {
    return open_;
}

const TibetSwapConfig& TibetSwapClient::config() const noexcept {
    return cfg_;
}

std::size_t TibetSwapClient::rate_limiter_count() const {
    return limiter_.current_count();
}

std::size_t TibetSwapClient::directory_size() const noexcept {
    return asset_to_pair_id_.size();
}

void TibetSwapClient::seed_directory(const std::vector<TibetSwapPool>& pools) {
    asset_to_pair_id_.clear();
    asset_to_pair_id_.reserve(pools.size());
    for (const auto& pool : pools) {
        asset_to_pair_id_[to_lower_hex(pool.asset_id)] = pool.pair_id;
    }
    directory_populated_    = !asset_to_pair_id_.empty();
    directory_refreshed_at_ = std::chrono::steady_clock::now();
}

// =======================================================================
// Internal HTTP: blocking CURL transfer
// =======================================================================

CURLcode TibetSwapClient::perform_request_(
    const std::string& url,
    std::string&       response_body,
    long&              http_status) {

    CurlEasyPtr easy(curl_easy_init());
    if (!easy) {
        log_->error("curl_easy_init returned null in perform_request_");
        return CURLE_FAILED_INIT;
    }

    response_body.clear();
    response_body.reserve(4096);
    http_status = 0;

    curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, tibetswap_write_cb);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &response_body);

    // Timeouts.
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS,
                     static_cast<long>(cfg_.request_timeout_ms));
    curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(cfg_.connect_timeout_ms));

    // Connection keep-alive.
    curl_easy_setopt(easy.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_TCP_KEEPIDLE,  60L);
    curl_easy_setopt(easy.get(), CURLOPT_TCP_KEEPINTVL, 30L);

    // Follow redirects (up to 5 hops).
    curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_MAXREDIRS, 5L);

    // Accept compressed responses -- the /pairs directory is ~370 records.
    curl_easy_setopt(easy.get(), CURLOPT_ACCEPT_ENCODING, "");

    // TLS verification (public API -- system CA bundle).
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, 2L);
#if defined(CURLSSLOPT_NATIVE_CA)
    curl_easy_setopt(easy.get(), CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

    // User-Agent header.
    curl_easy_setopt(easy.get(), CURLOPT_USERAGENT, cfg_.user_agent.c_str());

    // Build headers (RAII).  The API is unauthenticated -- no secrets here.
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Accept: application/json");

    CurlSlistPtr headers_guard(raw_headers);
    curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, raw_headers);

    // Execute blocking transfer.
    const CURLcode rc = curl_easy_perform(easy.get());
    if (rc == CURLE_OK) {
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &http_status);
    }

    return rc;
}

// =======================================================================
// Internal HTTP: async request with rate limiting + retry
// =======================================================================

asio::awaitable<nlohmann::json> TibetSwapClient::execute_request_(
    const std::string& url) {

    if (!open_) {
        throw TibetSwapError("TibetSwapClient is not open; call open() first");
    }

    std::size_t attempt = 0;
    auto retry_delay = std::chrono::milliseconds{cfg_.retry_base_delay_ms};

    for (;;) {
        // 1. Acquire a rate-limiter slot (non-blocking).
        for (;;) {
            const auto wait_dur = limiter_.try_acquire();
            if (wait_dur.count() == 0) {
                break;
            }
            log_->debug("Rate limiter: async wait {}ms before GET {}",
                        wait_dur.count(), url);
            asio::steady_timer timer(ioc_, wait_dur);
            co_await timer.async_wait(asio::use_awaitable);
        }

        // 2. Dispatch blocking CURL transfer to thread pool.
        std::string response_body;
        long        http_status = 0;

        const CURLcode rc = co_await asio::co_spawn(
            thread_pool_->get_executor(),
            [this, &url, &response_body, &http_status]()
                -> asio::awaitable<CURLcode>
            {
                co_return perform_request_(url, response_body, http_status);
            },
            asio::use_awaitable);

        if (rc != CURLE_OK) {
            log_->error("GET {} -- curl error: {}", url,
                        curl_easy_strerror(rc));
            throw TibetSwapError(std::string("curl_easy_perform: ") +
                                 curl_easy_strerror(rc));
        }

        log_->debug("GET {} -> HTTP {} ({} bytes)", url,
                    http_status, response_body.size());

        // 3. Classify response.

        // Success (2xx).
        if (http_status >= 200 && http_status < 300) {
            try {
                co_return nlohmann::json::parse(response_body);
            } catch (const nlohmann::json::parse_error& ex) {
                log_->error("JSON parse error on GET {}: {}", url, ex.what());
                throw TibetSwapError(std::string("JSON parse error: ") +
                                     ex.what());
            }
        }

        // Retryable: 429 or 5xx.
        if (http_status == 429 ||
            (http_status >= 500 && http_status < 600)) {

            ++attempt;
            if (attempt > cfg_.max_retries) {
                if (http_status == 429) {
                    log_->error("Rate limit exhausted after {} retries on "
                                "GET {}", cfg_.max_retries, url);
                    throw TibetSwapRateLimitError();
                }
                log_->error("Server error {} persisted after {} retries on "
                            "GET {}", http_status, cfg_.max_retries, url);
                throw TibetSwapServerError(static_cast<int>(http_status),
                                           response_body);
            }

            log_->warn("HTTP {} on GET {} -- retry {}/{} in {}ms",
                       http_status, url, attempt, cfg_.max_retries,
                       retry_delay.count());

            asio::steady_timer timer(ioc_, retry_delay);
            co_await timer.async_wait(asio::use_awaitable);
            retry_delay *= 2;  // Exponential backoff.
            continue;
        }

        // Non-retryable 4xx.
        throw TibetSwapClientError(static_cast<int>(http_status),
                                   response_body);
    }
}

// =======================================================================
// Public API: fetch_all_pools
// =======================================================================

asio::awaitable<std::vector<TibetSwapPool>>
TibetSwapClient::fetch_all_pools() {

    std::vector<TibetSwapPool> all;

    const std::uint32_t page_limit = std::max(1u, cfg_.page_limit);
    std::uint32_t       skip       = 0;

    for (;;) {
        std::ostringstream url;
        url << cfg_.base_url << "/pairs?skip=" << skip
            << "&limit=" << page_limit;

        nlohmann::json body = co_await execute_request_(url.str());
        auto page = parse_pools_json(body);

        // The number of RECORDS returned (not the number that parsed) decides
        // whether another page exists -- otherwise a page of malformed records
        // would silently truncate the directory.
        const std::size_t returned = body.is_array() ? body.size() : 0;

        all.insert(all.end(),
                   std::make_move_iterator(page.begin()),
                   std::make_move_iterator(page.end()));

        if (returned < page_limit) {
            break;
        }
        if (all.size() >= cfg_.max_pools) {
            log_->warn("TibetSwap pool directory hit max_pools={} -- "
                       "truncating", cfg_.max_pools);
            break;
        }
        skip += page_limit;
    }

    log_->info("TibetSwap pool directory fetched: {} pools", all.size());
    co_return all;
}

// =======================================================================
// Public API: fetch_pool
// =======================================================================

asio::awaitable<TibetSwapPool>
TibetSwapClient::fetch_pool(const std::string& pair_id) {

    if (pair_id.empty()) {
        throw TibetSwapError("fetch_pool: empty pair_id");
    }

    const std::string url = cfg_.base_url + "/pair/" + pair_id;

    nlohmann::json body = co_await execute_request_(url);

    auto parsed = parse_pool_json(body);
    if (!parsed) {
        throw TibetSwapError("TibetSwap /pair/" + pair_id +
                             " returned an unusable payload");
    }

    co_return *parsed;
}

// =======================================================================
// Internal: directory refresh
// =======================================================================

asio::awaitable<void> TibetSwapClient::refresh_directory_() {
    auto pools = co_await fetch_all_pools();

    if (pools.empty()) {
        // Keep whatever we already had rather than blanking a good directory.
        log_->warn("TibetSwap directory refresh returned no pools -- "
                   "keeping {} cached entries", asset_to_pair_id_.size());
        co_return;
    }

    seed_directory(pools);
    log_->info("TibetSwap directory refreshed: {} assets mapped",
               asset_to_pair_id_.size());
    co_return;
}

// =======================================================================
// Public API: fetch_pools_for_assets
// =======================================================================

asio::awaitable<std::vector<TibetSwapPool>>
TibetSwapClient::fetch_pools_for_assets(
    const std::vector<std::string>& asset_ids) {

    std::vector<TibetSwapPool> result;
    if (asset_ids.empty()) {
        co_return result;
    }

    // Normalise + de-duplicate the request set.
    std::vector<std::string> wanted;
    wanted.reserve(asset_ids.size());
    for (const auto& raw : asset_ids) {
        auto id = to_lower_hex(raw);
        if (id.empty() || id == "xch") {
            continue;   // Native XCH has no pool of its own.
        }
        if (std::find(wanted.begin(), wanted.end(), id) == wanted.end()) {
            wanted.push_back(std::move(id));
        }
    }
    if (wanted.empty()) {
        co_return result;
    }

    // Decide whether the asset_id -> pair_id directory needs refreshing.
    const auto now = std::chrono::steady_clock::now();
    const auto max_age = std::chrono::milliseconds{cfg_.directory_refresh_ms};

    bool needs_refresh = !directory_populated_;
    if (!needs_refresh && (now - directory_refreshed_at_) >= max_age) {
        needs_refresh = true;
    }
    if (!needs_refresh) {
        for (const auto& id : wanted) {
            if (asset_to_pair_id_.find(id) == asset_to_pair_id_.end()) {
                // A configured asset we have never seen: the directory may
                // predate a newly-listed pool.  Re-resolve once.
                needs_refresh = true;
                break;
            }
        }
    }

    if (needs_refresh) {
        try {
            co_await refresh_directory_();
        } catch (const std::exception& ex) {
            if (!directory_populated_) {
                // Nothing cached to fall back on -- surface the failure so the
                // caller can log it and carry on with no AMM data this cycle.
                log_->error("TibetSwap directory resolution failed: {}",
                            ex.what());
                throw;
            }
            log_->warn("TibetSwap directory refresh failed ({}) -- reusing "
                       "{} cached entries", ex.what(),
                       asset_to_pair_id_.size());
        }
    }

    // Fetch each pool individually.  Per-asset isolation: one bad pool must
    // not cost us the others.
    result.reserve(wanted.size());
    for (const auto& id : wanted) {
        const auto it = asset_to_pair_id_.find(id);
        if (it == asset_to_pair_id_.end()) {
            log_->debug("TibetSwap: no pool for asset {} -- skipping", id);
            continue;
        }

        try {
            auto pool = co_await fetch_pool(it->second);
            result.push_back(std::move(pool));
        } catch (const std::exception& ex) {
            log_->warn("TibetSwap: fetch failed for asset {} (pair_id={}): {}",
                       id, it->second, ex.what());
        }
    }

    log_->debug("TibetSwap reserves fetched for {} of {} requested assets",
                result.size(), wanted.size());
    co_return result;
}

} // namespace xop::rpc
