#pragma once
// ---------------------------------------------------------------------------
// mid_gate -- published-mid plausibility gate and independent anchor chain.
//
// [S20 2026-08-24] A single junk print on BYC/wUSDC.b (187.461980 against a
// ~1.0 peg, byte-identical for 12+ hours) valued ~104 BYC at $187 each,
// seeded a $15,180 phantom equity peak on a fresh process within 40 minutes
// of restart, and latched the max-drawdown breaker at a 98.29% "drawdown"
// against real ~$259 equity.  Three prior breaker trips on 2026-08-23 came
// from the same class of defect (a stale 0.75 print in an emptied book).
//
// Root causes, all closed here or by callers of this header:
//   * The per-offer outlier filter anchored on the pair's OWN previous
//     published mid -- once the mid was junk, honest offers deviated ~9900
//     bps from it and were rejected while the junk survived (self-referential
//     lock-in), and the filter was skipped entirely on the first cycle when
//     no previous mid existed (the fresh-restart re-poisoning path).
//   * No absolute plausibility bound existed on the published mid at all.
//
// The gate compares a candidate mid against an INDEPENDENT anchor -- never
// the pair's own history -- chosen by fixed priority:
//
//   CEX reference > implied cross (triangulated through healthy sibling
//   books) > AMM pool price > external fair-value estimate > peg target
//   (stablecoin pairs only).
//
// The implied cross is the generalisation requested for every triangle the
// bot trades: for a target pair A/B and any common asset C with live pairs
// (A,C) and (B,C), implied(A/B) = rate(A->C) * rate(C->B).  Both legs must
// be two-sided, fresh, and tight; the error of the implied mid is bounded
// by the healthy legs' spreads, not by the sick book it anchors.
//
// The band is deliberately WIDE (3x default): the gate exists to refuse
// absurdity (187x), not to adjudicate real repricing.  A genuine collapse
// beyond the band still publishes through the book-confirmation escape: a
// fresh two-sided third-party book with a coherent spread is executable
// evidence the whole market moved, which one absurd resting offer or a
// stale print can never produce.
//
// Pure header, no engine or feed types: everything here is pin-testable
// (cpp/tests/test_mid_gate.cpp) the same way risk/drawdown_breaker.hpp is.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xop::midgate {

enum class AnchorSource {
    None,
    Cex,
    ImpliedCross,
    Amm,
    FairValue,
    Peg,
};

struct Anchor {
    double       value{0.0};   // 0 = no anchor available
    AnchorSource source{AnchorSource::None};
};

// Candidate values are 0 when unavailable.  Freshness tapering is the
// CALLER's job (each source already carries its own age gate in the feed);
// a non-zero candidate here asserts "fresh enough to reference".
struct AnchorCandidates {
    double cex_mid{0.0};
    double implied_cross{0.0};
    double amm_mid{0.0};
    double fair_value_estimate{0.0};
    double peg_target{0.0};   // >0 only for stablecoin pairs
};

[[nodiscard]] inline Anchor select_anchor(const AnchorCandidates& c) noexcept
{
    // Non-finite candidates are skipped rather than selected-then-rejected:
    // +inf passes a bare `> 0` test and would WIN the priority chain, and
    // gate_mid would then discard it as invalid instead of falling through
    // to a perfectly good lower-priority anchor -- on a first cycle, with
    // no accepted history to fall back on, that silently disables the
    // plausibility check entirely.  The ingesters accept infinities today,
    // so this is reachable from feed data, not just from tests.
    auto usable = [](double v) noexcept {
        return v > 0.0 && std::isfinite(v);
    };
    if (usable(c.cex_mid))             return {c.cex_mid, AnchorSource::Cex};
    if (usable(c.implied_cross))       return {c.implied_cross, AnchorSource::ImpliedCross};
    if (usable(c.amm_mid))             return {c.amm_mid, AnchorSource::Amm};
    if (usable(c.fair_value_estimate)) return {c.fair_value_estimate, AnchorSource::FairValue};
    if (usable(c.peg_target))          return {c.peg_target, AnchorSource::Peg};
    return {};
}

// One leg of a two-hop implied cross.  `mid` is the leg pair's published
// price in its own quote-per-base orientation; `invert` selects 1/mid when
// the target rate needs the opposite orientation.  spread_bps of 0 means a
// one-sided or crossed book (compute_spread_bps semantics), which fails the
// health test below.
struct CrossLeg {
    double mid{0.0};
    double spread_bps{0.0};
    bool   fresh{false};
    bool   invert{false};
};

[[nodiscard]] inline bool leg_healthy(const CrossLeg& l,
                                      double max_leg_spread_bps) noexcept
{
    return l.mid > 0.0 && l.fresh
        && l.spread_bps > 0.0 && l.spread_bps <= max_leg_spread_bps;
}

// implied(A/B) from legs (A,C) and (C,B), each already oriented via
// `invert`.  Returns 0 unless BOTH legs are healthy: a half-healthy
// triangle is not an anchor, it is a guess.
[[nodiscard]] inline double implied_cross(const CrossLeg& first,
                                          const CrossLeg& second,
                                          double max_leg_spread_bps) noexcept
{
    if (!leg_healthy(first, max_leg_spread_bps)
        || !leg_healthy(second, max_leg_spread_bps)) {
        return 0.0;
    }
    const double a = first.invert  ? 1.0 / first.mid  : first.mid;
    const double b = second.invert ? 1.0 / second.mid : second.mid;
    const double implied = a * b;
    return (std::isfinite(implied) && implied > 0.0) ? implied : 0.0;
}

enum class GateVerdict {
    Accept,
    RejectAnchor,   // outside the anchor band, no book confirmation
    RejectStep,     // anchorless: moved more than max_step_frac in one cycle
};

struct GateInputs {
    double candidate_mid{0.0};
    Anchor anchor{};

    // Multiplicative band around the anchor; candidate/anchor must lie in
    // [1/ratio, ratio].  <= 1.0 disables the anchor test.
    double anchor_band_ratio{3.0};

    // Book-confirmation escape: a fresh two-sided dust-filtered third-party
    // book with spread in (0, confirm_max] overrides a band breach.
    bool   book_two_sided{false};
    bool   book_fresh{false};
    double book_spread_bps{0.0};
    double book_confirm_max_spread_bps{5000.0};

    // Anchorless fallback: bound the per-cycle step against the last
    // ACCEPTED mid.  <= 0 disables; 0 last_accepted means no history yet.
    double last_accepted_mid{0.0};
    double max_step_frac{0.5};
};

[[nodiscard]] inline bool book_confirms(const GateInputs& in) noexcept
{
    return in.book_two_sided && in.book_fresh
        && in.book_spread_bps > 0.0
        && in.book_spread_bps <= in.book_confirm_max_spread_bps;
}

[[nodiscard]] inline GateVerdict gate_mid(const GateInputs& in) noexcept
{
    // Finiteness FIRST.  Ordering matters: -infinity satisfies `<= 0.0`,
    // so a no-mid test placed ahead of this would Accept it and hand a
    // non-finite value to the snapshot conversion downstream.  NaN
    // likewise fails every comparison and would otherwise sail through the
    // band test as "in band".
    if (!std::isfinite(in.candidate_mid)) {
        return GateVerdict::RejectAnchor;
    }
    if (in.candidate_mid <= 0.0) {
        return GateVerdict::Accept;   // nothing to gate; no-mid is upstream's verdict
    }
    // A non-finite ANCHOR is not a reference; ignore it and fall through
    // to the step bound rather than letting it silently disable the test
    // (the config parser rejects these, but the struct is public API).
    if (std::isfinite(in.anchor.value)
        && in.anchor.value > 0.0 && in.anchor_band_ratio > 1.0) {
        const double ratio = in.candidate_mid / in.anchor.value;
        if (ratio > in.anchor_band_ratio
            || ratio < 1.0 / in.anchor_band_ratio) {
            return book_confirms(in) ? GateVerdict::Accept
                                     : GateVerdict::RejectAnchor;
        }
        return GateVerdict::Accept;
    }
    if (in.max_step_frac > 0.0 && in.last_accepted_mid > 0.0) {
        const double step =
            std::abs(in.candidate_mid / in.last_accepted_mid - 1.0);
        if (step > in.max_step_frac) {
            return book_confirms(in) ? GateVerdict::Accept
                                     : GateVerdict::RejectStep;
        }
    }
    return GateVerdict::Accept;
}

// How the two legs of a candidate triangle must be oriented, or nullopt
// when the two pairs do not form one with the target.
//
// A pair's mid is QUOTE per BASE, so building implied(A/B) = rate(A->C) *
// rate(C->B) needs each leg inverted or not depending on which way round
// it is configured -- four combinations, and getting any one backwards
// yields a plausible-looking number that is silently reciprocal.  The
// decision lives here, next to implied_cross, so the tests can drive the
// real branches rather than hand-supplying the flags they are meant to
// verify.
struct LegOrientation {
    bool invert_first{false};   // rate(A->C) from pair 1
    bool invert_second{false};  // rate(C->B) from pair 2
};

[[nodiscard]] inline std::optional<LegOrientation> orient_triangle(
    const std::string& target_base,  const std::string& target_quote,
    const std::string& p1_base,      const std::string& p1_quote,
    const std::string& p2_base,      const std::string& p2_quote)
{
    // Leg 1 must touch the target's BASE; the other side is the common
    // asset C.  A/C is used as-is (its mid is already C per A); C/A is
    // inverted.
    std::string common;
    LegOrientation o;
    if (p1_base == target_base) {
        common = p1_quote;
    } else if (p1_quote == target_base) {
        common          = p1_base;
        o.invert_first  = true;
    } else {
        return std::nullopt;
    }

    // The "triangle" through the target's own quote is the target itself.
    if (common == target_quote) return std::nullopt;

    // Leg 2 must carry C to the target's QUOTE.  C/B is used as-is; B/C is
    // inverted.
    if (p2_base == common && p2_quote == target_quote) {
        o.invert_second = false;
    } else if (p2_base == target_quote && p2_quote == common) {
        o.invert_second = true;
    } else {
        return std::nullopt;
    }
    return o;
}

// True median of a candidate set, SORTING IN PLACE.  Lives here rather
// than inline at the call site so production and tests exercise the same
// code: an even count must average the two middle observations, or a
// single high outlier becomes the whole anchor (n == 2 is the common case
// when two triangles exist).  Returns 0 for an empty set.
[[nodiscard]] inline double median_of(std::vector<double>& v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    if (n % 2 == 0) {
        return (v[n / 2 - 1] + v[n / 2]) / 2.0;
    }
    return v[n / 2];
}

// Absurdity bound for individual competing offers, DERIVED from the gate's
// anchor band so the two can never be configured into conflict.
//
// The per-offer filter and the published-mid gate do different jobs: the
// filter removes offers no honest market could produce, while the gate
// adjudicates whether the resulting mid is plausible.  Filtering at the
// gate's own band would make the gate's book-confirmation escape
// unreachable -- a genuine beyond-band repricing would lose every honest
// offer near the new market before the book was assembled, so a real
// collapse could never publish.  A hard 10x floor covers the default 3x
// band; beyond that the bound tracks the band at 2x, so the invariant
// "offer bound strictly wider than gate band" holds for every setting the
// parser accepts (a fixed 10x silently broke it at band >= 10).
inline constexpr double kOfferAbsurdityFloor    = 10.0;
inline constexpr double kOfferAbsurdityBandMult = 2.0;

[[nodiscard]] inline double offer_absurdity_ratio(double anchor_band_ratio) noexcept
{
    return std::max(kOfferAbsurdityFloor,
                    anchor_band_ratio * kOfferAbsurdityBandMult);
}

[[nodiscard]] inline const char* anchor_source_name(AnchorSource s) noexcept
{
    switch (s) {
        case AnchorSource::Cex:          return "cex";
        case AnchorSource::ImpliedCross: return "implied-cross";
        case AnchorSource::Amm:          return "amm";
        case AnchorSource::FairValue:    return "fair-value";
        case AnchorSource::Peg:          return "peg";
        case AnchorSource::None:         break;
    }
    return "none";
}

}  // namespace xop::midgate
