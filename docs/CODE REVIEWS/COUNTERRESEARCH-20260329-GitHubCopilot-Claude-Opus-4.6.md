# XOPTrader — Counter-Research Study

| Field | Value |
|-------|-------|
| **Date** | 2026-03-29 |
| **Reviewer** | GitHub Copilot (Claude Opus 4.6) |
| **Commit** | HEAD on `main` (post-PR #29, #31 fixes) |
| **Scope** | Re-evaluation of all 15 prior counter-research findings (CR-1 through CR-15) against the current codebase, plus 5 new challenges identified from implementation analysis |
| **Method** | Literature-informed adversarial analysis: for each theoretical or empirical model used in the engine, identify the strongest published critique, evaluate the codebase's current defences, and assign a residual risk score |

---

## Executive Summary

The 2026-03-25 counter-research study identified 15 challenge areas across the engine's theoretical foundations. Since that review, the codebase has implemented partial mitigations for 5 of those challenges — most notably the VPIN runtime validation gate (CR-1) and discounted Thompson Sampling (CR-7/CR-10). This follow-up re-evaluates all 15 prior findings plus 5 new challenges discovered during the current deep-read pass.

**Updated tally:** 5 HIGH, 9 MEDIUM, 6 LOW — 20 total challenge areas.

---

## Part A: Prior Finding Re-Evaluation

### CR-1 (HIGH → MEDIUM): VPIN Predictive Power — Andersen & Bondarenko (2014)

**Prior challenge:** VPIN has no incremental predictive power beyond raw volume and volatility; the 1.5× spread multiplier may be amplifying noise.

**Current status:** **Partially mitigated.** The engine now implements a VPIN validation gate (T5-CR1):
- Tracks each VPIN activation (`vpin > 0.01`) with block-level deduplication.
- After `kVpinBurnIn = 100` activations, logs precision (fraction of activations followed by adverse fills within `kVpinValidationWindow = 10` blocks).
- Emits a warning if precision falls below a configurable threshold.

**Residual risk:** The validation gate is observational only — it logs precision but does not automatically attenuate the VPIN multiplier when precision is poor. An operator must manually reduce `vpin` weight in response to warnings. In a thinly-staffed 24/7 operation, warnings may go unnoticed for days.

**Recommendation:** Implement an adaptive gain: $m_{vpin} = 1 + \text{vpin} \times 0.5 \times \min(1.0, \text{precision} / \text{precision\_target})$. This auto-attenuates the signal as precision drops.

**Severity:** Downgraded from HIGH to **MEDIUM** — the validation gate provides transparency, but the signal still acts on the spread before its predictive value is confirmed.

---

### CR-2 (HIGH — unchanged): Multi-Level OFI — Xu, Lehalle & Alfonsi (2023)

**Prior challenge:** Best-level OFI captures only 70–90% of the return variance that multi-level OFI provides. On thin books (typical for CHIA DEX), best-level OFI is dominated by single-order arrivals and departures.

**Current status:** **Not mitigated.** The `ingest_book_snapshot_for_ofi()` method in `MarketDataFeed` still uses only the best bid/ask level from the Dexie orderbook. An inline `TODO` comment references this counter-research finding but no implementation has been done.

**Residual risk:** The 0.3× OFI spread multiplier may spuriously widen spreads on single-order book changes that do not reflect genuine flow imbalance. On a DEX with 5–20 orders per level, this is a non-trivial concern.

**Recommendation:** Extend `ingest_book_snapshot_for_ofi()` to accept top-5 book levels, weighted by inverse distance from mid:

$$OFI_{multi} = \sum_{k=1}^{5} w_k \cdot (B_k(t) - B_k(t-1) - A_k(t) + A_k(t-1))$$

where $w_k = 1/k$ (harmonic weights).

**Severity:** Remains **HIGH**.

---

### CR-3 (HIGH → MEDIUM): Sawtooth Tau Exploitability

**Prior challenge:** The horizon-based tau reset (tau resets to `tau_0` every `horizon_blocks`) creates a sawtooth pattern. A competing bot can monitor the publicly-visible offer pattern — wide spreads at reset, narrowing over time — and front-run the narrowing phase.

**Current status:** **Partially mitigated.** The engine now uses exponential-decay tau (T5-CR3) tied to fills, not time. Tau decays on each fill and recovers gradually when no fills arrive. This removes the deterministic sawtooth pattern.

**Residual risk:** The decay rate $\lambda$ and recovery rate are constant, so a sophisticated adversary could still estimate the current tau by counting observed fills. However, this is significantly harder than reading a time-based sawtooth.

**Severity:** Downgraded from HIGH to **MEDIUM**.

---

### CR-4 (MEDIUM — unchanged): PIN Model — Duarte & Young (2009)

**Prior challenge:** PIN conflates illiquidity friction with genuine informed trading. Empirically, PIN is lowest during confirmed insider-trading periods (Collin-Dufresne & Fos 2015).

**Current status:** The Bayesian PIN estimator in `AdverseSelectionEstimator` is unchanged. It feeds into the adverse selection component of the spread:

$$s_{adverse} = \gamma \cdot \sigma \cdot \sqrt{T_{fill}} \cdot \text{PIN} \times 10^4$$

**Residual risk:** If PIN underestimates informed-trading probability (as Duarte & Young show), the adverse-selection component is too small, and the bot under-prices its spread during genuine information events. The other multipliers (VPIN, whale) provide partial defence, but they are independent signals — not a substitute for a correctly-calibrated PIN.

**Recommendation:** Consider switching to VPIN-based adverse selection intensity (combining CR-1 and CR-4): replace static PIN with a trailing fill-directional imbalance metric that responds to actual flow.

**Severity:** Remains **MEDIUM**.

---

### CR-5 (MEDIUM — unchanged): Variance Ratio Power — Richardson & Smith (1991)

**Prior challenge:** Lo-MacKinlay VR test at $q = 5$ and $q = 10$ with $n = 200$ observations has 5–9% power against relevant alternatives (AR(1) with $\phi = 0.1$, which is the range relevant for CHIA DEX returns).

**Current status:** The `RegimeDetector` is unchanged. VR thresholds 0.85/1.15 with hysteresis (5 blocks) and dual-horizon ($q_{short} = 5$, $q_{long} = 10$) remain the primary regime classification method. The optional HMM provides an alternative path, but HMM also has its own critique (CR-14).

**Residual risk:** With 5–9% power, the VR test labels ~91–95% of genuine mean-reverting periods as "Normal", causing the engine to use wider spreads than optimal. This is conservative (forfeits profit rather than incurring risk), but reduces competitiveness.

**Recommendation:** Increase the observation window to $n = 500$ (boosting power to ~25%) and reduce the VR thresholds to 0.90/1.10. Alternatively, use the Choi (1999) iterated-bootstrap VR test which has 2× the power at the same window size and does not require normality.

**Severity:** Remains **MEDIUM**.

---

### CR-6 (MEDIUM → LOW): Yang-Zhang Degeneration

**Prior challenge:** YZ estimator degenerates to close-to-close when >90% of blocks have degenerate (O=H=L=C) candles, which is typical for CHIA's sparse fills.

**Current status:** **Mitigated.** The `candle_aggregation_blocks = 10` setting (T5-CR6) aggregates 10 blocks (~8.7 min) into one OHLC candle, recovering H/L variation. At CHIA's current fill rate (~1 fill per 5 minutes per pair), ~80% of aggregated candles should have non-degenerate H/L.

**Residual risk:** For very low-activity pairs (e.g., wmilliETH/XCH, <1 fill/hour), aggregation may be insufficient. These pairs still get ~50% degenerate candles.

**Severity:** Downgraded from MEDIUM to **LOW**.

---

### CR-7 (MEDIUM → LOW): Thompson Sampling Staleness — Besbes et al. (2014)

**Prior challenge:** Standard Beta posterior is too slow to forget stale regime data. Arms are not independent (spread choice affects future fill distributions).

**Current status:** **Mitigated.** The engine now uses discounted Thompson Sampling (T5-CR10 / Besbes et al. 2014):

$$\alpha_{new} = \max(\alpha \cdot \gamma_{discount}, 1.0) + \mathbb{1}[\text{profit}]$$

with $\gamma_{discount} = 0.97$ (default). This exponentially discounts historical outcomes, weighting recent fills 30× more than fills 100 blocks ago.

**Residual risk:** The arm-independence assumption remains violated — choosing a wider spread reduces fill probability, which the Beta posterior interprets as evidence against that arm. However, the discount factor limits the damage: stale evidence from a different regime decays within ~100 blocks (~87 minutes).

**Severity:** Downgraded from MEDIUM to **LOW**.

---

### CR-8 (MEDIUM — unchanged): GLFT Continuous-vs-Discrete Control

**Prior challenge:** GLFT's continuous-time optimal control diverges from the true discrete-time optimal when fills arrive at CHIA's sparse rate (~1/5min rather than ~100/sec on equity markets).

**Current status:** The sparse-fill correction (T5-CR8, Fodra & Pham 2015) partially addresses this:

$$\delta_{adjusted} = \delta_{GLFT} \cdot \text{clamp}\left(\frac{f_{dense}}{f_{actual}}, 1, \text{cap}\right)$$

with `dense_rate = 100 fills/hr`, `actual_rate ≈ 12 fills/hr`, `cap = 10`.

**Residual risk:** The correction is multiplicative (spread widening only). It does not adjust the reservation price $r$, which remains optimal only under the continuous-time assumption. The inventory skew may still be suboptimal for CHIA's discrete fill cadence.

**Severity:** Remains **MEDIUM**.

---

### CR-9 (MEDIUM → LOW): Brock-Hommes β Calibration

**Prior challenge:** $\beta = 2.0$ intensity-of-choice lacks empirical justification. The model requires 30+ observations per evaluation window.

**Current status:** **Partially mitigated.** The `StrategyPortfolio` now implements sparse-fill damping (T3-26):

$$w_{effective} = w_{base} \cdot \min\left(1.0, \frac{n_{fills}}{n_{min}}\right)$$

with `min_fills_for_full_weight = 10`. This prevents strategy switching on insufficient data. The crowding cooldown (500 blocks) also limits frequency of regime-driven weight changes.

**Residual risk:** β = 2.0 is still empirically arbitrary. The fill-count damping addresses sample-size but not the calibration of β itself.

**Severity:** Downgraded from MEDIUM to **LOW**.

---

### CR-10 through CR-15: Unchanged

| ID | Prior Severity | Status | Notes |
|----|---------------|--------|-------|
| CR-10 | LOW | Unchanged | Kyle lambda: linear impact not empirically supported; square-root law preferred. Not used in production hot path. |
| CR-11 | LOW | Unchanged | LVR framework: applies to AMM LPs, not CLOB. TibetSwap arbitrage correctly uses constant-product math. |
| CR-12 | LOW | Unchanged | Farmer & Joshi crowding: 30% fill-rate-drop threshold lacks per-asset calibration. |
| CR-13 | MEDIUM | Unchanged | Glosten-Milgrom: spread attribution assumes adverse-selection dominance. On CHIA, order-processing costs likely dominate. |
| CR-14 | MEDIUM | Unchanged | HMM Baum-Welch: multiple local maxima. Optional path; VR is the primary classifier. |
| CR-15 | LOW | Unchanged | Amihud-Mendelson: static framework ignores liquidity dynamics. Correctly cited as theoretical motivation only. |

---

## Part B: New Challenges

### CR-16 — HIGH: Inventory Drift Under Persistent Adverse Selection

**Academic basis:** Cartea, Jaimungal & Penalva (2015) §10.6 — "Optimal Execution with Market Impact and Drift"; Guéant (2017) §6 — "Optimal Market Making".

**Challenge:** The A-S/GLFT models assume symmetric order arrival. When the true price drifts persistently (e.g., XCH trending down for weeks), the market maker accumulates an increasingly underwater long position because fills are predominantly asks (buyers taking the maker's sell offers) that are immediately replaced, while bids are not filled.

The `InventoryDriftAnalyzer` (drift_analyzer.hpp) models this with Monte Carlo simulation and time-to-breach forecasting. However, the analysis is advisory — it produces a `DriftReport` with a `RecommendedAction` that includes `ManualRebalance`, but the engine does not act on this automatically.

**Codebase evidence:**
- `InventoryDriftAnalyzer` correctly models trending-market drift: $\text{drift} = |\mu| \cdot t \cdot q_{ss}$ where $q_{ss}$ is the A-S steady-state inventory.
- The `StrategicLossManager` (loss_manager.hpp) can authorize deliberate losses for rebalancing but is disabled by default (`enabled = false`, `max_acceptable_loss_bps = 0.0`).
- The no-loss constraint at three layers (InventoryTracker, PreTradeCheck, per-strategy) prevents the system from ever selling at a loss.
- Inventory aging (T4-09) gradually relaxes the floor but is very slow: default `relax_rate_bps_per_block` accumulates ~170 bps per day.

**Residual risk:** In a sustained downtrend, the bot locks up capital in underwater inventory that aging takes days to free. The bot becomes a passive holder, not a market maker. This is the single largest operational risk for a never-loss-constrained system.

**Recommendation:** Enable a "circuit-breaker rebalance" mode: when inventory ratio exceeds hard_limit (80%) AND `DriftAnalyzer` recommends rebalance AND position age exceeds `2 × aging_start_blocks`, automatically enable the `StrategicLossManager` for that pair only, with a configurable maximum loss cap.

---

### CR-17 — MEDIUM: Blockchain Fee Estimation Feedback Loop

**Academic basis:** Roughgarden (2021) — "Transaction Fee Mechanism Design"; Huberman, Leshno & Moallemi (2021) — "Monopoly Without a Monopolist".

**Challenge:** The `FeeTracker` class uses a static `offer_fee_mojos` (100M mojos = 0.0001 XCH) with optional dynamic adjustment via `set_dynamic_fee()`. However, the fee estimation does not account for the market maker's own fee contribution to mempool congestion.

When the bot posts N offers per cycle across 5 pairs, it submits ~20–40 transactions per heartbeat. On a low-throughput blockchain (CHIA: ~1 MB / 52s), this can become a meaningful fraction of total mempool traffic, driving up fees via a self-referential feedback loop:
1. Bot sees high fees → widens spreads → fewer fills → bot cancels/reposts → more transactions → higher fees.

**Codebase evidence:**
- `fee_budget_daily_mojos` (default 10B mojos = 10 XCH/day) provides a hard cap on total fee spend.
- `fee_to_expected_gain_ratio` (default 0.25) prevents individual offer posting when fee > 25% of expected gain.
- But neither mechanism accounts for aggregate mempool impact.

**Recommendation:** Implement batch offer creation using the `create_offer_for_ids` RPC with multiple pairs in a single transaction, reducing per-cycle transaction count from ~40 to ~5–10.

---

### CR-18 — MEDIUM: Regime Detector Warm-Up Creates Blind Spot

**Academic basis:** This is a practical implementation concern, not a theoretical challenge.

**Challenge:** The `RegimeDetector` requires `min_window = 50` observations before producing a regime classification. During warm-up, all strategies default to `Regime::Normal` with unit multipliers ({1.0, 1.0, 1.0, 1.0}). Combined with the optional startup analysis phase (`startup_analysis_blocks`, default 0), the bot may begin trading during the first ~43 minutes (50 blocks × 52s) with no regime awareness.

If the bot starts during a momentum period (e.g., post-listing dump), it will use Normal-regime tight spreads, maximizing adverse selection exposure.

**Codebase evidence:**
- `strategies_` map creates per-pair `AvellanedaStoikov` or `GlftStrategy` instances at startup.
- Each strategy has its own `RegimeDetector` (or shares one via `set_regime_detector()`).
- The `startup_analysis_blocks` defaults to 0 ("skip analysis") — most operators probably leave this at 0 for faster startup.

**Recommendation:** When `RegimeDetector` is in warm-up, apply a defensive multiplier (e.g., 1.3× spread widening) rather than the neutral 1.0×. This is equivalent to assuming "momentum until proven otherwise" — a conservative default for an unobserved market.

---

### CR-19 — LOW: CoinGecko Price Latency vs DEX Price

**Academic basis:** Hasbrouck, Ait-Sahalia & Lo (1993) — "Nonsynchronous Trading"; Ait-Sahalia et al. (2005) — "Ultra-High Frequency".

**Challenge:** The 70/30 DEX/CEX price blend uses CoinGecko data that may be 30–60 seconds stale (free tier: 30 calls/min, 10-30s cache). On a 52-second block time, the CEX price could be from the previous block. The blend:

$$\text{mid} = 0.70 \cdot p_{dex} + 0.30 \cdot p_{cex(\text{stale})}$$

introduces a momentum-smoothing bias: during rapid price moves, the blended mid lags the true price, causing the bot to post offers at stale levels.

**Codebase evidence:**
- `CoinGeckoClient` has a `polling_interval_ms` floor of 5000ms (config.cpp validation).
- The staleness gradient (T3-06) widens spreads as data ages, but only kick in at 50% of `kStaleThreshold` (= 2.5 minutes). A 30-second lag is well below this threshold.

**Residual risk:** LOW because the CEX weight is only 30% and dexie.space orderbook data is primary. The maximum price error is $0.30 \times |\Delta p_{cex}|$ where $\Delta p_{cex}$ is the CEX price move during the stale interval.

**Recommendation:** Weight CEX data by freshness: $w_{cex} = w_{base} \cdot \max(0, 1 - \text{age} / \text{threshold})$.

---

### CR-20 — LOW: Constant-Product AMM Fee Assumption for TibetSwap

**Academic basis:** Adams et al. (2021, 2023) — Uniswap v2/v3/v4; Angeris et al. (2022) — "Optimal Fees for Geometric Mean Market Makers".

**Challenge:** The arbitrage detector uses a hardcoded `INVERSE_FEE = 993` for TibetSwap (0.7% fee). If TibetSwap changes its fee structure (as Uniswap did from v2 to v3), the arbitrage edge calculations become incorrect, potentially triggering false-positive arb signals or missing genuine opportunities.

**Codebase evidence:**
- `tibet::get_output_amount()` in `arbitrage.hpp` hardcodes `INVERSE_FEE = 993`.
- The `ArbitrageConfig` has a `tibetswap_fee_bps = 70` in YAML config, but this is only used for net-edge calculation — not passed to the AMM math.

**Recommendation:** Replace the hardcoded `INVERSE_FEE` with a config-derived value: `INVERSE_FEE = 1000 - config.tibetswap_fee_bps / 10`.

---

## Summary Table

| ID | Severity | Module | Challenge | Status |
|----|----------|--------|-----------|--------|
| CR-1 | ~~HIGH~~ → **MEDIUM** | engine.cpp | VPIN has no incremental predictive power | Partially mitigated (validation gate) |
| CR-2 | **HIGH** | market_data | Best-level OFI misses 10–30% return variance | Not mitigated |
| CR-3 | ~~HIGH~~ → **MEDIUM** | avellaneda.cpp | Sawtooth tau exploitability | Mitigated (fill-decay tau) |
| CR-4 | **MEDIUM** | adverse_selection | PIN conflates illiquidity with informed trading | Not mitigated |
| CR-5 | **MEDIUM** | regime.cpp | VR test has 5–9% power at n=200 | Not mitigated |
| CR-6 | ~~MEDIUM~~ → **LOW** | volatility | YZ degeneration with sparse fills | Mitigated (candle aggregation) |
| CR-7 | ~~MEDIUM~~ → **LOW** | spread.cpp | Thompson Sampling posterior staleness | Mitigated (discounted TS) |
| CR-8 | **MEDIUM** | avellaneda.cpp | GLFT continuous-vs-discrete control | Partially mitigated (sparse correction) |
| CR-9 | ~~MEDIUM~~ → **LOW** | strategy_portfolio | Brock-Hommes β calibration | Partially mitigated (fill-count damping) |
| CR-10 | LOW | — | Kyle linear impact | Not used in hot path |
| CR-11 | LOW | arbitrage | LVR framework applicability | Correctly scoped |
| CR-12 | LOW | strategy_portfolio | Crowding threshold calibration | Not mitigated |
| CR-13 | **MEDIUM** | spread.cpp | G-M adverse-selection dominance assumption | Not mitigated |
| CR-14 | **MEDIUM** | regime.cpp | HMM local maxima | Not mitigated (optional path) |
| CR-15 | LOW | — | Amihud-Mendelson static framework | Used as motivation only |
| **CR-16** | **HIGH** | engine.cpp, risk/ | Inventory drift under persistent adverse selection | **New** — not mitigated |
| **CR-17** | **MEDIUM** | fee_tracker | Fee estimation feedback loop | **New** — partially bounded by budget |
| **CR-18** | **MEDIUM** | regime.cpp | Regime detector warm-up blind spot | **New** — not mitigated |
| **CR-19** | LOW | market_data | CoinGecko price latency bias | **New** — mitigated by low weight |
| **CR-20** | LOW | arbitrage | Hardcoded TibetSwap fee constant | **New** — not mitigated |

---

## Aggregate Risk Assessment

| Severity | Prior | Current | Mitigated | New | Open |
|----------|-------|---------|-----------|-----|------|
| HIGH | 3 | 2 | 1 (CR-3 → MEDIUM) | 1 (CR-16) | 2 (CR-2, CR-16) |
| MEDIUM | 8 | 9 | 2 (CR-6 → LOW, CR-7 → LOW) | 3 (CR-17, CR-18, + CR-1 downgrade) | 7 |
| LOW | 4 | 6 | — | 2 (CR-19, CR-20) | 6 |
| **Total** | **15** | **20** | **5 downgraded** | **5 new** | **15 open** |

---

## Recommended Priority Order

1. **CR-16** (HIGH) — Inventory drift: enable automatic strategic loss for aged, high-inventory positions
2. **CR-2** (HIGH) — Multi-level OFI: extend OFI ingest to top-5 book levels
3. **CR-1** (MEDIUM) — Adaptive VPIN gain: auto-attenuate based on measured precision
4. **CR-17** (MEDIUM) — Batch offer creation to reduce mempool impact
5. **CR-18** (MEDIUM) — Defensive warm-up spread multiplier during regime detector burn-in

---

## Academic References (New)

| # | Citation | Relevance |
|---|----------|-----------|
| 63 | Cartea, Jaimungal & Penalva (2015), "Algorithmic and High-Frequency Trading", Ch. 10.6 | Inventory drift under persistent adverse selection |
| 64 | Guéant (2017), "Optimal Market Making", §6 | Drift-aware optimal control |
| 65 | Roughgarden (2021), "Transaction Fee Mechanism Design" | Fee feedback loops |
| 66 | Huberman, Leshno & Moallemi (2021), "Monopoly Without a Monopolist" | Blockchain fee market dynamics |
| 67 | Hasbrouck (1993), "Nonsynchronous Trading" | Stale-price blending bias |
| 68 | Angeris et al. (2022), "Optimal Fees for Geometric Mean Market Makers" | Dynamic AMM fee structures |
| 69 | Choi (1999), "Testing the Random Walk Hypothesis for Real Exchange Rates" | Iterated bootstrap VR test with higher power |
