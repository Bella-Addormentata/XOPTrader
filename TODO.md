# XOPTrader Master TODO List

**Created:** 2026-03-24
**Last audited: 2026-08-18 (v0.9.8+)** — full re-verification of every open item against the current codebase (grep, file reads, `git log -S`) following four months of landed work: PR #66 accounting/quoting/safety overhaul (da8f6c1), the P&L overhaul (e4405a9), drawdown-breaker recalibration to portfolio equity (692c1fd), the warp.green bridge + Base wallet subsystem (PRs through #78), fee reserves (PR #79), and revive_market (PR #80).
**Source:** Consolidated from all code reviews in `docs/CODE REVIEWS/` plus the 2026-08 live-operation sessions.

**Status Key:** `[ ]` = Not started | `[~]` = In progress | `[x]` = Complete

> Items closed before this audit (Tiers 1–3, T5–T7, and all previously-`[x]` entries) are archived — see this file's pre-audit revision in git history (`git show d3fc381:TODO.md`).

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
- **Status:** `[ ]` — Verified 2026-08-18, no fix since PR #79. Fix: convert the reserve into base-mojo terms via mid price, or reserve against the wallet XCH balance instead of the quote-sized pool.

### S4: Warp unwrap G10 gate budgets the Chia fee once but pays it twice
- **Files:** `gui/services/warp/service.py`
- **Issue:** G10 gate (line 1966) computes `toll_need = net.chia_toll_mojos + p.unwrap_chia_fee_mojos` (fee counted once), but the fee is paid on both the burn `cat_spend` (line 2108) and the toll-coin funding `send_transaction` (line 2224). Neither send site re-checks spendable XCH after the UNWRAP_CHECKS gate; an offer locking coins between gate and send makes the second spend fail.
- **Status:** `[ ]` — Verified 2026-08-18. Fix: gate on `toll + 2*fee` and re-check spendable at BURNING and FUNDING_CLAIM.

### S5: Published mid blends stale last-trade with no staleness gate
- **Files:** `cpp/src/execution/market_data.cpp:1009-1012, 1188-1189`
- **Issue:** `compute_mid` Case 3 falls back to `ps.dex_last_trade` at 0.70 weight with zero age check; no timestamp is stored for the print at all (market_data.hpp:406) and `dex_updated_at` is re-stamped `now()` every heartbeat (market_data.cpp:380, 1542). A 13-day-old trade dragged wmilliETH.b's mid 8%+ below fair.
- **Status:** `[ ]` — Verified 2026-08-18. Fix: carry the dexie trade timestamp through `ingest_dexie` and age-taper or refuse the fallback; the `dex_print_age` heartbeat counter (market_data.cpp:1544+) exists but nothing gates on it ("Signal only — no consumer gates on it yet").

### S6: CoinGecko staleness blindness in the general published-mid path
- **Files:** `cpp/src/engine.cpp:1815-1827, 2069-2100`; `cpp/src/execution/market_data.cpp:405-406`
- **Issue:** A failed CoinGecko fetch is swallowed, leaving the stale cache (`coingecko_last_fetch_` advances only on success), yet every heartbeat re-derives `cex_mid` from that cache and calls `ingest_cex_reference`, which stamps `cex_updated_at = now()` — so the `cex_freshness_threshold_sec` taper (market_data.cpp:1041-1048) always sees age ~0 and can never fire.
- **Status:** `[ ]` — Verified 2026-08-18. `revive_market` got a real success-stamped gate today (engine.cpp:4353-4370 via `coingecko_last_fetch_`); the general blend path needs the same: stamp `cex_updated_at` from actual fetch time, or stop re-ingesting on failed fetches.

### S7: Dexie ticker parse swaps bid and ask
- **Files:** `cpp/src/rpc/dexie_client.cpp:616-638`
- **Issue:** `prices.buy[0]` is labeled best bid (`t.price_buy`) and `prices.sell[0]` best ask when the sides are inverted, producing constant "Crossed book" warnings that market_data.cpp:361-371 rationalizes as "normal on Dexie". Harmless for the published mid only because `ingest_competing_offers` overwrites from the real book (market_data.cpp:1529-1530) — but OFI (engine.cpp:1937-1943) and the offer_manager ticker mid (offer_manager.cpp:2297) still consume the swapped sides.
- **Status:** `[ ]` — Verified 2026-08-18 (`git log -S price_buy` since 2026-04-21: no fix). Fix: swap the labels at parse and delete the crossed-book rationalization.

### S8: wmilliETH.b has no drift target — accumulation brakes unreachable
- **Files:** `config.yaml:168-187`; `cpp/src/engine.cpp:4891-4917`
- **Issue:** `strategy.asset_target_allocations` omits WMILLIETH.B (and `ratio_target_by_pair`/`ratio_band_enter_by_pair` omit wmilliETH.b/XCH) even though the pair is enabled; `acquire_scale` returns 1.0 for missing keys (engine.cpp:4891-4893) and is applied to the bid side (:4917), so accumulation is never tapered. Remaining limits are portfolio-fraction denominated (`single_cat_cap_pct` 0.25, soft/hard 0.6/0.8) — unreachable for a small CAT position on a large wallet.
- **Status:** `[ ]` — Verified 2026-08-18. Fix: add WMILLIETH.B target/tolerance and a wmilliETH.b/XCH ratio target, or add a per-pair absolute inventory cap.

---

## Resolved since 2026-04 (audit evidence)

- **#6 Stronger inventory rebalance pull** — DONE: ratio-rebalance sizing with hysteresis bands (2331dda, engine.cpp:4722-4810) + Avellaneda-Stoikov reservation offset now actually applied to the ladder centre (96eefb0/f7108e9, reservation_offset.hpp, tested in test_reservation_offset.cpp); implemented via reservation-offset + capital-ratio scaling rather than a skew-gain tweak.
- **#9 Persist cost basis across restarts** — DONE: `inventory_state` table (database.cpp:164, additive `IF NOT EXISTS`), save/load at database.cpp:1002/:1094, restored at startup via `restore_record` with persisted sentinel flag (engine.cpp:994-1005), persisted on shutdown + periodically (engine.cpp:10663). Commit e4405a9 (P&L overhaul).
- **T4-01 CEX reference price integration** — OBSOLETE: superseded by CoinGecko-derived `cex_mid` fed to `ingest_cex_reference` (engine.cpp:2060-2100) + WLS fair-value solver fusing CoinGecko/TibetSwap/dexie (fair_value_solver.cpp, test_fair_value.cpp). Direct OKX/Gate client never built; `ArbitrageDetector::set_cex_prices` is dead scaffolding. Live per-exchange tickers for CEX-DEX arb would be a new, narrower item.
- **T4-10 Per-tier fill-rate monitoring** — DONE: `query_tier_fill_rates` (database.cpp:1461) fed into ladder each cycle (engine.cpp:5088-5113), blended into `tier_size_pct` with 5% floor (liquidity.cpp:795-861); on by default (config.hpp:994). Commits 679a934, 65b671b. Deviations from spec: side-aggregated 24h DB window instead of per-side rolling tracker; no Thompson sampling.
- **T8-26 PnLTracker database tests** — DONE: `test_pnl_tracker.cpp` (11 tests, commit e4405a9) covers DB rehydration, trade_id idempotency, USD conversion, CSV export; snapshot insert/query covered in test_database.cpp:520-562; equity-curve API removed by the P&L overhaul (drawdown breaker now on portfolio equity, 692c1fd).
- **S1 Phantom offer removal** — DONE: cae2bfd (see New findings S1 above); the MEMORY.md "proven, unfixed" index line is stale.
- **S2 CI red since 2026-04-08** — DONE: PR #70 / 206d55c (see New findings S2 above); recent runs green as of 2026-08-18.
