// dexie_client.cpp -- Implementation of the dexie.space v1 REST client.
//
// Transport   : libcurl easy handles (per-request, RAII) dispatched to a
//               dedicated thread pool so the io_context is never blocked.
// Rate control: true sliding-window limiter with non-blocking async waits
//               via boost::asio::steady_timer (no std::this_thread::sleep).
// Retry policy: exponential back-off on HTTP 429 / 5xx via async timer;
//               immediate throw on 4xx client errors.
//
// ISO/IEC 27001:2022 -- offer bech32m payloads are never logged in full.
// ISO/IEC 5055       -- no raw owning pointers; RAII throughout; per-request
//                       CURL handles eliminate shared-state data races.

#include "xop/rpc/dexie_client.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <thread>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace xop::rpc {

namespace asio = boost::asio;

// =======================================================================
// SlidingWindowRateLimiter
// =======================================================================

SlidingWindowRateLimiter::SlidingWindowRateLimiter(
    std::size_t               max_requests,
    std::chrono::milliseconds window)
    : max_requests_(max_requests),
      window_(window) {
    assert(max_requests_ > 0);
    assert(window_.count() > 0);
}

std::chrono::milliseconds SlidingWindowRateLimiter::acquire() {
    using namespace std::chrono;

    milliseconds total_waited{0};

    for (;;) {
        std::unique_lock lock(mu_);

        const auto now = Clock::now();
        prune_(now);

        // If there is capacity, record this request and return.
        if (timestamps_.size() < max_requests_) {
            timestamps_.push_back(now);
            return total_waited;
        }

        // Window is full.  Compute how long until the oldest entry
        // expires, then sleep with the lock released.
        const auto oldest      = timestamps_.front();
        const auto expires_at  = oldest + window_;
        // Add 1 ms of margin to avoid a tight re-check loop due to
        // clock granularity on Windows (typically 15.6 ms ticks).
        const auto sleep_dur   = duration_cast<milliseconds>(
                                     expires_at - now) + milliseconds{1};
        lock.unlock();

        // NOTE: This blocking sleep is retained for non-coroutine callers.
        // Coroutine callers should use try_acquire() + steady_timer instead.
        std::this_thread::sleep_for(sleep_dur);
        total_waited += sleep_dur;
    }
}

std::chrono::milliseconds SlidingWindowRateLimiter::try_acquire() {
    using namespace std::chrono;

    std::unique_lock lock(mu_);

    const auto now = Clock::now();
    prune_(now);

    // If there is capacity, record this request and return zero (acquired).
    if (timestamps_.size() < max_requests_) {
        timestamps_.push_back(now);
        return milliseconds{0};
    }

    // Window is full.  Return the duration the caller should wait before
    // retrying.  We do NOT record a slot -- the caller must call again.
    const auto oldest     = timestamps_.front();
    const auto expires_at = oldest + window_;
    // Add 1 ms margin for clock granularity (Windows ~15.6 ms ticks).
    return duration_cast<milliseconds>(expires_at - now) + milliseconds{1};
}

std::size_t SlidingWindowRateLimiter::current_count() const {
    std::lock_guard lock(mu_);
    // Const-cast is safe: prune_ only removes stale entries.
    auto& self = const_cast<SlidingWindowRateLimiter&>(*this);
    self.prune_(Clock::now());
    return timestamps_.size();
}

void SlidingWindowRateLimiter::reset() {
    std::lock_guard lock(mu_);
    timestamps_.clear();
}

void SlidingWindowRateLimiter::prune_(TimePoint now) {
    const auto cutoff = now - window_;
    while (!timestamps_.empty() && timestamps_.front() <= cutoff) {
        timestamps_.pop_front();
    }
}

// =======================================================================
// libcurl write callback (appends to a std::string)
// =======================================================================

namespace {

/// libcurl invokes this for every chunk of the response body.
/// ISO/IEC 5055 -- null-pointer check on userdata before dereference,
/// matching the defensive pattern in chia_rpc.cpp's curl_write_cb.
std::size_t curl_write_cb(char*       data,
                          std::size_t /*size -- always 1*/,
                          std::size_t nmemb,
                          void*       userdata) {
    if (!userdata) {
        return 0; // Signal error to libcurl; prevents null dereference.
    }
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(data, nmemb);
    return nmemb;
}

/// Truncate a string for safe log output (e.g. offer payloads).
std::string truncate_for_log(std::string_view s, std::size_t max_len = 40) {
    if (s.size() <= max_len) {
        return std::string(s);
    }
    std::string result(s.substr(0, max_len));
    result += "...<truncated, len=";
    result += std::to_string(s.size());
    result += '>';
    return result;
}

/// Safely extract a string from JSON, returning fallback when the key
/// is missing OR the value is JSON null (nlohmann::json::value() throws
/// on null values rather than returning the default).
std::string json_string_or(const nlohmann::json& j,
                           const char*           key,
                           const std::string&    fallback = {}) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return fallback;
    }
    return it->get<std::string>();
}

} // anonymous namespace

// =======================================================================
// DexieClient -- construction / destruction / session lifecycle
// =======================================================================

DexieClient::DexieClient(asio::io_context& ioc, DexieConfig config)
    : ioc_(ioc),
      cfg_(std::move(config)),
      limiter_(cfg_.rate_limit_max_requests, cfg_.rate_limit_window) {

    // Obtain or create a named logger for this component.
    log_ = spdlog::get("dexie");
    if (!log_) {
        log_ = spdlog::stdout_color_mt("dexie");
    }
}

DexieClient::~DexieClient() {
    // Ensure handles are released even if the caller forgets close().
    close();
}

void DexieClient::open() {
    if (open_) {
        return; // Idempotent.
    }

    // curl_global_init() is called once at process startup in main.cpp.
    // It must NOT be called here -- it is not thread-safe and would race
    // with Chia RPC's curl usage.
    // ISO/IEC 5055: single initialization point for global resources.

    // NOTE (MEDIUM-4 cleanup): The CURLM multi-handle that was previously
    // allocated here was dead code -- per-request easy handles were never
    // attached to it via curl_multi_add_handle(), so it served no purpose.
    // Removed to eliminate misleading resource allocation and simplify the
    // session lifecycle.  Per-request easy handles (RAII via CurlEasyPtr)
    // manage their own connections independently.

    // Create the thread pool for offloading blocking CURL transfers.
    const auto pool_size = std::max(1u, cfg_.curl_thread_pool_size);
    thread_pool_ = std::make_unique<asio::thread_pool>(pool_size);

    open_ = true;
    log_->info("DexieClient opened (base_url={}, pool_size={})",
               cfg_.base_url, pool_size);
}

void DexieClient::close() {
    if (!open_) {
        return;
    }

    // Join all thread-pool workers before releasing shared state.
    if (thread_pool_) {
        thread_pool_->join();
        thread_pool_.reset();
    }

    // multi_ removed (MEDIUM-4): no CURLM handle to release.
    open_ = false;
    log_->info("DexieClient closed");
}

bool DexieClient::is_open() const noexcept {
    return open_;
}

const DexieConfig& DexieClient::config() const noexcept {
    return cfg_;
}

std::size_t DexieClient::rate_limiter_count() const {
    return limiter_.current_count();
}

// =======================================================================
// Internal HTTP helpers
// =======================================================================

std::string DexieClient::build_query_(
    const std::vector<std::pair<std::string, std::string>>& params) {

    if (params.empty()) {
        return {};
    }

    // Use a temporary curl handle for URL-encoding.
    CurlEasyPtr encoder(curl_easy_init());

    std::ostringstream qs;
    bool first = true;
    for (const auto& [key, value] : params) {
        if (value.empty()) {
            continue; // Skip unset optional parameters.
        }
        qs << (first ? '?' : '&');
        first = false;

        // URL-encode both key and value to guard against injection.
        // ISO/IEC 5055 -- curl_easy_escape can return nullptr on
        // allocation failure; skip the parameter rather than deref null.
        char* ek = curl_easy_escape(encoder.get(), key.c_str(),
                                    static_cast<int>(key.size()));
        char* ev = curl_easy_escape(encoder.get(), value.c_str(),
                                    static_cast<int>(value.size()));
        if (!ek || !ev) {
            // Allocation failure: free whichever succeeded and skip
            // this parameter to avoid undefined behaviour.
            curl_free(ek); // curl_free is safe on nullptr.
            curl_free(ev);
            continue;
        }
        qs << ek << '=' << ev;
        curl_free(ek);
        curl_free(ev);
    }
    return qs.str();
}

CURLcode DexieClient::perform_request_(
    std::string_view   method,
    const std::string& url,
    const std::string& body,
    std::string&       response_body,
    long&              http_status) {

    // --- Create a fresh per-request CURL handle (RAII) -------------------
    // Each request gets its own handle, eliminating thread-safety concerns
    // when dispatched to the thread pool.
    //
    // ISO/IEC 5055 -- RAII via CurlEasyPtr; no manual cleanup paths.
    CurlEasyPtr easy(curl_easy_init());
    if (!easy) {
        log_->error("curl_easy_init returned null in perform_request_");
        return CURLE_FAILED_INIT;
    }

    response_body.clear();
    response_body.reserve(4096);
    http_status = 0;

    curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &response_body);

    // Timeouts.
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS,
                     static_cast<long>(cfg_.request_timeout.count()));
    curl_easy_setopt(easy.get(), CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(cfg_.connect_timeout.count()));

    // Connection keep-alive.
    curl_easy_setopt(easy.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_TCP_KEEPIDLE,  60L);
    curl_easy_setopt(easy.get(), CURLOPT_TCP_KEEPINTVL, 30L);

    // Follow redirects (up to 5 hops).
    curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_MAXREDIRS, 5L);

    // Accept compressed responses for bandwidth savings.
    curl_easy_setopt(easy.get(), CURLOPT_ACCEPT_ENCODING, "");

    // TLS verification (public API -- system CA bundle is fine).
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    // User-Agent header.
    curl_easy_setopt(easy.get(), CURLOPT_USERAGENT,
                     cfg_.user_agent.c_str());

    // Build headers list (RAII).
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers,
                                    "Accept: application/json");

    // POST-specific configuration.
    if (method == "POST") {
        raw_headers = curl_slist_append(raw_headers,
                                        "Content-Type: application/json");
        curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size()));
    }

    CurlSlistPtr headers_guard(raw_headers);
    curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, raw_headers);

    // --- Execute the blocking transfer -----------------------------------
    const CURLcode rc = curl_easy_perform(easy.get());
    if (rc == CURLE_OK) {
        curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &http_status);
    }

    // CurlEasyPtr and CurlSlistPtr clean up automatically here.
    return rc;
}

asio::awaitable<nlohmann::json> DexieClient::execute_request_(
    std::string_view   method,
    const std::string& url,
    const std::string& body) {

    if (!open_) {
        throw DexieError("DexieClient is not open; call open() first");
    }

    std::size_t attempt = 0;
    auto retry_delay    = cfg_.retry_base_delay;

    for (;;) {
        // 1. Acquire a rate-limiter slot via non-blocking try_acquire().
        //    If the window is full, sleep asynchronously via steady_timer
        //    instead of blocking the io_context event loop thread.
        for (;;) {
            const auto wait_dur = limiter_.try_acquire();
            if (wait_dur.count() == 0) {
                break; // Slot acquired.
            }
            log_->debug("Rate limiter: async wait {}ms before {} {}",
                        wait_dur.count(), method, url);
            asio::steady_timer timer(ioc_, wait_dur);
            co_await timer.async_wait(asio::use_awaitable);
        }

        // 2. Dispatch the blocking CURL transfer to the thread pool.
        //    The inner coroutine runs on a pool thread; the outer
        //    coroutine suspends without blocking the io_context.
        std::string response_body;
        long        http_status = 0;

        // Capture method as a string to avoid dangling string_view in
        // the lambda when it runs on the thread pool.
        const std::string method_str(method);

        const CURLcode rc = co_await asio::co_spawn(
            thread_pool_->get_executor(),
            [this, &method_str, &url, &body, &response_body, &http_status]()
                -> asio::awaitable<CURLcode>
            {
                co_return perform_request_(method_str, url, body,
                                           response_body, http_status);
            },
            asio::use_awaitable);

        if (rc != CURLE_OK) {
            log_->error("{} {} -- curl error: {}", method, url,
                        curl_easy_strerror(rc));
            throw DexieError(std::string("curl_easy_perform: ") +
                             curl_easy_strerror(rc));
        }

        log_->debug("{} {} -> HTTP {} ({} bytes)", method, url,
                    http_status, response_body.size());

        // 3. Classify the response.

        // Success range (2xx).
        if (http_status >= 200 && http_status < 300) {
            try {
                co_return nlohmann::json::parse(response_body);
            } catch (const nlohmann::json::parse_error& ex) {
                log_->error("JSON parse error on {} {}: {}", method, url,
                            ex.what());
                throw DexieError(std::string("JSON parse error: ") +
                                 ex.what());
            }
        }

        // Retryable: 429 Too Many Requests or 5xx Server Error.
        if (http_status == 429 ||
            (http_status >= 500 && http_status < 600)) {

            ++attempt;
            if (attempt > cfg_.max_retries) {
                if (http_status == 429) {
                    log_->error("Rate limit exhausted after {} retries on "
                                "{} {}", cfg_.max_retries, method, url);
                    throw DexieRateLimitError();
                }
                log_->error("Server error {} persisted after {} retries "
                            "on {} {}", http_status, cfg_.max_retries,
                            method, url);
                throw DexieServerError(static_cast<int>(http_status),
                                       response_body);
            }

            log_->warn("{} {} -> HTTP {} (attempt {}/{}) -- retrying "
                       "in {}ms", method, url, http_status, attempt,
                       cfg_.max_retries, retry_delay.count());

            // Async exponential backoff via steady_timer (non-blocking).
            asio::steady_timer timer(ioc_, retry_delay);
            co_await timer.async_wait(asio::use_awaitable);
            retry_delay *= 2;
            continue;
        }

        // Non-retryable client error (4xx except 429).
        log_->error("{} {} -> HTTP {} (non-retryable)", method, url,
                    http_status);
        throw DexieClientError(static_cast<int>(http_status),
                               response_body);
    }
}

asio::awaitable<nlohmann::json> DexieClient::http_get_(
    const std::string& path) {
    const std::string url = cfg_.base_url + path;
    co_return co_await execute_request_("GET", url, /*body=*/"");
}

asio::awaitable<nlohmann::json> DexieClient::http_post_(
    const std::string& path,
    const nlohmann::json& body) {
    const std::string url      = cfg_.base_url + path;
    const std::string body_str = body.dump();
    co_return co_await execute_request_("POST", url, body_str);
}

// =======================================================================
// JSON -> struct parsing helpers
// =======================================================================

AssetInfo DexieClient::parse_asset_(const nlohmann::json& j) {
    AssetInfo a;
    a.id     = j.value("id", "");
    a.code   = j.value("code", "");
    a.name   = j.value("name", "");
    a.amount = j.value("amount", 0.0);
    return a;
}

OfferRecord DexieClient::parse_offer_(const nlohmann::json& j) {
    OfferRecord o;
    o.id             = json_string_or(j, "id");
    o.status         = j.value("status", 0);
    o.offer_bech32   = json_string_or(j, "offer");
    o.date_found     = json_string_or(j, "date_found");
    o.date_completed = json_string_or(j, "date_completed");
    o.date_pending   = json_string_or(j, "date_pending");
    o.date_expiry    = json_string_or(j, "date_expiry");
    o.price          = j.value("price", 0.0);
    o.fees           = j.value("fees", static_cast<uint64_t>(0));
    o.mod_version    = j.value("mod_version", 0);
    o.trade_id       = json_string_or(j, "trade_id");

    // Nullable integer fields.
    if (j.contains("block_expiry") && !j["block_expiry"].is_null()) {
        o.block_expiry = j["block_expiry"].get<uint64_t>();
    }
    if (j.contains("spent_block_index") && !j["spent_block_index"].is_null()) {
        o.spent_block_index = j["spent_block_index"].get<uint64_t>();
    }

    // Involved coins array (list of hex-encoded coin IDs).
    if (j.contains("involved_coins") && j["involved_coins"].is_array()) {
        for (const auto& c : j["involved_coins"]) {
            o.involved_coins.push_back(c.get<std::string>());
        }
    }

    // Offered / requested asset arrays.
    if (j.contains("offered") && j["offered"].is_array()) {
        for (const auto& a : j["offered"]) {
            o.offered.push_back(parse_asset_(a));
        }
    }
    if (j.contains("requested") && j["requested"].is_array()) {
        for (const auto& a : j["requested"]) {
            o.requested.push_back(parse_asset_(a));
        }
    }

    return o;
}

TickerData DexieClient::parse_ticker_(const nlohmann::json& j,
                                      std::string_view base_asset) {
    TickerData t;
    t.id          = j.value("id", "");
    t.code        = j.value("code", "");
    t.name        = j.value("name", "");
    t.pair_id     = j.value("pair_id", "");
    t.incentives  = j.value("incentives", false);

    // Volume -- keyed by asset id.
    if (j.contains("volume") && j["volume"].is_object()) {
        const auto& vol = j["volume"];
        // XCH-denominated daily volume.
        const std::string base_key(base_asset);
        if (vol.contains(base_key) && vol[base_key].is_object()) {
            t.volume_xch_daily = vol[base_key].value("daily", 0.0);
        }
        // Quote-token-denominated daily volume.
        if (vol.contains(t.id) && vol[t.id].is_object()) {
            t.volume_quote_daily = vol[t.id].value("daily", 0.0);
        }
    }

    // Prices.
    if (j.contains("prices") && j["prices"].is_object()) {
        const auto& px = j["prices"];

        // Best bid (buy depth 0).
        if (px.contains("buy") && px["buy"].is_array()) {
            for (const auto& lvl : px["buy"]) {
                if (lvl.value("depth", -1) == 0) {
                    t.price_buy = lvl.value("price", 0.0);
                    break;
                }
            }
        }

        // Best ask (sell depth 0).
        if (px.contains("sell") && px["sell"].is_array()) {
            for (const auto& lvl : px["sell"]) {
                if (lvl.value("depth", -1) == 0) {
                    t.price_sell = lvl.value("price", 0.0);
                    break;
                }
            }
        }

        // Last trade.
        if (px.contains("last") && px["last"].is_object()) {
            t.price_last = px["last"].value("price", 0.0);
        }

        // 24-hour high / low.
        if (px.contains("high") && px["high"].is_object()) {
            t.price_high = px["high"].value("daily", 0.0);
        }
        if (px.contains("low") && px["low"].is_object()) {
            t.price_low = px["low"].value("daily", 0.0);
        }
    }

    return t;
}

// =======================================================================
// Public API methods -- now coroutines using co_await
// =======================================================================

// -----------------------------------------------------------------------
// GET /v1/pairs
// -----------------------------------------------------------------------
asio::awaitable<std::vector<PairInfo>> DexieClient::get_pairs() {
    log_->info("get_pairs()");

    const auto json = co_await http_get_("/pairs");
    std::vector<PairInfo> result;

    if (!json.contains("pairs") || !json["pairs"].is_array()) {
        log_->warn("get_pairs: unexpected response shape");
        co_return result;
    }

    result.reserve(json["pairs"].size());
    for (const auto& p : json["pairs"]) {
        PairInfo pi;
        if (p.contains("base") && p["base"].is_object()) {
            pi.base.id   = p["base"].value("id", "");
            pi.base.code = p["base"].value("code", "");
            pi.base.name = p["base"].value("name", "");
        }
        if (p.contains("quote") && p["quote"].is_object()) {
            pi.quote.id   = p["quote"].value("id", "");
            pi.quote.code = p["quote"].value("code", "");
            pi.quote.name = p["quote"].value("name", "");
        }
        result.push_back(std::move(pi));
    }

    log_->info("get_pairs -> {} pairs", result.size());
    co_return result;
}

// -----------------------------------------------------------------------
// GET /v1/offers
// -----------------------------------------------------------------------
asio::awaitable<OffersPage> DexieClient::get_offers(
    std::string_view   pair_id,
    std::string_view   offered,
    std::string_view   requested,
    uint32_t           page,
    uint32_t           page_size,
    std::string_view   sort,
    bool               compact,
    std::optional<int> status) {

    log_->info("get_offers(pair_id={}, offered={}, page={}, page_size={}, "
               "compact={})",
               pair_id, offered, page, page_size, compact);

    std::vector<std::pair<std::string, std::string>> params;
    if (!pair_id.empty())   params.emplace_back("pair_id",   std::string(pair_id));
    if (!offered.empty())   params.emplace_back("offered",   std::string(offered));
    if (!requested.empty()) params.emplace_back("requested", std::string(requested));
    params.emplace_back("page",      std::to_string(page));
    params.emplace_back("page_size", std::to_string(page_size));
    if (!sort.empty())      params.emplace_back("sort",      std::string(sort));
    if (compact)            params.emplace_back("compact",   "true");
    if (status.has_value()) params.emplace_back("status",    std::to_string(*status));

    const std::string path = "/offers" + build_query_(params);
    const auto json = co_await http_get_(path);

    OffersPage result;
    result.success   = json.value("success", false);
    result.count     = json.value("count", static_cast<uint64_t>(0));
    result.page      = json.value("page", static_cast<uint64_t>(0));
    result.page_size = json.value("page_size", static_cast<uint64_t>(0));

    if (json.contains("offers") && json["offers"].is_array()) {
        result.offers.reserve(json["offers"].size());
        for (const auto& o : json["offers"]) {
            result.offers.push_back(parse_offer_(o));
        }
    }

    log_->info("get_offers -> {} offers (total={})",
               result.offers.size(), result.count);
    co_return result;
}

// -----------------------------------------------------------------------
// GET /v1/markets  (tickers)
// -----------------------------------------------------------------------
asio::awaitable<std::vector<TickerData>> DexieClient::get_tickers() {
    log_->info("get_tickers()");

    const auto json = co_await http_get_("/markets");
    std::vector<TickerData> result;

    if (!json.contains("markets") || !json["markets"].is_object()) {
        log_->warn("get_tickers: unexpected response shape");
        co_return result;
    }

    for (const auto& [base_asset, market_array] : json["markets"].items()) {
        if (!market_array.is_array()) {
            continue;
        }
        for (const auto& m : market_array) {
            result.push_back(parse_ticker_(m, base_asset));
        }
    }

    log_->info("get_tickers -> {} tickers", result.size());
    co_return result;
}

// -----------------------------------------------------------------------
// GET /v1/markets  (single pair ticker)
// -----------------------------------------------------------------------
asio::awaitable<std::optional<TickerData>> DexieClient::get_ticker(
    std::string_view pair_id) {

    log_->info("get_ticker(pair_id={})", pair_id);

    // The dexie API does not offer a single-pair ticker endpoint.
    // We fetch the full markets response and filter client-side.
    const auto json = co_await http_get_("/markets");

    if (!json.contains("markets") || !json["markets"].is_object()) {
        log_->warn("get_ticker: unexpected response shape");
        co_return std::nullopt;
    }

    for (const auto& [base_asset, market_array] : json["markets"].items()) {
        if (!market_array.is_array()) {
            continue;
        }
        for (const auto& m : market_array) {
            if (m.value("pair_id", "") == pair_id) {
                auto td = parse_ticker_(m, base_asset);
                log_->info("get_ticker -> found {} ({})", td.code, td.pair_id);
                co_return td;
            }
        }
    }

    log_->info("get_ticker -> pair_id={} not found", pair_id);
    co_return std::nullopt;
}

// -----------------------------------------------------------------------
// GET /v1/markets  (raw prices / depth data)
// -----------------------------------------------------------------------
asio::awaitable<nlohmann::json> DexieClient::get_prices() {
    log_->info("get_prices()");
    co_return co_await http_get_("/markets");
}

// -----------------------------------------------------------------------
// GET /v1/offers?status=4  (recent trades)
// -----------------------------------------------------------------------
asio::awaitable<OffersPage> DexieClient::get_trades(
    std::string_view pair_id,
    uint32_t         page,
    uint32_t         page_size) {

    log_->info("get_trades(pair_id={}, page={}, page_size={})",
               pair_id, page, page_size);

    // Settled offers have status=4.  Sort newest first.
    co_return co_await get_offers(
        pair_id,
        /*offered=*/   {},
        /*requested=*/ {},
        page,
        page_size,
        /*sort=*/      "date_completed_desc",
        /*compact=*/   true,
        /*status=*/    4);
}

// -----------------------------------------------------------------------
// POST /v1/offers  (submit a new offer)
// -----------------------------------------------------------------------
asio::awaitable<SubmitResult> DexieClient::submit_offer(
    std::string_view offer_bech32m) {
    // Security: never log the full offer payload.
    log_->info("submit_offer(offer={})",
               truncate_for_log(offer_bech32m, 24));

    nlohmann::json body;
    body["offer"] = offer_bech32m;

    const auto json = co_await http_post_("/offers", body);

    SubmitResult sr;
    sr.success       = json.value("success", false);
    sr.offer_id      = json_string_or(json, "id");
    sr.error_message = json_string_or(json, "error_message");

    if (sr.success) {
        log_->info("submit_offer -> id={}", sr.offer_id);
    } else {
        log_->warn("submit_offer -> failed: {}", sr.error_message);
    }
    co_return sr;
}

// -----------------------------------------------------------------------
// GET /v1/offers/{offer_id}  (offer status lookup)
// -----------------------------------------------------------------------
asio::awaitable<OfferStatus> DexieClient::get_offer_status(
    std::string_view offer_id) {
    log_->info("get_offer_status(id={})", offer_id);

    const std::string path = "/offers/" + std::string(offer_id);
    const auto json = co_await http_get_(path);

    OfferStatus os;
    os.success = json.value("success", false);

    if (json.contains("offer") && json["offer"].is_object()) {
        os.offer = parse_offer_(json["offer"]);

        // Additional settlement details (present on completed offers).
        if (json["offer"].contains("input_coins")) {
            os.input_coins = json["offer"]["input_coins"];
        }
        if (json["offer"].contains("output_coins")) {
            os.output_coins = json["offer"]["output_coins"];
        }
    }

    log_->info("get_offer_status -> success={}, status={}",
               os.success, os.offer.status);
    co_return os;
}

} // namespace xop::rpc
