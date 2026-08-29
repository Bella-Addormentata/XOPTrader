# XOPTrader Master TODO List

**Created:** 2026-03-24
**Last audited: 2026-08-27** -- S32 added while working the two Copilot reviews of PRs #115/#117: the S27 fix, on its own, would have paused the engine on every start with the live config. S27 and S29 moved to `[~]` (built, unmerged). Most other findings in those reviews were already fixed on-branch; both reviews ran against an older snapshot.
**Previously audited: 2026-08-26 (v0.9.22)** -- S22/S24/S26-S30 added from the 2026-08-25/26 incident sessions: the warp.green and Circuit DAO compromises, the equity-blindness and dead-pause-flag defects found while responding to them, and the peg registry built in reply. S23 and S25 shipped in v0.9.22 (PRs #113, #114).
**Previously audited: 2026-08-23 (v0.9.19)** -- statuses refreshed after the release train #96-#107: the spendable-XCH-zero incident fixes (coin-lock ledger PR #107, ensure_split half-split PR #106), the warp claim fix (PR #105, first live bridge completed 2026-08-23), the eth-account pin (PR #104), and the Warp tab split (PR #102).
**Source:** Consolidated from all code reviews in `docs/CODE REVIEWS/` plus the 2026-08 live-operation sessions.

**Status Key:** `[ ]` = Not started | `[~]` = In progress | `[x]` = Complete

> Items closed before this audit (Tiers 1–3, T5–T7, and all previously-`[x]` entries) are archived — see this file's pre-audit revision in git history (`git show d3fc381:TODO.md`).

---

## Target scope: v0.9.23 -- make it safe to resume

The engine has been paused since 2026-08-25 and every item here is about the
same failure: **the bot continuing to stand behind quotes it can no longer
manage.** Ordered by whether resuming without it is defensible.

| item | why it gates a resume |
| --- | --- |
| **PR #115** (S29/S30) | peg identity becomes an asset property; `enforce: false` lets an operator switch off a compromised peg without a release. **In flight** |
| **S27** | with only XCH/DBX enabled, equity is exactly $0 and `equity_drawdown_frac` returns 0.0 on a non-positive peak — **both breakers go inert rather than trip**. Resuming with no drawdown protection is worse than staying paused. **Fix built on `fix/s27-equity-blindness` (PR #117); was the blocker** |
| **S32** | the S27 fix, on its own, would have made the engine **pause itself on every start** with the live config — wUSDC.b and wmilliETH.b are held with no enabled pair, so every cycle degrades from cycle 0 and the peak never seeds. Found 2026-08-27 while working the Copilot reviews. **Fixed in PR #117 (78b97f0)** |
| **S28** | the engine cannot fall back to the wallet for block height, so it sat dead ~2.5h beside a healthy wallet RPC. (The pause-flag half was overstated; corrected in place — pause *is* applied before Step 8 on recovery) |
| **S31** | nothing cancels the book when the engine stops. Cost $12.71 and a tripped breaker on 08-25, and was demonstrated again on 08-26 when a stray `--dry-run` killed the live process and left 10 offers unmanaged |
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
- **Files:** `cpp/src/engine.cpp` (Step 8 reconciliation), `cpp/src/rpc/chia_rpc.cpp`
- **Issue:** ~800s of every ~840s cycle spent in reconciliation, scanning blocks at ~12.5s each. Pre-existing but invisible while v0.9.20 was breaker-paused.
- **Status:** `[ ]` — Suspected contributor to the 2026-08-25 full-node RPC wedge: sustained scan load against an I/O-strained node. Unproven, but the two facts sit close together.


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
