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

// ---------------------------------------------------------------------------
// [MIN-INPUT-COIN review #162, round 2] create_offer_for_ids.
//
// The min-input-coin fallback answers one specific wallet refusal with one
// more create, on the premise that a refusal proves no offer was built.
// rpc_post returns only its last attempt, so while this endpoint was re-sent
// after a timeout the refusal could answer a COPY whose original had already
// built the offer -- and the fallback then created a second one.
// ---------------------------------------------------------------------------

using xop::rpc::create_possibly_submitted;

TEST(RpcRetryPolicy, ACreateIsNeverResentOnceItMayHaveReachedTheWallet) {
    // Spelled exactly as ChiaWalletRPC::create_offer passes it.
    const RpcRetryPolicy create =
        retry_policy_for_endpoint("create_offer_for_ids");
    ASSERT_EQ(create, RpcRetryPolicy::NeverResend);

    // The shape that made two offers: no reply within the timeout, the
    // request possibly still running inside the wallet.
    EXPECT_FALSE(may_resend(create, CURLE_OPERATION_TIMEDOUT, /*transient=*/true));
    EXPECT_FALSE(may_resend(create, CURLE_RECV_ERROR, true));
    EXPECT_FALSE(may_resend(create, CURLE_GOT_NOTHING, true));
    EXPECT_FALSE(may_resend(create, CURLE_OK, true));  // an HTTP 5xx

    // A wallet that is down was never asked: still worth another attempt.
    EXPECT_TRUE(may_resend(create, CURLE_COULDNT_CONNECT, true));
    EXPECT_TRUE(may_resend(create, CURLE_SSL_CONNECT_ERROR, true));

    // Deliberately narrow: the other non-idempotent endpoint is unchanged.
    EXPECT_EQ(retry_policy_for_endpoint("take_offer"),
              RpcRetryPolicy::RetryTransient);
}

TEST(RpcRetryPolicy, AnUnansweredCreateMayHaveBuiltTheOffer) {
    // What post_merged_side asks before it answers a failed merged create
    // with one create per tier.
    EXPECT_TRUE(create_possibly_submitted(CURLE_OPERATION_TIMEDOUT, 0));
    EXPECT_TRUE(create_possibly_submitted(CURLE_RECV_ERROR, 0));
    EXPECT_TRUE(create_possibly_submitted(CURLE_GOT_NOTHING, 0));
    EXPECT_TRUE(create_possibly_submitted(CURLE_OK, 500));
    EXPECT_TRUE(create_possibly_submitted(CURLE_OK, 200));  // unparseable 2xx

    // Provably never written, or answered with a refusal: the wallet built
    // nothing, and the individual fallback is safe.
    EXPECT_FALSE(create_possibly_submitted(CURLE_COULDNT_CONNECT, 0));
    EXPECT_FALSE(create_possibly_submitted(CURLE_SSL_CONNECT_ERROR, 0));
    EXPECT_FALSE(create_possibly_submitted(CURLE_COULDNT_RESOLVE_HOST, 0));
    EXPECT_FALSE(create_possibly_submitted(CURLE_OK, 400));
    EXPECT_FALSE(create_possibly_submitted(CURLE_OK, 429));
}
