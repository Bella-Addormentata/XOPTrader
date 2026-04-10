# Strategy Change: Competitive Pricing Adjustments

**Date:** April 8, 2026  
**Revisit By:** April 9, 2026  
**Status:** Active — monitoring for outcomes  

---

## Situation Before Change

The engine has had **zero fills since ~1:49 PM** (last fill was an XCH/wUSDC.b ASK at block 8560626). The market is active — other traders are filling on Dexie — but we're not participating.

### Root Causes Identified

| Issue | Detail |
|---|---|
| **BYC inventory = 0** | Can't post bid-side offers for BYC/wUSDC.b or XCH/BYC |
| **wUSDC.b inventory = 0** | Can't post bid-side offers for XCH/wUSDC.b |
| **XCH reserve low (21.1%)** | Below 25% threshold → ask-side suppressed on XCH pairs |
| **Peg guard overpricing asks** | BYC/wUSDC.b asks all clamped to $1.0005 (peg=1.0 +5bps) |
| **Market trades BYC at ~$0.985** | Our $1.0005 asks are **~155 bps** above best competing ask ($0.9860) |
| **min_reserve_units = 1.0** | Even tiny remaining balances get suppressed as "below reserve" |
| **min_trading_units = 2.0** | Small inventory after a fill can't be redeployed |

### Dexie Market Snapshot at Time of Change

**BYC/wUSDC.b:**
- Best ask: $0.9860 (competitors)
- Best bid: $0.9817 (1/1.0183)
- Recent fill prices (asks): $0.985–$0.989
- Recent fill prices (bids): $0.981–$0.982
- Our ask: $1.0005 (peg guard) — **not competitive**

**XCH/wUSDC.b:**
- Best ask: $2.3403
- Best bid: ~$2.3265
- Our tiers 0-1 dropped as dust (< 1.0 XCH min size)
- Remaining tiers clamped to dex best bid price

---

## Changes Made

### 1. BYC/wUSDC.b `peg_target`: 1.0 → 0.985

**What it does:** The peg guard clamps ask prices to `peg_target + min_profit_margin_bps`. With peg=1.0 and margin=5bps, asks floored at $1.0005. BYC actually trades at ~$0.985, so this was 155bps above market.

**New behavior:** Ask floor is now $0.985 × (1 + 5/10000) = ~$0.9855, which is competitive with the $0.986 best ask on Dexie.

### 2. `min_reserve_units`: 1.0 → 0.1

**What it does:** The engine suppresses an asset's side when balance < min_reserve_units. At 1.0, even 0.5 BYC left after a fill couldn't be used for new offers.

**New behavior:** Only suppress when balance drops below 0.1 units, allowing fractional balances to be recycled into new offers.

### 3. `min_trading_units`: 2.0 → 0.5

**What it does:** Minimum balance needed to consider a pair "tradeable". At 2.0, small residual inventory after fills was treated as non-tradeable.

**New behavior:** Pairs become tradeable with as little as 0.5 units of inventory, keeping us in the market with smaller clip sizes when inventory is running low.

---

## Goals to Evaluate Tomorrow

- [ ] **Primary: Get fills again** — At least 1 fill on BYC/wUSDC.b by end of April 9
- [ ] **BYC asks are competitive** — Our best ask should be within 10bps of Dexie best ask
- [ ] **Fractional inventory is deployed** — Engine stops suppressing sides with small but nonzero balances
- [ ] **No unintended losses** — No fills at prices significantly worse than market mid
- [ ] **Spread capture is positive** — Any fills that occur are on our side of the spread (not adverse selection)

---

## Pros

1. **Immediately competitive on BYC/wUSDC.b asks** — $0.9855 floor vs $0.9860 market best. We'll be near top-of-book.
2. **Recycles residual inventory** — Small leftover balances after fills stay deployed rather than sitting idle.
3. **Aligns peg target to reality** — BYC has been consistently trading at $0.984–$0.989, not $1.00. Our config now reflects this.
4. **No code changes required** — Pure config tuning, easily reversible.
5. **Should increase fill rate** — The 0.7% 24h fill rate should improve as our offers become takeable.

## Cons

1. **Selling BYC below $1 peg** — If BYC recovers to $1.00+, we'll have sold at a discount. The peg_target change means we no longer enforce "don't sell below peg."
2. **Lower per-trade margin** — Tighter pricing means less profit per fill. At 5bps margin on $0.985, we earn ~$0.0005 per BYC traded.
3. **Smaller offer sizes** — With min_trading_units at 0.5, we may post very small offers (0.5–1.0 BYC) that earn negligible absolute profit.
4. **Doesn't solve the inventory problem** — We still have 0 BYC and 0 wUSDC.b. These changes only help *if* some inventory returns via fills or manual funding.
5. **Reserve buffer reduced** — With min_reserve at 0.1, we keep less in reserve for fee payments and emergency rebalancing.

---

## Pitfalls to Watch For

### 🔴 Critical
- **Adverse selection at the new peg target** — If BYC drops below $0.975, our $0.985 peg target becomes overpriced on bids (buying too expensive). Watch for fills where we buy BYC above market value.
- **Inventory spiral** — If we only get ask fills (selling BYC/XCH) without corresponding bid fills, we'll drain remaining inventory faster. Check that both sides are filling, not just one.

### 🟡 Important  
- **Peg recovery scenario** — If BYC rallies back to $1.00, our $0.9855 asks will fill immediately (good) but we'll have set a low peg target. We may want to raise peg_target back to $0.99+ if BYC shows signs of strengthening.
- **Fee exhaustion** — With smaller minimum sizes, we may post more offers, consuming more on-chain fees. Monitor `fee_reserve_xch` and ensure we don't spend the entire fee budget on dust offers.
- **Step 9e suppression still active** — The `peg_arb_max_inventory_ratio: 0.60` guard will still prevent arbitrage takes when inventory is unbalanced. This is fine for now but limits automatic rebalancing.

### 🟢 Informational
- **Fill rate improvement may be gradual** — Chia DEX has low throughput (~1 block/18.75s). Even with competitive pricing, fills may take hours depending on counterparty activity.
- **Other market makers may adjust** — If we undercut the current $0.986 best ask, competitors may tighten their pricing too. This is healthy for the market but compresses margins further.
- **Engine restart required** — The C++ engine reads config at startup only. Changes take effect after GUI restart.

---

## Rollback Plan

If outcomes are negative, revert these three values in `config.yaml`:

```yaml
# Revert to previous values:
peg_target: 1.0          # was 0.985
min_reserve_units: 1.0    # was 0.1
min_trading_units: 2.0    # was 0.5
```

Then restart the GUI/engine to pick up changes.

---

## Notes

- The most impactful next step beyond these config changes would be **seeding wUSDC.b and BYC inventory** (manual acquisition or via XCH swap) to enable two-sided markets again.
- The XCH/wUSDC.b pair has a separate competitiveness issue: our tier sizes (0.23–0.65 XCH) are below the `min_offer_size_units_override: 1` threshold. Consider lowering this to 0.2 if XCH inventory remains low.
