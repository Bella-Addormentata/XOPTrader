// peg_registry.hpp -- Asset-level declaration of what a coin is pegged TO.
//
// [PEG 2026-08-26]
//
// WHY THIS EXISTS
// ---------------
// Peg identity was a string comparison, repeated.  Fifteen sites across
// engine.cpp, pnl.cpp, arbitrage.cpp, market_allocator.cpp and
// feed_listings.hpp each asked some variant of
//
//     if (quote == "wUSDC.b" || quote == "wUSDC" || quote == "USDS")
//
// and each independently decided that meant "worth exactly $1".  Two
// failures in two days came out of that shape:
//
//   * BYC's depeg watch existed only because a PAIR carried
//     is_stablecoin, so disabling BYC/wUSDC.b removed monitoring from the
//     very asset that had just become the book's dollar anchor.  A peg is
//     a property of an ASSET; hanging it off a pair means it disappears
//     when that pair does.
//   * quote_usd_factor's BYC branch falls through to `return 1.0`, so an
//     asset whose issuer had announced it was being wound down kept
//     marking the entire portfolio at par.
//
// PEGGED TO WHAT
// --------------
// The old code could not express the question, let alone the answer: it
// hardcoded "pegged" to mean "worth one US dollar".  A EUR- or JPY-pegged
// asset is not $1.00, and treating it as such is the same class of error
// as treating a broken peg as intact -- silent, and wrong by a factor
// nobody wrote down.  So a peg here is a pair: an amount, and the CURRENCY
// that amount is denominated in.
//
// Converting a non-USD peg into USD needs an FX rate, which is a market
// observation like any other and therefore may be missing.  This header
// deliberately does NOT fetch it.  usd_value() takes the rate as an
// argument and returns nullopt when it is absent, so a missing EURUSD
// produces "no valuation" rather than a silent 1:1 substitution -- the
// same discipline the engine already applies to a missing mid.
//
// This header is pure: no I/O, no config parsing, no engine types.  The
// part that decides what an asset is worth is the part that should be
// exhaustively testable.

#ifndef XOP_PEG_REGISTRY_HPP
#define XOP_PEG_REGISTRY_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

/// An asset the operator has declared to be pegged, and to what.
struct PeggedAsset {
    /// Canonical asset id (CAT tail hex, or "xch").  The identity that
    /// cannot be renamed out from under us.
    std::string asset_id;

    /// Display symbol ("wUSDC.b").  Convenience for logs and the GUI only
    /// -- never the lookup key, because symbols collide and get reused.
    std::string symbol;

    /// ISO-4217-ish code for what the peg is denominated in: "USD",
    /// "EUR", "JPY".  Free text on purpose; a closed enum would need a
    /// code change to add a currency, which is exactly the rigidity this
    /// registry exists to remove.
    std::string peg_currency{"USD"};

    /// Units of peg_currency one unit of the asset is meant to be worth.
    /// Almost always 1.0, but a coin pegged to 100 JPY is expressible.
    double peg_target{1.0};

    /// Deviation from peg_target at which to warn, as a percentage.
    /// Must be strictly positive: the classify() comparison is inclusive
    /// (to agree with DepegDetector), so a warn_pct of 0 would make an
    /// observation sitting exactly ON the peg classify as Warn and no
    /// healthy peg could ever reach Holding.  Matches the per-pair
    /// validation in config.cpp.
    double warn_pct{2.0};

    /// Deviation at which the asset is considered broken rather than
    /// wobbling.  Must exceed warn_pct or the warning can never fire
    /// first; is_coherent() checks this.
    double bail_pct{10.0};

    /// Consecutive observations beyond bail_pct before acting.  A single
    /// bad print is noise on these books; the engine's own outlier
    /// filtering rejects up to 73% of offers on a thin pair.
    std::uint32_t sustained_observations{30};

    /// When false the asset is still DECLARED pegged but its peg is not
    /// enforced -- useful to keep a wound-down asset's identity on record
    /// without letting it mark the book at par.
    bool enforce{true};

    /// Prefer a live market cross over the declared par when one is
    /// available.
    ///
    /// This is the economic difference between the two kinds of pegged
    /// asset we hold.  A FIAT-COLLATERALISED WRAPPER (wUSDC.b, wUSDC,
    /// USDS) is par by construction -- one unit is a claim on one dollar
    /// held by the issuer -- so a market print that disagrees is evidence
    /// about the ISSUER, not about the value of the claim, and should not
    /// be used to mark the book.  A CDP STABLECOIN (BYC) has no such
    /// claim: its dollar value is whatever the market says, and a live
    /// cross is a better estimate than the target it aims at.
    ///
    /// Set false for wrappers, true for market-determined pegs.
    bool prefer_market_cross{false};

    [[nodiscard]] bool is_coherent() const noexcept {
        // Finiteness is checked explicitly: +inf satisfies `> 0.0` and an
        // infinite bail_pct satisfies `> warn_pct`, so a declaration built
        // from a typo could otherwise return an infinite USD factor into
        // llround.  Matches the finite check PairConfig::peg_target already
        // gets in config.cpp.
        return !asset_id.empty()
            && !peg_currency.empty()
            && std::isfinite(peg_target) && peg_target > 0.0
            && std::isfinite(warn_pct)   && warn_pct > 0.0
            && std::isfinite(bail_pct)   && bail_pct > warn_pct;
    }
};

/// Outcome of checking an observed rate against a declared peg.
enum class PegStatus {
    /// No declaration for this asset -- not an error, just not pegged.
    NotPegged,
    /// Declared, and the observation sits within warn_pct.
    Holding,
    /// Beyond warn_pct but within bail_pct.
    Warn,
    /// Beyond bail_pct.
    Broken,
    /// Declared, but no observation was supplied.  NOT the same as
    /// Holding: absence of evidence reads identically to a healthy peg if
    /// the two are conflated, which is how a real depeg went unremarked
    /// for hours on 2026-08-25.
    Unobserved,
};

[[nodiscard]] inline const char* to_string(PegStatus s) noexcept {
    switch (s) {
        case PegStatus::NotPegged:  return "not_pegged";
        case PegStatus::Holding:    return "holding";
        case PegStatus::Warn:       return "warn";
        case PegStatus::Broken:     return "broken";
        case PegStatus::Unobserved: return "unobserved";
    }
    return "unknown";
}

/// Declared pegs, looked up by asset id.
///
/// Deliberately dumb: it stores what the operator declared and answers
/// questions about it.  It does not fetch prices, does not decide policy,
/// and does not know what an engine is.
class PegRegistry {
public:
    PegRegistry() = default;

    explicit PegRegistry(std::vector<PeggedAsset> assets) {
        for (auto& a : assets) {
            add(std::move(a));
        }
    }

    /// Register a declaration.  Returns false (and changes nothing) if it
    /// is incoherent -- a caller loading config should surface that rather
    /// than silently run with a half-declared peg.
    bool add(PeggedAsset asset) {
        if (!asset.is_coherent()) {
            return false;
        }
        // A duplicate declaration is a config error, not an update.  Last
        // -write-wins would let a second entry silently replace the first
        // asset's SAFETY POLICY -- its enforce flag, its bail threshold --
        // and the operator would have no way to see which one was in
        // effect.  Refuse; the parser turns this into a loud startup
        // failure.
        if (by_asset_id_.find(asset.asset_id) != by_asset_id_.end()) {
            return false;
        }
        const std::string key = asset.asset_id;
        by_asset_id_[key] = std::move(asset);
        return true;
    }

    [[nodiscard]] const PeggedAsset* find(const std::string& asset_id) const {
        auto it = by_asset_id_.find(asset_id);
        return it == by_asset_id_.end() ? nullptr : &it->second;
    }

    /// Symbol lookup, for logs and UI.  Linear and case-sensitive: symbols
    /// are not unique and must never drive valuation, so this is
    /// deliberately not the fast path.
    [[nodiscard]] const PeggedAsset* find_by_symbol(const std::string& symbol) const {
        for (const auto& [_, a] : by_asset_id_) {
            if (a.symbol == symbol) return &a;
        }
        return nullptr;
    }

    /// True only for an asset that is declared AND enforced.  Call sites
    /// that used to ask `quote == "wUSDC.b"` should ask this instead.
    [[nodiscard]] bool is_pegged(const std::string& asset_id) const {
        const auto* a = find(asset_id);
        return a != nullptr && a->enforce;
    }

    [[nodiscard]] std::size_t size() const noexcept { return by_asset_id_.size(); }
    [[nodiscard]] bool empty() const noexcept { return by_asset_id_.empty(); }

    [[nodiscard]] std::vector<const PeggedAsset*> all() const {
        std::vector<const PeggedAsset*> out;
        out.reserve(by_asset_id_.size());
        for (const auto& [_, a] : by_asset_id_) out.push_back(&a);
        std::sort(out.begin(), out.end(),
                  [](const PeggedAsset* l, const PeggedAsset* r) {
                      return l->asset_id < r->asset_id;
                  });
        return out;
    }

    /// Par value in USD for one unit of the asset, per its declaration.
    ///
    /// @param asset_id  Asset to value.
    /// @param fx_to_usd Rate from the asset's peg_currency into USD.  Pass
    ///                  std::nullopt when unknown.  Ignored (and not
    ///                  required) when the peg is already in USD.
    ///
    /// @return nullopt when the asset is not declared, is not enforced, or
    ///         is pegged to a currency whose USD rate was not supplied.
    ///         Callers must treat nullopt as "no valuation available" and
    ///         fall through to whatever they already do for a missing
    ///         price -- NOT substitute 1.0.
    [[nodiscard]] std::optional<double> usd_par_value(
        const std::string& asset_id,
        std::optional<double> fx_to_usd = std::nullopt) const
    {
        const auto* a = find(asset_id);
        if (a == nullptr || !a->enforce) {
            return std::nullopt;
        }
        if (a->peg_currency == "USD") {
            return a->peg_target;
        }
        if (!fx_to_usd.has_value() || !std::isfinite(*fx_to_usd)
            || !(*fx_to_usd > 0.0)) {
            return std::nullopt;
        }
        const double v = a->peg_target * *fx_to_usd;
        // Two individually finite values can still overflow.  An infinite
        // valuation is not a valuation.
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
        return v;
    }

    /// Grade an observed rate against the declared peg.
    ///
    /// @param observed  Observed value of one unit of the asset, expressed
    ///                  in the SAME currency as peg_currency.  Converting
    ///                  into that currency is the caller's job, because
    ///                  only the caller knows the provenance of its own
    ///                  observation.
    [[nodiscard]] PegStatus classify(
        const std::string& asset_id,
        std::optional<double> observed) const
    {
        const auto* a = find(asset_id);
        if (a == nullptr || !a->enforce) {
            return PegStatus::NotPegged;
        }
        if (!observed.has_value() || !std::isfinite(*observed)
            || !(*observed > 0.0)) {
            return PegStatus::Unobserved;
        }
        const double dev_pct =
            std::abs(*observed - a->peg_target) / a->peg_target * 100.0;
        // INCLUSIVE, matching DepegDetector (depeg_detector.hpp:121,131 both
        // use >=) and the fields' own "deviation at which to warn/bail"
        // contract.  With `>` a deviation sitting exactly on a configured
        // limit lands in the lower band, so the two components would
        // disagree about whether that limit had been breached -- precisely
        // the inconsistency this registry exists to remove.
        if (dev_pct >= a->bail_pct)  return PegStatus::Broken;
        if (dev_pct >= a->warn_pct)  return PegStatus::Warn;
        return PegStatus::Holding;
    }

    /// Signed deviation from par, as a percentage.  Negative means the
    /// asset is trading BELOW its peg.  nullopt when not applicable.
    [[nodiscard]] std::optional<double> deviation_pct(
        const std::string& asset_id,
        std::optional<double> observed) const
    {
        // Must agree with classify() on WHICH assets have a deviation at
        // all: an unenforced peg classifies as NotPegged, so reporting a
        // deviation for it would have the two functions disagreeing about
        // whether the asset is pegged.  Non-finite observations are
        // rejected here for the same reason they are in classify.
        const auto* a = find(asset_id);
        if (a == nullptr || !a->enforce
            || !observed.has_value() || !std::isfinite(*observed)
            || !(*observed > 0.0)) {
            return std::nullopt;
        }
        return (*observed - a->peg_target) / a->peg_target * 100.0;
    }

private:
    std::unordered_map<std::string, PeggedAsset> by_asset_id_;
};

}  // namespace xop

#endif  // XOP_PEG_REGISTRY_HPP
