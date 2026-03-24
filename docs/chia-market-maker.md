# Chia DEX Market Maker Research

## Purpose
- Outline options for building and running a market maker for XCH/CAT pairs on decentralized venues.
- Provide architecture, operational playbooks, and risk controls tailored to Chia’s offer-based DEX ecosystem.

## Chia DEX landscape and constraints
- **Settlement model:** Chia uses “offers” (partially signed spend bundles) enabling atomic swaps without custodial smart contracts. Orders are off-chain until taken, then settle on-chain.
- **Venues/aggregators:** Offer-based order books such as Dexie and other indexers/relayers; some AMM-style experiments (e.g., TibetSwap/other CAT AMMs) may exist but have limited liquidity. Always validate current venues and APIs before integration.
- **Assets:** Native XCH plus CAT tokens; check each CAT’s TAIL and issuance policy. NFT trades are supported via offers but are out of scope unless used for incentives.
- **Latency:** No mempool order priority; offers can be mirrored across relayers. Protection relies on spreads, inventory controls, and cancel/refresh cadence rather than cancel speed.
- **Wallet considerations:** Light wallets cannot yet produce all advanced spend bundles; run a full node or trusted remote signer for production.

## Market-making models to consider
1) **Offer-based order-book quoting (primary path)**
   - Continuously post two-sided offers (XCH↔CAT) to one or more relayers/DEX UIs.
   - Tight control of pricing, size, and inventory; easy to pause by retracting/expiring offers.
   - Requires offer lifecycle management (create, publish, refresh/replace, cancel/timeout).

2) **AMM liquidity provision (where available)**
   - Provide liquidity to CAT/XCH pools (e.g., TibetSwap-like AMMs). Earn swap fees; pricing follows bonding curves.
   - Lower operational overhead, but exposes to impermanent loss and pool smart-contract risk. Liquidity depth on Chia AMMs is currently small; treat as secondary strategy.

3) **Hybrid hedged quoting**
   - Quote on DEX (via offers) while hedging exposure on centralized venues, perps, or OTC. Useful when CAT has correlated hedgeable asset.
   - Needs latency-aware hedger and position keeper; watch bridge/liquidity risks if using wrapped assets.

4) **Programmatic vault/custodial service**
   - Build a managed-liquidity product for third parties; introduces custodial risk, compliance, and audits. Likely out of initial scope but noted for future.

## Reference architecture (offer-based quoting)
- **Node + wallet layer:** Chia full node, wallet, and CAT support. Use separate hot keys for funding/ops; keep cold storage for treasury.
- **Offer engine:** Constructs offers, enforces inventory bounds, tracks expirations, and republishes replacements. Must avoid double-spend by locking coins per active offer.
- **Price adapter:** Pulls price feeds (DEX trades, CEX prices, oracle APIs). Apply sanity checks and circuit breakers.
- **Strategy module:** Sets spreads, skew, and size based on volatility, inventory, and target fill rates. Includes kill-switch thresholds.
- **Relayer connector:** Publishes offers to Dexie/other relayers; tracks acceptance/fills via wallet spend notifications and relayer callbacks/webhooks if available.
- **Hedger (optional):** Routes offsetting trades to external venues; netting rules defined by risk.
- **Monitoring:** Offer freshness, wallet balances, unmatched coins, node sync status, mempool congestion, and fill/PNL metrics.

## Offer lifecycle mechanics
- **Creation:** Lock coin(s) for each leg; build offer with desired rate and fees; include timeout/refresh horizon (e.g., replace every N minutes or after price move).
- **Publication:** Broadcast to multiple relayers to increase discovery; store offer IDs and coin IDs.
- **Fill detection:** Listen for wallet spend confirmations; reconcile with relayer fill events when available.
- **Cancellation/refresh:** Replace offers after price drift, inventory changes, or stale age; avoid reusing coin IDs until confirmed free.
- **Failure handling:** If double-spend errors or rejected spends appear, rescan wallet, unlock coins, and rebuild offers with fresh coins.

## Risk and controls
- **Key management:** Hot signer with least-privilege keys; rate-limit signing; prefer hardware-backed keys if supported. Cold storage for treasury and periodic top-ups.
- **Inventory and exposure:** Per-side and net position limits; dynamic skew to attract fills in the direction needed to rebalance.
- **Price protection:** Reference price bands, volatility-aware spreads, and max slippage on hedges. Circuit-breakers on price gaps or stale data.
- **Operational safety:** One-button pause that stops new offers and retracts active ones; health checks for node sync and wallet anomalies.
- **Settlement risk:** Since offers are atomic, counterparty risk is minimal, but watch chain reorgs and CAT authenticity (validate TAILs).
- **Compliance:** Track jurisdictional obligations (market-making may be MSB/EMI relevant); maintain audit trail of offers, fills, and key rotations.

## Operational runbooks (suggested)
- **Bring-up:** Verify node synced, wallet unlocked, balances sufficient; run dry-run offer creation; publish test min-size offers to confirm relay path.
- **Quoting on:** Enable strategy; publish starter grid with conservative sizes; monitor fills and wallet locks for 5–10 minutes before scaling.
- **Pausing:** Disable strategy; retract/expire all offers; ensure all coins unlocked; hedge residual exposures if hedged mode is on.
- **Failover:** Secondary node+signer in another region; replay last known inventory and outstanding offers; avoid duplicate coin spends by rekeying offer inputs.
- **Post-incident:** Reconcile fills vs. expected, rescan wallet, rotate hot keys if compromise suspected.

## Metrics and monitoring
- Offer freshness (age until replace), active bid/ask count, spread vs. reference price, inventory per asset, fill rate, rejection/double-spend rate.
- Node health (sync height, peer count), wallet locked coins, mempool size/fee environment.
- PNL attribution: trading PNL, fees paid/earned, hedging costs, and impermanent loss (if using AMMs).

## Roadmap suggestions
- **MVP (offer market maker):** Full node + wallet, offer engine, single-venue connector (Dexie), pricing from one reliable feed, manual pause switch.
- **Phase 2:** Multi-relayer publishing, automated refresh policies, inventory-based skew, alerting dashboard.
- **Phase 3:** Hedged mode with external venue connector, AMM LP module, and formal runbooks with chaos tests.

## Open questions to validate
- Current relayer APIs and webhook support (Dexie/others), rate limits, and any anti-spam rules.
- Availability and security model of hardware/remote signing for Chia offers.
- Depth and contract risk of existing Chia AMMs; whether audits exist and how fee economics compare to offer-book spreads.
