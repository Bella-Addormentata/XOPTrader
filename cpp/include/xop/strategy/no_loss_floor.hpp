// ---------------------------------------------------------------------------
// no_loss_floor.hpp -- what the ask floor is allowed to be.
//
// [FLOOR 2026-08-30] The Step 7 no-loss lift raised every ask to
// basis*(1+margin) UNCONDITIONALLY. During the 2026-08-30 XCH repricing
// that pinned XCH/BYC asks ~20% above a book that had already fallen --
// the floor was not protecting value, it was refusing to acknowledge a
// loss the market had already marked while the inventory stayed exposed.
// The operator's read: "artificially trying to avoid unrealized losses
// becoming realized, even though this might be unavoidable."
//
// Three explicit modes, operator-chosen (strategy.no_loss_floor_mode):
//
//  * strict -- the old behaviour: never quote an ask below basis+margin.
//  * aging  -- the floor DECAYS with position age via the existing
//              inventory_aging dials (this is what max_loss_relax_bps
//              was always meant to do; the lift just never read it):
//              losses get realized on a schedule the operator sets, not
//              instantly at the bottom of a dislocation.
//  * off    -- no floor: market pricing governs (anchor/book), with the
//              BBO aggressive cap still preventing sales >N% below best
//              ask. Realizes whatever loss the market has already marked.
// ---------------------------------------------------------------------------

#ifndef XOP_STRATEGY_NO_LOSS_FLOOR_HPP
#define XOP_STRATEGY_NO_LOSS_FLOOR_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xop::strategy {

enum class FloorMode { Strict, Aging, Off };

/// The minimum permissible ask price, or 0 for "no floor". `cost_basis`
/// is in quote units (0/negative = unknown, no floor); ages/rates use the
/// inventory_aging semantics (discount grows linearly past
/// aging_start_blocks, capped at max_loss_relax_bps; effective margin may
/// go negative down to -max_loss_relax_bps).
[[nodiscard]] inline std::int64_t compute_ask_floor(
    FloorMode mode,
    std::int64_t cost_basis,
    double margin_bps,
    int position_age_blocks,
    std::uint32_t aging_start_blocks,
    double relax_rate_bps_per_block,
    double max_loss_relax_bps) noexcept
{
    if (mode == FloorMode::Off || cost_basis <= 0) return 0;

    double effective_margin_bps = margin_bps;
    if (mode == FloorMode::Aging && position_age_blocks > 0
        && static_cast<std::uint32_t>(position_age_blocks)
               > aging_start_blocks) {
        const double age_past = static_cast<double>(
            static_cast<std::uint32_t>(position_age_blocks)
            - aging_start_blocks);
        const double discount_bps = std::min(
            max_loss_relax_bps, age_past * relax_rate_bps_per_block);
        effective_margin_bps =
            std::max(margin_bps - discount_bps, -max_loss_relax_bps);
    }
    return static_cast<std::int64_t>(std::llround(
        static_cast<double>(cost_basis)
        * (1.0 + effective_margin_bps / 10'000.0)));
}

}  // namespace xop::strategy

#endif  // XOP_STRATEGY_NO_LOSS_FLOOR_HPP
