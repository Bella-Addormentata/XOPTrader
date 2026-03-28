// hedging.hpp -- Hedging framework for XOPTrader CHIA DEX market-making bot.
//
// Implements the 7-Layer Hedge Priority Stack from Section 9 of the strategy
// document.  Phase 1 covers layers 1-4 (all free, no external exchange
// dependency):
//
//   Layer 1 -- Inventory-based self-hedging (quote skewing)       FREE
//   Layer 2 -- Natural two-sided balancing (maximize NHE)         FREE
//   Layer 3 -- Portfolio-level netting across pairs               FREE
//   Layer 4 -- Statistical pairs hedging (correlated CATs)        FREE
//
// Key constraint inherited from the strategy:
//   ALL hedges respect the NEVER-SELL-AT-A-LOSS rule.
//   If a hedge position is underwater, hold it and widen quotes.
//   Patience is the ultimate hedge.
//
// Thread safety: HedgingManager is internally synchronized.
// The correlation table (Layer 4) is protected by a shared_mutex.
// All other methods are stateless (pure functions of their arguments).
// Multiple threads may call any method concurrently.
//
// Compliant with:
//   ISO/IEC 27001:2022  (deterministic, auditable hedge computations)
//   ISO/IEC 5055        (no unchecked arithmetic, no UB paths)
//   ISO/IEC 25000       (clear naming, documented formulas)

#ifndef XOP_RISK_HEDGING_HPP
#define XOP_RISK_HEDGING_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"
#include "xop/state.hpp"

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// SuggestedTrade -- a rebalancing instruction produced by the hedging engine.
//
// These are advisory only -- the execution layer decides whether and when
// to act on them.  No trade is ever forced; the NEVER-SELL-AT-A-LOSS rule
// is pre-checked before a suggestion is emitted.
// ---------------------------------------------------------------------------

struct SuggestedTrade {
    AssetId     sell_asset;      // asset to reduce
    AssetId     buy_asset;       // asset to increase
    Mojo        quantity;        // amount of sell_asset to convert (mojos)
    std::string reason;          // human-readable rationale for audit trail
    double      urgency;         // [0.0, 1.0] -- how urgently this should execute
};

// ---------------------------------------------------------------------------
// CorrelationEntry -- pairwise correlation between two CATs, used by
// Layer 4 (statistical pairs hedging) to identify natural offsets.
// ---------------------------------------------------------------------------

struct CorrelationEntry {
    AssetId asset_a;            // first asset identifier
    AssetId asset_b;            // second asset identifier
    double  correlation;        // Pearson correlation of returns, [-1, +1]
    double  half_life_blocks;   // mean-reversion half-life in blocks (for
                                // pair-trade sizing)
};

// ---------------------------------------------------------------------------
// HedgingManager -- Phase 1 hedging engine (Layers 1-4).
//
// Usage (per-block heartbeat):
//
//     double skew = hedger.compute_skew_adjustment(inventory_q, q_max, phi);
//     // ... apply skew to bid/ask ...
//
//     double nhe = hedger.compute_nhe(net_change, volume);
//     if (nhe < 0.70) { /* widen spreads or adjust pair weights */ }
//
//     auto exposure = hedger.compute_portfolio_net_exposure(positions);
//     auto trades   = hedger.suggest_rebalancing_trades(positions, targets);
//     // ... pass suggestions to execution layer ...
//
// ---------------------------------------------------------------------------

class HedgingManager {
public:
    /// Construct a HedgingManager.
    ///
    /// @param strat_cfg  Strategy configuration (accepted for API stability;
    ///                   not stored -- all methods are stateless or use only
    ///                   the correlation table).
    /// @param risk_cfg   Risk configuration (accepted for API stability;
    ///                   not stored).
    explicit HedgingManager(const StrategyConfig& strat_cfg,
                            const RiskConfig&     risk_cfg) noexcept;

    // ======================================================================
    // Layer 1 -- Inventory-based self-hedging (quote skewing)
    // ======================================================================

    /// Compute the inventory-driven skew adjustment that shifts the
    /// reservation price away from the overweight side.
    ///
    /// Formula (GLFT model, Section 5):
    ///     skew = phi * inventory_q / q_max
    ///
    /// The result is a signed value:
    ///   - Positive skew means we hold too much base (shift quotes down to
    ///     encourage selling base / discourage buying base).
    ///   - Negative skew means we hold too little base (shift quotes up to
    ///     encourage buying base).
    ///
    /// The caller applies the skew symmetrically:
    ///     ask = mid + half_spread - skew
    ///     bid = mid - half_spread - skew
    ///
    /// @param inventory_q  Current signed inventory in base asset.
    ///                     Positive = long base, negative = short base.
    /// @param q_max        Maximum tolerated inventory magnitude.
    ///                     Must be > 0; asserts in debug, clamps in release.
    /// @param phi          Skew strength parameter.
    ///                     0 = no skewing, 1 = aggressive.
    /// @return             Signed skew value (same units as price).
    [[nodiscard]]
    static double compute_skew_adjustment(double inventory_q,
                                          double q_max,
                                          double phi) noexcept;

    // ======================================================================
    // Layer 2 -- Natural two-sided balancing (maximize NHE)
    // ======================================================================

    /// Compute the Natural Hedge Efficiency metric.
    ///
    /// Formula (Section 9):
    ///     NHE = 1 - (|net_inventory_change| / total_volume)
    ///     Target: NHE > 0.70
    ///
    /// NHE measures how effectively two-sided flow cancels out:
    ///   - 1.0 = perfectly balanced (every buy matched by a sell)
    ///   - 0.0 = completely one-sided (all buys or all sells)
    ///
    /// @param net_inventory_change  Signed sum of all fills in the
    ///                              measurement window (buys positive,
    ///                              sells negative).
    /// @param total_volume          Sum of absolute fill quantities in
    ///                              the same window.  Must be > 0.
    /// @return                      NHE in [0.0, 1.0], or 0.0 if
    ///                              total_volume is zero.
    [[nodiscard]]
    static double compute_nhe(double net_inventory_change,
                              double total_volume) noexcept;

    /// Return the recommended minimum NHE target from the strategy.
    [[nodiscard]]
    static constexpr double nhe_target() noexcept { return 0.70; }

    // ======================================================================
    // Layer 3 -- Portfolio-level netting across pairs
    // ======================================================================

    /// Compute the net exposure per asset across all positions.
    ///
    /// Each entry in the returned map represents the total balance of a
    /// single asset across every trading pair.  This lets the caller
    /// identify assets with outsized long exposure that could be offset
    /// by activity in a correlated pair.
    ///
    /// @param positions  All current positions (from State::get_all_positions).
    /// @return           Map from AssetId to net balance (Mojo, always >= 0
    ///                   because individual positions are non-negative).
    [[nodiscard]]
    static std::unordered_map<AssetId, double>
    compute_portfolio_net_exposure(const std::vector<Position>& positions);

    // ======================================================================
    // Layer 4 -- Statistical pairs hedging (correlated CATs)
    // ======================================================================

    /// Set or update the pairwise correlation table used for pairs-based
    /// hedge suggestions.  Typically recalculated once per epoch
    /// (e.g. every 1000 blocks).
    ///
    /// @param correlations  Updated correlation entries.
    void set_correlations(std::vector<CorrelationEntry> correlations);

    /// Read a snapshot of the current correlation table.
    /// Returns a copy to avoid holding the lock across the caller's scope.
    [[nodiscard]]
    std::vector<CorrelationEntry> get_correlations() const;

    // ======================================================================
    // Rebalancing suggestions (combines Layers 1-4)
    // ======================================================================

    /// Analyse current positions against target allocations and produce
    /// a list of suggested rebalancing trades, ordered by urgency
    /// (highest first).
    ///
    /// The function enforces the NEVER-SELL-AT-A-LOSS rule: no suggestion
    /// will recommend selling an asset whose current market price is below
    /// its cost basis.  If a position is underwater, the suggestion is
    /// withheld and a diagnostic "hold" entry is emitted instead.
    ///
    /// @param positions  Current positions (from State::get_all_positions).
    /// @param targets    Target allocation per asset as a fraction of total
    ///                   portfolio value.  Keys are AssetIds, values are in
    ///                   [0, 1] and should sum to approximately 1.0.
    /// @param market     Current market snapshots for mark-to-market
    ///                   valuation.  Used to check cost-basis constraint.
    /// @return           Ordered list of suggested trades (may be empty if
    ///                   portfolio is within tolerance or all deviations
    ///                   are in underwater assets).
    [[nodiscard]]
    std::vector<SuggestedTrade> suggest_rebalancing_trades(
        const std::vector<Position>&                    positions,
        const std::unordered_map<AssetId, double>&      targets,
        const std::unordered_map<AssetId, Mojo>&        current_prices) const;

    // ======================================================================
    // Diagnostic -- hedge status summary
    // ======================================================================

    /// Compute a summary of current hedge effectiveness for monitoring.
    ///
    /// @param positions        All current positions.
    /// @param net_inv_change   Net inventory change in the current window.
    /// @param total_volume     Total volume in the current window.
    /// @return                 A human-readable diagnostic string.
    [[nodiscard]]
    static std::string hedge_summary(const std::vector<Position>& positions,
                                     double net_inv_change,
                                     double total_volume);

private:
    /// Pairwise correlation table for Layer 4.
    /// Guarded by its own mutex because set_correlations() is a writer.
    mutable std::shared_mutex mtx_correlations_;
    std::vector<CorrelationEntry> correlations_;

    /// Tolerance band for rebalancing: positions within this fraction of
    /// their target are considered "close enough" and produce no suggestion.
    /// 5% avoids churn from tiny deviations.
    static constexpr double kRebalanceTolerance = 0.05;

    // -- Internal helpers ---------------------------------------------------

    /// Compute total portfolio value (sum of all balances) in mojos.
    [[nodiscard]]
    static double total_portfolio_value(const std::vector<Position>& positions) noexcept;

    /// Check whether selling `asset_id` at `current_price` would violate
    /// the never-sell-at-loss rule given its cost_basis.
    [[nodiscard]]
    static bool would_sell_at_loss(const Position& pos, Mojo current_price) noexcept;

    /// Find the asset with the highest negative deviation from target
    /// (most overweight) that is NOT underwater.
    /// Returns an empty AssetId if no eligible overweight asset exists.
    [[nodiscard]]
    static AssetId find_best_sell_candidate(
        const std::vector<Position>&               positions,
        const std::unordered_map<AssetId, double>&  deviations,
        const std::unordered_map<AssetId, Mojo>&    current_prices);

    /// Find the asset with the highest positive deviation from target
    /// (most underweight).
    [[nodiscard]]
    static AssetId find_best_buy_candidate(
        const std::unordered_map<AssetId, double>& deviations);
};

}  // namespace xop

#endif  // XOP_RISK_HEDGING_HPP
