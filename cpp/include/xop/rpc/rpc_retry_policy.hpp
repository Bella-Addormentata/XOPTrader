// ---------------------------------------------------------------------------
// rpc_retry_policy.hpp -- whether rpc_post may answer a failed request by
// SENDING THE SAME REQUEST AGAIN.
//
// [BULKCANCEL-B 2026-09-13] ChiaRPCBase::rpc_post re-sent EVERY endpoint on
// every failure is_transient() accepts, CURLE_OPERATION_TIMEDOUT included.  A
// timeout proves only that no answer arrived within request_timeout (30 s).
// It does not prove the wallet never received the request, and for a request
// that changes wallet state a second copy is a second action, not a retry of
// the first.
//
// cancel_offers is the case that happened.  engine.log, 2026-09-12 (local
// time; reproduced by the cancel-truth spec review):
//
//   22:47:38.834  cancel_all sends the wallet-wide cancel_offers
//   22:48:08.847  [attempt 1/4] CURL error 28 -- rpc_post re-sends it
//   22:48:09.563  the copy is refused: "Wallet needs to be fully synced"
//   22:48:19-31   the FIRST request, still running inside the wallet, writes
//                 its cancel transaction records
//
// The refusal was luck.  In chia 2.7.4 the handler pages the wallet's book
// from offset 0 on every call (wallet_rpc_api.py cancel_offers), so a copy
// the wallet admits runs the sweep a second time over the same offers: a
// second set of cancel spends for the same offer coins, batch_fee charged
// again, and another fee coin selected -- all of it conflicting in the
// mempool with what the first request pushed, which mempool_manager.py
// can_replace admits only for a superset of the conflicting coins paying at
// least MEMPOOL_MIN_FEE_INCREASE (10,000,000 mojos) more.
//
// WHAT MAY STILL BE RE-SENT.  Only a failure in which the request cannot have
// reached the handler: CURLE_COULDNT_CONNECT (no TCP connection was made) and
// CURLE_SSL_CONNECT_ERROR (the TLS handshake failed, and the HTTP request is
// only written after it completes).  perform_request builds a fresh easy
// handle for every attempt, so neither code can describe a reused connection
// that had already carried the request.  A timeout, a receive or send error,
// an empty reply, a partial transfer and an HTTP 5xx can each follow a
// request the wallet has already acted on, so they surface to the caller.
//
// WHO GETS THAT FAILURE.  This layer only stops the automatic copy.  The
// failure now reaches the caller about 30 s after the send, where before it
// arrived only once the re-sends ran out (up to four 30 s attempts plus 3.5 s
// of backoff, ~124 s).  [review 2026-09-13, round 2] cancel_possibly_submitted
// below tells "no answer" apart from "refused", and the cancel_all path acts
// on it:
//
//   OfferManager::cancel_all   sends NOTHING more after a possibly-submitted
//                              failure: every tracked id comes back failed,
//                              with CancelOutcome::bulk_possibly_submitted.
//                              A refusal, or a failure before the request was
//                              written, still falls back to per-id
//                              cancel_offer calls at once.
//   the S46 shutdown ladder    then waits at least one request timeout plus a
//                              margin, re-checks every offer, and re-cancels
//                              only the ones still live
//                              (execution/cancel_retry.hpp).  It ends in the
//                              S31 path below if it stops with offers still
//                              live or the sweep unproven.
//   operator Cancel All        waits the same way with cancel_all_inflight_
//                              held, then re-checks and re-cancels.
//   the S31 dead man's switch  (Engine::watchdog_cancel_book) is unchanged:
//                              ONE attempt on its own client, then FAILED and
//                              "cancel by hand", although the sweep may still
//                              be running.
//   XCH recovery               (Engine::step_xch_recovery, with
//                              recovery.cancel_on_enter set) is unchanged: it
//                              sends the wallet-wide cancel_offers AGAIN on
//                              its next block while recovery lasts, and that
//                              copy can overlap a first request that has not
//                              finished.
//
// DELIBERATELY CONSERVATIVE.  A connect-phase timeout (CURLOPT_CONNECTTIMEOUT_MS)
// also reports CURLE_OPERATION_TIMEDOUT, which this layer cannot tell apart
// from a response timeout, so it is not re-sent either.  A cancel that never
// left surfaces as a failure the callers above already handle; a second copy
// of one that did arrive cannot be called back.
//
// Pure and constexpr, no I/O.  The decision is unit-tested in
// tests/test_rpc_retry_policy.cpp; tests/test_rpc_retry_wiring.py pins that
// rpc_post consults it on BOTH of its re-send paths.
//
// Compliant with:
//   ISO/IEC 5055  -- pure functions, no I/O, no UB
//   ISO/IEC 25000 -- single responsibility, unit-tested in isolation
// ---------------------------------------------------------------------------

#ifndef XOP_RPC_RPC_RETRY_POLICY_HPP
#define XOP_RPC_RPC_RETRY_POLICY_HPP

#include <string_view>

#include <curl/curl.h>

namespace xop::rpc {

/// How rpc_post may treat a failed attempt at one endpoint.
enum class RpcRetryPolicy {
    /// Re-send on any failure ChiaRPCBase::is_transient accepts.  Right for
    /// reads, where a duplicate request costs one more round trip.
    RetryTransient,
    /// Re-send ONLY when the request provably never reached the handler.
    /// For endpoints whose second copy is a second wallet action.
    NeverResend,
};

/// The policy for an rpc_post endpoint name (e.g. "cancel_offers").
///
/// Only the two cancel endpoints are NeverResend.  create_offer_for_ids and
/// take_offer are not idempotent either; they are out of scope for this
/// change and keep the transient retry (see the PR that introduced this).
[[nodiscard]] constexpr RpcRetryPolicy retry_policy_for_endpoint(
    std::string_view endpoint) noexcept
{
    if (endpoint == "cancel_offers" || endpoint == "cancel_offer") {
        return RpcRetryPolicy::NeverResend;
    }
    return RpcRetryPolicy::RetryTransient;
}

/// May rpc_post send this request again after a failed attempt?
///
/// @param policy     retry_policy_for_endpoint(endpoint).
/// @param rc         The attempt's CURLcode; CURLE_OK when the transfer
///                   completed and the failure is an HTTP status.
/// @param transient  ChiaRPCBase::is_transient(rc, http_code) for the attempt.
///                   Never widened here: a failure that is not transient is
///                   not re-sent under any policy.
[[nodiscard]] constexpr bool may_resend(RpcRetryPolicy policy,
                                        CURLcode       rc,
                                        bool           transient) noexcept
{
    if (policy == RpcRetryPolicy::NeverResend) {
        return transient
            && (rc == CURLE_COULDNT_CONNECT || rc == CURLE_SSL_CONNECT_ERROR);
    }
    return transient;
}

/// Did a cancel that FAILED possibly reach the wallet's handler -- so that
/// the wallet may have run it, or may still be running it?
///
/// [review 2026-09-13, round 2] may_resend() above stops rpc_post sending a
/// cancel twice.  This stops its CALLER doing the same one layer up:
/// OfferManager::cancel_all used to answer any failed wallet-wide sweep with
/// per-offer cancel_offer calls at once.  In chia 2.7.4 cancel_offers and
/// cancel_offer both take the wallet state lock, and
/// TradeManager.cancel_pending_offers builds spends without checking trade
/// status, so a per-offer cancel queued behind a sweep that is still running
/// builds a SECOND, conflicting spend of an offer the sweep already cancelled
/// -- and neither bundle can replace the other.
///
/// TRUE when the failure can follow a request the handler received: a
/// timeout, an empty reply, a send or receive error, a partial transfer, or
/// an HTTP 5xx.  FALSE for every other code: those fail before the request
/// is written (no connection, a failed TLS handshake, a bad URL), and any
/// other HTTP status is an answer.
///
/// A CONNECT-PHASE TIMEOUT IS CLASSED AS POSSIBLY SUBMITTED ON PURPOSE.
/// CURLOPT_CONNECTTIMEOUT_MS (3 s on localhost, chia_rpc.cpp) reports the
/// same CURLE_OPERATION_TIMEDOUT as a response that never came, and nothing
/// here can tell the two apart.  The two mistakes do not cost the same: a
/// cancel that never left, misread as possibly submitted, costs one wait and
/// one re-check before the offers still live are cancelled; a cancel that
/// arrived, misread as never sent, builds a duplicate spend that cannot be
/// called back.
[[nodiscard]] constexpr bool cancel_possibly_submitted(
    CURLcode rc, long http_code) noexcept
{
    switch (rc) {
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_GOT_NOTHING:
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_PARTIAL_FILE:
            return true;
        case CURLE_OK:
            return http_code >= 500;
        default:
            return false;
    }
}

}  // namespace xop::rpc

#endif  // XOP_RPC_RPC_RETRY_POLICY_HPP
