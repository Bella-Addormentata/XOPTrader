// =============================================================================
// fair_value_solver.hpp -- Triangulated fair value over the asset/pair graph.
// =============================================================================
//
// WHY THIS EXISTS
// ---------------
// The offer ladder is centred on a dexie book mid.  Nothing validated that mid,
// so a uniformly wrong book moved every tier the same way and one taker could
// lift the whole ladder (2026-08-01 10:31 UTC: six XCH/BYC ask tiers filled in
// one block, each logging a POSITIVE realized P&L because the cost basis had
// itself been set by equally-mispriced bids).
//
// The first attempt at a fix valued a pegged leg at its declared peg.  That was
// rejected: BYC has never traded at par, so the peg is not an observation, it
// is an assumption -- and an assumption that happens to be wrong by ~14% is a
// worse anchor than no anchor at all.
//
// THE IDEA
// --------
// Treat assets as nodes and trading pairs as edges.  Any cycle in that graph is
// a consistency constraint: XCH/wUSDC.b must equal XCH/BYC * BYC/wUSDC.b.  With
// four pairs over four assets (three of them carrying an external USD feed) the
// system has more observations than unknowns, so it can be SOLVED rather than
// assumed, and the leftover disagreement is measurable.
//
// Let x_a = log(USD price of asset a).  Every observation is linear in x:
//
//     anchor a  (external USD feed price P_a)   :  x_a            ~ log P_a
//     edge   e  (pair base/quote, price p_e)    :  x_b - x_q      ~ log p_e
//
// Weight each by the inverse variance of its own uncertainty and minimise
//
//     J(x) = SUM_a w_a (x_a - log P_a)^2  +  SUM_e w_e (x_b - x_q - log p_e)^2
//
// This is a weighted graph-Laplacian least-squares problem.  Its normal matrix
// A = diag(w_anchor) + L(w_edge) is symmetric positive definite on any
// connected component that contains at least one anchor, so Gauss-Seidel
// converges and the solution is unique.  Log space is not a convenience: it
// makes the estimate of b from q and the estimate of q from b exactly
// reciprocal, so the answer does not depend on how a pair happens to be named.
//
// THE INDEPENDENCE GUARANTEE
// --------------------------
// A fair value derived from the book it is meant to validate is worthless.  So
// the fair value for pair T is produced by re-solving with T's OWN BOOK EDGE
// REMOVED.  T's price is then whatever the rest of the graph implies, and the
// difference between that and T's book is the consistency residual -- the
// disagreement signal.  Non-book observations for T (an AMM implied price) are
// kept, because an AMM quote is genuinely independent of the order book.
//
// CONFIDENCE IS COMPUTED, NOT ASSUMED
// -----------------------------------
// The variance of the answer follows from the same normal matrix:
//
//     Var(x_b - x_q) = e^T A^{-1} e,   e = 1_b - 1_q
//
// which is one extra linear solve.  That single number is what decides whether
// a fair value is usable at all.  It is why a pair whose only surviving path
// runs through a 1259 bps wide frozen book is reported as UNAVAILABLE instead
// of being handed a confident-looking number: the honest answer to "what is
// BYC worth" given only that book is "somewhere within +-6%", and a 3% clamp
// band around a +-6% estimate is not a guard, it is theatre.
//
// NO PAIR IS SPECIAL-CASED.  Nothing below knows the name of any asset or pair;
// the graph is whatever the caller passes in.  Adding a pair adds a node and an
// edge, and the extra cycles it closes make every other pair's answer better.
//
// Compliant with:
//   ISO/IEC 27001:2022 -- no secrets, no external I/O.
//   ISO/IEC 5055       -- bounded iteration counts, no unbounded recursion,
//                         every division guarded, no UB on empty input.
//   ISO/IEC 25010      -- deterministic and pure: same input, same output.
// =============================================================================

#ifndef XOP_EXECUTION_FAIR_VALUE_SOLVER_HPP
#define XOP_EXECUTION_FAIR_VALUE_SOLVER_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace xop {
namespace fv {

// ---------------------------------------------------------------------------
// Anchor -- an external USD price for one asset.
//
// "External" means it does not come from any book in the graph: a CEX/price
// feed listing.  sigma_bps is the 1-sigma disagreement expected between that
// feed and the on-chain market, NOT the feed's own quoting precision.  The two
// are very different: CoinGecko quotes XCH to eight digits, but its number and
// the dexie XCH/wUSDC.b mid routinely differ by ~1%, and 1% is the honest
// uncertainty to carry into the solve.
// ---------------------------------------------------------------------------
struct Anchor {
    std::string asset;            // Canonical asset key (caller's convention).
    double      usd_price{0.0};   // > 0 to be used.
    double      sigma_bps{0.0};   // > 0 to be used.
};

// ---------------------------------------------------------------------------
// Edge -- one price observation relating two assets.
//
// price is quote-per-base, exactly as pairs are quoted: XCH/BYC price 1.4 means
// one XCH buys 1.4 BYC, i.e. usd(base) = price * usd(quote).
//
// is_book distinguishes the observations that must be excluded when validating
// their own pair (dexie order-book mids) from those that need not be (AMM
// implied prices).  owner_pair identifies which pair the observation belongs
// to; -1 means "belongs to no pair" and is never excluded.
// ---------------------------------------------------------------------------
struct Edge {
    std::string base;
    std::string quote;
    double      price{0.0};        // > 0 to be used.
    double      sigma_bps{0.0};    // > 0 to be used.
    int         owner_pair{-1};    // Index of the pair this observation is of.
    bool        is_book{true};     // True for an order-book mid.
};

// ---------------------------------------------------------------------------
// Solution -- the answer for one pair, with everything needed to judge it.
// ---------------------------------------------------------------------------
struct Solution {
    /// False when the pair cannot be priced at all: an unknown leg, or a leg
    /// with no weighted path to any anchor.  Every other field is meaningless
    /// when this is false.
    bool   ok{false};

    /// Quote-per-base, on the same scale as Edge::price.
    double price{0.0};

    /// 1-sigma uncertainty of log(price), expressed in basis points.  This is
    /// the number that decides usability; it is derived from the observation
    /// weights, never assumed.
    double sigma_bps{0.0};

    /// True when the answer survives the deletion of ANY single remaining
    /// observation -- i.e. the pair is genuinely over-determined and the value
    /// has been cross-checked rather than merely propagated.
    bool   redundant{false};

    /// True when both legs carry a direct external anchor, so the answer needs
    /// no on-chain book at all.
    bool   both_anchored{false};

    /// Count of observations (anchors + edges) that actually participated,
    /// i.e. had positive weight and were not excluded.
    std::size_t observations{0};
};

// ---------------------------------------------------------------------------
// SolverOptions -- iteration budget only.  Everything that shapes the ANSWER
// lives in the weights the caller supplies, so that policy stays in config and
// this file stays pure arithmetic.
// ---------------------------------------------------------------------------
struct SolverOptions {
    /// Maximum Gauss-Seidel sweeps.  The systems here have a handful of nodes
    /// and converge in tens of sweeps; the cap exists so a pathological graph
    /// cannot stall a heartbeat.
    std::size_t max_iterations{500};

    /// Stop early once the largest coordinate update falls below this (in log
    /// units).  1e-12 is far below any price resolution we act on.
    double tolerance{1e-12};
};

// ---------------------------------------------------------------------------
// solve_pair -- fair value of base/quote implied by everything EXCEPT that
//               pair's own book.
//
// @param anchors            External USD prices.  Entries with a non-positive
//                           price or sigma are ignored.
// @param edges              Price observations.  Entries with a non-positive
//                           price or sigma, or an empty leg name, are ignored.
// @param base,quote         The pair to price.  Must differ.
// @param exclude_book_of    Exclude every edge with is_book == true and
//                           owner_pair == this value.  Pass -1 to exclude
//                           nothing (used to measure raw graph consistency).
// @param opts               Iteration budget.
//
// Cost is O(iterations * edges) per call plus one identical solve for the
// variance -- microseconds for the graphs this system has.
// ---------------------------------------------------------------------------
Solution solve_pair(const std::vector<Anchor>& anchors,
                    const std::vector<Edge>&   edges,
                    const std::string&         base,
                    const std::string&         quote,
                    int                        exclude_book_of,
                    const SolverOptions&       opts = {});

// ---------------------------------------------------------------------------
// combine_sigma_bps -- quadrature sum of independent uncertainty terms.
//
// Exposed because the engine builds an edge's sigma from several independent
// causes (book width, staleness, thin depth) and the composition rule should
// be stated once and tested, not repeated at each call site.  Negative terms
// are treated as zero rather than silently producing a smaller total.
// ---------------------------------------------------------------------------
double combine_sigma_bps(const std::vector<double>& terms);

// ---------------------------------------------------------------------------
// amm_sigma_bps -- uncertainty of an AMM implied price, from POOL DEPTH.
//
// WHY A FLAT CONSTANT WAS WRONG
// -----------------------------
// The claim that justifies trusting an AMM quote is "arbitrage holds it to
// fair value".  That claim is a statement about MONEY, not about the formula:
// it holds only when correcting a dislocation pays more than it costs.  A
// constant-product pool moves by roughly the fraction of its reserves that a
// trade consumes, so on a $500 pool a 10% dislocation is corrected by pushing
// ~$25 through it -- and nobody watches an obscure pool to earn a few dollars
// minus fees and two on-chain transactions.  The live pools are dust:
//
//     wUSDC.b   393.868 XCH / 564.862 wUSDC.b   ~ $1,125 total
//     BYC       175.406 XCH / 261.655 BYC       ~ $500   total
//
// At that size the pool is not an arbitrage-pinned oracle, it is one stale
// LP's last trade.  A flat 50 bps sigma gave it weight 1/(50e-4)^2 = 4.0e4
// against ~242 for the competing BYC/wUSDC.b book -- roughly 166:1 -- so a
// $500 pool silently outvoted every other observation in the graph.
//
// That ratio is not a theoretical worry.  At the 2026-08-01 sweep the BYC
// pool implied 1.4917 XCH/BYC while the truth was ~1.414: an error of ~550
// bps, against the 50 bps it was claiming.  The depth-derived sigma for that
// pool is 670 bps -- the same order as its ACTUAL error, which is the whole
// point of deriving it.
//
// THE FORM
// --------
//     sigma_bps = max(floor_bps, depth_k_bps / sqrt(pool_usd))
//
// The 1/sqrt(depth) shape is the usual price-impact scaling and, more to the
// point, it is the shape that makes weight (1/sigma^2) grow LINEARLY in pool
// size: doubling the money defending the price doubles its vote.  That is the
// property being modelled -- influence proportional to capital at risk -- and
// no other simple form has it.
//
// floor_bps stops a very deep pool from claiming more certainty than the swap
// fee and arbitrage band allow, so the old 50 bps survives as the BEST case
// rather than the assumed case.
//
// @param pool_usd      Total USD value of BOTH pool sides.  Must be > 0 and
//                      finite; anything else returns 0.0, meaning "this
//                      observation cannot be weighted, so do not use it".
// @param floor_bps     Lower bound on the result (deep-pool asymptote).
// @param depth_k_bps   Calibration constant: the sigma of a $1 pool.  See
//                      StrategyConfig::fair_value_amm_depth_k_bps.
// ---------------------------------------------------------------------------
double amm_sigma_bps(double pool_usd, double floor_bps, double depth_k_bps);

// ---------------------------------------------------------------------------
// CenterBlend / blend_quote_center -- uncertainty-weighted ladder centre.
//
// WHY THIS EXISTS
// ---------------
// The offer ladder used to be centred on the pair's own mid unconditionally,
// and the external fair value was consulted only as a binary gate: usable
// (clamp) or Unavailable (widen blindly).  That cliff threw away the estimate
// exactly when it mattered -- at the 2026-08-01 sweep the solve knew XCH/BYC
// was worth ~1.36 with a 467 bps error bar, and the ladder still quoted six
// asks centred on a 1.2673 book mid because 467 > the 200 bps usability
// ceiling.  Sigma is not a validity flag; it is a width instruction.
//
// THE FORM
// --------
// Both inputs are estimates of the same log-price, each with its own 1-sigma
// (in bps of log price, the solver's native unit).  The minimum-variance
// combination of two independent estimates is the inverse-variance weighted
// mean, taken in LOG space so the answer does not depend on which leg the
// pair is named after (the same argument as the solver itself):
//
//     w_b = 1/sigma_book^2          w_e = 1/sigma_ext^2
//     log(center) = (w_b log(book) + w_e log(ext)) / (w_b + w_e)
//     sigma(center) = sqrt(1 / (w_b + w_e))
//
// A tight fresh book (small sigma_book) dominates its own pricing; a wide or
// stale book cedes to the external estimate.  With only one side present the
// blend degrades to that side verbatim -- a pair with no external anchor at
// all quotes around its own mid with its own width, it does not go silent.
//
// No pair is named anywhere; the inputs are numbers and the policy that
// produces them lives in the caller.
// ---------------------------------------------------------------------------
struct CenterBlend {
    /// False when NEITHER input is usable (non-positive or non-finite price
    /// or sigma).  Every other field is meaningless when this is false.
    bool   ok{false};

    /// The blended centre, on the same price scale as the inputs.
    double center{0.0};

    /// 1-sigma of the blended centre in bps of log price.  This is the
    /// combined_sigma the ladder's minimum half-spread is derived from:
    /// always <= min(sigma_book, sigma_ext) when both sides participate,
    /// and exactly the surviving side's sigma when only one does.
    double sigma_bps{0.0};

    /// Weight the external estimate received, in [0, 1].  0 = book only,
    /// 1 = external only.  Logged so an operator can see who priced a pair.
    double w_external{0.0};
};

/// @param book_mid           The pair's own mid.  <= 0 or non-finite means
///                           "no book opinion" and the external side is used
///                           alone.
/// @param book_sigma_bps     1-sigma of the book mid (spread/2 + staleness +
///                           depth, combined by the caller with
///                           combine_sigma_bps).  Must be > 0 for the book to
///                           participate.
/// @param external_price     The independent solve estimate.  <= 0 or
///                           non-finite means "no external opinion".
/// @param external_sigma_bps 1-sigma of that estimate, from the solve's own
///                           normal matrix.  Must be > 0 to participate.
CenterBlend blend_quote_center(double book_mid,
                               double book_sigma_bps,
                               double external_price,
                               double external_sigma_bps);

}  // namespace fv
}  // namespace xop

#endif  // XOP_EXECUTION_FAIR_VALUE_SOLVER_HPP
