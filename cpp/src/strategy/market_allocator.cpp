// market_allocator.cpp -- Dynamic capital allocation across trading pairs.
//
// See market_allocator.hpp for the design rationale and scoring dimensions.

#include "xop/strategy/market_allocator.hpp"
#include "xop/execution/market_data.hpp"
#include "xop/database.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>

namespace xop {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MarketAllocator::MarketAllocator(const MarketAllocatorConfig& cfg,
                                 const MarketDataFeed*        market_data,
                                 const Database*              db)
    : cfg_(cfg)
    , market_data_(market_data)
    , db_(db)
{
    assert(market_data_ != nullptr);
    // db_ may be null in unit tests.
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

bool MarketAllocator::should_evaluate(BlockHeight block_height) const
{
    if (!cfg_.enabled) return false;
    if (last_eval_block_ == 0) return true;  // First evaluation.
    return (block_height >= last_eval_block_ + cfg_.eval_interval_blocks);
}

void MarketAllocator::evaluate(const std::vector<std::string>& pair_names,
                               BlockHeight                     block_height)
{
    if (pair_names.empty()) return;

    // Build per-pair scores.
    std::vector<PairScore> scores;
    scores.reserve(pair_names.size());

    // Pre-compute triangular arb edge (shared across pairs).
    TriArbResult tri_arb = compute_tri_arb_edge();

    for (const auto& name : pair_names) {
        PairScore ps;
        ps.pair_name = name;

        score_spread(ps);
        score_volume(ps);
        score_competition(ps);
        score_fill_rate(ps);
        score_tri_arb(ps, tri_arb);

        scores.push_back(std::move(ps));
    }

    // Normalise each dimension to [0, 1] relative to the pair set.
    normalise_scores(scores);

    // Weighted composite.
    compute_composites(scores);

    // Convert composites to allocation fractions with guardrails.
    apply_allocation(scores);

    // Log.
    for (const auto& ps : scores) {
        spdlog::info("[MarketAllocator] {} score={:.3f} alloc={:.1f}% "
                     "(spread={:.1f} vol={:.1f} comp={:.1f} fill={:.1f} "
                     "arb={:.1f})",
                     ps.pair_name, ps.composite, ps.allocation * 100.0,
                     ps.norm_spread, ps.norm_volume, ps.norm_competition,
                     ps.norm_fill_rate, ps.norm_tri_arb);
    }

    if (tri_arb.has_edge) {
        spdlog::info("[MarketAllocator] Triangular arb edge: "
                     "fwd={:.1f}bps rev={:.1f}bps",
                     tri_arb.forward_edge_bps, tri_arb.reverse_edge_bps);
    }

    // Commit.
    scores_ = std::move(scores);
    last_eval_block_ = block_height;
    for (const auto& ps : scores_) {
        allocations_[ps.pair_name] = ps.allocation;
    }
}

double MarketAllocator::get_allocation(const std::string& pair_name) const
{
    auto it = allocations_.find(pair_name);
    if (it != allocations_.end()) return it->second;

    // Never evaluated — return equal share or minimum.
    return cfg_.min_alloc_pct;
}

std::unordered_map<std::string, double>
MarketAllocator::get_all_allocations() const
{
    return allocations_;
}

std::vector<PairScore> MarketAllocator::get_scores() const
{
    return scores_;
}

// ---------------------------------------------------------------------------
// Dimension scorers
// ---------------------------------------------------------------------------

void MarketAllocator::score_spread(PairScore& ps) const
{
    // Raw spread this pair offers.  Wider = more profit per round trip.
    double our_spread = market_data_->get_spread_bps(ps.pair_name);

    // Best competitor spread (if better than ours, we earn the gap).
    double best_comp = market_data_->get_best_competing_spread_bps(ps.pair_name);

    // Net achievable spread: our spread minus best competitor.
    // If we're wider than any competitor, net is positive (we can fill).
    // Capped at 0 — negative means the market is too tight for us.
    ps.raw_spread = std::max(0.0, our_spread);
    // Blend: reward both absolute spread and competitive position.
    if (best_comp > 0.0) {
        // If competitor is tighter, our effective profit is limited.
        ps.raw_spread = std::min(ps.raw_spread, best_comp * 0.9);
    }
}

void MarketAllocator::score_volume(PairScore& ps) const
{
    ps.raw_volume = market_data_->get_volume_24h(ps.pair_name);
}

void MarketAllocator::score_competition(PairScore& ps) const
{
    auto n = market_data_->get_num_competing_offers(ps.pair_name);
    // Inverse: fewer competitors → higher score.
    // Smooth: 1/(1+n) so 0 competitors → 1.0, 8 competitors → 0.11.
    ps.raw_competition = 1.0 / (1.0 + static_cast<double>(n));
}

void MarketAllocator::score_fill_rate(PairScore& ps) const
{
    // Query fill rate from the database.
    if (db_) {
        // Use a recent window (last ~7 hours = 500 blocks).
        auto cutoff = std::chrono::system_clock::now()
            - std::chrono::hours(cfg_.eval_interval_blocks > 0
                                 ? 7 : 24);
        (void)cutoff;  // Fill rate via DB: use fill_rate_since_block.
        // Approximate: use global fill rate as a proxy.
        ps.raw_fill_rate = db_->fill_rate_since_block(0, 0.0);
    }
    // In tests or when DB is null, stays 0.
}

void MarketAllocator::score_tri_arb(PairScore& ps,
                                    const TriArbResult& tri) const
{
    if (!tri.has_edge) {
        ps.raw_tri_arb = 0.0;
        return;
    }
    // The edge benefits all three pairs in the triangle.  But the pair
    // that is "cheapest" on one leg benefits more — for simplicity, give
    // equal tri-arb score to all triangle pairs.
    double max_edge = std::max(tri.forward_edge_bps, tri.reverse_edge_bps);

    // Only score for pairs that participate in the triangle.
    const auto& name = ps.pair_name;
    bool in_triangle = (name.find("XCH") != std::string::npos &&
                        name.find("wUSDC") != std::string::npos) ||
                       (name.find("XCH") != std::string::npos &&
                        name.find("BYC") != std::string::npos) ||
                       (name.find("BYC") != std::string::npos &&
                        name.find("wUSDC") != std::string::npos);

    ps.raw_tri_arb = in_triangle ? max_edge : 0.0;
}

// ---------------------------------------------------------------------------
// Triangular arbitrage detection
// ---------------------------------------------------------------------------

TriArbResult MarketAllocator::compute_tri_arb_edge() const
{
    TriArbResult result;

    // We need prices for all three legs of the triangle:
    //   XCH/wUSDC, XCH/BYC, BYC/wUSDC
    //
    // Convention: price = how many quote per 1 base.
    //   XCH/wUSDC.b  → price in wUSDC per XCH  (~2.37)
    //   XCH/BYC      → price in BYC per XCH     (~2.40)
    //   BYC/wUSDC.b  → price in wUSDC per BYC   (~0.99)

    double xch_usdc_mid = market_data_->get_mid_price("XCH/wUSDC.b");
    double xch_byc_mid  = market_data_->get_mid_price("XCH/BYC");
    double byc_usdc_mid = market_data_->get_mid_price("BYC/wUSDC.b");

    // Need all three prices to compute the triangle.
    if (xch_usdc_mid <= 0.0 || xch_byc_mid <= 0.0 || byc_usdc_mid <= 0.0) {
        return result;
    }

    // Per-leg fee multiplier (each trade costs fee_bps).
    double fee_mult = 1.0 - cfg_.tri_arb_fee_bps / 10000.0;

    // Forward cycle: 1 XCH → sell for wUSDC → buy BYC → buy XCH
    //   1 XCH → xch_usdc_mid wUSDC
    //   wUSDC → wUSDC / byc_usdc_mid BYC
    //   BYC   → BYC / xch_byc_mid XCH
    double fwd = (xch_usdc_mid * fee_mult)        // XCH → wUSDC
               / (byc_usdc_mid / fee_mult)         // wUSDC → BYC (buying BYC at ask)
               / (xch_byc_mid / fee_mult);         // BYC → XCH  (buying XCH at ask)

    // Reverse cycle: 1 XCH → sell for BYC → sell BYC for wUSDC → buy XCH
    //   1 XCH → xch_byc_mid BYC
    //   BYC   → BYC * byc_usdc_mid wUSDC
    //   wUSDC → wUSDC / xch_usdc_mid XCH
    double rev = (xch_byc_mid * fee_mult)          // XCH → BYC
               * (byc_usdc_mid * fee_mult)          // BYC → wUSDC
               / (xch_usdc_mid / fee_mult);         // wUSDC → XCH

    // Edge in bps: (result - 1.0) * 10000.
    result.forward_edge_bps = (fwd - 1.0) * 10000.0;
    result.reverse_edge_bps = (rev - 1.0) * 10000.0;
    result.has_edge = (result.forward_edge_bps > cfg_.tri_arb_min_edge_bps ||
                       result.reverse_edge_bps > cfg_.tri_arb_min_edge_bps);

    return result;
}

// ---------------------------------------------------------------------------
// Normalisation & composite
// ---------------------------------------------------------------------------

void MarketAllocator::normalise_scores(std::vector<PairScore>& scores) const
{
    if (scores.empty()) return;

    // For each dimension, find the max raw value and scale to [0, 1].
    // If max is 0, all scores for that dimension stay 0.
    auto normalise = [](std::vector<PairScore>& s,
                        double PairScore::*raw,
                        double PairScore::*norm) {
        double mx = 0.0;
        for (const auto& ps : s) mx = std::max(mx, ps.*raw);
        if (mx > 0.0) {
            for (auto& ps : s) ps.*norm = (ps.*raw) / mx;
        }
    };

    normalise(scores, &PairScore::raw_spread, &PairScore::norm_spread);
    normalise(scores, &PairScore::raw_volume, &PairScore::norm_volume);
    normalise(scores, &PairScore::raw_competition, &PairScore::norm_competition);
    normalise(scores, &PairScore::raw_fill_rate, &PairScore::norm_fill_rate);
    normalise(scores, &PairScore::raw_tri_arb, &PairScore::norm_tri_arb);
}

void MarketAllocator::compute_composites(std::vector<PairScore>& scores) const
{
    double total_weight = cfg_.weight_spread + cfg_.weight_volume
                        + cfg_.weight_competition + cfg_.weight_fill_rate
                        + cfg_.weight_tri_arb;
    if (total_weight <= 0.0) total_weight = 1.0;

    for (auto& ps : scores) {
        ps.composite = (cfg_.weight_spread * ps.norm_spread
                      + cfg_.weight_volume * ps.norm_volume
                      + cfg_.weight_competition * ps.norm_competition
                      + cfg_.weight_fill_rate * ps.norm_fill_rate
                      + cfg_.weight_tri_arb * ps.norm_tri_arb)
                     / total_weight;
    }
}

void MarketAllocator::apply_allocation(std::vector<PairScore>& scores)
{
    const std::size_t n = scores.size();
    if (n == 0) return;

    // -- Step 1: raw allocation proportional to composite score.
    double sum = 0.0;
    for (const auto& ps : scores) sum += ps.composite;

    if (sum <= 0.0) {
        double eq = 1.0 / static_cast<double>(n);
        for (auto& ps : scores) ps.allocation = eq;
    } else {
        for (auto& ps : scores) ps.allocation = ps.composite / sum;
    }

    // -- Step 2: iterative projection onto [min, max] with sum=1.0.
    //    Freeze pairs at their bound, redistribute remainder to free pairs.
    //    Skip for single pair — it always gets 100%.
    std::vector<bool> frozen(n, false);
    if (n > 1) for (int iter = 0; iter < 20; ++iter) {
        bool changed = false;
        double frozen_sum = 0.0;
        double free_composite_sum = 0.0;

        // Identify and freeze violated pairs.
        for (std::size_t i = 0; i < n; ++i) {
            if (frozen[i]) {
                frozen_sum += scores[i].allocation;
                continue;
            }
            if (scores[i].allocation < cfg_.min_alloc_pct) {
                scores[i].allocation = cfg_.min_alloc_pct;
                frozen[i] = true;
                frozen_sum += cfg_.min_alloc_pct;
                changed = true;
            } else if (scores[i].allocation > cfg_.max_alloc_pct) {
                scores[i].allocation = cfg_.max_alloc_pct;
                frozen[i] = true;
                frozen_sum += cfg_.max_alloc_pct;
                changed = true;
            }
        }

        if (!changed) break;

        // Compute free pairs' composite sum.
        for (std::size_t i = 0; i < n; ++i) {
            if (!frozen[i]) free_composite_sum += scores[i].composite;
        }

        double remaining = 1.0 - frozen_sum;
        if (remaining <= 0.0 || free_composite_sum <= 0.0) {
            // All frozen — distribute remaining equally among free pairs.
            int free_count = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (!frozen[i]) ++free_count;
            }
            if (free_count > 0 && remaining > 0.0) {
                double each = remaining / static_cast<double>(free_count);
                for (std::size_t i = 0; i < n; ++i) {
                    if (!frozen[i]) scores[i].allocation = each;
                }
            } else if (remaining > 1e-9) {
                // All pairs are frozen but allocations don't sum to 1.0.
                // Distribute the surplus proportionally to pairs below
                // their max cap so the sum-to-one invariant is preserved.
                double headroom_sum = 0.0;
                for (std::size_t i = 0; i < n; ++i) {
                    headroom_sum += cfg_.max_alloc_pct - scores[i].allocation;
                }
                if (headroom_sum > 1e-12) {
                    for (std::size_t i = 0; i < n; ++i) {
                        double hr = cfg_.max_alloc_pct - scores[i].allocation;
                        scores[i].allocation += remaining * (hr / headroom_sum);
                    }
                } else {
                    // Truly stuck — spread equally as last resort.
                    double each = remaining / static_cast<double>(n);
                    for (auto& ps : scores) ps.allocation += each;
                }
            }
            break;
        }

        // Redistribute remaining proportionally among free pairs.
        for (std::size_t i = 0; i < n; ++i) {
            if (!frozen[i]) {
                scores[i].allocation = remaining
                    * (scores[i].composite / free_composite_sum);
            }
        }
    }

    // -- Step 3: hysteresis — only accept the new allocation if composites
    //    have changed by more than hysteresis_bps from the previous eval.
    if (!allocations_.empty()) {
        for (auto& ps : scores) {
            auto prev_it = allocations_.find(ps.pair_name);
            if (prev_it == allocations_.end()) continue;

            double prev_alloc = prev_it->second;
            double change_bps = std::abs(ps.allocation - prev_alloc) * 10000.0;

            if (change_bps < cfg_.hysteresis_bps) {
                ps.allocation = prev_alloc;
            } else {
                ps.allocation = cfg_.smooth_alpha * ps.allocation
                              + (1.0 - cfg_.smooth_alpha) * prev_alloc;
            }
        }

        // Re-normalise after smoothing.
        double total = 0.0;
        for (const auto& ps : scores) total += ps.allocation;
        if (total > 0.0) {
            for (auto& ps : scores) ps.allocation /= total;
        }
    }
}

}  // namespace xop
