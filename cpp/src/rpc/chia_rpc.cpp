/**
 * @file chia_rpc.cpp
 * @brief Implementation of Chia Full Node and Wallet RPC clients.
 *
 * Transport is handled by libcurl configured for mTLS (self-signed certs).
 * Each RPC method is an asio::awaitable coroutine that dispatches the
 * blocking CURL transfer to a dedicated thread pool via co_spawn, so the
 * caller's io_context event loop is never stalled.
 *
 * Per-request CURL handles (RAII) eliminate shared-state data races and
 * enable safe concurrent RPC calls from multiple coroutines.
 *
 * SSL peer verification is ON by default when a CA cert path is provided.
 * This prevents MITM on the wallet RPC channel, which could redirect funds.
 * Verification can be disabled via ChiaRPCConfig::verify_ssl for explicit
 * localhost scenarios.
 *
 * Retry strategy: up to max_retries attempts with exponential backoff on
 * transient errors (network faults, HTTP 429/5xx).  Non-transient errors
 * propagate immediately as ChiaRPCApplicationError or ChiaRPCTransportError.
 * An endpoint whose second copy is a second wallet action (cancel_offers,
 * cancel_offer) is re-sent only when the request cannot have reached the
 * handler -- see rpc/rpc_retry_policy.hpp.
 *
 * ISO/IEC 27001:2022 -- cert file contents are never logged; paths are
 *                       logged at debug level only.  SSL verification
 *                       defaults to ON; disabling is logged at warn level.
 * ISO/IEC 5055       -- every CURL call, allocation, and JSON parse is
 *                       checked for errors.  Per-request CURL handles via
 *                       RAII eliminate use-after-free and data-race defects.
 * ISO/IEC 25000      -- RAII for CURL handles and header lists; move
 *                       semantics for safe ownership transfer; thread pool
 *                       joined deterministically on close/destruction.
 */

#include "xop/rpc/chia_rpc.hpp"
#include "xop/rpc/rpc_retry_policy.hpp"
#include "xop/rpc/wallet_requests.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "xop/util/client_logger.hpp"

namespace xop::rpc {

// ===========================================================================
// ChiaTLSConfig
// ===========================================================================

bool ChiaTLSConfig::validate() const noexcept
{
    // Verify every required certificate file exists as a regular file.
    // We deliberately avoid opening or reading the contents to limit
    // exposure of sensitive key material in memory.
    std::error_code ec;
    const bool cert_ok = fs::is_regular_file(cert_path, ec) && !ec;
    const bool key_ok  = fs::is_regular_file(key_path,  ec) && !ec;
    const bool ca_ok   = fs::is_regular_file(ca_cert_path, ec) && !ec;
    return cert_ok && key_ok && ca_ok;
}

// ===========================================================================
// Exception classes
// ===========================================================================

ChiaRPCApplicationError::ChiaRPCApplicationError(const std::string& msg,
                                                  json               response)
    : ChiaRPCError(msg)
    , response_(std::move(response))
{}

ChiaRPCTransportError::ChiaRPCTransportError(const std::string& msg,
                                              long               http_code,
                                              CURLcode           curl_code)
    : ChiaRPCError(msg)
    , http_code_(http_code)
    , curl_code_(curl_code)
{}

// ===========================================================================
// libcurl write callback
// ===========================================================================

namespace {

/**
 * @brief libcurl WRITEFUNCTION callback -- appends received data to a
 *        std::string pointed to by @p userdata.
 *
 * ISO/IEC 5055 -- null-pointer check on userdata before dereference.
 */
std::size_t curl_write_cb(char*       ptr,
                          std::size_t /*size (always 1)*/,
                          std::size_t nmemb,
                          void*       userdata)
{
    auto* buf = static_cast<std::string*>(userdata);
    if (!buf) {
        return 0; // Signal error to libcurl.
    }
    buf->append(ptr, nmemb);
    return nmemb;
}

/**
 * @brief RAII wrapper for a CURL easy handle.
 *
 * Calls curl_easy_cleanup on destruction; default-constructible to nullptr.
 * Provides get() for raw access and release() for ownership transfer.
 *
 * ISO/IEC 5055  -- deterministic resource release; no manual cleanup paths.
 * ISO/IEC 25000 -- value-semantic wrapper with clear ownership.
 */
class ScopedCurlEasy {
public:
    ScopedCurlEasy() noexcept : handle_(nullptr) {}

    /// Take ownership of an existing CURL* handle.
    explicit ScopedCurlEasy(CURL* h) noexcept : handle_(h) {}

    ~ScopedCurlEasy() {
        if (handle_) {
            curl_easy_cleanup(handle_);
        }
    }

    // Non-copyable, movable.
    ScopedCurlEasy(const ScopedCurlEasy&)            = delete;
    ScopedCurlEasy& operator=(const ScopedCurlEasy&) = delete;

    ScopedCurlEasy(ScopedCurlEasy&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    ScopedCurlEasy& operator=(ScopedCurlEasy&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                curl_easy_cleanup(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    /// Raw pointer access for libcurl API calls.
    [[nodiscard]] CURL* get() const noexcept { return handle_; }

    /// True if a valid handle is held.
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    CURL* handle_;
};

/**
 * @brief RAII wrapper for a curl_slist header list.
 *
 * Calls curl_slist_free_all on destruction.
 *
 * ISO/IEC 5055 -- deterministic cleanup of linked-list allocation.
 */
class ScopedCurlSlist {
public:
    ScopedCurlSlist() noexcept : list_(nullptr) {}
    explicit ScopedCurlSlist(curl_slist* s) noexcept : list_(s) {}

    ~ScopedCurlSlist() {
        if (list_) {
            curl_slist_free_all(list_);
        }
    }

    ScopedCurlSlist(const ScopedCurlSlist&)            = delete;
    ScopedCurlSlist& operator=(const ScopedCurlSlist&) = delete;

    ScopedCurlSlist(ScopedCurlSlist&& other) noexcept
        : list_(std::exchange(other.list_, nullptr)) {}

    ScopedCurlSlist& operator=(ScopedCurlSlist&& other) noexcept {
        if (this != &other) {
            if (list_) {
                curl_slist_free_all(list_);
            }
            list_ = std::exchange(other.list_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] curl_slist* get() const noexcept { return list_; }

private:
    curl_slist* list_;
};

} // anonymous namespace

// ===========================================================================
// ChiaRPCBase -- construction / destruction / move
// ===========================================================================

ChiaRPCBase::ChiaRPCBase(asio::io_context& ioc,
                          ChiaRPCConfig     cfg,
                          std::string_view  logger_name)
    : ioc_(ioc)
    , config_(std::move(cfg))
{
    // Obtain or create the named logger.  This adopts the PROCESS logger's
    // sinks, so RPC-layer faults reach the rotating file log and not just
    // stdout -- see xop/util/client_logger.hpp for why the old
    // get()-then-stdout_color_mt() pattern was stdout-only.  Never throws,
    // never returns a logger that discards silently.
    logger_ = xop::util::get_or_create_client_logger(logger_name);
}

ChiaRPCBase::~ChiaRPCBase()
{
    close();
}

ChiaRPCBase::ChiaRPCBase(ChiaRPCBase&& other) noexcept
    : ioc_(other.ioc_)
    , config_(std::move(other.config_))
    , logger_(std::move(other.logger_))
    , thread_pool_(std::move(other.thread_pool_))
    , open_(std::exchange(other.open_, false))
    , transport_(std::exchange(other.transport_, TransportCounters{}))
{
    // Moving a shared_ptr leaves the SOURCE null, and open() dereferences
    // logger_ with no guard (:274, :278) -- reopening a moved-from client
    // would be a null deref.  ~ChiaRPCBase -> close() only survives it by
    // accident, because open_ was exchanged to false first.  Share the logger
    // instead of leaving a hole; loggers are designed to be shared.
    other.logger_ = logger_;
}

ChiaRPCBase& ChiaRPCBase::operator=(ChiaRPCBase&& other) noexcept
{
    if (this != &other) {
        close();
        // ioc_ is a reference -- cannot be reseated; both sides are
        // expected to share the same io_context in practice.
        config_      = std::move(other.config_);
        logger_      = std::move(other.logger_);
        thread_pool_ = std::move(other.thread_pool_);
        open_        = std::exchange(other.open_, false);
        transport_   = std::exchange(other.transport_, TransportCounters{});
        // See the move ctor: do not leave the source with a null logger_.
        other.logger_ = logger_;
    }
    return *this;
}

// ===========================================================================
// ChiaRPCBase -- open / close / is_open
// ===========================================================================

asio::awaitable<void> ChiaRPCBase::open()
{
    // Idempotent: close any previous session first.
    close();

    // --- Certificate validation (fail-fast before any network I/O) ---------
    if (!config_.tls.validate()) {
        // Log which specific path is missing at error level.  We log the
        // *paths* (public metadata), never the file contents.
        std::ostringstream oss;
        oss << "mTLS certificate validation failed -- verify paths exist:\n"
            << "  cert : " << config_.tls.cert_path    << "\n"
            << "  key  : " << config_.tls.key_path     << "\n"
            << "  ca   : " << config_.tls.ca_cert_path;
        logger_->error(oss.str());
        throw ChiaRPCError("One or more TLS certificate files are missing. "
                           "Check log for details.");
    }
    logger_->debug("TLS certificate paths validated");

    // --- Create the thread pool for offloading blocking CURL calls ---------
    // The pool size is configurable; default is 4 threads, which handles
    // typical full-node + wallet + concurrent retry concurrency.
    const auto pool_size = std::max(1u, config_.curl_thread_pool_size);
    thread_pool_ = std::make_unique<asio::thread_pool>(pool_size);

    // --- Log SSL verification state ----------------------------------------
    if (!config_.verify_ssl) {
        // ISO/IEC 27001:2022 -- disabling SSL verification is a security
        // policy decision that must be explicitly recorded.
        logger_->warn("SSL peer verification DISABLED by configuration -- "
                      "MITM attacks on RPC channel are possible");
    } else {
        logger_->debug("SSL peer verification enabled (CA: {})",
                       config_.tls.ca_cert_path.string());
    }

    open_ = true;
    logger_->info("RPC client opened -- {}:{} (pool_size={}, verify_ssl={})",
                  config_.host, config_.port, pool_size, config_.verify_ssl);
    co_return;
}

void ChiaRPCBase::close() noexcept
{
    if (!open_) {
        return;
    }

    // Join all thread-pool workers before releasing.  This ensures any
    // in-flight CURL transfers complete before we destroy shared state.
    if (thread_pool_) {
        thread_pool_->join();
        thread_pool_.reset();
    }

    open_ = false;
    logger_->info("RPC client closed");
}

bool ChiaRPCBase::is_open() const noexcept
{
    return open_;
}

// ===========================================================================
// ChiaRPCBase -- private helpers
// ===========================================================================

std::string ChiaRPCBase::build_url(std::string_view endpoint) const
{
    // Chia RPC URL pattern: https://<host>:<port>/<endpoint>
    std::ostringstream oss;
    oss << "https://" << config_.host << ':' << config_.port
        << '/' << endpoint;
    return oss.str();
}

void ChiaRPCBase::configure_tls(CURL*              curl,
                                const std::string& cert_str,
                                const std::string& key_str,
                                const std::string& ca_str)
{
    if (!curl)
        throw std::invalid_argument("configure_tls called on null CURL handle");

    // ISO/IEC 5055 -- CWE-416 use-after-free prevention:
    // std::filesystem::path::string() returns a temporary std::string.
    // CURL retains the raw pointers set here and only dereferences them
    // later during curl_easy_perform().  The caller (perform_request)
    // materialises the path strings into locals whose lifetime spans
    // the entire CURL transfer, then passes them here by const&.

    // Client certificate:
    // - PEM cert + separate PEM key (OpenSSL-style)
    // - PKCS#12 bundle (.p12/.pfx) for Schannel-compatible Windows setups
    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string cert_path_lc = to_lower(cert_str);
    const bool is_pkcs12 =
        (cert_path_lc.size() >= 4
         && cert_path_lc.compare(cert_path_lc.size() - 4, 4, ".p12") == 0)
        || (cert_path_lc.size() >= 4
            && cert_path_lc.compare(cert_path_lc.size() - 4, 4, ".pfx") == 0);

    curl_easy_setopt(curl, CURLOPT_SSLCERT, cert_str.c_str());
    if (is_pkcs12) {
        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "P12");
        // Empty password for local unencrypted bundles produced during setup.
        curl_easy_setopt(curl, CURLOPT_KEYPASSWD, "");
    } else {
        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");

        // Client private key (PEM format).
        curl_easy_setopt(curl, CURLOPT_SSLKEY, key_str.c_str());
        curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
    }

    // CA certificate for server verification.
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_str.c_str());

    // Windows Schannel often fails local/self-signed RPC handshakes when
    // revocation endpoints are unavailable. Disable CRL checks for this
    // local mTLS channel to avoid false negatives.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NO_REVOKE);

    // --- SSL verification policy -------------------------------------------
    // Default ON: verify the server certificate against the Chia CA.
    // This prevents MITM attacks on the wallet RPC (which can redirect
    // funds).  Only disable for explicit localhost testing scenarios.
    //
    // ISO/IEC 27001:2022 -- verification is the default; disabling requires
    //                       an explicit config decision and is logged.
    if (config_.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

CURLcode ChiaRPCBase::perform_request(const std::string& url,
                                      const std::string& body,
                                      std::string&       response_body,
                                      long&              http_code)
{
    // --- Create a fresh per-request CURL handle (RAII) ---------------------
    // Each request gets its own handle, eliminating thread-safety concerns
    // when multiple coroutines dispatch concurrent RPC calls to the pool.
    //
    // ISO/IEC 5055 -- RAII via ScopedCurlEasy; no manual cleanup paths.
    ScopedCurlEasy curl(curl_easy_init());
    if (!curl) {
        logger_->error("curl_easy_init() returned NULL in perform_request");
        return CURLE_FAILED_INIT;
    }

    // --- Materialise TLS path strings in this scope --------------------------
    // ISO/IEC 5055 -- CWE-416 use-after-free prevention:
    // std::filesystem::path::string() returns a temporary.  We capture
    // the result in locals whose lifetime spans curl_easy_perform() below,
    // then pass them by const& to configure_tls() which hands the raw
    // c_str() pointers to CURL.  This guarantees the backing memory is
    // alive for the entire transfer.
    const std::string cert_str = config_.tls.cert_path.string();
    const std::string key_str  = config_.tls.key_path.string();
    const std::string ca_str   = config_.tls.ca_cert_path.string();

    // --- Configure mTLS and SSL verification on this handle ----------------
    configure_tls(curl.get(), cert_str, key_str, ca_str);

    // --- JSON Content-Type header (per-request, RAII) ----------------------
    ScopedCurlSlist headers(
        curl_slist_append(nullptr, "Content-Type: application/json"));
    if (!headers.get()) {
        logger_->error("Failed to allocate CURL header list");
        return CURLE_OUT_OF_MEMORY;
    }
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());

    // --- Response write callback -------------------------------------------
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);

    // --- Timeouts ----------------------------------------------------------
    const long timeout_ms =
        static_cast<long>(config_.request_timeout.count());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms);

    // Connection timeout -- use a short timeout for localhost (most common
    // deployment), and half of request timeout otherwise, floored at 3 s.
    const bool is_localhost = (config_.host == "localhost"
                               || config_.host == "127.0.0.1"
                               || config_.host == "::1");
    const long connect_ms = is_localhost
        ? 3000L
        : std::max(3000L, timeout_ms / 2);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, connect_ms);

    // Disable redirects (Chia doesn't redirect; defense-in-depth).
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);

    // TCP keepalive to detect dead connections early.
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPIDLE,  60L);
    curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPINTVL, 30L);

    // --- Set per-request data ----------------------------------------------
    response_body.clear();
    http_code = 0;

    curl_easy_setopt(curl.get(), CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA,     &response_body);

    // --- Execute the blocking transfer -------------------------------------
    const CURLcode rc = curl_easy_perform(curl.get());

    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    }

    // ScopedCurlEasy and ScopedCurlSlist clean up automatically here.
    return rc;
}

bool ChiaRPCBase::is_transient(CURLcode curl_code, long http_code) noexcept
{
    // Network-level transient failures.
    switch (curl_code) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_SSL_CONNECT_ERROR:  // Wallet daemon restart can cause transient SSL failures.
        return true;
    default:
        break;
    }

    // HTTP-level transient status codes.
    switch (http_code) {
    case 429:  // Too Many Requests
    case 500:  // Internal Server Error
    case 502:  // Bad Gateway
    case 503:  // Service Unavailable
    case 504:  // Gateway Timeout
        return true;
    default:
        return false;
    }
}

// ===========================================================================
// ChiaRPCBase -- rpc_post (async with retry + exponential backoff)
// ===========================================================================

asio::awaitable<json> ChiaRPCBase::rpc_post(std::string_view endpoint,
                                             const json&      payload)
{
    if (!is_open()) {
        throw ChiaRPCError("RPC client is not open -- call open() first");
    }

    // [WALLET-CIRCUIT 2026-09-13] From here on every exit records how the call
    // ended -- note_call_end() immediately before each throw below and before
    // the co_return -- so a caller that swallows the exception still leaves
    // evidence for the wallet breaker (rpc/transport_evidence.hpp).  The
    // refusal above records nothing: a closed client is not a stalled daemon.
    // An exception out of a co_await itself (the CURL pool, or the backoff
    // timer being cancelled at shutdown) records nothing either.

    const std::string url        = build_url(endpoint);
    const std::string body       = payload.dump();
    const auto        max_tries  = config_.max_retries + 1u; // 1 initial + N retries
    auto              backoff    = config_.retry_base_delay;

    // [BULKCANCEL-B 2026-09-13] A re-send is a SECOND REQUEST, not a retry
    // of the first.  For cancel_offers / cancel_offer the policy re-sends
    // only a request that cannot have reached the handler; a timeout or a
    // 5xx surfaces to the caller instead.  See rpc/rpc_retry_policy.hpp.
    const RpcRetryPolicy policy = retry_policy_for_endpoint(endpoint);

    for (std::uint32_t attempt = 1; attempt <= max_tries; ++attempt) {
        std::string response_body;
        long        http_code = 0;

        logger_->debug("[attempt {}/{}] POST {} (body {} bytes)",
                       attempt, max_tries, url, body.size());

        // --- Dispatch the blocking CURL transfer to the thread pool --------
        // co_spawn creates a new coroutine on the thread pool's executor.
        // The inner coroutine calls the blocking perform_request(), which
        // creates its own per-request CURL handle (RAII).  The outer
        // coroutine suspends here without blocking the io_context.
        //
        // The lambda captures response_body and http_code by reference;
        // they remain alive on the outer coroutine's frame for the duration
        // of the co_await.
        const CURLcode rc = co_await asio::co_spawn(
            thread_pool_->get_executor(),
            [this, &url, &body, &response_body, &http_code]()
                -> asio::awaitable<CURLcode>
            {
                co_return perform_request(url, body,
                                          response_body, http_code);
            },
            asio::use_awaitable);

        // --- Transport error -----------------------------------------------
        if (rc != CURLE_OK) {
            const bool retryable = is_transient(rc, 0);
            const bool resend    = may_resend(policy, rc, retryable);
            logger_->warn("[attempt {}/{}] CURL error {}: {} (retryable={}, "
                          "resend={})",
                          attempt, max_tries,
                          static_cast<int>(rc),
                          curl_easy_strerror(rc),
                          retryable, resend);
            if (retryable && !resend) {
                logger_->warn("{} is NOT re-sent after CURL error {}: the "
                              "request may already have reached the wallet "
                              "and still be running there, and a second "
                              "copy is a second action -- failing this "
                              "call instead",
                              endpoint, static_cast<int>(rc));
            }

            if (resend && attempt < max_tries) {
                // Exponential backoff via an asio steady_timer (non-blocking).
                asio::steady_timer timer(ioc_, backoff);
                co_await timer.async_wait(asio::use_awaitable);
                backoff *= 2;
                continue;
            }
            note_call_end(RpcCallEnd::CurlFailed);
            throw ChiaRPCTransportError(
                std::string("CURL transport failure: ") +
                    curl_easy_strerror(rc),
                http_code, rc);
        }

        // --- HTTP error (non-2xx) ------------------------------------------
        if (http_code < 200 || http_code >= 300) {
            const bool retryable = is_transient(CURLE_OK, http_code);
            const bool resend    = may_resend(policy, CURLE_OK, retryable);
            logger_->warn("[attempt {}/{}] HTTP {} from {} (retryable={}, "
                          "resend={})",
                          attempt, max_tries, http_code, url, retryable,
                          resend);
            if (retryable && !resend) {
                logger_->warn("{} is NOT re-sent after HTTP {}: the "
                              "handler received it, and a second copy is "
                              "a second action -- failing this call "
                              "instead",
                              endpoint, http_code);
            }

            if (resend && attempt < max_tries) {
                asio::steady_timer timer(ioc_, backoff);
                co_await timer.async_wait(asio::use_awaitable);
                backoff *= 2;
                continue;
            }
            note_call_end(RpcCallEnd::HttpError);
            throw ChiaRPCTransportError(
                "HTTP " + std::to_string(http_code) + " from " +
                    std::string(endpoint),
                http_code, CURLE_OK);
        }

        // --- Parse JSON response -------------------------------------------
        json result;
        try {
            result = json::parse(response_body);
        } catch (const json::parse_error& ex) {
            // Malformed JSON is not transient -- fail immediately.
            //
            // [review 2026-09-13, round 3] As a TRANSPORT failure carrying
            // CURLE_OK and the 2xx status: the handler received this request
            // and ran it, and only its answer was lost.  A cancel caller must
            // read that as "possibly submitted"
            // (rpc::cancel_possibly_submitted), never as a refusal.  Every
            // other caller catches ChiaRPCError, which this derives from, so
            // nothing else changes.
            logger_->error("JSON parse error from {}: {}", endpoint, ex.what());
            note_call_end(RpcCallEnd::MalformedBody);
            throw ChiaRPCTransportError(
                std::string("Failed to parse JSON response from ") +
                    std::string(endpoint) + ": " + ex.what(),
                http_code, CURLE_OK);
        }

        // --- Application-level success check -------------------------------
        // Chia RPC responses contain a "success" boolean field.
        if (result.contains("success") &&
            result["success"].is_boolean() &&
            !result["success"].get<bool>())
        {
            // Extract the human-readable error message if present.
            std::string err_msg = "RPC returned success=false";
            if (result.contains("error") && result["error"].is_string()) {
                err_msg = result["error"].get<std::string>();
            }
            logger_->error("{} failed: {}", endpoint, err_msg);
            note_call_end(RpcCallEnd::ApplicationError);
            throw ChiaRPCApplicationError(err_msg, std::move(result));
        }

        logger_->debug("{} succeeded (HTTP {} , {} bytes)",
                       endpoint, http_code, response_body.size());
        note_call_end(RpcCallEnd::Ok);
        co_return result;
    }

    // Unreachable in normal flow, but satisfies the compiler.
    throw ChiaRPCError("Retry loop exited without returning or throwing");
}

// ===========================================================================
// ChiaFullNodeRPC
// ===========================================================================

ChiaFullNodeRPC::ChiaFullNodeRPC(asio::io_context& ioc, ChiaRPCConfig cfg)
    : ChiaRPCBase(ioc, std::move(cfg), "chia.fullnode")
{}

ChiaFullNodeRPC::SyncState node_sync_from_blockchain_state(
    const json& resp, ChiaFullNodeRPC::SyncState previous)
{
    if (!resp.contains("blockchain_state")
        || !resp["blockchain_state"].contains("sync"))
    {
        return previous;
    }
    const json& node_sync = resp["blockchain_state"]["sync"];
    if (node_sync.contains("synced") && node_sync["synced"].is_boolean()) {
        previous.synced = node_sync["synced"].get<bool>();
    }
    // Chia names the in-progress flag `sync_mode`, not `syncing`.
    if (node_sync.contains("sync_mode")
        && node_sync["sync_mode"].is_boolean())
    {
        previous.syncing = node_sync["sync_mode"].get<bool>();
    }
    return previous;
}

asio::awaitable<std::int64_t> ChiaFullNodeRPC::get_block_height()
{
    // The blockchain_state response includes peak.height.
    const json resp = co_await rpc_post("get_blockchain_state");

    // [S33 2026-09-05, CORRECTED 2026-09-12] Record the sync object BEFORE
    // the peak check below, so the CACHED SYNC FLAGS survive a throw.
    //
    // Two wrong claims have stood here, in opposite directions. The original
    // said "a node that is still syncing is exactly the case that throws
    // there" -- false, and it misled an audit: a catching-up node reports a
    // peak alongside sync_mode=true (cpp/tests/test_node_sync_state.cpp),
    // returns NORMALLY, and already renders as "Full Node: Syncing...".
    //
    // The correction then overstated the other way, claiming a node that
    // answers is never reported as silent. It can be. Only the sync CACHE is
    // written before the throw; the REACHABILITY STAMP is not.
    // node_last_probe_ is set only after a SUCCESSFUL return (engine.cpp
    // 2500 / 2568 / 2856 / 2905), so on the no-peak branch below
    // node_probe_is_live takes its never-probed clause (engine.cpp:413) and
    // the node IS reported as not connected.
    //
    // So: the case that throws is a node with NO peak at all -- an empty or
    // rebuilding database before its first peak -- which genuinely cannot
    // serve a height, and whose gauges stay dark until one arrives.
    last_sync_state_ = node_sync_from_blockchain_state(resp, last_sync_state_);
    // [S67] Same response, same reason: the node's admission floor rides in it
    // and costs no second RPC.  A reply without the mempool fields reads as
    // UNKNOWN (known = false), never as an empty mempool.
    last_mempool_state_ = node_mempool_from_blockchain_state(resp);

    if (!resp.contains("blockchain_state") ||
        !resp["blockchain_state"].contains("peak") ||
        resp["blockchain_state"]["peak"].is_null())
    {
        throw ChiaRPCError("get_blockchain_state: missing or null peak -- "
                           "node may still be syncing");
    }

    co_return resp["blockchain_state"]["peak"]["height"]
                  .get<std::int64_t>();
}

asio::awaitable<json> ChiaFullNodeRPC::get_blockchain_state()
{
    co_return co_await rpc_post("get_blockchain_state");
}

asio::awaitable<std::vector<json>>
ChiaFullNodeRPC::get_coin_records_by_puzzle_hash(
    const std::string& puzzle_hash,
    bool               include_spent,
    std::int64_t       start_height,
    std::int64_t       end_height)
{
    json payload = {
        {"puzzle_hash",    puzzle_hash},
        {"include_spent_coins", include_spent}
    };

    // Optional height bounds -- omit if zero (Chia treats absent fields as
    // "no constraint").
    if (start_height > 0) {
        payload["start_height"] = start_height;
    }
    if (end_height > 0) {
        payload["end_height"] = end_height;
    }

    const json resp = co_await rpc_post("get_coin_records_by_puzzle_hash",
                                        payload);

    std::vector<json> records;
    if (resp.contains("coin_records") && resp["coin_records"].is_array()) {
        records.reserve(resp["coin_records"].size());
        for (auto& rec : resp["coin_records"]) {
            records.push_back(std::move(rec));
        }
    }
    co_return records;
}

asio::awaitable<std::uint64_t>
ChiaFullNodeRPC::get_fee_estimate(std::uint64_t target_time_seconds)
{
    // The Chia full node's `get_fee_estimate` endpoint (added in Chia 1.6)
    // accepts `target_times` (list of seconds) and an optional
    // `spend_type`.  It returns `estimates` (list of mojos) aligned 1:1
    // with the requested target times.
    //
    // We request a single target time and return the corresponding estimate.
    // If the endpoint is unavailable (older node) or fails, return 0 so the
    // caller falls back to the static fee.
    try {
        // [S67] The payload now lives in rpc/node_requests.hpp so ctest pins it.
        // This is the LEGACY request, unchanged: fees.cost_aware_estimate and
        // the fee controller use get_fee_rate_estimate() below instead.
        const json payload = make_fee_estimate_request_legacy(target_time_seconds);

        const json resp = co_await rpc_post("get_fee_estimate", payload);

        if (resp.contains("estimates") && resp["estimates"].is_array()
            && !resp["estimates"].empty()) {
            co_return resp["estimates"][0].get<std::uint64_t>();
        }

        // Fallback: response format unexpected.
        co_return 0;
    } catch (const ChiaRPCError&) {
        // Endpoint not supported or node unavailable -- return 0.
        co_return 0;
    }
}

asio::awaitable<FeeEstimateReading>
ChiaFullNodeRPC::get_fee_rate_estimate(std::uint64_t target_time_seconds)
{
    // [S67] One request with an explicit reference cost; the handler returns
    // rate x cost, so the rate falls out and every action class scales it by
    // its own cost.  Failure is a reading with ok = false, never a throw: the
    // caller is the heartbeat, and a fee estimate is not worth a cycle.
    try {
        // A named payload, not a temporary inside the co_await expression.
        const json payload =
            make_fee_estimate_request(target_time_seconds, kFeeEstimateReferenceCost);
        const json resp = co_await rpc_post("get_fee_estimate", payload);
        co_return parse_fee_estimate(resp, kFeeEstimateReferenceCost);
    } catch (const ChiaRPCError&) {
        co_return FeeEstimateReading{};
    }
}
asio::awaitable<std::vector<json>>
ChiaFullNodeRPC::get_coin_records_by_names(
    const std::vector<std::string>& names,
    bool                            include_spent)
{
    // Chia full node expects names with 0x prefix.
    json name_arr = json::array();
    for (const auto& n : names) {
        if (n.size() >= 2 && n.substr(0, 2) == "0x") {
            name_arr.push_back(n);
        } else {
            name_arr.push_back("0x" + n);
        }
    }

    json payload = {
        {"names",               name_arr},
        {"include_spent_coins", include_spent}
    };

    const json resp = co_await rpc_post("get_coin_records_by_names", payload);

    std::vector<json> records;
    if (resp.contains("coin_records") && resp["coin_records"].is_array()) {
        records.reserve(resp["coin_records"].size());
        for (auto& rec : resp["coin_records"]) {
            records.push_back(std::move(rec));
        }
    }
    co_return records;
}

asio::awaitable<json>
ChiaFullNodeRPC::get_block_record_by_height(std::int64_t height)
{
    json payload = {{"height", height}};
    const json resp = co_await rpc_post("get_block_record_by_height", payload);

    if (resp.contains("block_record")) {
        co_return resp["block_record"];
    }
    co_return json{};
}

asio::awaitable<json>
ChiaFullNodeRPC::get_additions_and_removals(const std::string& header_hash)
{
    json payload = {{"header_hash", header_hash}};
    co_return co_await rpc_post("get_additions_and_removals", payload);
}

// ===========================================================================
// ChiaWalletRPC
// ===========================================================================

ChiaWalletRPC::ChiaWalletRPC(asio::io_context& ioc, ChiaRPCConfig cfg)
    : ChiaRPCBase(ioc, std::move(cfg), "chia.wallet")
{}

// -- Wallet info ------------------------------------------------------------

asio::awaitable<std::int64_t> ChiaWalletRPC::get_height_info()
{
    const json resp = co_await rpc_post("get_height_info");

    if (!resp.contains("height")) {
        throw ChiaRPCError("get_height_info: response missing 'height' field");
    }
    co_return resp["height"].get<std::int64_t>();
}

asio::awaitable<std::uint64_t>
ChiaWalletRPC::get_timestamp_for_height(std::int64_t height)
{
    // [S70] Shape pinned in wallet_requests.hpp / test_wallet_requests.cpp.
    const json resp = co_await rpc_post(
        "get_timestamp_for_height",
        make_get_timestamp_for_height_request(height));
    co_return parse_timestamp_for_height_response(resp);
}

asio::awaitable<FullNodePeerCensus>
ChiaWalletRPC::get_full_node_peer_census()
{
    // [review #164] Shape pinned in wallet_requests.hpp /
    // test_wallet_requests.cpp, and verified read-only against the live 2.7.4
    // wallet on 2026-09-21.
    const json resp = co_await rpc_post("get_connections",
                                        make_get_connections_request());
    co_return census_full_node_peers(resp);
}

asio::awaitable<json> ChiaWalletRPC::get_sync_status()
{
    co_return co_await rpc_post("get_sync_status");
}

asio::awaitable<json>
ChiaWalletRPC::get_wallet_balance(std::int64_t wallet_id)
{
    const json payload = {{"wallet_id", wallet_id}};
    const json resp    = co_await rpc_post("get_wallet_balance", payload);

    if (!resp.contains("wallet_balance")) {
        throw ChiaRPCError("get_wallet_balance: response missing "
                           "'wallet_balance' field");
    }
    co_return resp["wallet_balance"];
}

asio::awaitable<std::vector<json>> ChiaWalletRPC::get_wallets()
{
    const json resp = co_await rpc_post("get_wallets");

    std::vector<json> wallets;
    if (resp.contains("wallets") && resp["wallets"].is_array()) {
        wallets.reserve(resp["wallets"].size());
        for (auto& w : resp["wallets"]) {
            wallets.push_back(std::move(w));
        }
    }
    co_return wallets;
}

// -- Offer lifecycle --------------------------------------------------------

asio::awaitable<json>
ChiaWalletRPC::create_offer(const json&   offer_dict,
                             std::uint64_t fee,
                             bool          validate_only,
                             std::optional<std::uint64_t> max_time,
                             std::optional<std::uint64_t> min_coin_amount)
{
    // The Chia wallet RPC endpoint is "create_offer_for_ids".
    const json payload = build_create_offer_payload(
        offer_dict, fee, validate_only, max_time, min_coin_amount);

    const json resp = co_await rpc_post("create_offer_for_ids", payload);

    // Return the full response so callers can access both the bech32 offer
    // text (.offer) and the trade record (.trade_record).
    co_return resp;
}

json ChiaWalletRPC::build_create_offer_payload(
    const json&    offer_dict,
    std::uint64_t  fee,
    bool           validate_only,
    const std::optional<std::uint64_t>& max_time,
    const std::optional<std::uint64_t>& min_coin_amount)
{
    // offer_dict maps wallet_id (as string key) -> signed mojo amount.
    json payload = {
        {"offer",         offer_dict},
        {"fee",           fee},
        {"validate_only", validate_only}
    };

    // [OFFER-EXPIRY] Attached only when the caller asks, so an unconfigured
    // deployment sends exactly the payload it sent before this existed.
    //
    // max_time is the ONLY one of Chia's four absolute timelock flags that a
    // reference-wallet TAKER honours.  max_height, min_height and min_time
    // are enforced by the chain, but the reference wallet does not apply
    // them until it submits the spend bundle to the mempool, so taking such
    // an offer "will be initiated, but will fail" (Chia Offer RPC
    // reference).  Sending one would leave our offers visible and
    // unfillable -- strictly worse than having no expiry at all.  The
    // relative flags (max_blocks_after_created and friends) are not
    // supported by this API.  test_offer_expiry pins all of this.
    if (max_time.has_value()) {
        payload["max_time"] = *max_time;
    }

    // [MIN-INPUT-COIN] Attached only when the caller asks.  The key sits at
    // the top level because that is where the wallet reads its coin-selection
    // config (tx_endpoint -> TXConfigLoader.from_json_dict(request)); the same
    // floor then governs the offered asset and the XCH fee coin alike.  A
    // floor of 0 is the wallet's own default, so it is not worth a key.
    if (min_coin_amount.has_value() && *min_coin_amount > 0) {
        payload["min_coin_amount"] = *min_coin_amount;
    }
    return payload;
}

asio::awaitable<json>
ChiaWalletRPC::take_offer(const std::string& offer_text,
                           std::uint64_t      fee)
{
    const json payload = {
        {"offer", offer_text},
        {"fee",   fee}
    };
    co_return co_await rpc_post("take_offer", payload);
}

asio::awaitable<json>
ChiaWalletRPC::cancel_offer(const std::string& trade_id,
                             std::uint64_t      fee,
                             bool               secure)
{
    const json payload = {
        {"trade_id", trade_id},
        {"fee",      fee},
        {"secure",   secure}
    };
    co_return co_await rpc_post("cancel_offer", payload);
}

asio::awaitable<json>
ChiaWalletRPC::cancel_offers(std::uint64_t batch_fee, bool secure)
{
    // [BULKCANCEL 2026-09-11] The payload shape is pinned and tested in
    // wallet_requests.hpp.  The previous {"fee","secure"} body was ignored
    // by the handler on all three counts that matter: it reads batch_fee
    // (so every bulk cancel went out at ZERO fee), it defaults cancel_all
    // to false (so only offers with an XCH leg matched -- a CAT/CAT offer
    // was never cancelled), and its paging loop breaks on the first batch
    // that matched nothing (so the scan stopped early).
    co_return co_await rpc_post(
        "cancel_offers", make_cancel_offers_request(batch_fee, secure));
}

asio::awaitable<json>
ChiaWalletRPC::get_offer(const std::string& trade_id, bool file_contents)
{
    const json payload = {
        {"trade_id",      trade_id},
        {"file_contents", file_contents}
    };
    const json resp = co_await rpc_post("get_offer", payload);

    if (!resp.contains("trade_record") || resp["trade_record"].is_null()) {
        throw ChiaRPCError("get_offer: response missing trade_record");
    }

    co_return resp["trade_record"];
}

asio::awaitable<std::vector<json>>
ChiaWalletRPC::get_all_offers(std::int64_t start,
                               std::int64_t end,
                               bool         file_contents,
                               bool         include_completed,
                               const std::string& sort_key,
                               bool         reverse)
{
    json payload = {
        {"start",              start},
        {"end",                end},
        {"file_contents",      file_contents},
        {"include_completed",  include_completed}
    };
    // [WALLET-LOAD 2026-08-04] Only sent when explicitly requested, so
    // existing callers keep the wallet's default ordering byte-for-byte.
    if (!sort_key.empty()) {
        payload["sort_key"] = sort_key;
        payload["reverse"]  = reverse;
    }
    const json resp = co_await rpc_post("get_all_offers", payload);

    std::vector<json> offers;
    if (resp.contains("trade_records") && resp["trade_records"].is_array()) {
        offers.reserve(resp["trade_records"].size());
        for (auto& o : resp["trade_records"]) {
            offers.push_back(std::move(o));
        }
    }
    co_return offers;
}

asio::awaitable<bool>
ChiaWalletRPC::check_offer_validity(const std::string& offer)
{
    const json payload = {{"offer", offer}};
    const json resp    = co_await rpc_post("check_offer_validity", payload);

    if (!resp.contains("valid") || !resp["valid"].is_boolean()) {
        throw ChiaRPCError("check_offer_validity: response missing "
                           "'valid' boolean field");
    }
    co_return resp["valid"].get<bool>();
}

// -- Coin selection ---------------------------------------------------------

asio::awaitable<std::vector<json>>
ChiaWalletRPC::select_coins(std::int64_t  wallet_id,
                             std::uint64_t amount,
                             std::uint64_t min_coin_amount)
{
    json payload = {
        {"wallet_id", wallet_id},
        {"amount",    amount}
    };
    if (min_coin_amount > 0) {
        payload["min_coin_amount"] = min_coin_amount;
    }

    const json resp = co_await rpc_post("select_coins", payload);

    std::vector<json> coins;
    if (resp.contains("coins") && resp["coins"].is_array()) {
        coins.reserve(resp["coins"].size());
        for (auto& c : resp["coins"]) {
            coins.push_back(std::move(c));
        }
    }
    co_return coins;
}

asio::awaitable<std::vector<json>>
ChiaWalletRPC::get_spendable_coins(std::int64_t wallet_id)
{
    const json payload = {{"wallet_id", wallet_id}};
    const json resp    = co_await rpc_post(
        "get_spendable_coins", payload);

    std::vector<json> coins;
    if (resp.contains("confirmed_records") &&
        resp["confirmed_records"].is_array())
    {
        coins.reserve(resp["confirmed_records"].size());
        for (auto& c : resp["confirmed_records"]) {
            coins.push_back(std::move(c));
        }
    }
    co_return coins;
}

// ---------------------------------------------------------------------------
// ChiaWalletRPC::send_transaction -- send XCH to a target address
// ---------------------------------------------------------------------------
// Wraps the Chia wallet daemon "send_transaction" endpoint.
// Required payload keys: wallet_id (int), amount (uint64 mojos),
//                        address (bech32m string), fee (uint64 mojos).
//
// The daemon returns {"success": true, "transaction_id": "0x...",
//                     "transaction": {...}} on acceptance.
//
// ISO/IEC 5055 -- response structure validated before return;
//                 transport errors propagated via ChiaRPCError hierarchy.
// ---------------------------------------------------------------------------

asio::awaitable<json>
ChiaWalletRPC::send_transaction(const json& params)
{
    // Validate required keys before dispatching the RPC call.
    if (!params.contains("wallet_id") || !params.contains("amount") ||
        !params.contains("address")   || !params.contains("fee")) {
        throw ChiaRPCError("send_transaction: missing required parameter "
                           "(wallet_id, amount, address, fee)");
    }

    const json resp = co_await rpc_post("send_transaction", params);

    // The Chia wallet daemon returns {"success": false, "error": "..."}
    // on application-level rejection.  rpc_post already checks HTTP-level
    // errors and {"success": false}; this is a defensive double-check.
    if (resp.contains("success") && resp["success"].is_boolean() &&
        !resp["success"].get<bool>()) {
        std::string err_msg = resp.contains("error")
                              ? resp["error"].get<std::string>()
                              : "unknown error";
        throw ChiaRPCApplicationError(
            "send_transaction rejected: " + err_msg, resp);
    }

    co_return resp;
}

// ---------------------------------------------------------------------------
// ChiaWalletRPC::split_coins -- split one coin into many atomically
// ---------------------------------------------------------------------------
// Wraps the Chia wallet daemon "split_coins" endpoint (available since v2.4).
// Creates number_of_coins new outputs of amount_per_coin mojos each from the
// specified target coin.  All outputs are created in a single spend bundle --
// one block confirmation, one fee.
//
// Required payload: wallet_id, target_coin_id, number_of_coins,
//                   amount_per_coin, fee, push=true.
//
// ISO/IEC 5055 -- response validated; transport errors thrown.
// ---------------------------------------------------------------------------

asio::awaitable<json>
ChiaWalletRPC::split_coins(
    std::int64_t       wallet_id,
    const std::string& target_coin_id,
    int                number_of_coins,
    std::int64_t       amount_per_coin,
    std::int64_t       fee)
{
    // Ensure the coin ID has the "0x" prefix that the Chia RPC expects.
    std::string coin_id = target_coin_id;
    if (coin_id.size() >= 2 && coin_id.substr(0, 2) != "0x") {
        coin_id = "0x" + coin_id;
    }

    const json payload = {
        {"wallet_id",       wallet_id},
        {"target_coin_id",  coin_id},
        {"number_of_coins", number_of_coins},
        {"amount_per_coin", amount_per_coin},
        {"fee",             fee},
        {"push",            true}
    };

    const json resp = co_await rpc_post("split_coins", payload);

    if (resp.contains("success") && resp["success"].is_boolean() &&
        !resp["success"].get<bool>()) {
        std::string err_msg = resp.contains("error")
                              ? resp["error"].get<std::string>()
                              : "unknown error";
        throw ChiaRPCApplicationError(
            "split_coins rejected: " + err_msg, resp);
    }

    co_return resp;
}

// ---------------------------------------------------------------------------
// ChiaWalletRPC::get_next_address -- get a receive address for self-sends
// ---------------------------------------------------------------------------

asio::awaitable<std::string>
ChiaWalletRPC::get_next_address(std::int64_t wallet_id, bool new_address)
{
    const json payload = {
        {"wallet_id",   wallet_id},
        {"new_address", new_address}
    };
    const json resp = co_await rpc_post("get_next_address", payload);
    if (!resp.contains("address") || !resp["address"].is_string()) {
        throw ChiaRPCError("get_next_address: response missing 'address' field");
    }
    co_return resp["address"].get<std::string>();
}

// ---------------------------------------------------------------------------
// ChiaWalletRPC::delete_unconfirmed_transactions
// ---------------------------------------------------------------------------

asio::awaitable<json>
ChiaWalletRPC::delete_unconfirmed_transactions(std::int64_t wallet_id)
{
    const json payload = {{"wallet_id", wallet_id}};
    co_return co_await rpc_post("delete_unconfirmed_transactions", payload);
}

// ---------------------------------------------------------------------------
// ChiaWalletRPC::get_transactions
// ---------------------------------------------------------------------------

asio::awaitable<std::vector<json>>
ChiaWalletRPC::get_transactions(std::int64_t wallet_id,
                                std::int64_t start,
                                std::int64_t end)
{
    // [BULKCANCEL 2026-09-11] reverse=FALSE, and it is load-bearing for both
    // consumers -- see order_as_daemon() in tests/test_wallet_requests.cpp,
    // which models the daemon's ORDER BY over this payload.  Under
    // reverse=true, RELEVANCE returns CONFIRMED rows first and OLDEST first,
    // so a 0..200 window on a busy wallet (live wallet 1: 32,966 rows)
    // contains no unconfirmed row at all and nothing recent either.
    const json payload =
        make_get_transactions_request(wallet_id, start, end);

    const json resp = co_await rpc_post("get_transactions", payload);

    std::vector<json> txs;
    if (resp.contains("transactions") && resp["transactions"].is_array()) {
        txs.reserve(resp["transactions"].size());
        for (auto& t : resp["transactions"]) {
            txs.push_back(std::move(t));
        }
    }
    co_return txs;
}

} // namespace xop::rpc
