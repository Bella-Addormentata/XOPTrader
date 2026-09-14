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
// [STEP6-CAUSE 2026-09-13] LimitBlockWarnGate (below) is the one stateful
// type in this header.  It is caller-owned state -- the engine keeps one per
// pair -- and PreTradeCheck itself stays stateless.
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
#include <string_view>
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
    bool        soft_limit_breached;  // concentration >= the effective per-pair soft limit
    bool        hard_limit_breached;  // concentration >= the effective per-pair hard limit

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
// [PACE D1 2026-09-13] Per-pair concentration limits.
//
// The concentration rule read risk.soft_limit_pct / risk.hard_limit_pct
// directly, so no pair could be given more room than another.  A pair may
// now carry soft_limit_pct_override / hard_limit_pct_override (PairConfig).
// effective_concentration_limits() resolves them, and the PreTradeCheck
// overloads that take a ConcentrationLimits apply them to THAT pair only.
// The overloads without one forward the global pair, so every existing
// caller computes exactly what it computed before.
// ---------------------------------------------------------------------------

/// Concentration thresholds in force for ONE pair.
struct ConcentrationLimits {
    double soft_limit_pct{0.60};
    double hard_limit_pct{0.80};
};

/// `pair`'s soft/hard overrides where present, else `risk`'s.  Defence in
/// depth (load_config already rejects these): unless the result is finite
/// with 0 < soft < hard <= 1, the global pair is returned unchanged.
/// nullptr -> global.
[[nodiscard]] ConcentrationLimits effective_concentration_limits(
    const RiskConfig& risk, const PairConfig* pair) noexcept;

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
// [STEP6-CAUSE 2026-09-13] Per-side attribution for PreTradeCheck's limits.
//
// apply_limits() returns nullopt when BOTH sizes are zero after its checks,
// which does not mean a limit blocked both sides: a side that arrives at 0
// passes every rule untouched.  On 2026-09-13 the live XCH/BYC bid arrived
// at 0 from the strategy (24.57 XCH of total holdings against q_max 20,
// avellaneda.cpp:268-270) and only the ask was cut -- to 0, by the single-CAT
// cap's full block.  The Step 6 warn still said "both sides blocked by risk
// limits", because it printed pair-level breach flags from
// get_limit_status(), an independent recomputation that cannot tell which
// rule touched which side.
//
// evaluate_limits() runs the identical arithmetic and records, per side, the
// size handed in, the size returned, every rule that LOWERED it, and the rule
// that took it from >0 to 0.
// ---------------------------------------------------------------------------

/// A PreTradeCheck limit rule.  The values are bits so one side can record
/// several rules.  On one side at most one concentration rule applies (hard
/// or soft), then the single-CAT cap, then the pair-capital cap.
enum class LimitRule : std::uint8_t {
    None              = 0,
    SoftConcentration = 1,
    HardConcentration = 2,
    SingleCatCap      = 4,
    PairCapitalCap    = 8
};

/// snake_case label for logs ("soft_concentration", "single_cat_cap", ...).
[[nodiscard]] const char* to_string(LimitRule r) noexcept;

[[nodiscard]] constexpr std::uint8_t limit_rule_bit(LimitRule r) noexcept
{
    return static_cast<std::uint8_t>(r);
}

[[nodiscard]] constexpr bool has_limit_rule(std::uint8_t mask, LimitRule r) noexcept
{
    return (mask & limit_rule_bit(r)) != 0;
}

/// Why one side of a quote is, or is not, zero after the limits.
enum class SideZeroCause : std::uint8_t {
    NotZero          = 0,  ///< the side left the limits with a positive size
    ZeroBeforeLimits = 1,  ///< it ARRIVED at 0 mojos, so no limit acted on it
    LimitZeroed      = 2   ///< it arrived positive and a limit took it to 0
};

/// What the limits did to one side of a quote.
struct SideLimitTrace {
    Mojo         pre_size{0};                 ///< size handed in (mojos)
    Mojo         post_size{0};                ///< size returned (mojos)
    std::uint8_t reduced_by{0};               ///< limit_rule_bit() of every rule that LOWERED this side
    LimitRule    zeroed_by{LimitRule::None};  ///< the rule that took it from >0 to 0
};

[[nodiscard]] SideZeroCause side_zero_cause(const SideLimitTrace& side) noexcept;

/// Both sides, plus the fractions the rules compared against their caps.
struct LimitsTrace {
    SideLimitTrace bid{};
    SideLimitTrace ask{};
    double base_concentration{0.5};     ///< base / (base + quote), XCH-marked
    double quote_concentration{0.5};    ///< 1 - base_concentration
    bool   base_is_cat{false};          ///< the base-side single-CAT rule was evaluated
    double base_cat_fraction{0.0};      ///< base asset / portfolio (0 unless base_is_cat)
    bool   quote_is_cat{false};         ///< the quote-side single-CAT rule was evaluated
    double quote_cat_fraction{0.0};     ///< quote asset / portfolio (0 unless quote_is_cat)
    double cat_full_block_pct{0.0};     ///< single_cat_cap_pct * the full-block multiple: the CAT taper is 0 from here
    double pair_capital_fraction{0.0};  ///< (base + quote) / portfolio
};

/// evaluate_limits()'s result.  has_quote is false exactly when apply_limits()
/// returns nullopt for the same inputs, and quote is then value-initialised.
/// A bool plus a Quote rather than std::optional<Quote>: Quote has no default
/// member initialisers, and copying a disengaged optional of it is the
/// pattern GCC's Release/LTO -Wmaybe-uninitialized flags.
struct LimitsDecision {
    bool        has_quote{false};
    Quote       quote{};
    LimitsTrace trace{};
};

/// "<pre> -> <post> (<reason>)" for one side.  Sizes print in base units to
/// 6 dp at base_mojos_per_unit, or as raw mojos when that is not positive.
[[nodiscard]] std::string describe_side_limits(const SideLimitTrace& side,
                                               std::int64_t base_mojos_per_unit);

/// Both sides, the fractions, and the strategy's q and q_max.
[[nodiscard]] std::string describe_limits_block(const LimitsTrace& limits_trace,
                                                std::int64_t base_mojos_per_unit,
                                                double strategy_q,
                                                double strategy_q_max);

/// Step 6's whole no-quote line; the engine adds only its "[Engine] " tag.
/// Built here, not in engine.cpp, so a test pins what the operator reads.
[[nodiscard]] std::string format_step6_no_quote(std::string_view pair_name,
                                                const LimitsTrace& limits_trace,
                                                std::int64_t base_mojos_per_unit,
                                                double strategy_q,
                                                double strategy_q_max,
                                                const RiskConfig& risk_cfg,
                                                BlockHeight reminder_blocks);

/// [PACE D1 2026-09-13] The same line, printing `conc_limits` as cfg_soft /
/// cfg_hard: the limits evaluate_limits() actually applied to this pair.
/// The overload above forwards risk_cfg's own soft/hard, so a pair without
/// overrides prints exactly what it printed before.
[[nodiscard]] std::string format_step6_no_quote(std::string_view pair_name,
                                                const LimitsTrace& limits_trace,
                                                std::int64_t base_mojos_per_unit,
                                                double strategy_q,
                                                double strategy_q_max,
                                                const RiskConfig& risk_cfg,
                                                const ConcentrationLimits& conc_limits,
                                                BlockHeight reminder_blocks);

/// Step 6's info line for the first quote after a no-quote warn.
[[nodiscard]] std::string format_step6_quote_resumed(std::string_view pair_name,
                                                     BlockHeight last_warn_height,
                                                     BlockHeight reminder_blocks);

/// What the no-quote warn is keyed on: each side's cause and zeroing rule.
/// Fractions and reduced_by are deliberately left out, so values drifting
/// within one episode (live BYC moved between 0.506 and 0.509) do not
/// re-warn; the reminder prints whatever values are current then.
struct LimitBlockSignature {
    SideZeroCause bid_cause{SideZeroCause::NotZero};
    LimitRule     bid_zeroed_by{LimitRule::None};
    SideZeroCause ask_cause{SideZeroCause::NotZero};
    LimitRule     ask_zeroed_by{LimitRule::None};

    friend constexpr bool operator==(const LimitBlockSignature&,
                                     const LimitBlockSignature&) = default;
};

[[nodiscard]] LimitBlockSignature limit_block_signature(const LimitsTrace& limits_trace) noexcept;

/// Reminder cadence for a persisting no-quote episode: 192 peak-height
/// blocks, about 1 hour at 4,608 blocks/day (18.75 s per block).
inline constexpr BlockHeight kLimitBlockWarnReminderBlocks{192};

/// [STEP6-CAUSE 2026-09-13] Rate limit for Step 6's per-pair no-quote warn.
///
/// The warn used to fire on every heartbeat: 556 XCH/BYC lines between the
/// 2026-09-12 23:24 restart and 04:45, about 106 an hour.  Modelled on
/// BreakerRealertGate (risk/drawdown_breaker.hpp:361-385), keyed on chain
/// height instead of wall time.  It warns on the first blocked heartbeat, at
/// once when the per-side cause changes, and otherwise every reminder_blocks.
///
/// It deliberately has NO clear().  Heartbeats where the pair quotes never
/// reach should_warn(), so a pair flapping between a quote and a same-cause
/// block warns at most once per window instead of re-arming into the old
/// per-heartbeat spam.  Live BYC sits 0.006-0.009 past the CAT cap's 0.500
/// full-block point, where small moves could flap, although none was
/// measured: all 556 heartbeats from the 23:24 restart to 04:45 warned.
/// The end of an episode is announced instead by should_log_recovery(),
/// once per warn.
///
/// Heights only increase within a process (engine.cpp:2812).  A smaller
/// height would wrap the unsigned difference and warn at once, which is the
/// loud direction.
class LimitBlockWarnGate {
public:
    /// True when the no-quote line should log at warn level now.  Marks the
    /// gate as warned at `height`, and arms the recovery line, when it is.
    [[nodiscard]] bool should_warn(const LimitBlockSignature& sig,
                                   BlockHeight height,
                                   BlockHeight reminder_blocks) noexcept;

    /// Call when the pair DID get a quote through the limits.  True on the
    /// first such heartbeat after a warn, then false until the next warn, so
    /// a flapping pair cannot turn the recovery line into spam.
    [[nodiscard]] bool should_log_recovery() noexcept;

    /// Height of the most recent warn (0 before the first).
    [[nodiscard]] BlockHeight last_warn_height() const noexcept { return last_warn_height_; }

private:
    bool                fired_{false};
    bool                recovery_pending_{false};
    LimitBlockSignature last_sig_{};
    BlockHeight         last_warn_height_{0};
};

// ---------------------------------------------------------------------------
// PreTradeCheck -- stateless risk gate applied to every quoting cycle.
//
// Usage (per-block heartbeat):
//
//     Quote q = strategy.compute_quotes(...);
//     q = risk.enforce_no_loss(q, cost_basis, enable_constraint);
//     const LimitsDecision decision =
//         risk.evaluate_limits(q, base_id, quote_id, state);
//     if (!decision.has_quote) {
//         // both sizes zero after limits -- skip this cycle.  For each of
//         // decision.trace.bid and .ask, side_zero_cause() says whether it
//         // arrived at 0 or a limit zeroed it, and zeroed_by names the rule.
//     }
//     offer_manager.post(decision.quote);
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

    /// [STEP6-CAUSE 2026-09-13] The checks apply_limits() documents below,
    /// plus a per-side record of what each rule did.  decision.has_quote and
    /// decision.quote are EXACTLY apply_limits()'s result for the same inputs
    /// (apply_limits delegates here), so the attribution cannot drift from
    /// the quote.  A side that arrives at 0 is never attributed to a rule:
    /// its cause reads ZeroBeforeLimits whatever fires afterwards.
    [[nodiscard]]
    LimitsDecision evaluate_limits(Quote          quote,
                                   const AssetId& base_id,
                                   const AssetId& quote_id,
                                   const State&   state) const;

    /// [PACE D1 2026-09-13] evaluate_limits() with `limits` in place of the
    /// global soft/hard concentration thresholds, on BOTH the base-overweight
    /// bid and the quote-overweight ask.  The single-CAT and pair-capital caps
    /// stay global.  The overload above forwards
    /// ConcentrationLimits{soft_limit_pct, hard_limit_pct} of the RiskConfig.
    [[nodiscard]]
    LimitsDecision evaluate_limits(Quote                      quote,
                                   const AssetId&             base_id,
                                   const AssetId&             quote_id,
                                   const State&               state,
                                   const ConcentrationLimits& limits) const;

    /// Apply soft-limit, hard-limit, single-CAT cap, and max-capital-per-pair
    /// checks.  Returns a (possibly modified) quote, or std::nullopt when BOTH
    /// sizes are zero after the checks.
    ///
    /// nullopt does NOT mean a limit blocked both sides.  A side the strategy
    /// already sized at 0 plus a side a limit zeroed also yields nullopt (the
    /// live XCH/BYC case on 2026-09-13).  Use evaluate_limits() to learn which
    /// rule, if any, zeroed each side.
    ///
    /// Modifications when limits are breached:
    ///   - Soft limit (60%): progressively reduce the overweight side.
    ///   - Hard limit (80%): reduce the overweight side to a tiny continuity
    ///     quote, tapering to zero only as concentration approaches 100%.
    ///   - Single CAT cap: if a CAT exceeds the configured portfolio cap,
    ///     reduce the accumulation side to a tiny continuity quote, tapering
    ///     to zero at twice the cap (the full-block multiple).
    ///   - Max capital per pair: taper both sizes to a tiny continuity quote
    ///     when pair capital exceeds the configured cap.
    ///
    /// @param quote      Two-sided quote.
    /// @param pair_name  Human-readable pair label for logging.
    /// @param base_id    Base asset identifier.
    /// @param quote_id   Quote asset identifier.
    /// @param state      Current bot state (positions, markets).
    /// @return           Filtered quote, or nullopt if both sizes are zero.
    [[nodiscard]]
    std::optional<Quote> apply_limits(Quote              quote,
                                      const std::string& pair_name,
                                      const AssetId&     base_id,
                                      const AssetId&     quote_id,
                                      const State&       state) const;

    /// [PACE D1 2026-09-13] apply_limits() with this pair's effective
    /// concentration limits; delegates to the 5-argument evaluate_limits().
    /// The overload above forwards the RiskConfig's own soft/hard.
    [[nodiscard]]
    std::optional<Quote> apply_limits(Quote                      quote,
                                      const std::string&         pair_name,
                                      const AssetId&             base_id,
                                      const AssetId&             quote_id,
                                      const State&               state,
                                      const ConcentrationLimits& limits) const;

    /// [PACE D1 2026-09-13] The keep fraction the concentration rule scales
    /// the overweight side by: nullopt below the soft limit (and for NaN),
    /// otherwise exactly the soft-band or hard-band expression
    /// evaluate_limits() applied before per-pair limits existed.
    [[nodiscard]]
    static std::optional<double> concentration_keep_fraction(
        double concentration, const ConcentrationLimits& limits) noexcept;

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
    /// @param window  Consider only the most recent `window` entries
    ///                (one entry per block).  0 scans the ENTIRE retained
    ///                history -- the pre-S12 behaviour, under which a single
    ///                junk print pinned the running maximum for the life of
    ///                the ~1000-entry buffer and, because the Crash branch
    ///                consults this before the stability checks, held a
    ///                GLOBAL posting halt for hours after the market had
    ///                retraced.  A "flash" crash is a recent event; the
    ///                window makes the detector say so.
    static bool check_flash_crash(const std::vector<Mojo>& price_history,
                                  double threshold = 0.20,
                                  std::size_t window = 0) noexcept;

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

    /// [PACE D1 2026-09-13] get_limit_status() with this pair's effective
    /// concentration limits in the soft/hard breach flags.  The overload
    /// above forwards the RiskConfig's own soft/hard.
    [[nodiscard]]
    LimitStatus get_limit_status(const AssetId&             base_id,
                                 const AssetId&             quote_id,
                                 const State&               state,
                                 const ConcentrationLimits& limits) const;

private:
    const RiskConfig&     risk_cfg_;

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
public:
    [[nodiscard]]
    static Mojo mark_to_xch(const Position& pos,
                             const State&    state) noexcept;

    /// Compute the concentration of base value relative to the sum of
    /// base and quote values (mark-to-market in XCH), returning [0, 1].
    /// Returns 0.5 if both values are zero (limits.cpp compute_concentration).
    ///
    /// Uses mark_to_xch() so that different assets are compared in a common
    /// numeraire rather than by raw mojo count.
    ///
    /// [PACE D1 2026-09-13] Public: the pace controller's wallet-truth
    /// concentration check (engine Step 7) runs it on wallet balances.
    [[nodiscard]]
    static double compute_concentration(const Position& base_pos,
                                        const Position& quote_pos,
                                        const State&    state) noexcept;
private:

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
