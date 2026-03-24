# XOPTrader Comprehensive Code Review

Date: 2026-03-24
Reviewer: GitHub Copilot

## Original review prompt

> now please perform a comprehensive code review of XOPTrader. lok for logical errors and coding errors. also look for pitfalls and missing logical strategies. also look for ways to clean up the code and timing of logical strategies. please place your review in the code review folder when you are complete. plese include this prompt in the note for added context. please consider anything we might have missed for this review.

## Scope and method

This review focused on the C++ trading engine, execution, risk, monitoring, and tests. It was primarily a static review of the source tree, cross-checked against tests and internal comments.

What was checked:
- Engine lifecycle, heartbeat sequencing, and async control flow.
- Fill handling, inventory accounting, PnL math, and pre-trade risk checks.
- Strategy wiring, quote construction, and timing/data-freshness gates.
- Coverage gaps and incomplete live-trading paths.

Environment notes:
- IDE diagnostics reported no current compile/lint errors in the workspace.
- A direct CMake configure attempt could not be completed because `cmake` is not installed in the current shell environment.
- Because of that, findings below should be treated as a static code review with source-level evidence, not a compiled/runtime validation.

## Executive summary

The repository contains strong research coverage and a substantial amount of implementation work, but the live trading path is not yet production-safe.

The highest-risk issues are:
1. The executable entrypoint still instantiates a stub `Engine` instead of the fully implemented `xop::Engine`.
2. The real engine uses blocking `co_spawn(..., use_future)` + `get()` patterns that can deadlock the same `io_context` thread.
3. Position, fill, inventory, and PnL accounting are inconsistent across pair names, asset IDs, and units.
4. Quote generation discards important strategy outputs and re-synthesizes prices later, weakening inventory-aware logic.
5. Several important safeguards are present only as comments, diagnostics, or placeholders rather than enforced trading controls.

## Findings

### 1. Critical: the built entrypoint still uses a stub `Engine`

Evidence:
- [cpp/src/main.cpp](../../cpp/src/main.cpp#L89-L252)
- [cpp/src/main.cpp](../../cpp/src/main.cpp#L424-L479)

Issue:
- `main.cpp` defines a local `Engine` class and instantiates that type in `main()`.
- That bypasses the actual `xop::Engine` implemented in [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L43-L180).
- The shipped executable therefore appears to run a placeholder heartbeat rather than the real 13-step engine.

Impact:
- A successful build can still produce a binary that is logically incomplete.
- Reviewing only `engine.cpp` can create a false sense of readiness.

Recommendation:
- Remove the local stub from `main.cpp`.
- Include and instantiate `xop::Engine` directly.
- Keep only one production engine path.

### 2. Critical: the real engine can deadlock on its own event loop

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L214-L234)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L345-L349)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L589-L591)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1017-L1022)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1040-L1044)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1310-L1319)

Issue:
- `open_connections()` is called before `ioc_.run()`, but it immediately does `co_spawn(..., use_future)` and blocks on `get()`.
- Similar blocking `future.get()` calls appear in block polling, fill processing, stale-offer cancellation, and posting quotes.
- When those calls run on the same `io_context` thread they are waiting on, the engine can hang.

Impact:
- Startup can freeze before the main loop even begins.
- Per-block processing can stall under normal operation.
- Time-sensitive quote placement and cancellation become unreliable.

Recommendation:
- Make the engine consistently asynchronous using `awaitable`/`co_await`, or move RPC boundaries to explicit worker threads.
- Avoid synchronous `future.get()` from code that schedules onto the same event loop.

### 3. Critical: state positions are keyed by pair name in fills but read by asset ID in risk checks

Evidence:
- [cpp/src/execution/offer_manager.cpp](../../cpp/src/execution/offer_manager.cpp#L241-L254)
- [cpp/src/state.cpp](../../cpp/src/state.cpp#L300-L318)
- [cpp/src/risk/limits.cpp](../../cpp/src/risk/limits.cpp#L129-L205)

Issue:
- Fill handling calls `state_->record_buy(po.pair_name, ...)` and `state_->record_sell(po.pair_name, ...)`.
- Risk checks later read positions using `base_id` and `quote_id` asset identifiers.
- That means the state map can contain positions under pair symbols while risk logic queries asset symbols.

Impact:
- Risk limits may see zero or incomplete positions.
- Concentration, CAT caps, and pair capital checks can silently under-enforce.

Recommendation:
- Make asset ID the canonical key for holdings everywhere.
- Derive pair exposure from asset-level balances rather than storing the primary book by pair name.

### 4. Critical: engine inventory updates only the `xch` record regardless of traded pair

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L595-L638)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L731-L738)

Issue:
- After a fill, the engine updates `inventory_->record_buy("xch", ...)` or `inventory_->record_sell("xch", ...)` for every pair.
- The quote asset and any non-XCH base assets are never updated here.

Impact:
- Inventory ratios, cost basis, and exposure are wrong for multi-asset trading.
- Any portfolio-level or per-pair risk decision becomes unreliable.

Recommendation:
- Update both base and quote asset legs per fill.
- Centralize fill-to-inventory accounting in one unit-tested function.

### 5. Critical: bid size semantics are inconsistent between ladder generation and order execution

Evidence:
- [cpp/src/strategy/liquidity.cpp](../../cpp/src/strategy/liquidity.cpp#L163-L182)
- [cpp/src/execution/offer_manager.cpp](../../cpp/src/execution/offer_manager.cpp#L575-L611)

Issue:
- `LiquidityEngine` sets bid `TierQuote.size` from `available_capital`, explicitly treating it as quote-side capital.
- `OfferManager::build_offer_dict()` interprets bid `tier.size` as base quantity and converts it into quote spend using price.

Impact:
- Bid orders can be materially oversized or undersized.
- Wallet offers may not match the intended ladder allocation.

Recommendation:
- Make `TierQuote` explicit, e.g. `base_size` plus `quote_notional`, or enforce one invariant across the whole codebase.
- Add unit tests around bid/ask quantity translation into wallet `offer_dict`.

### 6. Critical: one mutable `AvellanedaStoikov` instance is shared across all pairs

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L111-L116)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L701-L743)
- [cpp/include/xop/strategy/avellaneda.hpp](../../cpp/include/xop/strategy/avellaneda.hpp#L62-L62)
- [cpp/include/xop/strategy/avellaneda.hpp](../../cpp/include/xop/strategy/avellaneda.hpp#L175-L181)

Issue:
- The engine creates one `strategy_` instance and reuses it across all pairs.
- That strategy holds mutable `price_buffer_`, `regime_`, and `cost_basis_` state.
- Each pair update contaminates the others.

Impact:
- Regime detection and no-loss cost-basis state can bleed between unrelated markets.
- The model can quote one pair using another pair’s recent price history.

Recommendation:
- Maintain one strategy instance per pair, or make the strategy stateless and pass pair state explicitly.

### 7. Critical: inventory units passed into Avellaneda-Stoikov do not match `q_max`

Evidence:
- [cpp/include/xop/strategy/avellaneda.hpp](../../cpp/include/xop/strategy/avellaneda.hpp#L62-L62)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L728-L738)
- [cpp/src/strategy/avellaneda.cpp](../../cpp/src/strategy/avellaneda.cpp#L168-L180)

Issue:
- `q_max` is documented in base-asset units.
- The engine passes `inventory_->net_inventory(AssetId{"xch"})`, which appears to be in mojos.
- The sizing logic computes `q / q_max` directly.

Impact:
- `q_ratio` can be off by orders of magnitude.
- Bid size can collapse to zero and ask size can explode or vice versa.

Recommendation:
- Normalize inventory into the same unit convention as the strategy config before calling `compute_quotes()`.
- Document all unit boundaries explicitly.

### 8. High: strategy price skew is discarded and rebuilt as symmetric mid ± spread

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L740-L744)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L904-L917)

Issue:
- Step 4 computes `pcs.raw_quote` with strategy-specific bid and ask prices.
- Step 6 rebuilds prices from `mid ± half_spread` instead of preserving the strategy’s actual reservation-price skew.

Impact:
- Inventory-aware price tilts from Avellaneda-Stoikov can be lost.
- Final posted quotes may be much more symmetric than intended.

Recommendation:
- Use strategy bid/ask as the base quote and then apply spread/risk adjustments on top.
- Avoid reconstructing prices from scratch downstream.

### 9. High: PnL calculations appear to use inconsistent units and can overflow

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L621-L624)
- [cpp/src/monitoring/pnl.cpp](../../cpp/src/monitoring/pnl.cpp#L598-L608)

Issue:
- Realized and inventory PnL are calculated as `(price - basis) * size` or `(current_price - basis) * balance` directly.
- The comments describe mojo-of-quote per mojo-of-base relationships, but the code mixes pair-level price, asset balances, and mojo scales without an explicit normalization boundary.

Impact:
- PnL can be inflated by unit mismatch.
- Large positions risk integer overflow in `Mojo` arithmetic.
- Monitoring and alerting can become misleading even when the trading logic seems healthy.

Recommendation:
- Define one canonical monetary unit for stored PnL.
- Convert price × size with explicit scale factors and checked arithmetic.
- Add tests covering representative fills and mark-to-market snapshots.

### 10. High: stale market data is detected but not used to suppress quoting

Evidence:
- [cpp/src/execution/market_data.cpp](../../cpp/src/execution/market_data.cpp#L221-L239)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1-L13)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L720-L740)

Issue:
- The market-data layer marks pairs stale.
- The engine comments say stale data is tolerated and the quote path continues as long as `mid > 0`.
- There is no hard pre-trade freshness gate before quote creation or posting.

Impact:
- The bot can quote on delayed data.
- This is a classic adverse-selection and stale-quote risk.

Recommendation:
- Add a kill-switch or quote-suppression gate for stale pairs.
- Cancel existing offers if freshness drops below a configured threshold.

### 11. High: database offer status updates can fail because posted offer IDs are placeholders

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1046-L1062)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L630-L630)
- [cpp/src/database.cpp](../../cpp/src/database.cpp#L335-L349)

Issue:
- Step 8 inserts placeholder `offer_id` values into the database.
- Step 2 later updates status using the actual fill `offer_id`.
- `Database::update_offer_status()` throws if no row matches.

Impact:
- A normal fill can break persistence or leave the audit trail inconsistent.

Recommendation:
- Persist the actual wallet/Dexie offer ID returned by the posting path.
- Treat placeholder IDs only as transient local correlation keys, not database primary identifiers.

### 12. High: risk concentration uses raw balances instead of marked-to-market values

Evidence:
- [cpp/src/risk/limits.cpp](../../cpp/src/risk/limits.cpp#L386-L440)

Issue:
- `compute_concentration()`, `compute_portfolio_fraction()`, and `compute_pair_capital_fraction()` sum raw balances across assets.
- That is not economically comparable when different assets have different prices.

Impact:
- CAT-heavy portfolios can appear safe or unsafe for the wrong reasons.
- Concentration controls do not represent true capital-at-risk.

Recommendation:
- Mark positions to a common numeraire before exposure checks.
- Use live or conservative reference pricing for risk gating.

### 13. Medium-High: the strategic loss manager is consulted only in logs and cannot change execution

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L922-L937)

Issue:
- The loss manager is only mentioned in a debug log when inventory ratio exceeds a threshold.
- No decision is requested and no quote/output changes are applied.

Impact:
- The code suggests a rebalance exception path exists, but operationally it does not.
- Operators may assume controlled loss-taking is supported when it is not.

Recommendation:
- Introduce an explicit `LossDecision` flow and make step 6 apply it.
- If not implemented, disable the feature in config and docs.

### 14. Medium-High: multiple important live-trading flows remain placeholders or signal-only

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L565-L565)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1088-L1100)
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1105-L1126)
- [cpp/src/execution/offer_manager.cpp](../../cpp/src/execution/offer_manager.cpp#L626-L649)

Issue:
- CEX price ingestion is still a comment placeholder.
- Arbitrage logic only logs signals.
- Hedging is diagnostic-only.
- Dexie submission is a placeholder that returns success.

Impact:
- The codebase looks more complete than the live path actually is.
- Backtesting/research expectations can diverge from production behavior.

Recommendation:
- Make incomplete live features opt-in and visibly disabled until end-to-end implemented.
- Prefer explicit `NotImplemented`/feature flags over silent success placeholders.

### 15. Medium: alert baselines are placeholders, making monitoring less trustworthy

Evidence:
- [cpp/src/engine.cpp](../../cpp/src/engine.cpp#L1267-L1294)

Issue:
- `avg_fill_rate_24h`, `recent_high`, `peak_pnl`, `normal_spread_bps`, and `max_inventory_ratio` are placeholders or current-value stand-ins.

Impact:
- Drawdown, fill-rate decay, spread anomaly, and exposure alerts can fail to trigger or trigger inaccurately.

Recommendation:
- Track actual rolling baselines and high-water marks.
- Treat missing baseline state as a separate monitoring state rather than faking values.

### 16. Medium: test coverage omits the most failure-prone engine and execution paths

Evidence:
- [cpp/tests/CMakeLists.txt](../../cpp/tests/CMakeLists.txt#L33-L41)

Issue:
- Current tests cover strategy and analytics components but do not include engine lifecycle, fill processing, posting/cancellation flows, or database offer reconciliation.

Impact:
- The most operationally dangerous defects are unlikely to be caught before deployment.

Recommendation:
- Add tests for:
  - engine startup/shutdown and polling behavior,
  - fill processing and inventory updates,
  - `TierQuote` to wallet offer translation,
  - stale-data suppression,
  - database reconciliation of posted/fill/cancel states.

## Missing logical strategies / safeguards

The following controls appear missing or only partially enforced and should be considered before live trading:

1. **Hard stale-data circuit breaker**  
   Staleness is detected but not used to cancel or suppress quotes.

2. **Canonical unit policy**  
   Prices, balances, quote notionals, and PnL need one documented unit model.

3. **Per-pair state isolation**  
   Strategy, regime, and cost-basis state should not be shared across pairs.

4. **Order lifecycle reconciliation**  
   Posted offer IDs, wallet trade IDs, and DB records should be reconciled by one source of truth.

5. **Real portfolio valuation for risk**  
   Raw balances are not adequate for concentration controls across dissimilar assets.

6. **Feature gating for incomplete paths**  
   Arbitrage, hedging, and Dexie submission should be explicitly disabled until fully wired.

## Cleanup opportunities

1. Remove the duplicate engine architecture split between [cpp/src/main.cpp](../../cpp/src/main.cpp) and [cpp/src/engine.cpp](../../cpp/src/engine.cpp).
2. Replace repeated pair-config linear searches with a prebuilt map keyed by pair name.
3. Consolidate fill accounting into a single function that updates state, inventory, PnL, and database atomically.
4. Introduce typed quantity wrappers for base size, quote notional, price, and PnL units.
5. Move placeholder Phase 2 behavior behind config flags so incomplete paths are obvious at runtime.

## Priority remediation order

1. Unify entrypoint on `xop::Engine`.
2. Eliminate `co_spawn(... use_future)` blocking on the same event loop.
3. Fix asset/pair identity and unit consistency across fills, positions, risk, and PnL.
4. Preserve strategy-generated skew all the way to posted orders.
5. Add stale-data and incomplete-feature gates before any live deployment.
6. Add integration tests for engine, execution, and persistence.

## Final assessment

XOPTrader has strong design intent and a large amount of thoughtful infrastructure, but it still needs architectural cleanup and accounting hardening before live use. The most important theme is consistency: one engine path, one identity model for holdings, one unit model for money and size, and one authoritative order lifecycle.
