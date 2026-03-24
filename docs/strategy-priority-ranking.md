# XOPTrader Strategy Priority Ranking

> Issue #9: Rigorous prioritization of all 38 strategies by ROI-per-effort,
> calibrated to the reality of CHIA DEX at ~$2K/day volume.
>
> Generated 2026-03-24 | Reviewed against engine.hpp wiring and profitability model.

---

## Methodology

Each strategy is scored on five axes:

| Axis | Weight | Scale |
|------|--------|-------|
| **Marginal PnL** (edge in bps at current volume) | 30% | 0-100 bps |
| **Implementation effort** (days of work) | 25% | Lower is better |
| **Risk of negative impact** (could it lose money?) | 20% | 0=safe, 5=dangerous |
| **Dependency chain** (what must exist first?) | 15% | 0=standalone, 3=deep chain |
| **Capital requirement** (extra capital needed?) | 10% | 0=none, 3=significant |

**Key reality check**: At $2K/day DEX volume, the entire daily revenue opportunity
is approximately $10-60 (at 50-300 bps capture rate). Any strategy that costs more
than 2-3 days to implement must justify itself by either (a) protecting existing
PnL, (b) being required infrastructure for other strategies, or (c) preparing for
volume growth (June 2026 hard fork catalyst).

**Core constraint**: NEVER SELL AT A LOSS is hardcoded. All rankings respect this.

---

## Tier 1: IMPLEMENT NOW (Highest ROI per Effort)

These are either already implemented and need tuning, or are low-effort additions
that directly increase PnL or prevent losses at current volume levels.

### 1. Lead-Lag Signals from CEX Price Feeds (3.3)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Unimplemented |
| **Effort** | 2-3 days |
| **Estimated edge** | 50-150 bps per adverse-selection-avoided cycle |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | MarketDataFeed (exists), CEX websocket client (partially exists via OKX feed) |
| **Capital requirement** | None |

**Justification**: This is the single highest-ROI unimplemented strategy. CEX
trades $2.4M/day vs DEX $2K/day -- CEX price moves lead DEX by minutes to hours.
Simply shifting the reference mid-price to the CEX TWAP prevents the most common
and most expensive failure mode: quoting stale prices that get picked off by
informed takers. The MarketDataFeed already ingests CEX data for arbitrage
detection; this just uses it as the primary fair-value anchor instead of the thin
DEX book. At current spreads of 300-1000 bps on DEX, avoiding even one stale-quote
snipe per day pays for itself.

---

### 2. Oracle-Based Fair-Value Anchoring (3.11)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Unimplemented |
| **Effort** | 1-2 days |
| **Estimated edge** | 30-80 bps (adverse selection reduction) |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | MarketDataFeed (exists) |
| **Capital requirement** | None |

**Justification**: Overlaps heavily with Lead-Lag (3.3) and should be implemented
together as a single "CEX-anchored fair value" module. On a DEX with $2K/day
volume, the on-chain order book is trivially manipulable. Anchoring mid-price to
aggregated CEX TWAP is not optional -- it is a prerequisite for every other
strategy to function correctly. Without it, A-S and GLFT compute quotes around a
manipulated mid-price. This is 1-2 days of work because the CEX feed already
exists; the change is making it authoritative.

---

### 3. Volatility Forecasting -- GARCH (3.2)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Unimplemented |
| **Effort** | 2-3 days |
| **Estimated edge** | 20-50 bps (tighter spreads in calm, wider in storm) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | VolatilityEstimator (exists -- Yang-Zhang), CEX price feed |
| **Capital requirement** | None |

**Justification**: The existing Yang-Zhang estimator is backward-looking. A
GARCH(1,1) on CEX block-level returns adds forward-looking volatility that
directly feeds into every spread computation. On a market with 3-10% spreads,
switching from trailing to predictive volatility lets us tighten 20-30% during
calm periods (capturing more fills) and widen pre-emptively before realized vol
confirms (avoiding adverse selection). GARCH(1,1) is ~50 lines of code on top
of the existing vol infrastructure. The CEX return series has 1000x more data
points than the DEX, making the GARCH fit robust.

---

### 4. Avellaneda-Stoikov Tuning (1.1) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Implemented |
| **Effort** | 1-2 days (parameter calibration, not code) |
| **Estimated edge** | 10-30 bps (from better kappa/gamma calibration) |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | Backtesting framework, live fill data |
| **Capital requirement** | None |

**Justification**: The A-S implementation exists but the fill-rate parameter
kappa and risk-aversion gamma are likely initialized from textbook defaults, not
Chia-specific data. At $2K/day volume with ~52-second blocks, the fill intensity
curve is radically different from traditional equities. A systematic parameter
sweep using GPU backtesting (which the user has infrastructure for) can
meaningfully improve quote placement. This is calibration work, not new code.

---

### 5. GLFT Tuning (1.2) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Implemented |
| **Effort** | 1-2 days (parameter calibration alongside A-S) |
| **Estimated edge** | 10-30 bps |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | Same as A-S tuning |
| **Capital requirement** | None |

**Justification**: GLFT's phi and gamma need Chia-specific calibration. Since
GLFT is the preferred model for 24/7 markets (no session end), getting its
parameters right is arguably more important than A-S tuning. Run both sweeps
together.

---

### 6. Maker-Taker Fee Optimization (3.4)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Unimplemented |
| **Effort** | 0.5-1 day |
| **Estimated edge** | 5-15 bps (routing savings) |
| **Risk of negative impact** | None (0/5) |
| **Dependencies** | Multi-venue support (Dexie client exists; TibetSwap needs adapter) |
| **Capital requirement** | None |

**Justification**: Dexie charges 0% for native offers but 1% on Combined Swap.
TibetSwap charges 0.7%. Hashgreen charges 0.9%. Simply routing passive orders to
Dexie and using TibetSwap only for rebalancing or AMM LP saves 70-100 bps per
TibetSwap interaction. This is a routing table, not an algorithm. Half a day of
work for a permanent fee reduction on every trade that touches non-Dexie venues.

---

### 7. Thompson Sampling Tuning (1.12) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Implemented |
| **Effort** | 1 day |
| **Estimated edge** | 10-25 bps (from grid refinement and prior tuning) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | Sufficient fill history |
| **Capital requirement** | None |

**Justification**: Thompson Sampling is implemented but may have a too-coarse
spread grid or inappropriate priors for Chia's wide-spread environment. The
default grid likely covers 10-100 bps, but optimal Chia spreads may be 100-500
bps given the thin book. Adjusting the grid range and bucket count to match
reality, plus warming up the Beta priors with simulated data, takes a day and
meaningfully improves convergence speed.

---

### 8. Latency Arbitrage Defense (3.7)

| Attribute | Value |
|-----------|-------|
| **Tier** | 1 |
| **Status** | Unimplemented |
| **Effort** | 2-3 days |
| **Estimated edge** | 30-100 bps (loss prevention, not revenue generation) |
| **Risk of negative impact** | Low (1/5) -- may over-cancel in volatile markets |
| **Dependencies** | Lead-Lag / CEX feed (Strategy #1 above) |
| **Capital requirement** | None |

**Justification**: Once CEX-anchored fair value is live (Strategy #1), this
becomes a natural extension: if CEX mid moves >X bps since our last quote
refresh, immediately cancel outstanding offers before they get sniped. On Chia's
52-second blocks, a 5% CEX price move gives an arbitrageur a full block to take
our stale offer. This is the defensive complement to Lead-Lag's offensive
improvement. At current spreads of 300-1000 bps the threshold is generous, but
as we tighten spreads to dominate, stale-quote sniping becomes the primary PnL
leak. Build this now so it is ready when spreads tighten.

---

## Tier 2: IMPLEMENT NEXT (Good ROI, Moderate Effort)

These strategies have clear value but require more work, more data, or depend
on Tier 1 items being complete first.

### 9. Dynamic Position Limits -- VaR/CVaR (3.5)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Unimplemented |
| **Effort** | 3-4 days |
| **Estimated edge** | 10-20 bps (risk reduction, not direct PnL) |
| **Risk of negative impact** | Low (1/5) -- may over-restrict in volatile periods |
| **Dependencies** | GARCH volatility forecast (Tier 1 #3), InventoryTracker (exists) |
| **Capital requirement** | None |

**Justification**: Static q_max limits are a blunt instrument. When vol spikes,
the same dollar position limit carries 2-3x more risk. VaR-scaled limits
automatically reduce exposure during high-vol regimes and relax during calm,
improving capital utilization. Depends on GARCH being live for the vol forecast.
Worth 3-4 days once the vol infrastructure is upgraded.

---

### 10. Cartea-Jaimungal Alpha Signal (2.3)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Considered |
| **Effort** | 3-5 days |
| **Estimated edge** | 15-40 bps (improved quote asymmetry) |
| **Risk of negative impact** | Medium (2/5) -- overfitting risk on sparse data |
| **Dependencies** | OFI (implemented), VPIN (implemented), Lead-Lag CEX feed (Tier 1 #1) |
| **Capital requirement** | None |

**Justification**: Formalizes the directional signal that OFI and VPIN already
partially capture into an explicit next-block price predictor. The alpha signal
shifts the reservation price: alpha > 0 means "price going up, widen ask, tighten
bid." OFI and VPIN are already wired as multipliers, but a proper alpha model
integrates them into the A-S/GLFT reservation price formula rather than applying
them as post-hoc multipliers. The risk is overfitting on sparse DEX data, but
using CEX returns as the dependent variable (abundant data) mitigates this.

---

### 11. Cross-Platform Arbitrage Tuning (1.6) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Implemented |
| **Effort** | 2-3 days (threshold tuning, execution path optimization) |
| **Estimated edge** | 50-200 bps per arb cycle |
| **Risk of negative impact** | Low (1/5) -- only executes when profitable |
| **Dependencies** | CEX API integration, Dexie/TibetSwap adapters |
| **Capital requirement** | Moderate -- needs capital on both CEX and DEX sides |

**Justification**: The arb engine is implemented but needs live calibration of
thresholds (minimum edge to execute, slippage buffers, bridge fee accounting).
CEX-DEX arb at 50-200 bps per cycle is the second-largest revenue source after
spread capture. At $2.4M CEX daily volume, even capturing 0.1% of volume as arb
profit is $2,400/day -- more than the entire DEX volume. However, execution on
Chia's 52-second blocks means arb opportunities may close before settlement.
Focus on arb where DEX price is persistently stale (slow mean-reversion), not
on fleeting CEX microstructure.

---

### 12. Regime Detection Tuning (1.5) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Implemented |
| **Effort** | 2-3 days (window tuning, HMM re-calibration) |
| **Estimated edge** | 10-20 bps (better spread adaptation) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | GARCH vol (Tier 1 #3) improves regime accuracy |
| **Capital requirement** | None |

**Justification**: The variance-ratio test and HMM are implemented, but the
lookback window and HMM transition probabilities need calibration to Chia's
block cadence. At 52-second blocks, a 50-block window covers ~43 minutes -- this
may be too short or too long. The June 2026 hard fork will create a definitive
regime change; having well-calibrated detection ready for that event is valuable.

---

### 13. Impermanent Loss Hedging for AMM LP (3.13)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Unimplemented |
| **Effort** | 5-7 days |
| **Estimated edge** | 15-40 bps on AMM capital (IL reduction) |
| **Risk of negative impact** | Medium (2/5) -- hedge cost may exceed IL benefit |
| **Dependencies** | TibetSwap LP integration, hedging framework (exists) |
| **Capital requirement** | Moderate -- needs capital in AMM pools |

**Justification**: The profitability model allocates 30% of capital to TibetSwap
AMM (passive fee income). At 50K+ XCH TVL in TibetSwap, IL is a real risk when
XCH moves 10%+ from entry. A delta-neutral hedge using the offer book (sell XCH
offers when AMM position is long XCH) reduces IL without touching the AMM
position. This matters because the never-sell-at-loss constraint means underwater
AMM positions cannot be exited -- hedging prevents them from becoming underwater
in the first place. 5-7 days because it requires modeling TibetSwap's
constant-product curve and computing the hedge ratio per block.

---

### 14. Kyle Lambda / Permanent Price Impact (2.4)

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Considered |
| **Effort** | 2-3 days |
| **Estimated edge** | 10-20 bps (better sizing for large trades) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | Trade database (exists), CEX data for regression |
| **Capital requirement** | None |

**Justification**: A simple linear regression of delta-P on delta-Q from CEX data
(abundant observations) gives a permanent impact coefficient that can scale offer
sizes on DEX. When lambda is high (large trades move price permanently), reduce
offer sizes to avoid being on the wrong side. At $2K/day DEX volume this is less
critical, but it costs only 2-3 days and becomes important as volume grows. The
regression itself runs on CEX data where N is large enough for statistical
significance.

---

### 15. Whale Detection Tuning (1.8) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Implemented |
| **Effort** | 1 day |
| **Estimated edge** | 10-20 bps (better whale threshold calibration) |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | None |
| **Capital requirement** | None |

**Justification**: The 50 XCH / 5% volume thresholds are defaults. At $2K/day
DEX volume (~740 XCH), a 50 XCH trade is 6.8% of daily volume -- possibly too
high a threshold. Lowering to 20-30 XCH may catch more informed flow. The
10-block window (~8.7 min) may also need adjustment. One day of analysis and
threshold tuning.

---

### 16. VPIN Tuning (1.9) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Implemented |
| **Effort** | 1 day |
| **Estimated edge** | 5-15 bps |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | None |
| **Capital requirement** | None |

**Justification**: The default 10 XCH bucket size and 50-bucket window need
validation against actual Chia DEX trade sizes. At $2K/day volume, 10 XCH
(~$27) buckets may fill too slowly for VPIN to be responsive. Consider reducing
to 3-5 XCH buckets. Quick tuning pass.

---

### 17. OFI Tuning (1.10) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Implemented |
| **Effort** | 1 day |
| **Estimated edge** | 5-15 bps |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | None |
| **Capital requirement** | None |

**Justification**: OFI's 20-snapshot window (~17 min) and normalization constants
need validation. On a thin DEX book, OFI may saturate frequently (single large
order flips the entire signal). May need wider normalization bounds or
volume-weighted dampening. One day of analysis.

---

### 18. Competitor Detection Tuning (1.7) -- ALREADY IMPLEMENTED

| Attribute | Value |
|-----------|-------|
| **Tier** | 2 |
| **Status** | Implemented |
| **Effort** | 0.5 day |
| **Estimated edge** | 5-10 bps (avoid quoting into the void) |
| **Risk of negative impact** | Very low (0/5) |
| **Dependencies** | None |
| **Capital requirement** | None |

**Justification**: Currently near-zero competition, so this is mostly a sentinel.
Ensure alerts fire correctly so we know the moment a competitor appears. The
50 bps alert threshold is reasonable. Half-day verification pass.

---

## Tier 3: DEFER (Low ROI at Current Volumes, or High Effort)

These strategies are either over-engineered for $2K/day volume, require
infrastructure that does not yet exist, or have unfavorable effort-to-edge
ratios. Revisit when volume exceeds $10K/day or after the June 2026 hard fork.

### 19. Inventory-Aware Hedging via CEX Positions (3.1)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Unimplemented |
| **Effort** | 7-10 days |
| **Estimated edge** | 20-50 bps (risk reduction) |
| **Risk of negative impact** | Medium (2/5) -- cross-venue settlement risk |
| **Dependencies** | CEX API with trading permissions, capital on CEX |
| **Capital requirement** | High -- doubles capital requirement (CEX + DEX) |

**Justification**: The textbook market-maker hedge. But at $2K/day DEX volume
with $10K capital, the inventory risk is already small in dollar terms. Hedging
on CEX requires maintaining capital on both sides, managing API keys for exchange
trading, and dealing with cross-venue settlement timing. The operational
complexity is disproportionate to the risk being hedged. Revisit when DEX volume
exceeds $10K/day or capital exceeds $30K.

---

### 20. Bayesian PIN (2.1)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Considered |
| **Effort** | 3-5 days |
| **Estimated edge** | 5-15 bps (marginally better than VPIN) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | Trade classification (ambiguous on Chia DEX) |
| **Capital requirement** | None |

**Justification**: VPIN was specifically designed as a practical, real-time
approximation of PIN. The marginal improvement of full Bayesian PIN over VPIN
is small and the MLE estimation is numerically unstable with sparse Chia data.
At ~40 trades/day on DEX, the PIN model's Poisson arrival assumptions break
down entirely. VPIN is good enough; spend the 3-5 days on Lead-Lag instead.

---

### 21. Predatory Trading Defense (2.2)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Considered |
| **Effort** | 4-6 days |
| **Estimated edge** | 5-15 bps (loss prevention in rare scenarios) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | Whale detection (implemented), inventory visibility analysis |
| **Capital requirement** | None |

**Justification**: Predatory trading requires (a) a predator and (b) a visible
distressed position. With near-zero competition on CHIA DEX, there are no
predators. Our inventory is visible on-chain, but there is nobody sophisticated
enough to exploit it. Whale detection + asymmetric widening already provides
80% of the defensive value. Revisit if competition appears.

---

### 22. Stochastic Optimal Control -- Guilbaud-Pham (2.5)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Considered |
| **Effort** | 10-15 days |
| **Estimated edge** | 10-25 bps (theoretical; may not survive discretization) |
| **Risk of negative impact** | Medium (2/5) -- numerical instability in HJB solver |
| **Dependencies** | None (replaces A-S/GLFT) |
| **Capital requirement** | None |

**Justification**: Solving the HJB PDE for joint limit/market order optimization
is a research project, not a production task. The model assumes continuous order
books and sub-second execution -- neither exists on Chia. A-S and GLFT, properly
calibrated, achieve 90%+ of the theoretical optimum at 1% of the implementation
cost. The 10-15 day estimate is optimistic; numerical stability testing alone
could take a week.

---

### 23. Multi-Asset Joint Quoting (3.8)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Unimplemented |
| **Effort** | 7-10 days |
| **Estimated edge** | 10-20 bps (cross-pair risk reduction) |
| **Risk of negative impact** | Medium (2/5) -- correlation estimation on sparse data |
| **Dependencies** | Correlation estimator, multi-pair quoting infrastructure |
| **Capital requirement** | Moderate -- needs capital across multiple pairs |

**Justification**: Joint quoting across correlated pairs is valuable when you
have 10+ liquid pairs with well-estimated correlations. CHIA DEX has 2-3 pairs
with meaningful volume (XCH/wUSDC, SBX/XCH, DBX/XCH). Estimating correlations
from ~40 trades/day produces noise, not signal. The hedging framework's
portfolio-level netting (Layer 3, already implemented) captures most of the
cross-pair benefit without the modeling complexity. Revisit when 5+ pairs
each have >$500/day volume.

---

### 24. Toxic Flow Classification -- ML-based (3.6)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Unimplemented |
| **Effort** | 10-14 days |
| **Estimated edge** | 10-25 bps (better than VPIN for flow classification) |
| **Risk of negative impact** | Medium (2/5) -- model drift, training data labeling |
| **Dependencies** | Labeled training data (does not exist), ML inference pipeline |
| **Capital requirement** | None |

**Justification**: The fundamental problem: there is no labeled training data.
Labeling a trade as "informed" or "uninformed" requires knowing the future price
path, which makes this a hindsight exercise. At ~40 DEX trades/day, even 6
months of history gives ~7,200 samples -- insufficient for any meaningful ML
model. VPIN + OFI + whale detection already capture the main signal. Spend 10-14
days on this only after volume grows 10x and sufficient labeled data accumulates.

---

### 25. Cross-Chain Bridge Arbitrage (3.10)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Unimplemented |
| **Effort** | 7-10 days |
| **Estimated edge** | 20-80 bps per cycle (but very infrequent) |
| **Risk of negative impact** | High (3/5) -- bridge exploit risk, settlement delays |
| **Dependencies** | warp.green or Portal bridge integration, multi-chain wallet |
| **Capital requirement** | High -- needs capital on both chains |

**Justification**: wUSDC (ETH) vs wUSDC.b (Base) arb is real but infrequent.
Bridge settlement takes 15-60 minutes, capital is locked during transit, and
bridge exploits are a non-trivial smart contract risk. The arb engine already
handles cross-bridge pricing detection (Strategy 1.6). Full execution across
chains requires multi-chain infrastructure that does not exist in the codebase.
7-10 days of work for an opportunity that may arise once per week.

---

### 26. Quote Stuffing / Spoofing Detection (3.12)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Unimplemented |
| **Effort** | 4-6 days |
| **Estimated edge** | 0-5 bps (defensive only, very rare on CHIA) |
| **Risk of negative impact** | Low (1/5) -- false positives cause missed fills |
| **Dependencies** | Order book history database |
| **Capital requirement** | None |

**Justification**: Spoofing requires a liquid market and multiple participants.
CHIA DEX has neither. On-chain offer creation costs real (if tiny) fees and
takes 52 seconds to confirm, making quote stuffing impractical. This is a
compliance checkbox, not a PnL driver. Build it for regulatory goodwill when
the ecosystem matures, not now.

---

### 27. Market Microstructure Invariance (3.15)

| Attribute | Value |
|-----------|-------|
| **Tier** | 3 |
| **Status** | Unimplemented |
| **Effort** | 5-7 days |
| **Estimated edge** | 3-8 bps (principled threshold calibration) |
| **Risk of negative impact** | Low (1/5) |
| **Dependencies** | Cross-sectional market data, calibration framework |
| **Capital requirement** | None |

**Justification**: Using the Kyle-Obizhaeva invariance hypothesis to auto-
calibrate whale thresholds and VPIN bucket sizes is elegant but the hypothesis
itself is debated. At $2K/day volume, hand-tuned thresholds based on common
sense (e.g., "a trade >5% of daily volume is big") work fine. The invariance
framework adds complexity for marginal calibration improvement. Revisit when
operating across 10+ pairs where manual tuning becomes infeasible.

---

## Tier 4: NEVER (Theoretically Interesting but Impractical for CHIA)

These strategies are either architecturally incompatible with CHIA's offer
system, require infrastructure that will never exist on CHIA, or have negative
expected value at any realistic volume level.

### 28. Batch Auction Strategies (3.14)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 |
| **Status** | Unimplemented |
| **Effort** | 10-15 days |
| **Estimated edge** | 0 bps (mechanism does not exist on CHIA) |
| **Risk of negative impact** | N/A |
| **Dependencies** | CHIA protocol change to batch auctions (not planned) |
| **Capital requirement** | None |

**Justification**: CHIA uses an offer/take model, not batch auctions. There is
no proposal (CHIP or otherwise) to introduce batch auctions. CHIP-0052 (partial
offers) moves toward an on-chain order book, which is the opposite direction from
batch auctions. Building batch auction strategy code is writing software for a
market that does not and likely will not exist on CHIA. Pure waste.

---

### 29. Adaptive Market-Making with Reinforcement Learning (3.9)

| Attribute | Value |
|-----------|-------|
| **Tier** | 4 |
| **Status** | Unimplemented |
| **Effort** | 30-60 days |
| **Estimated edge** | Unknown (could be negative) |
| **Risk of negative impact** | High (4/5) -- black box, out-of-distribution failure |
| **Dependencies** | Chia DEX simulator (does not exist), RL training infrastructure |
| **Capital requirement** | None |

**Justification**: RL market-making requires (a) a high-fidelity simulator for
training (does not exist for CHIA), (b) millions of training episodes (at 40
trades/day, this is 70+ years of real data), and (c) careful reward shaping that
respects the never-sell-at-loss constraint (non-trivial to encode). Even if built,
the agent would train on simulated data of a $2K/day market -- the simulation
fidelity is inherently low because there is so little real data to calibrate
against. Thompson Sampling already provides bandit-style learning with provable
regret bounds and full explainability. RL is a research project with negative
expected ROI at current scale.

---

## Summary Table

| # | Strategy | Tier | Status | Effort (days) | Edge (bps) | Dep. on |
|---|----------|------|--------|---------------|------------|---------|
| 1 | Lead-Lag CEX Signals (3.3) | 1 | New | 2-3 | 50-150 | MarketDataFeed |
| 2 | Oracle Fair-Value Anchoring (3.11) | 1 | New | 1-2 | 30-80 | MarketDataFeed |
| 3 | GARCH Volatility Forecast (3.2) | 1 | New | 2-3 | 20-50 | VolatilityEstimator |
| 4 | A-S Tuning (1.1) | 1 | Tune | 1-2 | 10-30 | Backtest framework |
| 5 | GLFT Tuning (1.2) | 1 | Tune | 1-2 | 10-30 | Backtest framework |
| 6 | Fee Optimization (3.4) | 1 | New | 0.5-1 | 5-15 | None |
| 7 | Thompson Sampling Tuning (1.12) | 1 | Tune | 1 | 10-25 | Fill history |
| 8 | Latency Arb Defense (3.7) | 1 | New | 2-3 | 30-100 | Lead-Lag (#1) |
| 9 | VaR/CVaR Position Limits (3.5) | 2 | New | 3-4 | 10-20 | GARCH (#3) |
| 10 | C-J Alpha Signal (2.3) | 2 | New | 3-5 | 15-40 | OFI, VPIN, Lead-Lag |
| 11 | Arb Engine Tuning (1.6) | 2 | Tune | 2-3 | 50-200 | CEX API |
| 12 | Regime Detection Tuning (1.5) | 2 | Tune | 2-3 | 10-20 | GARCH (#3) |
| 13 | IL Hedging for AMM (3.13) | 2 | New | 5-7 | 15-40 | TibetSwap LP |
| 14 | Kyle Lambda (2.4) | 2 | New | 2-3 | 10-20 | Trade database |
| 15 | Whale Detection Tuning (1.8) | 2 | Tune | 1 | 10-20 | None |
| 16 | VPIN Tuning (1.9) | 2 | Tune | 1 | 5-15 | None |
| 17 | OFI Tuning (1.10) | 2 | Tune | 1 | 5-15 | None |
| 18 | Competitor Detection Tune (1.7) | 2 | Tune | 0.5 | 5-10 | None |
| 19 | CEX Hedging (3.1) | 3 | New | 7-10 | 20-50 | CEX API + capital |
| 20 | Bayesian PIN (2.1) | 3 | New | 3-5 | 5-15 | Trade classification |
| 21 | Predatory Defense (2.2) | 3 | New | 4-6 | 5-15 | Whale detection |
| 22 | SOC Guilbaud-Pham (2.5) | 3 | New | 10-15 | 10-25 | None |
| 23 | Multi-Asset Joint Quoting (3.8) | 3 | New | 7-10 | 10-20 | Correlation est. |
| 24 | Toxic Flow ML (3.6) | 3 | New | 10-14 | 10-25 | Labeled data |
| 25 | Cross-Chain Bridge Arb (3.10) | 3 | New | 7-10 | 20-80 | Multi-chain infra |
| 26 | Spoofing Detection (3.12) | 3 | New | 4-6 | 0-5 | Order book DB |
| 27 | Microstructure Invariance (3.15) | 3 | New | 5-7 | 3-8 | Cross-sectional data |
| 28 | Batch Auction (3.14) | 4 | New | 10-15 | 0 | Protocol change |
| 29 | RL Market Making (3.9) | 4 | New | 30-60 | Unknown | DEX simulator |

**Already-implemented strategies without separate tuning entries** (functioning
as designed, tuning bundled with the system above):

| # | Strategy | Tier | Notes |
|---|----------|------|-------|
| 30 | Four-Component Spread (1.3) | 1 | Core pipeline; tuned via components above |
| 31 | Multi-Tier Liquidity (1.4) | 1 | Core pipeline; tuned via Thompson Sampling |
| 32 | Asymmetric Spread Widening (1.11) | 1 | Core pipeline; tuned via whale detection |

---

## Dependency Graph

```
                    +-----------------------+
                    |   CEX Price Feed      |
                    |   (MarketDataFeed)    |
                    +-----------+-----------+
                                |
                +---------------+---------------+
                |               |               |
                v               v               v
        +-------+------+ +-----+------+ +------+--------+
        | Lead-Lag     | | GARCH Vol  | | Oracle Fair   |
        | Signals (#1) | | Fore (#3)  | | Value (#2)    |
        +-------+------+ +-----+------+ +------+--------+
                |               |               |
        +-------+       +------+------+         |
        |               |             |         |
        v               v             v         |
 +------+-------+ +-----+------+ +---+--------+ |
 | Latency Arb  | | VaR/CVaR   | | Regime     | |
 | Defense (#8) | | Limits (#9)| | Tune (#12) | |
 +--------------+ +-----+------+ +---+--------+ |
                        |             |          |
                        v             v          |
                  +-----+------+ +---+--------+  |
                  | C-J Alpha  | | A-S/GLFT   +--+
                  | Signal(#10)| | Tuning     |
                  +-----+------+ | (#4/#5)    |
                        |        +---+--------+
                        |            |
                        v            v
                  +-----+------------+-----+
                  |   Final Quote Pipeline |
                  |   (Spread Opt + Tiers) |
                  +------------------------+
                              |
                              v
                  +-----------+------------+
                  |   Thompson Sampling    |
                  |   Tuning (#7)          |
                  +------------------------+

 Independent (no dependencies above):
   +----------------+  +------------------+  +------------------+
   | Fee Opt (#6)   |  | Whale Tune (#15) |  | VPIN Tune (#16)  |
   +----------------+  +------------------+  +------------------+
   +----------------+  +------------------+
   | OFI Tune (#17) |  | Comp Tune (#18)  |
   +----------------+  +------------------+

 Arb branch (semi-independent):
   +-----------+      +-------------------+
   | CEX API   +----->| Arb Tune (#11)    |
   | (existing)|      +--------+----------+
   +-----------+               |
                               v
                      +--------+----------+
                      | CEX Hedging (#19) | (Tier 3)
                      +-------------------+

 AMM branch:
   +------------------+      +-----------------+
   | TibetSwap LP     +----->| IL Hedging (#13)|
   | Integration      |      +-----------------+
   +------------------+
```

### Critical Path

The highest-value implementation sequence is:

```
Week 1:  Lead-Lag (#1) + Oracle (#2) + Fee Opt (#6)
         [All independent, can be done in parallel, ~4 days total]

Week 2:  GARCH Vol (#3) + A-S/GLFT Tuning (#4/#5) + Thompson Tuning (#7)
         [GARCH depends on CEX feed from Week 1, tuning is independent]

Week 3:  Latency Defense (#8) + Whale/VPIN/OFI/Competitor Tuning (#15-18)
         [Defense depends on Lead-Lag from Week 1, tuning is independent]

Week 4:  VaR Limits (#9) + Regime Tuning (#12)
         [Both depend on GARCH from Week 2]

Week 5+: C-J Alpha (#10), Arb Tuning (#11), Kyle Lambda (#14), IL Hedge (#13)
         [These depend on Weeks 1-4 being complete and live data accumulating]
```

---

## Self-Reflection: Honesty Check

### Are any Tier 1 items overrated?

**Latency Arb Defense (#8)**: Possibly. At 300-1000 bps DEX spreads, stale-quote
sniping requires a price move of 3-10% within 52 seconds. This happens maybe
1-2 times per month for XCH. The defense is more important *after* we tighten
spreads to 50-150 bps, where a 1% CEX move would put us underwater. Honest
assessment: this is Tier 1 for *future* importance but Tier 2 for *today's*
importance. Keep it in Tier 1 because it is a prerequisite for spread tightening,
and spread tightening is the path to volume growth.

**Thompson Sampling Tuning (#7)**: Also possibly overrated. With ~40 fills/day
across all pairs, Thompson Sampling converges slowly no matter how we tune the
grid. The real learning will come from A-S/GLFT parameter sweeps on CEX
historical data (abundant), not from bandit exploration on sparse DEX fills.
Keep in Tier 1 because it is only 1 day of work and the grid adjustment is
nearly free.

### Are any Tier 3 items actually more valuable than ranked?

**CEX Hedging (#19, Tier 3)**: This is the most underrated strategy. If XCH
drops 30% (it has been at ATLs), the never-sell-at-loss constraint locks up all
inventory indefinitely. CEX hedging (shorting XCH futures/perps on OKX) would
prevent this scenario entirely. The problem is operational: it requires CEX
trading API integration, key management, cross-venue capital, and margin
monitoring -- essentially building a second trading system. At $10K capital,
the dollar risk of a 30% drawdown is $3K, which is painful but survivable.
At $30K+, hedging becomes essential. If you plan to scale to $30K within 3
months, start building CEX hedging infrastructure now (move to Tier 2).

**IL Hedging (#13, Tier 2)**: May deserve Tier 1 if the AMM allocation (30% of
capital per the profitability model) is already deployed in TibetSwap. With XCH
at ATLs and a hard fork coming in June 2026, directional volatility is high --
exactly when IL hurts most. If AMM capital is not yet deployed, this is
correctly Tier 2 (build before deploying AMM capital).

### What matters most at $2K/day volume?

At $2K/day DEX volume, the total addressable daily PnL is approximately:
- Spread capture: $2,000 x 1.5% effective capture rate = ~$30/day
- CEX-DEX arb: $2,400,000 x 0.01% capture rate = ~$240/day (if execution works)
- DBX incentives: ~$5-10/day
- **Total: ~$275-280/day optimistic**

The honest truth: **CEX-DEX arbitrage is the primary revenue source**, not spread
capture on the DEX order book. The $2.4M CEX daily volume dwarfs the DEX. This
means Lead-Lag CEX Signals (#1) and Arb Engine Tuning (#11) are the two most
important revenue drivers. Everything else is about not losing money while
collecting the spread on the DEX side.

The second honest truth: **at $2K/day DEX volume, most of the 38 strategies are
solving problems that do not exist yet.** VPIN, OFI, whale detection, competitor
detection, regime detection -- these are defenses against a liquid market with
sophisticated participants. CHIA DEX has neither. The core need is:
1. Do not quote stale prices (Lead-Lag + Oracle)
2. Capture CEX-DEX dislocations (Arb tuning)
3. Set spreads that actually fill (A-S/GLFT/Thompson tuning)
4. Collect DBX incentives (just show up with competitive quotes)

Everything else is preparation for the future or insurance against tail risk.

---

*CONSTRAINT: All strategies enforce NEVER SELL AT A LOSS*

*Generated for XOPTrader Issue #9*
