// ---------------------------------------------------------------------------
// par_anchor.hpp -- a declared peg as a fair-value USD anchor.
//
// [PARANCHOR 2026-08-30]
//
// WHY THIS EXISTS
// ---------------
// XCH/BYC went fair-value blind on 2026-08-30: BYC has no CoinGecko
// listing, its only market edge (the BYC/wUSDC.b book) left the graph
// when that pair was disabled after the bridge compromise, and the
// TibetSwap AMM feed was down.  Yet the operator had DECLARED BYC pegged
// to USD in pegged_assets -- a declaration the anchor graph never read,
// because Step B built anchors exclusively from CoinGecko feed listings.
// The result was QUOTING BLIND widening on a pair whose quote leg the
// config already valued.
//
// UNIVERSAL BY CONSTRUCTION
// -------------------------
// Nothing here knows about BYC, or about dollars.  The registry supplies
// (asset, peg currency, target); the caller supplies the FX rate into USD
// for non-USD currencies; and PegRegistry::usd_par_value() already
// refuses to invent a missing EURUSD -- absence of an FX rate yields
// nullopt, never a silent 1:1.  A JPY peg with target 100 and a live
// JPYUSD rate anchors exactly as a USD peg does.
//
// SUSPENSION GATES IT
// -------------------
// wUSDC.b's par is declared too, and anchoring it at $1 mid-depeg would
// mark the graph with exactly the lie the peg-suspension machinery
// exists to catch.  A suspended par contributes nothing.
//
// FALLBACK ONLY ([review] the sigma-inversion finding)
// -----------------------------------------------------
// A par anchor participates in a pair's solve ONLY when the solve finds
// no path without it (engine Step D retries a failed solve with the par
// set added).  This is what "the par is the fallback" was always meant
// to mean: with ANY live market evidence present the par contributes
// nothing, so a 100-150bps declaration can never out-vote a wide but
// honest market observation -- and a depegged asset's par can only
// anchor a pair that nothing else can price at all, where the
// alternative was no price rather than a better one.
//
// SIBLINGS SHARE A NODE ([review] the conflation finding)
// -------------------------------------------------------
// Leg canonicalisation strips the ".b" bridge suffix, so wUSDC.b and
// wUSDC are ONE graph node with TWO asset ids, two declarations, and
// two suspension latches.  par_anchor_consensus() therefore requires
// every asset id mapping onto the leg to independently justify the SAME
// par: one suspended, undeclared, or disagreeing sibling and the node
// gets nothing.  Anything laxer lets a healthy sibling anchor a broken
// one at par -- the exact bypass the review demonstrated.
//
// SIGMA IS THE TRUST DIAL
// -----------------------
// Two kinds of pegged asset, two error bars (config-set):
//   * wrapper (prefer_market_cross == false): par by construction -- one
//     unit is a claim on one currency unit held by the issuer.
//   * market-determined (prefer_market_cross == true): par is an aim the
//     market usually honours (BYC 7-day VWAP 1.001) but does not owe us.
// Both must stay small enough that feed-sigma (+) par-sigma in quadrature
// clears fair_value_max_sigma_bps, or the anchor is a silent no-op --
// config.cpp warns about that combination at load.
// ---------------------------------------------------------------------------

#ifndef XOP_EXECUTION_PAR_ANCHOR_HPP
#define XOP_EXECUTION_PAR_ANCHOR_HPP

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "xop/execution/fair_value_solver.hpp"
#include "xop/peg_registry.hpp"

namespace xop {

/// The anchor a declared peg justifies for one graph leg, or nullopt.
///
/// @param registry    Declared pegs (asset-id keyed).
/// @param asset_id    The leg's canonical asset id ("xch" or CAT tail).
/// @param leg         The leg's canonical GRAPH symbol ("byc") -- the
///                    solver's naming convention, distinct from asset_id.
/// @param suspended   The runtime peg-suspension latch for this asset.
/// @param fx_to_usd   USD per one unit of the declaration's peg currency.
///                    Ignored for USD pegs; required (else nullopt) for
///                    every other currency.
/// @param wrapper_sigma_bps  Error bar for prefer_market_cross == false.
/// @param market_sigma_bps   Error bar for prefer_market_cross == true.
[[nodiscard]] inline std::optional<fv::Anchor> par_anchor(
    const PegRegistry& registry,
    const std::string& asset_id,
    const std::string& leg,
    bool suspended,
    std::optional<double> fx_to_usd,
    double wrapper_sigma_bps,
    double market_sigma_bps)
{
    if (suspended || leg.empty()) {
        return std::nullopt;
    }
    const PeggedAsset* a = registry.find(asset_id);
    if (a == nullptr) {
        return std::nullopt;
    }
    // usd_par_value owns enforce, FX presence, and finiteness -- including
    // the refusal to treat a missing FX rate as 1.0.
    const auto par = registry.usd_par_value(asset_id, fx_to_usd);
    if (!par) {
        return std::nullopt;
    }
    const double sigma =
        a->prefer_market_cross ? market_sigma_bps : wrapper_sigma_bps;
    if (!std::isfinite(sigma) || !(sigma > 0.0)) {
        return std::nullopt;
    }
    return fv::Anchor{leg, *par, sigma};
}

/// One asset id mapping onto a graph leg, with its runtime state.
struct ParLegInput {
    std::string asset_id;
    bool suspended{false};
    std::optional<double> fx_to_usd{};
};

/// The par anchor a CONFLATED leg justifies: every asset id sharing the
/// leg must independently yield the same USD par, or the leg gets
/// nothing.  The surviving anchor carries the WIDEST sigma among the
/// agreeing declarations -- consensus can only lower confidence.
[[nodiscard]] inline std::optional<fv::Anchor> par_anchor_consensus(
    const PegRegistry& registry,
    const std::string& leg,
    const std::vector<ParLegInput>& assets,
    double wrapper_sigma_bps,
    double market_sigma_bps)
{
    if (assets.empty()) {
        return std::nullopt;
    }
    std::optional<fv::Anchor> consensus;
    for (const auto& in : assets) {
        auto a = par_anchor(registry, in.asset_id, leg, in.suspended,
                            in.fx_to_usd, wrapper_sigma_bps,
                            market_sigma_bps);
        if (!a) {
            return std::nullopt;
        }
        if (consensus) {
            if (std::abs(consensus->usd_price - a->usd_price)
                > 1e-9 * consensus->usd_price) {
                return std::nullopt;  // siblings disagree about the par
            }
            consensus->sigma_bps =
                std::max(consensus->sigma_bps, a->sigma_bps);
        } else {
            consensus = std::move(a);
        }
    }
    return consensus;
}

}  // namespace xop

#endif  // XOP_EXECUTION_PAR_ANCHOR_HPP
