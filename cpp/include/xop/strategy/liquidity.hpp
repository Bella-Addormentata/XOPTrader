// liquidity.hpp -- Multi-tier liquidity provision engine for XOPTrader CHIA
//                  DEX market-making bot.
//
// Implements the hybrid AMM + Offer strategy from CHIA_MARKET_MAKER_STRATEGY.md
// Section 11 (Liquidity Provision Strategies) and Section 7 (Capital Allocation).
//
// The engine translates high-level strategy quotes into a concrete multi-level
// offer ladder, managing four capital layers:
//
//   | Layer          | Capital    | Strategy                          |
//   |----------------|------------|-----------------------------------|
//   | Foundation     | 30-40%     | TibetSwap AMM, passive fees       |
//   | Active Core    | 30-40%     | Dexie 4-tier bid/ask, inv-aware   |
//   | Opportunistic  | 10-15%     | Cross-venue arb                   |
//   | Reserve        | 10-15%     | Dry powder, emergency buying      |
//
// The Active Core layer is the primary output of this engine: a vector of
// TierQuote structs that the execution layer (offer_manager) converts into
// on-chain CHIA offers via the wallet RPC.
//
// Coin-set model implications (Section 4):
//   Each tier's offer locks specific UTXO coins.  The engine sizes tiers
//   according to pre-split coin denominations and ensures no single coin
//   backs offers on multiple tiers or venues.  Rebalancing cancels all
//   existing offers (spending their locked coins, ~52 s confirmation) then
//   re-posts a fresh ladder, avoiding stale-coin conflicts.
//
// Compliant with:
//   ISO/IEC 27001:2022  (no secrets, audit-friendly rebalance logging)
//   ISO/IEC 5055        (bounds-checked vectors, no UB on edge inputs)
//   ISO/IEC 25000       (clear naming, documented invariants)
//   ISO/IEC JTC 1/SC 22 (standard-conforming C++20)

#ifndef XOP_STRATEGY_LIQUIDITY_HPP
#define XOP_STRATEGY_LIQUIDITY_HPP

#include <xop/config.hpp>
#include <xop/types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace xop {

// TierQuote and RebalanceReason are defined in <xop/types.hpp> (unified).
// Both the strategy and execution layers share the same definitions.

// ---------------------------------------------------------------------------
// OrderBookGap -- a contiguous price range with no competing offers.
//
// Detected by analyse_order_book_gaps() from the active dexie order book.
// Used by compute_ladder() to dynamically shift tier spacing toward
// underserved price levels, capturing spread that competitors miss.
// ---------------------------------------------------------------------------

struct OrderBookGap {
    Side   side;         // Bid or Ask
    double low_bps;      // Lower bound of the gap (bps from mid)
    double high_bps;     // Upper bound of the gap (bps from mid)
    double center_bps;   // Midpoint of the gap (target for tier placement)
    double width_bps;    // Width of the gap (high - low)
};

/// Analyse competing offers to find price gaps on each side of the book.
///
/// For each side (bid/ask), the offers are sorted by distance from mid,
/// and gaps larger than min_gap_bps are identified.  The function returns
/// only gaps within the max_scan_bps range.
///
/// @param offers       Active competing offers (from MarketDataFeed).
/// @param mid          Current mid-price in mojos.
/// @param min_gap_bps  Minimum gap width to report (default 50 bps).
/// @param max_scan_bps Maximum distance from mid to scan (default 1500 bps).
/// @return Vector of OrderBookGap, sorted by width descending.
std::vector<OrderBookGap> analyse_order_book_gaps(
    const std::vector<CompetingOffer>& offers,
    std::int64_t                       mid,
    double                             min_gap_bps  = 50.0,
    double                             max_scan_bps = 1500.0);

// ---------------------------------------------------------------------------
// QuoteRecoveryPrice / floor_recovery_ask_price -- sigma-floored
// quote-recovery repricing (engine Step 8).
//
// [2026-08-01 adversarial review, finding 1] Quote-recovery used to reprice
// the tightest ask tier to third_party_best_ask * (1 - undercut) with no
// floor, AFTER every Step 7 guard including the uncertainty width-floor pass
// and the fair-value clamp -- re-anchoring that tier to the very order book
// the fair-value design distrusts.  A mispriced-low third-party ask inside
// the outlier band could pull the recovery tier arbitrarily far below the
// uncertainty floor, reproducing the 2026-08-01 sweep failure mode on that
// one tier.
//
// The recovery price is therefore floored at
//
//     floor_mid_mojos * (1 + min_half_spread_bps / 1e4)
//
// where floor_mid_mojos is Step 7's blended ladder centre and
// min_half_spread_bps is the same per-pair minimum half-spread Step 7's
// width-floor pass enforced (max of min profit margin, AMM fee, and
// k_sigma * combined sigma).  When that floor exceeds the third-party best
// ask, recovery cannot both undercut the book and respect the floor, so the
// caller must skip the repricing for that heartbeat: recovering liquidity
// must not price below the uncertainty floor.
// ---------------------------------------------------------------------------
struct QuoteRecoveryPrice {
    /// False -> do not reprice this heartbeat.  Set when there is no book
    /// reference (best ask <= 0), no Step 7 floor is available
    /// (floor_mid_mojos <= 0), or the floor exceeds the third-party best
    /// ask.  The other fields are meaningless when this is false.
    bool apply{false};

    /// The recovery price in mojos, already floored.  Only when `apply`.
    Mojo price{0};

    /// True when the sigma floor lifted the price above the plain undercut
    /// (diagnostic, for logging).
    bool floored{false};
};

/// @param third_party_best_ask Best third-party ask (pseudo-price mojos);
///                             <= 0 means no book reference -> skip.
/// @param undercut_bps         How far below best ask recovery wants to
///                             price; negative values are treated as zero.
/// @param floor_mid_mojos      Step 7's blended ladder centre in mojos;
///                             <= 0 means no floor available -> skip.
/// @param min_half_spread_bps  Step 7's per-pair uncertainty minimum
///                             half-spread; negative treated as zero.
QuoteRecoveryPrice floor_recovery_ask_price(Mojo   third_party_best_ask,
                                            double undercut_bps,
                                            Mojo   floor_mid_mojos,
                                            double min_half_spread_bps);

// ---------------------------------------------------------------------------
// width_floor_exempts_competitiveness -- reconcile the Step 7 uncertainty
// width floor with the Step 8 competitiveness guard.
//
// [2026-08-01 dark-pair fix] Observed live: a pair with a tight (~8 bps) but
// ~90% stale book gets a large uncertainty floor (~150 bps of fair-value
// sigma), so Step 7 correctly forces every tier ~150 bps from centre; Step 8
// then scores every tier as hopelessly uncompetitive against the 8 bps BBO
// and suppresses all of them.  Width floor says "quote wide"; guard says
// "wide is pointless"; the pair posts zero tiers every heartbeat.  The
// owner's directive is quote-wide-rather-than-silent on low-certainty
// markets, so a tier standing exactly where the floor PUT it must not be
// suppressed for uncompetitiveness alone.
//
// A tier is exempt when its actual distance from the Step 7 centre is
// within the width the pipeline mandated for it:
//
//     dist(price, centre)  <=  min_half_spread_bps
//                              + (tier_spread_bps - innermost_spread_bps)
//
// where tier_spread_bps is the half-spread Step 7 assigned this tier (the
// TierQuote::spread_bps field, which tracks the width-floor shift, gap
// adjustments, and inventory skew) and innermost_spread_bps is the smallest
// assigned half-spread on the SAME side of the full Step 7 ladder.  The
// parenthesised term is the ladder's shape offset for the tier, so the
// bound reads "the floor, plus this tier's place in the ladder shape".
//
// Consequences:
//   - Floor binding (shift delta > 0): tier i sits at
//     floor + (spacing_i - spacing_0), which equals the bound exactly ->
//     exempt.  The pair quotes wide instead of going dark.
//   - Healthy pair (floor < innermost spacing, no shift): tier i sits at
//     spacing_i, and the bound is floor + spacing_i - spacing_0
//     < spacing_i, so the exemption can NEVER fire -- byte-identical
//     behaviour to before this fix.
//   - A tier pushed beyond floor + shape (skew-widened side, stale
//     repricing, anything else) stays suppressible exactly as today.
//
// Only the uncompetitiveness suppression is waived; every other Step 8
// suppression reason (queue position, fees, sanity, exposure) is untouched.
// ---------------------------------------------------------------------------

/// @param tier_price           The tier's posted price in mojos.
/// @param tier_spread_bps      Half-spread Step 7 assigned this tier
///                             (TierQuote::spread_bps).
/// @param innermost_spread_bps Smallest assigned half-spread among the FULL
///                             Step 7 ladder's tiers on the same side.
/// @param centre_mojos         Step 7's blended ladder centre
///                             (PairCycleState::quote_mid_mojos); <= 0 means
///                             no centre available -> never exempt.
/// @param min_half_spread_bps  Step 7's per-pair uncertainty minimum
///                             half-spread (PairCycleState::
///                             quote_min_half_spread_bps); <= 0 -> never
///                             exempt.
/// @return True when the tier is at (or inside) its mandated width and must
///         not be suppressed for uncompetitiveness alone.
bool width_floor_exempts_competitiveness(Mojo   tier_price,
                                         double tier_spread_bps,
                                         double innermost_spread_bps,
                                         Mojo   centre_mojos,
                                         double min_half_spread_bps) noexcept;

/// Human-readable label for logging and Prometheus metric labels.
inline const char* to_string(RebalanceReason r) noexcept {
    switch (r) {
        case RebalanceReason::None:           return "None";
        case RebalanceReason::PriceMove:      return "PriceMove";
        case RebalanceReason::InventorySkew:  return "InventorySkew";
        case RebalanceReason::TTLExpired:     return "TTLExpired";
        case RebalanceReason::RegimeChange:   return "RegimeChange";
        case RebalanceReason::CompetitorMove: return "CompetitorMove";
        case RebalanceReason::ForcedRefresh:  return "ForcedRefresh";
        default:                              return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// LiquidityConfig -- tuning knobs for the multi-tier liquidity engine.
//
// Defaults are drawn from the strategy document examples:
//   - 4 tiers at [60, 200, 500, 1000] bps
//   - Size fractions [0.30, 0.25, 0.25, 0.20]
//   - phi (skew strength) = 1.5  (Section 5: typical [0.1, 2.0])
//   - Rebalance thresholds from Section 11 trigger table
// ---------------------------------------------------------------------------

struct LiquidityConfig {
    // -- Tier ladder parameters ---------------------------------------------

    /// Number of tiers per side (bid and ask).  Must match the lengths of
    /// tier_spacing_bps and tier_size_pct.
    std::uint32_t num_tiers{4};

    /// Spread from mid-price for each tier, in basis points.
    /// Length must equal num_tiers.
    /// Default: [60, 200, 500, 1000] bps  (strategy doc Section 11 example).
    std::vector<double> tier_spacing_bps{60.0, 200.0, 500.0, 1000.0};

    /// Fraction of allocated capital (bids) or inventory (asks) placed at
    /// each tier.  Length must equal num_tiers.  Values should sum to ~1.0.
    /// Default: [0.30, 0.25, 0.25, 0.20].
    std::vector<double> tier_size_pct{0.30, 0.25, 0.25, 0.20};

    // -- Inventory skew parameters ------------------------------------------

    /// Skew strength coefficient (phi).  Controls how aggressively the engine
    /// shifts tier spreads to rebalance inventory.
    ///   skew_multiplier = 1.0 +/- phi * (inventory_ratio - 0.5) / 0.5
    /// Higher phi => more aggressive shifting.  Typical range [0.1, 2.0].
    double phi{1.5};

    /// Inventory ratio above which the engine considers the portfolio
    /// long-biased.  Below (1 - threshold) is short-biased.
    double inventory_skew_threshold{0.60};

    // -- Rebalance trigger thresholds (Section 11 trigger table) ------------

    /// Price deviation threshold: rebalance if mid-price moved by more than
    /// this fraction since the last rebalance.  Default 2% (0.02).
    double price_deviation_threshold{0.02};

    /// Inventory skew ratio crossing this value triggers rebalance.
    /// Same as inventory_skew_threshold by default (0.60).
    double rebalance_skew_threshold{0.60};

    /// Maximum number of blocks an offer may remain outstanding before it
    /// is considered stale.  Default 69 blocks (~1 hour at 52 s/block).
    std::uint32_t offer_ttl_blocks{69};

    /// Volume spike factor: rebalance if current volume > this multiple
    /// of the rolling average.  Default 3.0x.
    double volume_spike_factor{3.0};

    /// Volatility spike factor: rebalance if current sigma > this multiple
    /// of the 7-day average sigma.  Default 2.0x.
    double volatility_spike_factor{2.0};

    // -- Dynamic tier spacing (gap-aware) -----------------------------------

    /// Enable dynamic tier spacing that shifts tiers toward detected order
    /// book gaps.  When enabled, the engine analyses competing offers to
    /// find underserved price ranges and adjusts tier_spacing_bps to place
    /// liquidity in those valleys.  tier_spacing_bps is still used as a
    /// baseline/fallback when no gaps are detected.
    /// Default: true.
    bool gap_aware_spacing{true};

    /// Minimum gap width (bps) in the competing order book to consider
    /// worth targeting.  Gaps narrower than this are ignored.  Default 50.
    double min_gap_bps{50.0};

    /// Maximum distance from mid (bps) to scan for gaps.  Default 1500.
    double max_gap_scan_bps{1500.0};

    /// Blend factor [0,1] controlling how strongly tiers shift toward gaps.
    /// 0.0 = use baseline tier_spacing_bps (no adjustment).
    /// 1.0 = shift fully to gap center (subject to ascending constraint).
    /// Default 0.6.
    double gap_blend_factor{0.6};

    // -- Adverse-selection-aware sizing -------------------------------------

    /// Enable adverse-selection-aware tier sizing.  On slow blockchains
    /// (Chia: ~52 s/block), tight quotes are picked off by informed traders
    /// before we can cancel.  This feature reduces tier 0 (tightest) size
    /// and redistributes capital to outer tiers where adverse selection risk
    /// is lower.
    /// Default: true.
    bool adverse_selection_sizing{true};

    /// Decay factor per tier for adverse-selection sizing.
    /// tier_0 gets decay^0 = 1.0 of its base size,
    /// tier_1 gets decay^(-1), tier_2 gets decay^(-2), etc.
    /// Lower values = more aggressive reallocation to outer tiers.
    /// Typical range: [0.5, 0.9].  Default 0.7.
    ///
    /// Example with 4 tiers and decay=0.7:
    ///   Raw weights: [1.0, 1/0.7, 1/0.49, 1/0.343] = [1.0, 1.43, 2.04, 2.92]
    ///   Normalised:  [0.135, 0.193, 0.275, 0.395]
    ///   (tier 0 drops from default 0.30 to 0.135; tier 3 rises to 0.395)
    double adverse_selection_decay{0.7};

    /// Sigma (volatility) threshold above which adverse selection sizing
    /// activates extra aggressively.  When sigma > this value, the decay
    /// factor is halved (more conservative sizing inner tiers).
    /// Compared against ANNUALIZED sigma.  Default 2.0 (200% annualized):
    /// the pre-warm-start defaults (0.05 here, 0.005 in config.yaml) were
    /// tuned while sigma was pinned at the 0.001 floor and the branch never
    /// fired; with honest sigma (measured 0.4-1.9 across pairs) they would
    /// fire permanently and silently swap the configured tier sizes for an
    /// outer-heavy [0.8..57.8]% profile.  See StrategyConfig::
    /// adverse_selection_sigma_threshold for the full calibration note.
    /// 0 = always use base decay.
    double adverse_selection_sigma_threshold{2.0};

    // -- Fill-rate-weighted adaptive tier sizing ----------------------------

    /// Enable fill-rate-adaptive tier sizing.  When enabled, historical
    /// per-tier fill rates from the offer_log are blended with the current
    /// tier_size_pct to allocate more capital to tiers that get taken more
    /// frequently.  Applied after adverse-selection sizing.
    /// Default: true.
    bool fill_rate_sizing{true};

    /// Blend factor [0,1] controlling how strongly tier sizing shifts toward
    /// empirical fill-rate weights.
    /// 0.0 = ignore fill rates (use adverse-selection sizing only).
    /// 1.0 = size entirely by fill rates (subject to min floor).
    /// Default 0.30 (30% weight on fill data, 70% on analytical sizing).
    double fill_rate_blend{0.30};

    /// Lookback window in hours for computing per-tier fill rates from
    /// the offer_log.  Longer windows provide more stable estimates but
    /// adapt more slowly to regime changes.  Default 24 hours.
    int fill_rate_lookback_hours{24};

    /// Minimum allocation fraction per tier when fill-rate sizing is active.
    /// Prevents any tier from being starved of capital, which would leave
    /// gaps in the order book.  Default 0.05 (5%).
    double fill_rate_min_pct{0.05};

    /// Per-tier fill rates from the database (runtime data, not YAML config).
    /// Populated by the engine before calling compute_ladder.
    /// Empty vector = no fill data available (skip fill-rate sizing).
    std::vector<double> tier_fill_rates;

    // -- Competitive anchor pricing -----------------------------------------

    /// When enabled, anchor Tier 0 on each side to the best competing
    /// offer (bid/ask) and space subsequent tiers outward from there,
    /// rather than spacing symmetrically around mid.  Falls back to
    /// mid-based spacing when no competing offers exist on a side.
    /// Default: false (use traditional mid-based spacing).
    bool competitive_anchor_enabled{false};

    /// Maximum distance (bps) the anchor point may deviate from mid.
    /// If the best competing offer is further than this, fall back to
    /// mid-based spacing.  Prevents anchoring to stale/garbage offers.
    /// Default: 500 bps.
    double competitive_anchor_max_distance_bps{500.0};

    /// Inter-tier stride (bps) when competitive anchor is active.
    /// Tier 0 lands at the anchor; Tier i lands at anchor ± i*stride.
    /// Default: 65 bps.
    double competitive_anchor_stride_bps{65.0};

    // -- Per-side book quality (runtime data, not YAML config) --------------
    //
    // [SIDEQUALITY 2026-09-01] Copied from MarketSnapshot by the engine
    // before each compute_ladder call, the same way tier_fill_rates is.
    // They exist because the competitive anchor's safety reference,
    // bbo_ref, is the MIDPOINT of the two sides -- so one dislocated side
    // moves the reference that is supposed to police it.
    //
    // Live XCH/BYC 2026-09-01 (BYC per XCH): honest bids at 1.5000 and a
    // junk ask stack from 4.9995 gave bbo_ref = 3.24975, 2.3x the solved
    // fair value of 1.41022765.  bid_cap = bbo_ref therefore did not bind
    // at all, and the anchor parked a bid at best_comp_bid + tick =
    // 1.5001 -- a ~6.3% overpay on every XCH bought, every cycle.
    //
    // Both default TRUE so every existing caller, test and backtest keeps
    // byte-identical behaviour: the tightened cap engages ONLY in the new
    // disqualified state, which no healthy book can enter.  That is
    // deliberate -- this is shared hot-path code for the pairs that are
    // actually earning, and the pre-existing bid_cap == bbo_ref rule for
    // healthy books is a separate decision that is NOT revisited here.
    // (Its history matters: an earlier min(bbo_ref, mid) was reverted
    // because it anchored zero bids whenever the blended mid sat below
    // best_comp_bid.  That revert is left standing for healthy books.)
    bool book_bid_side_anchor_ok{true};
    bool book_ask_side_anchor_ok{true};
};

// ---------------------------------------------------------------------------
// LiquidityEngine -- the multi-tier liquidity provision engine.
//
// Lifecycle (invoked once per block by the main engine loop):
//
//   1. Call should_rebalance() to check if the ladder needs refreshing.
//   2. If yes, call compute_ladder() with current market state.
//   3. The execution layer cancels all existing offers for the pair, then
//      creates new offers from the returned TierQuote vector.
//   4. After creation, call record_rebalance() to update internal tracking.
//
// The engine is stateful: it tracks the last rebalance price, block, and
// inventory ratio to evaluate trigger conditions.  One LiquidityEngine
// instance is created per trading pair.
//
// Thread safety: NOT thread-safe.  The caller serialises access per pair
// (one pair per engine instance, one engine per strategy thread).
// ---------------------------------------------------------------------------

class LiquidityEngine {
public:
    /// Construct for a specific trading pair with the given configuration.
    ///
    /// @param pair_name  Human-readable pair label (e.g. "XCH/wUSDC").
    /// @param cfg        Tier ladder and rebalance configuration.
    ///
    /// @throws std::invalid_argument if config vectors are inconsistent or
    ///         contain invalid values.
    LiquidityEngine(const std::string& pair_name,
                    const LiquidityConfig& cfg);

    // -- Core interface -----------------------------------------------------

    /// Generate the full multi-tier offer ladder for both bid and ask sides.
    ///
    /// For each tier i in [0, num_tiers):
    ///   bid_price[i] = mid * (1 - spread_bps[i] / 10000)
    ///   ask_price[i] = mid * (1 + spread_bps[i] / 10000)
    ///   bid_size[i]  = available_capital * tier_size_pct[i]
    ///   ask_size[i]  = available_inventory * tier_size_pct[i]
    ///
    /// The ladder is then passed through apply_inventory_skew() to shift
    /// tier spreads based on the current inventory ratio.
    ///
    /// @param mid                Mid-price in mojos.
    /// @param sigma              Annualised volatility (decimal fraction).
    /// @param inventory_ratio    Fraction of capital held in base asset [0,1].
    ///                           0.5 = balanced, >0.6 = long-biased,
    ///                           <0.4 = short-biased.
    /// @param available_capital  Total quote-asset mojos available for bids.
    /// @param available_inventory Total base-asset mojos available for asks.
    /// @param cfg                Configuration override; if not provided, uses
    ///                           the instance config set at construction.
    ///
    /// @return Vector of TierQuote, length = 2 * num_tiers (bids + asks).
    ///         Sorted by side (all bids first, then all asks), then by tier.
    std::vector<TierQuote> compute_ladder(
        std::int64_t mid,
        double       sigma,
        double       inventory_ratio,
        std::int64_t available_capital,
        std::int64_t available_inventory) const;

    /// Overload accepting an explicit LiquidityConfig for one-shot use
    /// (e.g. backtesting parameter sweeps).
    std::vector<TierQuote> compute_ladder(
        std::int64_t          mid,
        double                sigma,
        double                inventory_ratio,
        std::int64_t          available_capital,
        std::int64_t          available_inventory,
        const LiquidityConfig& cfg) const;

    /// Overload that accepts competing offers for gap-aware dynamic spacing.
    /// When cfg.gap_aware_spacing is true, analyses the order book for gaps
    /// and shifts tier spacing to fill underserved price ranges.
    /// When cfg.adverse_selection_sizing is true, redistributes tier sizes
    /// away from the tightest tier (highest adverse-selection risk) toward
    /// outer tiers.
    std::vector<TierQuote> compute_ladder(
        std::int64_t                       mid,
        double                             sigma,
        double                             inventory_ratio,
        std::int64_t                       available_capital,
        std::int64_t                       available_inventory,
        const std::vector<CompetingOffer>& competing_offers,
        const LiquidityConfig&             cfg) const;

    // -- Rebalance decision -------------------------------------------------

    /// Evaluate whether the current market state warrants a full tier
    /// recalculation and offer refresh.
    ///
    /// Checks five trigger conditions in priority order:
    ///   1. Price deviation  > threshold since last rebalance.
    ///   2. Inventory skew crossed the 60% threshold boundary.
    ///   3. Offers stale     > offer_ttl_blocks since last rebalance block.
    ///   4. Volume spike     > 3x rolling average.
    ///   5. Volatility spike > 2x 7-day average.
    ///
    /// @param current_block   Current Chia blockchain height.
    /// @param current_price   Current mid-price in mojos.
    /// @param current_vol     Current rolling volume in mojos.
    /// @param avg_vol         Rolling average volume in mojos.
    /// @param current_sigma   Current annualised volatility.
    /// @param avg_sigma       7-day average annualised volatility.
    /// @param inventory_ratio Current inventory ratio [0, 1].
    ///
    /// @return True if at least one trigger condition is met.
    bool should_rebalance(BlockHeight   current_block,
                          std::int64_t  current_price,
                          std::int64_t  current_vol,
                          std::int64_t  avg_vol,
                          double        current_sigma,
                          double        avg_sigma,
                          double        inventory_ratio) const;

    /// Return the reason for the most recent should_rebalance() == true.
    /// If should_rebalance() was never called or returned false, returns
    /// RebalanceReason::None.
    RebalanceReason get_rebalance_reason() const noexcept;

    /// Return a human-readable string for the most recent rebalance reason.
    const char* get_rebalance_reason_str() const noexcept;

    // -- Inventory skew adjustment ------------------------------------------

    /// Apply inventory-aware spread skewing to an existing tier ladder.
    ///
    /// When inventory_ratio > 0.6 (long-biased):
    ///   - Ask tiers tighten (reduced spread) to encourage selling.
    ///   - Bid tiers widen   (increased spread) to discourage buying.
    ///
    /// When inventory_ratio < 0.4 (short-biased):
    ///   - Bid tiers tighten to encourage buying.
    ///   - Ask tiers widen   to discourage selling.
    ///
    /// Skew multiplier for each side:
    ///   bid_mult = 1.0 + phi * (inventory_ratio - 0.5) / 0.5
    ///   ask_mult = 1.0 - phi * (inventory_ratio - 0.5) / 0.5
    ///
    /// When balanced (ratio near 0.5): both multipliers are ~1.0 (no shift).
    /// When long (ratio = 0.8): bid_mult > 1 (widen), ask_mult < 1 (tighten).
    /// When short (ratio = 0.2): bid_mult < 1 (tighten), ask_mult > 1 (widen).
    ///
    /// @param ladder          The unadjusted tier ladder to modify.
    /// @param inventory_ratio Fraction of capital in base asset [0, 1].
    /// @param phi             Skew strength coefficient.
    ///
    /// @return A new vector with skew-adjusted prices.  Sizes unchanged.
    static std::vector<TierQuote> apply_inventory_skew(
        const std::vector<TierQuote>& ladder,
        double inventory_ratio,
        double phi);

    // -- State tracking (call after successful offer placement) --------------

    /// Record that a rebalance cycle completed successfully.
    /// Updates internal tracking for subsequent should_rebalance() calls.
    ///
    /// @param block           Block height at which new offers were placed.
    /// @param mid_price       Mid-price at the time of rebalance (mojos).
    /// @param inventory_ratio Inventory ratio at the time of rebalance.
    void record_rebalance(BlockHeight  block,
                          std::int64_t mid_price,
                          double       inventory_ratio);

    // -- Accessors ----------------------------------------------------------

    /// Trading pair name this engine manages.
    const std::string& pair_name() const noexcept;

    /// Read-only access to the active configuration.
    const LiquidityConfig& config() const noexcept;

    /// Total number of rebalance cycles completed.
    std::uint64_t rebalance_count() const noexcept;

    /// Block height of the most recent rebalance (0 if never rebalanced).
    BlockHeight last_rebalance_block() const noexcept;

    /// Mid-price at the most recent rebalance (0 if never rebalanced).
    std::int64_t last_rebalance_price() const noexcept;

private:
    // -- Configuration ------------------------------------------------------

    std::string    pair_name_;  // e.g. "XCH/wUSDC"
    LiquidityConfig cfg_;

    // -- Rebalance state tracking -------------------------------------------

    /// Most recent rebalance reason (set by should_rebalance).
    mutable RebalanceReason last_reason_{RebalanceReason::None};

    /// Block height at which the last rebalance was executed.
    BlockHeight  last_rebalance_block_{0};

    /// Mid-price (mojos) at the time of the last rebalance.
    std::int64_t last_rebalance_price_{0};

    /// Inventory ratio at the time of the last rebalance.
    /// Used to detect skew threshold crossings.
    double       last_inventory_ratio_{0.5};

    /// Cumulative count of completed rebalance cycles.
    std::uint64_t rebalance_count_{0};

    // -- Internal helpers ---------------------------------------------------

    /// Validate that config vectors are consistent with num_tiers and contain
    /// physically meaningful values.  Throws std::invalid_argument on failure.
    static void validate_config(const LiquidityConfig& cfg);

    /// Build the raw (unskewed) tier ladder from config and market parameters.
    /// Called by the public compute_ladder methods.
    static std::vector<TierQuote> build_raw_ladder(
        std::int64_t          mid,
        std::int64_t          available_capital,
        std::int64_t          available_inventory,
        const LiquidityConfig& cfg);
};

}  // namespace xop

#endif  // XOP_STRATEGY_LIQUIDITY_HPP
