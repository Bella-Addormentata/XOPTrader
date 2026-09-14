#ifndef XOP_EXECUTION_TERMINAL_RECHECK_HPP
#define XOP_EXECUTION_TERMINAL_RECHECK_HPP
// ---------------------------------------------------------------------------
// terminal_recheck.hpp -- the verdict OfferManager::recheck_terminal returns.
//
// [review 2026-09-13, round 2] Moved unchanged out of offer_manager.hpp, which
// includes this header, so that the pure cancel ladder (cancel_retry.hpp) can
// sort re-checked offers by it without pulling in asio, State or the RPC
// clients.
// ---------------------------------------------------------------------------

namespace xop::execution {

/// [S25 2026-08-24] Verdict from OfferManager::recheck_terminal().
enum class TerminalRecheck {
    /// Wallet still reports CANCELLED/FAILED -- the buffered cancellation
    /// may be written.
    StillTerminal,
    /// Wallet reports a recognised PENDING state again.  The offer has
    /// been re-adopted into State by recheck_terminal; discard the
    /// buffered write.
    Revived,
    /// Wallet reports CONFIRMED -- the observation was reorged into a
    /// FILL.  recheck_terminal has put the offer back into State so
    /// detect_fills() can record it; the caller must NOT write a
    /// cancellation.
    Confirmed,
    /// Wallet unreachable, or its status is one this build does not
    /// recognise.  NOT a verdict: the caller must retry rather than
    /// assume any of the above.  An unknown code is specifically NOT
    /// treated as "live" -- that would discard a one-way cancellation on
    /// no evidence.
    NoVerdict,
};

}  // namespace xop::execution

#endif  // XOP_EXECUTION_TERMINAL_RECHECK_HPP
