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

}  // namespace fv
}  // namespace xop

#endif  // XOP_EXECUTION_FAIR_VALUE_SOLVER_HPP
