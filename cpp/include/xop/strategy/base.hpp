// base.hpp -- Abstract strategy interface for the XOPTrader market-making engine.
//
// Every concrete strategy (Avellaneda-Stoikov, GLFT, future extensions) derives
// from StrategyBase and implements compute_quotes().  The engine invokes
// compute_quotes() once per block (~52 s) to obtain a two-sided quotation.
//
// The QuoteResult struct carries floating-point prices (in XCH, not mojos) so
// that the mathematical models operate in natural units.  Conversion to mojos
// happens in the execution layer (offer_manager) before creating on-chain
// offers, keeping the strategy layer free of integer-rounding concerns.
//
// ISO/IEC 27001:2022 -- no secrets handled here; pure algorithmic computation.
// ISO/IEC 5055       -- no raw pointers; virtual destructor for safe deletion.
// ISO/IEC 25000      -- clear naming; single-responsibility interface.

#ifndef XOP_STRATEGY_BASE_HPP
#define XOP_STRATEGY_BASE_HPP

#include <xop/config.hpp>
#include <xop/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// QuoteResult -- output of a single strategy evaluation.
//
// All prices are expressed as doubles in units of the quote asset per one unit
// of the base asset (e.g. wUSDC per XCH).  The execution layer converts to
// mojos at the point of offer creation.
// ---------------------------------------------------------------------------

struct QuoteResult {
    double bid_price;  // recommended bid price (quote asset per base)
    double ask_price;  // recommended ask price (quote asset per base)
    double bid_size;   // recommended bid quantity (base asset units)
    double ask_size;   // recommended ask quantity (base asset units)
    double spread_bps; // full spread in basis points = 10000*(ask-bid)/mid
};

// ---------------------------------------------------------------------------
// MarketRegime -- classification of current micro-structure regime derived
// from the variance-ratio test over a rolling block window.
//
//   MeanReverting : VR < 0.85  -- tighten spreads, reduce inventory shedding.
//   Random        : 0.85 <= VR <= 1.15 -- default parameterisation.
//   Momentum      : VR > 1.15 -- widen spreads, aggressive inventory shedding.
// ---------------------------------------------------------------------------

enum class MarketRegime : std::uint8_t {
    MeanReverting = 0,
    Random        = 1,
    Momentum      = 2
};

/// Human-readable label for logging / Prometheus metrics.
inline const char* to_string(MarketRegime r) noexcept {
    switch (r) {
        case MarketRegime::MeanReverting: return "MeanReverting";
        case MarketRegime::Random:        return "Random";
        case MarketRegime::Momentum:      return "Momentum";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// RegimeInfo -- result of the variance-ratio regime detector.
// ---------------------------------------------------------------------------

struct RegimeInfo {
    MarketRegime regime;        // current classification
    double       variance_ratio; // raw VR statistic (1.0 = random walk)
    double       spread_mult;   // multiplicative adjustment for half-spread
    double       skew_mult;     // multiplicative adjustment for inventory skew
};

// ---------------------------------------------------------------------------
// StrategyBase -- abstract interface that all market-making strategies must
// implement.
//
// The engine holds a unique_ptr<StrategyBase> and calls:
//   1. update_market_data() -- push the latest prices and fills.
//   2. compute_quotes()     -- obtain the optimal two-sided quote for this block.
//
// Strategies are stateful: they retain rolling price histories, regime state,
// and internal estimators between blocks.
// ---------------------------------------------------------------------------

class StrategyBase {
public:
    virtual ~StrategyBase() = default;

    // -- Core interface -----------------------------------------------------

    /// Compute a two-sided quote for the current block.
    ///
    /// @param mid           Mid-price (quote asset per base asset), typically
    ///                      the average of the best bid and best ask from the
    ///                      aggregated order book.
    /// @param sigma         Annualised volatility of the base asset (dimensionless,
    ///                      e.g. 0.05 for 5% annual vol).
    /// @param q             Current net inventory in base-asset units.
    ///                      Positive  = long (we own more base than target).
    ///                      Negative  = short (we own less base than target).
    /// @param block_height  Current Chia block height.  Used together with
    ///                      the horizon to compute remaining time (tau).
    ///
    /// @return QuoteResult  The recommended bid/ask prices, sizes, and spread.
    virtual QuoteResult compute_quotes(double mid,
                                       double sigma,
                                       double q,
                                       BlockHeight block_height) = 0;

    // -- Market data feed ---------------------------------------------------

    /// Push a new mid-price observation (called once per block).
    /// The strategy stores these in a rolling buffer for regime detection.
    ///
    /// @param mid           Mid-price at this block.
    /// @param block_height  Block height corresponding to the observation.
    virtual void update_price(double mid, BlockHeight block_height) = 0;

    // -- Accessors ----------------------------------------------------------

    /// Retrieve the most recent regime classification.
    virtual RegimeInfo current_regime() const = 0;

    /// Return the strategy name (e.g. "AvellanedaStoikov", "GLFT") for
    /// logging and metrics labelling.
    virtual const std::string& name() const = 0;

    // -- No-loss constraint -------------------------------------------------

    /// Optionally set the cost basis so the strategy can enforce the
    /// never-sell-at-loss constraint.  The cost basis is the weighted-average
    /// acquisition price per unit of the base asset.
    ///
    /// @param cost_basis     Average cost (quote per base).
    /// @param min_margin_bps Minimum margin above cost basis in basis points.
    virtual void set_cost_basis(double cost_basis,
                                double min_margin_bps) = 0;

protected:
    // Non-copyable, non-movable -- lifetime managed via unique_ptr.
    StrategyBase() = default;
    StrategyBase(const StrategyBase&) = delete;
    StrategyBase& operator=(const StrategyBase&) = delete;
};

}  // namespace xop

#endif  // XOP_STRATEGY_BASE_HPP
