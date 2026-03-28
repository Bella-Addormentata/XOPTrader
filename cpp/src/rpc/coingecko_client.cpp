// coingecko_client.cpp -- Implementation of the CoinGecko REST client.
//
// Transport   : libcurl easy handles (per-request, RAII) dispatched to a
//               dedicated thread pool so the io_context is never blocked.
// Rate control: true sliding-window limiter with non-blocking async waits
//               via boost::asio::steady_timer.
// Retry policy: exponential back-off on HTTP 429 / 5xx via async timer;
//               immediate throw on 4xx client errors.
//
// ISO/IEC 27001:2022 -- api_key is never logged in full.
// ISO/IEC 5055       -- no raw owning pointers; RAII throughout; per-request
//                       CURL handles eliminate shared-state data races.

#include "xop/rpc/coingecko_client.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace xop::rpc {

namespace asio = boost::asio;

// =======================================================================
// libcurl write callback (appends to a std::string)
// =======================================================================

namespace {

/// libcurl invokes this for every chunk of the response body.
std::size_t coingecko_write_cb(char*       data,
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

} // anonymous namespace

// =======================================================================
// CoinGeckoClient -- construction / destruction / session lifecycle
// =======================================================================

CoinGeckoClient::CoinGeckoClient(asio::io_context&      ioc,
                                 const CoinGeckoConfig& config)
    : ioc_(ioc),
      cfg_(config),
      limiter_(cfg_.rate_limit_max_requests,
               std::chrono::milliseconds{cfg_.rate_limit_window_ms}) {

    log_ = spdlog::get("coingecko");
    if (!log_) {
        log_ = spdlog::stdout_color_mt("coingecko");
    }
}

CoinGeckoClient::~CoinGeckoClient() {
    close();
}

void CoinGeckoClient::open() {
    if (open_) {
        return;
    }

    const auto pool_size = std::max(1u, cfg_.curl_thread_pool_size);
    thread_pool_ = std::make_unique<asio::thread_pool>(pool_size);

    open_ = true;
    log_->info("CoinGeckoClient opened (base_url={}, coin_ids={}, pool_size={})",
               cfg_.base_url, cfg_.coin_ids.size(), pool_size);
}

void CoinGeckoClient::close() {
    if (!open_) {
        return;
    }

    if (thread_pool_) {
        thread_pool_->join();
        thread_pool_.reset();
    }

    open_ = false;
    log_->info("CoinGeckoClient closed");
}

bool CoinGeckoClient::is_open() const noexcept {
    return open_;
}

const CoinGeckoConfig& CoinGeckoClient::config() const noexcept {
    return cfg_;
}

std::size_t CoinGeckoClient::rate_limiter_count() const {
    return limiter_.current_count();
}

// =======================================================================
// Internal HTTP: blocking CURL transfer
// =======================================================================

CURLcode CoinGeckoClient::perform_request_(
    const std::string& url,
    std::string&       response_body,
    long&              http_status) {

    CurlEasyPtr easy(curl_easy_init());
    if (!easy) {
        log_->error("curl_easy_init returned null in perform_request_");
        return CURLE_FAILED_INIT;
    }

    response_body.clear();
    response_body.reserve(2048);
    http_status = 0;

    curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, coingecko_write_cb);
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

    // Accept compressed responses.
    curl_easy_setopt(easy.get(), CURLOPT_ACCEPT_ENCODING, "");

    // TLS verification (public API -- system CA bundle).
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    // User-Agent header.
    curl_easy_setopt(easy.get(), CURLOPT_USERAGENT,
                     cfg_.user_agent.c_str());

    // Build headers (RAII).
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers,
                                    "Accept: application/json");

    // CoinGecko Demo plan: pass API key via header.
    // Free tier: no header needed.
    if (!cfg_.api_key.empty()) {
        std::string key_header = "x-cg-demo-api-key: " + cfg_.api_key;
        raw_headers = curl_slist_append(raw_headers, key_header.c_str());
    }

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

asio::awaitable<nlohmann::json> CoinGeckoClient::execute_request_(
    const std::string& url) {

    if (!open_) {
        throw CoinGeckoError("CoinGeckoClient is not open; call open() first");
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
            throw CoinGeckoError(std::string("curl_easy_perform: ") +
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
                throw CoinGeckoError(std::string("JSON parse error: ") +
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
                    throw CoinGeckoRateLimitError();
                }
                log_->error("Server error {} persisted after {} retries on "
                            "GET {}", http_status, cfg_.max_retries, url);
                throw CoinGeckoServerError(static_cast<int>(http_status),
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
        throw CoinGeckoClientError(static_cast<int>(http_status),
                                   response_body);
    }
}

// =======================================================================
// Public API: fetch_prices
// =======================================================================

asio::awaitable<std::map<std::string, double>>
CoinGeckoClient::fetch_prices() {

    if (cfg_.coin_ids.empty()) {
        co_return std::map<std::string, double>{};
    }

    // Build the comma-separated ids parameter.
    std::ostringstream ids_stream;
    for (std::size_t i = 0; i < cfg_.coin_ids.size(); ++i) {
        if (i > 0) ids_stream << ',';
        ids_stream << cfg_.coin_ids[i];
    }

    // Build the full URL.  Use a temporary CURL handle for URL-encoding.
    // GET /simple/price?ids=chia,ethereum,usd-coin&vs_currencies=usd
    CurlEasyPtr encoder(curl_easy_init());
    std::string url = cfg_.base_url + "/simple/price?ids=";

    // URL-encode the ids parameter value.
    const std::string ids_raw = ids_stream.str();
    char* encoded_ids = curl_easy_escape(encoder.get(), ids_raw.c_str(),
                                         static_cast<int>(ids_raw.size()));
    if (encoded_ids) {
        url += encoded_ids;
        curl_free(encoded_ids);
    } else {
        url += ids_raw;  // Fallback: use raw (safe for simple coin IDs).
    }
    url += "&vs_currencies=usd";

    // Execute the request.
    nlohmann::json result = co_await execute_request_(url);

    // Parse the response.
    // Expected format: { "chia": { "usd": 2.71 }, "ethereum": { "usd": 2450.0 }, ... }
    std::map<std::string, double> prices;

    for (const auto& coin_id : cfg_.coin_ids) {
        auto coin_it = result.find(coin_id);
        if (coin_it == result.end() || !coin_it->is_object()) {
            log_->warn("CoinGecko response missing coin: {}", coin_id);
            continue;
        }
        auto usd_it = coin_it->find("usd");
        if (usd_it == coin_it->end() || !usd_it->is_number()) {
            log_->warn("CoinGecko response missing usd price for: {}",
                       coin_id);
            continue;
        }
        prices[coin_id] = usd_it->get<double>();
    }

    log_->info("CoinGecko prices fetched: {} of {} coins",
               prices.size(), cfg_.coin_ids.size());
    for (const auto& [id, price] : prices) {
        log_->debug("  {} = ${:.4f}", id, price);
    }

    co_return prices;
}

} // namespace xop::rpc
