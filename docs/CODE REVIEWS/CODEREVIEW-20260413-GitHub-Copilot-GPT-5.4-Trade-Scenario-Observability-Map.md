# Trade Scenario Observability Map

Date: 2026-04-13

## Purpose

This document maps each trade-decision scenario ID to:

1. The exact log substring that proves the branch fired, when one exists.
2. The primary `xop_trade_decision_total{strategy,scenario_id,result}` label set.
3. The secondary Prometheus signal that can corroborate state around it.
4. Any residual instrumentation gap beyond the decision counter.

Current state:

1. The engine now emits `xop_trade_decision_total{strategy,scenario_id,result}`
   for the documented system, recovery, crossed-book, and buyer branches that
   are instrumented in code.
2. The counter measures branch occurrences, not unique blocks, unique offers,
   or unique trades.
3. The gauge and counter signals listed below are secondary corroboration only.
4. Where older row text still says `no counter`, read that as `no specialized
   metric beyond xop_trade_decision_total`.

## Current Relevant Prometheus Signals

| Metric | Type | Notes |
|---|---|---|
| `xop_trade_decision_total{strategy=...,scenario_id=...,result=...}` | counter | Primary decision-tree counter for system, recovery, crossed-book, and buyer branches |
| `xop_node{metric="wallet_connected"}` | gauge | Confirms wallet connectivity only |
| `xop_node{metric="synced"}` | gauge | Confirms node sync, not wallet sync |
| `xop_fees_paid_24h_mojos` | gauge | Indirect evidence a take paid a fee |
| `xop_spendable_reserve_pct{wallet=...}` | gauge | Indirect evidence of reserve depletion or improvement |
| `xop_inventory_skew` | gauge | Indirect evidence for buyer inventory gate |
| `xop_market_mid_price` | gauge | Indirect evidence for fair value availability |
| `xop_analysis_pair` | gauge | Indirect market-analysis context only |
| `xop_bot_paused` | gauge | GUI pause state only |
| `xop_offers_total{event="filled|cancelled|expired"}` | counter | Maker offer lifecycle only |

## System Scenario Map

| ID | Exact log substring | Current Prometheus signal | Gap |
|---|---|---|---|
| SYS-01 | `wallet circuit breaker open` | `xop_node{metric="wallet_connected"}` may degrade, but no circuit-open metric exists | No direct counter or gauge for circuit-open state |
| SYS-02 | `Steps 7-8 SKIPPED: XCH recovery mode active` | `xop_spendable_reserve_pct{wallet="xch"}` may be low | No direct recovery-mode metric |
| SYS-03 | `TOOK crossed-book offer` then `SKIPPED: pending_change=` | `xop_fees_paid_24h_mojos` increases after the take | No direct same-block handoff counter |
| SYS-04 | `DRY RUN -- would take offer` | none today | No dry-run scenario metric |

## Recovery Scenario Map

| ID | Exact log substring | Current Prometheus signal | Gap |
|---|---|---|---|
| REC-T01 | `[Recovery] ... TOOK ask --` | `xop_fees_paid_24h_mojos` may increase when fee > 0 | No recovery-take counter |
| REC-T02 | `[Recovery] ... TAKING ask` on entry block | none today | No counter; currently unreachable with `cancel_on_enter=true` |
| REC-B01 | No dedicated log emitted | `xop_spendable_reserve_pct{wallet="xch"}` can show healthy balance | Silent no-op branch |
| REC-B02 | `confirmed ... is healthy -- UTXO locking from own offers, not entering recovery` | `xop_spendable_reserve_pct{wallet="xch"}` low while reserves exist | No counter for UTXO-lock interpretation |
| REC-B03 | `Cancelling all offers to free locked coins` and `Waiting for coins to settle after cancellation` | `xop_offers_total{event="cancelled"}` may rise later if tracked offers cancel through lifecycle | No recovery-cancel counter |
| REC-B04 | `No CoinGecko CEX reference for XCH/wUSDC -- cannot evaluate ask prices, skipping` | `xop_market_mid_price` may still exist from DEX data | No counter for missing recovery CEX anchor |
| REC-B05 | `[Recovery] XCH/BYC -- no CEX reference, skipping` or `[Recovery] XCH/DBX -- no CEX reference, skipping` | `xop_market_mid_price` may exist | No pair-skip counter |
| REC-B06 | Candidate-only branch. Use candidate log plus absence of take; no dedicated rejection log exists | `xop_market_mid_price{pair_name="XCH/wUSDC.b"}` only | Silent premium reject |
| REC-B07 | `failed to fetch offer text` or `offer ... no longer active` | none today | No stale-offer reject counter |
| REC-B08 | `No acceptable XCH asks found this block` | none today | No empty-scan counter |
| REC-B09 | `EXITING recovery mode` | `xop_spendable_reserve_pct{wallet="xch"}` may improve | No recovery-exit counter |

## Crossed-Book Scenario Map

| ID | Exact log substring | Current Prometheus signal | Gap |
|---|---|---|---|
| ARB-T01 | `TOOK crossed-book offer` | `xop_fees_paid_24h_mojos` increases | No crossed-book take counter |
| ARB-T02 | `freed coin by cancelling` then `TOOK crossed-book offer` | `xop_offers_total{event="cancelled"}` may later reflect maker cancel lifecycle | No liberation-path counter |
| ARB-B01 | No dedicated log when feature disabled or dependencies absent | none today | Silent early return |
| ARB-B02 | `crossed-book SKIPPED -- wallet circuit breaker open` | `xop_node{metric="wallet_connected"}` may be degraded | No circuit-open metric |
| ARB-B03 | No dedicated log emitted | none today | Silent no-two-sided-market branch |
| ARB-B04 | No dedicated log emitted | none today | Silent not-crossed branch |
| ARB-B05 | `crossed book edge=` and `< min=` | `xop_market_mid_price` is only supporting context | No min-edge reject counter |
| ARB-B06 | `spendable ... < 0.25 -- attempting cancel-worst` plus either `cancel-worst failed` or later wallet error | `xop_spendable_reserve_pct{wallet="xch"}` may be low | No insufficient-preflight counter |
| ARB-B07 | `DRY RUN -- would take offer` | none today | No dry-run counter |
| ARB-B08 | `failed to fetch offer text` or `offer ... no longer active` | none today | No inactive-offer counter |
| ARB-B09 | `take_offer failed:` or `crossed-book take failed:` | none today | No take-failure counter |

## Buyer Scenario Map

| ID | Exact log substring | Current Prometheus signal | Gap |
|---|---|---|---|
| BUY-T01 | `[Buyer] XCH/wUSDC.b TOOK offer` | `xop_fees_paid_24h_mojos` increases; `xop_inventory_skew` may move later | No buyer-take counter |
| BUY-T02 | `[Buyer] ... TOOK offer` on a `side: bid` rule | same as BUY-T01 | No bid-side buyer counter; currently unreachable in config |
| BUY-B01 | No dedicated log emitted | none today | Silent early return on disabled buyer |
| BUY-B02 | `SKIPPED: wallet circuit breaker open` | `xop_node{metric="wallet_connected"}` may be degraded | No circuit-open metric |
| BUY-B03 | `SKIPPED: XCH recovery mode active` | `xop_spendable_reserve_pct{wallet="xch"}` may be low | No recovery-mode metric |
| BUY-B04 | `SKIPPED: flash crash state=` | none today | No flash-crash-state metric exported |
| BUY-B05 | `SKIPPED: wallet not synced` or `Wallet sync check failed:` | `xop_node{metric="wallet_connected"}` only partially relevant | No wallet-sync metric |
| BUY-B06 | `SKIPPED: pending_change=` | none today | No pending-change metric |
| BUY-B07 | `SKIPPED: fee tracker returned 0 (budget exhausted)` | `xop_fees_paid_24h_mojos` may be high | No fee-budget-exhausted counter |
| BUY-B08 | `SKIPPED: xch_spendable=` and `< 0.25 XCH preflight` | `xop_spendable_reserve_pct{wallet="xch"}` may be low | No preflight-fail counter |
| BUY-B09 | `SKIPPED: buyer fee budget exhausted` or `round-trip fee` and `exceeds buyer budget headroom` | `xop_fees_paid_24h_mojos` is the nearest gauge | No buyer-budget counter |
| BUY-B10 | No dedicated log emitted | none today | Silent disabled-rule branch |
| BUY-B11 | `cooldown active (last take block` | none today | No cooldown counter |
| BUY-B12 | `daily cap reached` | none today | No daily-cap counter |
| BUY-B13 | `skipped: fee/edge model currently supports only pairs involving XCH` | `xop_market_mid_price` may exist | No unsupported-pair counter |
| BUY-B14 | `no fair price -- skipping` | `xop_market_mid_price` likely zero or absent | No missing-fair-value counter |
| BUY-B15 | `inventory gate:` | `xop_inventory_skew` is the direct corroborating gauge | No buyer-inventory-reject counter |
| BUY-B16 | `no CEX reference -- skipping`, `missing CEX age metadata -- skipping`, or `CEX age` and `> max` | `xop_market_mid_price{pair_name=...}` may exist but no CEX gauge is exported | No CEX-staleness counter or gauge |
| BUY-B17 | `CEX premium` and `> max` | none today | No CEX-premium reject counter |
| BUY-B18 | `VPIN` and `> max` | `xop_analysis_pair` does not export VPIN; no direct metric exists | No VPIN gauge exported |
| BUY-B19 | `unresolved wallet IDs` | none today | No wallet-id-resolution counter |
| BUY-B20 | `spend wallet` and `has pending_change=` | none today | No CAT pending-change metric |
| BUY-B21 | No dedicated log emitted | none today | Silent no-candidate branch |
| BUY-B22 | No dedicated log emitted | none today | Silent size-filter reject |
| BUY-B23 | No dedicated log emitted | none today | Silent band reject |
| BUY-B24 | `candidate` and `requires` and `but only` and `available` | none today | No insufficient-spend-balance counter |
| BUY-B25 | No dedicated log emitted | none today | Silent net-edge reject |
| BUY-B26 | No dedicated log emitted | `xop_fees_paid_24h_mojos` is only contextual | Silent fee-to-gain reject |
| BUY-B27 | `candidate would exceed daily cap` | none today | No over-cap candidate counter |
| BUY-B28 | `DRY RUN -- would take offer` | none today | No dry-run counter |
| BUY-B29 | `failed to fetch offer text for` or `offer ... no longer active (status=` | none today | No inactive-offer counter |
| BUY-B30 | `take_offer failed:` or `take failed:` | none today | No buyer take-failure counter |
| BUY-B31 | `stopping after successful take: pending_change=` | none today | No post-take-stop counter |

## Implementation Status

The scenario counter family is now implemented:

1. `xop_trade_decision_total{strategy="system|recovery|crossed_book|buyer",scenario_id="...",result="triggered|blocked"}`

The next useful observability steps are:

1. Add Grafana panels keyed by `scenario_id` and `strategy`.
2. Alert on high-rate block paths such as `BUY-B07`, `BUY-B08`, `REC-B04`, and `ARB-B09`.
3. Add dedicated state gauges only where the decision counter is not enough for diagnosis.

That single family would cover every row above without exploding label
cardinality because the scenario ID set is fixed and small.
