#ifndef XOP_EXECUTION_KEPT_BOOK_HPP
#define XOP_EXECUTION_KEPT_BOOK_HPP
// ---------------------------------------------------------------------------
// kept_book.hpp -- [S74 2026-09-20] what a KEEP stop leaves behind, in words.
//
// A stop with the policy "keep" (xop/util/stop_offers_policy.hpp) cancels
// nothing, so the one thing it owes the operator is a truthful line: how many
// offers are still resting, on which pairs, and until when they can live with
// no engine behind them.
//
// WHAT THE ENGINE CAN AND CANNOT KNOW WITHOUT ASKING THE WALLET
// -------------------------------------------------------------
// The keep path sends NO RPC -- it must work against a wedged wallet, which is
// one of the reasons to want it -- so everything here comes from State and the
// running config:
//
//   * An offer THIS PROCESS posted has a wall-clock creation time
//     (PendingOffer::created_at_ts) and was posted with the running config's
//     expiry, whose echo was verified at creation (offer_expiry.hpp). Its
//     on-chain expiry is that time plus the expiry, to within the create RPC's
//     latency: max_time is computed just BEFORE the RPC and created_at_ts is
//     stamped just after, so the figure reported here is never EARLIER than
//     the real one.
//   * An offer restored from offer_log at boot, or adopted from the wallet,
//     has no wall-clock creation time in State, and this process never saw
//     what it was posted with. If it carries the configured expiry it cannot
//     outlive now + that expiry; if it was posted before the expiry was turned
//     on it carries NONE. The line says exactly that and no more.
//   * A pair whose effective expiry is 0 posts offers that never expire.
//
// Offers already cancel_pending are counted apart: a cancel was submitted for
// them before the stop, so they are not "kept" -- the wallet verdict or the
// next engine's escalation finishes them.
//
// Pure: no I/O, no engine types, and the clock is an argument.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xop::execution {

/// What the keep path knows about one offer in State.
struct KeptOfferFacts {
    std::string   pair_name;
    bool          cancel_pending{false};
    /// Wall-clock second this PROCESS posted the offer; 0 when it did not
    /// (restored at boot, or adopted from the wallet).
    std::int64_t  posted_unix_s{0};
    /// effective_offer_expiry_secs() for the offer's pair under the running
    /// config; 0 means the pair posts offers with no on-chain expiry.
    std::uint32_t expiry_secs{0};
};

struct KeptBookSummary {
    /// Live, takeable, and left that way by this stop.
    std::size_t resting{0};
    /// A cancel was already submitted before the stop: not kept, not counted
    /// in anything below.
    std::size_t cancel_in_flight{0};
    /// Resting offers per pair, sorted by pair name.
    std::vector<std::pair<std::string, std::size_t>> per_pair;

    /// Posted by this process on a pair with an expiry: the expiry is known.
    std::size_t  expiry_known{0};
    std::int64_t soonest_expiry_unix_s{0};
    std::int64_t latest_expiry_unix_s{0};

    /// On a pair with an expiry, but this process did not post them.
    std::size_t  expiry_unseen{0};
    /// now + the longest configured expiry among them: the latest instant
    /// such an offer can expire IF it carries the configured expiry.
    std::int64_t unseen_expiry_bound_unix_s{0};

    /// On a pair whose effective expiry is 0.
    std::size_t  no_expiry{0};
};

[[nodiscard]] inline KeptBookSummary summarise_kept_book(
    const std::vector<KeptOfferFacts>& offers,
    std::int64_t                       now_unix_s)
{
    KeptBookSummary out;
    std::map<std::string, std::size_t> by_pair;
    for (const KeptOfferFacts& offer : offers) {
        if (offer.cancel_pending) {
            ++out.cancel_in_flight;
            continue;
        }
        ++out.resting;
        ++by_pair[offer.pair_name];

        if (offer.expiry_secs == 0) {
            ++out.no_expiry;
            continue;
        }
        const std::int64_t life = static_cast<std::int64_t>(offer.expiry_secs);
        if (offer.posted_unix_s > 0) {
            const std::int64_t expires = offer.posted_unix_s + life;
            if (out.expiry_known == 0) {
                out.soonest_expiry_unix_s = expires;
                out.latest_expiry_unix_s  = expires;
            } else {
                out.soonest_expiry_unix_s = std::min(out.soonest_expiry_unix_s, expires);
                out.latest_expiry_unix_s  = std::max(out.latest_expiry_unix_s, expires);
            }
            ++out.expiry_known;
        } else {
            out.unseen_expiry_bound_unix_s =
                std::max(out.unseen_expiry_bound_unix_s, now_unix_s + life);
            ++out.expiry_unseen;
        }
    }
    out.per_pair.assign(by_pair.begin(), by_pair.end());
    return out;
}

/// "2026-09-21 15:04:05 UTC". Civil-from-days arithmetic rather than
/// gmtime: no static buffer, no platform variant, and a test can pin it.
[[nodiscard]] inline std::string format_unix_utc(std::int64_t unix_s)
{
    constexpr std::int64_t kDay = 86400;
    std::int64_t days = unix_s / kDay;
    std::int64_t rem  = unix_s % kDay;
    if (rem < 0) {
        rem += kDay;
        --days;
    }
    const std::int64_t z   = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp  = (5 * doy + 2) / 153;
    const std::int64_t day   = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year  = yoe + era * 400 + (month <= 2 ? 1 : 0);

    // Zero-padded by hand: a bounded printf of six wide integers is a
    // -Wformat-truncation diagnostic on GCC, and CI builds with -Werror.
    const auto padded = [](std::int64_t value, std::size_t width) {
        std::string digits = std::to_string(value);
        if (digits.size() < width) {
            digits.insert(0, width - digits.size(), '0');
        }
        return digits;
    };
    return padded(year, 4) + "-" + padded(month, 2) + "-" + padded(day, 2) + " "
         + padded(rem / 3600, 2) + ":" + padded((rem % 3600) / 60, 2) + ":"
         + padded(rem % 60, 2) + " UTC";
}

/// The operator-facing line. One string, so the engine logs it once and a
/// test reads exactly what the operator will.
[[nodiscard]] inline std::string describe_kept_book(const KeptBookSummary& s)
{
    if (s.resting == 0) {
        std::string none = "no offers were resting, so nothing was left on the book";
        if (s.cancel_in_flight > 0) {
            none += " (" + std::to_string(s.cancel_in_flight)
                  + " already had a cancel in flight before the stop; this stop "
                    "sent nothing for them)";
        }
        return none;
    }

    std::string out = std::to_string(s.resting)
        + " offer(s) left RESTING on the book and NOT cancelled -- ";
    for (std::size_t i = 0; i < s.per_pair.size(); ++i) {
        if (i != 0) out += ", ";
        out += s.per_pair[i].first + " " + std::to_string(s.per_pair[i].second);
    }
    out += ".";
    if (s.cancel_in_flight > 0) {
        out += " " + std::to_string(s.cancel_in_flight)
             + " more already had a cancel in flight before the stop and are "
               "not counted; this stop sent nothing for them.";
    }

    out += " On-chain expiry:";
    if (s.expiry_known > 0) {
        out += " " + std::to_string(s.expiry_known)
             + " posted by this process stop being takeable between "
             + format_unix_utc(s.soonest_expiry_unix_s) + " (soonest) and "
             + format_unix_utc(s.latest_expiry_unix_s) + " (latest);";
    }
    if (s.expiry_unseen > 0) {
        out += " " + std::to_string(s.expiry_unseen)
             + " were restored at boot or adopted, so this process never saw "
               "their expiry -- one posted under the configured expiry stops "
               "being takeable no later than "
             + format_unix_utc(s.unseen_expiry_bound_unix_s)
             + ", one posted before the expiry was turned on carries NONE;";
    }
    if (s.no_expiry > 0) {
        out += " " + std::to_string(s.no_expiry)
             + " carry NO on-chain expiry (offer_expiry_secs is 0 for their "
               "pair) and stay takeable until an engine or the operator "
               "cancels them;";
    }
    out += " until then they are TAKEABLE and UNMANAGED: no TTL, no "
           "repricing, no dead man's switch. The next engine start re-adopts "
           "them from the wallet and offer_log.";
    return out;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_KEPT_BOOK_HPP
