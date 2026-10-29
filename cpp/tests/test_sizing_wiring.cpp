// test_sizing_wiring.cpp -- source scans of Step 7's minimum-size and reserve
// glue ([PAIRMIN 2026-10-26]), and of Step 4's target-gap bid floor
// ([TARGET-GAP 2026-10-26]).
//
// wmilliETH.b/XCH posted no offers on 2026-10-26: the deploy-idle floor sized
// a pool to the GLOBAL strategy.min_offer_size_units whatever the pair's
// override said, and Step 7 sized a CAT with no asset_reserve_units entry to
// its whole balance while Step 8's reserve gates held strategy.min_reserve_units
// back -- so the only ask breached the reserve and its whole side was
// suppressed every cycle.  The arithmetic is unit-tested in test_config.cpp
// (effective_min_offer_units) and test_asset_reserve.cpp
// (sizing_reserve_units); these scans pin that engine.cpp calls it, and where.
// Like test_pace_wiring.cpp, each scan is mutation-checked: it fails against
// the engine.cpp this change replaced.

#include <gtest/gtest.h>

#include "engine_source_scan.hpp"

#include <cstddef>
#include <string>

namespace {

using xop_tests::engine_scan::count_of;
using xop_tests::engine_scan::function_body;
using xop_tests::engine_scan::position_of;
using xop_tests::engine_scan::read_engine_cpp;
using xop_tests::engine_scan::squeeze;

const char* const kLadderSignature = "void Engine::step_generate_ladder(BlockHeight block_height)";
const char* const kReviveSignature =
    "asio::awaitable<void> Engine::step_revive_maintenance(BlockHeight block_height)";

const char* const kPairMinCall =
    "effective_min_offer_units(*pair_cfg, config_.strategy.min_offer_size_units)";
const char* const kSizingReserveCall =
    "execution::sizing_reserve_units(config_.strategy.asset_reserve_units, "
    "config_.strategy.min_reserve_units, config_.strategy.wallet_balance_caps_enabled, asset_id)";

}  // namespace

TEST(SizingWiring, DeployIdleFloorUsesThePairsMinimum) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "const double floor_min_units = pair_cfg ? "
                                "effective_min_offer_units(*pair_cfg, config_.strategy.min_offer_size_units) : 0.0;"),
              std::string::npos);
    EXPECT_NE(position_of(body, "&& floor_min_units > 0.0)"), std::string::npos);
    EXPECT_NE(position_of(body, "const Mojo min_pool = static_cast<Mojo>(std::llround("
                                "floor_min_units * static_cast<double>(pair_cfg->base_mojos_per_unit)));"),
              std::string::npos);
    EXPECT_NE(position_of(body, "const double required_quote_units = floor_min_units * market_mid;"),
              std::string::npos);
    // The global minimum reaches Step 7 only through the helper: the floor
    // and the up-scale, nowhere else.
    EXPECT_EQ(count_of(body, kPairMinCall), 2u);
    EXPECT_EQ(count_of(body, "config_.strategy.min_offer_size_units"), count_of(body, kPairMinCall));
}

TEST(SizingWiring, UpScaleAndDustFilterUseThePairsMinimum) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t floor_at = position_of(body, "const double floor_min_units =");
    const std::size_t upscale_at =
        position_of(body, std::string("const double eff_min_units = ") + kPairMinCall + ";");
    ASSERT_NE(floor_at, std::string::npos);
    ASSERT_NE(upscale_at, std::string::npos);
    EXPECT_LT(floor_at, upscale_at);
    EXPECT_EQ(position_of(body, "min_offer_size_units_override.value_or"), std::string::npos);
}

TEST(SizingWiring, Step7SizesEveryCatToLeaveTheStep8Reserve) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    // The final caps and the post-up-scale budget filter, in that order,
    // after the floor and around the up-scale.
    EXPECT_EQ(count_of(body, kSizingReserveCall), 2u);
    const std::size_t floor_at = position_of(body, "const double floor_min_units =");
    const std::size_t caps_at = position_of(body, kSizingReserveCall);
    const std::size_t upscale_at = position_of(body, "const double eff_min_units =");
    const std::size_t funding_at = position_of(body, "[S3 rounds 5+7] Re-enforce EVERY side's funding budget");
    ASSERT_NE(floor_at, std::string::npos);
    ASSERT_NE(caps_at, std::string::npos);
    ASSERT_NE(upscale_at, std::string::npos);
    ASSERT_NE(funding_at, std::string::npos);
    EXPECT_LT(floor_at, caps_at);
    EXPECT_LT(caps_at, upscale_at);
    const std::size_t budget_at = body.find(squeeze(kSizingReserveCall), caps_at + 1);
    ASSERT_NE(budget_at, std::string::npos);
    EXPECT_LT(funding_at, budget_at);
    // Neither site is limited to assets with an asset_reserve_units entry.
    EXPECT_EQ(position_of(body, "if (pair_cfg && !config_.strategy.asset_reserve_units.empty())"),
              std::string::npos);
    EXPECT_EQ(position_of(body, "config_.strategy.asset_reserve_units.find("), std::string::npos);
}

TEST(SizingWiring, ReviveMaintenanceUsesThePairsMinimum) {
    const std::string body = function_body(read_engine_cpp(), kReviveSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "const double min_units = "
                                "effective_min_offer_units(pc, config_.strategy.min_offer_size_units);"),
              std::string::npos);
    EXPECT_EQ(position_of(body, "min_offer_size_units_override"), std::string::npos);
}

// -- [TARGET-GAP 2026-10-26] Step 4's target-gap bid floor --------------------
// XCH/wUSDC.b posted nothing on 2026-10-26: the strategy's bid was q_max 20 -
// 34.76 XCH held = 0 while XCH sat at 13.8% against a 50% target.  The floor's
// arithmetic is unit-tested in test_target_gap.cpp; these scans pin that Step 4
// values the portfolio only when the switch is on and only from validated,
// fresh balances, applies the floor once every pair is quoted and before
// anything reads the size, shares each coin's gap across the pairs, skips a
// quote the strategy rejected, only ever raises the bid, converts it at the
// pair's own mojos per unit, and that the pace balance refresh keeps the
// targeted balances fresh while the floor is on, and every writer of the
// balance cache reads from a synced wallet only.

namespace {

const char* const kComputeQuotesSignature = "void Engine::step_compute_quotes(BlockHeight block_height)";
const char* const kHeartbeatSignature = "asio::awaitable<void> Engine::on_new_block_coro(BlockHeight block_height)";
const char* const kRefreshSignature = "asio::awaitable<void> Engine::refresh_pace_balances(BlockHeight block_height)";
const char* const kStep4Log = "spdlog::debug(\"[Engine] Step 4: {} bid={:.6f} ask={:.6f} spread={:.1f}bps \"";
const char* const kValuationCall =
    "strategy::target_gap::value_targeted(gap_holdings, config_.strategy.pace_max_balance_age_blocks);";
const char* const kSharedCall = "strategy::target_gap::shared_bid_floors_xch(gap_valuation, gap_pairs);";

}  // namespace

TEST(SizingWiring, TargetGapValuationIsGatedValidatedAndFresh) {
    const std::string body = function_body(read_engine_cpp(), kComputeQuotesSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t log_at = position_of(body, kStep4Log);
    const std::size_t gate_at = position_of(body, "if (!config_.strategy.target_gap_sizing_enabled) {");
    const std::size_t value_at = position_of(body, kValuationCall);
    ASSERT_NE(log_at, std::string::npos);
    ASSERT_NE(gate_at, std::string::npos);
    ASSERT_NE(value_at, std::string::npos);
    EXPECT_LT(log_at, gate_at) << "after the pair loop: once per heartbeat";
    EXPECT_LT(gate_at, value_at);
    // Every targeted coin, from the balances Step 8 and the pace refresh
    // validated, with their age, at the marks the drift guard reads; a
    // balance with no mark is unpriced.
    EXPECT_NE(position_of(body, "for (const auto& [gap_key, gap_target] : config_.strategy.asset_target_allocations)"),
              std::string::npos);
    EXPECT_NE(position_of(body, "gap_cached != cached_wallet_balances_.end() && gap_cached->second.fields_validated"),
              std::string::npos);
    EXPECT_NE(position_of(body, "const Mojo gap_confirmed = gap_cached->second.confirmed;"), std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.balance_known = true;"), std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.balance_age_blocks = block_height > gap_cached->second.as_of_block "
                                "? block_height - gap_cached->second.as_of_block : 0u;"),
              std::string::npos);
    EXPECT_NE(position_of(body, "gap_holding.priced = gap_confirmed == 0 || gap_rate > 0.0;"), std::string::npos);
    EXPECT_EQ(count_of(body, "value_targeted("), 1u);
    // An unusable valuation posts no floor.
    const std::size_t off_at = position_of(body, "if (!gap_valuation.ok) { return; }");
    ASSERT_NE(off_at, std::string::npos);
    EXPECT_LT(value_at, off_at);
}

TEST(SizingWiring, TargetGapFloorIsSharedAndOnlyRaisesAUsableBid) {
    const std::string body = function_body(read_engine_cpp(), kComputeQuotesSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t strategy_at =
        position_of(body, "pcs.raw_quote = strategy.compute_quotes(mid, sigma, q, block_height);");
    const std::size_t log_at = position_of(body, kStep4Log);
    const std::size_t usable_at = position_of(body, "if (!pcs.quote_valid || !std::isfinite(pcs.raw_quote.spread_bps) "
                                                    "|| !(pcs.raw_quote.spread_bps > 0.0)) { continue; }");
    const std::size_t claim_at = position_of(body, "gap_pairs.push_back(std::move(gap_coins));");
    const std::size_t shared_at = position_of(body, kSharedCall);
    const std::size_t units_at = position_of(body,
        "strategy::target_gap::floor_units(gap_floors_xch[gap_i], gap_base_xch_per_unit, "
        "static_cast<double>(gap_pc->base_mojos_per_unit));");
    const std::size_t raise_at = position_of(body, "if (gap_floor > 0.0 && gap_floor > gap_pcs.raw_quote.bid_size) {");
    const std::size_t assign_at = position_of(body, "gap_pcs.raw_quote.bid_size = gap_floor;");
    ASSERT_NE(strategy_at, std::string::npos);
    ASSERT_NE(log_at, std::string::npos);
    ASSERT_NE(usable_at, std::string::npos);
    ASSERT_NE(claim_at, std::string::npos);
    ASSERT_NE(shared_at, std::string::npos);
    ASSERT_NE(units_at, std::string::npos);
    ASSERT_NE(raise_at, std::string::npos);
    ASSERT_NE(assign_at, std::string::npos);
    EXPECT_LT(strategy_at, log_at);
    EXPECT_LT(log_at, usable_at) << "every pair is quoted before any floor";
    EXPECT_LT(usable_at, claim_at) << "a rejected quote claims no share";
    EXPECT_LT(claim_at, shared_at);
    EXPECT_LT(shared_at, units_at);
    EXPECT_LT(units_at, raise_at);
    EXPECT_LT(raise_at, assign_at);
    // floor_units() caps the size where Step 6's llround stays in range
    // (TargetGapUnits.CapsTheSizeWhereStep6CanConvertIt): that needs the
    // pair's own mojos per unit, and nothing else converts the floor.
    EXPECT_EQ(count_of(body, "floor_units("), 1u);
    // The base coin's XCH per unit at the mark the valuation used.
    EXPECT_NE(position_of(body, "state_->get_asset_xch_rate(AssetId{gap_pc->base_asset_id}) "
                                "* static_cast<double>(gap_pc->base_mojos_per_unit) "
                                "/ static_cast<double>(kMojosPerXch);"),
              std::string::npos);
    // The shared floors are the only ones: no pair takes an unshared floor.
    EXPECT_EQ(count_of(body, "shared_bid_floors_xch("), 1u);
    EXPECT_EQ(position_of(body, "unshared_bid_floor_xch("), std::string::npos);
    // The floor is the only write to the sizes in Step 4, and the ask is
    // never touched: the strategy sizes it q_max + q.
    EXPECT_EQ(count_of(body, "raw_quote.bid_size ="), 1u);
    EXPECT_EQ(count_of(body, "raw_quote.ask_size ="), 0u);
}

TEST(SizingWiring, TheBalanceRefreshRunsWhileTheTargetGapFloorIsOn) {
    const std::string body = function_body(read_engine_cpp(), kHeartbeatSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "if (config_.strategy.pace_enabled || config_.strategy.target_gap_sizing_enabled) { "
                                "try { co_await refresh_pace_balances(block_height); }"),
              std::string::npos);
    // The refresh's own gate is pinned with its other gates in
    // test_pace_wiring.cpp (PaceWiring.BalanceRefreshClosedByEveryPostingGate).
}

TEST(SizingWiring, TheBalanceRefreshKeepsItsBackoffWhilePaceIsOff) {
    // step_evaluate_pace() resets pace's state every heartbeat while pace is
    // off.  The refresh's backoff (one RPC per asset per refresh age, a warning
    // on the first failure of a run) must survive that while the target-gap
    // floor still runs the refresh, or a failing reply is retried every
    // heartbeat.
    const std::string body =
        function_body(read_engine_cpp(), "void Engine::step_evaluate_pace(BlockHeight block_height)");
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t off_at = position_of(body, "if (!config_.strategy.pace_enabled) {");
    const std::size_t kept_at = position_of(body, "if (!config_.strategy.target_gap_sizing_enabled) { "
                                                  "pace_refresh_attempted_at_.clear(); "
                                                  "pace_refresh_failures_.clear(); }");
    ASSERT_NE(off_at, std::string::npos);
    ASSERT_NE(kept_at, std::string::npos);
    EXPECT_LT(off_at, kept_at);
    // Cleared nowhere else.
    EXPECT_EQ(count_of(body, "pace_refresh_attempted_at_.clear();"), 1u);
    EXPECT_EQ(count_of(body, "pace_refresh_failures_.clear();"), 1u);
    EXPECT_EQ(count_of(read_engine_cpp(), "pace_refresh_attempted_at_.clear()"), 1u);
    EXPECT_EQ(count_of(read_engine_cpp(), "pace_refresh_failures_.clear()"), 1u);
}

TEST(SizingWiring, TheBalanceRefreshReadsOnlyFromASyncedWallet) {
    // A wallet that is not synced can report a partial balance, and the next
    // heartbeat's Step 4 values the targeted coins from the refresh's cache
    // before Step 8 reads anything.  So the refresh asks the wallet once,
    // before its first due read, reads it the way Step 8's gate does, and
    // records no attempt when it is not synced, so the balance stays due.
    const std::string body = function_body(read_engine_cpp(), kRefreshSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t due_at = position_of(body, "if (!strategy::pace::pace_refresh_due(");
    const std::size_t ask_at = position_of(body, "if (!pace_wallet_synced.has_value()) {");
    const std::size_t sync_at = position_of(body, "co_await wallet_->get_sync_status();");
    const std::size_t stop_at = position_of(body, "if (!*pace_wallet_synced) { co_return; }");
    const std::size_t record_at = position_of(body, "pace_refresh_attempted_at_[*pace_id] = block_height;");
    const std::size_t read_at = position_of(body, "co_await wallet_->get_wallet_balance(pace_wid)");
    ASSERT_NE(due_at, std::string::npos);
    ASSERT_NE(ask_at, std::string::npos);
    ASSERT_NE(sync_at, std::string::npos);
    ASSERT_NE(stop_at, std::string::npos);
    ASSERT_NE(record_at, std::string::npos);
    ASSERT_NE(read_at, std::string::npos);
    EXPECT_LT(due_at, ask_at) << "asked only when a balance is due";
    EXPECT_LT(ask_at, sync_at);
    EXPECT_LT(sync_at, stop_at);
    EXPECT_LT(stop_at, record_at) << "an unsynced wallet records no attempt";
    EXPECT_LT(record_at, read_at);
    EXPECT_EQ(count_of(body, "get_sync_status("), 1u) << "asked once per refresh";
    // Starts unsynced, so a reply that throws reads nothing; synced only as
    // Step 8's gate reads it.
    EXPECT_NE(position_of(body, "pace_wallet_synced = false; try {"), std::string::npos);
    EXPECT_NE(position_of(body, "const bool pace_synced = pace_sync.contains(\"synced\") "
                                "&& pace_sync[\"synced\"].is_boolean() && pace_sync[\"synced\"].get<bool>();"),
              std::string::npos);
    EXPECT_NE(position_of(body, "const bool pace_syncing = !pace_sync.contains(\"syncing\") "
                                "|| !pace_sync[\"syncing\"].is_boolean() || pace_sync[\"syncing\"].get<bool>();"),
              std::string::npos);
    EXPECT_NE(position_of(body, "pace_wallet_synced = pace_synced && !pace_syncing;"), std::string::npos);
}

TEST(SizingWiring, EveryBalanceCacheWriterReadsFromASyncedWallet) {
    // fields_validated says only that a reply carried its fields.  Step 4
    // values the targeted coins from cached_wallet_balances_ before Step 8
    // reads anything (Step 7's wallet caps and the ledger invariant read it
    // too), and a mid-sync reply is a partial balance: the first synced
    // heartbeat after a resync would size from it.  So every writer reads from
    // a wallet known synced.  There are five: three in Step 8, below its sync
    // gate; the pace refresh, after its own check; and the bridge scans'
    // fetch, whose every caller asks secondary_wallet_synced() first.
    const std::string raw = read_engine_cpp();
    const std::string engine = squeeze(raw);
    EXPECT_EQ(count_of(engine, "cached_wallet_balances_["), 5u) << "a new writer needs its own sync check";

    const std::string step8 =
        function_body(raw, "asio::awaitable<void> Engine::step_manage_offers(BlockHeight block_height)");
    ASSERT_FALSE(step8.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t gate_at = position_of(step8, "step8_sync_gate_passed_ = true;");
    ASSERT_NE(gate_at, std::string::npos);
    EXPECT_EQ(count_of(step8, "cached_wallet_balances_["), 3u);
    EXPECT_LT(gate_at, position_of(step8, "cached_wallet_balances_["));

    const std::string refresh = function_body(raw, kRefreshSignature);
    ASSERT_FALSE(refresh.empty());
    EXPECT_EQ(count_of(refresh, "cached_wallet_balances_["), 1u);
    EXPECT_LT(position_of(refresh, "if (!*pace_wallet_synced) { co_return; }"),
              position_of(refresh, "cached_wallet_balances_["));

    const std::string fetch = function_body(raw, "asio::awaitable<bool> Engine::fetch_bridge_balance(");
    ASSERT_FALSE(fetch.empty());
    EXPECT_EQ(count_of(fetch, "cached_wallet_balances_["), 1u);
    EXPECT_EQ(count_of(engine, "co_await fetch_bridge_balance("), 3u) << "the three callers below";
    const std::string primary = function_body(raw, "asio::awaitable<void> Engine::step_ingest_bridge_flows(");
    ASSERT_FALSE(primary.empty());
    EXPECT_NE(position_of(primary, "snapshot_current = co_await secondary_wallet_synced(block_height) "
                                   "&& co_await fetch_bridge_balance(asset, block_height);"),
              std::string::npos);
    EXPECT_EQ(count_of(primary, "fetch_bridge_balance("), 1u);
    const std::string secondary =
        function_body(raw, "asio::awaitable<void> Engine::step_ingest_secondary_bridge_flows(");
    ASSERT_FALSE(secondary.empty());
    EXPECT_NE(position_of(secondary, "if (!co_await secondary_wallet_synced(block_height) "
                                     "|| !co_await fetch_bridge_balance(asset, block_height)) {"),
              std::string::npos);
    EXPECT_EQ(count_of(secondary, "fetch_bridge_balance("), 1u);
    const std::string takes = function_body(raw, "asio::awaitable<void> Engine::step_ingest_topup_takes(");
    ASSERT_FALSE(takes.empty());
    const std::size_t takes_synced_at =
        position_of(takes, "if (!co_await secondary_wallet_synced(block_height)) co_return false;");
    const std::size_t takes_fetch_at =
        position_of(takes, "(void)co_await fetch_bridge_balance(a, block_height, &validated);");
    ASSERT_NE(takes_synced_at, std::string::npos);
    ASSERT_NE(takes_fetch_at, std::string::npos);
    EXPECT_LT(takes_synced_at, takes_fetch_at);
    EXPECT_EQ(count_of(takes, "fetch_bridge_balance("), 1u);

    // Synced means Step 8 passed its gate this heartbeat, or an on-demand
    // reading the way that gate reads it.
    const std::string synced =
        function_body(raw, "asio::awaitable<bool> Engine::secondary_wallet_synced(BlockHeight block_height)");
    ASSERT_FALSE(synced.empty());
    EXPECT_NE(position_of(synced, "if (step8_sync_gate_passed_) co_return true;"), std::string::npos);
    EXPECT_NE(position_of(synced, "secondary_sync_ok_ = synced && !syncing;"), std::string::npos);
}

// -- [TIERMIN 2026-10-29] Step 7's per-tier minimums and per-pair cap --------
// Every XCH/wUSDC.b bid sat at the flat 5 XCH cap on 2026-10-29 and the
// operator asked for 10 XCH at the tight tier down to 1 XCH at the wide one.
// The arithmetic is unit-tested in test_config.cpp (tier_min_offer_units,
// effective_max_offer_units); these scans pin that Step 7 caps through the
// pair's own cap, never below a tier's own minimum, up-scales each tier to
// its own minimum (not on a pace-managed pair), and that the funding filter
// sets the minimums aside for a side whose tightest tier the wallet cannot
// fund, so the side falls back to the plain ladder instead of vanishing.

TEST(SizingWiring, Step7CapsThroughThePairsOwnCapNeverBelowATiersMinimum) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(body, "const double eff_max_units = effective_max_offer_units("
                                "*pair_cfg, config_.strategy.max_offer_size_units);"),
              std::string::npos);
    EXPECT_EQ(count_of(body, "config_.strategy.max_offer_size_units"), 1u)
        << "the global cap reaches Step 7 only through the helper";
    EXPECT_NE(position_of(body, "const Mojo tier_cap = std::max(max_base_mojos, tier_min_mojos(tq));"),
              std::string::npos);
    EXPECT_NE(position_of(body, "if (tq.size > tier_cap) {"), std::string::npos);
    EXPECT_EQ(position_of(body, "if (tq.size > max_base_mojos) {"), std::string::npos);
}

TEST(SizingWiring, Step7UpScalesEachTierToItsOwnMinimum) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t on_at = position_of(body,
        "const bool tier_minimums_on = has_tier_minimums(*pair_cfg) "
        "&& !(config_.strategy.pace_enabled && pcs.pace.managed);");
    const std::size_t line_at = position_of(body,
        "tier_min_offer_units(*pair_cfg, eff_min_units, tq.tier_index, ladder_cfg.num_tiers)");
    const std::size_t upscale_at = position_of(body, "const Mojo tier_min = tier_min_mojos(tq);");
    ASSERT_NE(on_at, std::string::npos);
    ASSERT_NE(line_at, std::string::npos);
    ASSERT_NE(upscale_at, std::string::npos);
    EXPECT_LT(on_at, line_at);
    EXPECT_LT(line_at, upscale_at);
    EXPECT_NE(position_of(body, "if (tq.size < tier_min) {"), std::string::npos);
    EXPECT_EQ(position_of(body, "if (tq.size < min_base_mojos) { const Mojo old_size"),
              std::string::npos);
}

TEST(SizingWiring, FundingFilterFallsBackToThePlainLadder) {
    const std::string body = function_body(read_engine_cpp(), kLadderSignature);
    ASSERT_FALSE(body.empty()) << XOP_TESTS_ENGINE_CPP;
    const std::size_t plain_at = position_of(body,
        "plain_tier_sizes[{tq.side, tq.tier_index}] = std::max(capped, min_base_mojos);");
    const std::size_t funding_at = position_of(body, "[S3 rounds 5+7] Re-enforce EVERY side's funding budget");
    const std::size_t fallback_at = position_of(body, "if (keep == 0 && !costs.empty()) {");
    ASSERT_NE(plain_at, std::string::npos);
    ASSERT_NE(funding_at, std::string::npos);
    ASSERT_NE(fallback_at, std::string::npos);
    EXPECT_LT(plain_at, funding_at);
    EXPECT_LT(funding_at, fallback_at);
    // Restores only downwards (a revive cap may already have gone lower),
    // then re-measures the side before the drop.
    EXPECT_NE(position_of(body, "if (plain != plain_tier_sizes.end() && plain->second < tq.size) {"),
              std::string::npos);
    const std::size_t remeasure_at = body.find(
        squeeze("costs = side_costs(); keep = tiers_within_budget(costs, *budget);"), fallback_at);
    const std::size_t drop_at = body.find(squeeze("if (keep >= costs.size()) continue;"), fallback_at);
    ASSERT_NE(remeasure_at, std::string::npos);
    ASSERT_NE(drop_at, std::string::npos);
    EXPECT_LT(remeasure_at, drop_at);
}

TEST(SizingWiring, PaceBandSeesThePairsOwnCap) {
    const std::string all = squeeze(read_engine_cpp());
    ASSERT_FALSE(all.empty()) << XOP_TESTS_ENGINE_CPP;
    EXPECT_NE(position_of(all, "pair_in.max_offer_override = pace_pc.max_offer_size_units_override;"),
              std::string::npos);
}
