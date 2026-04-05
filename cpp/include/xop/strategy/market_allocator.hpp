// market_allocator.hpp -- Dynamic capital allocation across trading pairs.
//
// Scores each enabled pair on five dimensions and computes a target capital
// allocation fraction per pair.  A hysteresis layer prevents oscillation
// between markets that are changing slowly (the "grass is greener" problem).
//
// Five scoring dimensions (equal weight by default, configurable):
//
//   1. Spread score:    wider net spread (ours-vs-competitor) → more profit
//                       per round trip.
//   2. Volume score:    higher 24h volume → more fill probability.
//   3. Competition score: fewer competing offers → less adverse selection.
//   4. Fill-rate score: higher historical fills/hour → proven demand.
//   5. Triangular-arb score: edges in the XCH↔wUSDC↔BYC triangle boost
//                             the pairs that participate in the arb cycle.
//
// The allocator outputs a [0, 1] weight per pair (sums to 1.0) which the
// engine multiplies by total available capital before passing to Step 7
// (generate_ladder).
//
// Guardrails to avoid pitfalls:
//   - min_alloc_pct:   every enabled pair retains at least this fraction
//                      (default 10%).  Keeps presence in thin markets.
//   - max_alloc_pct:   no pair can exceed this fraction (default 50%).
//   - hysteresis_bps:  new score must exceed current by this margin before
//                      the allocator shifts capital (default 50 bps).
//   - eval_interval:   re-score only every N blocks (default 50).
//   - smooth_alpha:    EMA blend factor (0, 1].  1.0 = instant switch.
//                      Default 0.2 = slow blend → 5-update effective lag.
//
// ISO/IEC 27001:2022 -- no secrets, deterministic scoring.
// ISO/IEC 5055       -- bounded containers, no UB.

#ifndef XOP_STRATEGY_MARKET_ALLOCATOR_HPP
#define XOP_STRATEGY_MARKET_ALLOCATOR_HPP

#include <xop/config.hpp>
#include <xop/types.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xop {

// Forward declarations.
class MarketDataFeed;
class Database;

// ---------------------------------------------------------------------------
// PairScore -- per-pair scoring breakdown (public for logging / GUI).
// ---------------------------------------------------------------------------

struct PairScore {
    std::string pair_name;

    // Raw dimension values (before normalisation).
    double raw_spread{0.0};         // Our achievable spread minus best competitor (bps).
    double raw_volume{0.0};         // 24h volume in base units.
    double raw_competition{0.0};    // 1 / (1 + num_competing_offers).
    double raw_fill_rate{0.0};      // Fills per hour (trailing window).
    double raw_tri_arb{0.0};        // Triangular arb edge for this pair (bps).

    // Normalised [0, 1] scores (relative within the pair set).
    double norm_spread{0.0};
    double norm_volume{0.0};
    double norm_competition{0.0};
    double norm_fill_rate{0.0};
    double norm_tri_arb{0.0};

    // Weighted composite score [0, 1].
    double composite{0.0};

    // Current allocation fraction [0, 1].
    double allocation{0.0};
};

// ---------------------------------------------------------------------------
// TriArbResult -- triangular arbitrage edge for the XCH↔wUSDC↔BYC triangle.
// ---------------------------------------------------------------------------

struct TriArbResult {
    double forward_edge_bps{0.0};   // XCH → wUSDC → BYC → XCH
    double reverse_edge_bps{0.0};   // XCH → BYC → wUSDC → XCH
    bool   has_edge{false};         // Either direction exceeds threshold.
};

// ---------------------------------------------------------------------------
// MarketAllocator -- the main class.
// ---------------------------------------------------------------------------

class MarketAllocator {
public:
    /// Construct with configuration and market data source.
    MarketAllocator(const MarketAllocatorConfig& cfg,
                    const MarketDataFeed*        market_data,
                    const Database*              db);

    /// Re-score all pairs and update allocations.
    /// Called from the engine whenever eval_interval_blocks triggers.
    /// @param pair_names   Enabled pair names.
    /// @param block_height Current block height.
    void evaluate(const std::vector<std::string>& pair_names,
                  BlockHeight                     block_height);

    /// Get current allocation fraction for a pair [0, 1].
    /// Returns min_alloc_pct if the pair is unknown or never evaluated.
    [[nodiscard]] double get_allocation(const std::string& pair_name) const;

    /// Get all current allocations (for GUI / metrics).
    [[nodiscard]] std::unordered_map<std::string, double>
    get_all_allocations() const;

    /// Get the full scoring breakdown for all pairs (for logging / GUI).
    [[nodiscard]] std::vector<PairScore> get_scores() const;

    /// Should the engine re-evaluate this block?
    [[nodiscard]] bool should_evaluate(BlockHeight block_height) const;

    /// Compute the triangular arbitrage edge in the XCH↔wUSDC↔BYC triangle.
    /// Public so Step 9 can also use it.
    [[nodiscard]] TriArbResult compute_tri_arb_edge() const;

    /// Read-only access to config.
    [[nodiscard]] const MarketAllocatorConfig& config() const noexcept {
        return cfg_;
    }

private:
    // -- Scoring helpers ------------------------------------------------------
    void score_spread(PairScore& ps) const;
    void score_volume(PairScore& ps) const;
    void score_competition(PairScore& ps) const;
    void score_fill_rate(PairScore& ps) const;
    void score_tri_arb(PairScore& ps, const TriArbResult& tri) const;
    void normalise_scores(std::vector<PairScore>& scores) const;
    void compute_composites(std::vector<PairScore>& scores) const;
    void apply_allocation(std::vector<PairScore>& scores);

    // -- Data sources ---------------------------------------------------------
    MarketAllocatorConfig cfg_;
    const MarketDataFeed* market_data_;     // Non-owning.
    const Database*       db_;              // Non-owning.

    // -- State ----------------------------------------------------------------
    BlockHeight                            last_eval_block_{0};
    std::vector<PairScore>                 scores_;
    std::unordered_map<std::string, double> allocations_;  // pair → [0,1]
};

}  // namespace xop

#endif  // XOP_STRATEGY_MARKET_ALLOCATOR_HPP
