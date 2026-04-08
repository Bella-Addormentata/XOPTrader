"""Patch engine.cpp to add coin pool maintenance step.

Uses binary-safe read/write to preserve the triple-encoded UTF-8.
All markers use simple substring matches that work regardless of line endings.
"""
import pathlib

ENGINE = pathlib.Path(r"C:\GitHub\XOPTrader\cpp\src\engine.cpp")

data = ENGINE.read_bytes()
text = data.decode("utf-8")

# Detect line ending style
NL = "\r\n" if "\r\n" in text else "\n"

# ============================================================================
# PATCH 1: Add step_maintain_coin_pool() before closing namespace
# ============================================================================

lines = [
    "",
    "// ---------------------------------------------------------------------------",
    "// step_maintain_coin_pool -- ensure enough pre-split XCH coins for trading",
    "// ---------------------------------------------------------------------------",
    "",
    "asio::awaitable<void> Engine::step_maintain_coin_pool(BlockHeight block_height)",
    "{",
    "    const int target_count = config_.strategy.coin_pool_target_count;",
    "    if (target_count <= 0) {",
    "        co_return;",
    "    }",
    "",
    "    const double target_xch = config_.strategy.coin_pool_target_xch;",
    "    const auto target_mojos = static_cast<Mojo>(",
    "        std::llround(target_xch * static_cast<double>(kMojosPerXch)));",
    "",
    "    // Skip if prior splits are still confirming.",
    "    try {",
    '        auto bal = co_await wallet_->get_wallet_balance(1);',
    "        Mojo pending_change = 0;",
    '        if (bal.contains("pending_change"))',
    '            pending_change = bal["pending_change"].get<Mojo>();',
    "        if (pending_change > 0) {",
    '            spdlog::debug("[Engine] Coin pool: pending_change={} mojos, "',
    '                          "waiting for prior splits to confirm",',
    "                          pending_change);",
    "            co_return;",
    "        }",
    "    } catch (const std::exception& e) {",
    '        spdlog::warn("[Engine] Coin pool: balance check failed: {}", e.what());',
    "        co_return;",
    "    }",
    "",
    "    int free_count = 0;",
    "    try {",
    "        free_count = co_await coin_mgr_->count_free_coins(1);",
    "    } catch (const std::exception& e) {",
    '        spdlog::warn("[Engine] Coin pool: count_free_coins failed: {}", e.what());',
    "        co_return;",
    "    }",
    "",
    "    if (free_count >= target_count) {",
    '        spdlog::debug("[Engine] Coin pool: {} free coins >= target {} -- OK",',
    "                      free_count, target_count);",
    "        coin_pool_last_block_ = block_height;",
    "        co_return;",
    "    }",
    "",
    '    spdlog::info("[Engine] Coin pool: {} free coins < target {} -- "',
    '                 "splitting to create {} more coins of {:.2f} XCH each",',
    "                 free_count, target_count,",
    "                 target_count - free_count, target_xch);",
    "",
    "    std::string address;",
    "    try {",
    "        address = co_await wallet_->get_next_address(1, false);",
    "    } catch (const std::exception& e) {",
    '        spdlog::error("[Engine] Coin pool: get_next_address failed: {}", e.what());',
    "        co_return;",
    "    }",
    "",
    "    constexpr Mojo split_fee = 0;",
    "",
    "    try {",
    "        auto result = co_await coin_mgr_->ensure_split(",
    "            1, target_count, target_mojos, address, split_fee);",
    "",
    "        if (result.success) {",
    '            spdlog::info("[Engine] Coin pool: created {} new coins "',
    '                         "(fee={} mojos total)",',
    "                         result.coins_created, result.fee_paid);",
    "        } else if (result.coins_created > 0) {",
    '            spdlog::warn("[Engine] Coin pool: partial split -- created "',
    '                         "{} of {} needed coins",',
    "                         result.coins_created,",
    "                         target_count - free_count);",
    "        } else {",
    '            spdlog::error("[Engine] Coin pool: split failed "',
    '                          "(insufficient balance or RPC error)");',
    "        }",
    "    } catch (const std::exception& e) {",
    '        spdlog::error("[Engine] Coin pool: ensure_split failed: {}", e.what());',
    "    }",
    "",
    "    coin_pool_last_block_ = block_height;",
    "    co_return;",
    "}",
    "",
]

NEW_FUNCTION = NL.join(lines)

CLOSING_NS = "}  // namespace xop"
assert CLOSING_NS in text, "closing namespace not found"
text = text.replace(CLOSING_NS, NEW_FUNCTION + CLOSING_NS)
print("PATCH 1 OK: step_maintain_coin_pool() added")

# ============================================================================
# PATCH 2: Startup call (before market analysis)
# ============================================================================

STARTUP_FIND = "    // -- Startup market analysis phase"
assert STARTUP_FIND in text, "startup marker not found"

startup_lines = [
    "    // -- Coin pool maintenance at startup ------------------------------------",
    "    if (!wallet_circuit_open_ && config_.strategy.coin_pool_target_count > 0) {",
    "        try {",
    "            co_await step_maintain_coin_pool(0);",
    "        } catch (const std::exception& ex) {",
    '            spdlog::warn("[Engine] Startup coin pool maintenance failed: {}; "',
    '                         "continuing without splitting", ex.what());',
    "        }",
    "    }",
    "",
]

STARTUP_INSERT = NL.join(startup_lines)
text = text.replace(STARTUP_FIND, STARTUP_INSERT + STARTUP_FIND, 1)
print("PATCH 2 OK: startup coin pool call added")

# ============================================================================
# PATCH 3: Periodic call before Step 3
# ============================================================================

STEP3_FIND = "    try { step_update_analytics(block_height); }"
assert STEP3_FIND in text, "Step 3 marker not found"

periodic_lines = [
    "    // -- Periodic coin pool maintenance ------------------------------------",
    "    if (!wallet_circuit_open_",
    "        && config_.strategy.coin_pool_target_count > 0",
    "        && config_.strategy.coin_pool_interval_blocks > 0",
    "        && block_height >= coin_pool_last_block_",
    "                           + config_.strategy.coin_pool_interval_blocks) {",
    "        try {",
    "            co_await step_maintain_coin_pool(block_height);",
    "        } catch (const std::exception& e) {",
    '            spdlog::warn("[Engine] Coin pool maintenance failed: {}", e.what());',
    "        }",
    "    }",
    "",
]

PERIODIC_INSERT = NL.join(periodic_lines)
text = text.replace(STEP3_FIND, PERIODIC_INSERT + STEP3_FIND, 1)
print("PATCH 3 OK: periodic coin pool call added")

# Write back
ENGINE.write_bytes(text.encode("utf-8"))
print("All patches applied successfully!")
