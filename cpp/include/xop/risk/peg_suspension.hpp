// ---------------------------------------------------------------------------
// peg_suspension.hpp -- asset-level peg failure: detect, latch, re-enable.
//
// [PEGSUSPEND 2026-08-29] The pair-level DepegDetector watches a
// stable/stable pair's OWN mid against its own peg target, which is the
// right instrument for BYC/wUSDC.b and useless for XCH/wUSDC.b -- that mid
// moves with XCH, so the wrapper can lose its peg entirely while every pair
// that prices things IN it looks healthy. warp.green was compromised on
// 2026-08-25 and wUSDC.b depegged; nothing at the asset level noticed.
//
// This is the asset-level half. Each declared, enforced pegged asset is
// observed in USD through a route that DOES NOT pass through its own par
// (usd_per_xch / mid(XCH/asset) -- a par-based observation of the par would
// be circular and blind). Sustained deviation past the asset's bail
// threshold latches a SUSPENSION:
//
//   * valuation stops honouring the declared par (declared_usd_par ->
//     nullopt; the asset values off the market like any unpegged CAT);
//   * every pair whose base OR quote is the asset stops quoting, and its
//     resting offers are cancelled at the transition -- off means flat;
//   * the latch is STICKY. A depeg that heals on the chart is not a bridge
//     that healed; whether the peg is trustworthy again is the operator's
//     judgement, exercised through the GUI's re-enable button, not a
//     counter's.
//
// Pure and total: the decisions live here, testable without an engine; the
// Engine owns the wiring (observation route, cancels, alerts, the
// re-enable flag file).
// ---------------------------------------------------------------------------

#ifndef XOP_RISK_PEG_SUSPENSION_HPP
#define XOP_RISK_PEG_SUSPENSION_HPP

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xop::risk {

// ---------------------------------------------------------------------------
// Runtime state for one declared asset's peg.
// ---------------------------------------------------------------------------
struct PegRuntime {
    /// Consecutive observations at or past the bail threshold.
    std::uint32_t above_bail{0};

    /// The sticky latch. Set by observe() on a sustained bail, cleared ONLY
    /// by reenable() -- never by the price recovering, because a chart that
    /// heals says nothing about whether the bridge behind the wrapper did.
    bool suspended{false};

    /// Block at which the suspension latched (0 = not suspended). For the
    /// operator-facing display and the audit trail.
    std::uint32_t suspended_at_block{0};

    /// Last observed deviation, percent. For display; never for decisions.
    double last_deviation_pct{0.0};
};

/// What one observation asks the caller to do.
enum class PegObservation {
    Holding,        ///< within warn threshold
    Warn,           ///< past warn, not (yet sustained) past bail
    JustSuspended,  ///< THIS observation latched the suspension -- cancel now
    Suspended,      ///< latch already held; nothing new to do
};

inline const char* to_string(PegObservation o)
{
    switch (o) {
        case PegObservation::Holding:       return "holding";
        case PegObservation::Warn:          return "warn";
        case PegObservation::JustSuspended: return "just_suspended";
        case PegObservation::Suspended:     return "suspended";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// One observation of one asset.
//
// `usd_observed` is the asset's market-derived USD price, from a route that
// does not consult the asset's own par. Non-finite or non-positive
// observations are IGNORED rather than counted in either direction: an
// unpriceable tick is a data gap, not evidence about the peg -- counting it
// as deviation would suspend on an outage, and counting it as health would
// reset a genuine streak with a blind read. The streak simply holds.
//
// `sustained_observations` guards against suspending on one junk print or a
// momentary wick: the deviation must persist. Zero is clamped to one --
// "latch immediately" -- rather than "never latch", because a threshold an
// operator set to zero means now, not never.
// ---------------------------------------------------------------------------
[[nodiscard]] inline PegObservation observe_peg(
    PegRuntime&   rt,
    double        usd_observed,
    double        peg_target,
    double        bail_pct,
    double        warn_pct,
    std::uint32_t sustained_observations,
    std::uint32_t block_height) noexcept
{
    if (rt.suspended) {
        // Keep the display honest while latched, but no state transitions:
        // the latch is the operator's to clear.
        if (std::isfinite(usd_observed) && usd_observed > 0.0
                && peg_target > 0.0) {
            rt.last_deviation_pct =
                std::fabs(usd_observed - peg_target) / peg_target * 100.0;
        }
        return PegObservation::Suspended;
    }

    if (!(std::isfinite(usd_observed) && usd_observed > 0.0)
            || !(std::isfinite(peg_target) && peg_target > 0.0)) {
        return rt.above_bail > 0 ? PegObservation::Warn
                                 : PegObservation::Holding;
    }

    const double dev_pct =
        std::fabs(usd_observed - peg_target) / peg_target * 100.0;
    rt.last_deviation_pct = dev_pct;

    if (dev_pct >= bail_pct) {
        ++rt.above_bail;
        const std::uint32_t needed =
            sustained_observations == 0 ? 1u : sustained_observations;
        if (rt.above_bail >= needed) {
            rt.suspended = true;
            rt.suspended_at_block = block_height;
            return PegObservation::JustSuspended;
        }
        return PegObservation::Warn;
    }

    // Below bail: the streak resets. Warn is stateless -- it is a message,
    // not a latch, and half-latching it would re-create the S17 alert spam.
    rt.above_bail = 0;
    return dev_pct >= warn_pct ? PegObservation::Warn
                               : PegObservation::Holding;
}

// ---------------------------------------------------------------------------
// The operator's re-enable. Clears the latch AND the streak, so detection
// re-arms from zero -- if the asset is still depegged, the very next
// sustained run re-suspends it, which is the safety property that makes the
// button safe to offer at all: re-enabling is never "trust it forever",
// only "look again".
// ---------------------------------------------------------------------------
inline void reenable_peg(PegRuntime& rt) noexcept
{
    rt.suspended = false;
    rt.suspended_at_block = 0;
    rt.above_bail = 0;
}

// ---------------------------------------------------------------------------
// WHERE THE USD-PER-XCH ANCHOR CAME FROM.
//
// [CIRCANCHOR 2026-09-02] The observation route below divides an XCH price
// in dollars by the XCH/<asset> mid. That is only an observation about the
// asset if the DOLLAR side came from somewhere other than the asset itself.
//
// Engine::usd_per_xch() has two sources. The first is an external CoinGecko
// quote for XCH -- genuinely independent of every asset here. The second is
// a FALLBACK, used when the external feed goes stale: it values XCH off the
// first enabled XCH/<par wrapper> pair as mid * that wrapper's DECLARED par.
// When the wrapper it picked is the very asset the watcher is about to
// judge, the two mids cancel and the observation is the declared par again,
// to floating-point rounding -- the watcher reads its own input back, sits
// permanently at par, and never detects the depeg it exists to detect.
//
// That is exactly the property the fair-value refusal below already rules
// out; this type is what lets the same rule reach the fallback. So the
// anchor reports its PROVENANCE, not just its value: which source priced
// XCH, and -- for the fallback -- whose declared par it consumed.
//
// Provenance, not a heuristic on the VALUE. "Refuse when the observation
// lands suspiciously exactly on par" would refuse precisely when a healthy
// wrapper is healthy, and would be unfalsifiable. A par-holding wrapper is
// SUPPOSED to read at par; the defect is the route, so the route is what
// gets inspected.
// ---------------------------------------------------------------------------
enum class XchAnchorKind {
    None,              ///< no anchor available; usd is 0 ("unknown")
    ExternalFeed,      ///< external USD quote for XCH; independent of all assets
    DeclaredParCross,  ///< XCH valued off a pair using that quote's DECLARED par
};

inline const char* to_string(XchAnchorKind k)
{
    switch (k) {
        case XchAnchorKind::None:             return "none";
        case XchAnchorKind::ExternalFeed:     return "external_feed";
        case XchAnchorKind::DeclaredParCross: return "declared_par_cross";
    }
    return "unknown";
}

/// USD per XCH, together with how it was priced.
struct XchUsdAnchor {
    /// USD per XCH. 0 means "unknown", exactly as usd_per_xch() has always
    /// meant it; callers that only want the number are unaffected.
    double usd{0.0};

    XchAnchorKind kind{XchAnchorKind::None};

    /// The asset whose DECLARED par was consumed. Non-empty only for
    /// DeclaredParCross.
    ///
    /// ASSET ID, NOT PAIR NAME, and the distinction is load-bearing. The
    /// observer accepts BOTH orientations (XCH/<asset> and <asset>/XCH), and
    /// a config may carry two markets on the same asset. Keying on the pair
    /// name would let `XCH/wUSDC.b` anchor the price while `wUSDC.b/XCH` is
    /// judged -- a different pair name and the identical circle. It is also
    /// GROUND TRUTH rather than a re-derivation: it is recorded by the same
    /// code path that consumed it, so the two cannot drift apart the way
    /// market_cross_for's comment describes.
    ///
    /// BORROWED, non-owning: it views the config's asset id, which outlives
    /// the observation cycle. Consume it within the cycle; never store it.
    std::string_view par_asset_id{};
};

/// True when this anchor was derived from the declared par of the very asset
/// it is about to be used to judge -- i.e. the observation would be the
/// declared par read back to itself.
///
/// Keyed on the ASSET, deliberately NOT on the kind. A DeclaredParCross
/// anchored on a DIFFERENT wrapper is a real, independent observation: with
/// XCH anchored on XCH/BYC, usd(wUSDC.b) = par(BYC) * mid(XCH/BYC) /
/// mid(XCH/wUSDC.b) is wUSDC.b priced in BYC, and it MOVES when wUSDC.b
/// depegs -- it is the arithmetic that produced the honest $0.44 on
/// 2026-08-30. Refusing every fallback would blind every asset for the whole
/// outage, trading one silent false negative for a broad avoidable one.
///
/// KNOWN LIMIT, recorded rather than chased here: that cross still inherits
/// trust in the ANCHOR asset's par, so two wrappers depegging by the same
/// fraction at once read at par and neither is caught. That second-order
/// dependence is common to every declared par in the system. What this rule
/// fixes is the FIRST-order case, where the anchor asset and the judged
/// asset are the same one -- which is also what stops the compounding,
/// because it un-blinds the anchor asset's own detector.
///
/// [review round 8] THE MORE LIKELY HALF OF THAT LIMIT, spelled out because
/// the paragraph above covers only the both-depeg-together case. When ONE
/// wrapper depegs and it is the ANCHOR, every OTHER asset judged through it
/// is mis-priced by the reciprocal of the anchor's own depeg -- and those
/// assets are not circular for this predicate, so they are observed and
/// believed. Worked example on the shipped config, with the external feed
/// out: XCH truly $1.43, wUSDC.b truly $0.50, BYC healthy at $1.00. XCH is
/// anchored at mid(XCH/wUSDC.b) * par = $2.86, wUSDC.b's own observation is
/// correctly REFUSED as circular, and BYC observes at $2.00 -- 100% off --
/// and latches. The alert names the healthy asset, the remediation cancels
/// the healthy pairs, and XCH/wUSDC.b goes on quoting a 50c wrapper at a
/// declared par of $1.00. Unchanged by this fix in either direction: the
/// guard fires only for the anchor asset, so the BYC path is identical
/// before and after. The engine prints the anchor in the suspension alert so
/// this is recognisable in the log; a real fix needs a corroborating second
/// route before a DeclaredParCross-anchored suspension may latch.
///
/// DOES NOT CONSULT `kind`, on purpose, and the reason is the failure
/// DIRECTION. `par_asset_id` is non-empty only for DeclaredParCross today,
/// so adding `kind == DeclaredParCross` would change no answer -- but it
/// would decide the answer if that invariant were ever broken, and it would
/// decide it the wrong way: an anchor carrying a par asset id under some
/// other kind would be declared NOT circular and observed. Keying on the
/// recorded par asset alone means a confused anchor is REFUSED, and refusing
/// is the safe direction here (see peg_usd_observation). `kind` earns its
/// place on the struct by naming the source in the operator's log line, not
/// by gating this.
///
/// Both terms are load-bearing: without the emptiness check, an anchor with
/// no provenance recorded would "match" an asset with no id.
[[nodiscard]] constexpr bool xch_anchor_is_circular_for(
    const XchUsdAnchor& anchor, std::string_view judged_asset_id) noexcept
{
    return !anchor.par_asset_id.empty()
        && anchor.par_asset_id == judged_asset_id;
}

// Decided at compile time, and asserted here rather than only in the test,
// because this branch has shipped four MSVC-pass/GCC-fail defects and a
// static_assert is checked by whichever compiler is building.
static_assert(xch_anchor_is_circular_for(
                  XchUsdAnchor{1.4142, XchAnchorKind::DeclaredParCross,
                               "wusdc.b"},
                  "wusdc.b"),
              "the anchor's own asset must be refused");
static_assert(!xch_anchor_is_circular_for(
                  XchUsdAnchor{1.43, XchAnchorKind::DeclaredParCross, "byc"},
                  "wusdc.b"),
              "a cross against a DIFFERENT wrapper is a real observation");
static_assert(!xch_anchor_is_circular_for(
                  XchUsdAnchor{1.43, XchAnchorKind::ExternalFeed, {}},
                  "wusdc.b"),
              "an external feed is never circular");
static_assert(!xch_anchor_is_circular_for(
                  XchUsdAnchor{0.0, XchAnchorKind::None, {}}, {}),
              "unpopulated provenance must not masquerade as a match");
static_assert(xch_anchor_is_circular_for(
                  XchUsdAnchor{1.43, XchAnchorKind::ExternalFeed, "wusdc.b"},
                  "wusdc.b"),
              "a par asset recorded under a mislabelled kind is refused, "
              "not waved through");

// ---------------------------------------------------------------------------
// CHOOSING THE ANCHOR -- and recording its provenance in the same breath.
//
// [CIRCANCHOR review round 8] EXTRACTED BECAUSE THE PROVENANCE WAS THE ONE
// PART OF THIS FIX NOTHING COULD TEST. The guard above, its five
// static_asserts and its seven gtests all exercise a struct that some OTHER
// code has to populate correctly, and that code lived in
// Engine::usd_per_xch_anchor(), which no test can reach -- nothing in
// cpp/tests constructs an Engine (TODO S36).
//
// That is not a theoretical gap. Mutating the fallback's return to
// `ExternalFeed, {}` -- dropping the provenance while keeping the VALUE
// identical -- fully reinstates the original defect and leaves the entire
// suite green: xch_anchor_is_circular_for() then answers false for every
// anchor forever, the observation lands on the declared par, and
// observe_peg's below-bail branch zeroes the streak on every graded cycle.
// The header half of this fix is well covered and the engine half was bare.
//
// So the SELECTION moves here, next to the predicate that consumes it, and
// Engine::usd_per_xch_anchor() becomes the adapter -- exactly the split
// usd_route.hpp already made for this same function, and for the same stated
// reason. The rule and the record of which rule fired are now one piece of
// code, so they cannot drift apart.
// ---------------------------------------------------------------------------

/// One ENABLED market, reduced to what anchor selection actually reads.
///
/// BORROWED VIEWS, all of them: they point at the config's own strings, which
/// outlive every consumer of the returned anchor. The vector of candidates is
/// free to be a temporary -- the returned XchUsdAnchor::par_asset_id views
/// the CONFIG, not this struct. Do not build a candidate over a temporary
/// string.
struct XchAnchorCandidate {
    std::string_view pair_name{};
    std::string_view base_asset_id{};
    std::string_view quote_asset_id{};

    /// Does usd_per_xch() accept a pair quoted in this asset as its anchor?
    ///
    /// NARROWER than "has a declared par", and the difference has already
    /// caused one defect (see usd_route.hpp's ParLookups): BYC declares an
    /// enforced par AND prefers its market cross, so it can be VALUED at par
    /// while being rejected as XCH's anchor. This is the narrow question.
    bool quote_anchors_xch{false};
};

/// What one candidate's published book says USD-per-XCH is: mid * declared
/// par. nullopt when that pair cannot produce one at all -- no snapshot, no
/// par, or a par that is currently SUSPENDED.
///
/// A callback, so this header need not know about State or PegRegistry, and so
/// selection stays LAZY: a candidate that is never reached is never priced,
/// which keeps the snapshot reads of the adapter identical to the loop this
/// replaces.
using XchAnchorPrice =
    std::function<std::optional<double>(const XchAnchorCandidate&)>;

/// USD per XCH, together with an honest record of where it came from.
///
/// `enabled_pairs` must ALREADY be filtered to enabled markets -- a disabled
/// pair is not an anchor, and passing the unfiltered list is the mistake this
/// signature exists to make obvious. Same convention as usd_route.hpp.
///
/// ORDER IS THE CONTRACT: external feed first, then the FIRST usable
/// XCH/<par wrapper> candidate in configuration order, then "unknown". The
/// external feed is preferred because XCH has a real, liquid, externally
/// quoted USD price, and routing it through a bridged stablecoin on a thin
/// Chia book adds the wrapper issuer's risk to a number that never needed it.
///
/// NOTE THE TWO WAYS THE FALLBACK IS REACHED, because the operator-facing
/// message has already been wrong about this: a STALE feed, and a feed that
/// is perfectly fresh but carries no usable XCH price (a missing key, a
/// renamed id, a NaN, a non-positive quote). Both land on DeclaredParCross.
/// "The feed is stale" is therefore not a safe thing for a caller to assert.
[[nodiscard]] inline XchUsdAnchor select_xch_usd_anchor(
    bool                                   external_feed_fresh,
    double                                 external_xch_usd,
    const std::vector<XchAnchorCandidate>& enabled_pairs,
    const XchAnchorPrice&                  price_of)
{
    if (external_feed_fresh && std::isfinite(external_xch_usd)
            && external_xch_usd > 0.0) {
        // Independent of every asset in the config: no pair, no declared
        // par. Never circular, and recorded as such.
        return XchUsdAnchor{external_xch_usd, XchAnchorKind::ExternalFeed,
                            {}};
    }

    for (const auto& c : enabled_pairs) {
        if (c.base_asset_id != "xch") continue;
        if (!c.quote_anchors_xch) continue;
        if (!price_of) break;   // no pricer supplied: no fallback exists
        const auto usd = price_of(c);
        if (!usd || !std::isfinite(*usd) || !(*usd > 0.0)) continue;

        // RECORD WHOSE PAR THIS IS. The value is mid(XCH/W) * par(W): it
        // carries both that pair's mid and that asset's DECLARED par. Divide
        // it back out by the same mid -- precisely what the asset-level peg
        // watcher does when judging W -- and what comes out is par(W), an
        // all-clear manufactured from our own declaration.
        //
        // The asset id, NOT the pair name: the observer accepts both
        // orientations, so keying on the name would let XCH/wUSDC.b anchor
        // the price while wUSDC.b/XCH is judged -- a different name and the
        // identical circle.
        return XchUsdAnchor{*usd, XchAnchorKind::DeclaredParCross,
                            c.quote_asset_id};
    }

    // 0 means "unknown", NOT a hard-coded historical rate: that rate was
    // 2.70 while XCH traded near $1.35, and cost basis is PERSISTED, so
    // guessing would bake a 2x error in permanently. Kind None with an empty
    // par asset -- the watcher's positive-anchor guard already refuses this,
    // so it needs no circularity treatment, but recording None rather than
    // leaving the field defaulted keeps "no anchor" distinguishable from
    // "nobody filled this in".
    return XchUsdAnchor{0.0, XchAnchorKind::None, {}};
}

// ---------------------------------------------------------------------------
// peg_usd_observation -- what one pair's published book says an asset is
// worth in USD, or nothing at all.
//
// [SIDEQUALITY 2026-09-01] Extracted from Engine::step_observe_asset_pegs so
// the two ways this route has already gone wrong are drivable by a test
// rather than only by a live engine. Both failures cancelled real offers:
//
//   * SCALE. MarketSnapshot::mid_price is MOJO-SCALED (1e12). Reading it as
//     a bare price yielded usd ~ 3.4e-13 every heartbeat and produced
//     "[PEGSUSPEND] observed $0.0000 vs target 1.0000, 100.0% off" -- for
//     wUSDC.b on 2026-08-29 and BYC on 2026-08-30, both false, both
//     suspending the par and cancelling every offer on every pair touching
//     the asset.
//
//   * SOURCE. Fixing the scale alone was NOT sufficient. On the dislocated
//     XCH/BYC book the correctly scaled observation was 1.43 / 3.25 = $0.44
//     -- still 56% off par, still past bail_pct -- because the published mid
//     inherits a junk side. A book with a disqualified side is not an
//     observation about this asset's peg, so it yields nothing.
//
// Returning nullopt routes the caller into observe_peg's data-gap branch,
// which HOLDS the streak rather than advancing or resetting it: absence of
// evidence neither confirms nor clears a depeg.
//
// DELIBERATELY NOT re-sourced to a fair-value estimate, which is the
// obvious-looking alternative and is circular: par_anchor.hpp feeds a
// DECLARED par into the fair-value solve precisely when nothing else can
// price the asset, so a peg watcher reading that estimate would read its own
// input back, sit permanently at par, and never detect the depeg it exists
// to detect. A peg is observed from a market or not at all.
//
// [CIRCANCHOR 2026-09-02] AND THE SAME RULE NOW REACHES THE USD-PER-XCH
// ANCHOR, which had the identical property and was not covered. The fair-
// value refusal above is enforced structurally -- a fair-value estimate has
// no parameter to arrive through. The anchor cannot be refused that way,
// because the legitimate cross-asset case uses the very same source, so it
// is enforced by the guard below.
//
// [review round 8] WHAT THAT GUARD DOES *NOT* CLOSE, corrected here because
// the previous wording claimed it made the file's opening invariant ("a
// route that DOES NOT pass through its own par") true rather than
// aspirational. It does not. The guard inspects the provenance of the
// NUMERATOR (usd_per_xch) only. The DENOMINATOR -- the published mid -- has
// its own, unexamined path back to the same place:
//
//   * XCH/wUSDC.b's CEX leg is CoinGecko `chia.usd / usd-coin.usd`
//     (engine.cpp's Step 1). That prices the mid off NATIVE USDC, which is
//     structurally incapable of observing a CHIA BRIDGE failure. When the
//     external feed is FRESH -- the normal state, not an outage -- and the
//     DEX book is empty enough that the published mid is the CEX leg alone,
//     usd = chia.usd / (chia.usd / usd-coin.usd) = usd-coin.usd ~= $1.00.
//     The anchor is ExternalFeed, so this guard passes it, and the reading
//     lands on a par PROXY: the same silent false negative, arriving
//     through the source this file certifies as never circular.
//
//   * Even on a healthy two-sided book the CEX leg is blended in at
//     kCexWeight (0.30), which ATTENUATES a real deviation: a true $0.90
//     wUSDC.b reads as 1/(0.7/0.9 + 0.3) = $0.9278, i.e. 7.2% off against a
//     configured bail_pct of 10.0. The effective threshold on wUSDC.b is
//     ~13.7%, not the 10% the operator configured.
//
// Both are OPEN, and both are recorded rather than fixed here: the fix is a
// provenance flag on the published mid, which belongs in market_data.cpp
// alongside the blend that creates the problem, not in this header. XCH/BYC
// is unaffected -- BYC has no CoinGecko mapping, so its mid carries no CEX
// leg at all, which is why the $0.44 figure quoted above (an XCH/BYC book)
// IS what the live route produced.
//
// READ THIS TWICE BEFORE "SIMPLIFYING" IT: THE FAIL-SAFE DIRECTION IS
// INVERTED HERE relative to the house rule. Everywhere else in this codebase
// -- the whole documented close_out family -- missing data must not be read
// as good news, so the reflex is that returning a number is safe and
// returning nothing is the risky shortcut. Here it is the other way round:
//
//   * REFUSING is SAFE. nullopt reaches observe_peg's data-gap branch, which
//     writes NOTHING: rt.above_bail HOLDS. A streak of 29 survives an entire
//     outage and completes when a real observation returns.
//
//   * OBSERVING a circular anchor is UNSAFE. The value lands exactly on
//     peg_target, so dev_pct is 0, so observe_peg takes the below-bail branch
//     and executes `rt.above_bail = 0` -- a confident all-clear manufactured
//     out of our own declared par, which also overwrites last_deviation_pct
//     with a reassuring 0.0% for the operator. With sustained_observations
//     at 30, no streak can ever complete while the outage lasts, and any
//     streak built beforehand is destroyed by the first graded cycle.
//
// A watcher that cannot source an independent anchor must go blind LOUDLY
// (the caller logs it at warn), not read its own input back and call it
// health.
//
// @param scaled_mid   MarketSnapshot::mid_price, mojo-scaled.
// @param scale        Mojos per unit (kMojosPerXch at every call site today).
// @param xch_base     True when the pair is XCH/<asset>, so the price is
//                     ASSET per XCH and the asset's USD value is
//                     usd_per_xch / mid. False for <asset>/XCH, where the
//                     price is XCH per asset and the value is the product.
// @param mid_valuation_grade  MarketSnapshot::mid_valuation_grade.
// @param bid_side_ok / ask_side_ok  Per-side anchor agreement.
// @param anchor       USD per XCH AND its provenance; usd must be finite and
//                     positive. Passed as a whole rather than as a bare
//                     double on purpose: changing the parameter's TYPE makes
//                     every call site a compile error, whereas adding a
//                     defaulted "is it circular" flag would let a new call
//                     site compile silently with "not circular" -- a
//                     fail-open default of exactly the shape this guard
//                     exists to close.
// @param judged_asset_id  The asset this observation is about.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::optional<double> peg_usd_observation(
    double                 scaled_mid,
    double                 scale,
    bool                   xch_base,
    bool                   mid_valuation_grade,
    bool                   bid_side_ok,
    bool                   ask_side_ok,
    const XchUsdAnchor&    anchor,
    std::string_view       judged_asset_id) noexcept
{
    // FIRST because it is a property of the ROUTE rather than of this tick's
    // book -- the cheapest and most general reason to decline, and true
    // before any snapshot field is even looked at.
    //
    // [review round 8] AND FOR NO OTHER REASON. An earlier version of this
    // comment claimed the ordering "changes what the operator is told". It
    // does not: every guard here returns a bare std::nullopt with no
    // discriminant, and the operator-facing announcement is made by the
    // CALLER, which re-evaluates xch_anchor_is_circular_for itself (once per
    // asset per heartbeat, rather than once per pair here). Moving this
    // check to the end of the function would change neither the value
    // returned nor any log line. Said plainly because this file's comments
    // are cited as contracts, and a rationale that is not true of the code
    // is what gets re-derived incorrectly later.
    if (xch_anchor_is_circular_for(anchor, judged_asset_id)) {
        return std::nullopt;
    }
    const double usd_per_xch = anchor.usd;
    if (!(usd_per_xch > 0.0) || !std::isfinite(usd_per_xch)) {
        return std::nullopt;
    }
    if (!(scaled_mid > 0.0) || !std::isfinite(scaled_mid)) {
        return std::nullopt;
    }
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        return std::nullopt;
    }
    if (!mid_valuation_grade) {
        return std::nullopt;
    }
    // Either side disqualified poisons the midpoint, whichever side it is:
    // the mid is the mean of the two, so one junk side moves it regardless
    // of which one.
    //
    // [review round 7, PR #134] THIS GUARD IS LIVE, and the reasoning that
    // said otherwise is worth recording because it was nearly right.
    //
    // When apply_mid_gate gated its WHOLE grade expression on the side
    // verdicts, grade implied both flags within a single published snapshot
    // -- engine.cpp passes all three from ONE get_market() call -- so the
    // `!mid_valuation_grade` check above would always have fired first and
    // this branch would have been unreachable at runtime while its unit
    // tests (which hand-supply the flags) stayed green.  That is the
    // vacuous-guard shape arriving through a fix rather than through a test.
    //
    // Round 7 removed that implication deliberately: the side verdicts now
    // gate the grade only when the book is TWO-SIDED and actually feeds the
    // mid.  On a ONE-SIDED book the published mid is the CEX/anchor value
    // alone, so it is graded even though the lone surviving touch may be
    // disqualified -- grade TRUE with ask_side_ok FALSE is a state the feed
    // now really publishes (BookSideQualityFeed.AOneSidedBookDoesNotForfeit-
    // APureCexValuation pins it).  This branch is what catches that case,
    // and refusing the observation is the fail-closed answer: a peg watcher
    // that cannot trust a touch should decline to observe, not guess.
    if (!bid_side_ok || !ask_side_ok) {
        return std::nullopt;
    }
    const double mid_units = scaled_mid / scale;
    if (!(mid_units > 0.0) || !std::isfinite(mid_units)) {
        return std::nullopt;
    }
    const double usd = xch_base ? usd_per_xch / mid_units
                                : usd_per_xch * mid_units;
    if (!std::isfinite(usd) || !(usd > 0.0)) {
        return std::nullopt;
    }
    return usd;
}

}  // namespace xop::risk

#endif  // XOP_RISK_PEG_SUSPENSION_HPP
