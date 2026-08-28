#pragma once
// ---------------------------------------------------------------------------
// usd_route -- [S32/S27 review] does a route from an asset to USD exist AT
// ALL, as a pure function of configuration?
//
// Extracted for the same reason drawdown_breaker.hpp and
// valuation_authority.hpp were, and with more cause: review pointed out that
// nothing in the suite ever invokes quote_usd_factor(), usd_per_xch() or
// asset_usd_pseudo_price(), because no test constructs an Engine.  So the
// decision that says "this asset can never be priced, write it off at $0"
// lived entirely inside a method no test could reach.
//
// WHY THE ANSWER IS NOT "IS IT IN AN ENABLED PAIR".  That was the first
// implementation and it is wrong in a way that fails toward paralysis rather
// than toward danger, which is why it survived review once.  An enabled
// CAT_A/CAT_B pair where neither leg is XCH and neither carries an enforced
// par gives quote_usd_factor() nothing to compute from: it returns 0.0 on
// every tick, forever.  Counting that as "has a pricing path" withholds the
// S32 write-off, so the asset degrades instead -- and because the cause is
// CONFIGURATION rather than a feed outage, the degradation never lifts.  The
// engine ends up permanently paused on a state that cannot resolve itself,
// which is exactly the outcome the write-off was introduced to avoid.
//
// So a route means reaching an ANCHOR, and there are only three:
//
//   1. The asset's own declared, enforced par.
//   2. An enabled pair against a wrapper that has one.
//   3. An enabled pair against XCH -- but only when XCH is itself anchored,
//      because otherwise the chain evaluates to 0 * mid.
//
// And XCH's own anchor is either the external CoinGecko quote or an enabled
// XCH pair against a declared par.  The CoinGecko half carries a condition
// that is easy to miss: usd_per_xch() gates the cached price through a
// freshness check that treats a non-positive or non-finite threshold as
// permanently stale -- deliberately, so a frozen feed cannot quote forever.
// A config with `cex_freshness_threshold_sec: 0` is therefore legal and
// anchorless.  The threshold is part of whether the anchor exists.
//
// [review] No longer SILENT, though it was when this was written:
// validate_usd_anchor() now declines the CoinGecko exemption for a
// non-positive threshold and warns at startup when enabled pairs have no
// other anchor. Describing the old behaviour as current is how a defect gets
// re-introduced by someone tidying up a warning they think is spurious.
// ---------------------------------------------------------------------------

#ifndef XOP_RISK_USD_ROUTE_HPP
#define XOP_RISK_USD_ROUTE_HPP

#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace xop::risk {

/// One enabled market, reduced to the only two things a route cares about.
struct RoutePair {
    std::string base_asset_id;
    std::string quote_asset_id;
};

/// The external XCH/USD feed, reduced likewise.
struct ExternalXchFeed {
    bool   enabled{false};
    bool   quotes_chia{false};
    double freshness_threshold_sec{0.0};

    /// Usable means the RUNTIME will accept it, not merely that it is
    /// configured.  A non-positive or non-finite threshold makes the revival
    /// gate reject the cached price on every tick.
    [[nodiscard]] bool usable() const noexcept {
        return enabled && quotes_chia
            && std::isfinite(freshness_threshold_sec)
            && freshness_threshold_sec > 0.0;
    }
};

/// Answers "does this asset have a declared, still-enforced USD par?".
/// A callback so this header need not know about PegRegistry.
using ParLookup = std::function<bool(const std::string&)>;

/// The two questions, which are NOT the same question.
///
/// `has_par` is "can this asset be valued at its declared par". `anchors_xch`
/// is the narrower "will usd_per_xch() accept a pair quoted in this asset as
/// its anchor", which additionally excludes `prefer_market_cross` assets:
/// BYC declares an enforced par AND prefers its market cross, so
/// `declared_usd_par("byc")` returns a value while `is_par_wrapper_quote()`
/// rejects it. Using one lookup for both made an `XCH/BYC` pair report XCH --
/// and therefore every XCH-routed asset -- as routable when usd_per_xch()
/// would have skipped that pair and returned 0.
struct ParLookups {
    ParLookup has_par;
    ParLookup anchors_xch;
};

/// Can XCH reach USD at all, for a DOWNSTREAM asset's conversion?
///
/// Narrow on purpose: this answers "will usd_per_xch() return a rate", which
/// is what an asset priced through XCH actually depends on, and that function
/// accepts only par WRAPPERS. Use :func:`xch_is_valuable` for the different
/// question of whether XCH itself can be priced.
[[nodiscard]] inline bool xch_is_anchored(
    const ExternalXchFeed& feed,
    const std::vector<RoutePair>& enabled_pairs,
    const ParLookups& pars)
{
    if (feed.usable()) return true;
    for (const auto& p : enabled_pairs) {
        if (p.base_asset_id == "xch" && pars.anchors_xch(p.quote_asset_id)) {
            return true;
        }
    }
    return false;
}

/// Can XCH ITSELF be priced?
///
/// [review] Broader than :func:`xch_is_anchored`, because the runtime is.
/// asset_usd_pseudo_price("xch") walks XCH's own base pairs and calls
/// quote_usd_factor(), which values a BYC quote through its market cross --
/// so an XCH/BYC pair CAN price XCH even though BYC, preferring its market,
/// is not accepted as usd_per_xch()'s par anchor. Using the narrow test here
/// classified XCH as having no route before that snapshot arrived, wrote it
/// off at $0, and let a partial book seed the drawdown high-water mark.
[[nodiscard]] inline bool xch_is_valuable(
    const ExternalXchFeed& feed,
    const std::vector<RoutePair>& enabled_pairs,
    const ParLookups& pars)
{
    if (feed.usable()) return true;
    for (const auto& p : enabled_pairs) {
        if (p.base_asset_id == "xch" && pars.has_par(p.quote_asset_id)) {
            return true;
        }
    }
    return false;
}

/// Can `asset_id` reach USD at all?
///
/// `enabled_pairs` must already be filtered to enabled markets: a disabled
/// pair is not a route, and passing the unfiltered list is the mistake this
/// signature exists to make obvious.
[[nodiscard]] inline bool asset_is_routable_to_usd(
    const std::string& asset_id,
    const ExternalXchFeed& feed,
    const std::vector<RoutePair>& enabled_pairs,
    const ParLookups& pars)
{
    if (asset_id == "xch") {
        return xch_is_valuable(feed, enabled_pairs, pars);
    }
    if (pars.has_par(asset_id)) {
        return true;   // priceable with no market at all
    }

    // Computed at most once, and only if some pair actually needs it.
    bool xch_checked = false;
    bool xch_ok      = false;

    for (const auto& p : enabled_pairs) {
        const bool is_base  = (p.base_asset_id  == asset_id);
        const bool is_quote = (p.quote_asset_id == asset_id);
        if (!is_base && !is_quote) continue;

        const std::string& other = is_base ? p.quote_asset_id
                                           : p.base_asset_id;
        // [review] Only when the asset is the BASE. quote_usd_factor() gives
        // USD per unit of the QUOTE, which prices the base through the mid.
        // The mirror image does not exist: it cannot invert an arbitrary
        // par-valued base to price the quote, so WRAP/CAT_A returned "route"
        // for CAT_A while the runtime returned 0 -- and CAT_A then degraded
        // permanently rather than taking the write-off.
        if (is_base && pars.has_par(other)) return true;
        if (other == "xch") {
            if (!xch_checked) {
                xch_ok      = xch_is_anchored(feed, enabled_pairs, pars);
                xch_checked = true;
            }
            if (xch_ok) return true;
        }
    }
    return false;
}

}  // namespace xop::risk

#endif  // XOP_RISK_USD_ROUTE_HPP
