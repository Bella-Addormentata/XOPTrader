// =============================================================================
// fair_value_solver.cpp -- Weighted least-squares solve over the asset graph.
// =============================================================================
//
// See fair_value_solver.hpp for the derivation.  In brief: with x_a the log USD
// price of asset a, minimising
//
//     J(x) = SUM_a w_a (x_a - log P_a)^2 + SUM_e w_e (x_b - x_q - log p_e)^2
//
// gives, at the stationary point of node a,
//
//     (w_a + SUM_{e inc a} w_e) x_a
//         = w_a log P_a
//         + SUM_{e: base=a}  w_e (x_q + log p_e)
//         + SUM_{e: quote=a} w_e (x_b - log p_e)
//
// which is exactly one Gauss-Seidel sweep.  The same matrix, driven by the
// indicator vector 1_b - 1_q instead, yields Var(x_b - x_q).
//
// Compliant with:
//   ISO/IEC 5055 -- every loop bounded, every division guarded, no UB on
//                   empty or degenerate input.
// =============================================================================

#include "xop/execution/fair_value_solver.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace xop {
namespace fv {

namespace {

/// Smallest sigma we will honour, in bps.  Anything below this would produce a
/// weight large enough to swamp the rest of the system on rounding alone; an
/// observation claiming sub-micro-bp certainty is a bug in the caller, not a
/// windfall of precision.
constexpr double kMinSigmaBps = 1e-3;

/// Inverse-variance weight of an observation quoted with a 1-sigma of
/// sigma_bps.  Weights live in log-price units, so bps are converted first.
double weight_from_sigma_bps(double sigma_bps) noexcept {
    const double s = std::max(sigma_bps, kMinSigmaBps) / 10'000.0;
    return 1.0 / (s * s);
}

/// A normalised copy of the problem: names resolved to indices, unusable
/// observations dropped, and the target pair's own book edge removed.
struct System {
    std::vector<std::string> assets;                 // index -> name
    std::unordered_map<std::string, std::size_t> id; // name  -> index

    // Anchors, aligned with `assets` (weight 0 == no anchor).
    std::vector<double> anchor_log_price;
    std::vector<double> anchor_weight;

    // Active edges.
    struct E { std::size_t b, q; double log_price, weight; };
    std::vector<E> active;

    // Per-node diagonal of the normal matrix.
    std::vector<double> diag;

    std::size_t index_of(const std::string& name) {
        auto it = id.find(name);
        if (it != id.end()) return it->second;
        const std::size_t n = assets.size();
        assets.push_back(name);
        id.emplace(name, n);
        anchor_log_price.push_back(0.0);
        anchor_weight.push_back(0.0);
        diag.push_back(0.0);
        return n;
    }
};

bool usable(const Anchor& a) noexcept {
    return !a.asset.empty() && a.usd_price > 0.0 && std::isfinite(a.usd_price)
        && a.sigma_bps > 0.0 && std::isfinite(a.sigma_bps);
}

bool usable(const Edge& e) noexcept {
    return !e.base.empty() && !e.quote.empty() && e.base != e.quote
        && e.price > 0.0 && std::isfinite(e.price)
        && e.sigma_bps > 0.0 && std::isfinite(e.sigma_bps);
}

/// Build the normalised system, honouring the exclusion rule.
System build(const std::vector<Anchor>& anchors,
             const std::vector<Edge>&   edges,
             const std::string&         base,
             const std::string&         quote,
             int                        exclude_book_of,
             std::size_t                skip_observation) {
    System sys;

    // Seed the target legs first so they always exist as nodes even when no
    // observation mentions them -- that case must report "unreachable", not
    // silently resolve to some other node's index.
    sys.index_of(base);
    sys.index_of(quote);

    // Observations are numbered anchors-first, then edges, so that a single
    // `skip_observation` index can address either kind.  That numbering is the
    // whole mechanism behind the redundancy test.
    std::size_t obs = 0;
    for (const Anchor& a : anchors) {
        if (!usable(a)) continue;
        const std::size_t here = obs++;
        if (here == skip_observation) continue;
        const std::size_t i = sys.index_of(a.asset);
        const double w = weight_from_sigma_bps(a.sigma_bps);
        // Repeated anchors on one asset combine as independent measurements.
        const double w_new = sys.anchor_weight[i] + w;
        sys.anchor_log_price[i] =
            (sys.anchor_weight[i] * sys.anchor_log_price[i]
             + w * std::log(a.usd_price)) / w_new;
        sys.anchor_weight[i] = w_new;
    }

    for (const Edge& e : edges) {
        if (!usable(e)) continue;
        // The independence guarantee: a pair's own ORDER BOOK never
        // contributes to that pair's fair value.  Non-book observations of the
        // same pair (an AMM implied price) survive, because they are not the
        // thing being validated.
        if (e.is_book && e.owner_pair >= 0 && e.owner_pair == exclude_book_of) {
            continue;
        }
        const std::size_t here = obs++;
        if (here == skip_observation) continue;
        const std::size_t b = sys.index_of(e.base);
        const std::size_t q = sys.index_of(e.quote);
        sys.active.push_back(
            System::E{b, q, std::log(e.price), weight_from_sigma_bps(e.sigma_bps)});
    }

    sys.diag.assign(sys.assets.size(), 0.0);
    for (std::size_t i = 0; i < sys.assets.size(); ++i) {
        sys.diag[i] = sys.anchor_weight[i];
    }
    for (const auto& e : sys.active) {
        sys.diag[e.b] += e.weight;
        sys.diag[e.q] += e.weight;
    }
    return sys;
}

/// Nodes with a weighted path to at least one anchor.  Only on those is the
/// normal matrix positive definite, so only there is the answer defined.
std::vector<char> anchored_reach(const System& sys) {
    std::vector<char> reach(sys.assets.size(), 0);
    for (std::size_t i = 0; i < sys.assets.size(); ++i) {
        if (sys.anchor_weight[i] > 0.0) reach[i] = 1;
    }
    // Bounded relaxation: each sweep can only add nodes, and there are at most
    // |V| nodes to add, so this terminates.
    for (std::size_t sweep = 0; sweep <= sys.assets.size(); ++sweep) {
        bool changed = false;
        for (const auto& e : sys.active) {
            if (e.weight <= 0.0) continue;
            if (reach[e.b] && !reach[e.q]) { reach[e.q] = 1; changed = true; }
            if (reach[e.q] && !reach[e.b]) { reach[e.b] = 1; changed = true; }
        }
        if (!changed) break;
    }
    return reach;
}

/// Gauss-Seidel on (diag) x = rhs_const + off-diagonal coupling.
///
/// @param use_anchor_rhs  true  -> right-hand side is the anchor term
///                                 (solves for the prices themselves);
///                        false -> right-hand side is `unit`
///                                 (solves A y = e for the variance).
std::vector<double> gauss_seidel(const System&              sys,
                                 const std::vector<double>& unit,
                                 bool                       use_anchor_rhs,
                                 const SolverOptions&       opts) {
    const std::size_t n = sys.assets.size();
    std::vector<double> x(n, 0.0);

    // Warm start the price solve at the anchors: for a well-anchored graph this
    // is already close, so the sweeps below mostly refine rather than search.
    if (use_anchor_rhs) {
        for (std::size_t i = 0; i < n; ++i) {
            if (sys.anchor_weight[i] > 0.0) x[i] = sys.anchor_log_price[i];
        }
    }

    for (std::size_t it = 0; it < opts.max_iterations; ++it) {
        double max_delta = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            if (sys.diag[i] <= 0.0) continue;  // Unconstrained node: leave at 0.
            double rhs = use_anchor_rhs
                ? sys.anchor_weight[i] * sys.anchor_log_price[i]
                : unit[i];
            for (const auto& e : sys.active) {
                if (e.b == i) {
                    rhs += e.weight * (x[e.q] + (use_anchor_rhs ? e.log_price : 0.0));
                } else if (e.q == i) {
                    rhs += e.weight * (x[e.b] - (use_anchor_rhs ? e.log_price : 0.0));
                }
            }
            const double next = rhs / sys.diag[i];
            max_delta = std::max(max_delta, std::abs(next - x[i]));
            x[i] = next;
        }
        if (max_delta < opts.tolerance) break;
    }
    return x;
}

/// Count observations that survive the exclusion rule (before any redundancy
/// deletion).  Mirrors build()'s numbering exactly.
std::size_t count_observations(const std::vector<Anchor>& anchors,
                               const std::vector<Edge>&   edges,
                               int                        exclude_book_of) {
    std::size_t n = 0;
    for (const Anchor& a : anchors) if (usable(a)) ++n;
    for (const Edge& e : edges) {
        if (!usable(e)) continue;
        if (e.is_book && e.owner_pair >= 0 && e.owner_pair == exclude_book_of) {
            continue;
        }
        ++n;
    }
    return n;
}

/// True when base and quote are both anchor-reachable in `sys`.
bool legs_reachable(const System& sys,
                    const std::string& base, const std::string& quote) {
    auto ib = sys.id.find(base);
    auto iq = sys.id.find(quote);
    if (ib == sys.id.end() || iq == sys.id.end()) return false;
    const auto reach = anchored_reach(sys);
    return reach[ib->second] != 0 && reach[iq->second] != 0;
}

}  // namespace

double combine_sigma_bps(const std::vector<double>& terms) {
    double sum_sq = 0.0;
    for (double t : terms) {
        if (!std::isfinite(t) || t <= 0.0) continue;  // A negative sigma is not
        sum_sq += t * t;                              // a negative variance.
    }
    return std::sqrt(sum_sq);
}

Solution solve_pair(const std::vector<Anchor>& anchors,
                    const std::vector<Edge>&   edges,
                    const std::string&         base,
                    const std::string&         quote,
                    int                        exclude_book_of,
                    const SolverOptions&       opts) {
    Solution out;
    if (base.empty() || quote.empty() || base == quote) {
        return out;  // Not a pair.
    }

    // Sentinel larger than any observation index: skip nothing.
    constexpr std::size_t kSkipNothing =
        static_cast<std::size_t>(-1);

    System sys = build(anchors, edges, base, quote, exclude_book_of, kSkipNothing);

    const auto ib = sys.id.find(base);
    const auto iq = sys.id.find(quote);
    if (ib == sys.id.end() || iq == sys.id.end()) return out;
    const std::size_t b = ib->second;
    const std::size_t q = iq->second;

    const auto reach = anchored_reach(sys);
    if (!reach[b] || !reach[q]) {
        // No weighted path from either leg to any external anchor.  There is
        // nothing to say about this pair that does not come from its own book,
        // so say nothing -- the caller must widen, not guess.
        return out;
    }

    const std::vector<double> x =
        gauss_seidel(sys, /*unit=*/{}, /*use_anchor_rhs=*/true, opts);

    const double log_price = x[b] - x[q];
    if (!std::isfinite(log_price)) return out;

    // Var(x_b - x_q) = e^T A^{-1} e with e = 1_b - 1_q.
    std::vector<double> unit(sys.assets.size(), 0.0);
    unit[b] += 1.0;
    unit[q] -= 1.0;
    const std::vector<double> y =
        gauss_seidel(sys, unit, /*use_anchor_rhs=*/false, opts);

    const double var = y[b] - y[q];
    if (!std::isfinite(var) || var < 0.0) return out;

    out.ok           = true;
    out.price        = std::exp(log_price);
    out.sigma_bps    = std::sqrt(var) * 10'000.0;
    out.observations = count_observations(anchors, edges, exclude_book_of);
    out.both_anchored =
        sys.anchor_weight[b] > 0.0 && sys.anchor_weight[q] > 0.0;

    if (!std::isfinite(out.price) || out.price <= 0.0) {
        out = Solution{};
        return out;
    }

    // Redundancy: does the answer survive the loss of ANY single surviving
    // observation?  If deleting one observation orphans a leg, the value was
    // merely propagated along a unique path and has never been cross-checked.
    // With two or fewer observations there is nothing to cross-check with.
    out.redundant = out.observations >= 2;
    for (std::size_t i = 0; i < out.observations && out.redundant; ++i) {
        System reduced =
            build(anchors, edges, base, quote, exclude_book_of, i);
        if (!legs_reachable(reduced, base, quote)) out.redundant = false;
    }

    return out;
}

}  // namespace fv
}  // namespace xop
