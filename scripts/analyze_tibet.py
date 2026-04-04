#!/usr/bin/env python3
"""Analyze Tibet Swap AMM behavior on dexie.space to predict which offers it takes."""

import requests
import json
from datetime import datetime, timedelta
from collections import defaultdict

ASSET_ID = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
API_BASE = "https://api.dexie.space/v1"


def fetch_completed_trades(pages=5):
    """Fetch recent completed trades involving wUSDC.b from both sides."""
    all_trades = []
    
    for direction in ["offered", "requested"]:
        for page in range(1, pages + 1):
            try:
                r = requests.get(f"{API_BASE}/offers", params={
                    direction: ASSET_ID,
                    "status": 4,
                    "page": page,
                    "page_size": 100,
                    "sort": "date_completed",
                    "compact": "true"
                }, timeout=15)
                data = r.json()
                all_trades.extend(data.get("offers", []))
            except Exception as e:
                print(f"  Error fetching page {page} ({direction}): {e}")
    
    # Deduplicate
    seen = set()
    unique = []
    for t in all_trades:
        if t["id"] not in seen:
            seen.add(t["id"])
            unique.append(t)
    return unique


def fetch_swap_quote(from_token, to_token, amount, direction="from"):
    """Get a swap quote to determine Tibet Swap's current pricing."""
    try:
        params = {"from": from_token, "to": to_token}
        if direction == "from":
            params["from_amount"] = str(amount)
        else:
            params["to_amount"] = str(amount)
        r = requests.get(f"{API_BASE}/swap/quote", params=params, timeout=10)
        return r.json()
    except Exception as e:
        print(f"  Quote error: {e}")
        return None


def analyze_trade(t):
    """Parse a trade into a structured dict."""
    offered = t["offered"]
    requested = t["requested"]
    off_codes = [a["code"] for a in offered]
    
    if "XCH" in off_codes:
        xch_amt = sum(a["amount"] for a in offered if a["code"] == "XCH")
        usdc_amt = sum(a["amount"] for a in requested if a["code"] == "wUSDC.b")
        direction = "ASK_TAKEN"  # Someone sold XCH, Tibet bought XCH
        price = usdc_amt / xch_amt if xch_amt > 0 else 0
    else:
        xch_amt = sum(a["amount"] for a in requested if a["code"] == "XCH")
        usdc_amt = sum(a["amount"] for a in offered if a["code"] == "wUSDC.b")
        direction = "BID_TAKEN"  # Someone bought XCH, Tibet sold XCH
        price = usdc_amt / xch_amt if xch_amt > 0 else 0
    
    taker = t.get("known_taker", {}).get("name", "Unknown")
    date_completed = t.get("date_completed", "")
    fees = t.get("fees", 0)
    
    return {
        "id": t["id"],
        "direction": direction,
        "xch_amount": xch_amt,
        "usdc_amount": usdc_amt,
        "price": price,
        "taker": taker,
        "date": date_completed,
        "fees": fees,
        "block": t.get("spent_block_index", 0),
    }


def main():
    print("=" * 80)
    print("TIBET SWAP AMM BEHAVIOR ANALYSIS")
    print("=" * 80)
    
    # 1. Fetch trade history
    print("\n[1] Fetching completed trades...")
    trades = fetch_completed_trades(pages=5)
    print(f"    Total unique trades: {len(trades)}")
    
    # 2. Classify trades
    analyzed = [analyze_trade(t) for t in trades]
    tibet = [t for t in analyzed if t["taker"] == "TibetSwap AMM"]
    others = [t for t in analyzed if t["taker"] != "TibetSwap AMM"]
    
    print(f"    Tibet Swap fills: {len(tibet)}")
    print(f"    Other fills: {len(others)}")
    tibet_pct = len(tibet) / len(analyzed) * 100 if analyzed else 0
    print(f"    Tibet Swap share: {tibet_pct:.1f}%")
    
    # 3. Tibet Swap trade summary
    print("\n[2] RECENT TIBET SWAP FILLS (newest first)")
    print(f"    {'Date':20s}  {'Direction':12s}  {'XCH':>8s}  {'wUSDC.b':>10s}  {'Price':>8s}  {'Fees':>8s}")
    print("    " + "-" * 76)
    for t in tibet[:40]:
        print(f"    {t['date'][:19]:20s}  {t['direction']:12s}  {t['xch_amount']:8.3f}  {t['usdc_amount']:10.3f}  {t['price']:8.4f}  {t['fees']:8.5f}")
    
    # 4. Price distribution analysis
    print("\n[3] TIBET SWAP PRICE ANALYSIS")
    ask_taken = [t for t in tibet if t["direction"] == "ASK_TAKEN"]
    bid_taken = [t for t in tibet if t["direction"] == "BID_TAKEN"]
    
    if ask_taken:
        prices = [t["price"] for t in ask_taken]
        sizes = [t["xch_amount"] for t in ask_taken]
        print(f"    ASK fills (Tibet buys XCH):")
        print(f"      Count: {len(ask_taken)}")
        print(f"      Price range: {min(prices):.4f} - {max(prices):.4f} wUSDC.b/XCH")
        print(f"      Avg price: {sum(p*s for p,s in zip(prices,sizes))/sum(sizes):.4f} (VWAP)")
        print(f"      Size range: {min(sizes):.3f} - {max(sizes):.3f} XCH")
        print(f"      Total volume: {sum(sizes):.3f} XCH")
    
    if bid_taken:
        prices = [t["price"] for t in bid_taken]
        sizes = [t["xch_amount"] for t in bid_taken]
        print(f"    BID fills (Tibet sells XCH):")
        print(f"      Count: {len(bid_taken)}")
        print(f"      Price range: {min(prices):.4f} - {max(prices):.4f} wUSDC.b/XCH")
        print(f"      Avg price: {sum(p*s for p,s in zip(prices,sizes))/sum(sizes):.4f} (VWAP)")
        print(f"      Size range: {min(sizes):.3f} - {max(sizes):.3f} XCH")
        print(f"      Total volume: {sum(sizes):.3f} XCH")
    
    # 5. Batch pattern analysis
    print("\n[4] BATCH TRADE PATTERN ANALYSIS")
    print("    Tibet Swap takes multiple orders in the same block:")
    by_block = defaultdict(list)
    for t in tibet:
        if t["block"]:
            by_block[t["block"]].append(t)
    
    multi_block = {b: ts for b, ts in by_block.items() if len(ts) > 1}
    for block, ts in sorted(multi_block.items(), reverse=True)[:10]:
        total_xch = sum(t["xch_amount"] for t in ts)
        directions = set(t["direction"] for t in ts)
        prices = [t["price"] for t in ts]
        print(f"    Block {block}: {len(ts)} fills, {total_xch:.3f} XCH total, "
              f"prices {min(prices):.4f}-{max(prices):.4f}, dirs={directions}")
    
    # 6. Get current Tibet Swap AMM state via swap quotes
    print("\n[5] CURRENT TIBET SWAP AMM PRICING (via dexie swap quotes)")
    
    # Quote: Sell 1 XCH → get wUSDC.b
    test_sizes = [0.1, 0.5, 1.0, 2.0, 5.0, 10.0]
    print("    Selling XCH → receiving wUSDC.b:")
    for size_xch in test_sizes:
        mojos = int(size_xch * 1e12)
        quote = fetch_swap_quote("xch", ASSET_ID, mojos, "from")
        if quote and quote.get("success"):
            q = quote.get("quote", {})
            to_amt = q.get("to_amount", 0)
            usdc_out = to_amt / 1000.0  # wUSDC.b has 3 decimal places (1000 mojos per unit)
            eff_price = usdc_out / size_xch if size_xch > 0 else 0
            print(f"      {size_xch:6.1f} XCH → {usdc_out:10.3f} wUSDC.b  (eff. price: {eff_price:.4f})")
        else:
            err = quote.get("error_message", quote) if quote else "no response"
            print(f"      {size_xch:6.1f} XCH → ERROR: {err}")
    
    # Quote: Buy XCH with wUSDC.b
    print("    Buying XCH ← spending wUSDC.b:")
    for size_xch in test_sizes:
        mojos = int(size_xch * 1e12)
        quote = fetch_swap_quote(ASSET_ID, "xch", mojos, "to")
        if quote and quote.get("success"):
            q = quote.get("quote", {})
            from_amt = q.get("from_amount", 0)
            usdc_in = from_amt / 1000.0
            eff_price = usdc_in / size_xch if size_xch > 0 else 0
            print(f"      {size_xch:6.1f} XCH ← {usdc_in:10.3f} wUSDC.b  (eff. price: {eff_price:.4f})")
        else:
            err = quote.get("error_message", quote) if quote else "no response"
            print(f"      {size_xch:6.1f} XCH ← ERROR: {err}")
    
    # 7. Prediction: at what prices will Tibet take our offers?
    print("\n[6] TIBET SWAP TAKE PREDICTION")
    print("    Based on AMM constant-product formula (x*y=k, 0.7% fee):")
    print("    Tibet will take our offer IFF our price is better than their AMM execution price.")
    print()
    
    # Get a baseline quote for a small trade to infer reserves
    baseline = fetch_swap_quote("xch", ASSET_ID, int(0.001 * 1e12), "from")
    if baseline and baseline.get("success"):
        q = baseline["quote"]
        # The marginal price ≈ token_reserve / xch_reserve
        # For small input: output ≈ input * (token_reserve / xch_reserve) * (993/1000)
        input_mojos = int(0.001 * 1e12)
        output_mojos = q.get("to_amount", 0)
        # implied_price * (993/1000) ≈ output/input
        # implied_price ≈ output / input / 0.993
        marginal_price = (output_mojos / 1000) / 0.001 / 0.993
        print(f"    Estimated AMM marginal price: {marginal_price:.4f} wUSDC.b/XCH")
        
        # Now calculate at what prices Tibet will take
        # Tibet buys our ASK when: our_ask_price < amm_sell_price_for_that_size
        # Tibet takes our BID when: our_bid_price > amm_buy_price_for_that_size
        print()
        print("    PREDICTION: Tibet will TAKE our ASK (buy our XCH) if our price is BELOW:")
        for size_xch in [0.5, 1.0, 2.0, 2.5, 5.0]:
            q2 = fetch_swap_quote("xch", ASSET_ID, int(size_xch * 1e12), "from")
            if q2 and q2.get("success"):
                out = q2["quote"]["to_amount"] / 1000
                eff = out / size_xch
                print(f"      {size_xch:5.1f} XCH:  < {eff:.4f} wUSDC.b/XCH")
        
        print()
        print("    PREDICTION: Tibet will TAKE our BID (sell us XCH) if our price is ABOVE:")
        for size_xch in [0.5, 1.0, 2.0, 2.5, 5.0]:
            q2 = fetch_swap_quote(ASSET_ID, "xch", int(size_xch * 1e12), "to")
            if q2 and q2.get("success"):
                cost = q2["quote"]["from_amount"] / 1000
                eff = cost / size_xch
                print(f"      {size_xch:5.1f} XCH:  > {eff:.4f} wUSDC.b/XCH")
    
    # 8. Strategy recommendations
    print("\n[7] STRATEGY RECOMMENDATIONS")
    print("    1. Tibet Swap is a constant-product AMM — its behavior is 100% deterministic")
    print("    2. It will ALWAYS take offers that are priced better than its pool execution price")
    print("    3. To AVOID Tibet picking off your offers:")
    print("       - Price ASK offers ABOVE the AMM's effective sell price for that tier size")
    print("       - Price BID offers BELOW the AMM's effective buy price for that tier size")
    print("    4. To EXPLOIT Tibet (cross-DEX arb):")
    print("       - When AMM price diverges from CEX, place offers that Tibet will fill")
    print("       - Tibet fills are fast (same block as offer detection)")
    print("    5. The AMM pool reserves shift after each trade, so check reserves frequently")
    print("    6. Tibet takes batches: it will sweep ALL profitable tiers in one block")
    
    print("\n" + "=" * 80)


if __name__ == "__main__":
    main()
