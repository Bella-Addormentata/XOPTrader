# XOPTrader Master TODO List

**Created:** 2026-03-24
**Last audited: 2026-09-02 -- two independent audits of 2026-09-01, which do not overlap.**

**Dexie side (PR #134, v0.10.13), findings S33-S44.** Started as "can dexie trade history price BYC" and became an audit of Step 8's reference frames, then of the taker paths. S33/S34 shipped as MEASUREMENT rather than fixes -- the defect was real, the observed cost zero, and the pair exhibiting it disabled. S40 was the opposite: an inert size cap on three taker paths that had already halved four ledger entries. The structural item is S36 -- nothing in `cpp/tests` constructs an `Engine`, which is how regressions in this family survived four review rounds and a thousand green tests.

**Last audited: 2026-09-01 (PRs #132/#133/#135)** -- the Permuto contest
session. The account was LIQUIDATED to zero equity that morning, so depth
ended at 4,093.892 against a 300,000,000 gate and no code change can
restart it without re-funding. Findings **P1-P5** at the end of this file
are what has to be true before the second account runs: P1 and part of P2
ship in #135, while P3, P4 and P5 are open and each needs its own PR.
(#134 audits the same day from the dexie side and numbers its findings
S38/S39; the two series do not overlap.)

**Previously audited: 2026-08-29 (v0.10.5)** -- post-release operator-experience round: config hot-reload (a Settings save now applies pair DISABLES to the running engine live, cancelling their resting offers; enables and every other field are honestly reported as restart-required -- the 2026-08-25 "disable landed on disk but the engine kept quoting" hole), and allocation zeroing (a disabled pair's asset is applied as a 0% target while hidden from Target Portfolio Allocation; its saved percent returns when the pair does).

**Previously audited: 2026-08-29 (v0.10.0)** -- the seven-PR review cycle merged:
#115 (peg registry, S29/S30), #117 (S27+S32), #120 (S28), #121 (S31), #118
(emergency consolidation), #116 (Permuto research + probes), and #119 (the
complete Permuto trading stack: identity, auth, quoting, risk, the live
session and the venue switch). Eleven Copilot review rounds plus a
release-review audit ran against these; every finding is fixed or
explicitly declined on the PR. What remains below is what was NOT in that
cycle.

**Previously audited: 2026-08-27** -- S32 added while working the two Copilot reviews of PRs #115/#117: the S27 fix, on its own, would have paused the engine on every start with the live config. S27 and S29 moved to `[~]` (built, unmerged). Most other findings in those reviews were already fixed on-branch; both reviews ran against an older snapshot.
**Previously audited: 2026-08-26 (v0.9.22)** -- S22/S24/S26-S30 added from the 2026-08-25/26 incident sessions: the warp.green and Circuit DAO compromises, the equity-blindness and dead-pause-flag defects found while responding to them, and the peg registry built in reply. S23 and S25 shipped in v0.9.22 (PRs #113, #114).
**Previously audited: 2026-08-23 (v0.9.19)** -- statuses refreshed after the release train #96-#107: the spendable-XCH-zero incident fixes (coin-lock ledger PR #107, ensure_split half-split PR #106), the warp claim fix (PR #105, first live bridge completed 2026-08-23), the eth-account pin (PR #104), and the Warp tab split (PR #102).
**Source:** Consolidated from all code reviews in `docs/CODE REVIEWS/` plus the 2026-08 live-operation sessions.

**Status Key:** `[ ]` = Not started | `[~]` = In progress | `[x]` = Complete

> Items closed before this audit (Tiers 1–3, T5–T7, and all previously-`[x]` entries) are archived — see this file's pre-audit revision in git history (`git show d3fc381:TODO.md`).

---

## SHIPPED in v0.10.0 (2026-08-29) -- the resume gate is cleared

S27/S32 (#117), S28 (#120), S31 (#121) and the peg registry (#115) are on
main. The items that gated a defensible resume are done; S13 and S14 remain
open below and are still marked "touches live order handling: NOT to be
fixed casually" -- they did not gate the resume and were not rushed into it.

v0.10.0 also ships the PERMUTO stack (#119): a second venue behind its own
toolbar switch, disabled until the operator registers. Registration closes
Mon 31 Aug 17:00 ET; the contest runs to Fri 4 Sep 16:00 ET. The operator
sequence (register -> supervised test order -> arm before the Sunday pause)
is in TODO-COMPETITION.md.

## Superseded target scope: v0.9.23 -- make it safe to resume

The engine has been paused since 2026-08-25 and every item here is about the
same failure: **the bot continuing to stand behind quotes it can no longer
manage.** Ordered by whether resuming without it is defensible.

| item | why it gates a resume |
| --- | --- |
| **PR #115** (S29/S30) | peg identity becomes an asset property; `enforce: false` lets an operator switch off a compromised peg without a release. **SHIPPED v0.10.0 (PR #115)** |
| **S27** | with only XCH/DBX enabled, equity is exactly $0 and `equity_drawdown_frac` returns 0.0 on a non-positive peak — **both breakers go inert rather than trip**. Resuming with no drawdown protection is worse than staying paused. **SHIPPED v0.10.0 (PR #117)** |
| **S32** | the S27 fix, on its own, would have made the engine **pause itself on every start** with the live config — wUSDC.b and wmilliETH.b are held with no enabled pair, so every cycle degrades from cycle 0 and the peak never seeds. Found 2026-08-27 while working the Copilot reviews. **Fixed in PR #117 (78b97f0)** |
| **S28** | the engine cannot fall back to the wallet for block height, so it sat dead ~2.5h beside a healthy wallet RPC. (The pause-flag half was overstated; corrected in place — pause *is* applied before Step 8 on recovery) **SHIPPED v0.10.0 (PR #120)** |
| **S31** | nothing cancels the book when the engine stops. Cost $12.71 and a tripped breaker on 08-25, and was demonstrated again on 08-26 when a stray `--dry-run` killed the live process and left 10 offers unmanaged. **SHIPPED v0.10.0 (PR #121)** |
| **S13** | "wallet needs to be fully synced" is **88% of all error lines** (~4,000 of 4,514). A silently failed cancel leaves a stale quote live — the same outcome as S31, by a different route |
| **S14** (absorbs S26) | forced cancel retries with the same fee forever. Watched fail for 6+ hours on two offers this week; worst recorded case 158 warnings over 36h with coins locked throughout |

**Not in scope, deliberately.** S24 (reconciliation dominating the
heartbeat) and S22 (pre-S20 DB junk, plus the S11 orphan backfill) are real
but neither prevents a safe restart. S15 (11,798 spread warnings, 14% of all
warnings, still firing while paused) is cheap noise reduction — include only
if there is room.

**Note on S13 and S14:** both are marked "touches live order handling: NOT
to be fixed casually". They want care, not speed. But both directly caused
exposure this week.

---

## Open items (audited 2026-08-18)

### Correctness

### T8-29: Consolidate trade_log DDL ownership (PnL vs Engine database)
- **Files:** `cpp/src/database.cpp`, `cpp/src/monitoring/pnl.cpp`
- **Issue:** Both classes open the *same* DB file (`config_.database.path`, engine.cpp:196 and :476), but `trade_log` DDL is defined in BOTH `database.cpp:60` and `pnl.cpp:203` and the two definitions have diverged (database.cpp adds `CHECK(side IN ('bid','ask'))`, `fee_mojos DEFAULT 0`, `block_height NOT NULL`; pnl.cpp has none — whichever initializes first wins via `IF NOT EXISTS`).
- **Status:** `[ ]` — Audit 2026-08-18: still open and worse than written; consolidate DDL ownership into one class or make one the sole schema owner.

### Test infrastructure

### T4-06: Integration test framework (backtest as CI test)
- **Files:** `cpp/tests/CMakeLists.txt`, `cpp/src/backtest.cpp`
- **Issue:** No integration test target — only 30 pure-logic unit tests registered (CMakeLists.txt:48-77); no `cpp/tests/fixtures/` dir, no ctest label filtering. `BacktestEngine` exists and remains the natural harness, unused.
- **Status:** `[ ]` — Audit 2026-08-18: no `test_backtest_integration.cpp`; CI itself was red until PR #70, so no test tier had run in CI at all.

### T4-07: Add engine startup/shutdown and fill processing tests
- **[S25 2026-08-24] Now also blocks the terminal-offer handoff test.** PR
  #114 makes `detect_fills()` report externally-cancelled/failed offers so
  the engine can close their `offer_log` rows, but `OfferManager` takes a
  concrete `std::shared_ptr<rpc::ChiaWalletRPC>`, so there is no seam to
  feed a CANCELLED wallet record through. The PR's tests pin the database
  contract the fix relies on (a terminal row is not reopened except by a
  fill); the handoff itself -- wallet record in, `offer_log` row closed out
  -- is unpinned until the wallet interface and fake this item tracks exist.
- **[S20 2026-08-24] Now also blocks an ordering regression test.** PR #112
  round 4 found anchors being injected AFTER the Step 1 ingest loop, so a
  first-cycle pair had no anchor while its offers were filtered -- the
  restart-poisoning path the PR exists to close. Feed-level tests cannot
  catch a reordering inside the heartbeat; a runtime warn-once in
  ingest_competing_offers covers it for now, but a real regression test
  needs the harness this item tracks.
- **Files:** `cpp/tests/` (new), `cpp/src/engine.cpp`
- **Issue:** Engine lifecycle (`Running` → `ShuttingDown` → `Stopped`), shutdown idempotency, and `detect_fills()`/`record_buy`/`record_sell` remain untested; no mock `ChiaFullNodeRPC`/`ChiaWalletRPC`/`DexieClient`/`CoinGeckoClient` and no DI seams exist anywhere in `cpp/tests/`.
- **Status:** `[ ]` — Audit 2026-08-18: gap confirmed (zero `mock` hits in cpp/tests/); made more acute by the phantom-offer-removal bug that lived in exactly this path (fixed cae2bfd, still uncovered by tests).

### T4-08: Add `TierQuote` → wallet offer translation tests
- **Files:** `cpp/src/execution/offer_manager.cpp`, `cpp/include/xop/execution/offer_manager.hpp`
- **Issue:** `build_offer_dict()` (pure `PairConfig` + `TierQuote` → JSON) has no tests; bid/ask sign conventions and CAT `quote_mojos_per_unit=1000` handling unverified.
- **Status:** `[ ]` — Audit 2026-08-18: no `test_offer_translation.cpp`; function remains private (offer_manager.hpp:662) with no friend/test seam. Existing TierQuote tests cover ladder construction only.

### T8-25: Add integration tests for the 13-step heartbeat cycle
- **Files:** `cpp/tests/` (new), `cpp/src/engine.cpp`
- **Issue:** No heartbeat-cycle integration test with mocked RPC endpoints; the heartbeat is still 13 steps (engine.cpp:5, Step 13 alerts at engine.cpp:1783), so the item applies as written.
- **Status:** `[ ]` — Audit 2026-08-18: 592-test C++ suite has no cycle-level test and no RPC mocks. Overlaps T4-07; a shared mock harness serves both.

### T8-27: Add DexieClient/ChiaRPC mock HTTP tests
- **Files:** `cpp/src/rpc/dexie_client.cpp`, `cpp/src/rpc/chia_rpc.cpp`, `cpp/tests/` (new)
- **Issue:** JSON parsing, error handling, retry logic, and rate limiting in both clients are untested (zero test references to either class).
- **Status:** `[ ]` — Audit 2026-08-18: still open; `test_tibetswap.cpp`'s socket-free `parse_pool_json` seam ("No socket is created", line 503) is a ready template the Dexie/ChiaRPC clients never received. Would have caught the ticker inversion (see 2026-08 findings #7).

### T8-28: Add GUI service tests (ConfigService / DatabaseService / MetricsService)
- **Files:** `gui/services/config_service.py`, `gui/services/database_service.py`, `gui/services/metrics_service.py`
- **Issue:** Of the four services named, only `EngineBridge` has any coverage — a single frozen-install path-resolution test (tests/test_installed_paths.py:152-163). The other three have zero tests.
- **Status:** `[ ]` — Audit 2026-08-18: scope narrowed — GUI test infrastructure now exists at scale (547 Python tests covering warp services, widgets, config_split), but these three services remain at zero.

### Enhancements

### #11: Split `fee_mojos` into `fee_create` / `fee_cancel`
- **Files:** `cpp/src/database.cpp`
- **Issue:** `trade_log` and `offer_log` carry a single combined `fee_mojos` column (database.cpp:68, :94; all insert/select paths at :295-345, :520, :604), so posting fees vs cancellation burn are indistinguishable, biasing adverse-selection cost analysis.
- **Status:** `[ ]` — Audit 2026-08-18: zero hits for `fee_create`/`fee_cancel` across cpp/. Additive columns (default 0) as originally specified.

### T4-14: Introduce typed quantity wrappers (BaseMojo / QuoteMojo / PriceMojo)
- **Files:** `cpp/include/xop/types.hpp`
- **Issue:** Unit confusion between mojos, XCH, base, quote, price persists; types.hpp:27 still has only `using Mojo = std::int64_t`.
- **Status:** `[ ]` — Audit 2026-08-18: no strong types exist. Partial mitigation: canonical `quote_mojos_for()` helper (types.hpp:55-64, covered by test_pnl_units.cpp) centralises the conversion formula at runtime — but the fee-reserve unit bug (2026-08 findings #3) shows exactly the class of error compile-time types would catch.

### T4-20: Cross-strategy disagreement detection for inventory direction
- **Files:** `cpp/src/strategy/strategy_portfolio.cpp`
- **Issue:** `blend()` (strategy_portfolio.cpp:268-336) remains a plain weight-normalised average of component bid/ask prices and sizes; when strategies disagree on direction the correct response is to shrink size, not average.
- **Status:** `[ ]` — Audit 2026-08-18: zero matches for disagree/cv_threshold/dispersion in strategy_portfolio.{hpp,cpp}; no CV metric, no `DisagreementEvent`, no size shrink.

### T4-26: Multivariate / cross-asset correlation modeling
- **Files:** `cpp/src/risk/`, `cpp/src/execution/market_data.cpp`
- **Issue:** No return-correlation or covariance modeling anywhere; portfolio VaR absent.
- **Status:** `[ ]` — Audit 2026-08-18: nearest features are the *unpopulated* HedgingManager correlation table (hedging.hpp:58; `set_correlations()` has no caller outside hedging.cpp) and the off-by-default shared-asset cross-pair skew heuristic (engine.cpp:4476-4561, `cross_pair_skew_enabled{false}`) — a ratio heuristic, not statistical correlation.

### T4-27: Calibration registry for all thresholds
- **Files:** `cpp/include/xop/config.hpp`
- **Issue:** Tunable thresholds occupy multiple roles (theory/data/operator) without labeling, source metadata, or audit trail.
- **Status:** `[ ]` — Audit 2026-08-18: no `CalibrationRegistry` exists (repo-wide grep finds only the Prometheus metrics registry); thresholds remain plain `AppConfig` fields.

### T8-19: Deduplicate Monte Carlo simulation in backtest
- **Files:** `cpp/src/backtest.cpp`
- **Issue:** `run_monte_carlo()` (line 814) still inlines its own simplified simulation loop instead of reusing `simulate_range()` (line 523, used by walk-forward and optimizer); bug fixes in one path do not propagate.
- **Status:** `[ ]` — Audit 2026-08-18: the deferral comment survives at ~line 875-880 ("For now, we inline a simplified simulation loop"). Fix: refactor `simulate_range()` to accept a block vector.

---

## New findings from the 2026-08 sessions

### S1: Phantom offer removal destroying confirmed fills
- **Files:** `cpp/src/execution/offer_manager.cpp`
- **Issue:** Reconcile removed offers absent from the paginated scan without verifying status, silently destroying confirmed fills before `detect_fills()` could record them.
- **Status:** `[x]` — FIXED cae2bfd (2026-07-31): reconcile verifies `get_offer` status before removal (offer_manager.cpp:1784-1840 [SETTLE-FIX]) — CONFIRMED offers stay tracked, RPC errors fail safe and retry, only wallet-verified CANCELLED/FAILED are removed; April–July history backfilled (472 maker fills + 579 taker trades) per docs/ERA-REMEDIATION-2026-08.md.

### S2: CI red since 2026-04-08
- **Files:** `.github/workflows/compile-check.yml`
- **Issue:** `-Werror` failures kept every CI run red for four months; pytest had never run in CI at all.
- **Status:** `[x]` — FIXED PR #70 (merged 206d55c): -Werror cleared (0d978e3/68e0692), Ubuntu legs on gcc-13 (compile-check.yml:56-64), new python-tests job actually runs pytest with clvm/chia_rs/eth_account import assertions (compile-check.yml:140-195). Follow-ups: main has no branch protection so jobs are advisory; CI resolves pytest 9 while pyproject.toml pins <9.

### S3: XCH fee reserve subtracted from base-denominated bid pool
- **Files:** `cpp/src/engine.cpp:4590-4603`
- **Issue:** When `quote_asset_id=='xch'`, `reserve_mojos = fee_reserve_xch * kMojosPerXch` (1e12-scale) is subtracted from `avail_capital`, but `avail_capital = pcs.risk_quote.bid_size` is BASE-asset mojos (proven by lines 4681-4696: compared against `bid_cap_base`, divided by `base_mojos_per_unit`). On CAT-base pairs (wmilliETH.b/XCH, 1000 mojos/unit) even a 0.001 XCH reserve zeroes the bid pool every heartbeat, making Avellaneda q_max/gamma/kappa sizing inert; only floor mechanisms quote. The base=='xch' branch (4576-4589) is coincidentally correct.
- **Status:** `[x]` — RESOLVED (PR #81). `engine.cpp` now converts the reserve through `reserve_base_mojos()` before subtracting it from the base-denominated pool, under an explicit `[S3 2026-08-18]` comment; a missing mid converts to 0 so no subtraction happens. Re-verified 2026-08-21.

### S4: Warp unwrap G10 gate budgets the Chia fee once but pays it twice
- **Files:** `gui/services/warp/service.py`
- **Issue:** G10 gate (line 1966) computes `toll_need = net.chia_toll_mojos + p.unwrap_chia_fee_mojos` (fee counted once), but the fee is paid on both the burn `cat_spend` (line 2108) and the toll-coin funding `send_transaction` (line 2224). Neither send site re-checks spendable XCH after the UNWRAP_CHECKS gate; an offer locking coins between gate and send makes the second spend fail.
- **Status:** `[x]` — RESOLVED. `service.py` now gates on `toll_need = net.chia_toll_mojos + 2 * p.unwrap_chia_fee_mojos`, and the sending ticks re-check spendable XCH with named pends. Re-verified 2026-08-21 against the merged code; the entry was stale.

### S5: Published mid blends stale last-trade with no staleness gate
- **Files:** `cpp/src/execution/market_data.cpp:1009-1012, 1188-1189`
- **Issue:** `compute_mid` Case 3 falls back to `ps.dex_last_trade` at 0.70 weight with zero age check; no timestamp is stored for the print at all (market_data.hpp:406) and `dex_updated_at` is re-stamped `now()` every heartbeat (market_data.cpp:380, 1542). A 13-day-old trade dragged wmilliETH.b's mid 8%+ below fair.
- **Status:** `[x]` — FIXED (PR #87). `PairState` carries the print's own change-clock (`last_trade_print` / `last_trade_changed_at`), maintained in `ingest_dexie` by VALUE rather than poll time, and `compute_mid` refuses the fallback past `market_data.dex_last_trade_max_age_sec` (default 1800 s, `<= 0` disables). `check_arbitrage` gates the identical fallback. A print never observed to move is refused: its age is unknown, not zero.

### S6: CoinGecko staleness blindness in the general published-mid path
- **Files:** `cpp/src/engine.cpp:1815-1827, 2069-2100`; `cpp/src/execution/market_data.cpp:405-406`
- **Issue:** A failed CoinGecko fetch is swallowed, leaving the stale cache (`coingecko_last_fetch_` advances only on success), yet every heartbeat re-derives `cex_mid` from that cache and calls `ingest_cex_reference`, which stamps `cex_updated_at = now()` — so the `cex_freshness_threshold_sec` taper (market_data.cpp:1041-1048) always sees age ~0 and can never fire.
- **Status:** `[x]` — FIXED (PR #88). `ingest_cex_reference` now takes an `observed_at` and stores it VERBATIM, mirroring `ingest_amm_mid`, which already solved this exact bug on the AMM leg. The engine stamps `coingecko_last_success_at_` only in the fetch success path and passes it, so a failed fetch no longer refreshes the sample's apparent age: every CEX gate — `compute_mid`'s weight taper, `detect_stale`, and the `cex_age_seconds` heartbeat — now measures the age of the DATA rather than the age of the re-ingest. A default-constructed stamp falls back to `now()`, so "never observed" cannot read downstream as a 56-year-old sample.

### S7: Dexie ticker parse swaps bid and ask
- **Files:** `cpp/src/rpc/dexie_client.cpp:616-638`
- **Issue:** `prices.buy[0]` is labeled best bid (`t.price_buy`) and `prices.sell[0]` best ask when the sides are inverted, producing constant "Crossed book" warnings that market_data.cpp:361-371 rationalizes as "normal on Dexie". Harmless for the published mid only because `ingest_competing_offers` overwrites from the real book (market_data.cpp:1529-1530) — but OFI (engine.cpp:1937-1943) and the offer_manager ticker mid (offer_manager.cpp:2297) still consume the swapped sides.
- **Status:** `[x]` — NOT A DEFECT (closed 2026-08-21). The audit read `parse_ticker_` in isolation and missed the direction-handling layer between it and every consumer: `get_ticker` (`dexie_client.cpp:790-840`) already applies the invert+swap for Case A (`td.price_buy = invert_price(raw_sell); td.price_sell = invert_price(raw_buy)`), added in 04e96e1 "dexie inversion fix" (2026-04-03). `parse_ticker_` is a raw JSON transcriber holding Dexie's native XCH-per-CAT values, which are not yet bid/ask in our convention.
  **Do not apply the swap this entry originally proposed** — it would double-invert and put bid/ask backwards on XCH/wUSDC.b, XCH/BYC and XCH/DBX simultaneously. Follow-up worth doing instead: a regression test for the dexie RPC layer (none exists) and the clarifying comment now in `dexie_client.hpp`.

### S8: wmilliETH.b has no drift target — accumulation brakes unreachable
- **Files:** `config.yaml:168-187`; `cpp/src/engine.cpp:4891-4917`
- **Issue:** `strategy.asset_target_allocations` omits WMILLIETH.B (and `ratio_target_by_pair`/`ratio_band_enter_by_pair` omit wmilliETH.b/XCH) even though the pair is enabled; `acquire_scale` returns 1.0 for missing keys (engine.cpp:4891-4893) and is applied to the bid side (:4917), so accumulation is never tapered. Remaining limits are portfolio-fraction denominated (`single_cat_cap_pct` 0.25, soft/hard 0.6/0.8) — unreachable for a small CAT position on a large wallet.
- **Status:** `[x]` — RESOLVED. `config.yaml` now carries `asset_target_allocations.WMILLIETH.B` plus `ratio_target_by_pair` and `ratio_band_enter_by_pair` entries for `wmilliETH.b/XCH`, so `acquire_scale` no longer returns 1.0 for the pair and the accumulation brake is reachable. Re-verified 2026-08-21.

### S9: The last-trade fallback path carries no published-mid band
- **Files:** `cpp/src/execution/market_data.cpp` (Case 3, and the clamp below it)
- **Issue:** The published-mid band clamps only when `dex_best_bid` AND `dex_best_ask` are both present, but Case 3 is reached precisely when they are not. **Neither** Case 3 outcome is banded: accepting a fresh print leaves the unclamped DEX/CEX/AMM blend, and refusing one leaves the centre to CEX/AMM alone. `PublishedMidBandTest.OneSidedBook_NoClamp` asserts exactly this. Surfaced reviewing PR #89, where a comment of mine wrongly claimed the band covered this path.
- **Status:** `[ ]` — OPEN. Decide whether the fallback path should carry a band of its own (around the refused print, or around the AMM leg) or whether the sizing-layer brakes are the right control. Not urgent, with one dependency worth recording: the CEX and AMM legs have to pass their own freshness tapers to be used at all, and if none survives the pair publishes no mid and stops quoting — but the CEX half of that only holds since **S6** (PR #88). Before S6 a frozen CoinGecko cache re-stamped `cex_updated_at` every heartbeat, so the CEX taper could not reject a stale feed and this path had even less protection than it appears. Any decision here should not re-introduce that assumption without checking it.

### S10: The wallet balances page ignores the UI font-size setting
- **Files:** `gui/widgets/wallet_balances.py` (local QSS on both tables)
- **Issue:** The balances and target-allocation tables pin `font-size: 12px` and `11px` for headers in widget-level stylesheets, which override the application stylesheet built by `theme.get_stylesheet(font_size_delta=...)`. Measured 2026-08-21: row height and total table height are byte-identical at deltas -2, 0 and +4. An operator who enlarges the UI font gets no change on this page. Surfaced reviewing PR #92, where a parameterised test appeared to cover font scaling but exercised the same geometry three times.
- **Status:** `[x]` — FIXED (PR #97). `apply_theme()` records the active delta and exposes `theme.scaled_px()`; the wallet page's table/header QSS and row heights derive from it, so the setting reaches the page (verified: delta 4 grows rows 30→34 and the fitted height follows). Widgets built before a theme change keep their old sizes until recreated, like any local QSS.

### S11: offer_log keeps never-resolved 'pending' rows
- **Files:** `cpp/src/execution/offer_manager.cpp` (reconciliation), `gui/services/database_service.py` (consumers)
- **Issue:** Rows stay `status='pending'` indefinitely when an offer ends without the resolution being written back. Measured 2026-08-21: 23 of 57 pending rows are over a day old, the oldest dated 2026-08-08, against a chain 38k blocks ahead. Taking the best price across all pending rows reported a SELF-CROSSED book (bid 1.4857 above ask 1.4534) on XCH/BYC. The engine separately logs `verify_pending_offer_coins: offer ... has terminal wallet status 3 but still in State` 1,614 times, which looks like the same class of bookkeeping gap — though five sampled ids from those warnings do NOT appear among the stale rows, so the link is unconfirmed.
- **Also affected:** `fetch_deployed_capital()` aggregates every pending row the same way, so the Balances tab's "Deployed %" is likely overstated by the same ghosts. Not yet quantified.
- **The heuristic is wrong in BOTH directions.** Too wide and eight-day ghosts return; too narrow and genuinely resting offers disappear. Observed 2026-08-22 while the engine was paused by the drawdown breaker: no new offers were posted for hours, so every side aged out of the 6.25h window and the panel showed em dashes — yet those offers were still resting on chain and takeable, because a pause stops new posting without cancelling what is live. An operator reading the panel would think we had no book at all.
- **Status:** `[x]` — LARGELY RESOLVED by S25 (PR #114, shipped v0.9.22).
  The root cause was exactly what S25 fixed: terminal outcomes were detected
  every cycle and never written down. Measured 2026-08-26, two days after
  the release: **pending+unresolved is 10, down from 48**, and **61 terminal
  outcomes were written in 48 hours**. Remaining work is not this bug: the
  10 survivors are the orphan class PR #114 identified (the wallet no longer
  knows them, so they can never self-heal) and need the bounded backfill
  SQL from that PR description, run once by the operator against the live
  DB. Tracked with S22.

### S12: One spurious print latches a GLOBAL offer halt for ~1000 blocks
- **Files:** `cpp/src/risk/limits.cpp` (`check_flash_crash`), `cpp/src/engine.cpp` (flash-crash state machine)
- **Issue:** `check_flash_crash` scans the ENTIRE retained price history tracking a running maximum, and returns true if any later price sits `flash_crash_threshold_pct` (0.20) below it. The maximum never decays and no outlier is rejected, so a single bad print holds it until that sample is evicted from the history buffer. In the `Crash` branch `any_pair_crashing` is evaluated FIRST and short-circuits, so `is_stable_after_crash()` is never consulted while it holds — the recovery machinery cannot run.
- **Observed 2026-08-22:** one XCH/BYC print of 2.3419 (versus ~1.57 either side, fully retraced within minutes) set the running max. It entered the feed at 09:28:17 UTC, block 9184230 (`logs/xop_trader.*.log`, `MarketDataFeed: pair=XCH/BYC mid=2.341920`); the 09:43:34 stamp sometimes quoted for it is the snapshots-table persistence time, fifteen minutes later. The ingest line shows the source: dexie delivered a CROSSED book that tick (`bid=3.904229 ask=2.320784 last=2.341920`), so the outlier is a junk ticker snapshot, not a trade. Every subsequent price ~1.63 reads as a 30% drawdown from it, so the detector reported a crash continuously. Step 8 stayed gated **for every pair**, not just XCH/BYC, for 4+ hours — while all five pairs measured stable within 5% with full history. The state can only clear when the print falls out of the 1000-entry buffer (`kDefaultPriceHistoryCapacity`), which is a function of elapsed blocks, not of market conditions.
- **Compounding:** the same print also fed the max-drawdown breach, from the OTHER side than first assumed. XCH/BYC is BYC-per-XCH, so 2.3419 against ~1.64 DEVALUES the BYC holding: 79.3 BYC fell from ~$73 to ~$51 of marks, about $22 of the $36.90 drawdown that tripped the breaker minutes later. The print played no part in setting the `$262.11` peak, which predates it by hours. One junk tick therefore both suppressed posting (this latch) and supplied most of the drawdown that paused the engine. The breaker's peak is in-memory and re-anchors on restart (`cpp/include/xop/engine.hpp`, `peak_equity_hwm_usd_`).
- **Status:** `[x]` — FIXED (PR #96). `check_flash_crash` now scans a window (`risk.flash_crash_window_blocks`, default 100 = `recovery_stable_blocks_phase2`; 0 restores whole-history), so an aged, retraced spike stops holding `any_pair_crashing` and the existing stability machinery runs. The 2026-08-22 latch replays as: junk print trips Crash; ~100 blocks (~31 min) later the spike exits the window, whose samples already satisfy BOTH stability phases — Recovery on that evaluation, Normal on the next, ~32 min total instead of >5 h pinned to buffer eviction. Outlier rejection at ingest (the crossed-ticker source) remains a possible second layer, deliberately not bundled into this fix.

### S13: Wallet mutations fail in post-submit sync flaps -- stale quotes stay live
- **Files:** `cpp/src/engine.cpp` (Step 8 loop), `cpp/src/rpc/chia_rpc.cpp:617`, `cpp/src/execution/offer_manager.cpp:1595`
- **Issue:** "Wallet needs to be fully synced" accounts for ~4,000 of 4,514 error lines (88%). Submitting one pair's offers flips the wallet into a transient syncing state; the sequential Step-8 loop issues the NEXT pair's cancels/creates ~76 ms later with no per-mutation sync re-check or backoff (measured: 6/6 offers created, then 4 consecutive selective_cancel failures 76 ms after). Failed cancels leave stale quotes live on Dexie at old prices — pick-off exposure — and feed S14. The only sync gate (engine.cpp:6585) fired 8 times against ~4,000 errors.
- **Status:** `[ ]` — OPEN. Sync-aware retry with short backoff around mutation RPCs, and/or reorder Step 8 to run all cancels before any creates. Touches live order handling: NOT to be fixed casually.

### S14: Stuck-offer forced cancel retries forever with no escalation
- **Files:** `cpp/src/engine.cpp:7109-7121`, `cpp/src/execution/offer_manager.cpp:3206`
- **Issue:** The forced-cancel loop retries with the SAME fee indefinitely: worst case one offer re-warned 158 times over ~36 h (age 851 → 7,782 blocks), fee frozen at 5,000 mojos on every attempt. While stuck, coins stay locked (170 "entering XCH-buy-only mode" warnings) and the quote stays live. Mostly downstream of S13.
- **Status:** `[ ]` — OPEN. Escalation ladder: retry with escalated fee → delete_unconfirmed_transactions + re-cancel → mark unrecoverable and alert ONCE. Rate-limit the per-offer warning. Touches live order handling.
- **[2026-08-26] S26 folded in here.** A separate item was filed after
  watching two wmilliETH.b/XCH bids (`0xa49d5c9edd`, `0xca4eed3a27`) be
  reported stuck and "attempting forced cancel" on every cycle for 6+ hours,
  age climbing 895 -> 1173 blocks (~3x the 400-block TTL), with no error
  logged after the attempt. Same signature later on XCH/DBX ("3 stuck
  offers ... attempting forced cancel"). That is not a new bug -- it is this
  one observed live, and S14 already names the mechanism (same fee, retried
  forever). Filing it twice was a duplicate on my part.
- **Second call site:** the escalation ladder must also cover
  `engine.cpp:7435` (`cancel_stale` in Step 8), not only 7109-7121.
- **⚠ Do NOT route `engine.cpp:7439` through the S25 re-verify buffer.**
  That write looks like the ones S25 fixed, but `cancel_stale` is
  *self-initiated*: the wallet reports PENDING_CANCEL for a while, which
  `recheck_terminal` reads as non-terminal and would discard -- breaking a
  path that currently works.
- **Diagnose before fixing:** either `cancel_stale` is not returning these
  ids, or the cancel is issued and does not take. Different fixes.


### S15: Spread-cap warning fires every block, including while paused
- **Files:** `cpp/src/engine.cpp:3456`
- **Issue:** 11,798 warnings (~14% of all warnings), the largest single shape in the log; 3,358 on 2026-08-22 alone while the engine was PAUSED — spreads recomputed and warned for quotes that will never post. Also indicates the vol/regime multiplier chain stays pinned above the cap for days.
- **Status:** `[ ]` — OPEN. Demote per-block to debug; warn on capped/uncapped TRANSITION with duration + worst overshoot; skip (or silence) Step-5 warnings while Paused. Separately investigate why the multiplier chain latches above the cap.

---

## Resolved since 2026-04 (audit evidence)

- **#6 Stronger inventory rebalance pull** — DONE: ratio-rebalance sizing with hysteresis bands (2331dda, engine.cpp:4722-4810) + Avellaneda-Stoikov reservation offset now actually applied to the ladder centre (96eefb0/f7108e9, reservation_offset.hpp, tested in test_reservation_offset.cpp); implemented via reservation-offset + capital-ratio scaling rather than a skew-gain tweak.
- **#9 Persist cost basis across restarts** — DONE: `inventory_state` table (database.cpp:164, additive `IF NOT EXISTS`), save/load at database.cpp:1002/:1094, restored at startup via `restore_record` with persisted sentinel flag (engine.cpp:994-1005), persisted on shutdown + periodically (engine.cpp:10663). Commit e4405a9 (P&L overhaul).
- **T4-01 CEX reference price integration** — OBSOLETE: superseded by CoinGecko-derived `cex_mid` fed to `ingest_cex_reference` (engine.cpp:2060-2100) + WLS fair-value solver fusing CoinGecko/TibetSwap/dexie (fair_value_solver.cpp, test_fair_value.cpp). Direct OKX/Gate client never built; `ArbitrageDetector::set_cex_prices` is dead scaffolding. Live per-exchange tickers for CEX-DEX arb would be a new, narrower item.
- **T4-10 Per-tier fill-rate monitoring** — DONE: `query_tier_fill_rates` (database.cpp:1461) fed into ladder each cycle (engine.cpp:5088-5113), blended into `tier_size_pct` with 5% floor (liquidity.cpp:795-861); on by default (config.hpp:994). Commits 679a934, 65b671b. Deviations from spec: side-aggregated 24h DB window instead of per-side rolling tracker; no Thompson sampling.
- **T8-26 PnLTracker database tests** — DONE: `test_pnl_tracker.cpp` (11 tests, commit e4405a9) covers DB rehydration, trade_id idempotency, USD conversion, CSV export; snapshot insert/query covered in test_database.cpp:520-562; equity-curve API removed by the P&L overhaul (drawdown breaker now on portfolio equity, 692c1fd).
- **S1 Phantom offer removal** — DONE: cae2bfd (see New findings S1 above); the MEMORY.md "proven, unfixed" index line is stale.
- **S2 CI red since 2026-04-08** — DONE: PR #70 / 206d55c (see New findings S2 above); recent runs green as of 2026-08-18.

### S16: Taker paths lack floor-aware XCH pre-checks
- **Files:** `cpp/src/engine.cpp` (Step 9c ~9203, 9e ~9796, 9f ~10322)
- **Status:** `[ ]` -- Found 2026-08-23 by the coin-lock-ledger adversarial
  review (minor): Step 9c's crossed-book taker lifts asks with NO balance
  check at all (on a quote=XCH pair it spends XCH principal + fee straight
  from the pool; bounded per cycle by max_take_xch, so not incident-class).
  9e and the 9f drift corrector check spendable >= cost but omit the fee
  and do not require the fee-reserve floor to survive. Takers are instant
  spends, not standing locks, so they stay outside the cycle ledger by
  design -- but each should get a floor-aware pre-check:
  spendable - cost - fee >= fee_reserve_xch.

### S17: Depeg bail-out re-alerts at ERROR every block (no rate limit)
- **Files:** `cpp/src/engine.cpp` (Step 3 depeg bail-out alert)
- **Status:** `[x]` FIXED 2026-08-23 -- Step 3 logs depeg state
  TRANSITIONS at full severity (with an info recovery line) and ongoing
  states at debug; the Step 4 per-cycle suppression note dropped to
  debug. The second-source-confirmation idea (below) remains open as a
  separate hardening. Originally observed live 2026-08-23 10:25-10:30+: "DEPEG
  BAIL-OUT BYC/wUSDC.b price=0.750000 -- pulling all quotes!" repeated at
  ERROR level every ~30-60s for the duration of the bail-out, unlike the
  Step 13 drawdown breaker which gates re-alerts to 30 min. Alert once on
  the transition (and on recovery), debug thereafter. Bonus observation
  from the same incident: the bail price was a single stale order on an
  emptied dexie book (TibetSwap AMM simultaneously priced BYC at $0.92,
  not $0.75) -- consider requiring depeg confirmation from a second source
  (TibetSwap reserves) before bail-out, mirroring the S12 flash-crash
  junk-print lesson.

### S18: Max-drawdown re-alert gate not holding while breaker-latched
- **Files:** `cpp/src/engine.cpp` (Step 13 drawdown breaker alerting)
- **Status:** `[x]` FIXED 2026-08-23 -- root cause: the gate re-armed on
  ANY single lifted evaluation of the breach condition, so one transient
  false read (flaky wallet RPC corrupting an equity computation) cleared
  it mid-episode; the log shows 1,088 correctly-suppressed evaluations
  interleaved with the premature re-alerts, and equity never actually
  recovered above the threshold. Fix: re-arm only after 10 consecutive
  lifted evaluations (~2-3 min), so an RPC blip cannot reset the 30-min
  interval but a genuine recovery still re-arms promptly. Originally
  observed live 2026-08-23 10:43:51 / 10:44:12 /
  10:44:53: three ALERT:CRITICAL max-drawdown alerts in 62 seconds while
  breaker_pause_active_ was already latched, despite the configured
  realert=30min gate (which held correctly on 2026-08-22 during the
  v0.9.10 incident, ~15-30 min cadence). Suspect the re-alert timestamp
  resets on some per-evaluation path when the state is already Paused, or
  the gate only covers the un-latched trip path. Alert once per 30 min as
  documented, regardless of latch state.

### S19: Bridge transfers need first-class ledger accounting (deposits, not P&L)
- **Files:** `cpp/include/xop/accounting/bridge_ingest.hpp` (new),
  `cpp/src/engine.cpp` (step_ingest_bridge_flows), `cpp/src/monitoring/pnl.cpp`,
  `gui/services/database_service.py`, `gui/widgets/dashboard.py`
- **Status:** `[x]` BUILT 2026-08-23 -- engine reads warp_jobs.db read-only
  each heartbeat and books bridge_deposit (+post_tip mojos, inbound at
  COMPLETED) / bridge_withdrawal (-burned mojos, outbound at the FIRST
  burn-confirmed status: COLLECTING_EVM_SIGS/RELAYING/
  AWAITING_EXTERNAL_RELAY, since COMPLETED can lag unboundedly behind the
  wallet-affecting burn) BEFORE the divergence control, idempotent via
  ledger event_id "bridge:job:<id>:<created_at>"; chronology (opening
  filter + peak guard) uses the job's first booking-eligible warp_events
  timestamp, immutable unlike updated_at.  GIPS/TWR treatment:
  signed USD accumulates as a net-deposits figure outside trading P&L
  (PnLSummary::net_deposits_usd, Prometheus component="net_deposits",
  dashboard "Net Deposits" card); the drawdown peak is NOT adjusted
  in place for flows (owner decision after review rounds 24-41 refuted
  every in-place scheme -- fills are tracked base-side only, so wallet
  movement cannot be attributed to a specific flow): booking an
  in-process flow resets the peak and the next equity valuation
  re-anchors it, identical to the accepted restart semantics.
  Pre-process flows are already inside the current anchor and leave it
  untouched.  wUSDC.b valued at the $1.00 numeraire (peg monitored, not
  priced in).  Originally raised by the operator after the first live bridge
  (2026-08-23, job 2: +4.985 wUSDC.b). The ledger has no deposit/transfer
  event type, so a completed bridge inflow is absorbed by the divergence
  control as an "adjust" entry ("unexplained divergence reconciled to
  wallet") -- books tie, but attribution is blind: the equity jump can
  mask real trading losses in the rolling window, and nothing separates
  capital movements from performance. Design (as implemented; see the
  Status line for the review-hardened details): the engine reads
  data/warp_jobs.db (read-only; the GUI owns writes) during the ledger
  tie and posts event_type "bridge_deposit" (+post_tip_mojos, inbound
  at COMPLETED) / "bridge_withdrawal" (unwraps, at the FIRST
  burn-confirmed event -- COMPLETED can lag unboundedly behind the
  wallet-affecting burn) for jobs not yet booked -- BEFORE the
  divergence control runs, so the movement is explained rather than
  adjusted. P&L tracker gains a net-deposits component excluded from
  performance; GUI P&L display gains a "Net deposits" line.

### S22: pre-S20 junk persisted to the DB
- **Files:** `data/xop_trader.db` (`inventory_state`, `snapshots`)
- **Issue:** S20 stops junk prints reaching equity going forward, but rows written before it survive -- BYC/wUSDC.b `sigma_annual=143.30` from a warm-start, and cost basis stamped at junk prices.
- **Status:** `[ ]` — Live DB, so any sweep needs an explicit operator go-ahead.

### S24: run_full_reconciliation dominates the heartbeat
- **Files:** `cpp/src/monitoring/on_chain_reconciler.cpp`, `cpp/src/engine.cpp`
- **Issue:** ~800s of every ~840s cycle. The original "~12.5s per block" cost model was a MISATTRIBUTION (pass_duration/blocks_elapsed): log-bracketing proved ~97% of every pass was `verify_pending_offer_coins` paginating the wallet's ENTIRE trade archive (20-25k records, 400-500 round-trips) with the default sort -- a pre-fix fork of the exact pathology the [WALLET-LOAD 2026-08-04] fix cured in `OfferManager::reconcile_offers`. Blocks/balances were the 23-27s tail. Also the dominant source of the wallet process's measured 64%-of-a-core sustained burn, and the pressure behind S13's sync flaps.
- **Status:** `[x]` — **FIXED, PR #123, merged 2026-08-30** after a 16-agent adversarial review (which caught a blocker in its own first fix: `get_offer` THROWS on absence, so wallet-lost offers could never be reaped). RELEVANCE + early-stop pagination; age at scan-snapshot; no miss accrual in grace; direct `get_offer` verification routed through the pure, unit-tested `classify_direct_lookup` (only explicit terminals reap); per-phase timing in the completion log. **Field-verified 2026-08-30 08:30**, first pass on the v0.10.8 engine: `(verify 2.7s, balances 24.7s, fees 0.0s, total 27.3s)`, 3 pages scanned for 6 pending offers -- was ~820s and 400-500 pages. Correctness intact (all pending found, 0 stale, 0 discrepancies). Follow-ups deferred: route wallet-terminal stales through the S25 buffer; reconciler RPC-mock harness; revisit `reconciliation_interval_blocks`.


### S27: assets with no enabled pair price at $0 -- silently, and the S20 gate cannot see it
- **Files:** `cpp/src/engine.cpp:11582` (`asset_usd_pseudo_price`), `:11277` (`usd_per_xch`), `cpp/include/xop/risk/drawdown_breaker.hpp`
- **Issue:** `asset_usd_pseudo_price` prices an asset only through **enabled** pairs. Disabling the wUSDC.b pairs on 2026-08-25 therefore dropped XCH, DBX, wUSDC.b and wmilliETH.b to $0 each, leaving equity equal to the BYC balance alone (peak $63.82 == 63.818 BYC). Two compounding faults: (a) `usd_per_xch()` accepts **only** an enabled `XCH/⟨wUSDC.b|wUSDC|USDS⟩` pair as its anchor -- BYC never qualified -- so `snapshots.xch_usd_rate` went from 1.9165 (35% too high, priced through a depegged wrapper) to 0.0; (b) the S20 carry-expiry degradation check is nested **inside** the "carry entry found" branch, so an asset that was NEVER valued contributes $0 without setting `degraded`, the `ValuationAuthorityGate` never fires, and the peak re-anchors to a partial book.
- **Consequence:** with only XCH/DBX enabled, equity is exactly $0. `equity_drawdown_frac` returns 0.0 when the peak is non-positive, so **both breakers go inert rather than trip** -- trading with no drawdown protection, and fills landing on `record_fill_unpriced`.
- **Status:** `[~]` — Built on `fix/s27-equity-blindness` (PR #117), not merged. Two of the three parts shipped: "never valued" now sets `degraded`, and `usd_per_xch()` prefers the external CoinGecko XCH/USD (freshness-gated via `coingecko_feed_fresh_for_revival`, so a frozen feed cannot take permanent priority over the DEX fallback) — the only anchor that survives both issuers being compromised. Also added `unvaluable_book_must_fail_closed`, so a book that cannot be valued pauses rather than trading blind.
  **The third part was answered differently.** "Decouple observation from `pair.enabled` so a disabled pair is still watched" would price wUSDC.b from its own book — but that book is thin and issued by a compromised bridge, which is precisely the input S20 exists to reject. S32 takes the other route: an asset with no enabled pair is worth **$0**, explicitly. Re-enabling a pair restores a real price with no code change, so the watch-disabled-pairs option stays open if a credible book ever returns.

### S28: engine cannot fall back to the wallet for block height mid-outage
- **Files:** `cpp/src/engine.cpp:1237` (height call), `:13901-13929` (`open_connections`), `:1597` (`check_pause_flag`)
- **⚠ CORRECTED 2026-08-27.** Originally filed as "the GUI pause flag is unreachable during a node outage", claiming Pause and Resume are dead controls. **That was overstated.** `check_pause_flag()` runs at the TOP of `on_new_block_coro` (1597) while Step 8 posts at 1881, so on recovery the flag IS read before anything posts — and during the outage nothing is trading regardless. The flag genuinely is not read *while* the node is down, but the safety consequence asserted does not follow from that.
- **What is actually wrong, in two parts of very different weight:**
  1. **SUBSTANTIVE — no wallet fallback.** `wallet_only_mode_` is assigned only inside `open_connections()`, so the `mode: auto` decision is one-shot at startup. On 2026-08-25 the full node was unreachable for ~2.5h (529 consecutive `get_block_height` failures) **while the wallet RPC stayed healthy the whole time** — and `wallet_->get_height_info()` is already the height source in wallet-only mode. The engine sat dead beside a working alternative it could not switch to.
  2. **MINOR — no pause feedback during an outage.** An operator who sets `pause.flag` while the node is down gets no log line and a stale GUI status, so cannot tell whether it registered. `check_pause_flag()` is pure filesystem and state with no RPC, so it can safely be called from the poll loop before the height call.
- **Status:** `[ ]` — (2) is trivial and safe. (1) is the real item and needs care: switching height source mid-flight changes which RPC the heartbeat depends on, and `mode: auto` semantics need thought. Re-scoped, not descoped.


### S29: peg identity is a repeated string comparison, not a property of the asset
- **Files:** `cpp/include/xop/peg_registry.hpp` (new), `cpp/src/engine.cpp` (5 sites), `cpp/src/monitoring/pnl.cpp:1512`, `cpp/src/strategy/arbitrage.cpp:916`, `cpp/src/strategy/market_allocator.cpp:188`, `cpp/include/xop/feed_listings.hpp:47`
- **Issue:** 15 sites each ask a variant of `quote == "wUSDC.b" || ... == "USDS"` and independently conclude "worth exactly $1". Two failures followed: BYC's depeg watch existed only because a *pair* carried `is_stablecoin`, so disabling BYC/wUSDC.b removed monitoring from the asset that had just become the book's dollar anchor; and `quote_usd_factor`'s BYC branch falls through to `return 1.0`, so BYC kept marking the portfolio at par after Circuit DAO announced the protocol would be sunset.
- **Status:** `[~]` — Registry built and the VALUATION call sites converted
  on `feat/peg-registry` (PR #115): `engine.cpp` now contains zero
  comparisons against `"wUSDC.b"` / `"wUSDC"` / `"USDS"` / `"BYC"` and no
  bare `return 1.0` in the USD-factor path. Asset-keyed, declares the peg
  **currency** (so EUR/JPY pegs are expressible and a missing FX rate yields
  *no valuation* rather than a silent 1:1), separates `Unobserved` from
  `Holding`, and has an `enforce` flag. 22 registry tests plus 14 parser
  tests, sabotage-verified.
  **`pnl.cpp:1512` is now handled too, and it was the dangerous one.** Its
  retired-pair fallback triggered whenever `usd_per_quote_unit <= 0`, which
  is exactly what `quote_usd_factor` returns when a declared par is
  UNAVAILABLE — so `enforce:false` on wUSDC.b, the whole point of this work,
  silently routed back to a hardcoded $1.00 per unit in the P&L totals.
  Registered-but-unpriceable now contributes 0.0; only never-registered
  pairs keep the symbol fallback, which rehydration genuinely needs.
  **Still carrying the $1 assumption:** `arbitrage.cpp:916`,
  `market_allocator.cpp:188`, `feed_listings.hpp:47`.
  **STILL OPEN — the observation half.** `PegRegistry::classify` has no
  production caller; `DepegDetector` is registered and updated solely from
  `PairConfig::is_stablecoin` and the pair loop, so disabling `BYC/wUSDC.b`
  still removes BYC's only peg observation. Wiring an asset-level detector
  independent of `pair.enabled` is the remaining work, and is what actually
  closes S30.

### S30: both issuers of our quote assets were compromised within a day
- **Files:** `config.yaml` (pairs), `TODO-COMPETITION.md`, memory `warp-green-bridge-compromise`
- **Issue:** warp.green (minter of every `.b` asset) reported compromised 2026-08-25 — wUSDC.b traded ~26% below par on the implied peg for hours while accounting valued it at $1.00. Then Circuit DAO reported its treasury drained and stated the protocol "will have to be sunset and relaunched", so BYC is being wound down rather than merely depegged. Four XCH→BYC asks filled 2–4 hours *after* that announcement, because BYC had no external feed and its only peg watch was on the pair we had already disabled.
- **Status:** `[~]` — Contained: trading paused, all BYC/wUSDC.b-acquiring offers cancelled (0 takeable), XCH/wUSDC.b, BYC/wUSDC.b, wmilliETH.b/XCH and XCH/BYC all disabled, `recovery.pair_allowlist` emptied. **Open:** the ~58 BYC and ~79 wUSDC.b holdings, and whether to follow Circuit's relaunch. Both are operator calls. Blocks S27 — with every stablecoin pair disabled there is no USD anchor left, which is why the external-feed fix matters.

### S31: no dead man's switch -- a wedged engine leaves live offers on the book
- **Files:** `cpp/src/engine.cpp` (heartbeat), `cpp/src/execution/offer_manager.cpp`
- **Issue:** On 2026-08-25 the engine spent ~4h unable to reach the full node while offers rested on dexie. When the node returned, six four-hour-old XCH/BYC bids filled in the same second and the rolling-window breaker tripped (-$12.71 over 3 blocks). Nothing cancels our book when the engine stops functioning -- the offers outlive the process that is supposed to be managing them. S28 compounds it differently from how this item first claimed: `pause.flag` is not read *during* the outage, but it IS applied before Step 8 on recovery (see the correction in S28), so the operator can still stop the engine — what they cannot do is retract the already-live book, which is exactly why this item is separate from S28.
- **Prior art:** Permuto exposes exactly this as a first-class endpoint (`POST /exchange/schedule_cancel`, Hyperliquid-style): arm a future cancel-all, extend it on every healthy loop, and the venue cancels for you if you stop. Policy there is `min_delay_ms` 5000, `max_triggers_per_day` 10 fresh arms, with rescheduling-while-armed unlimited -- so the pattern is *extend*, never disarm-and-rearm. See `docs/permuto-api-reference.md` §2, which lands with PR #116 -- this path does not exist on `main` until that merges.
- **Design note:** dexie has no server-side equivalent, so ours must be local and must NOT depend on the heartbeat it is protecting against -- a watchdog inside the loop that wedges is worthless. Wallet RPC stayed healthy throughout the 2026-08-25 outage while the full node was unreachable, so a wallet-only cancel path is viable.
- **Status:** `[ ]` — Identified 2026-08-26 while scoping Permuto; the capability is a straight transfer back to the dexie bot.

### S32: "no price" hid two conditions, and the safe response to each is the opposite
- **Files:** `cpp/include/xop/risk/drawdown_breaker.hpp` (`unpriced_asset_is_written_off`), `cpp/src/engine.cpp` (`asset_has_pricing_path`, `compute_portfolio_equity_usd`), `cpp/include/xop/engine.hpp`
- **Issue:** S27 made never-valued held assets degrade the cycle *unconditionally* — correct when the engine is trying and failing to price something, wrong when the operator has removed every route to a price. Verified against the live wallet 2026-08-27: **wUSDC.b (78.609 units)** and **wmilliETH.b (1.0)** are held with every pair naming them disabled, wUSDC.b since the warp.green compromise. Neither can ever be priced as configured, so both degrade from cycle 0 on every start; `ValuationAuthorityGate` needs 10 consecutive clean cycles to re-arm and never gets one; `peak_equity_hwm_usd_` stays `0.0`; grace expires; `unvaluable_book_must_fail_closed` fires through its no-peak branch.
- **Consequence:** **the release whose stated goal is "make it safe to resume" would not have resumed.** The engine would start, run out its startup grace, pause itself, and do it again on every restart. The fail-closed logic was right; what was missing was any way to say "this holding has no price and I know it".
- **Fix:** split "no price available" on whether a **route** exists, never on whether a **quote** exists. `asset_has_pricing_path()` asks whether a route to a USD anchor exists at all — a pure function of CONFIGURATION, deterministic at startup, deliberately ignoring mids, factors and valuation grades so a junk book can never masquerade as "no path". Three route types, not one: (a) the asset has its own declared, enforced par; (b) an enabled pair names it against a wrapper that does; (c) an enabled pair names it against XCH **and XCH itself is anchored** — by `coingecko.enabled` with `"chia"` in `coin_ids` and a usable `cex_freshness_threshold_sec`, or by an enabled XCH pair against a declared par. Mere pair membership is NOT a route: an enabled `CAT_A/CAT_B` pair with no XCH leg and no enforced par gives `quote_usd_factor()` nothing, forever — and counting it would withhold the write-off, degrade instead, and never lift, since the cause is configuration rather than a feed outage. No route and no carry → written off at $0, contributing nothing and degrading nothing. Route present but quiet → carry it and degrade on expiry, exactly as before.
- **Why not simply zero every unpriced asset:** that is strictly worse than the bug. A momentary CoinGecko outage would write the whole book to nothing, and on a fresh process — before the first fetch lands — it would seed the drawdown peak from a near-zero equity and leave the breaker under-protective for the entire run. `S32_AQuietFeedIsNeverWrittenOff` pins it.
- **Containment:** written-off assets stay out of `live_count`, so a book made up *entirely* of them still reports all-unpriced and still fails closed. The write-off cannot become a way to switch the breaker off for the whole portfolio.
- **Also:** the never-valued log moved `debug` → `warn` (Kimi K3 review §1.4). It is the signature of the 2026-08-25 incident and stayed invisible in the logs while the breakers sat inert. Both new logs fire once per asset per process.
- **Operator note:** equity now **excludes** the wUSDC.b and wmilliETH.b holdings. That is honest rather than lossy — the units stay in inventory with their cost basis, so if warp.green announces a redemption ratio for old coins, re-enabling a pair restores a real price with no code change.
- **Status:** `[x]` — `78b97f0` on `fix/s27-equity-blindness` (PR #117). 794/794 tests pass, 5 new.

### S21: Bridge chronology uses status time, not wallet-effect time (bounded, deferred)

Copilot round 30 (suppressed comment, acknowledged): the S19 opening filter's
lower bound is MIN ts of CLAIMING / BURN_SENT, but the warp service enters
CLAIMING before a later handler invocation pushes the actual claim spend --
so a ledger genesis captured in that window produces an opening WITHOUT the
mint while `flow_lb <= opening_time` skips the booking permanently.  The
flow then books as an adjustment (pre-S19 behaviour): equity and the
invariant stay correct, only Net Deposits attribution misses it.  Fix
requires the GUI warp service to persist the wallet-affecting claim/burn
chain height per job (new column + event write at push time), with the
engine comparing THAT against the opening.  Cross-component; deliberately
not smuggled into PR #109 at round 30.  Window is minutes wide and only
matters when ledger genesis lands inside it.  Round 43 adds the inbound
twin: a claimed mint sits in CLAIMING for the confirmation-depth wait
after the CAT is on-chain, so the invariant briefly absorbs it as an
adjust before the COMPLETED booking converges through the three-entry
catch-up -- the same persisted wallet-effect event closes both windows.

### S20: Equity valuation rides warm-up/stale prices -- breaker false trips
- **Files:** `cpp/include/xop/execution/mid_gate.hpp` (new pure header),
  `cpp/src/execution/market_data.cpp` (gate wiring, anchored offer filter,
  ob_updated_at), `cpp/src/engine.cpp` (implied-cross anchor injection,
  valuation-grade gates, carry TTL, Step 13 authority gating),
  `cpp/tests/test_mid_gate.cpp`
- **Status:** `[x]` -- Built 2026-08-24 (branch feat/s20-valuation-guards)
  after the definitive exhibit: the junk 187.461980 BYC/wUSDC.b print
  (byte-identical 12+ h) re-poisoned a FRESHLY RESTARTED v0.9.20 in ~40
  minutes (peak seeded $15,180.38, honest equity $258.94 read as a 98.29%
  drawdown, breaker latched 09:25).  Root causes found by the review of
  the feed: (1) the per-offer outlier filter anchored on the pair's OWN
  previous mid -- self-referential lock-in that rejected honest ~1.0
  offers as outliers while junk passed, and was skipped entirely on the
  first cycle (the fresh-restart re-poisoning path); (2) ps.orderbook_mid
  had no age of its own, so a throwing offers fetch froze it while
  dex_updated_at kept re-stamping; (3) no plausibility bound existed on
  the published mid at all.  Fix (design B "guard at the source" +
  grafts, judged 2-0 by independent panels): every candidate mid is
  gated against an independent anchor -- CEX > triangulated implied
  cross through healthy sibling books (generalised for EVERY triangle,
  per operator direction) > AMM > fair value > peg -- with a wide 3x
  band and a book-confirmation escape so real repricing passes; a breach
  publishes NO mid.  MarketSnapshot gained mid_valuation_grade (equity
  valuation and P&L marks consume only grade prices; a last-trade-only
  0.75-class print publishes but cannot mark).  The depeg detector is
  deliberately NOT grade-gated -- liquidity leaves before a peg breaks,
  so gating would mute it in the mode it exists for -- and the
  carry cache gained a TTL (risk.valuation_carry_ttl_blocks, degraded
  equity freezes the PEAK ONLY -- both breakers stay armed against the
  frozen peak, S18-style 10-clean-cycle re-arm for peak updates), and the
  raw-BBO 10x guard now falls back to the anchor
  chain on CEX-less pairs.  692/692 tests incl. 12 incident-replay pins.
  SCOPE CORRECTED after review (45-agent panel + Copilot rounds 1-2
  found 25 confirmed issues, 5 critical): the first cut over-reached and
  several gates created NEW failure modes, now reverted --
  (a) the PnL get_price grade gate ZEROED unrealized P&L rather than
  carrying it (pnl.cpp assigns inventory_pnl = 0 on price<=0), injecting
  a false step into the very rolling-window series it claimed to protect;
  (b) grade-gating usd_per_xch/quote_usd_factor collapsed ALL USD figures
  to zero whenever CoinGecko blipped, because XCH/wUSDC.b's filtered book
  is one-sided most cycles so grade reduced to "is CoinGecko up?";
  (c) grade-gating the depeg detector muted it during a liquidity-crisis
  depeg -- the mode it exists for -- and silently rescaled
  depeg_sustained_blocks from blocks to invocations;
  (d) disarming BOTH breakers on degraded valuation was fail-OPEN (engine
  kept quoting with no drawdown protection); degradation now freezes the
  PEAK only, and the comparison still runs.
  Grade is now scoped to asset_usd_pseudo_price (the actual incident
  hole) + peak authority.  ADDED from the panel: bbo_from_filtered_book
  provenance flag (set by the filtered ingest, CLEARED by the raw ticker)
  -- without it the bot could read its OWN resting offers back as
  third-party confirmation of a band breach after a failed offers fetch;
  dex_print_age wired to a consumer at last (frozen book cannot confirm);
  per-offer absurdity band widened to 10x and SEPARATED from the 3x gate
  band, because filtering at the gate's own band made the
  book-confirmation escape unreachable for the real move it exists for;
  anchorless pairs keep the legacy near-reference filter (dropping it was
  a strict weakening); implied_cross_max_leg_spread_bps 300 -> 1500 (300
  was below the measured spread of most books, so the triangulated anchor
  could never form); non-finite config/gate guards; unsigned underflow on
  block regression.  699/699 tests incl. 3 ingestion-path tests that
  drive the real feed (the pure-gate tests could not catch a
  gate-vs-ingestion contradiction).
  Residual (documented, accepted): the wash-book exposure extends to the
  OFFER-ABSURDITY bound, not the 3x gate band.  Offers are screened at
  max(10x, 2*band) -- deliberately wider than the gate, or a real collapse
  could never assemble the two-sided book its confirmation escape needs
  (RealCollapseSurvivesTheOfferFilterAndPublishes exercises exactly that
  at 0.2x).  So a coordinated fresh two-sided book anywhere in roughly
  0.1x-10x of the anchor can override the band and mark equity, at real
  capital cost to whoever posts it.  Tightening the confirmation policy
  would trade this away for the inability to price a genuine collapse --
  the opposite failure, and the one that actually happened;
  pre-S20 junk persisted to DB (cost basis, AS warm-start snapshots) is
  NOT repaired by this change -- separate sanitation task if it bites;
  valuation_carry_ttl_blocks is calibrated at 52 s/block while the
  deployment runs ~17-21 s/block, so the real TTL is ~3.5-4 h not 10.4 h;
  the carry TTL cannot fire for BYC itself because quote_usd_factor's par
  fallback always reports a live price.

### S33: Step 8's crossed-mid guard has been mis-referenced since 2026-04-13
- **Files:** `cpp/src/engine.cpp` (Step 8, "Crossed-mid pre-post guard"), `cpp/include/xop/execution/cross_guard.hpp`
- **Issue:** The guard drops a bid above the PUBLISHED MID and an ask below it, to pre-empt `classify_tier_staleness` cancelling the offer next cycle. It was a bit-exact predictor of that canceller when written in `4d3f30d` (2026-04-12). **One day later `a932a5d` moved the canceller onto the BBO and did not touch the guard**, recording why in `offer_manager.cpp`: "Using model mid as the threshold is too conservative -- a bid between mid and best_ask is a valid competitive bid, not a crossed offer." `git log -L` over the guard returns `4d3f30d` alone. So for ~4.5 months it has been strictly stricter than the thing it pre-empts, removing every ask in `(best_bid, mid]` and every bid in `[mid, best_ask)` -- the profitable half-spread on each side. The divergence widened when `fv::blend_quote_center` landed 2026-08-01.
- **Status:** `[~]` -- SHADOW SHIPPED, decision deferred. `cross_guard.hpp` computes both verdicts; Step 8 suppresses on the live one unchanged and logs `[CROSSGUARD-SHADOW]` only on disagreement. **Deliberately not fixed:** the guard is inert on the only enabled pair (zero firings across six live rotations, one in the entire retained corpus), every "would suppress" figure is a reconstruction rather than an observation, and the obvious fix -- re-pointing at `pcs.quote_mid_mojos` -- is wrong: at centre 1.41 against best_bid 1.50 an ask at 1.41 genuinely IS crossed, and only the BBO separates that from an ask at 2.00.
- **Next step:** read the counter after a deploy. Silence closes the question.

### S34: The fee-to-gain gate scores edge in the wrong price frame
- **Files:** `cpp/src/engine.cpp` (Step 8 fee gate), `cpp/include/xop/strategy/tier_gain.hpp`
- **Issue:** The gate measures expected gain as distance from the PUBLISHED MID, justified in-comment as avoiding A-S skew bias. That effect is real -- tier price is `centre*(1 +/- spacing)`, so scoring from the post-A-S centre reports the nominal spacing identically on both sides and erases the inventory-skew cost exactly where A-S places it. But the published mid is a third frame again, and measured on XCH/DBX over 5,080 cycles the error it introduces is **mean 143.2 bps (max 959)** against an A-S skew of **mean 11.8 bps (max 29.9)** -- twelve times larger. Direction is what the comment misses: the gate only DROPS tiers, so a farther reference is more PERMISSIVE, not more conservative. The correct reference is the fair-value centre BEFORE the A-S shift, now persisted as `pcs.quote_fair_centre_mojos`.
- **Status:** `[~]` -- SHADOW SHIPPED, decision deferred. The gate has **never fired**: zero `skipped (round-trip fee` lines across every retained log including `engine.log`. At the live fee it would need a tier within 0.00134 bps of its reference; measured closest approach is 6.97 bps, and the width floor holds tiers at 70 bps in the fair-value frame.
- **Next step:** read `[FEEGAIN-SHADOW]` after a deploy.
- **COUPLED WITH THE SIGN TRAP -- do not fix half of it.** The gate takes `std::abs`, which makes its own `std::max(0.0, ...)` dead code and credits a wrong-side tier in full. The obvious cleanup is a signed edge. **Alone that is a disaster:** under the published-mid frame, bid tiers sit ABOVE the mid whenever the centre shift exceeds the spacing -- 81.4% of cycles -- so a signed edge from the published mid clamps to zero and drops nearly every bid. Frame and sign move together or not at all.

### S35: A per-pair price band was asked for; most of it already existed
- **Files:** `cpp/include/xop/strategy/bbo_sanity.hpp`, `cpp/include/xop/config.hpp` (`bbo_sanity_*_override`)
- **Outcome:** `[x]` -- CLOSED. `strategy::classify_tier` already was the requested control: a per-side percentage max-distance bound measured against the SAME-SIDE BBO that SUPPRESSES rather than clamps. Only its two thresholds were global, and the pairs are not alike -- XCH/DBX's sigma width floor never exceeded 141 bps in 48,402 evaluations while XCH/BYC's exceeded 200 bps on 87.7%. Per-pair overrides shipped in PR #134, bounded `(0, 1]` and rejected at load otherwise, because `10` and `1000` are both plausible operator entries and either yields a cap that can never bind.
- **Recorded so it is not re-proposed, each with evidence:**
  - **Do not reference the mid.** It breaks exactly when a bound would bind: a mid-referenced 2% band would have forced correct 1.33 bids to 5.4635 on 2026-08-30, and forfeits 78.4% of realized P&L against 5.6% for the same band measured same-side. No production band in the verified literature (LULD, Nasdaq 4702(b)(7), CME, Binance, Kraken, Hyperliquid, Xetra) references an instantaneous mid.
  - **Do not depth-qualify the reference.** Depth walking is monotone away from the book's interior, so it repairs only too-aggressive touches -- while on a venue with no matching engine aggressive offers are consumed and passive ones fossilize (Spearman rho between dislocation and resting age +0.615; median resting age 0.41 d within 10% of fair vs 125 d at 10-100% off). Its helpful direction covers the small errors; its harmful direction covers the catastrophic ones.
  - **Tight is not conservative.** When the sigma floor exceeds the bound the innermost tier is already outside it, so every tier is. A 2% passive bound withdraws XCH/BYC on 87.7% of blocks; on XCH/DBX anything >= 10% never binds. Inert or fatal, little in between.

### S36: No test in `cpp/tests` constructs an `Engine`
- **Files:** all of `cpp/tests/`
- **Issue:** `xop_core` compiles `engine.cpp` into the test binary, so Step 8's guards LINK but are never EXECUTED. Not theoretical: a change to Step 8 Check 1 in PR #134 converted a check that never fired (0% deviation) into one that cleared every tier on every block (116.7%), and passed **four review rounds and 1000 green tests** before an independent adversarial pass caught it.
- **Status:** `[~]` -- PARTIALLY MITIGATED. The response so far is to extract decisions into pure headers that tests CAN drive: `book_side_quality.hpp` (`step8_references`), `cross_guard.hpp`, `strategy/tier_gain.hpp`, `risk::peg_usd_observation`. That covers the branch tables but not the wiring between them. A real fix is an Engine-level harness, or a seam letting Step 8 run end to end.
- **Interim rule:** any change to a Step 8 guard must be accompanied by a pure-function extraction, or it ships unverified.

### S37: `is_own_fill` is never persisted, so no venue-tape statistic is reproducible
- **Files:** `cpp/src/execution/market_data.cpp` (`ingest_trade`, `ingest_trade_for_vpin`), `db/schema.md`
- **Issue:** `is_own_fill` is an in-memory VPIN filter parameter. `PRAGMA table_info` confirms neither `trade_log` nor `taker_fills` carries an own-fill column, and both hold only our own fills by construction. ~19% of the August XCH/BYC venue tape was ours (270 of ~1,402 trades), and every estimator in the microstructure literature assumes flow is exogenous -- quoting from a tape we partly wrote closes a feedback loop.
- **Status:** `[ ]` -- OPEN, and a prerequisite rather than a task on its own. Excluding own fills means matching settled dexie offers against our own ids (`offer_log.offer_id` posted, `trade_log.offer_hash` filled, plus `taker_fills` for offers we crossed) and then PERSISTING the verdict. Until that exists no venue-tape statistic is reproducible. See `docs/price-discovery-from-trade-history.md`.

### S38: TibetSwap pools are DRAINED, and its fee still sets our quoting floor
- **Files:** `cpp/src/rpc/tibetswap_client.cpp:155-161` (reserve guard), `cpp/src/main.cpp:335-346` + the four RPC clients (logging), `cpp/src/engine.cpp:7098-7101` (the floor)
- **What is actually true, measured 2026-09-01.** The API is UP (HTTP 200) and our client's pagination works. **The LIQUIDITY is gone: 364 of 374 pools carry non-positive reserves (97.3%), including DBX, BYC, wUSDC.b and wmilliETH, all at `xch_reserve: 1, token_reserve: 0`.** `/quote` returns `amount_out: 0, price_impact: 1.0`. The pools are genuinely drained -- consistent with the hack.
  - An earlier revision of this entry said the venue was reachable and implied it was usable. That was wrong: HTTP 200 was the wrong test. Also note an unpaginated `GET /pairs` returns only 10 rows, which briefly made it look as though our assets had no pools at all -- they do, they are simply empty.
- **Our code is behaving CORRECTLY.** `parse_pool_json` rejects non-positive reserves because a zero `token_reserve` divides by zero in `tibet::get_implied_price()`. The only defect is REPORTING: it collapses "this pool is empty" into "no pools", which is the sentence the operator read 1,738 times.
- **THE REAL DEFECT THIS EXPOSED, and it is systemic rather than TibetSwap-specific.** All four RPC clients build their loggers with `spdlog::stdout_color_mt` and never receive the rotating FILE sink created in `main.cpp:335-346`: `chia_rpc.cpp:217`, `coingecko_client.cpp:63`, `dexie_client.cpp:193`, `tibetswap_client.cpp:62,289`. So the file log carries the engine's **symptom** 1,743 times and the client's **diagnosis** zero times. Any RPC-layer fault is invisible in the only log that survives a restart.
- **Status:** `[ ]` -- OPEN. Three separable actions, in value order:
  - **(a) Logging sinks -- do this one.** Register the client loggers over the same sink list as the default logger (or have clients `spdlog::get(name)` with a clone fallback). Extractable as a pure `make_client_logger(name, sinks)` with a gtest asserting the returned logger carries both sinks. Zero blast radius on quoting.
  - **(b) Diagnostic honesty in the directory parse.** Split the DIRECTORY parse (needs `pair_id` + `asset_id` only) from the RESERVE-validity check, so an empty pool is indexed-but-unusable and the engine can say "TibetSwap pool for DBX is empty (reserves 1/0)". `parse_pool_json` is a free function on a JSON node and is directly testable. **Do NOT relax the reserve guard itself** -- it is load-bearing against the division by zero, and relaxing it recovers no data because `/pair/{id}` returns the same 1/0.
  - **(c) `tibetswap.enabled: false` -- optional, and weaker than it looked.** The client already degrades gracefully (skips absent assets, keeps cached reserves, and the AMM freshness taper decays the leg out of the blend). Disabling saves ~1,440 futile polls a day and the warning spam, at the cost of not auto-recovering if the pools refill. Needs an engine restart; do not edit `config.yaml` while the GUI is running.
- **The fee in the quoting floor: RESOLVED, leave it alone.** The rationale is real and was found in the introducing commit `679a934` (2026-04-04), verbatim: *"The TibetSwap fee creates a natural arbitrage boundary - any offer tighter than ~70 bps can be profitably arbed."* So the term is not arbitrary. Two caveats worth recording: it was never reviewed (no design note, review or strategy doc discusses the quoting use), and its scope was silently widened -- in `679a934` it floored only the competitive cap, not the whole ladder.
  - **But removing it would be right-for-the-wrong-reason and wrong in practice.** With the pools drained the stated arb route does not exist -- and adverse selection independently justifies a floor of about that size. Post-fill markout at h=36 blocks, signed so positive = moved against us, each against a matched placebo: XCH/DBX **+96.8 bps** (n=89, placebo +1.1), XCH/wUSDC.b **+85.4** (n=611, placebo -0.7), XCH/BYC **+244.0** (n=431, placebo +2.0). On the only well-powered pair the sign flip sits between **35 and 70 bps captured**, and expected value per posted offer is most NEGATIVE below 35 bps. More volume at a thin margin is the losing regime.
  - **XCH/DBX cannot decide this on its own data:** 9 fills, per-fill SD 182 bps, ~103 fills needed for 80% power -- about a year at the current rate. And 6 of the 7 recent DBX fills have a post-fill mid identical to the fill-time mid, because the DBX mid genuinely freezes (consecutive identical readings p50=7, max=2365). The strong result is imported from wUSDC.b and BYC.
  - **RESOLVED 2026-09-01 toward PICKOFF, and the three markout magnitudes are RETRACTED.** The fork was whether post-fill drift is informed pickoff or our own mid being mislocated at post time. Matched-sampling puts the pickoff share at **>=0.65** (1.14 on observations where the third-party book genuinely brackets the horizon) with fill-minus-placebo book drift **+53.1 bps [30.5, 75.5]**. The mislocation reading was a sampling artifact: the third-party book series is reconstructed only from post-time captures, median inter-observation gap 92-200 blocks against a horizon of 36, so `near(t)` and `near(t+36)` snap to the same observation and force the book leg to zero on 55% of rows while the total is measured on the dense snapshot grid. The residual absorbed the difference by construction. **So the floor is NOT treating a symptom and S38's do-nothing recommendation stands on the surviving branch.**
  - **RETRACT the magnitudes +96.8 / +85.4 / +244.0 bps.** Two independent analysts failed to reproduce them across ~11 constructions; best reproductions were 30.4/38.7/160.6 and 67.8/66.0/219.8. **Keep the sign, the cross-pair ordering (BYC >> wUSDC.b ~ DBX) and the placebo separation** -- those are robust, and the direction of the conclusion is unaffected. Also record what the magnitudes hid: **median markout is ~0 on every pair**, 30.4% (DBX) and 44.2% (BYC) of observations are structurally zero because the post-fill mid is byte-identical to the fill mid, and XCH/BYC's tail is 26 of 457 fills over just **10 distinct blocks**, all on 2026-08-22/23. This is a tail risk concentrated in a few episodes, not a per-fill drag.
  - **QUALIFY "the sign flip sits between 35 and 70 bps captured":** direction only. It rests on the retracted magnitudes and on a distribution whose median is zero. Do not quote 35-70 bps as a calibrated threshold.
  - **The April evidence was overstated.** The claimed "258-455 bps below the book mid while 7-205 above our own" needs 3 of 10 rows discarded; the true ranges are -455.5..+199.0 and -218.1..+407.2. The April pathology is real for 7 of 8 asks but the band is not, and at-touch does not select it. **Also: 61.9% of April `offer_log` rows carry `competitiveness_score = 0` as a migration backfill (0.0% in May/Jun/Jul/Sep) -- verified. Exclude April from every competitiveness analysis.**
  - **Still open, and one change unblocks it:** `resolved_book_best_bid/ask` is not bound in `kUpdateOfferStatus` (`database.cpp:345`), which is what forces the 92-200 block interpolation gap. Binding it -- a one-line-class change at a site that already runs on every fill -- plus two weeks of fills is the only thing that moves the magnitude from unusable to decision-grade.
- **Not testable today:** the floor expression is reachable only through the running binary, and `cpp/tests/test_fair_value.cpp:1219-1224` DUPLICATES it rather than binding to it -- that helper would go red on any edit without ever having covered the Step 8 behaviour. See S36.

### S39: Both fill-rate PIDs are saturated at their rails, and one has silently switched off a Step 8 gate
- **Files:** `cpp/src/engine.cpp:5089-5142` (spread PID), `:5146-5179` (competitiveness PID tick), `:10577-10600` (the Step 8 gate it drives); `cpp/include/xop/engine.hpp:1668-1676` (`SpreadPidState`); `cpp/include/xop/strategy/competitiveness_pid.hpp`; `config.yaml:210-228`; `cpp/include/xop/config.hpp:1202-1281`
- **Measured 2026-09-01 over the live log rotation, 2026-08-30T23:50 -> 2026-09-01T15:37 (~39.8 h).** Scope: XCH/DBX is effectively the only pair quoting in this window (21,251 Step 5 entries against 65 for XCH/BYC), so every number below is XCH/DBX.

| controller | variable | value | share | meaning |
| --- | --- | --- | --- | --- |
| spread PID | half-spread multiplier | `mult=0.820` | 4,935 / 5,287 (93.3%) | the most it can tighten |
| competitiveness PID | Step 8 score offset | `offset=-3` | 5,147 / 5,287 (97.4%) | gate driven to `effective=0` |

  The remaining spread-PID observations are 272 warm-up ticks at `1.000` plus the decay path between the two. **The minimum multiplier ever observed is 0.820** -- it has never gone lower, in any rotation.
- **Neither value is a coincidence; both are the analytic maximum, so the controllers have no authority left.**
  - Spread PID: total authority is `kp*target + ki*integral_max` = `0.8*0.10 + 0.05*2.0` = **0.18**, so `mult` bottoms at `1 - 0.18 = 0.820` -- exactly the logged value.
  - **The derivative term does NOT extend that bound, and writing `+ kd*ema_alpha` into the authority is a mistake this entry made in its first revision.** Substituting the EMA recursion, `d(output)/d(ema_prev) = kd*alpha - kp*(1-alpha)` = `0.004 - 0.784` = **-0.780**. The coefficient is NEGATIVE: raising the EMA to earn a positive d-term costs 195x more in the p-term than it gains. The maximum is attained at `ema = 0`, where the d-term is zero. So the reachable floor is 0.820 exactly, not 0.816 -- confirmed by 5,287 logged ticks in which 0.816-0.819 never appear and 0.820 is the minimum ever observed.
  - **`pid_min_mult: 0.7` (`config.yaml:216`) is unreachable dead config**, short of the floor by 0.120. The gains cannot produce an output large enough to approach it. It is set explicitly in `config.yaml`, so an operator would reasonably believe it is a live knob; it is not, and no test binds it.
  - **The `config.hpp` DEFAULTS carry the same bug**, which matters more than the shipped value: `pid_target_fill_rate{0.10}` with `pid_integral_max{2.0}` gives the same 0.18 authority against `pid_min_mult{0.70}`. A deployment with no `pid_*` keys at all is equally broken, and so is `config.example.yaml` (which has no `pid_*` keys). Any fix must target the defaults, not just the shipped file -- and any BOOT-TIME refusal would therefore also reject a bare config and turn `test_config.cpp`'s `kMinimalValidYaml` red. **This must be a non-throwing advisory, not a `ConfigError`.**
  - Competitiveness PID: `config.yaml:220` sets `comp_pid_target_fill_rate: 0.15`, **three times the `0.05` default at `config.hpp:1263`**. That gives `8.0*0.15 + 0.5*4.0` = **3.20**, raw offset `-3.20`, which **ROUNDS** to `-3`. Note carefully: `-3` is reached by the round-half-away-from-zero step, NOT by the clamp -- `clamp(-3, -3, 3)` is the identity here and `comp_pid_min_offset` never fires. Widening it to -5 or -10 would change nothing; the floor is the gain budget, not the clamp. (`comp_pid_warmup_blocks` diverges the same way: 5 in the YAML, 50 in the header. And `pid_integral_max` is absent from `config.yaml` entirely, so the 2.0 default binds while every sibling knob is explicit -- easy to misread as tuned.)
- **The consequence nobody signed off on:** `kBaseCompetitivenessScore` is 3 (1 for stablecoin pairs, **`engine.cpp:10669`**); `3 + (-3)` clamps to **`effective=0`**, i.e. the Step 8 competitiveness gate admits every tier regardless of score.
  - **CORRECTION 2026-09-01 -- "fully open for the entire retained window" was FALSE, and the real shape is worse.** The gate is above 0 on **2.69% of ticks (133/4,947)**, and the reason is that `CompetitivenessPid` state is **in-memory with no persistence**: every restart replays 5 warm-up ticks at the full configured gate of 3, then ramps 2->1->0 over ~23 ticks. There were six restarts on 2026-08-31 alone. **The configured gate is in force for roughly the first 20 minutes of each run and never again.** That is a different bug from "dead config text" -- the gate is not a constant, it is a sawtooth keyed to process lifetime, which also means gate behaviour is not reproducible across restarts.
  - **`kBaseCompetitivenessScore` is NOT a config key** -- verified, `grep -c competitiveness config.yaml` returns 0. S39(b)'s "set the base to 0" is a source edit, not a config edit.
- **A saturated controller is a constant with extra steps.** Neither PID has been *controlling* anything in this window; both are fixed offsets that happen to be recomputed every heartbeat. Nothing reports this, which is why it went unnoticed.
- **Do NOT add a third PID on the quoting floor.** This was proposed and rejected on 2026-09-01, for two independent reasons:
  1. **The floor is mostly not the binding width term.** Over 5,289 observations `min_half_spread == 70` exactly only **20.9%** of the time; the `quote_width_sigma_mult * combined_sigma` term is larger **79.1%** of the time (median 118 bps, p90 141, max 171; it is never below 70). A controller on the floor would be inert four times out of five on the only pair that trades.
  2. **Fill rate is the wrong setpoint, and it is the setpoint both existing PIDs use.** Post-fill markout against matched placebos is +96.8 bps on XCH/DBX (n=89, placebo +1.1), +85.4 on XCH/wUSDC.b (n=611, placebo -0.7), +244.0 on XCH/BYC (n=431, placebo +2.0), and expected value per posted offer is most negative below 35 bps captured (see S38). A controller that chases fills will find them by buying them at a loss. The width floor is currently the only thing standing between two railed fill-rate controllers and the adverse-selection zone; a PID on the floor would hand them what they are asking for.
  - The defensible setpoint is **markout-adjusted edge per fill**, not fill count -- but that loop cannot be closed here: XCH/DBX yields ~3 fills/month, markout needs ~11 minutes to resolve, and the per-fill SD is 182 bps. Loop latency is months against a noise floor that swamps the signal. This is a measurement problem, not a controller-tuning problem.
- **Status:** `[ ]` -- OPEN. Actions in value order:
  - **(a) Report saturation.** Emit a warning (and a GUI/telemetry field) when either controller sits at a bound for N consecutive blocks. Cheapest item here and the one that would have surfaced this months ago. A railed controller should be loud, not silent.
  - **(b) The competitiveness gate: MEASURED 2026-09-01, and it does not matter on the live configuration.** Of XCH/DBX offers scoring below the configured base of 3, **5 of 1,284 filled = 0.39%**, against 71/2,866 = 2.48% for score>=3 (April excluded as a backfill; verified from `offer_log`). **XCH/DBX is the only pair with `enabled: true`.** Re-closing the gate would delete 1,284 postings to avoid an exposure realised five times in four months. **Do not close it.** But record the counter-fact, because it inverts: on **XCH/BYC low-score offers fill at 95/830 = 11.45%**, statistically indistinguishable from its own score>=3 rate of 10.86%. **This decision is pair-specific and MUST be re-taken if XCH/BYC is re-enabled.** The honest change is therefore not to close the gate but to stop the sawtooth: either set the base deliberately in source, or bound `comp_pid_min_offset` to -2 (which binds through `effective_comp_pid_min_offset = max(configured, reachable)`).
  - **Do NOT "fix" this by lowering `comp_pid_target_fill_rate`.** It contradicts this entry's own arithmetic: the steady-state offset is `-round(kp*T + ki*integral_max)`, and the saturated integral contributes 2.0 independent of T. Dropping T to 0.05 moves the rail one step to -2 while making escape HARDER (needs ema > 0.1125 versus today's 0.0875). The lever is `ki * integral_max`.
  - **(c) Derive or delete `pid_min_mult`.** Either compute the reachable bound from the gains (the DERIVE-don't-validate pattern already used for `effective_agree_max_spread_bps` and `offer_absurdity_ratio`) or remove the knob. A configuration value that cannot be reached is a lie told to the next operator.
  - **(d) Reconcile `config.yaml` against `config.hpp` defaults** for the whole PID block, and add a test that fails when a shipped YAML value diverges from its documented default without comment. Three divergences found in one block.
- **Testable today, unusually for this area.** `CompetitivenessPid` is already a pure header with its own gtest (`cpp/tests/test_competitiveness_pid.cpp`), so (a) and (c) can be driven directly without constructing an `Engine` -- which nothing in `cpp/tests` does (S36). The saturation predicate and the reachable-bound calculation are both pure functions of the config struct.

### S40: the crossed-book taker computes a size cap and then throws it away  [P0 -- real money]
- **Files:** `cpp/src/engine.cpp:11453` (the cap), `:11458` (its only use), `:11515` (the call), `cpp/include/xop/rpc/chia_rpc.hpp:499` (the signature), `cpp/src/engine.cpp:11842-11844` (the correct pattern, 200 lines below), `config.yaml:295`
- **Verified in source at HEAD, 2026-09-01.** Step 9c computes
  `const Mojo take_size = std::min(best_ask_size, max_take_mojos);` at `:11453`. That value reaches **only the log line at `:11458` and the accounting call**. The actual execution is
  `co_await wallet_->take_offer(offer_status.offer.offer_bech32, fee);`
  and the signature is `take_offer(const std::string& offer_text, std::uint64_t fee = 0)` -- **there is no size parameter, and Chia offers are atomic with no partial fill.** The engine takes whatever the counterparty posted.
- **`arbitrage.crossed_book_max_take_xch: 5.0` is therefore INERT on the 9c path.** There is currently no bound on crossed-book exposure.
- **The only thing that has prevented an over-cap take is that we are broke.** Measured across the live rotation: **208 attempts logged `size=5000000000000`** -- exactly the 5.0 XCH cap, meaning the offer was LARGER and got clamped in the log only -- and **3 attempts below it**. There has been exactly **one success**: `2026-08-31T13:47:43 Step 9c: XCH/DBX TOOK ... edge=14.4bps size=1289726060241`, i.e. the sub-cap one. Every attempt at the cap failed on funding (186 `insufficient` error lines). **Funding the DBX wallet without fixing this unmasks the bug** -- the two must land together, in that order.
- **It also poisons the books -- structurally, though not yet in fact.** `engine.cpp:11538` passes the CLAMPED `take_size` to `record_taker_fill`, which then derives the quote leg as `quote_mojos_for(base_mojos, price_mojos, ...)`. So both double-entry ledger legs are **synthesised from a size the RPC never saw**, and any take that consumed more than the cap would be recorded at the cap.
  - **RETRACTED 2026-09-01: "no corruption has occurred yet" was wrong, and it was asserted without querying the database.** The claim above -- 23 rows, all below cap, including "BYC/wUSDC.b 0.000" -- came from an XCH-equivalent aggregation that silently rounded the BYC/wUSDC.b rows to zero. Re-read read-only from `data/xop_trader.db`, `taker_fills WHERE strategy='crossed_book'` has 23 rows of which **nineteen are strictly below their pair's cap and FOUR are EXACTLY ON IT**:
    ```
    2026-08-06T21:38:01.800Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005  price=1001000000000
    2026-08-06T22:00:26.487Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005  price=1001000000000
    2026-08-06T22:23:07.326Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005  price=1001000000000
    2026-08-06T22:45:05.796Z  BYC/wUSDC.b  base_delta=5000  quote_delta=-5005  price=1001000000000
    ```
    BYC is a CAT, so `base_mojos_per_unit = 1000` and the 9c cap is `5.0 * 1000 = 5000` mojos exactly. **Landing exactly on the cap is the clamp signature** -- it is what `std::min(size, 5000)` returns for every counterparty offer of 5000 or more, and it is the same pattern the 208 at-cap XCH/DBX log lines show. The quote leg is derived from the base leg, so if the base was clamped both double-entry legs are wrong by the same construction. Logs older than 2026-08-31 have rotated away, so this **cannot now be settled from logs**; it needs the four counterparty offer ids adjudicated on chain or on Dexie.
  - **`[x]` ADJUDICATED 2026-09-01 against the Dexie API. The clamp DID corrupt the ledger, and here is the proof.** All four counterparty offers were fetched read-only from `https://api.dexie.space/v1/offers/{id}` -- the endpoint the bot itself uses (`dexie_client.cpp:966`). All four are **identical and `status: 4` (taken)**:
    ```
    5rFcwA36FKBp  date_found 2026-08-06T21:20:20Z  offered BYC 10  requested wUSDC.b 10.01
    DF2V2ycj2mmf  date_found 2026-08-06T21:39:39Z  offered BYC 10  requested wUSDC.b 10.01
    HN61MykqzzTd  date_found 2026-08-06T22:02:19Z  offered BYC 10  requested wUSDC.b 10.01
    6yHHwF8Mr1nr  date_found 2026-08-06T22:27:22Z  offered BYC 10  requested wUSDC.b 10.01
    ```
    BYC and wUSDC.b are both CATs, so 1,000 mojos/unit (`config.cpp:638-639`). **The offer was 10 BYC = 10,000 mojos. `taker_fills` records 5,000.** The ratio is exactly 0.500, which is `std::min(10000, 5000) / 10000` -- the clamp, not a coincidence and not a partial fill, because Chia takes are atomic.
  - **Quantified ledger error.** Per trade: base 10,000 actual vs 5,000 recorded (**5.000 BYC short**); quote 10,010 actual vs 5,005 recorded (**5.005 wUSDC.b short**, because the quote leg is derived from the clamped base). Across the four trades on 2026-08-06: **20.00 BYC received but never recorded, and 20.02 wUSDC.b paid but never recorded.**
  - **What this affects.** The per-unit price recorded is correct, so realised P&L *per unit* is right -- but the SIZE is half, so since 2026-08-06 the inventory model has believed it holds 20 BYC less than it does and has spent 20.02 wUSDC.b less than it did. That feeds the Avellaneda-Stoikov inventory skew, the risk limits and P&L attribution. **RETRACTED: `ledger_entries` DOES hold the legs.** That claim was mine and it was a failed join, not a missing row -- `event_type = 'take'` stores `event_id` PREFIXED with `take:` (e.g. `take:0x6bc1759d41...`) and I queried the raw trade ids. Matching on the prefix returns **three legs per trade** for all four, and they carry the clamped values exactly as the defect predicts:
    ```
    leg=base   asset=ae1536f567(BYC)      delta=  +5000   <- should be +10000
    leg=quote  asset=fa4a180ac3(wUSDC.b)  delta=  -5005   <- should be -10010
    leg=fee    asset=xch                  delta=  -5000   <- correct, not size-derived
    ```
    So the corruption propagated into the double-entry ledger precisely as expected, and there is no separate missing-legs defect. This is the SECOND time in this investigation that I reported an absence which was really a wrong key -- the first was applying a flat 5e12 cap to a CAT pair whose cap is 5,000. **Before recording any absence as a finding, verify the join key and the denomination.**
  - **`[x]` DO NOT correct the ledger legs. The books were ALREADY re-tied to the wallet, and my proposed remediation would have done harm.** The auto-adjust reconciler (`engine.cpp:15968-15995`, `auto_adjust_enabled: true`) caught the shortfall within two days, and these are the FIRST adjusts for each asset after 2026-08-06 (priors 07-30 and 07-31):
    ```
    2026-08-07T19:46:04Z  wUSDC.b  -20,054   predicted -20,020   residual 34 (0.17%)
    2026-08-08T10:06:13Z  BYC      +19,973   predicted +20,000   residual 27 (0.135%)
    ```
    Both signs correct, both magnitudes inside 0.2%, verified directly against the read-only DB. **Posting the four correcting entries today would move the ledger ~20,000 mojos AWAY from the wallet in each asset and immediately re-trip the invariant control.** I recommended those entries; the recommendation was wrong, and it was a database WRITE, so the error would have been active rather than merely inaccurate. It was caught only because the write was gated on approval.
  - **`[ ]` What remediation actually means, and it is optional:** correct the SIZE in `taker_fills` only (`base 5,000 -> 10,000`, `quote -5,005 -> -10,010`, four rows) so per-strategy P&L stops understating `crossed_book` by half. **Do NOT touch `ledger_entries` -- that would double-count against the two adjust rows.** Requires a DB write, operator approval, and is safest offline (single-writer sqlite). Also worth annotating the 08-07 and 08-08 adjusts as EXPLAINED rather than "unexplained divergence".
- **The fix already exists in this file.** Step 9d at `:11842-11844` does it correctly: *"The whole offer is consumed -- already validated against the cap by the size filter above."* **Filter oversized offers out; do not clamp a log field.** Mirror 9d.
- **`[x]` The cap is now honoured, by FILTERING (2026-09-01).** Step 9c's selection, cross test, edge test and size filter moved into the pure header `cpp/include/xop/execution/crossed_book.hpp`, driven by `cpp/tests/test_crossed_book.cpp`. An oversized cheapest ask makes us skip the pair, exactly as 9d does; we do not re-select a smaller ask.
- **`[x]` Two more sites of the same shape, found in review and fixed in the same pass:**
  - **Step 9e (peg arb), `take_peg`.** Same `std::min` clamp, and armed in live config (`peg_arb_enabled: true`, `peg_arb_max_take_units: 50`). **Worse than 9c**, because the clamped size fed the *pre-balance guard*, so for any offer larger than the cap the guard priced a fraction of the take, passed, and let `take_offer` lift the whole thing. It also treated a BID's QUOTE-denominated size as base mojos in the cap comparison, the funding estimate and the ledger. Both fixed; the unit maths now lives in `cpp/include/xop/execution/take_sizing.hpp` with `cpp/tests/test_take_sizing.cpp`, shared with 9f.
  - **Step 9 XCH recovery taker.** `recovery.max_take_per_block_xch` **never bounded a take at all** -- the running total was tested at the top of the candidate loop, where it is structurally zero, and incremented only after `take_offer` had already settled, immediately before an unconditional `break`. `cand.size` was never compared to the budget anywhere. Now an integer budget with 9d's filter on each candidate. Dormant today only because the one pair that can supply a `cex_ref` is disabled.
- **`[x]` PRE-TRADE BALANCE CHECK (2026-09-01).** Step 9c now prices the take in QUOTE mojos and compares it to `spendable_balance` **before** `get_offer_status()`, so a decline costs zero RPCs instead of two. Policy in `cpp/include/xop/execution/take_retry.hpp`, driven by `cpp/tests/test_take_retry.cpp`. A failed balance read is `Unknown` and DECLINES -- it is not `bal.value("spendable_balance", 0)`, which substitutes a number for a failed read (the S41 family). Measured counterfactual: all 50 Cgp9 attempts and all 96 funding failures in the 39h window are declined at zero RPC cost, because DBX spendable never left 111,214..437,875 against a 485,908 requirement.
- **`[~]` BACKOFF -- three classes, three schedules, deliberately not one.** The distinction is the whole design: a funding failure on an unchanged offer is DETERMINISTIC (`take_offer` has no size parameter, Chia offers are atomic, so the only mutable input is our balance) while a sync rejection is TRANSIENT (92 episodes in 35.2h, 87 of them one cycle long).
  - *Deterministic:* balance-keyed and self-invalidating -- no timer at all. The cycle our balance covers the cost, we take, with zero hold-off.
  - *RPC-proven funding* (the read said affordable and the wallet still said no -- our own just-posted offers lock whole UTXOs for tens of seconds): its own enumerator and its own log line, held 8 blocks on first contact, doubling to 192, decaying when the wallet stops rejecting.
  - *Transient:* its own short schedule, 2 blocks doubling to 32. **Not zero**: the first cut left this class unbounded on the argument that a thrown `get_wallet_balance` would catch it, and the logs refute that -- **zero** balance-read failures in 39h; every "fully synced" line comes from `get_spendable_coins`, `create_offer` or `selective_cancel`. Unbounded, a desync that does not clear reproduces the storm verbatim on this class.
  - *Unmodelled:* 4 blocks doubling to 64. All three counters **decay** once the fault stops recurring, so `consecutive` means consecutive -- without that, five isolated transport blips 210 blocks apart escalated a fully funded 235 bps cross to the 20-minute ceiling.
- **`[x]` BLACKLIST -- REFUSED, on evidence, on both candidate keys.** From `taker_fills` (607 rows, read-only): two counterparty offer ids were each taken **three** times at three distinct heights, so an id-keyed blacklist armed on first failure would have blocked **4 of our 23** profitable `crossed_book` fills. Grouping by content instead -- (pair, price, size, side) -- shows the counterparty re-posting IDENTICAL content under FRESH ids (BYC/wUSDC.b 5000 @ 1001000000000, four fills under four ids in 67 min), so a content hash would have blocked **five more**. Neither key is sound, and a balance comparison needs no offer identity at all. The offer id is a map key for log suppression only; every entry carries a (price, size) fingerprint so a re-post resets it.
- **`[x]` And the log volume, which is the reason any of this is legible.** The step emitted 3 info/error lines per cycle per doomed offer (150 in the Cgp9 window alone, ~94% of Step 9c's entire output). It now emits an EDGE plus a heartbeat: one line when a suppression starts, one per hour while it lasts, one when it clears. The `CROSSED BOOK detected` line was demoted to debug -- it sat 140 lines ABOVE the gate, so leaving it at info would have left 50 of the 51 remaining lines in place and the gate could not have suppressed a line already printed.
- **Status:** `[x]` -- CLOSED for the taker path. The P0 half (the inert cap, on all three paths) is closed, the four ledger rows are ADJUDICATED (corruption confirmed and quantified), and the retry/backoff/blacklist half is now closed as above. **Outstanding and tracked separately:** the four remediation entries and the missing `ledger_entries` rows (see the two `[ ]` bullets above). Still unguarded by any test, as ever: the Step 9c/9e/9f CALL SITES, because nothing in `cpp/tests` can construct an `Engine` (S36) -- delete the wiring and the suite stays green.

### S41: `get_spendable_coins` fails OPEN, and the handler written for it is unreachable
- **Files:** `cpp/src/execution/coin_manager.cpp:163-166`, `:373-374`, `cpp/src/engine.cpp:17306-17310`
- **Verified 2026-09-01.** The body is
  `} catch (const rpc::ChiaRPCError& e) { logger_->error(...); }` followed by `co_return coins;`
  -- an **empty vector on RPC failure, indistinguishable from a legitimately empty wallet.** This is the eleventh member of the documented `close_out` fail-open family and has exactly its shape: an error path that returns the same value as a valid negative answer.
- **Consequence, observed:** it fabricates `ensure_split`'s *"Have 0 mojos total"* while `get_wallet_balance` answered successfully in the same second. The same empty-on-error return also feeds `count_free_coins`, `count_pool_ready_coins` and `ensure_split` -- i.e. **every coin-pool sizing decision.**
- **The handler at `engine.cpp:17306-17310` has 0 occurrences in 37.4h** against 32 `ensure_split` errors and 64 `get_spendable_coins` errors: it is unreachable for the RPC-failure case it was written for, because the failure was already swallowed upstream.
- **`[x]` FIXED 2026-09-01.** `get_spendable_coins` and `count_free_coins` return `std::optional`; the engine's coin-pool branches read `read_ok` from `has_value()` and map unknown to Skip through the pure header `cpp/include/xop/execution/coin_pool_verdict.hpp`. `get_balance_xch()`/`get_balance_mojos()` were deleted rather than ported -- both had zero callers and both failed open the same way.
- **`[x]` And the guard itself is now guarded.** The first attempt pinned only the return TYPE (two `static_assert`s) and the POLICY (the pure verdict header). A review reinstated the original defect -- `co_return std::vector<CoinInfo>{};` inside the `ChiaRPCError` catch -- and **all 1109 tests stayed green**, because a `static_assert` cannot see into a function body and the bug was a VALUE, not a type. `CoinManager::collect_spendable_coins()` now takes the wallet call as a parameter, so `cpp/tests/test_coin_pool_verdict.cpp` drives a throwing fetch and a malformed record and observes the returned value. Mutation-checked: restoring the fail-open in either catch now turns a test red.
- **`[ ]` Still unguarded, recorded rather than glossed:** nothing pins the ENGINE's wiring -- that it sets `read_ok` from `has_value()` and not from a `value_or`. That needs a constructible `Engine` (S36).
- **Log-string rename, for anyone re-measuring the before/after counts.** `"XCH split failed (insufficient balance or RPC error)"` became `"XCH split failed (insufficient balance or no improving split)"` (same change in the CAT branch), and the old signal is now split across three messages: `no improving split`, `split abandoned`, `free-coin read FAILED`. Grep for **both** wordings. Separately, Step 9c's detection line gained a ` cap=` field and its `size=` now prints the TRUE counterparty size rather than the clamped one.
- **Status:** `[x]` -- FIXED, minus the engine-wiring test noted above. Added to the fail-open family ledger as member eleven.

### S42: the published mid is the best BID in 98.2% of clamp firings, and no GUI surface would show it
- **Files:** `cpp/src/execution/orderbook_mid.cpp:224-228`; `cpp/src/monitoring/metrics.cpp:719`; `cpp/src/engine.cpp:2484`/`:2582`; `gui/widgets/bot_log.py:313`; `gui/widgets/market_analysis.py:184`
- **Measured 2026-09-01 by parsing the live rotation.** The `INVARIANT CLAMP` guard fired **2,026 times in ~37h**, and of those **1,990 (98.2%) were the blended mid landing BELOW the best bid**, versus 36 (1.8%) above the ask. The clamp then sets `mid = best_bid`. On an 8 bps book with `w_micro=1.000` the micro-price carries full weight, so the Layer-2 taper cannot damp it. **A "mid" that is the bid is biased low by at least half the spread, systematically, on the only pair we quote.** The guard is doing its job; the estimator feeding it is not.
- **Do NOT justify a change here with the volatility argument.** The claim that the mid freeze deflates measured vol by 3.00x is **retracted**: 3.00 is the arithmetic identity `1/sqrt(1 - s)` at `s = 0.889` (predicted 2.998), i.e. it restates the stale fraction and is not evidence of anything. The variance ratio (0.145 at h=200) says short-horizon sigma is if anything already too HIGH. **Do not change `market_data.cpp:1732` on that basis.**
- **Related, and it invalidates a recommendation made earlier in S39(a):** there is currently **no working GUI route for live per-pair strategy state.**
  - `gui/widgets/bot_log.py:313 append_log()` has **zero callers repo-wide** (verified). The Bot Log tab, its filters, its regex search and its `error_detected` signal are fed by nothing.
  - Both `metrics_->update_analysis(...)` call sites (`engine.cpp:2484`, `:2582`) are inside `Engine::run_startup_analysis()`, so the analysis gauges are published **once at startup and never again**.
  - The GUI's **"Spread Multiplier"** row (`market_analysis.py:184`) reads `xop_analysis{metric="recommended_spread_multiplier"}` -- a frozen `MarketAnalyzer` startup *recommendation*. The live PID multiplier (`current_mult`) is **exported nowhere at all** (0 hits in `metrics.cpp`/`metrics.hpp`). An operator asking the GUI what multiplier the bot is using has been getting a number unrelated to the answer while the real one sat railed at 0.820.
- **Status:** `[ ]` -- OPEN. Three separable tickets: the micro-price bound, the dead Bot Log tab, and exporting live PID state. Until the third exists, `spdlog::warn` is the only route that reaches a human -- which is why S38(a)'s logging-sink fix mattered more than it looked.

### S43: the micro-price is not a micro-price, and the test that should have caught it is vacuous
- **Files:** `cpp/src/execution/orderbook_mid.cpp:191-192` (the defect), `:216-228` (the clamp, which is CORRECT and stays), `cpp/tests/test_orderbook_mid.cpp:98` (the vacuous test), `:135-164` (two assertions that encode the bug), `cpp/include/xop/execution/orderbook_mid.hpp:22-27` and `cpp/include/xop/config.hpp:602-605` (the same wrong conclusion, twice)
- **ROOT CAUSE, and it is a substitution nobody flagged.** A Stoikov micro-price is `(Q_ask*B + Q_bid*A)/(Q_bid+Q_ask)` -- touch PRICES weighted by sizes -- and is bounded in `[B,A]` because it is a **convex combination of B and A**. This implementation substituted each side's top-5 **VWAP** for the touch price: `micro = (ask_depth*bid_vwap + bid_depth*ask_vwap)/(bid_depth+ask_depth)`. A convex combination of `V_b` and `V_a` is bounded in `[V_b, V_a]`, and since `V_b <= B` and `V_a >= A` on any book with more than one level a side, that interval **strictly contains** `[B,A]`. **Boundedness was discarded at the moment of substitution.** The depth substitution (top-N cumulative rather than touch size) is fine and should be kept; only the PRICE substitution is the defect.
  - With `d_b = B - V_b`, `d_a = V_a - A`, `S = A - B`: `micro < B` iff `D_a/D_b > (S + d_a)/d_b`. **`S` appears additively in the NUMERATOR -- a NARROW spread makes the violation EASIER.** That is why the Layer-2 taper, which keys on spread alone, is fully off at 95.3% of firings, and why the estimator's good behaviour on wide books (XCH/BYC) said nothing about XCH/DBX.
- **Measured over ~39h:** 1,849 clamp firings, **100% XCH/DBX** (the only enabled pair), 98.38% below the bid, binding on **46.9% of ingests**. Raw excursion p50 **12.29 bps against a modal 8 bps spread** -- 1.54x the entire spread; 82.1% of firings exceed a full spread; max 494 bps. Depth ratio at firing p50 2.85 but **min 0.46**, so it fires at below-parity depth: this is not an extreme-asymmetry edge case.
- **Consequence, honestly sized.** XCH/DBX has no CEX leg, so the published mid IS the clamped orderbook mid, and the bias against the true BBO midpoint is exactly `spread/2`: p50 4.0 bps, **p90 61.0, p95 95.5, max 160.5**; 18.6% exceed 25 bps. The modal 4 bps is noise; the tail is not -- against `min_profit_margin_bps: 50` and a 45 bps tier stride, a 61-96 bps centre error exceeds a whole tier. The only material consumer is the Step 7 ladder centre (`engine.cpp:6280`), which centres tiers symmetrically, so a low centre means **bids less aggressive and asks MORE aggressive -- a systematic sell-side skew on the only earning pair.** `bid_cap`/`bbo_ref` is insulated (it recomputes the true BBO midpoint); the competitiveness scorer and `compute_queue_ahead_mojos` read the raw BBO and are unaffected. **Do NOT attribute the 0.820 PID rail to this** -- the PID's input is a binary fill signal.
- **THE TEST IS VACUOUS, which is why this survived.** `test_orderbook_mid.cpp:98 HoldsAcrossEveryDepthRatioAndSpread` sweeps 7 ratios x 8 spreads and asserts only `mid >= best_bid && mid <= best_ask` -- **which the clamp guarantees unconditionally.** It could never fail. This is the exact failure mode the repo's mutation rule exists for.
- **Fix (4 lines):** use the touch prices, `micro = (ask_depth*best_bid + bid_depth*best_ask)/(bid_depth+ask_depth)`. Keep the VWAPs for the usability gate and the clamp diagnostic. Bounded by construction at every depth ratio and spread, so Layer 1 becomes provably non-binding on a two-sided book and is restored to the defect alarm its own comment claims it is. Loses nothing intended: `D_a/D_b -> inf` gives B, `-> 0` gives A, parity gives the midpoint, still leaning to the thin side continuously. Also removes a free manipulation surface -- today, splitting one bid into ten at 4 bps steps moves our published mid at zero cost.
  - "Bound each side's VWAP to the touch before weighting" is **algebraically identical**, since `clamp(V_b,B,A) == B` and `clamp(V_a,B,A) == A` unconditionally. Write it in touch-price form so nobody later restores the VWAP believing it did work.
  - **Pin it:** add `ASSERT_FALSE(r.clamped)` inside the existing sweep -- it must FAIL on today's code at essentially every ratio != 1, and pass at all 56 grid points after. If it passes before the fix, the grid has no real `d_b` and must be rebuilt. Add a regression case from the live modal state (bid 84.502400, ask 84.570143, depths 2048/5844) asserting `!clamped`, strict interiority, **and `mid < midpoint`** -- that last one blocks "fixing" this by returning the plain midpoint.
  - **Expect two existing assertions to invert:** `TheBycCaseCannotProduceAMidAboveTheAsk:163-164` encodes the defective behaviour. Under the fix that book yields mid ~1.0626, strictly interior and ~115 bps CLOSER to the ~$1.01 truth than today's 1.0750. Say so in the commit or the next reviewer reads it as a regression.
- **Status:** `[ ]` -- OPEN. Highest-value quoting fix on the branch. **Deploy alone; do not co-deploy with any tuning change** -- the clamp count falling from 46.9% to ~0 is the only acceptance metric available, and moving two things makes it unreadable.

### S44: a take was booked into the ledger that the chain never settled, and the reconciler hid it
- **Files:** `cpp/src/engine.cpp:14982` and `:15001` (`record_taker_fill`), `:15968-15995` (the auto-adjust reconciler)
- **The instance, verified leg by leg.** Take `0x190884904a4b...` was booked at `2026-08-30T15:30:11` as `base xch +2,000,000,000,000`, `quote BYC -2,909`, `fee xch -3,513,303`. At `17:16:59` -- 1h47m later -- the reconciler posted `xch -2,000,000,000,000` and `BYC +2,909`: an **exact reversal of the base and quote legs**, with the fee correctly not reversed because the fee was really spent. That is a take we recorded as settled which the chain never settled, silently unwound as "unexplained divergence".
- **Why it matters more than the one row.** The adjust series is the control that re-tied the books after the S40 clamp -- but it labels everything "unexplained divergence", so it **masks** the thing it corrects. It hid the 08-06 half-booking for four weeks and it is currently hiding at least this one live discrepancy. A reconciler that silently absorbs booking errors converts a loud bug into a quiet one.
- **A REAL FAIL-OPEN in the same function, never yet fired.** `engine.cpp:14982`, inside `record_taker_fill`'s `add` lambda: `if (!ledger_opened_assets_.count(asset)) return;` -- silent, no log, no flag. Then `:15001`: `if (!legs.empty() && !db_->append_ledger_entries(legs)) { ledger_incomplete_ = true; }`. **If every asset on a take is unopened, `legs` is empty, the `&&` short-circuits, a `taker_fills` row is written with ZERO ledger rows, nothing is logged, and `ledger_incomplete_` stays false while the invariant control asserts the books are sound.** The partial case (base opened, quote not) yields a permanently unbalanced two-leg event that still reports success. This is the `close_out` family shape exactly: **an empty collection treated as success.** It has never fired (26/26 engine-written takes carry exactly 3 legs) and is untestable today (S36). The maker-fill twin at `:14867` at least emits a `spdlog::warn`.
- **Status:** `[ ]` -- OPEN. Two separable pieces: (a) log and flag at `:14982`, and drop the `!legs.empty() &&` short-circuit at `:15001` so an all-skipped or partial take sets `ledger_incomplete_` -- ~10 lines, no quoting blast radius, but confirm what that flag gates before deploying; (b) investigate the 08-30 unwound take and make the reconciler distinguish "a booking we know is wrong" from "unexplained".


## Permuto contest, 2026-09-01 -- the account was liquidated, and what that taught

**The headline is not a code finding.** The contest account
(`b3edfaa8...9001`) reached **equity 0, balance 0, position 0** on
2026-09-01, with `realized_pnl -1,218,420` and `total_pnl` floored at
**-500,000** -- the entire starting allocation. Depth froze permanently at
**4,093.892** against a 300,000,000 gate. Only 3 of 41 market makers were
in that state and median MM equity was still exactly 500,000, so this was
NOT a venue-wide reset like the 2026-08-30 `batch_failed` incident.

Last observable state before the log died (2026-08-31 19:55, GUI still
running): the venue refusing every batch with *"Carried-session stress
margin: need 5,394,844 USDC to survive 8x index move (available
591,782)"* -- the carried short needed ~9x the cash on hand. The oracle
then gapped **+73% to +229%** at the open, which is the direction that
destroys a short.

`risk.assess()` returns FLATTEN on `not (equity_usd > 0.0)`, so with
equity at zero no config, mode or PR produces a single quote. Depth
cannot restart without the account being re-funded.

**A second account exists on another machine.** The work below is what
has to be true before it runs.

### S45: every crossed-book TAKE is invisible in the GUI, because the offers view is maker-only
- **Files:** `gui/widgets/order_panel.py:112` and `:930` (the `status == "filled"` filter), `gui/services/database_service.py:330` and `:1089` (the same aggregate), `cpp/src/engine.cpp` `record_taker_fill` (writes `taker_fills`, never `offer_log`)
- **Reported by the operator 2026-09-02:** an offer was accepted at 06:19 CDT and did not appear in the list of filled offers. **The data is fine; the view is maker-only.** Traced end to end:
  - `taker_fills` id=608, `taken_at 2026-09-02T11:19:36.062Z` (= 06:19:36 CDT), pair XCH/DBX, counterparty offer `79Yxe3DK4PaMKcvziiMgjGVtbjasW4ucVQTgB7DeYjP5`.
  - Reconciles EXACTLY against the Dexie API: that offer is `status: 4` (taken), `offered XCH 1`, `requested DBX 84.175`; we recorded `base_delta +1,000,000,000,000` (1 XCH), `quote_delta -84,175` (84.175 DBX), `price 84.175`, `fee 874,627`. All three `ledger_entries` legs are present (base, quote, fee).
  - **We were the TAKER.** We accepted someone else's offer; nobody accepted ours. `offer_log` holds only offers WE post, so a take correctly has no row there -- and the GUI's list filters exactly that table on `status == "filled"`. **`taker_fills` is read NOWHERE in `gui/`** (verified by grep across the whole package).
- **Scope: all 23 `crossed_book` takes ever executed are invisible in that view**, as are the `cross_stable`, `peg_arb` and recovery takes. For contrast, the last genuine MAKER fill was **2026-08-26 09:40:19** -- a week before the report -- so an operator watching that list has seen nothing for a week while the bot was trading.
- **Not a data-loss defect and not caused by anything in PR #134.** Nothing in this branch touches the GUI's offer list. Recording is complete; only the presentation is maker-shaped.
- **Third instance of the same theme.** S42 records that `append_log()` has zero callers and that the analysis gauges publish once at startup, so the GUI shows a frozen startup recommendation where an operator expects live state. The pattern is that the GUI reports the *maker quoting loop* and is silent about everything else the engine does.
- **Status:** `[ ]` -- OPEN, deferred to the next release by the operator on 2026-09-02.
  - **Preferred fix:** union `taker_fills` into the fills view with an explicit MAKER/TAKER column, so one list answers "what did we trade". A separate "takes" section is the cheaper alternative but leaves the operator correlating two lists by timestamp.
  - Carry the `strategy` column through (`crossed_book`, `cross_stable`, `peg_arb`, recovery) -- it is the only thing that says WHY a take happened, and the four paths have different risk profiles.
  - **Do not** synthesise `offer_log` rows for takes to make them appear. `offer_log` means "an offer we posted" and its `status`/`cancel_reason`/`competitiveness_score` columns are meaningless for a take; faking rows there would corrupt every fill-rate and competitiveness statistic that reads the table, including the 0.39% figure S39(b) rests on.
- **Related and NOT deferred:** the take path itself had an inert size cap (S40). Fixed on this branch but **not deployed** -- the running binary predates it. Today's take was 1 XCH against a 5 XCH cap so it was recorded truthfully; a larger one would not have been. That is a deployment argument, not a GUI one.

### S46: the shutdown cancel tried ONCE, hit a transient, and left seven offers live
- **Files:** `cpp/include/xop/execution/cancel_retry.hpp` (new, pure policy), `cpp/tests/test_cancel_retry.cpp`, `cpp/tests/test_startup_probe.cpp`, `cpp/src/engine.cpp` (`shutdown`, `watchdog_cancel_book`, the `startup_reconcile` call site), `cpp/src/execution/offer_manager.cpp` (`cancel_all`, new `cancel_ids`, `startup_reconcile`, `emergency_cancel`)
- **Observed 2026-09-02 13:10:17.** A planned graceful stop ran the whole shutdown path correctly and then logged five lines **in the same second**:
  - `Failed to cancel offer 0x9a72dfe913: Wallet needs to be fully synced before making transactions.`
  - `cancel_all: 0/7 offers cancelled successfully`
  - `[S31] graceful cancellation got 0/7 -- invoking the independent fallback`
  - `[S31] cancel FAILED: Wallet needs to be fully synced`
  - `[ALERT:CRITICAL] DEAD MAN'S SWITCH COULD NOT CANCEL`
- Seven offers were left live and unmanaged and the process **exited cleanly**. The engine tried exactly once; the "independent fallback" goes through the same wallet RPC, so it failed for the same reason in the same millisecond. **A net that fails identically to the thing it catches is not a second chance.** The condition was a flap -- the replacement engine was transacting again minutes later.
- The alert said "cancel them by hand NOW" **without naming a single offer**. The seven ids were recoverable only by querying `offer_log` afterwards.

**What this PR now fixes -- and only this.**
- **K1 -- the bounded retry.** `cancel_retry.hpp` is a pure policy (plain integers, no engine types, no clock -- elapsed time is an INPUT) driven by `test_cancel_retry.cpp`. The shutdown retries the **failed subset**, not the whole book: `OfferManager::cancel_ids` is new because the wallet's bulk endpoint takes no offer id, so "cancel just these three" was not expressible and re-running `cancel_all` would re-charge a fee against offers that already went through. `classify_take_failure` from `take_retry.hpp` is reused verbatim -- one classifier, one place -- and folded across the whole batch with `more_retryable`, because a single trailing funding refusal used to strip the retry ladder from six offers whose only problem was the flap. The ladder's STATE is `execution::CancelLadder`, moved out of `engine.cpp` for a measured reason: reinstating the original fail-open at the engine call site (`outstanding = oc.failed` → `outstanding.clear()`) left the entire suite green, because the four lines that decide WHAT IS STILL LIVE were the untested part and they are the whole bug.
- **K2 -- the actionable alert.** `watchdog_cancel_book` takes the still-live ids and every shutdown alert names them **in full** (not `substr(0,12)` -- a prefix cannot be pasted into a cancel command). An empty list is stated as an absence rather than omitted, so an operator can tell "no offers were live" from "this build forgot to pass them".
- **K3 -- the `startup_reconcile` DB→wallet leg.** This is the independently valuable half. The reconcile was **wallet→DB only**: `known_offer_ids` was a membership test used to label a wallet record known-vs-orphan, so a DB row the wallet never mentioned was invisible, and a `get_all_offers` failure took an early return logging `0 wallet offers scanned, 0 known/restored, 0 orphans` -- **verbatim what ran on 2026-09-02 with seven rows stranded, and it read as success.** That is the `close_out` fail-open shape. The leg now probes every DB-pending id the scan did not mention, and a **failed scan marks everything unverifiable rather than returning early**.

**What was ATTEMPTED AND WITHDRAWN, and why.** The fix grew to 2,893 insertions across 7 files; review findings went 5 → 17 → 23 across three passes. Each pass fixed real defects and introduced new ones. That is divergence, not convergence.
- **D1 -- the `cancel_pending` latch. The deciding finding, and a bad trade three ways.** (i) A latched offer is **excluded from the pending-exposure accumulator**, so a live, fillable offer contributed **zero** to the reserve guard -- the exact fail-open shape this PR exists to close, relocated to a new site. (ii) One permanently-latched offer **wedges the cancel-all posting gate forever**, halting quoting on every pair for the process lifetime. (iii) It removed the only thing that ever unstuck a submitted-but-unlanded cancel -- a process restart -- and the PR text presented that removal as a benefit. It traded a bounded, cents-scale duplicate fee for **unbounded live exposure**. Worse than the bug.
  - `State::cancel_pending` is **one-way**: `state.cpp` is its only mutator and it only sets. A repo-wide grep finds **no clearer and no timeout**. Nothing may latch it from a DB status or a startup probe. The one surviving `mark_cancel_pending` added by this PR (`cancel_ids`) fires only after a cancel **this process submitted and the wallet accepted** -- the same provenance `cancel_all` already had on `main`.
  - **The accepted cost, named:** `StartupDbLeg::pending_cancel` is still a bucket (PENDING_CANCEL is positive evidence, and collapsing it into `unverifiable` -- which means "we learned nothing" -- throws information away), and the engine **reports** those ids at warn level. If one is still resting past `offer_ttl_blocks` the TTL sweep may pay a **second secure cancel fee for a spend already in flight**. Bounded, cents-scale, and deliberately preferred over the latch. See S47.
- **D2 -- the durable intent file** (`data/uncancelled.txt`/`.json`, `write_cancel_intent`, `load_cancel_intent`, `sweep_cancel_intent`, `cancel_intent_`). It produced a **fresh fail-open on each of three passes** and is not needed by K1–K3. Withdrawn entirely; the underlying problem is S47.
- **D3 -- every aggregate timing figure and the per-leg table.** Three versions were published and **all three were wrong: 90 s, then 247 s, then 390.5 s.** The last confirm pass still found `close_connections()` blocking on `thread_pool_->join()` **unaccounted for**. The retry states **its own budget** (a property of the pure policy, proved at compile time by `cancel_last_effective_attempt_start_ms`) and nothing else.
  - **SHUTDOWN DURATION IS NOT ESTABLISHED.** If a reader wants to know how long shutdown can take, that is the honest answer. **The concrete lead for whoever picks it up: `close_connections()` joins the curl thread pools, and that join was never in any of the three figures.** The retracted numbers live here, in this entry, and deliberately **nowhere in the source** -- so a grep of `cpp/` finds no figure to mistake for a live claim.
- Also dropped: the Windows `kill_old_instances` change (F4). It belongs to the durability story, not to the retry, and it touches `main.cpp`, which cannot be linked in this environment.

**Where the withdrawn work lives.** Branch **`wip/s46-full-scope`, commit `b3ea169`** (pushed to origin, not deleted). The per-leg shutdown enumeration in particular is worth keeping. Nothing needs salvaging by hand -- `git show` / `git diff` against that ref recovers all of it.

**Coverage, stated exactly (S36: nothing in `cpp/tests` constructs an `Engine` or an `OfferManager`).** Pinned by tests: `plan_cancel_retry`, `CancelLadder`, `cancel_ids_admits`, `plan_startup_probe`, `classify_startup_probe`, `StartupDbLeg::add`. **NOT pinned, and each carries an explicit `[UNGUARDED -- HAND-REASONED]` note at its call site:** the retry loop's plumbing in `Engine::shutdown`, the deadline check inside `cancel_ids`, `emergency_cancel`'s `may_start()` ladder, the probe loop in `startup_reconcile`, and everything `engine.cpp` does with the resulting buckets. Do not let a report claim coverage S36 makes impossible.
- **Mutation-checked**, all four caught: revert to a single cancel attempt → 8 failures; classify a wallet refusal as terminal → **3 compile-time `static_assert` failures**; abandon the remainder on a partial success → 8 failures; make an empty/failed wallet read mean "nothing pending" → **`static_assert` failure, and independently a gtest failure when the compile-time guard is also deleted**.
- **Status:** `[x]` on `fix/dead-mans-switch-retry` -- K1, K2, K3 only. Not deployed; the running binary predates it.

### S47: nothing records that an offer was MEANT to be dead, and `cancel_pending` has no way out
The deferred half of S46, kept separate because S46's attempt at it (D2, the durable intent file) produced a fresh fail-open on each of three review passes. **Two distinct problems, both real, neither solved:**
- **(a) Routes that terminate the process without running `shutdown()` leave a live book with no record.** `taskkill /F`, `kill -9` and console-window close run no shutdown path at all, so no retry length whatsoever helps them. The intent to cancel is **never persisted anywhere**: an `offer_log` row left at `status="pending"` is byte-identical to a healthy resting offer, so the next engine restores it into `State` and resumes quoting as if nothing happened. **No startup path can act on a state that was never written.** K3 helps only when the wallet can be asked and answers.
- **(c) HOW LONG SHUTDOWN CAN TAKE IS NOT ESTABLISHED, and this is its open home.** S46 records the history -- three published figures, 90 s then 247 s then 390.5 s, all three wrong -- but S46 is closed, so the unresolved question must not live only there. **The concrete lead: `close_connections()` calls `ChiaRPCBase::close()`, which does `thread_pool_->join()` and therefore blocks shutdown on any wallet curl transfer still executing, up to `CURLOPT_TIMEOUT_MS`. That join appeared in none of the three figures.** Anyone attempting a bound must enumerate every leg of `Engine::shutdown()` first and state per leg what bounds it, naming the unbounded ones -- a single aggregate number is what went wrong three times. Note also that `rpc_post` has no per-call timeout override and abandoning an in-flight transfer would be memory-unsafe, so a genuine hard bound needs plumbing, not arithmetic.
- **(b) `State::cancel_pending` has no clearer and no timeout.** `state.cpp` is its only mutator and it only sets; a repo-wide grep finds nothing that unsets it. Every per-offer drain path skips a `cancel_pending` offer -- `cancel_stale`, `selective_cancel`, `cancel_ids`, `classify_tier_staleness`, `step_sweep_stale_offers`, the peg-suspend drain -- **and the exposure gate, which is not merely one more drain path.** There the skip does not just decline to cancel: the offer contributes **ZERO to the reserve-guard sum**, so a live, fillable offer stops counting against the limit it is supposed to consume. That is the same failure mode S46 D1(i) was withdrawn for -- unbounded live exposure -- reachable here through a flag set by ordinary running rather than by a latch. So an offer whose cancel spend was **submitted but never landed** (the documented partial-broadcast state -- PENDING_CANCEL is not proof of a coin spend) is skip-forever until the process restarts. **A restart is currently the only clearer**, which is exactly why D1's latch was withdrawn: it removed the restart without adding a replacement.
- **Today's evidence (2026-09-03).** S46's K3 leg now names PENDING_CANCEL rows at boot instead of silently restoring them, and the engine warns that the TTL sweep may pay a second cancel fee for a spend already in flight. That warning is the visible symptom of (b): we can see the state, report it, and do nothing about it.
- **Order of work, learned the expensive way.** Fix **(b) first**: give `cancel_pending` a clearer and a timeout, so the flag is safe to set from weaker evidence. Only then is any durable-intent mechanism for (a) worth building -- D2 failed three times because it kept latching state that nothing could unlatch. Any design for (a) must say up front what CLEARS the record, and "the next process trusts the last one's final word" is not an answer: a secure cancel is a SUBMISSION and the book is not empty until those spends confirm.
- **Do not** move the DB row to a new status to represent intent. `query_pending_offers()` filters on `status='pending'` exactly, so a row moved elsewhere vanishes from the restore at boot -- a schema change whose failure mode is worse than the bug.
- **Status:** `[ ]` -- OPEN. Own PR, own review round. Withdrawn prior art on `wip/s46-full-scope` (`b3ea169`) is worth reading first, as a record of what does not work.

### P1: max_position_usd is PER MARKET and nothing aggregated it
Three markets at the shipped 250,000 authorise **750,000 of exposure on a
500,000 account** -- 1.5x equity before the venue's 8x carried multiplier,
with every individual market perfectly inside its own limit. That is a
liquidation no per-market check can see, and it is the likeliest shape of
the one already suffered.
- **Status:** `[x]` PR #135 -- `portfolio_cap_usd()` reduces each market's
  cap by what the rest of the book holds, denominated in EQUITY because
  that is what the venue liquidates against. Risk-INCREASING legs are
  clamped to remaining headroom; reducing legs never are, or the book is
  trapped at the moment it is trying to get back inside.

### P2: the overnight cap is a one-way ratchet, denominated in ~21 legs
Overnight flow is one-directional -- buyers lift our ask, nothing sells
back against a frozen oracle -- so the short room drains. Each leg is
`target_depth_usd` = $1,200 against a `0.10 x 250,000` = $25,000 overnight
short cap: **~21 lifted asks** and the market pins one-sided, earning
EXACTLY ZERO because credit is `min(bid, ask)`. Simulated at a
conservative 2 lifts/market/hour: **135M depth-seconds against 227M**, the
last ~5 hours at zero.
- **Cutting the position cap makes this WORSE, not safer.** At 25,000 the
  overnight short cap is $2,500 -- about **two** legs.
- **Status:** `[~]` PR #135 makes the pin announce itself once (entry and
  recovery) instead of hiding in a per-tick line that repeats all night
  while the tick still reports `action="quote"`. The RATCHET itself is
  unfixed: see P3.

### P3: decide() re-quotes on absolute distance, not on drift
`drift = abs(price - view.oracle) / view.oracle` cannot tell a leg
DELIBERATELY placed wide from one the oracle moved away from -- and
overnight the oracle is frozen, so there is no drift at all. That single
check is also currently the only thing keeping a leg inside the ring.
Splitting it into an absolute ring bound plus a true
drift-since-placement measure would let the ask retreat toward the ring
edge, where **credit is flat and fills are rarer** -- the actual
anti-ratchet, and the same root cause as the anti-cross retreat being
capped at 0.96% of a 2% ring.
- **Status:** `[ ]` Core quoting-loop change; own PR, own review round.

### P4: RestingQuote records prices only, and two fixes are blocked on it
No `reduce_only` flag, no size. Consequently nothing can prove a resting
leg still fits a cap that has shrunk under it (RAMP's caps shrink
CONTINUOUSLY while the stage stays RAMP -- measured 6000 -> 3200 between
T-2400s and T-1000s), and nothing can distinguish a lone reduce-only
quote from an ordinary one (so exempting it from `risk_forced` risks an
order filling THROUGH flat into opposite exposure).
- Both were attempted on 2026-09-01 and **reverted** rather than shipped
  on state that does not exist.
- **Status:** `[ ]` Populate from the venue in `reconcile()`; then both
  revalidations become straightforward.

### P5: close_out.py accumulates guards where it should validate once
**Thirteen** defects of one shape -- a value that could not be read
treated as a value meaning something. Six in `read_positions`
(2026-08-31), then on 2026-09-01: blank market key dropped with live
size; absent size collapsed by `or 0.0`; empty/non-dict acknowledgement
counted as sent; `+inf` lot passing a `lot == lot` guard; boolean
position; boolean fill quantity; a non-empty-but-junk fills list counted
as acceptance. Two parsers of one field drifted apart twice
(`order_verdict` vs `filled_size`; close vs runner on `market`/`symbol`).
- Every one was real and every one is fixed. The COUNT is the argument.
- **Status:** `[ ]` Rewrite as validated conversions, one rule stated
  once. Not a patch, and not inside an active review cycle -- PR #132
  took 34 findings across 16 rounds, several of them defects introduced
  by the previous round's fix.

### Verification note, recorded because it changes what "tested" means here
Six tests written on 2026-09-01 passed with their bug reinstated, four of
them written specifically to prove a fix. The cause was mutating by
DELETING a mechanism, which proves a test touches code but not that it
catches the defect; the check that works is reinstating the PREVIOUS
BUGGY VERSION. Also: two consecutive "0 new comments" review rounds on
#132 were read as convergence and reported as such, and the next round
found a design-level hole in the same code -- a stale resting order could
fill after "Close all" and recreate the exposure.
