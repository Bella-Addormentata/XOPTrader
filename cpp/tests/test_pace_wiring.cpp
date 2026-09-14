// test_pace_wiring.cpp -- source scans of the pace controller's engine glue.
//
// [PACE round 2, PR #160] xop_tests constructs no Engine (S36), so the hooks
// that round 2 added to engine.cpp cannot be driven by a unit test.  These
// tests read engine.cpp itself and pin the wiring the pure-header tests rely
// on: which function is called, where, and with which inputs.  A scan is
// weaker than a behavioural test -- it proves the call is there, not that the
// engine state feeding it is right -- so each one is mutation-checked, and
// the staged rollout remains the behavioural check.
//
// Every whitespace character is removed from both the source and the needle
// before comparing, so re-wrapping a line does not break a scan; renaming or
// reordering does.

#include <gtest/gtest.h>

#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string read_engine_cpp()
{
    std::ifstream in(XOP_TESTS_ENGINE_CPP, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string squeeze(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            out.push_back(c);
        }
    }
    return out;
}

/// The squeezed text of the function whose definition begins with
/// `signature`, up to the first line holding only "}" (engine.cpp closes
/// every function at column 0).  Empty when the signature is absent.
std::string function_body(const std::string& raw, const std::string& signature)
{
    const std::size_t start = raw.find(signature);
    if (start == std::string::npos) {
        return {};
    }
    std::size_t at = start;
    while (true) {
        at = raw.find("\n}", at);
        if (at == std::string::npos) {
            return squeeze(raw.substr(start));
        }
        const std::size_t after = at + 2;
        if (after >= raw.size() || raw[after] == '\r' || raw[after] == '\n') {
            return squeeze(raw.substr(start, after - start));
        }
        at = after;
    }
}

std::size_t position_of(const std::string& body, const std::string& needle)
{
    return body.find(squeeze(needle));
}

const char* const kRefreshSignature = "asio::awaitable<void> Engine::refresh_pace_balances(BlockHeight block_height)";
const char* const kLadderSignature  = "void Engine::step_generate_ladder(BlockHeight block_height)";
const char* const kCapsSignature    = "asio::awaitable<void> Engine::step_enforce_pace_caps(BlockHeight block_height,";

}  // namespace

TEST(PaceWiring, BalanceRefreshClosedByEveryPostingGate) {
    const std::string body = function_body(read_engine_cpp(), kRefreshSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const char* const gates[] = {
        "pace_gates.pace_enabled = config_.strategy.pace_enabled;",
        "pace_gates.dry_run = dry_run_;",
        "pace_gates.wallet_circuit_open = wallet_circuit_open_;",
        "pace_gates.watchdog_fired = watchdog_fired_.load(std::memory_order_acquire);",
        "pace_gates.wallet_consecutive_failures = wallet_ ? wallet_->transport_counters().consecutive_failures : 0u;",
        "pace_gates.gui_pause = gui_pause_active_;",
        "pace_gates.breaker_pause = breaker_pause_active_;",
        "pace_gates.cancel_all_inflight = cancel_all_inflight_;",
        "pace_gates.cancel_all_draining = cancel_all_draining_;",
        "pace_gates.flash_crash_normal = (flash_crash_state_ == FlashCrashState::Normal);",
        "pace_gates.xch_recovery = xch_recovery_mode_;",
    };
    for (const char* const gate : gates) {
        EXPECT_NE(position_of(body, gate), std::string::npos) << gate;
    }
    const std::size_t check = position_of(body, "if (!strategy::pace::pace_refresh_gates_open(pace_gates)");
    const std::size_t rpc = position_of(body, "co_await wallet_->get_wallet_balance(pace_wid)");
    ASSERT_NE(check, std::string::npos);
    ASSERT_NE(rpc, std::string::npos);
    EXPECT_LT(check, rpc);
}

TEST(PaceWiring, BalanceRefreshBacksOffBeforeEachRpc) {
    const std::string body = function_body(read_engine_cpp(), kRefreshSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body,
                          "strategy::pace::pace_refresh_age_blocks(config_.strategy.pace_max_balance_age_blocks)"),
              std::string::npos);
    const std::size_t due = position_of(body, "if (!strategy::pace::pace_refresh_due(");
    const std::size_t record = position_of(body, "pace_refresh_attempted_at_[*pace_id] = block_height;");
    const std::size_t rpc = position_of(body, "co_await wallet_->get_wallet_balance(pace_wid)");
    ASSERT_NE(due, std::string::npos);
    ASSERT_NE(record, std::string::npos);
    ASSERT_NE(rpc, std::string::npos);
    EXPECT_LT(due, record);
    EXPECT_LT(record, rpc);
}

TEST(PaceWiring, FairValueCapRunsOnEveryManagedLadder) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t shape = position_of(body, "strategy::pace::shape_bid_side(pcs.ladder, pcs.pace);");
    const std::size_t cap = position_of(body,
        "strategy::pace::cap_bids_at_fair_value(pcs.ladder, pcs.pace, config_.strategy.pace_min_edge_bps, "
        "config_.strategy.pace_edge_sigma_mult, pace_tier_spacing_bps, static_cast<double>(pcs.quote_mid_mojos));");
    const std::size_t funding = position_of(body, "[S3 rounds 5+7] Re-enforce EVERY side's funding budget");
    ASSERT_NE(shape, std::string::npos);
    ASSERT_NE(cap, std::string::npos);
    ASSERT_NE(funding, std::string::npos);
    ASSERT_LT(shape, cap);
    EXPECT_LT(cap, funding);
    // Not gated on the tightening state: nothing between the size post-pass
    // and the cap reads tighten_bps.
    EXPECT_EQ(body.substr(shape, cap - shape).find("tighten_bps"), std::string::npos);
}

TEST(PaceWiring, TierSpacingIsTheOrderBookGuardStep) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    // The guard's own step and the pace copy of it: if either changes alone,
    // this fails.
    EXPECT_NE(position_of(body, "const double step_bps = std::max(50.0, config_.strategy.fair_value_clamp_tier_step_bps);"),
              std::string::npos);
    EXPECT_NE(position_of(body,
                          "const double pace_tier_spacing_bps = std::max(50.0, config_.strategy.fair_value_clamp_tier_step_bps);"),
              std::string::npos);
    EXPECT_NE(position_of(body, "pace_guards.tier_step_bps = pace_tier_spacing_bps;"), std::string::npos);
}

TEST(PaceWiring, RestingBidsAboveFairValueCancelled) {
    const std::string body = function_body(read_engine_cpp(), kCapsSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body,
                          "pace_add(strategy::pace::select_resting_above_fair_value(pace_resting, pcs.pace), \"pace_above_fv\");"),
              std::string::npos);
}
