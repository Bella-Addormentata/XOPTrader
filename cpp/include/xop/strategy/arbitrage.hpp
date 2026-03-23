// arbitrage.hpp -- Arbitrage detection and execution for XOPTrader CHIA DEX
//                  market-making bot.
//
// Implements four arbitrage strategies from CHIA_MARKET_MAKER_STRATEGY.md S10:
//
//   1. CEX-DEX   -- exploit price dislocations between centralised exchanges
//                   (OKX, MEXC, Gate.io) and CHIA DEX venues.  Primary
//                   opportunity due to the 1000:1 volume ratio (~$2.4M/day CEX
//                   vs ~$2K/day DEX).  Expected edge: 50-200 bps per cycle.
//
//   2. Cross-DEX -- dexie order-book vs TibetSwap AMM price divergences.
//                   Atomic: take on one venue, make on the other.
//
//   3. Triangular -- XCH -> CAT_A -> CAT_B -> XCH multi-hop routes.
//                    Profitable when the product of exchange rates along a
//                    three-hop cycle exceeds 1.0 by more than cumulative fees.
//
//   4. Cross-Bridge -- wUSDC (from ETH via warp.green) vs wUSDC.b (from Base).
//                      Same underlying USD stablecoin, different CHIA asset IDs.
//                      Arbitrage when price diverges beyond bridge costs.
//
// TibetSwap AMM math (constant-product with 0.7% fee):
//
//   output = (output_reserve * input * 993) / (input_reserve * 1000 + input * 993)
//
//   The 993/1000 factor encodes the 0.7% taker fee:  1000 - 7 = 993.
//   The fee numerator (993) is called INVERSE_FEE in the on-chain Chialisp code.
//
// Thread safety: NOT thread-safe.  The engine serialises arbitrage scans via
//   the per-block heartbeat.  Each scan_*() call is a pure function of its
//   inputs (no internal mutable state beyond configuration).
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets; pure market-data analysis
//   ISO/IEC 5055       -- no raw pointers; bounds-checked containers
//   ISO/IEC 25000      -- clear naming, comprehensive documentation
//   ISO/IEC JTC 1/SC 22 -- standard-conforming C++17

#ifndef XOP_STRATEGY_ARBITRAGE_HPP
#define XOP_STRATEGY_ARBITRAGE_HPP

#include <xop/types.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace xop {

// ---------------------------------------------------------------------------
// ArbitrageType -- which category an opportunity belongs to.
// ---------------------------------------------------------------------------

enum class ArbitrageType : std::uint8_t {
    CexDex      = 0,  // CEX vs DEX price dislocation
    CrossDex    = 1,  // dexie order book vs TibetSwap AMM
    Triangular  = 2,  // multi-hop cycle (XCH -> A -> B -> XCH)
    CrossBridge = 3   // wUSDC (ETH) vs wUSDC.b (Base)
};

/// Human-readable label for logging and metrics.
inline const char* to_string(ArbitrageType t) noexcept {
    switch (t) {
        case ArbitrageType::CexDex:      return "CexDex";
        case ArbitrageType::CrossDex:    return "CrossDex";
        case ArbitrageType::Triangular:  return "Triangular";
        case ArbitrageType::CrossBridge: return "CrossBridge";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// ArbitrageOpportunity -- a single detected arbitrage opportunity with full
//                         cost accounting and confidence assessment.
//
// All prices are in floating-point (quote-per-base) for human readability.
// Conversion to mojos occurs in the execution layer.
// ---------------------------------------------------------------------------

struct ArbitrageOpportunity {
    ArbitrageType type;              // category

    std::string   description;       // human-readable route or pair description
                                     //   e.g. "XCH/wUSDC: buy dexie, sell OKX"
                                     //   or   "XCH -> SBX -> DBX -> XCH"

    std::string   buy_venue;         // venue to buy on (e.g. "dexie", "tibetswap")
    std::string   sell_venue;        // venue to sell on (e.g. "OKX", "dexie")

    double        buy_price;         // price at which we buy (quote per base)
    double        sell_price;        // price at which we sell (quote per base)

    double        edge_bps;          // raw edge in basis points before costs
                                     //   = 10000 * (sell_price - buy_price) / buy_price

    double        estimated_profit;  // net profit after all fees, slippage, and
                                     //   bridge costs (in quote-asset units, e.g. USD)

    double        confidence;        // 0.0-1.0 probability assessment that the
                                     //   opportunity will still exist when our
                                     //   transaction settles

    std::uint32_t urgency_blocks;    // estimated blocks before the opportunity
                                     //   closes (lower = more urgent)
};

// ---------------------------------------------------------------------------
// Input data structures -- callers populate these from market data feeds.
// ---------------------------------------------------------------------------

/// Snapshot of a dexie order-book for a single pair.
struct DexieBookSnapshot {
    std::string pair_name;     // e.g. "XCH/wUSDC"
    double      best_bid;      // highest bid (quote per base)
    double      best_ask;      // lowest ask  (quote per base)
    double      bid_depth;     // total bid volume (base units) at best bid
    double      ask_depth;     // total ask volume (base units) at best ask
    double      mid_price;     // (best_bid + best_ask) / 2
};

/// TibetSwap AMM pool reserves for a single pair.
struct TibetSwapReserves {
    std::string pair_name;          // e.g. "XCH/wUSDC"
    std::int64_t xch_reserve;      // XCH-side reserve (mojos)
    std::int64_t token_reserve;    // token-side reserve (smallest unit)
    std::uint32_t fee_bps;         // pool fee in basis points (default 7 = 0.07%
                                   //   per leg; total round-trip 0.7%)
                                   //   NOTE: TibetSwap v2 takes 0.7% total, so
                                   //   the INVERSE_FEE factor is 993/1000 for a
                                   //   single swap direction.
};

/// CEX price for a single asset pair, fetched from OKX / MEXC / Gate.io.
struct CexPrice {
    std::string pair_name;     // e.g. "XCH/USDT"
    std::string exchange;      // e.g. "OKX"
    double      bid;           // best bid
    double      ask;           // best ask
    double      mid;           // (bid + ask) / 2
    double      volume_24h;    // 24h volume in base-asset units
};

/// All pair prices for triangular-arb scanning.
/// Key: "BASE/QUOTE" (e.g. "XCH/SBX"), Value: mid-price (quote per base).
using PairPriceMap = std::unordered_map<std::string, double>;

// ---------------------------------------------------------------------------
// ArbitrageConfig -- tunable parameters for each arbitrage strategy.
// ---------------------------------------------------------------------------

struct ArbitrageConfig {
    // -- CEX-DEX arbitrage ---------------------------------------------------

    double cex_dex_min_edge_bps{50.0};     // Minimum raw edge (bps) to flag an
                                           //   opportunity.  Default 50 bps per
                                           //   strategy doc.

    double cex_dex_max_edge_bps{500.0};    // Reject edges above this threshold as
                                           //   likely stale / erroneous data.

    double cex_settlement_seconds{600.0};  // Typical CEX withdrawal + bridge time.
    double dex_settlement_seconds{52.0};   // One CHIA block (~52 s).

    double cex_fee_bps{10.0};              // CEX taker fee (bps).  OKX VIP0 = 10 bps.
    double bridge_fee_bps{0.0};            // Bridge fee (bps).  warp.green = 0 for
                                           //   XCH-native, but non-zero for bridged
                                           //   tokens.

    // -- Cross-DEX arbitrage -------------------------------------------------

    double cross_dex_min_edge_bps{15.0};   // Lower threshold than CEX-DEX because
                                           //   both legs are on-chain and atomic.

    double tibetswap_fee_bps{70.0};        // TibetSwap v2 swap fee: 0.7% = 70 bps.
    double dexie_fee_bps{0.0};             // dexie regular offers: 0%.
    double dexie_combined_swap_fee_bps{100.0};  // dexie Combined Swap: 1%.

    // -- Triangular arbitrage ------------------------------------------------

    double triangular_min_profit_bps{30.0}; // Minimum net profit (bps) after ALL
                                            //   fees along the route.

    double triangular_slippage_bps{10.0};   // Per-leg slippage estimate (bps).
    double triangular_per_leg_fee_bps{5.0}; // Blockchain settlement fee per hop.

    std::uint32_t triangular_max_legs{3};   // Maximum hops in a route (3 for
                                            //   triangular, extensible to 4).

    // -- Cross-Bridge arbitrage ----------------------------------------------

    double cross_bridge_min_edge_bps{20.0}; // Minimum edge for wUSDC vs wUSDC.b.
    double bridge_cost_bps{15.0};           // warp.green bridge round-trip cost.

    // -- General parameters --------------------------------------------------

    double max_position_size{100.0};        // Maximum base-asset units per arb
                                            //   trade.  Limits exposure.

    double default_confidence{0.75};        // Base confidence when we cannot
                                            //   compute a better estimate.

    std::uint32_t default_urgency_blocks{5};// Default blocks before opportunity
                                            //   closes if we cannot estimate.

    double min_confidence_threshold{0.40};  // Discard opportunities below this
                                            //   confidence level.
};

// ---------------------------------------------------------------------------
// TibetSwap AMM math utilities.
//
// The constant-product invariant with a 0.7% fee is:
//
//   output = (output_reserve * input * INVERSE_FEE)
//          / (input_reserve * 1000 + input * INVERSE_FEE)
//
// where INVERSE_FEE = 1000 - fee_per_mille.
//
// For the standard 0.7% fee:  INVERSE_FEE = 1000 - 7 = 993.
//
// IMPORTANT: The 993/1000 split means 0.7% is taken from the INPUT before
// the swap, not from the output.  The pool sees only 99.3% of the input,
// and the constant-product formula x*y=k operates on that reduced amount.
// ---------------------------------------------------------------------------

namespace tibet {

/// Compute the exact integer output amount for a given input into a
/// constant-product AMM pool.
///
/// @param input_amount    Amount of input token (smallest units).
/// @param input_reserve   Pool reserve of the input token.
/// @param output_reserve  Pool reserve of the output token.
/// @param fee_bps         Pool fee in basis points (default 7 bps per the
///                        TibetSwap on-chain code; 7 bps * 10 = 70 bps
///                        effective because the formula uses
///                        fee_per_mille = fee_bps / 10... HOWEVER, the
///                        on-chain INVERSE_FEE is 993, meaning the fee
///                        deducted is 7/1000 = 0.7%, i.e. 70 bps).
///                        To keep the interface clean we accept fee_bps=7
///                        to mean the INVERSE_FEE is (1000 - fee_bps) = 993,
///                        matching the on-chain Chialisp exactly.
///
/// @return Output amount (smallest units), or 0 if inputs are invalid.
///
/// Formula:
///   inverse_fee = 1000 - fee_bps
///   numerator   = output_reserve * input_amount * inverse_fee
///   denominator = input_reserve * 1000 + input_amount * inverse_fee
///   output      = numerator / denominator   (integer division, rounds down)
///
/// Rounding down favours the pool, which is correct per the security
/// checklist in strategy doc S12.
std::int64_t get_output_amount(std::int64_t input_amount,
                               std::int64_t input_reserve,
                               std::int64_t output_reserve,
                               std::int64_t fee_bps = 7);

/// Compute the implied mid-price of the pool (output per unit of input)
/// from the current reserves.
///
/// For a constant-product pool the marginal price (infinitesimal trade) is:
///
///   implied_price = output_reserve / input_reserve
///
/// This is the price BEFORE fees.  The effective price after fees for a
/// finite-size trade is lower (computed via get_output_amount).
///
/// @param input_reserve   Pool reserve of the input token.
/// @param output_reserve  Pool reserve of the output token.
///
/// @return Implied price (double), or 0.0 if either reserve is non-positive.
double get_implied_price(std::int64_t input_reserve,
                         std::int64_t output_reserve);

/// Compute the effective execution price for a given trade size, accounting
/// for the AMM fee and price impact (slippage).
///
/// effective_price = get_output_amount(input, in_res, out_res, fee)
///                 / static_cast<double>(input)
///
/// @param input_amount    Trade size (smallest units).
/// @param input_reserve   Pool reserve of the input token.
/// @param output_reserve  Pool reserve of the output token.
/// @param fee_bps         Pool fee in basis points (default 7).
///
/// @return Effective price (output per input), or 0.0 on invalid inputs.
double get_effective_price(std::int64_t input_amount,
                           std::int64_t input_reserve,
                           std::int64_t output_reserve,
                           std::int64_t fee_bps = 7);

}  // namespace tibet

// ---------------------------------------------------------------------------
// ArbitrageDetector -- scans multiple data sources for arbitrage opportunities
//                      across all four strategy types.
//
// Usage (per-block heartbeat):
//
//   ArbitrageDetector detector(config);
//
//   // Provide data feeds:
//   detector.set_dex_snapshot(dex_snap);
//   detector.set_cex_prices(cex_map);
//   detector.set_dexie_book(book);
//   detector.set_tibetswap_reserves(reserves);
//   detector.set_pair_prices(prices);
//   detector.set_bridge_prices(wusdc_price, wusdc_b_price);
//
//   // Run all scans:
//   auto opportunities = detector.scan_all();
//
//   // Or run individual scans:
//   auto cex_dex_opps = detector.scan_cex_dex(dex_snap, cex_prices);
// ---------------------------------------------------------------------------

class ArbitrageDetector {
public:
    /// Construct with the given configuration.
    explicit ArbitrageDetector(const ArbitrageConfig& cfg);

    // -----------------------------------------------------------------------
    // Individual scan methods -- each returns opportunities sorted by
    //   estimated_profit descending.
    // -----------------------------------------------------------------------

    /// Scan for CEX-DEX arbitrage on a single pair.
    ///
    /// Compares the DEX mid-price against each CEX venue's price.  If the
    /// absolute edge exceeds cex_dex_min_edge_bps and falls below
    /// cex_dex_max_edge_bps, an opportunity is emitted.
    ///
    /// Cost accounting: CEX taker fee + DEX settlement fee + bridge fee
    /// + slippage estimate based on available depth.
    ///
    /// Confidence: degrades with lower liquidity depth and longer expected
    ///   settlement time (52s DEX + CEX withdrawal).
    ///
    /// @param dex_snapshot  Latest dexie snapshot for the pair.
    /// @param cex_prices    CEX prices from one or more exchanges.
    ///
    /// @return Vector of opportunities, sorted by estimated_profit descending.
    std::vector<ArbitrageOpportunity>
    scan_cex_dex(const DexieBookSnapshot& dex_snapshot,
                 const std::vector<CexPrice>& cex_prices) const;

    /// Scan for cross-DEX arbitrage between dexie and TibetSwap.
    ///
    /// Compares the dexie order-book best bid/ask against the TibetSwap
    /// implied price (and effective price at trade size).  If the price
    /// difference exceeds cross_dex_min_edge_bps, an opportunity is emitted.
    ///
    /// Both legs are on-chain CHIA transactions, so settlement is ~52s and
    /// the arb can be near-atomic (submit both spend bundles in the same
    /// block).
    ///
    /// @param dexie_book         dexie order-book snapshot for the pair.
    /// @param tibetswap_reserves TibetSwap pool reserves for the same pair.
    ///
    /// @return Vector of opportunities, sorted by estimated_profit descending.
    std::vector<ArbitrageOpportunity>
    scan_cross_dex(const DexieBookSnapshot& dexie_book,
                   const TibetSwapReserves& tibetswap_reserves) const;

    /// Scan for triangular arbitrage across all 3-hop cycles.
    ///
    /// Enumerates all ordered triples (A, B, C) from the pair-price map such
    /// that the route XCH -> A -> B -> XCH exists.  For each cycle:
    ///
    ///   profit_ratio = rate(XCH->A) * rate(A->B) * rate(B->XCH)
    ///
    /// If profit_ratio > 1.0 + (sum of per-leg fees + slippage), the cycle
    /// is a profitable opportunity.
    ///
    /// Enumeration cost: O(N^2) where N = number of unique non-XCH assets.
    /// With the current CHIA CAT universe (~50 active tokens), this is
    /// at most ~2500 triples -- negligible at 52s block time.
    ///
    /// @param all_pair_prices  Map of "BASE/QUOTE" -> mid_price for all
    ///                         known pairs.
    ///
    /// @return Vector of opportunities, sorted by estimated_profit descending.
    std::vector<ArbitrageOpportunity>
    scan_triangular(const PairPriceMap& all_pair_prices) const;

    /// Scan for cross-bridge arbitrage between wUSDC variants.
    ///
    /// wUSDC (bridged from ETH via warp.green) and wUSDC.b (bridged from
    /// Base) represent the same underlying USD stablecoin but have different
    /// CHIA asset IDs.  If their DEX prices diverge by more than the
    /// bridge round-trip cost, an arbitrage exists.
    ///
    /// @param wusdc_price    Current DEX price of wUSDC (XCH per wUSDC).
    /// @param wusdc_b_price  Current DEX price of wUSDC.b (XCH per wUSDC.b).
    ///
    /// @return Vector of opportunities (0 or 1 element).
    std::vector<ArbitrageOpportunity>
    scan_cross_bridge(double wusdc_price, double wusdc_b_price) const;

    /// Run all four scans using the most recently set data and return a
    /// merged list sorted by estimated_profit descending.
    ///
    /// Requires that set_*() methods have been called with current data.
    /// Scans whose data has not been set are silently skipped.
    ///
    /// @return All detected opportunities across all strategies, sorted by
    ///         estimated_profit descending, filtered by min_confidence_threshold.
    std::vector<ArbitrageOpportunity> scan_all() const;

    // -----------------------------------------------------------------------
    // Data feed setters -- call these before scan_all() each block.
    // -----------------------------------------------------------------------

    /// Set the latest DEX snapshots (one per pair).
    void set_dex_snapshots(const std::vector<DexieBookSnapshot>& snapshots);

    /// Set the latest CEX prices (one per exchange-pair combination).
    void set_cex_prices(const std::vector<CexPrice>& prices);

    /// Set the latest dexie order-book snapshots (one per pair).
    void set_dexie_books(const std::vector<DexieBookSnapshot>& books);

    /// Set the latest TibetSwap pool reserves (one per pair).
    void set_tibetswap_reserves(const std::vector<TibetSwapReserves>& reserves);

    /// Set the latest pair-price map for triangular scanning.
    void set_pair_prices(const PairPriceMap& prices);

    /// Set the latest wUSDC and wUSDC.b prices for cross-bridge scanning.
    void set_bridge_prices(double wusdc_price, double wusdc_b_price);

    // -----------------------------------------------------------------------
    // Configuration access
    // -----------------------------------------------------------------------

    /// Read-only access to the active configuration.
    const ArbitrageConfig& config() const noexcept;

private:
    // -- Helper: compute confidence based on depth and settlement time ------
    double compute_confidence(double available_depth,
                              double trade_size,
                              double settlement_seconds) const;

    // -- Helper: compute urgency (blocks before opportunity closes) ---------
    std::uint32_t compute_urgency(double edge_bps,
                                  double volatility_per_block_bps) const;

    // -- Configuration ------------------------------------------------------
    ArbitrageConfig cfg_;

    // -- Cached data feeds for scan_all() -----------------------------------
    std::vector<DexieBookSnapshot>  cached_dex_snapshots_;
    std::vector<CexPrice>           cached_cex_prices_;
    std::vector<DexieBookSnapshot>  cached_dexie_books_;
    std::vector<TibetSwapReserves>  cached_tibetswap_reserves_;
    PairPriceMap                    cached_pair_prices_;
    double                          cached_wusdc_price_{0.0};
    double                          cached_wusdc_b_price_{0.0};
};

}  // namespace xop

#endif  // XOP_STRATEGY_ARBITRAGE_HPP
