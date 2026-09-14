// cancel_escalation_config.hpp -- [S14 2026-09-13] The strategy.cancel_
// escalation_* config keys as the CancelEscalationParams the pure decisions
// in cancel_escalation.hpp read.
//
// Kept out of cancel_escalation.hpp so that header stays free of engine and
// config types, and kept out of config.hpp so config.hpp does not pull in
// nlohmann::json.  The mapping is a separate function so a test can load a
// real YAML file and check that every key lands in the right field.

#ifndef XOP_EXECUTION_CANCEL_ESCALATION_CONFIG_HPP
#define XOP_EXECUTION_CANCEL_ESCALATION_CONFIG_HPP

#include "xop/config.hpp"
#include "xop/execution/cancel_escalation.hpp"

namespace xop::execution {

[[nodiscard]] inline CancelEscalationParams cancel_escalation_params_from(
    const StrategyConfig& strategy) noexcept
{
    CancelEscalationParams params;
    params.window_blocks        = strategy.cancel_escalation_window_blocks;
    params.max_escalations      = strategy.cancel_escalation_max_attempts;
    params.fee_step_mojos       = strategy.cancel_escalation_fee_step_mojos;
    params.max_fee_mojos        = strategy.cancel_escalation_max_fee_mojos;
    params.retry_blocks         = strategy.cancel_escalation_retry_blocks;
    params.max_probes_per_sweep = strategy.cancel_escalation_max_probes;
    return params;
}

}  // namespace xop::execution

#endif  // XOP_EXECUTION_CANCEL_ESCALATION_CONFIG_HPP
