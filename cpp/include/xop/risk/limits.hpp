// limits.hpp -- Pre-trade risk checks for XOPTrader CHIA DEX market-making bot.
//
// Implements the inventory & risk management rules from Section 8 of the
// strategy document, including the CORE RULE: NEVER SELL AT A LOSS.
//
// Every quote the strategy engine produces passes through PreTradeCheck
// before reaching the offer manager.  The checks are deterministic and
// side-effect-free so that the same inputs always yield the same output,
// making the risk layer easy to unit-test and audit.
//
// Emergency playbook rules (flash crash, one-sided fills, congestion,
// exploit detection) are encoded as named circuit-breaker methods that
// the main loop invokes on each heartbeat.
//
// Thread safety: PreTradeCheck is stateless -- all mutable data lives in
// xop::State (which is thread-safe).  Callers may invoke these methods
// concurrently from any thread without synchronization.
//
// Compliant with:
//   ISO/IEC 27001:2022  (secure coding -- deterministic risk gates)
//   ISO/IEC 5055        (no unchecked arithmetic on monetary paths)
//   ISO/IEC 25000       (clear naming, documented invariants)

#ifndef XOP_RISK_LIMITS_HPP
#define XOP_RISK_LIMITS_HPP

#include "xop/types.hpp"
#include "xop/config.hpp"
#include "xop/state.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xop {

// ---------------------------------------------------------------------------
// LimitStatus -- per-pair diagnostic snapshot returned by get_limit_status().
// Each flag indicates whether the corresponding limit is currently breached.
// ---------------------------------------------------------------------------

struct LimitStatus {
    std::string pair_name;            // human-readable pair label
    AssetId     base_id;              // base asset identifier
    AssetId     quote_id;             // quote asset identifier

    // Inventory concentration (Section 8 -- Inventory Controls table).
    double      base_concentration;   // base_balance / (base_balance + quote_balance), [0,1]
    bool        soft_limit_breached;  // concentration >= soft_limit_pct (60%)
    bool        hard_limit_breached;  // concentration >= hard_limit_pct (80%)

    // Single-CAT cap (never exceed 12% of portfolio in any one CAT).
    double      cat_portfolio_pct;    // this CAT's value / total portfolio value, [0,1]
    bool        cat_cap_breached;     // cat_portfolio_pct >= single_cat_cap_pct

    // Capital per pair (max_capital_per_pair_pct).
    double      pair_capital_pct;     // capital in this pair / total capital, [0,1]
    bool        pair_cap_breached;    // pair_capital_pct >= max_capital_per_pair_pct

    // Flash-crash circuit breaker.
    bool        flash_crash_active;   // true while circuit breaker is engaged
};

// ---------------------------------------------------------------------------
// EmergencyRule -- named constants for the four playbook scenarios so that
// callers can inspect which rule fired.
// ---------------------------------------------------------------------------

enum class EmergencyRule : std::uint8_t {
    None            = 0,
    FlashCrash      = 1,   // >20% drop -- pull all quotes, hold everything
    OneSidedFills   = 2,   // all fills on one side -- widen opposite, never panic sell
    Congestion      = 3,   // network congestion -- increase rebalancing buffer to 25-30%
    ExploitDetected = 4    // smart-contract exploit -- cancel all offers, exit AMM
};

/// Human-readable label for logging.
const char* to_string(EmergencyRule r) noexcept;

// ---------------------------------------------------------------------------
// PreTradeCheck -- stateless risk gate applied to every quoting cycle.
//
// Usage (per-block heartbeat):
//
//     Quote q = strategy.compute_quotes(...);
//     q = risk.enforce_no_loss(q, cost_basis, enable_constraint);
//     auto checked = risk.apply_limits(q, pair, base, quote, state);
//     if (!checked) {
//         // hard limit breached on both sides -- skip this cycle
//     }
//     offer_manager.post(*checked);
//
// ---------------------------------------------------------------------------

class PreTradeCheck {
public:
    /// Construct with an immutable reference to the risk configuration.
    /// The RiskConfig must outlive this object (typically AppConfig lifetime).
    /// @throws std::invalid_argument if config yields invalid margin fraction.
    explicit PreTradeCheck(const RiskConfig& cfg,
                           const StrategyConfig& strat_cfg);

    // -- Core rule: NEVER SELL AT A LOSS ------------------------------------

    /// Enforce the never-sell-at-loss constraint on the ask price.
    ///
    /// If `enable_constraint` is true:
    ///     ask = max(optimal_ask, cost_basis + min_profit_margin)
    /// where min_profit_margin is derived from StrategyConfig::min_profit_margin_bps.
    ///
    /// If `enable_constraint` is false the quote is returned unchanged.
    /// The bid side is never modified by this function.
    ///
    /// @param quote          Two-sided quote from the strategy engine.
    /// @param cost_basis     Weighted-average cost basis for the base asset
    ///                       (mojos-of-quote per mojo-of-base).
    /// @param enable_constraint  Master switch (normally always true).
    /// @return               Modified quote with the ask floor applied.
    [[nodiscard]]
    Quote enforce_no_loss(Quote quote,
                          Mojo  cost_basis,
                          bool  enable_constraint) const noexcept;

    // -- Inventory and capital limits ---------------------------------------

    /// Apply soft-limit, hard-limit, single-CAT cap, and max-capital-per-pair
    /// checks.  Returns a (possibly modified) quote, or std::nullopt if the
    /// hard limit is breached on BOTH sides (which means no safe quote exists).
    ///
    /// Modifications when limits are breached:
    ///   - Soft limit (60%): zero the size on the overweight side to begin
    ///     aggressive skewing (strategy should already be skewing; this is
    ///     the backstop).
    ///   - Hard limit (80%): zero the size on the overweight side.
    ///   - Single CAT cap: if the base asset is a CAT above 12% of
    ///     portfolio, zero the bid size (stop accumulating).
    ///   - Max capital per pair: zero both sizes if pair capital exceeds
    ///     the configured cap.
    ///
    /// @param quote      Two-sided quote.
    /// @param pair_name  Human-readable pair label for logging.
    /// @param base_id    Base asset identifier.
    /// @param quote_id   Quote asset identifier.
    /// @param state      Current bot state (positions, markets).
    /// @return           Filtered quote, or nullopt if both sides blocked.
    [[nodiscard]]
    std::optional<Quote> apply_limits(Quote              quote,
                                      const std::string& pair_name,
                                      const AssetId&     base_id,
                                      const AssetId&     quote_id,
                                      const State&       state) const;

    // -- Flash-crash circuit breaker ----------------------------------------

    /// Detect whether a flash crash has occurred in the recent price history
    /// using a rolling max-drawdown algorithm.
    ///
    /// Algorithm:
    ///   Scan chronologically, maintaining a running maximum price.  At each
    ///   point, compute the drawdown from the running max.  If any drawdown
    ///   exceeds the threshold, a crash is detected.
    ///
    /// This detects early crashes even if the price later recovers to a new
    /// high (the previous global-max anchor missed those cases).
    ///
    /// The window should contain the last N block-level mid-prices (caller
    /// decides N; strategy doc says 100+ stable blocks for recovery).
    ///
    /// @param price_history  Chronologically ordered mid-prices (oldest first).
    /// @param threshold      Fractional drop that triggers the breaker (default 0.20).
    /// @return               true if a flash crash is detected.
    [[nodiscard]]
    static bool check_flash_crash(const std::vector<Mojo>& price_history,
                                  double threshold = 0.20) noexcept;

    // -- Emergency playbook helpers -----------------------------------------

    /// Determine whether block-level stability has been restored after a
    /// flash crash.  Returns true only when the last `required_stable_blocks`
    /// entries in `price_history` all fall within `stability_band` of the
    /// most recent price.
    ///
    /// @param price_history          Chronologically ordered mid-prices.
    /// @param required_stable_blocks Minimum consecutive stable blocks (default 100).
    /// @param stability_band         Maximum fractional deviation from latest
    ///                               price considered "stable" (default 0.05).
    /// @return true if the market is stable enough to resume quoting.
    [[nodiscard]]
    static bool is_stable_after_crash(const std::vector<Mojo>& price_history,
                                      std::size_t required_stable_blocks = 100,
                                      double stability_band = 0.05) noexcept;

    /// Compute the recommended rebalancing buffer multiplier during network
    /// congestion.  Normal operation returns 1.0; when `congested` is true
    /// the multiplier increases to the configured congestion factor (1.25-1.50).
    ///
    /// @param congested  true if the network is experiencing high mempool depth.
    /// @return           Multiplier to apply to the rebalancing reserve fraction.
    [[nodiscard]]
    double congestion_buffer_multiplier(bool congested) const noexcept;

    // -- Diagnostic ---------------------------------------------------------

    /// Build a LimitStatus snapshot for a single pair.
    [[nodiscard]]
    LimitStatus get_limit_status(const AssetId& base_id,
                                 const AssetId& quote_id,
                                 const State&   state) const;

private:
    const RiskConfig&     risk_cfg_;
    const StrategyConfig& strat_cfg_;

    /// Minimum profit margin expressed in mojos-per-mojo, derived from
    /// StrategyConfig::min_profit_margin_bps at construction time.
    /// margin_fraction_ = min_profit_margin_bps / 10'000.0
    double margin_fraction_;

    /// Congestion buffer multiplier (25-30% increase = 1.25-1.30).
    static constexpr double kCongestionMultiplier = 1.30;

    // -- Internal helpers ---------------------------------------------------

    /// Convert a raw mojo balance to XCH-equivalent mojos using mark-to-market
    /// pricing from the State's market snapshots.
    ///
    /// For "xch" positions the balance is already denominated in XCH mojos, so
    /// it passes through unchanged.  For CAT positions the mid price from the
    /// asset's XCH pair is used:
    ///   xch_value = balance * mid_price_mojos_per_xch_mojo
    ///
    /// Conservative fallback (ISO/IEC 27001:2022 -- fail-safe design):
    ///   When no mid price is available (market snapshot missing or mid == 0),
    ///   the raw mojo balance is returned as-is (1:1 with XCH mojos).  This
    ///   over-estimates the value of cheap CATs, causing risk limits to trigger
    ///   earlier -- the safe direction.
    ///
    /// @param pos    Position whose balance is being valued.
    /// @param state  Current bot state (provides market snapshots).
    /// @return       Balance expressed in XCH-equivalent mojos.
    [[nodiscard]]
    static Mojo mark_to_xch(const Position& pos,
                             const State&    state) noexcept;

    /// Compute the concentration of base value relative to the sum of
    /// base and quote values (mark-to-market in XCH), returning [0, 1].
    /// Returns 0.0 if both values are zero.
    ///
    /// Uses mark_to_xch() so that different assets are compared in a common
    /// numeraire rather than by raw mojo count.
    [[nodiscard]]
    static double compute_concentration(const Position& base_pos,
                                        const Position& quote_pos,
                                        const State&    state) noexcept;

    /// Compute what fraction of total portfolio value (mark-to-market in XCH)
    /// a single asset represents.  Returns 0.0 if total portfolio value is zero.
    [[nodiscard]]
    static double compute_portfolio_fraction(const Position&              asset_pos,
                                             const std::vector<Position>& all,
                                             const State&                 state) noexcept;

    /// Compute what fraction of total capital (mark-to-market in XCH) is
    /// deployed in a single pair.
    [[nodiscard]]
    static double compute_pair_capital_fraction(const Position&              base_pos,
                                                const Position&              quote_pos,
                                                const std::vector<Position>& all,
                                                const State&                 state) noexcept;
};

}  // namespace xop

#endif  // XOP_RISK_LIMITS_HPP
