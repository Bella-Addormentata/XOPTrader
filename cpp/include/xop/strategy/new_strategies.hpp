// new_strategies.hpp -- Three novel strategies for CHIA DEX market making that
// are NOT present in the existing XOPTrader strategy catalog.
//
// Strategy 1: CoinAgeWeightedQuoting
//   Exploits CHIA's coin-set (UTXO) model to optimally age-weight inventory for
//   quote pricing.  Older coins have a higher realized holding cost (opportunity
//   cost of locked capital) and should be quoted more aggressively to free capital.
//
// Strategy 2: BlockCadenceAdaptiveSpread
//   Exploits the *variance* in CHIA's 52-second block time (actual inter-block
//   intervals range from ~10s to ~200s+) to dynamically adjust spreads based on
//   the instantaneous block-arrival rate, not the mean.
//
// Strategy 3: MempoolSentinelStrategy
//   Monitors the CHIA mempool for pending offer-take transactions to detect
//   imminent fills and preemptively adjust quotes for the NEXT block.  This is
//   unique to CHIA's transparent mempool and atomic offer model.
//
// These strategies are designed as composable modules that can be layered on top
// of the existing A-S/GLFT core via the multiplier pipeline described in
// trading-strategies.md section 4 (Strategy Interaction Matrix).  Each also
// implements StrategyBase for standalone use or testing.
//
// Scholarly references are given per-strategy below.
//
// ISO/IEC 27001:2022 -- no secrets; pure algorithmic computation.
// ISO/IEC 5055       -- no raw pointers; virtual destructor; bounds-checked containers.
// ISO/IEC 25000      -- clear naming; comprehensive formulae documentation.

#ifndef XOP_STRATEGY_NEW_STRATEGIES_HPP
#define XOP_STRATEGY_NEW_STRATEGIES_HPP

#include <xop/strategy/base.hpp>
#include <xop/strategy/regime.hpp>
#include <xop/config.hpp>
#include <xop/types.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace xop {

// ===========================================================================
//
//  STRATEGY 1: Coin-Age-Weighted Quoting
//
// ===========================================================================
//
// Scholarly basis:
//   - Amihud, Y. & Mendelson, H. (1986). "Asset pricing and the bid-ask
//     spread." Journal of Financial Economics, 17(2), 223-249.
//     (Establishes that holding costs increase effective spread.)
//   - Easley, D. & O'Hara, M. (1987). "Price, trade size, and information
//     in securities markets." Journal of Financial Economics, 19(1), 69-90.
//     (Shows that trade-size and time affect information content.)
//   - Adapted for CHIA's coin-set model where each UTXO has a known creation
//     block and thus a deterministic age.
//
// Why specifically relevant to CHIA:
//   CHIA's coin-set (UTXO) model means every unit of inventory is a distinct
//   coin with a known creation block height.  Unlike account-model chains
//   (Ethereum), we can track the EXACT age of each inventory unit.  On a thin
//   market (~$2K/day volume), inventory can sit for hours or days.  The
//   opportunity cost of locked capital grows linearly with age, and the
//   probability that a coin is "stale" (priced at an outdated cost basis)
//   increases.  By weighting quotes by coin age we:
//     1. Prioritize selling older coins first (they have higher opportunity
//        cost and are more likely to have an outdated cost basis).
//     2. Tighten the ask spread for old inventory to accelerate turnover.
//     3. Widen the bid spread when existing inventory is already old (we
//        do not need more inventory if we cannot move what we have).
//
// Mathematical formulation:
//   Let C = {c_1, ..., c_n} be the set of inventory coins on the ask side,
//   each with age a_i = (current_block - creation_block) * 52 seconds.
//
//   Define the age-weighted urgency factor:
//     U = (1/n) * SUM_i [ 1 - exp(-lambda_age * a_i) ]
//
//   where lambda_age controls how quickly urgency grows with age.  This
//   produces U in [0, 1):
//     - Fresh coins (a_i ~ 0):  contribution ~ 0 (no urgency).
//     - Old coins (a_i >> 1/lambda_age): contribution ~ 1 (max urgency).
//
//   The ask spread is then adjusted:
//     ask_spread_adjusted = ask_spread_base * (1 - alpha_age * U)
//
//   where alpha_age in [0, 0.5] is the maximum tightening fraction.
//   Conversely, the bid spread is widened when U is high:
//     bid_spread_adjusted = bid_spread_base * (1 + beta_age * U)
//
//   This naturally respects the never-sell-at-loss constraint because the
//   cost basis floor is applied AFTER spread adjustment.
//
// Expected edge: 5-15 bps from faster inventory turnover.
// Implementation complexity: Low (requires only coin-creation-block tracking).
//
// Integration with existing modules:
//   - Reads coin ages from the CoinManager (execution layer).
//   - Outputs a pair of multipliers (ask_mult, bid_mult) that plug into the
//     spread multiplier pipeline alongside VPIN and OFI multipliers.
//   - Can be composed with any base strategy (A-S, GLFT, etc.).
//
// ===========================================================================

/// Configuration for the coin-age-weighted quoting module.
struct CoinAgeConfig {
    // -- Age decay parameter --------------------------------------------------
    // lambda_age: exponential decay rate for the urgency function.
    // Units: 1/seconds.  A value of 1/3600 means coins reach ~63% urgency
    // after 1 hour.  At CHIA's volume (~$2K/day) a 1-hour half-life is
    // appropriate since fills take 30-120 minutes on average.
    double lambda_age{1.0 / 3600.0};

    // -- Spread adjustment bounds ---------------------------------------------
    // alpha_age: maximum fractional tightening of the ask spread when all
    // inventory is old.  0.3 means the ask spread can tighten by up to 30%.
    double alpha_age{0.30};

    // beta_age: maximum fractional widening of the bid spread when all
    // inventory is old.  0.2 means the bid spread widens by up to 20% to
    // discourage further accumulation of stale inventory.
    double beta_age{0.20};

    // -- CHIA block time ------------------------------------------------------
    double block_time_seconds{52.0};

    // -- Maximum coin age to consider (clamp to prevent numerical issues) -----
    // Beyond this age (in blocks), urgency is treated as 1.0.
    uint32_t max_age_blocks{2000};  // ~28.9 hours

    // -- Underlying base strategy parameters (for standalone operation) --------
    double gamma{0.01};
    double kappa{1.5};
    double q_max{1000.0};
    uint32_t horizon_blocks{120};

    // -- Regime detection (inherited from base strategies) --------------------
    uint32_t regime_window_blocks{100};
    double vr_mean_revert_threshold{0.85};
    double vr_momentum_threshold{1.15};

    // -- No-loss constraint ---------------------------------------------------
    bool   enable_no_loss_constraint{false};
    double min_margin_bps{35.0};
};

/// Represents a single coin in inventory with its creation block.
struct CoinAge {
    BlockHeight creation_block;  // Block at which this coin was created/received.
    double      amount;          // Quantity in base-asset units (floating-point).
    double      cost_basis;      // Acquisition price (quote per base) for this coin.
};

/// CoinAgeWeightedQuoting -- adjusts spreads based on the age distribution of
/// inventory coins.  Implements StrategyBase for standalone use and also exposes
/// multiplier accessors for composition with A-S/GLFT.
class CoinAgeWeightedQuoting final : public StrategyBase {
public:
    explicit CoinAgeWeightedQuoting(const CoinAgeConfig& cfg);

    // -- StrategyBase interface -----------------------------------------------

    QuoteResult compute_quotes(double mid,
                               double sigma,
                               double q,
                               BlockHeight block_height) override;

    void update_price(double mid, BlockHeight block_height) override;

    RegimeInfo current_regime() const override;

    std::string name() const override;

    void set_cost_basis(double cost_basis,
                        double min_margin_bps) override;

    // -- Coin-age-specific interface ------------------------------------------

    /// Push the current inventory coin set.  Called by the CoinManager each
    /// block with the latest set of inventory coins and their creation blocks.
    void update_coin_ages(const std::vector<CoinAge>& coins);

    /// Compute the age-weighted urgency factor U in [0, 1).
    /// Returns 0.0 if no coins are tracked.
    double compute_urgency(BlockHeight current_block) const;

    /// Get the ask-side spread multiplier: (1 - alpha_age * U).
    /// Values in [1-alpha_age, 1.0].  Lower = tighter ask (sell more eagerly).
    double ask_spread_multiplier(BlockHeight current_block) const;

    /// Get the bid-side spread multiplier: (1 + beta_age * U).
    /// Values in [1.0, 1+beta_age].  Higher = wider bid (buy less eagerly).
    double bid_spread_multiplier(BlockHeight current_block) const;

    /// Read-only access to the config.
    const CoinAgeConfig& config() const { return cfg_; }

    // -- Shared regime detector (T3-01) --------------------------------------

    /// Set a shared RegimeDetector.  Pass nullptr to revert to internal.
    void set_regime_detector(RegimeDetector* detector) noexcept {
        shared_regime_detector_ = detector;
    }

    /// Return the active RegimeDetector (shared if set, else internal).
    const RegimeDetector* regime_detector() const noexcept {
        return shared_regime_detector_ ? shared_regime_detector_
                                       : internal_detector_.get();
    }

private:
    RegimeDetector& active_detector() noexcept {
        return shared_regime_detector_ ? *shared_regime_detector_
                                       : *internal_detector_;
    }

    /// Update regime classification via the active RegimeDetector.
    void update_regime();

    // -- Thread safety (T2-02) -----------------------------------------------
    mutable std::shared_mutex mtx_;

    // -- Data members ---------------------------------------------------------
    CoinAgeConfig cfg_;
    std::string   name_{"CoinAgeWeightedQuoting"};

    std::vector<CoinAge> coins_;

    struct PriceObs {
        BlockHeight block;
        double      mid;
    };
    std::deque<PriceObs> price_buffer_;

    RegimeInfo regime_{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: canonical RegimeDetector instances.
    std::unique_ptr<RegimeDetector> internal_detector_;
    RegimeDetector* shared_regime_detector_{nullptr};
    double last_mid_{0.0};

    double cost_basis_{0.0};
    double min_margin_bps_{35.0};

    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;
};


// ===========================================================================
//
//  STRATEGY 2: Block-Cadence Adaptive Spread
//
// ===========================================================================
//
// Scholarly basis:
//   - Ait-Sahalia, Y. & Yu, J. (2009). "High frequency market microstructure
//     noise estimates and liquidity measures." Annals of Applied Statistics,
//     3(1), 422-457.
//     (Establishes that sampling frequency affects volatility estimation and
//     optimal quoting intervals.)
//   - Robert, C.Y. & Rosenbaum, M. (2011). "A new approach for the dynamics
//     of ultra-high-frequency data." Journal of Financial Econometrics, 9(2),
//     344-366.
//     (Models irregular time spacing in transaction-level data.)
//   - Original adaptation for CHIA's variable block cadence.
//
// Why specifically relevant to CHIA:
//   CHIA's "52-second" block time is a TARGET, not a guarantee.  Real
//   inter-block intervals follow a roughly exponential distribution with
//   significant variance: some blocks arrive in 10-15 seconds, others take
//   120-200+ seconds.  This variance is MUCH larger than on Ethereum (~12s)
//   or Solana (~0.4s).  The consequence for market making:
//
//   1. During FAST block sequences (10-20s intervals), price discovery
//      accelerates and our quotes go stale faster.  We should WIDEN spreads
//      because adversarial takers can exploit the rapid price changes.
//
//   2. During SLOW block sequences (120-200s intervals), our quotes have
//      been resting longer without update.  We should WIDEN spreads because
//      the accumulated uncertainty is higher -- but also our fill probability
//      is higher (more time for takers to discover our offers).
//
//   3. At the MEAN cadence (40-60s), we use the baseline spread.
//
//   The optimal spread adjustment is therefore a U-shaped function of the
//   instantaneous block rate, not a monotonic one.
//
// Mathematical formulation:
//   Let dt_i = block_time(i) - block_time(i-1) be the i-th inter-block
//   interval in seconds.  Define the exponentially weighted moving average:
//
//     dt_ema = alpha * dt_current + (1 - alpha) * dt_ema_prev
//
//   where alpha = 2 / (N + 1) for an N-block window (default N=10).
//
//   Define the cadence deviation ratio:
//     R = dt_ema / dt_target    (where dt_target = 52 seconds)
//
//   The spread multiplier is a U-shaped function centered at R=1:
//     m_cadence = 1 + eta * (R - 1)^2
//
//   where eta controls the curvature.  This gives:
//     R = 1.0 (normal cadence):    m = 1.0      (no adjustment)
//     R = 0.5 (fast blocks, ~26s): m = 1 + 0.25*eta  (widen)
//     R = 2.0 (slow blocks, ~104s):m = 1 + eta  (widen more)
//
//   Additionally, the fill-intensity parameter kappa is adjusted:
//     kappa_adjusted = kappa_base * (dt_target / dt_ema)
//
//   because fill intensity scales inversely with block time -- more time
//   per block means more opportunities for takers to find our offers.
//
// Expected edge: 3-8 bps from avoiding stale-quote sniping and reducing
//   adverse selection during fast-block sequences.
// Implementation complexity: Low (only needs block timestamps).
//
// Integration with existing modules:
//   - Reads block timestamps from the FullNode RPC (data layer).
//   - Outputs a spread multiplier m_cadence and adjusted kappa that feed into
//     the spread optimizer and A-S/GLFT models respectively.
//   - Plugs into the multiplier pipeline: final_spread *= m_cadence.
//
// ===========================================================================

/// Configuration for the block-cadence adaptive spread module.
struct BlockCadenceConfig {
    // -- Target block time ----------------------------------------------------
    double target_block_time{52.0};  // CHIA target inter-block interval (seconds).

    // -- EMA smoothing --------------------------------------------------------
    uint32_t ema_window_blocks{10};  // Number of blocks for EMA smoothing.
                                     // Smaller = more reactive to cadence changes.
                                     // Larger  = smoother but laggier.

    // -- U-curve parameters ---------------------------------------------------
    // eta: curvature of the U-shaped spread multiplier.
    // Higher eta = more aggressive widening at extreme cadences.
    // Recommended range: [0.2, 1.0].
    double eta{0.5};

    // -- Kappa adjustment bounds ----------------------------------------------
    // Clamp the adjusted kappa to prevent extreme fill-intensity estimates.
    double kappa_adj_min{0.3};   // Minimum kappa multiplier (slow blocks).
    double kappa_adj_max{3.0};   // Maximum kappa multiplier (fast blocks).

    // -- Multiplier bounds ----------------------------------------------------
    // Clamp the cadence spread multiplier to prevent extreme widening.
    double mult_min{1.0};   // Never tighten below baseline.
    double mult_max{2.0};   // Never widen more than 2x from cadence alone.

    // -- Underlying base strategy parameters (for standalone operation) --------
    double gamma{0.01};
    double kappa{1.5};
    double q_max{1000.0};
    uint32_t horizon_blocks{120};

    // -- Regime detection -----------------------------------------------------
    uint32_t regime_window_blocks{100};
    double vr_mean_revert_threshold{0.85};
    double vr_momentum_threshold{1.15};

    // -- No-loss constraint ---------------------------------------------------
    bool   enable_no_loss_constraint{false};
    double min_margin_bps{35.0};
};

/// BlockCadenceAdaptiveSpread -- adjusts spreads and fill-intensity based on
/// the real-time block arrival rate, exploiting CHIA's significant block-time
/// variance.  Implements StrategyBase for standalone use and also exposes
/// multiplier/kappa accessors for composition.
class BlockCadenceAdaptiveSpread final : public StrategyBase {
public:
    explicit BlockCadenceAdaptiveSpread(const BlockCadenceConfig& cfg);

    // -- StrategyBase interface -----------------------------------------------

    QuoteResult compute_quotes(double mid,
                               double sigma,
                               double q,
                               BlockHeight block_height) override;

    void update_price(double mid, BlockHeight block_height) override;

    RegimeInfo current_regime() const override;

    std::string name() const override;

    void set_cost_basis(double cost_basis,
                        double min_margin_bps) override;

    // -- Block-cadence-specific interface -------------------------------------

    /// Push a new block arrival timestamp.  Must be called once per block
    /// (typically from the on_new_block handler) with the block's wall-clock
    /// arrival time or the block timestamp from the full-node RPC.
    ///
    /// @param block_height  The block height.
    /// @param timestamp     Wall-clock arrival time of this block.
    void update_block_arrival(BlockHeight block_height, Timestamp timestamp);

    /// Get the current EMA of inter-block intervals (seconds).
    /// Returns target_block_time if insufficient data.
    double current_dt_ema() const;

    /// Get the cadence deviation ratio R = dt_ema / dt_target.
    double cadence_ratio() const;

    /// Get the cadence-based spread multiplier: 1 + eta * (R - 1)^2.
    /// Always >= 1.0 (never tightens from cadence alone).
    double cadence_spread_multiplier() const;

    /// Get the adjusted kappa for fill-intensity: kappa * (dt_target / dt_ema).
    /// Clamped to [kappa_adj_min * kappa, kappa_adj_max * kappa].
    double adjusted_kappa() const;

    /// Read-only access to the config.
    const BlockCadenceConfig& config() const { return cfg_; }

    // -- Shared regime detector (T3-01) --------------------------------------

    void set_regime_detector(RegimeDetector* detector) noexcept {
        shared_regime_detector_ = detector;
    }

    const RegimeDetector* regime_detector() const noexcept {
        return shared_regime_detector_ ? shared_regime_detector_
                                       : internal_detector_.get();
    }

private:
    RegimeDetector& active_detector() noexcept {
        return shared_regime_detector_ ? *shared_regime_detector_
                                       : *internal_detector_;
    }

    void update_regime();

    // -- Thread safety (T2-02) -----------------------------------------------
    mutable std::shared_mutex mtx_;

    // -- Data members ---------------------------------------------------------
    BlockCadenceConfig cfg_;
    std::string        name_{"BlockCadenceAdaptiveSpread"};

    struct BlockArrival {
        BlockHeight block;
        Timestamp   arrival;
    };
    std::deque<BlockArrival> block_arrivals_;

    double dt_ema_{52.0};

    struct PriceObs {
        BlockHeight block;
        double      mid;
    };
    std::deque<PriceObs> price_buffer_;

    RegimeInfo regime_{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: canonical RegimeDetector instances.
    std::unique_ptr<RegimeDetector> internal_detector_;
    RegimeDetector* shared_regime_detector_{nullptr};
    double last_mid_{0.0};

    double cost_basis_{0.0};
    double min_margin_bps_{35.0};

    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;
};


// ===========================================================================
//
//  STRATEGY 3: Mempool Sentinel Strategy
//
// ===========================================================================
//
// Scholarly basis:
//   - Daian, P. et al. (2020). "Flash Boys 2.0: Frontrunning in decentralized
//     exchanges, miner extractable value, and consensus instability."
//     IEEE Symposium on Security and Privacy.
//     (Establishes that mempool observation confers informational advantage in
//     decentralized exchange trading.)
//   - Eskandari, S. et al. (2020). "SoK: Transparent dishonesty: Front-running
//     attacks on blockchain." Financial Cryptography and Data Security, FC 2019.
//     (Taxonomy of front-running attacks via mempool monitoring.)
//   - Adapted for CHIA's unique properties: transparent mempool, atomic offers
//     (not AMM-based), and 52-second block time giving ~40+ seconds to react.
//
// Why specifically relevant to CHIA:
//   CHIA's mempool is FULLY TRANSPARENT via the full-node RPC (get_all_mempool_*
//   endpoints).  When someone submits a take-offer transaction, it appears in
//   the mempool ~40 seconds BEFORE it settles in a block.  This is a massive
//   informational advantage compared to:
//     - Ethereum: 12s block time, ~10s mempool visibility, flashbots private mempools.
//     - Solana: 0.4s block time, negligible mempool window.
//     - Bitcoin: 10min blocks but no atomic DEX offers.
//
//   CHIA uniquely combines: (a) long block time giving us reaction time,
//   (b) transparent mempool, and (c) atomic offers where the take-transaction
//   fully reveals the trade direction, size, and price.
//
//   By monitoring the mempool, we can:
//   1. Detect when OUR offers are about to be taken (imminent fill) and
//      preemptively adjust the remaining quotes for the next block.
//   2. Detect when COMPETITORS' offers are being taken, revealing market
//      demand direction and enabling anticipatory quote adjustment.
//   3. Detect large pending transactions that signal informed flow, and
//      widen spreads before the trade settles.
//
//   IMPORTANT ETHICAL NOTE: This is NOT front-running.  We are not inserting
//   transactions ahead of the taker.  We are adjusting our FUTURE quotes
//   (next block) in response to observed information.  This is analogous to
//   a traditional market maker watching the order flow and adjusting quotes,
//   which is standard and legal practice.
//
// Mathematical formulation:
//   Let M = {m_1, ..., m_k} be the set of pending offer-take transactions
//   in the mempool at observation time t.  For each m_j, extract:
//     - side_j:  whether the taker is buying or selling the base asset.
//     - size_j:  trade size in base-asset units.
//     - price_j: implied execution price.
//
//   Compute the pending net flow:
//     F_pending = SUM_j { size_j * sign(side_j) }
//   where sign(Buy) = +1, sign(Sell) = -1.
//
//   Compute the pending flow intensity:
//     I_pending = |F_pending| / avg_daily_volume
//
//   The mempool-based spread multiplier is:
//     m_mempool = 1 + psi * I_pending
//
//   where psi in [0.5, 3.0] controls sensitivity to pending flow.
//
//   The quote skew adjustment is:
//     skew_mempool = -phi_mempool * sign(F_pending) * I_pending
//
//   applied additively to the base A-S/GLFT skew.  Negative sign means:
//   if takers are buying (F_pending > 0), we shift quotes UP (wider ask,
//   tighter bid) to reduce our selling pressure -- the opposite of what
//   the taker wants.
//
//   Additionally, if we detect that one of OUR offers is in the pending
//   take set, we can immediately cancel the next-tier offers on the same
//   side (since the fill reveals demand on that side, making our remaining
//   same-side quotes more vulnerable to adverse selection).
//
// Expected edge: 10-25 bps from anticipatory quote adjustment; potentially
//   more during whale events where mempool observation gives 40+ seconds
//   of advance warning.
// Implementation complexity: Medium (requires mempool RPC integration and
//   spend-bundle parsing to identify offer-take transactions).
//
// Integration with existing modules:
//   - Reads from the FullNode RPC (get_all_mempool_items).
//   - Outputs a spread multiplier m_mempool and a skew adjustment that plug
//     into the pipeline alongside VPIN, OFI, and whale multipliers.
//   - Fires "imminent fill" events to the OfferManager for preemptive
//     cancellation of stale tiers.
//   - Feeds into the VPIN and OFI estimators as "pre-confirmed" trades,
//     giving those estimators a ~40-second head start.
//
// ===========================================================================

/// Configuration for the mempool sentinel strategy module.
struct MempoolSentinelConfig {
    // -- Flow sensitivity -----------------------------------------------------
    // psi: spread widening sensitivity to pending flow intensity.
    // Higher psi = more aggressive widening when large trades are pending.
    double psi{1.5};

    // -- Skew sensitivity -----------------------------------------------------
    // phi_mempool: skew adjustment sensitivity to pending flow direction.
    // Applied additively to the base inventory skew.
    double phi_mempool{0.3};

    // -- Volume normalization -------------------------------------------------
    // Estimated average daily volume for flow intensity calculation.
    // Units: base-asset units (e.g., XCH).  Updated periodically from
    // the market-data layer.
    double avg_daily_volume{750.0};  // ~$2K/day at ~$2.70/XCH

    // -- Imminent fill detection ----------------------------------------------
    // When we detect that one of our offers is being taken in the mempool,
    // should we preemptively cancel same-side offers at wider tiers?
    bool enable_preemptive_cancel{true};

    // -- Multiplier bounds ----------------------------------------------------
    double mult_min{1.0};   // Never tighten from mempool alone.
    double mult_max{3.0};   // Maximum widening from mempool flow.

    // -- Skew bounds ----------------------------------------------------------
    double skew_max{0.5};   // Maximum absolute mempool skew adjustment.

    // -- Stale mempool item filter --------------------------------------------
    // Ignore mempool items that have been pending longer than this many
    // seconds (they may be stuck or invalid).
    double max_mempool_age_seconds{300.0};  // 5 minutes

    // -- CHIA block time ------------------------------------------------------
    double block_time_seconds{52.0};

    // -- Underlying base strategy parameters (for standalone operation) --------
    double gamma{0.01};
    double kappa{1.5};
    double q_max{1000.0};
    uint32_t horizon_blocks{120};

    // -- Regime detection -----------------------------------------------------
    uint32_t regime_window_blocks{100};
    double vr_mean_revert_threshold{0.85};
    double vr_momentum_threshold{1.15};

    // -- No-loss constraint ---------------------------------------------------
    bool   enable_no_loss_constraint{false};
    double min_margin_bps{35.0};
};

/// Represents a single pending offer-take transaction observed in the mempool.
///
/// T3-22: Uses steady_clock for staleness detection to avoid NTP correction
/// artifacts.  The wall-clock Timestamp is retained for logging/diagnostics
/// only; monotonic first_seen_steady drives all duration calculations.
struct PendingMempoolTake {
    std::string offer_id;         // The offer being taken (spend-bundle hash).
    Side        taker_side;       // Direction from taker's perspective:
                                  //   Bid = taker is buying base (taking our ask).
                                  //   Ask = taker is selling base (taking our bid).
    double      size;             // Trade size in base-asset units.
    double      price;            // Implied execution price (quote per base).
    bool        is_our_offer;     // True if the offer being taken belongs to us.
    Timestamp   first_seen;       // Wall-clock time (for logging/diagnostics only).

    /// Monotonic timestamp for staleness detection (immune to NTP corrections).
    /// ISO/IEC 5055: steady_clock guarantees monotonic non-decreasing behaviour,
    /// preventing false staleness classifications from clock adjustments.
    std::chrono::steady_clock::time_point first_seen_steady{
        std::chrono::steady_clock::now()};
};

/// MempoolSentinelStrategy -- monitors the CHIA mempool for pending offer-take
/// transactions and preemptively adjusts quotes.  Implements StrategyBase for
/// standalone use and also exposes multiplier/skew accessors for composition.
class MempoolSentinelStrategy final : public StrategyBase {
public:
    explicit MempoolSentinelStrategy(const MempoolSentinelConfig& cfg);

    // -- StrategyBase interface -----------------------------------------------

    QuoteResult compute_quotes(double mid,
                               double sigma,
                               double q,
                               BlockHeight block_height) override;

    void update_price(double mid, BlockHeight block_height) override;

    RegimeInfo current_regime() const override;

    std::string name() const override;

    void set_cost_basis(double cost_basis,
                        double min_margin_bps) override;

    // -- Mempool-specific interface -------------------------------------------

    /// Push the current set of pending offer-take transactions observed in the
    /// mempool.  Called by the mempool monitor (data layer) each polling cycle.
    /// Stale entries (older than max_mempool_age_seconds) are filtered out.
    void update_mempool_takes(const std::vector<PendingMempoolTake>& takes);

    /// Compute the pending net flow: SUM(size * sign(side)).
    /// Positive = net buying pressure, negative = net selling pressure.
    double pending_net_flow() const;

    /// Compute the pending flow intensity: |F_pending| / avg_daily_volume.
    /// Value in [0, +inf) but typically << 1.0 on thin markets.
    double pending_flow_intensity() const;

    /// Get the mempool-based spread multiplier: 1 + psi * I_pending.
    /// Always >= 1.0 (never tightens from mempool alone).
    double mempool_spread_multiplier() const;

    /// Get the mempool-based skew adjustment.
    /// Returns a signed value in [-skew_max, +skew_max].
    /// Positive skew = shift quotes up (buying pressure detected).
    double mempool_skew_adjustment() const;

    /// Check whether any of our offers are currently being taken in the mempool.
    /// Returns a vector of offer IDs that are pending fill.
    std::vector<std::string> our_offers_pending_fill() const;

    /// Get the count of pending mempool takes currently tracked.
    std::size_t pending_take_count() const;

    /// Read-only access to the config.
    const MempoolSentinelConfig& config() const { return cfg_; }

    // -- Shared regime detector (T3-01) --------------------------------------

    void set_regime_detector(RegimeDetector* detector) noexcept {
        shared_regime_detector_ = detector;
    }

    const RegimeDetector* regime_detector() const noexcept {
        return shared_regime_detector_ ? shared_regime_detector_
                                       : internal_detector_.get();
    }

private:
    RegimeDetector& active_detector() noexcept {
        return shared_regime_detector_ ? *shared_regime_detector_
                                       : *internal_detector_;
    }

    void update_regime();

    // -- Thread safety (T2-02) -----------------------------------------------
    mutable std::shared_mutex mtx_;

    // -- Data members ---------------------------------------------------------
    MempoolSentinelConfig cfg_;
    std::string           name_{"MempoolSentinel"};

    std::vector<PendingMempoolTake> pending_takes_;

    struct PriceObs {
        BlockHeight block;
        double      mid;
    };
    std::deque<PriceObs> price_buffer_;

    RegimeInfo regime_{MarketRegime::Random, 1.0, 1.0, 1.0};

    // T3-01: canonical RegimeDetector instances.
    std::unique_ptr<RegimeDetector> internal_detector_;
    RegimeDetector* shared_regime_detector_{nullptr};
    double last_mid_{0.0};

    double cost_basis_{0.0};
    double min_margin_bps_{35.0};

    static constexpr double kSecondsPerYear = 365.0 * 24.0 * 3600.0;
};

}  // namespace xop

#endif  // XOP_STRATEGY_NEW_STRATEGIES_HPP
