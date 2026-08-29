# Code Review: Open Pull Requests (#115, #116, #117)

**Review Date:** 2026-08-27  
**Reviewer:** GitHub Copilot (Gemini 3.7 Flash)  
**Workspace:** `Bella-Addormentata/XOPTrader`  
**Review Target:** All currently open pull requests on GitHub  

---

## Executive Summary

As of 2026-08-27, there are **three open pull requests** in the repository:

1. **[PR #115](https://github.com/Bella-Addormentata/XOPTrader/pull/115)**: `feat/peg-registry` -> `main`  
   *Title:* "Retire the hardcoded USD par: peg identity becomes an asset property"  
   *Scope:* Asset-level peg registry (`PegRegistry`), removal of hardcoded `$1.00` literals in valuation paths, asset ID indexing, config-driven `enforce` toggle, and non-USD peg structure with FX requirements.

2. **[PR #116](https://github.com/Bella-Addormentata/XOPTrader/pull/116)**: `docs/permuto-research` -> `main`  
   *Title:* "Permuto competition research and SVPerp notes (split from #115)"  
   *Scope:* Documentation and research for the Permuto Capital perps competition (`TODO-COMPETITION.md`), complete endpoint mapping (`docs/permuto-api-reference.md`), and SVPerp academic literature & live empirical measurements (`docs/advanced-trading-methods.md`).

3. **[PR #117](https://github.com/Bella-Addormentata/XOPTrader/pull/117)**: `fix/s27-equity-blindness` -> `feat/peg-registry` (Stacked on #115)  
   *Title:* "S27: equity computed to $0 and both breakers went inert"  
   *Scope:* Fixes XCH USD valuation by prioritizing external CoinGecko price feed with DEX-derived fallback, marks unpriced/never-valued holdings as unconditionally degrading the valuation cycle, and adds pure math unit tests documenting zero-peak drawdown behavior.

Below is the detailed review of each pull request, identifying bugs, edge cases, compiler warnings, design gaps, and specific code change recommendations.

---

## 1. Review of PR #115 (`feat/peg-registry`)

### 1.1 Overview & Strengths
- **Clean Architectural Separation:** Moves peg definitions from implicit pair properties (`PairConfig::is_stablecoin` or hardcoded ticker substring matches) into an explicit, asset-level `PegRegistry` keyed on canonical asset IDs (e.g., CAT tail hashes or `"xch"`).
- **Enforce Switch:** Introduces `enforce: false`, allowing operators to quickly revoke peg assumptions for wound-down or compromised assets without deleting configuration records.
- **Fail-Loud Configuration:** Validates `bail_pct > warn_pct`, positive finite targets, and rejects malformed sequence types (`!root["pegged_assets"].IsSequence()`), preventing silent zero-valuation states caused by YAML indentation errors.
- **Comprehensive Unit Tests:** 16 tests in `cpp/tests/test_peg_registry.cpp` and 8 parser-level tests in `cpp/tests/test_config.cpp`.

---

### 1.2 Identified Issues & Errors

#### Issue 1.2.1: Unused Variable Compilation Warnings (`-Wall -Werror` Hazard)
In `cpp/src/engine.cpp`, the functions `Engine::quote_usd_factor_is_par` and `Engine::quote_usd_factor` retain legacy substring parsing logic:
```cpp
const auto slash = pc.name.find('/');
const std::string quote = (slash == std::string::npos)
    ? std::string{}
    : pc.name.substr(slash + 1);
```
Since `quote` is no longer referenced anywhere in either function body after switching to `is_par_wrapper_quote(pc)` and `quote_prefers_market_cross(pc)`, compilers (GCC/Clang) emit `-Wunused-variable`. In release/CI builds where `-Werror` is enforced, this causes build failure.

#### Issue 1.2.2: Inconsistent Input Validation in `PegRegistry::deviation_pct`
In `cpp/include/xop/peg_registry.hpp`:
```cpp
[[nodiscard]] std::optional<double> deviation_pct(
    const std::string& asset_id,
    std::optional<double> observed) const
{
    const auto* a = find(asset_id);
    if (a == nullptr || !observed.has_value() || !(*observed > 0.0)) {
        return std::nullopt;
    }
    return (*observed - a->peg_target) / a->peg_target * 100.0;
}
```
1. **Finiteness Check:** Unlike `classify()`, `deviation_pct` does not check `std::isfinite(*observed)`. If `*observed` is `+inf`, `+inf > 0.0` evaluates to `true` and `deviation_pct` returns `+inf`.
2. **Enforcement Check:** `classify()` checks `if (a == nullptr || !a->enforce) return PegStatus::NotPegged;`. However, `deviation_pct()` computes a deviation even when `a->enforce == false`. It should return `std::nullopt` when the peg is not enforced.

#### Issue 1.2.3: Duplicate `asset_id` Declaration in YAML
In `cpp/src/config.cpp` (`parse_pegged_assets`), if an operator accidentally declares the same `asset_id` multiple times, `reg.add(std::move(a))` silently overwrites the previous entry via `by_asset_id_[key] = std::move(asset);`. Rejecting duplicate asset IDs with a `ConfigError` is more consistent with the fail-loud contract.

#### Issue 1.2.4: Runtime Monitoring Integration Gap (Acknowledged S30)
As noted in the PR description, `PegRegistry::classify` has no production caller in the main trading loop yet; `DepegDetector` is still attached to `PairConfig::is_stablecoin` and pair iterations. Disabling a pair like `BYC/wUSDC.b` still eliminates depeg monitoring for BYC. While correctly scoped for follow-up work under S30, the review confirms this remains an operational gap.

---

### 1.3 Specific Code Change Suggestions for PR #115

#### Change 1.3.1: Clean up unused `quote` and `slash` variables in `cpp/src/engine.cpp`

```diff
--- a/cpp/src/engine.cpp
+++ b/cpp/src/engine.cpp
@@ -11424,10 +11424,6 @@ bool Engine::quote_usd_factor_trusted(const PairConfig& pc) const
 bool Engine::quote_usd_factor_is_par(const PairConfig& pc) const
 {
-    const auto slash = pc.name.find('/');
-    const std::string quote = (slash == std::string::npos)
-        ? std::string{}
-        : pc.name.substr(slash + 1);
-
     if (is_par_wrapper_quote(pc)) {
         return true;
     }
@@ -11443,10 +11439,6 @@ bool Engine::quote_usd_factor_is_par(const PairConfig& pc) const
 double Engine::quote_usd_factor(const PairConfig& pc) const
 {
-    const auto slash = pc.name.find('/');
-    const std::string quote = (slash == std::string::npos)
-        ? std::string{}
-        : pc.name.substr(slash + 1);
-
     // Fiat-collateralised wrappers hold their peg tightly enough to treat
```

#### Change 1.3.2: Enhance `PegRegistry::deviation_pct` in `cpp/include/xop/peg_registry.hpp`

```diff
--- a/cpp/include/xop/peg_registry.hpp
+++ b/cpp/include/xop/peg_registry.hpp
@@ -279,7 +279,9 @@ public:
     [[nodiscard]] std::optional<double> deviation_pct(
         const std::string& asset_id,
         std::optional<double> observed) const
     {
         const auto* a = find(asset_id);
-        if (a == nullptr || !observed.has_value() || !(*observed > 0.0)) {
+        if (a == nullptr || !a->enforce || !observed.has_value()
+            || !std::isfinite(*observed) || !(*observed > 0.0)) {
             return std::nullopt;
         }
         return (*observed - a->peg_target) / a->peg_target * 100.0;
```

#### Change 1.3.3: Detect duplicate `asset_id` declarations in `cpp/src/config.cpp`

```diff
--- a/cpp/src/config.cpp
+++ b/cpp/src/config.cpp
@@ -443,6 +443,10 @@ PegRegistry parse_pegged_assets(const YAML::Node& root)
         if (item["prefer_market_cross"]) {
             a.prefer_market_cross = item["prefer_market_cross"].as<bool>();
         }
+        if (reg.find(a.asset_id) != nullptr) {
+            throw ConfigError("pegged_assets: duplicate asset_id '" + a.asset_id
+                              + "' declared for symbol '" + a.symbol + "'");
+        }
         if (!reg.add(std::move(a))) {
             // Loud: a half-declared peg silently dropped is how an asset
```

---

## 2. Review of PR #116 (`docs/permuto-research`)

### 2.1 Overview & Strengths
- **Clean Extraction:** Successfully separates >1,000 lines of venue and competition documentation from PR #115 into standalone markdown files.
- **Empirical Transparency:** Openly documents self-corrections (e.g., leaderboard pagination factor, monotonic score misconception, Cash Session oracle freeze).
- **Literature Review:** Appropriately separates verified academic literature (Demeterfi et al., Avellaneda-Stoikov, BitMEX perps) from unverified web citations.

---

### 2.2 Identified Issues & Improvements

#### Issue 2.2.1: Authentication Scope in `docs/permuto-api-reference.md`
- **Current Text (line 94):** *"All require a session token: `Authorization: Bearer <token>`..."*
- **Correction:** Several endpoints (`GET /info/markets`, `GET /info/meta`, `GET /leaderboard`, `GET /ready`, `GET /health`) are public and do not require session headers. Document the public exception explicitly so automated collectors do not fail on unauthenticated probes.

#### Issue 2.2.2: Probe Semantics (`/ready` vs `/health`)
- **Current Text (line 212):** Catalog lists `GET /ready` as *"k8s-style liveness"*.
- **Correction:** In standard Kubernetes architecture, `/ready` represents a *readiness* probe (controlling whether traffic is routed to an instance), whereas liveness probes control container restarts. Labeling `/ready` as readiness avoids operational confusion.

#### Issue 2.2.3: Realized Volatility vs. Implied Volatility Terminology
- In certain sections of `docs/permuto-api-reference.md` (e.g., line 87), the oracle values are referenced colloquially as "IV" (Implied Volatility), despite §0 correctly establishing that the oracle is a 60-second trailing Realized Volatility (RV) estimate. Standardizing on **RV** prevents confusion with options-derived implied volatility.

#### Issue 2.2.4: Cross-Reference to Task S31
- In `TODO-COMPETITION.md` (line 350), the text states: *"Filed as **S31** in `TODO.md`"*.
- Note: In `main`, `TODO.md` did not yet contain S31 (which is introduced in branch `feat/s31-dead-mans-switch`). Ensure `TODO.md` is synchronized across branches or clarify the branch dependency.

---

## 3. Review of PR #117 (`fix/s27-equity-blindness`)

### 3.1 Overview & Strengths
- **Root-Cause Resolution:** Resolves the critical failure from 2026-08-25 where disabling `wUSDC.b` pairs caused equity to collapse to `$0`, zeroing the high-water mark and rendering circuit breakers inert.
- **External Price Anchor:** Prioritizes CoinGecko external XCH/USD price feed (`coingecko_prices_`), reducing dependency on thin on-chain bridged stablecoin pairs while keeping the on-chain DEX mid as a fallback.
- **Unconditional Degradation on Never-Valued Assets:** Removes the requirement that an asset must have had a prior carry entry to trigger degradation; never-valued held assets now immediately flag `degraded = true`.
- **Pure Math Verification:** Adds unit tests in `cpp/tests/test_drawdown_breaker.cpp` documenting that `equity_drawdown_frac(0.0, ...)` returns `0.0`.

---

### 3.2 Identified Critical Issues & Errors

#### Issue 3.2.1: CRITICAL — Stale CoinGecko Cache Indefinitely Blocks DEX Fallback
In `cpp/src/engine.cpp` (`Engine::usd_per_xch()`):
```cpp
    {
        auto it = coingecko_prices_.find("chia");
        if (it != coingecko_prices_.end() && std::isfinite(it->second)
            && it->second > 0.0) {
            return it->second;
        }
    }
```
**The Failure Mode:**
1. At startup, CoinGecko fetches prices successfully once. `coingecko_prices_["chia"]` is populated and `coingecko_last_fetch_` is stamped.
2. If the CoinGecko API subsequently experiences an extended outage, rate-limiting (HTTP 429), or network disconnection, `coingecko_prices_` is **not cleared** (as intended for transient resilience).
3. However, `usd_per_xch()` checks only `coingecko_prices_.find("chia")` **without checking feed freshness or elapsed time**.
4. Consequently, a price from hours or days ago will continue to be returned indefinitely as "live".
5. The function **never falls through to the on-chain DEX fallback loop**.
6. Even worse, inside `compute_portfolio_equity_usd()`, `last_asset_live_block_["xch"]` is continuously updated to `current_block` because `pseudo > 0`, which means carry TTL **never expires**, `degraded` remains `false`, and the bot trades with a frozen external price.

**Required Fix:**
Gate the CoinGecko lookup in `usd_per_xch()` on the existing feed-freshness helper `coingecko_feed_fresh_for_revival()` or check `(now - coingecko_last_fetch_) <= config_.market_data.cex_freshness_threshold_sec`. If stale, log a debug message and fall through to the DEX pair loop.

---

#### Issue 3.2.2: CRITICAL — Startup Degradation Leaves Circuit Breakers Inert (`peak == 0.0`)
In `Engine::compute_portfolio_equity_usd()`, held assets that have never been valued set `degraded = true`.
In Step 13 (`Engine::step_run_hedging`):
```cpp
const auto authority = valuation_authority_.step(valuation_degraded_);
const bool valuation_authoritative = authority.may_update_peak;

if (valuation_authoritative) {
    peak_equity_hwm_usd_ = std::max(peak_equity_hwm_usd_, equity_usd);
}
```
**The Failure Mode:**
1. If an unvalued asset exists in the wallet at process startup, `valuation_degraded_` is `true` from cycle 0.
2. `authority.may_update_peak` is `false`.
3. `peak_equity_hwm_usd_` remains at its default uninitialized value of `0.0`.
4. After `startup_grace_blocks` elapses, `equity_drawdown_frac(peak_equity_hwm_usd_, equity_usd)` receives `peak = 0.0` and continuously returns `0.0`.
5. **The circuit breakers are completely inert.** If the portfolio experiences a catastrophic drop, neither drawdown breaker can ever trip because no peak was ever recorded.
6. Freezing the HWM is safe *only when a valid positive HWM already exists*. When `peak_equity_hwm_usd_ == 0.0`, degradation prevents initial calibration.

**Required Fix:**
When `peak_equity_hwm_usd_ <= 0.0` and the startup grace period has expired while valuation remains degraded:
- The engine must flag an alarm and transition to a fail-closed / paused state (or block new offer creation), rather than silently trading without circuit breaker protection.

---

### 3.3 Specific Code Change Suggestions for PR #117

#### Change 3.3.1: Freshness-gated CoinGecko lookup in `Engine::usd_per_xch()`

```diff
--- a/cpp/src/engine.cpp
+++ b/cpp/src/engine.cpp
@@ -11322,9 +11322,17 @@ double Engine::usd_per_xch() const
     // The note in the loop below is right that a CoinGecko outage must not
     // zero every USD figure in the bot, so this prefers the external feed
     // and falls back rather than failing.
-    {
+    const bool cg_fresh = coingecko_feed_fresh_for_revival(
+        !coingecko_prices_.empty(),
+        coingecko_last_fetch_,
+        std::chrono::steady_clock::now(),
+        config_.market_data.cex_freshness_threshold_sec);
+
+    if (cg_fresh) {
         auto it = coingecko_prices_.find("chia");
         if (it != coingecko_prices_.end() && std::isfinite(it->second)
             && it->second > 0.0) {
             return it->second;
         }
+    } else if (!coingecko_prices_.empty()) {
+        spdlog::debug("[Engine] usd_per_xch: CoinGecko feed stale -- falling back to DEX mid");
     }
```

#### Change 3.3.2: Guard Uninitialized Peak Breaker Inactivity in Step 13

```diff
--- a/cpp/src/engine.cpp
+++ b/cpp/src/engine.cpp
@@ -13670,6 +13670,16 @@ void Engine::step_run_hedging(BlockHeight block_height)
     if (valuation_authoritative) {
         peak_equity_hwm_usd_ = std::max(peak_equity_hwm_usd_, equity_usd);
     }
+
+    // [S27 Guard] If startup grace expired but peak was never initialized due to
+    // continuous valuation degradation, the breakers are inert. Fail closed.
+    if (peak_equity_hwm_usd_ <= 0.0 && equity_usd > 0.0 && valuation_authoritative) {
+        peak_equity_hwm_usd_ = equity_usd;
+    } else if (peak_equity_hwm_usd_ <= 0.0 && block_height > startup_grace_height_) {
+        spdlog::warn("[Engine] Step 13: [S27] Peak equity HWM uninitialized ($0.0) "
+                     "due to degraded valuation past startup grace -- circuit breakers are INERT");
+    }
```

---

## 4. Recommended PR Progression & Merge Order

1. **Step 1: Fix & Merge PR #115 (`feat/peg-registry`)**
   - Apply unused variable cleanup (`quote` removal in `engine.cpp`).
   - Add `enforce` and `isfinite` checks to `deviation_pct`.
   - Add duplicate `asset_id` parser check.
   - Merge PR #115 into `main`.

2. **Step 2: Update & Merge PR #117 (`fix/s27-equity-blindness`)**
   - Retarget PR #117 base to `main` (after #115 merges).
   - Apply CoinGecko freshness gate in `usd_per_xch()` to ensure fallback to DEX mid works when the external API fails.
   - Add uninitialized HWM safety logging / gating.
   - Run full test suite (`xop_tests`) and merge into `main`.

3. **Step 3: Merge PR #116 (`docs/permuto-research`)**
   - Polish API route auth descriptions and readiness/liveness labels.
   - Merge PR #116 into `main`.

---
*Review generated and documented for the XOPTrader engineering team.*
