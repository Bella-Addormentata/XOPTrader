// limits.cpp -- Pre-trade risk checks implementation.
//
// See limits.hpp for interface documentation and safety rationale.
//
// All monetary arithmetic uses Mojo (int64_t).  Fractional comparisons
// (concentration, portfolio fraction) are performed in double only AFTER
// the monetary values have been established, and the doubles are never
// used for further monetary computation.  This prevents floating-point
// drift from affecting order prices.
//
// Compliant with:
//   ISO/IEC 27001:2022  (deterministic risk gates, audit-friendly logging)
//   ISO/IEC 5055        (bounds-checked, no undefined behaviour)
//   ISO/IEC 25000       (single-responsibility functions, documented invariants)

#include "xop/risk/limits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>

namespace xop {

namespace {

constexpr double kHardLimitContinuityFloorPct = 0.05;
constexpr double kCatCapContinuityFloorPct = 0.05;
constexpr double kCatCapFullBlockMultiple = 2.0;
constexpr double kPairCapContinuityFloorPct = 0.05;

Mojo scale_size_with_floor(Mojo size, double keep_fraction) noexcept
{
    if (size <= 0 || !(keep_fraction > 0.0)) {
        return 0;
    }

    const auto kept = static_cast<Mojo>(
        std::llround(static_cast<double>(size) * keep_fraction));
    return std::max<Mojo>(1, kept);
}

// [STEP6-CAUSE 2026-09-13] The one place a limit changes a size.  It scales
// exactly as the bare scale_size_with_floor() call it replaced, and records
// the rule only when the size actually went DOWN, so a side that is already
// 0 is never attributed to a rule that fires later.
void apply_rule(SideLimitTrace& side, Mojo& size, double keep_fraction,
                LimitRule rule) noexcept
{
    const Mojo before = size;
    size = scale_size_with_floor(size, keep_fraction);
    if (size < before) {
        side.reduced_by = static_cast<std::uint8_t>(side.reduced_by | limit_rule_bit(rule));
        if (size == 0) {
            side.zeroed_by = rule;
        }
    }
}

// The order rule names print in: the order the rules run on one side.
constexpr std::array<LimitRule, 4> kRuleOrder{{
    LimitRule::SoftConcentration, LimitRule::HardConcentration,
    LimitRule::SingleCatCap, LimitRule::PairCapitalCap}};

// Base units to 6 dp, or raw mojos when there is no unit size.  The classic
// locale keeps any process locale's digit grouping out of logs and tests.
std::string format_units(Mojo size, std::int64_t mojos_per_unit)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    if (mojos_per_unit > 0) {
        out << std::fixed << std::setprecision(6)
            << static_cast<double>(size) / static_cast<double>(mojos_per_unit);
    } else {
        out << size << " mojos";
    }
    return out.str();
}

// Names of the rules set in `mask`, in kRuleOrder, skipping `exclude`,
// joined with '+'.
std::string format_rules(std::uint8_t mask, LimitRule exclude)
{
    std::string joined;
    for (const LimitRule rule : kRuleOrder) {
        if (rule == exclude || !has_limit_rule(mask, rule)) {
            continue;
        }
        if (!joined.empty()) {
            joined += '+';
        }
        joined += to_string(rule);
    }
    return joined;
}

}  // namespace

// ---------------------------------------------------------------------------
// EmergencyRule stringification
// ---------------------------------------------------------------------------

const char* to_string(EmergencyRule r) noexcept {
    switch (r) {
        case EmergencyRule::None:            return "None";
        case EmergencyRule::FlashCrash:      return "FlashCrash";
        case EmergencyRule::OneSidedFills:   return "OneSidedFills";
        case EmergencyRule::Congestion:      return "Congestion";
        case EmergencyRule::ExploitDetected: return "ExploitDetected";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// [STEP6-CAUSE 2026-09-13] Limit attribution: rule labels, per-side causes,
// the operator-facing Step 6 text, and the no-quote warn gate.  See
// limits.hpp for the contract of each.
// ---------------------------------------------------------------------------

const char* to_string(LimitRule r) noexcept {
    switch (r) {
        case LimitRule::None:              return "none";
        case LimitRule::SoftConcentration: return "soft_concentration";
        case LimitRule::HardConcentration: return "hard_concentration";
        case LimitRule::SingleCatCap:      return "single_cat_cap";
        case LimitRule::PairCapitalCap:    return "pair_capital_cap";
    }
    return "unknown";
}

SideZeroCause side_zero_cause(const SideLimitTrace& side) noexcept
{
    if (side.post_size > 0) {
        return SideZeroCause::NotZero;
    }
    if (side.pre_size <= 0) {
        return SideZeroCause::ZeroBeforeLimits;
    }
    return SideZeroCause::LimitZeroed;
}

std::string describe_side_limits(const SideLimitTrace& side,
                                 std::int64_t base_mojos_per_unit)
{
    std::string reason;
    switch (side_zero_cause(side)) {
        case SideZeroCause::ZeroBeforeLimits:
            // Sizes are converted to mojos before the limits run, so a tiny
            // positive strategy size that rounds to 0 mojos also lands here;
            // the wording says exactly that much and no more.
            reason = "zero before limits: the strategy's size converted to 0 mojos; "
                     "no risk limit acted on it";
            break;
        case SideZeroCause::LimitZeroed: {
            reason = "zeroed by ";
            reason += (side.zeroed_by == LimitRule::None) ? "an unrecorded rule"
                                                          : to_string(side.zeroed_by);
            const std::string others = format_rules(side.reduced_by, side.zeroed_by);
            if (!others.empty()) {
                reason += "; also reduced by ";
                reason += others;
            }
            break;
        }
        case SideZeroCause::NotZero: {
            const std::string all_rules = format_rules(side.reduced_by, LimitRule::None);
            if (all_rules.empty()) {
                reason = "no limit applied";
            } else {
                reason = "reduced by " + all_rules;
            }
            break;
        }
    }
    return format_units(side.pre_size, base_mojos_per_unit) + " -> "
         + format_units(side.post_size, base_mojos_per_unit) + " (" + reason + ")";
}

std::string describe_limits_block(const LimitsTrace& limits_trace,
                                  std::int64_t base_mojos_per_unit,
                                  double strategy_q,
                                  double strategy_q_max)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "bid " << describe_side_limits(limits_trace.bid, base_mojos_per_unit)
        << " | ask " << describe_side_limits(limits_trace.ask, base_mojos_per_unit)
        << " | " << std::fixed << std::setprecision(3)
        << "base_conc=" << limits_trace.base_concentration
        << " quote_conc=" << limits_trace.quote_concentration;
    if (limits_trace.base_is_cat) {
        out << " base_cat_pct=" << limits_trace.base_cat_fraction
            << " (full block at " << limits_trace.cat_full_block_pct << ")";
    }
    if (limits_trace.quote_is_cat) {
        out << " quote_cat_pct=" << limits_trace.quote_cat_fraction
            << " (full block at " << limits_trace.cat_full_block_pct << ")";
    }
    out << " pair_pct=" << limits_trace.pair_capital_fraction
        << " | strategy " << std::setprecision(6)
        << "q=" << strategy_q << " q_max=" << strategy_q_max;
    return out.str();
}

std::string format_step6_no_quote(std::string_view pair_name,
                                  const LimitsTrace& limits_trace,
                                  std::int64_t base_mojos_per_unit,
                                  double strategy_q,
                                  double strategy_q_max,
                                  const RiskConfig& risk_cfg,
                                  BlockHeight reminder_blocks)
{
    // [PACE D1 2026-09-13] The global soft/hard pair, forwarded.
    return format_step6_no_quote(pair_name, limits_trace, base_mojos_per_unit,
                                 strategy_q, strategy_q_max, risk_cfg,
                                 ConcentrationLimits{risk_cfg.soft_limit_pct,
                                                     risk_cfg.hard_limit_pct},
                                 reminder_blocks);
}

std::string format_step6_no_quote(std::string_view pair_name,
                                  const LimitsTrace& limits_trace,
                                  std::int64_t base_mojos_per_unit,
                                  double strategy_q,
                                  double strategy_q_max,
                                  const RiskConfig& risk_cfg,
                                  const ConcentrationLimits& conc_limits,
                                  BlockHeight reminder_blocks)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "Step 6: " << pair_name << " -- no quote this block: "
        << describe_limits_block(limits_trace, base_mojos_per_unit,
                                 strategy_q, strategy_q_max)
        << " | " << std::fixed << std::setprecision(3)
        << "cfg_soft=" << conc_limits.soft_limit_pct
        << " cfg_hard=" << conc_limits.hard_limit_pct
        << " cfg_cat=" << risk_cfg.single_cat_cap_pct
        << " cfg_pair=" << risk_cfg.max_capital_per_pair_pct
        << " (same-cause repeats log at debug for " << reminder_blocks
        << " blocks)";
    return out.str();
}

std::string format_step6_quote_resumed(std::string_view pair_name,
                                       BlockHeight last_warn_height,
                                       BlockHeight reminder_blocks)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "Step 6: " << pair_name
        << " -- limits let a quote through again after the no-quote warn at height "
        << last_warn_height << "; a same-cause block before height "
        << (last_warn_height + reminder_blocks) << " logs at debug only";
    return out.str();
}

LimitBlockSignature limit_block_signature(const LimitsTrace& limits_trace) noexcept
{
    LimitBlockSignature sig{};
    sig.bid_cause     = side_zero_cause(limits_trace.bid);
    sig.bid_zeroed_by = limits_trace.bid.zeroed_by;
    sig.ask_cause     = side_zero_cause(limits_trace.ask);
    sig.ask_zeroed_by = limits_trace.ask.zeroed_by;
    return sig;
}

bool LimitBlockWarnGate::should_warn(const LimitBlockSignature& sig,
                                     BlockHeight height,
                                     BlockHeight reminder_blocks) noexcept
{
    if (!fired_ || sig != last_sig_ || height - last_warn_height_ >= reminder_blocks) {
        fired_            = true;
        recovery_pending_ = true;
        last_sig_         = sig;
        last_warn_height_ = height;
        return true;
    }
    return false;
}

bool LimitBlockWarnGate::should_log_recovery() noexcept
{
    if (!recovery_pending_) {
        return false;
    }
    recovery_pending_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PreTradeCheck::PreTradeCheck(const RiskConfig&     cfg,
                             const StrategyConfig& strat_cfg)
    : risk_cfg_(cfg)
    , margin_fraction_(strat_cfg.min_profit_margin_bps / 10'000.0)
{
    // Validate: margin_fraction_ should be a small positive number (e.g. 0.0035
    // for 35 bps).  A negative or enormous value indicates a config error.
    // ISO/IEC 5055: throw instead of assert (stripped in Release builds).
    if (!(margin_fraction_ >= 0.0 && margin_fraction_ < 1.0)) {
        throw std::invalid_argument(
            "RiskConfig: min_profit_margin_bps yields margin_fraction outside [0, 1)");
    }

    // ISO/IEC 5055: validate that soft_limit_pct does not exceed
    // hard_limit_pct.  A reversed ordering would cause the graduated
    // reduction logic in apply_limits() to divide by zero or produce
    // nonsensical skew factors.
    if (risk_cfg_.soft_limit_pct > risk_cfg_.hard_limit_pct) {
        throw std::invalid_argument(
            "RiskConfig: soft_limit_pct must be <= hard_limit_pct");
    }
}

// ---------------------------------------------------------------------------
// enforce_no_loss -- CORE RULE: NEVER SELL AT A LOSS.
//
// If enabled, the ask price is floored at (cost_basis + min_profit_margin).
// The bid side is never modified -- buying more of an asset does not violate
// the no-loss rule.
//
// The margin is computed as a fraction of cost_basis rather than as an
// absolute number of mojos, so it scales correctly regardless of the
// asset's per-mojo price.
// ---------------------------------------------------------------------------

Quote PreTradeCheck::enforce_no_loss(Quote quote,
                                     Mojo  cost_basis,
                                     bool  enable_constraint) const noexcept
{
    if (!enable_constraint) {
        return quote;  // pass through unchanged
    }

    // Compute the minimum acceptable ask price.
    // min_ask = cost_basis + cost_basis * margin_fraction_
    //         = cost_basis * (1 + margin_fraction_)
    // Using integer arithmetic to avoid floating-point monetary drift:
    //   margin_mojos = round(cost_basis * margin_fraction_)
    //   min_ask      = cost_basis + margin_mojos
    //
    // Guard against cost_basis <= 0 (no inventory, or free acquisition).
    // When cost_basis is zero the constraint is vacuously satisfied.
    if (cost_basis <= 0) {
        return quote;
    }

    const auto margin_mojos = static_cast<Mojo>(
        std::llround(static_cast<double>(cost_basis) * margin_fraction_));

    // Overflow guard: if cost_basis is near Mojo max, adding margin could wrap.
    // In practice this cannot happen (max position ~11K XCH = 1.1e16 mojos,
    // well within int64 range), but defensive coding is warranted.
    const Mojo headroom = std::numeric_limits<Mojo>::max() - cost_basis;
    const Mojo safe_margin = (margin_mojos > 0 && margin_mojos <= headroom)
                           ? margin_mojos
                           : headroom;

    const Mojo min_ask = cost_basis + safe_margin;

    // Floor the ask price.
    if (quote.ask_price < min_ask) {
        quote.ask_price = min_ask;

        // Recompute informational spread_bps to reflect the adjusted ask.
        // spread_bps = (ask - bid) / ((ask + bid) / 2) * 10'000
        if (quote.bid_price > 0) {
            const double mid = (static_cast<double>(quote.ask_price)
                              + static_cast<double>(quote.bid_price)) / 2.0;
            quote.spread_bps = (static_cast<double>(quote.ask_price)
                              - static_cast<double>(quote.bid_price))
                             / mid * 10'000.0;
        }
    }

    return quote;
}

// ---------------------------------------------------------------------------
// evaluate_limits -- inventory, CAT cap, and capital-per-pair checks, with a
// per-side record of what each rule did.
//
// Design:  Each check can independently reduce or zero out a side of the
//          quote.  If both sides end up zeroed, decision.has_quote is false
//          (apply_limits() returns nullopt), signalling that no quote should
//          be posted this cycle.
//
// [STEP6-CAUSE 2026-09-13] Every size cut goes through apply_rule(), which
// scales exactly as the bare scale_size_with_floor() call it replaced and
// records the rule on that side, so the trace cannot disagree with the
// quote.  The order of the checks, every formula and constant, and the
// no-quote condition are unchanged.
// ---------------------------------------------------------------------------

LimitsDecision PreTradeCheck::evaluate_limits(
    Quote          quote,
    const AssetId& base_id,
    const AssetId& quote_id,
    const State&   state) const
{
    // [PACE D1 2026-09-13] The global soft/hard pair, forwarded: the same two
    // doubles the concentration rule read from risk_cfg_ before per-pair
    // limits existed.
    return evaluate_limits(quote, base_id, quote_id, state,
                           ConcentrationLimits{risk_cfg_.soft_limit_pct,
                                               risk_cfg_.hard_limit_pct});
}

LimitsDecision PreTradeCheck::evaluate_limits(
    Quote                      quote,
    const AssetId&             base_id,
    const AssetId&             quote_id,
    const State&               state,
    const ConcentrationLimits& limits,
    const std::unordered_set<AssetId>& unverified) const
{
    LimitsDecision decision{};
    LimitsTrace& limits_trace = decision.trace;
    limits_trace.bid.pre_size = quote.bid_size;
    limits_trace.ask.pre_size = quote.ask_size;

    const Position base_pos  = state.get_position(base_id);
    const Position quote_pos = state.get_position(quote_id);
    // [SEED-FAIL-CLOSED review round 11] Less every unverified position.
    const auto all_positions = positions_in_totals(state, unverified);

    // ---- 1. Inventory concentration (soft / hard limits) ------------------
    //
    //   concentration = base_balance / (base_balance + quote_balance)
    //   If concentration >= hard_limit (80%): pull quotes on the overweight
    //     side (i.e., stop buying base if we already hold too much base).
    //   If concentration >= soft_limit (60%): same effect -- the strategy
    //     should already be skewing, but this is the backstop.
    //
    //   The "overweight side" when base concentration is high means we hold
    //   too much base, so we must stop BUYING base (zero the bid).
    //   Conversely, low base concentration (high quote concentration)
    //   means we stop SELLING base (zero the ask).

    // Mark-to-market: convert raw mojo balances to a common XCH numeraire
    // so that concentration reflects economic value, not raw token counts.
    const double base_conc = compute_concentration(base_pos, quote_pos, state);
    const double quote_conc = 1.0 - base_conc;
    limits_trace.base_concentration  = base_conc;
    limits_trace.quote_concentration = quote_conc;

    // [PACE D1 2026-09-13] Both sides go through concentration_keep_fraction()
    // with THIS pair's limits.  The helper holds the two expressions this
    // block used to spell out inline, in the same order and with the same
    // constant, so the keep fractions are bit-identical when `limits` is the
    // global pair, and the rule label is chosen by the same `>= hard`
    // comparison the old if/else-if made:
    //   - hard band (conc >= hard): a tiny continuity quote, so the bot does
    //     not collapse into a permanently one-sided book; it tapers to zero
    //     only as concentration approaches 100%;
    //   - soft band (soft <= conc < hard): a linear reduction from full size
    //     at soft to that continuity quote at hard;
    //   - below soft, or NaN: no call at all, exactly as before.
    // Base overweight: stop BUYING base, so the bid tapers.
    if (const auto keep = concentration_keep_fraction(base_conc, limits)) {
        apply_rule(limits_trace.bid, quote.bid_size, *keep,
                   base_conc >= limits.hard_limit_pct ? LimitRule::HardConcentration
                                                      : LimitRule::SoftConcentration);
    }

    // Quote overweight: stop SELLING base, so the ask tapers.
    if (const auto keep = concentration_keep_fraction(quote_conc, limits)) {
        apply_rule(limits_trace.ask, quote.ask_size, *keep,
                   quote_conc >= limits.hard_limit_pct ? LimitRule::HardConcentration
                                                       : LimitRule::SoftConcentration);
    }

    // ---- 2. Single-CAT cap (12% of total portfolio) ----------------------
    //
    //   "Never exceed 12% of portfolio in any one CAT regardless of
    //    opportunity."  -- Section 8.
    //
    //   If the base asset is a CAT (not "xch") and its portfolio fraction
    //   exceeds the cap, stop buying it (zero bid).  We do NOT force-sell
    //   (never sell at a loss).

    if (base_id != "xch") {
        const double cat_frac = compute_portfolio_fraction(base_pos, all_positions, state);
        limits_trace.base_is_cat       = true;
        limits_trace.base_cat_fraction = cat_frac;
        if (cat_frac >= risk_cfg_.single_cat_cap_pct) {
            const double taper = std::clamp(
                1.0 - (cat_frac - risk_cfg_.single_cat_cap_pct)
                    / std::max(
                        risk_cfg_.single_cat_cap_pct
                            * (kCatCapFullBlockMultiple - 1.0),
                        1e-9),
                0.0, 1.0);
            apply_rule(limits_trace.bid, quote.bid_size,
                       kCatCapContinuityFloorPct * taper,
                       LimitRule::SingleCatCap);
        }
    }

    // Same check for quote side if the quote asset is a CAT.
    if (quote_id != "xch") {
        const double cat_frac = compute_portfolio_fraction(quote_pos, all_positions, state);
        limits_trace.quote_is_cat       = true;
        limits_trace.quote_cat_fraction = cat_frac;
        if (cat_frac >= risk_cfg_.single_cat_cap_pct) {
            const double taper = std::clamp(
                1.0 - (cat_frac - risk_cfg_.single_cat_cap_pct)
                    / std::max(
                        risk_cfg_.single_cat_cap_pct
                            * (kCatCapFullBlockMultiple - 1.0),
                        1e-9),
                0.0, 1.0);
            apply_rule(limits_trace.ask, quote.ask_size,
                       kCatCapContinuityFloorPct * taper,
                       LimitRule::SingleCatCap);
        }
    }

    limits_trace.cat_full_block_pct =
        risk_cfg_.single_cat_cap_pct * kCatCapFullBlockMultiple;

    // ---- 3. Max capital per pair ------------------------------------------
    //
    //   If this pair consumes more than max_capital_per_pair_pct of total
    //   capital, taper both sides towards a continuity quote. This avoids a
    //   hard deadlock when only one pair is enabled while still preserving a
    //   strong cap as concentration approaches 100%.

    const double pair_frac = compute_pair_capital_fraction(base_pos, quote_pos,
                                                           all_positions, state);
    limits_trace.pair_capital_fraction = pair_frac;
    if (pair_frac >= risk_cfg_.max_capital_per_pair_pct) {
        const double reduction = std::clamp(
            (pair_frac - risk_cfg_.max_capital_per_pair_pct)
                / std::max(1.0 - risk_cfg_.max_capital_per_pair_pct, 1e-9),
            0.0, 1.0);
        const double keep_fraction =
            1.0 - reduction * (1.0 - kPairCapContinuityFloorPct);
        apply_rule(limits_trace.bid, quote.bid_size, keep_fraction,
                   LimitRule::PairCapitalCap);
        apply_rule(limits_trace.ask, quote.ask_size, keep_fraction,
                   LimitRule::PairCapitalCap);
    }

    // ---- Result -----------------------------------------------------------

    limits_trace.bid.post_size = quote.bid_size;
    limits_trace.ask.post_size = quote.ask_size;
    if (quote.bid_size == 0 && quote.ask_size == 0) {
        return decision;  // both sizes zero: no quote this cycle; the trace says why
    }

    decision.has_quote = true;
    decision.quote     = quote;
    return decision;
}

// ---------------------------------------------------------------------------
// apply_limits -- the original interface.  It delegates, so the engine's
// evaluate_limits() call and every apply_limits() caller run one copy of the
// arithmetic.
// ---------------------------------------------------------------------------

std::optional<Quote> PreTradeCheck::apply_limits(
    Quote              quote,
    const std::string& pair_name,
    const AssetId&     base_id,
    const AssetId&     quote_id,
    const State&       state) const
{
    // [PACE D1 2026-09-13] The global soft/hard pair, forwarded.
    return apply_limits(quote, pair_name, base_id, quote_id, state,
                        ConcentrationLimits{risk_cfg_.soft_limit_pct,
                                            risk_cfg_.hard_limit_pct});
}

std::optional<Quote> PreTradeCheck::apply_limits(
    Quote                      quote,
    const std::string&         /*pair_name*/,
    const AssetId&             base_id,
    const AssetId&             quote_id,
    const State&               state,
    const ConcentrationLimits& limits) const
{
    const LimitsDecision decision = evaluate_limits(quote, base_id, quote_id, state, limits);
    if (!decision.has_quote) {
        return std::nullopt;
    }
    return decision.quote;
}

// ---------------------------------------------------------------------------
// [PACE D1 2026-09-13] concentration_keep_fraction -- the concentration
// rule's keep fraction for one side, with one pair's limits.  These are the
// two expressions evaluate_limits() spelled out inline before per-pair limits
// existed, in the same order and with the same constant.  At conc == soft the
// soft branch returns exactly 1.0 (not nullopt), so the scale_size_with_floor
// call -- and its size <= 0 -> 0 mapping -- still happens; a NaN fails both
// comparisons and returns nullopt, which is the old "no call".
// ---------------------------------------------------------------------------

std::optional<double> PreTradeCheck::concentration_keep_fraction(
    double concentration, const ConcentrationLimits& limits) noexcept
{
    if (concentration >= limits.hard_limit_pct) {
        // Hard limit breach: leave only a tiny continuity quote so the bot
        // does not collapse into a permanently one-sided book.  The size
        // tapers to zero only as concentration approaches 100%.
        const double taper = std::clamp(
            1.0 - (concentration - limits.hard_limit_pct)
                / std::max(1.0 - limits.hard_limit_pct, 1e-9),
            0.0, 1.0);
        return kHardLimitContinuityFloorPct * taper;
    }
    if (concentration >= limits.soft_limit_pct) {
        // Soft limit: graduated proportional reduction instead of zeroing.
        // ISO/IEC 5055: clamped to [0.0, 1.0] to guard against config
        // where soft_pct == hard_pct (division by zero yields 0.0 via clamp).
        const double reduction = std::clamp(
            (concentration - limits.soft_limit_pct)
                / (limits.hard_limit_pct - limits.soft_limit_pct),
            0.0, 1.0);
        return 1.0 - reduction * (1.0 - kHardLimitContinuityFloorPct);
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// [PACE D1 2026-09-13] effective_concentration_limits -- see limits.hpp.
// ---------------------------------------------------------------------------

ConcentrationLimits effective_concentration_limits(const RiskConfig& risk,
                                                   const PairConfig* pair) noexcept
{
    const ConcentrationLimits global{risk.soft_limit_pct, risk.hard_limit_pct};
    if (pair == nullptr) {
        return global;
    }
    const ConcentrationLimits eff{pair->soft_limit_pct_override.value_or(risk.soft_limit_pct),
                                  pair->hard_limit_pct_override.value_or(risk.hard_limit_pct)};
    const bool valid = std::isfinite(eff.soft_limit_pct) && std::isfinite(eff.hard_limit_pct)
                    && eff.soft_limit_pct > 0.0 && eff.soft_limit_pct < eff.hard_limit_pct
                    && eff.hard_limit_pct <= 1.0;
    return valid ? eff : global;
}

// ---------------------------------------------------------------------------
// check_flash_crash -- circuit breaker for sudden price drops.
//
// Algorithm (rolling max-drawdown):
//   Scan the price window chronologically, maintaining a running maximum.
//   At each point, compute the drawdown from the running max.  If any
//   drawdown exceeds the threshold, a crash is detected.
//
//   This correctly detects:
//     - Early crashes followed by recovery to new highs (the previous
//       global-max algorithm missed these because the later peak dominated).
//     - Multiple successive crashes within one window.
//     - V-shaped recoveries (crash still flagged).
//     - Monotonically rising prices (no crash).
//     - Flat markets (no crash).
//
//   The running-max approach is O(N) in time and O(1) in auxiliary space,
//   matching the previous implementation's complexity while providing
//   strictly superior detection coverage.
//
//   Edge cases:
//     - Empty or single-element history: no crash.
//     - All prices zero or negative: degenerate, treated as no crash.
//     - Running max is the last element: drawdown is 0.0, no false positive.
//
// ISO/IEC 5055: division guarded against zero denominator; no UB.
// ---------------------------------------------------------------------------

bool PreTradeCheck::check_flash_crash(const std::vector<Mojo>& price_history,
                                      double threshold,
                                      std::size_t window) noexcept
{
    if (price_history.size() < 2) {
        return false;  // not enough data to detect a crash
    }

    // [S12] Restrict the scan to the most recent `window` entries when one
    // is given.  The running maximum below never decays, so over the whole
    // retained history one junk print (observed 2026-08-22: a crossed dexie
    // ticker put a 2.3419 mid in a ~1.57 market) reads as a live crash until
    // the buffer evicts it ~1000 blocks later -- and the Crash state
    // consults this signal BEFORE the stability checks, so recovery never
    // ran.  A windowed scan lets an aged, retraced spike stop counting while
    // a genuine recent collapse still trips.
    const std::size_t start =
        (window > 0 && price_history.size() > window)
            ? price_history.size() - window
            : 0;

    // Track the running maximum as we scan chronologically.
    Mojo running_max = price_history[start];

    for (std::size_t i = start + 1; i < price_history.size(); ++i) {
        // Update running max with the current price.
        if (price_history[i] > running_max) {
            running_max = price_history[i];
        }

        // Guard: skip drawdown computation when running_max is non-positive
        // to avoid division by zero on degenerate data.
        if (running_max <= 0) {
            continue;
        }

        // Compute fractional drawdown from the running maximum.
        // Use double for the division only (operands are exact integers).
        const double drawdown =
            static_cast<double>(running_max - price_history[i])
            / static_cast<double>(running_max);

        if (drawdown >= threshold) {
            return true;  // Flash crash detected.
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// is_stable_after_crash -- recovery gate.
//
// Returns true only when the most recent `required_stable_blocks` prices
// all lie within `stability_band` of the latest price.  This ensures we
// do not resume quoting during a dead-cat bounce.
// ---------------------------------------------------------------------------

bool PreTradeCheck::is_stable_after_crash(
    const std::vector<Mojo>& price_history,
    std::size_t              required_stable_blocks,
    double                   stability_band) noexcept
{
    if (price_history.size() < required_stable_blocks) {
        return false;  // not enough blocks to declare stability
    }

    const Mojo latest = price_history.back();
    if (latest <= 0) {
        return false;  // degenerate
    }

    // Check the tail of the history.
    const auto start = price_history.cend()
                     - static_cast<std::ptrdiff_t>(required_stable_blocks);

    for (auto it = start; it != price_history.cend(); ++it) {
        const double deviation = std::abs(static_cast<double>(*it - latest))
                               / static_cast<double>(latest);
        if (deviation > stability_band) {
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// congestion_buffer_multiplier
//
// Section 8 Emergency Playbook: "Network congestion: Increase rebalancing
// buffer to 25-30%."  The normal buffer is ~15-20%, so a multiplier of
// 1.30 brings it to ~19.5-26%, within the specified range.
// ---------------------------------------------------------------------------

double PreTradeCheck::congestion_buffer_multiplier(bool congested) const noexcept
{
    return congested ? kCongestionMultiplier : 1.0;
}

// ---------------------------------------------------------------------------
// get_limit_status -- diagnostic snapshot for monitoring dashboards.
// ---------------------------------------------------------------------------

LimitStatus PreTradeCheck::get_limit_status(
    const AssetId& base_id,
    const AssetId& quote_id,
    const State&   state) const
{
    // [PACE D1 2026-09-13] The global soft/hard pair, forwarded.
    return get_limit_status(base_id, quote_id, state,
                            ConcentrationLimits{risk_cfg_.soft_limit_pct,
                                                risk_cfg_.hard_limit_pct});
}

LimitStatus PreTradeCheck::get_limit_status(
    const AssetId&             base_id,
    const AssetId&             quote_id,
    const State&               state,
    const ConcentrationLimits& limits,
    const std::unordered_set<AssetId>& unverified) const
{
    const Position base_pos  = state.get_position(base_id);
    const Position quote_pos = state.get_position(quote_id);
    // [SEED-FAIL-CLOSED review round 11] Less every unverified position.
    const auto all_positions = positions_in_totals(state, unverified);

    LimitStatus ls{};
    ls.base_id  = base_id;
    ls.quote_id = quote_id;

    // Inventory concentration (mark-to-market in XCH numeraire), against this
    // pair's effective limits.
    ls.base_concentration  = compute_concentration(base_pos, quote_pos, state);
    ls.soft_limit_breached = (ls.base_concentration >= limits.soft_limit_pct)
                          || ((1.0 - ls.base_concentration) >= limits.soft_limit_pct);
    ls.hard_limit_breached = (ls.base_concentration >= limits.hard_limit_pct)
                          || ((1.0 - ls.base_concentration) >= limits.hard_limit_pct);

    // Single-CAT cap (only relevant for CAT assets, mark-to-market).
    if (base_id != "xch") {
        ls.cat_portfolio_pct = compute_portfolio_fraction(base_pos, all_positions, state);
    } else if (quote_id != "xch") {
        ls.cat_portfolio_pct = compute_portfolio_fraction(quote_pos, all_positions, state);
    } else {
        ls.cat_portfolio_pct = 0.0;  // XCH/XCH pair (hypothetical)
    }
    ls.cat_cap_breached = (ls.cat_portfolio_pct >= risk_cfg_.single_cat_cap_pct);

    // Capital per pair (mark-to-market in XCH numeraire).
    ls.pair_capital_pct  = compute_pair_capital_fraction(base_pos, quote_pos,
                                                         all_positions, state);
    ls.pair_cap_breached = (ls.pair_capital_pct >= risk_cfg_.max_capital_per_pair_pct);

    // Flash-crash flag is set externally by the caller; initialise to false.
    ls.flash_crash_active = false;

    return ls;
}

// ===========================================================================
// Private helpers
// ===========================================================================

// ---------------------------------------------------------------------------
// mark_to_xch -- convert a position's raw mojo balance to XCH-equivalent mojos.
//
// The common numeraire for all risk calculations is XCH.  For XCH positions
// the balance is already in the correct unit; for CAT positions we look up
// the market mid price (mojos of XCH per mojo of CAT) from the State's
// market snapshot cache.
//
// Pair name convention: market snapshots are keyed as "BASE/QUOTE" where one
// side is typically XCH.  We probe both orderings:
//   (1) "<asset_id>/xch" -- asset is base, XCH is quote.
//       mid_price is in mojos-of-xch per mojo-of-base.
//       xch_value = balance * mid_price.
//   (2) "xch/<asset_id>" -- XCH is base, asset is quote.
//       mid_price is in mojos-of-asset per mojo-of-xch.
//       xch_value = balance / mid_price.
//
// Conservative fallback (ISO/IEC 27001:2022 -- fail-safe):
//   If neither market snapshot exists or mid_price == 0 (data not yet
//   available), the raw mojo balance is returned unchanged.  This treats
//   1 mojo of the CAT as if it were worth 1 mojo of XCH, which massively
//   over-estimates the value of cheap tokens and causes risk limits to fire
//   earlier -- the safe direction.
//
// ISO/IEC 5055: double used only for the dimensionless ratio computation;
//   the returned Mojo is rounded back to integer via llround.
// ---------------------------------------------------------------------------

Mojo PreTradeCheck::mark_to_xch(const Position& pos,
                                 const State&    state) noexcept
{
    // XCH positions need no conversion -- balance is already in XCH mojos.
    if (pos.asset_id == "xch" || pos.asset_id.empty()) {
        return pos.balance;
    }

    // Guard: non-positive balance yields zero value regardless of price.
    if (pos.balance <= 0) {
        return 0;
    }

    // Use pre-computed XCH rate (xch_mojos per asset_mojo) set by the
    // engine from market data and pair config (mojos_per_unit).  This
    // correctly converts CAT mojo balances to XCH-equivalent mojos
    // without needing to know the price scaling convention here.
    const double rate = state.get_asset_xch_rate(pos.asset_id);
    if (rate > 0.0) {
        const auto xch_val = static_cast<double>(pos.balance) * rate;
        // Clamp to Mojo range to prevent undefined behaviour on cast.
        if (xch_val >= static_cast<double>(std::numeric_limits<Mojo>::max())) {
            return std::numeric_limits<Mojo>::max();
        }
        return static_cast<Mojo>(std::llround(xch_val));
    }

    // Fallback: no price available.  Return raw balance (conservative --
    // over-estimates value, triggers risk limits sooner).
    return pos.balance;
}

// ---------------------------------------------------------------------------
// compute_concentration -- fraction of combined mark-to-market value held
// in the base asset, expressed in XCH-equivalent mojos.
//
// Uses mark_to_xch() to convert each position's raw mojo balance into a
// common numeraire (XCH) before computing the ratio.  This ensures that
// e.g. 10,000 mojos of USDS ($0.01) and 10,000 mojos of XCH ($30,000)
// are weighted by their actual economic value, not raw token counts.
//
// Returns 0.0 if both marked values are zero (no capital deployed).
// ISO/IEC 5055: division guarded against zero denominator.
// ---------------------------------------------------------------------------

double PreTradeCheck::compute_concentration(
    const Position& base_pos,
    const Position& quote_pos,
    const State&    state) noexcept
{
    const auto base_val  = static_cast<double>(mark_to_xch(base_pos, state));
    const auto quote_val = static_cast<double>(mark_to_xch(quote_pos, state));
    const double total = base_val + quote_val;

    if (total <= 0.0) {
        return 0.5;  // no capital deployed -- assume balanced to avoid
                     // erroneously tripping inventory limits on either side
    }
    return base_val / total;
}

// ---------------------------------------------------------------------------
// compute_portfolio_fraction -- what share of total portfolio value (mark-to-
// market in XCH) a single asset represents.
//
// Sums mark_to_xch() across all positions to obtain the denominator.
// Returns 0.0 if total portfolio value is zero.
// ---------------------------------------------------------------------------

double PreTradeCheck::compute_portfolio_fraction(
    const Position&              asset_pos,
    const std::vector<Position>& all,
    const State&                 state) noexcept
{
    // Sum total XCH-equivalent value across all assets.
    double total = 0.0;
    for (const auto& p : all) {
        total += static_cast<double>(mark_to_xch(p, state));
    }

    if (total <= 0.0) {
        return 0.0;
    }

    return static_cast<double>(mark_to_xch(asset_pos, state)) / total;
}

// ---------------------------------------------------------------------------
// compute_pair_capital_fraction -- combined mark-to-market (XCH) value of
// base + quote positions as a fraction of total portfolio value.
// ---------------------------------------------------------------------------

double PreTradeCheck::compute_pair_capital_fraction(
    const Position&              base_pos,
    const Position&              quote_pos,
    const std::vector<Position>& all,
    const State&                 state) noexcept
{
    double total = 0.0;
    for (const auto& p : all) {
        total += static_cast<double>(mark_to_xch(p, state));
    }

    if (total <= 0.0) {
        return 0.0;
    }

    const double pair_capital = static_cast<double>(mark_to_xch(base_pos, state))
                              + static_cast<double>(mark_to_xch(quote_pos, state));
    return pair_capital / total;
}

// ---------------------------------------------------------------------------
// [SEED-FAIL-CLOSED review round 11] positions_in_totals -- the positions a
// portfolio total sums: every one State holds, less the unverified.  An
// unverified position is a startup fallback no wallet read has confirmed.
// Counted, an overstated one would understate every other asset's share of
// the total, and loosen the single-CAT and pair-capital caps of pairs whose
// own positions are verified.  Before the seed kept such a fallback in State,
// a failed read left the asset out of it, and so out of the total.
// ---------------------------------------------------------------------------

std::vector<Position> PreTradeCheck::positions_in_totals(
    const State&                       state,
    const std::unordered_set<AssetId>& unverified)
{
    std::vector<Position> all = state.get_all_positions();
    all.erase(std::remove_if(all.begin(), all.end(),
                             [&unverified](const Position& p) {
                                 return unverified.count(p.asset_id) > 0U;
                             }),
              all.end());
    return all;
}

}  // namespace xop
