# Code Review: Open Pull Requests #115, #116, #117

**Review Date:** 2026-08-27
**Reviewer:** GitHub Copilot (Kimi K3)
**Repository:** `Bella-Addormentata/XOPTrader`
**Method:** Static review. PR metadata and full file patches pulled from the
GitHub REST API; all three head branches (`feat/peg-registry`,
`fix/s27-equity-blindness`, `docs/permuto-research`) fetched locally and the
diffs traced against the surrounding code at those revisions. **No build or
test run was performed** — CI should be green before merge.

A prior same-day review exists
(`CODEREVIEW-20260827-GitHub-Copilot-Gemini3.7Flash-Open-PRs-Review.md`).
This review was conducted independently against the code; where it reaches
the same conclusion that is noted as *corroborated*, and two of its claims
are *corrected* below (§1.3, §3.2).

---

## PR inventory

| PR | Branch -> base | Scope | Size |
|----|----------------|-------|------|
| #115 | `feat/peg-registry` -> `main` | Peg identity becomes an asset property (`PegRegistry`, config `pegged_assets`, engine helpers, tests) | +989/-26, 10 files |
| #116 | `docs/permuto-research` -> `main` | Permuto competition research + SVPerp notes (docs only) | +1045/-0, 3 files |
| #117 | `fix/s27-equity-blindness` -> `feat/peg-registry` (stacked on #115) | S27: CoinGecko-first `usd_per_xch`; never-valued assets degrade the cycle; 3 pure-maths tests | +112/-10, 2 files |

**Merge order matters.** #117 is stacked on #115 (base will retarget to
`main`). Under the *current operator config* (XCH/wUSDC.b disabled, wUSDC.b
`enforce: false`), #115 alone leaves `usd_per_xch()` returning `0.0` — no
enabled XCH/<enforced par wrapper> pair exists, and only #117's
external-feed preference restores the anchor. Merging #115 without #117
re-creates part of the S27 hole for DBX-denominated valuation
(`quote_usd_factor(XCH/DBX) = usd_per_xch()/quote_per_xch = 0`). Land them
together. #116 is independent.

**Test-count check.** `main` carries 752 gtest macros in `cpp/tests`;
`feat/peg-registry` carries 777 (+25: 16 registry + 9 parser, consistent
with the diff); `fix/s27-equity-blindness` carries 780 (+3). #117's claimed
"780 tests" checks out exactly. #115's body says "768" — the measured count
is 777, so that number is stale by the 9 parser tests (presumably counted
before they landed). Cosmetic, but worth fixing in the PR body since the
author holds themselves to precise claims.

---

## 1. PR #117 — S27: equity computed to $0 and both breakers went inert

Reviewed first: it is the safety-critical one, and two of the findings below
mean **the S27 failure shape is not fully closed by this PR**.

### 1.1 [HIGH] The max-drawdown comparison is still unreachable at equity == $0

`cpp/src/engine.cpp`, Step 13 (~line 13715 on the branch):

```cpp
if (equity_usd > 0.0 && drawdown_grace_remaining_ == 0) {
    const double drawdown_frac = risk::equity_drawdown_frac(
        peak_equity_hwm_usd_, equity_usd);
    ...
```

The comparison is gated on `equity_usd > 0.0`. In a *total* pricing failure
(CoinGecko never up this run + no enforced wrapper pair + junk books — the
exact 2026-08-25 shape), equity computes to exactly `$0`, this guard skips
the evaluation, and the breaker never trips. The PR's own new test pins
`equity_drawdown_frac(500.0, 0.0) == 1.0` ("a real peak still catches a
collapse to zero") — but the engine never calls the function with equity
`0.0`, so that property is unreachable in production. The test pins the
maths; the call site defeats it.

Is the change safe against false trips? Yes, for a structural reason:
`portfolio_equity_usd` carries last-known prices, so mid-run equity can only
reach `$0` when **every currently-held asset has never been valued this
process run** (any prior valuation leaves a carry entry). With a real peak
already established, that state is "everything held is unpriced" — pausing
is the safe direction, not a nuisance trip.

**Suggested change:**

```diff
--- a/cpp/src/engine.cpp
+++ b/cpp/src/engine.cpp
@@
-    if (equity_usd > 0.0 && drawdown_grace_remaining_ == 0) {
+    // [S27 follow-up] Gate on the PEAK, not on current equity.  A collapse
+    // to $0 against an established peak is a 100% drawdown and must trip;
+    // equity_drawdown_frac already returns 0.0 for a non-positive peak, so
+    // warm-up behaviour is unchanged.  Mid-run equity can only reach $0
+    // when no held asset has ever been valued this run (a prior valuation
+    // leaves a carry entry), which is exactly the state that must not
+    // trade unprotected.
+    if (peak_equity_hwm_usd_ > 0.0 && drawdown_grace_remaining_ == 0) {
         const double drawdown_frac = risk::equity_drawdown_frac(
             peak_equity_hwm_usd_, equity_usd);
```

### 1.2 [HIGH] Degraded-from-start means the peak never initializes — breakers inert with non-zero equity

*Corroborates Gemini 3.2.2, with the mechanism pinned.*

`peak_equity_hwm_usd_` initialises to `0.0`
(`cpp/include/xop/engine.hpp:1034`). `ValuationAuthorityGate::step` starts
fully armed (`clean_streak_{kRearmCleanCycles}`,
`cpp/include/xop/risk/valuation_authority.hpp:88`), but a **single** degraded
cycle sets `may_update_peak = false` and re-arming needs 10 consecutive
clean cycles. PR #117 makes never-valued assets degrade *unconditionally*,
so if any held asset is unpriced from cycle 0, the peak stays `0.0`
indefinitely and `equity_drawdown_frac(0.0, equity)` returns `0.0` forever —
**even while equity is non-zero** (e.g. XCH and BYC priced, wUSDC.b not).
That is the S27 "breaker goes inert" outcome persisting in a different
corner: not "equity $0" but "peak $0".

This is not hypothetical for the next resume: the operator's live
`config.yaml` sets `enforce: false` on wUSDC.b while ~79 units remain in the
wallet (TODO S30). Under #115+#117 that asset can never be valued, every
cycle is degraded from process start, the re-arm streak can never reach 10,
and the peak never seeds. The engine would resume with the drawdown breaker
armed against a **$0 peak** — inert — which is precisely what S27 was filed
about.

**Suggested changes (together):**

1. Fail closed when no peak exists after the grace window. At Step 13,
   after the grace decrement:

```diff
+    // [S27 follow-up] No peak after the grace window means the breaker
+    // cannot protect this run at all (frac is 0.0 on a non-positive peak
+    // BY DESIGN).  With a non-empty book that is a fail-closed condition,
+    // not a warm-up nicety: pause and alert rather than trade unprotected.
+    if (drawdown_grace_remaining_ == 0 && peak_equity_hwm_usd_ <= 0.0
+        && equity_usd > 0.0 && state_->status() != BotStatus::Paused) {
+        spdlog::error("[Engine] Step 13: [S27] grace expired with equity "
+                      "${:.2f} but NO peak ever established (valuation "
+                      "degraded from cycle 0) -- drawdown breaker is INERT; "
+                      "pausing fail-closed", equity_usd);
+        state_->set_status(BotStatus::Paused);
+        alerts_->send_alert(AlertRule::CircuitBreaker,
+            "No equity peak established after startup grace (persistent "
+            "valuation degradation) -- engine PAUSED fail-closed.  Check "
+            "for held assets with no pricing path (e.g. enforce:false on a "
+            "held pegged asset).");
+    }
```

   (`equity_usd > 0.0` here distinguishes "book is genuinely empty" from
   "book exists but cannot seed a peak".)

2. Give the operator a way out of permanent degradation that isn't "sell
   the dust": either a per-asset `exclude_from_equity: true` declaration in
   `pegged_assets` (explicit write-off of a residual the operator has
   accepted as worthless), or a documented operational note that
   `enforce: false` on a **held** asset freezes the drawdown peak until the
   holding is gone. The first is a small registry addition
   (`find`/`is_pegged` unchanged; `compute_portfolio_equity_usd` skips
   flagged assets entirely). Flagging rather than prescribing — this is a
   policy decision.

### 1.3 [MEDIUM] Stale CoinGecko price is served indefinitely as the primary anchor

*Corroborates Gemini 3.2.1; the freshness helper it references exists and is
verified below.*

The new block in `Engine::usd_per_xch()` returns the cached
`coingecko_prices_["chia"]` whenever it is finite and positive, with no
staleness bound. On a fetch failure the catch in `step_update_market_state`
(`cpp/src/engine.cpp:2005`) only logs; the map keeps its last values
(deliberate, for transient resilience). But under the new ordering that
means a price from hours ago is returned as "live" **forever**, the DEX
fallback loop is never reached, and because the pseudo-price stays positive,
`last_asset_live_block_["xch"]` keeps refreshing — so even the S20 carry TTL
never notices. The asset looks freshly priced while the anchor is frozen.

The repo already has the exact tool for this: `coingecko_feed_fresh_for_revival`
(`cpp/include/xop/config.hpp:212`), already used for the revive path with
`coingecko_last_fetch_` and `config_.market_data.cex_freshness_threshold_sec`
(`cpp/src/engine.cpp:4889`).

**Suggested change:**

```diff
--- a/cpp/src/engine.cpp
+++ b/cpp/src/engine.cpp
@@
     // and falls back rather than failing.
-    {
+    //
+    // [S27 review] ...but only while the feed is FRESH.  The cache is
+    // kept across failed fetches by design; without an age check a
+    // hours-old price is served as live indefinitely, the DEX fallback is
+    // never reached, and the S20 carry TTL never fires because the pseudo
+    // price stays positive.  Same freshness derivation the revive path
+    // already uses (config.hpp:212).
+    const bool cg_fresh = coingecko_feed_fresh_for_revival(
+        !coingecko_prices_.empty(),
+        coingecko_last_fetch_,
+        std::chrono::steady_clock::now(),
+        config_.market_data.cex_freshness_threshold_sec);
+    if (cg_fresh) {
         auto it = coingecko_prices_.find("chia");
         if (it != coingecko_prices_.end() && std::isfinite(it->second)
             && it->second > 0.0) {
             return it->second;
         }
     }
+    // Stale or absent external feed: fall through to the DEX-derived mid.
```

One config subtlety to decide: `coingecko_feed_fresh_for_revival`
conservatively reads `threshold_sec <= 0` ("CEX freshness decay disabled")
as **stale**. An operator running with the freshness decay disabled would
silently lose the CoinGecko anchor under this change and always take the DEX
fallback. If that combination is meant to stay supported, give the anchor
its own threshold (e.g. `market_data.xch_anchor_max_age_sec`) rather than
reusing the revive semantics.

### 1.4 [LOW] The never-valued degradation logs at `debug` — the incident signature stays invisible

The new branch in `compute_portfolio_equity_usd` emits
`spdlog::debug("[Engine] [S20] held asset {} has NEVER been valued...")`.
S27 happened precisely because this condition produced nothing anyone could
see. The existing expired-carry branch has the same problem. Recommend a
rate-limited `warn` (e.g. once per asset per N blocks, or routed through the
breaker re-alert gate idiom) — first occurrence per asset at minimum. The
Step 13 "equity valuation DEGRADED" warn does fire once per degraded episode,
which partly covers this; per-asset attribution at warn level would still
have saved hours on 2026-08-25.

### 1.5 Verified correct

- `coingecko_prices_.find("chia")` matches the key used at all four existing
  call sites (`engine.cpp:2282`, `:2362`, `:10978`, `:13178`).
- New tests type-check against `risk/drawdown_breaker.hpp`
  (`equity_drawdown_frac(peak, equity)`, `AssetValuationInput`,
  `portfolio_equity_usd` all present at the used signatures).
- The moved degrade check preserves the unsigned-height underflow guard and
  the `ttl == 0` semantics for the carry branch.
- Test count claim (780) verified exactly.

---

## 2. PR #115 — Retire the hardcoded USD par: peg identity becomes an asset property

### 2.1 [MEDIUM] The BYC market-cross branch of `quote_usd_factor` ignores the cross wrapper's declared par

`cpp/src/engine.cpp`, `Engine::quote_usd_factor`, cross-preferring branch:

```cpp
            if (snap.mid_price > 0
                && snap.spread_bps > 0.0
                && snap.spread_bps <= kMaxCrossSpreadBps) {
                return static_cast<double>(snap.mid_price)
                     / static_cast<double>(kMojosPerXch);
            }
```

The mid here is denominated in the **cross wrapper asset** (e.g. wUSDC.b),
so the returned value is in wrapper units per BYC — and it is returned
directly as USD without multiplying by the wrapper's declared par. Commit
`5641bd2` in this same PR fixed exactly this bug class in `usd_per_xch()`
("apply the declared par in usd_per_xch; fail loud; guard non-finite") — the
BYC cross path was missed. Latent today (the only configured wrapper is USD,
target 1.0, so `* 1.0` is a no-op), but it is precisely the
silent-wrong-currency failure the registry exists to eliminate, and it
reactivates the moment anyone declares a non-USD or non-unit wrapper.

`byc_cross_source_pair` must be updated in lockstep: its own comment
requires the eligibility test to match `quote_usd_factor`'s BYC branch
*exactly* (so `quote_usd_factor_is_par` and the factor cannot disagree about
which snapshot supplies the value). If the factor skips a cross whose par is
unavailable but the source-pair helper still names it, the two drift apart —
the same inconsistency class S20 was burned by.

**Suggested change (both functions):**

```diff
--- a/cpp/src/engine.cpp
+++ b/cpp/src/engine.cpp
@@
         auto snap = state_->get_market(other.name);
         if (snap.mid_price > 0
             && snap.spread_bps > 0.0
             && snap.spread_bps <= kMaxCrossSpreadBps) {
+            // The mid is denominated in the CROSS wrapper; convert through
+            // its declared par exactly as usd_per_xch() does.  nullopt
+            // (non-USD peg with no FX rate) means this cross cannot yield
+            // a USD factor -- skip it rather than report wrapper units as
+            // dollars.  Keep byc_cross_source_pair() in lockstep.
+            const auto cross_par = declared_usd_par(other.quote_asset_id);
+            if (!cross_par) {
+                continue;
+            }
             return static_cast<double>(snap.mid_price)
-                 / static_cast<double>(kMojosPerXch);
+                 / static_cast<double>(kMojosPerXch) * *cross_par;
         }
```

and the identical guard inside the `byc_cross_source_pair` eligibility test.

### 2.2 [MEDIUM] `enforce: false` does not mean "no valuation" for an XCH-base pair — semantics overclaimed

The PR body and config comments say an unenforced asset "yields no valuation
at all" / "stops the asset valuing anything". That is true for the two
registry-gated branches, but trace `quote_usd_factor(XCH/BYC)` with BYC set
to `enforce: false`:

1. `is_par_wrapper_quote(pc)` — false (requires `enforce`).
2. `quote_prefers_market_cross(pc)` — false (requires `enforce`).
3. `pc.quote_asset_id == "xch"` — no.
4. `pc.base_asset_id == "xch"` — **yes** → derives the factor from the
   pair's *own mid*: `usd_per_xch() / quote_per_xch` (grade-gated via
   `quote_usd_factor_trusted`/`quote_usd_factor_source_pair`).

So flipping `enforce: false` on BYC does not stop BYC being valued — it
reroutes BYC valuation to the XCH/BYC book's own mid. That may well be the
*right* answer for a market-determined CDP stablecoin (arguably better than
par), but it is not what the PR text says the switch does, and an operator
reaching for it during an issuer failure would be surprised. Either document
the rerouting ("`enforce: false` retires the declared par; an asset with a
live XCH cross is still valued by the market") or make the switch absolute
and return `0.0` for declared-but-unenforced assets before the derived
branch. The former is one paragraph in `config.example.yaml`; the latter is
a behaviour choice worth an explicit decision.

### 2.3 [LOW] `PegRegistry::deviation_pct` is inconsistent with `classify`

*Corroborates Gemini 1.2.2.*

`classify` guards `!a->enforce` and `std::isfinite(*observed)`;
`deviation_pct` checks neither. `+inf` passes `*observed > 0.0` and returns
`+inf`; an unenforced asset still gets a deviation. Today `deviation_pct`
has no production caller, so this is latent — fix it now, before the S30
monitor wires in both and they disagree.

```diff
--- a/cpp/include/xop/peg_registry.hpp
+++ b/cpp/include/xop/peg_registry.hpp
@@
     [[nodiscard]] std::optional<double> deviation_pct(
         const std::string& asset_id,
         std::optional<double> observed) const
     {
         const auto* a = find(asset_id);
-        if (a == nullptr || !observed.has_value() || !(*observed > 0.0)) {
+        // Same admission contract as classify(): unenforced is NotPegged,
+        // and a non-finite observation is no observation.
+        if (a == nullptr || !a->enforce || !observed.has_value()
+            || !std::isfinite(*observed) || !(*observed > 0.0)) {
             return std::nullopt;
         }
         return (*observed - a->peg_target) / a->peg_target * 100.0;
```

### 2.4 [LOW] Duplicate `asset_id` declarations silently overwrite

*Corroborates Gemini 1.2.3.*

`PegRegistry::add` does `by_asset_id_[key] = std::move(asset)` — a second
YAML entry with the same `asset_id` silently replaces the first. For a
parser whose stated contract is fail-loud, a duplicated identity should
throw:

```diff
--- a/cpp/src/config.cpp
+++ b/cpp/src/config.cpp
@@
         if (item["prefer_market_cross"]) {
             a.prefer_market_cross = item["prefer_market_cross"].as<bool>();
         }
+        if (reg.find(a.asset_id) != nullptr) {
+            throw ConfigError(
+                "pegged_assets: duplicate asset_id '" + a.asset_id
+                + "' (symbol '" + a.symbol
+                + "') -- one declaration per asset; merge or delete one");
+        }
         if (!reg.add(std::move(a))) {
```

Related, smaller: the `PegRegistry(std::vector<PeggedAsset>)` constructor
discards incoherent entries silently (`add`'s `false` return is ignored).
Only tests use it today; either assert in debug builds or document the
drop.

### 2.5 [LOW] Dead `slash`/`quote` locals after the refactor — cleanup, **not** a build break

*Corrects Gemini 1.2.1's severity claim.*

`quote_usd_factor` and `quote_usd_factor_is_par` still compute
`slash`/`quote` from `pc.name` but no longer reference `quote`; the
`find('/')` guard in `usd_per_xch` is now vestigial too (lookup is by asset
id). This is dead code worth deleting — but contrary to the earlier review
it will **not** fail the `-Werror`/`/WX` build: `quote` is a
`std::string` (non-trivial type; GCC/Clang/MSVC all suppress unused-variable
warnings for it), and `slash` is still used to compute it. Cleanup:

```diff
 bool Engine::quote_usd_factor_is_par(const PairConfig& pc) const
 {
-    const auto slash = pc.name.find('/');
-    const std::string quote = (slash == std::string::npos)
-        ? std::string{}
-        : pc.name.substr(slash + 1);
-
     if (is_par_wrapper_quote(pc)) {
```
(same removal at the top of `quote_usd_factor`; drop the `find('/')` guard
in `usd_per_xch`).

### 2.6 [LOW] `config.example.yaml` declares only two of the four assets the PR body names

The body claims "config declares wUSDC.b/wUSDC/USDS as USD par wrappers …
The shipped `config.example.yaml` does exactly that." The example declares
only **wUSDC.b** and **BYC**. No live pair quotes wUSDC or USDS today, so
nothing breaks — but an operator re-adding a legacy `XCH/USDS` or
`XCH/wUSDC` pair gets *no par valuation* (factor `0.0`) rather than the old
implicit `$1`. Either add the other two declarations to the example
(commented, with a note) or soften the body sentence. Also worth one line in
the example that the GUI's `pnl_usdc_expr`
(`gui/services/database_service.py:66`) still hardcodes `$1.00` for
wUSDC.b/wUSDC — after any `enforce: false`, engine and GUI accounting
visibly disagree. Follow-up item, not a blocker.

### 2.7 Verified correct

- `is_par_wrapper_quote` / `quote_prefers_market_cross` are applied
  consistently across `usd_per_xch`, `byc_cross_source_pair`,
  `quote_usd_factor_source_pair`, `quote_usd_factor_is_par` and
  `quote_usd_factor`; the first-match-in-config-order selection is identical
  in the cross helper and the factor, preserving the S20 provenance
  invariant.
- `parse_pegged_assets` fails loud on a non-sequence section and on
  incoherent entries; absent section is legal and empty. The 9 new parser
  tests cover exactly these edges, including `.inf` and the
  mapping-instead-of-sequence typo.
- The deliberately-untouched sites check out: `arbitrage.cpp:916` and
  `market_allocator.cpp:188` are membership/log-label tests, not price
  references; `feed_listings.hpp:47` is a CoinGecko-id mapping;
  `pnl.cpp:1512`'s BYC inclusion is documented as deferred (the `1000.0`
  CAT scale deserves its own change).
- "Zero symbol comparisons and no bare `return 1.0` in the USD-factor path
  of engine.cpp" — confirmed against the branch.

---

## 3. PR #116 — Permuto competition research and SVPerp notes (docs only)

No code risk. The documents are unusually honest about their own corrections
(the four-row corrections table in `TODO-COMPETITION.md`), which is the right
practice and worth keeping.

### 3.1 [LOW] Broken blockquote in `TODO-COMPETITION.md` (verified on branch)

The "Prizes" paragraph of the `> ## THE CLOCK` blockquote loses its `>`
prefix mid-sentence:

```
> entrant may win in both — $45,000 total pool. **This document has been wrong
 twice; corrections
are recorded rather than quietly edited out, because the errors are
instructive.**
```

Lines 2–4 have no `>`, so the correction note renders outside the quote
block and the `**…**` bold spans a block boundary. Fix:

```diff
 > **Prizes** (each category, paid in XCH): 1st **$15,000**, 2nd **$5,000**,
 > 3rd **$2,500**. Traders and Market Makers are separate categories and one
-> entrant may win in both — $45,000 total pool. **This document has been wrong
- twice; corrections
-are recorded rather than quietly edited out, because the errors are
-instructive.**
+> entrant may win in both — $45,000 total pool. **This document has been
+> wrong twice; corrections are recorded rather than quietly edited out,
+> because the errors are instructive.**
```

### 3.2 Corrections to the earlier review (Gemini 3.7 Flash), so neither is propagated

- Its Issue 2.2.1 ("authentication scope") is **mistaken**:
  `docs/permuto-api-reference.md` §1 already states "No authentication.
  Verified working unauthenticated" for `/info/*`; the "All require a
  session token" line is scoped to §2 (`/exchange/*`). No doc change needed.
- Its Issue 2.2.4 is **backwards**: S31 *is* added to `TODO.md` by PR #115's
  own diff, not only on `feat/s31-dead-mans-switch`. The real (minor)
  ordering note is the reverse: if #116 merges **before** #115,
  `TODO-COMPETITION.md`'s "Filed as S31 in `TODO.md`" reference dangles on
  `main` until #115 lands. Merge #115 first, or drop the pointer.
- Its terminology note (2.2.3) is half-right: `permuto-api-reference.md`
  calls order prices "decimal annualized IV" while §0 establishes the
  *oracle* is a realized-vol estimate. Both can be true (the traded mark is
  an implied-forward-vol number; the oracle prints realized), but one
  clarifying sentence at the "Prices are decimal annualized IV" line would
  stop a future reader conflating them.

### 3.3 Spot-checks passed

- Contest dates: 31 Aug 2026 is a Monday; 4 Sep 2026 is a Friday. The
  "under 5 days" urgency as of authorship (2026-08-26) is arithmetically
  consistent.
- Leaderboard table: 252,782,915 / 300,000,000 = 84.3% as stated.
- The S31 design note in `TODO.md` (from #115) and the `schedule_cancel`
  section of the API reference agree on the policy numbers
  (`min_delay_ms` 5000, 10 fresh arms/day, extend-don't-rearm).

---

## 4. Consolidated action list, in priority order

| # | PR | Severity | Action |
|---|----|----------|--------|
| 1 | #117 | HIGH | Re-gate the max-drawdown comparison on `peak_equity_hwm_usd_ > 0.0` (§1.1) |
| 2 | #117 | HIGH | Fail closed when no peak exists after grace with a non-empty book; give operators an `exclude_from_equity`-style escape for permanently unpriceable residuals (§1.2) — **blocks resume with the current live config** |
| 3 | #117 | MEDIUM | Freshness-gate the CoinGecko anchor via `coingecko_feed_fresh_for_revival`; decide the `threshold_sec <= 0` semantics (§1.3) |
| 4 | #115 | MEDIUM | Multiply the BYC cross by the wrapper's declared par, in `quote_usd_factor` **and** `byc_cross_source_pair` (§2.1) |
| 5 | #115 | MEDIUM | Document or change the `enforce: false` rerouting into the derived path (§2.2) |
| 6 | #115 | LOW | `deviation_pct` guard parity with `classify` (§2.3); duplicate `asset_id` throws (§2.4) |
| 7 | #115 | LOW | Delete dead `slash`/`quote` locals (§2.5); fix the "768" count and the example-config claim in the PR body (§intro, §2.6) |
| 8 | #117 | LOW | Warn-level, rate-limited log for never-valued held assets (§1.4) |
| 9 | #116 | LOW | Fix the broken blockquote (§3.1) |
| — | both | — | Merge order: #115, then #117 (retargets to `main`), #116 any time after #115 (§intro) |

## 5. What was not done

- No build or test execution (static review only). Given `-Wall -Wextra
  -Wpedantic -Werror` / `/W4 /WX` in `cpp/CMakeLists.txt`, run the full
  suite on the merged result before release; the new code paths in
  `usd_per_xch`/`quote_usd_factor` are, by the author's own T4-07 note, not
  reachable from current fixtures.
- The S30 follow-up (asset-level depeg monitoring independent of
  `pair.enabled`, i.e. production callers for `PegRegistry::classify`)
  remains open by the author's explicit scoping — confirmed: `classify` has
  zero production callers on the branch. That gap should stay tracked as the
  reason not to consider the incident closed after these merge.
