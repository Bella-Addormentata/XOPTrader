// ---------------------------------------------------------------------------
// [BULKCANCEL-B 2026-09-13] rpc_post's re-send decision, per endpoint.
//
// rpc_post re-sent every endpoint on every transient failure.  On 2026-09-12
// a wallet-wide cancel_offers timed out after 30 s, was re-sent while the
// first request was still running inside the wallet, and only a lucky
// "Wallet needs to be fully synced" refusal kept the copy from running the
// whole sweep a second time.  See rpc/rpc_retry_policy.hpp for the full
// account and the chia 2.7.4 handler behaviour it rests on.
//
// These tests call the production decision functions directly.  That
// rpc_post actually CONSULTS them on both of its re-send paths is pinned
// separately by tests/test_rpc_retry_wiring.py, because rpc_post needs a live
// mTLS endpoint and ChiaWalletRPC is final.
// ---------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <curl/curl.h>

#include "xop/rpc/rpc_retry_policy.hpp"

using xop::rpc::cancel_possibly_submitted;
using xop::rpc::may_resend;
using xop::rpc::request_possibly_submitted;
using xop::rpc::retry_policy_for_endpoint;
using xop::rpc::RpcRetryPolicy;

TEST(RpcRetryPolicy, CancelEndpointsNeverResend) {
    // The two endpoints whose second copy is a second cancel.  Spelled
    // exactly as ChiaWalletRPC::cancel_offers / cancel_offer pass them.
    EXPECT_EQ(retry_policy_for_endpoint("cancel_offers"),
              RpcRetryPolicy::NeverResend);
    EXPECT_EQ(retry_policy_for_endpoint("cancel_offer"),
              RpcRetryPolicy::NeverResend);

    // Reads keep the transient retry: a duplicate costs one round trip.
    EXPECT_EQ(retry_policy_for_endpoint("get_offer"),
              RpcRetryPolicy::RetryTransient);
    EXPECT_EQ(retry_policy_for_endpoint("get_all_offers"),
              RpcRetryPolicy::RetryTransient);
    EXPECT_EQ(retry_policy_for_endpoint("get_sync_status"),
              RpcRetryPolicy::RetryTransient);
}

TEST(RpcRetryPolicy, ATimeoutIsNeverResentForACancel) {
    const RpcRetryPolicy cancel = RpcRetryPolicy::NeverResend;

    // The 2026-09-12 shape: CURL error 28 after 30 s, request possibly
    // still executing wallet-side.  is_transient says true; the policy
    // must still say no.
    EXPECT_FALSE(may_resend(cancel, CURLE_OPERATION_TIMEDOUT, /*transient=*/true));

    // Every other transient transport failure can follow a request the
    // wallet already received.
    EXPECT_FALSE(may_resend(cancel, CURLE_RECV_ERROR, true));
    EXPECT_FALSE(may_resend(cancel, CURLE_GOT_NOTHING, true));
    EXPECT_FALSE(may_resend(cancel, CURLE_SEND_ERROR, true));
    EXPECT_FALSE(may_resend(cancel, CURLE_PARTIAL_FILE, true));

    // An HTTP 5xx: rpc_post passes CURLE_OK with the HTTP-level transient
    // flag.  The handler ran; it is not re-sent.
    EXPECT_FALSE(may_resend(cancel, CURLE_OK, true));

    // No connection, or no completed TLS handshake: the request cannot have
    // reached the handler, so re-sending it is safe.
    EXPECT_TRUE(may_resend(cancel, CURLE_COULDNT_CONNECT, true));
    EXPECT_TRUE(may_resend(cancel, CURLE_SSL_CONNECT_ERROR, true));

    // Never wider than the transient classifier itself.
    EXPECT_FALSE(may_resend(cancel, CURLE_COULDNT_CONNECT, false));
}

TEST(RpcRetryPolicy, ReadsStillRetryTransientFailures) {
    const RpcRetryPolicy read = RpcRetryPolicy::RetryTransient;

    EXPECT_TRUE(may_resend(read, CURLE_OPERATION_TIMEDOUT, /*transient=*/true));
    EXPECT_TRUE(may_resend(read, CURLE_RECV_ERROR, true));
    EXPECT_TRUE(may_resend(read, CURLE_OK, true));  // e.g. HTTP 503

    EXPECT_FALSE(may_resend(read, CURLE_OPERATION_TIMEDOUT, false));
    EXPECT_FALSE(may_resend(read, CURLE_OK, false));  // e.g. HTTP 404
}

// [review 2026-09-13, round 2] The failures after which the wallet may have
// run -- or may still be running -- a cancel.  OfferManager::cancel_all sends
// nothing more after one of these until each offer has been re-checked; any
// other failure keeps its immediate per-offer fallback.
//
// [review 2026-09-13, round 3] The default is "possibly submitted": only a
// failure that provably happens before the request is written is not.
//
// MUTATION: list CURLE_OPERATION_TIMEDOUT (the 2026-09-12 shape) among the
// pre-send codes -> FAILS here, and nowhere else.
// MUTATION: default an unlisted code to "not submitted" (round 2's rule)
// -> FAILS here, and nowhere else.
// MUTATION: class a 2xx -- an unparseable reply -- as not submitted
// -> FAILS here, and nowhere else.
TEST(RpcRetryPolicy, APostSendCancelFailureIsPossiblySubmitted) {
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_OPERATION_TIMEDOUT, 0));
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_GOT_NOTHING, 0));
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_SEND_ERROR, 0));
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_RECV_ERROR, 0));
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_PARTIAL_FILE, 0));
    // A code no list names is possibly submitted too: a garbled reply, an
    // out-of-memory mid-transfer.
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_WEIRD_SERVER_REPLY, 0));
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_OUT_OF_MEMORY, 0));
    // rpc_post throws an HTTP failure with CURLE_OK: the handler, or the
    // server in front of it, answered 5xx.
    for (long code : {500L, 502L, 503L, 504L}) {
        EXPECT_TRUE(cancel_possibly_submitted(CURLE_OK, code)) << "HTTP " << code;
    }
    // ...or 2xx, which reaches here only when the body could not be parsed:
    // the wallet ran the request and its answer was lost.
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_OK, 200));
    // A status that is not a refusal the client can read.
    EXPECT_TRUE(cancel_possibly_submitted(CURLE_OK, 302));

    // Never built, never resolved, never connected, or no completed TLS
    // handshake: the request was never written, so the per-offer fallback
    // cannot race it.
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_FAILED_INIT, 0));
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_URL_MALFORMAT, 0));
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_COULDNT_RESOLVE_HOST, 0));
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_COULDNT_CONNECT, 0));
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_SSL_CONNECT_ERROR, 0));
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_PEER_FAILED_VERIFICATION, 0));
    EXPECT_FALSE(cancel_possibly_submitted(CURLE_SSL_CACERT_BADFILE, 0));
    // A 4xx is an answer that the request was refused before any cancel ran.
    for (long code : {400L, 404L, 429L}) {
        EXPECT_FALSE(cancel_possibly_submitted(CURLE_OK, code)) << "HTTP " << code;
    }
}

// [review #165, round 4] THE SAME QUESTION, ASKED BY A FAILED CREATE.
//
// A create that throws clears its PostingMark, so the keep-stop drain sees
// "nothing in flight" and reported clean success -- but a timeout is not a
// refusal, and the wallet may hold an offer this process never recorded. The
// classification never depended on the endpoint, so cancel_possibly_submitted
// became a thin alias of request_possibly_submitted and OfferManager's create
// handlers ask the generic name (offer_manager.cpp
// note_create_outcome_unknown; pinned there by tests/test_stop_keep_wiring.py,
// which is where the wiring lives -- nothing in cpp/tests can construct an
// OfferManager).
//
// MUTATION: make request_possibly_submitted return false for a timeout
// -> FAILS here and in APostSendCancelFailureIsPossiblySubmitted (the alias),
//    and nowhere else.
// MUTATION: give cancel_possibly_submitted a body of its own that differs on
//    any code -> FAILS the agreement block below, and nowhere else.
TEST(RpcRetryPolicy, ACreateThatGotNoAnswerIsPossiblySubmittedToo) {
    // The shapes an offer create actually meets on localhost: the 30 s
    // request timeout, a wallet that died mid-reply, a reply that would not
    // parse. Each of these may follow an offer the wallet really built.
    EXPECT_TRUE(request_possibly_submitted(CURLE_OPERATION_TIMEDOUT, 0));
    EXPECT_TRUE(request_possibly_submitted(CURLE_GOT_NOTHING, 0));
    EXPECT_TRUE(request_possibly_submitted(CURLE_RECV_ERROR, 0));
    EXPECT_TRUE(request_possibly_submitted(CURLE_OK, 200));  // unparseable body
    EXPECT_TRUE(request_possibly_submitted(CURLE_OK, 502));

    // ...and the ones that prove no offer was built, so a keep stop can still
    // report a clean book: the request was never written.
    EXPECT_FALSE(request_possibly_submitted(CURLE_COULDNT_CONNECT, 0));
    EXPECT_FALSE(request_possibly_submitted(CURLE_COULDNT_RESOLVE_HOST, 0));
    EXPECT_FALSE(request_possibly_submitted(CURLE_SSL_CONNECT_ERROR, 0));
    EXPECT_FALSE(request_possibly_submitted(CURLE_FAILED_INIT, 0));
    // A 4xx is the daemon answering that it refused the request.
    EXPECT_FALSE(request_possibly_submitted(CURLE_OK, 404));

    // The alias agrees everywhere the callers can reach, including the codes
    // neither list names. One rule, two names -- not two rules that can drift.
    for (CURLcode rc : {CURLE_OK, CURLE_OPERATION_TIMEDOUT, CURLE_GOT_NOTHING,
                        CURLE_SEND_ERROR, CURLE_RECV_ERROR, CURLE_PARTIAL_FILE,
                        CURLE_WEIRD_SERVER_REPLY, CURLE_OUT_OF_MEMORY,
                        CURLE_FAILED_INIT, CURLE_URL_MALFORMAT,
                        CURLE_UNSUPPORTED_PROTOCOL, CURLE_COULDNT_RESOLVE_PROXY,
                        CURLE_COULDNT_RESOLVE_HOST, CURLE_COULDNT_CONNECT,
                        CURLE_SSL_CONNECT_ERROR, CURLE_PEER_FAILED_VERIFICATION,
                        CURLE_SSL_CERTPROBLEM, CURLE_SSL_CIPHER,
                        CURLE_SSL_CACERT_BADFILE}) {
        for (long http : {0L, 200L, 302L, 400L, 404L, 429L, 500L, 503L}) {
            EXPECT_EQ(cancel_possibly_submitted(rc, http),
                      request_possibly_submitted(rc, http))
                << "curl " << static_cast<int>(rc) << " HTTP " << http;
        }
    }
}
