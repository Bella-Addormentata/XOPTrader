// ---------------------------------------------------------------------------
// reconcile_verdict.hpp -- what a direct get_offer answer permits.
//
// [S24 2026-08-29] The one-way stale verdict (State::remove_offer + a DB
// 'cancelled' write) permanently loses any later fill -- the 2026-07-31
// 6-XCH incident. This routing is therefore pure and unit-tested: given
// the wallet's parsed status for an offer that was ABSENT from the paged
// scan, say exactly what the reconciler may do. The rule mirrors
// OfferManager::reconcile_offers and the S25 principle: an unrecognised
// code is no evidence of cancellation -- never grounds for removal.
// ---------------------------------------------------------------------------

#ifndef XOP_MONITORING_RECONCILE_VERDICT_HPP
#define XOP_MONITORING_RECONCILE_VERDICT_HPP

namespace xop::monitoring {

enum class DirectLookupVerdict {
    Live,                 ///< pending in the wallet: clear misses, keep.
    DeferToFillDetector,  ///< CONFIRMED: a fill, never label it cancelled.
    Stale,                ///< CANCELLED/FAILED: terminal, safe to reap.
    KeepTracked,          ///< anything else: no evidence, not ours to remove.
};

/// Route a parsed wallet trade status (canonical ints; -1 = unparseable).
[[nodiscard]] constexpr DirectLookupVerdict
classify_direct_lookup(int status) noexcept
{
    if (status >= 0 && status <= 2) {
        return DirectLookupVerdict::Live;            // PENDING_*
    }
    if (status == 4) {
        return DirectLookupVerdict::DeferToFillDetector;  // CONFIRMED
    }
    if (status == 3 || status == 5) {
        return DirectLookupVerdict::Stale;           // CANCELLED / FAILED
    }
    // Unparseable (-1) or a status code this build does not know (>= 6,
    // e.g. a newer wallet's extension of TradeStatus). Removing on an
    // unknown code would repeat the exact incident this file guards.
    return DirectLookupVerdict::KeepTracked;
}

}  // namespace xop::monitoring

#endif  // XOP_MONITORING_RECONCILE_VERDICT_HPP
