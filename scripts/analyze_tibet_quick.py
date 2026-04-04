#!/usr/bin/env python3
"""Quick analysis of Tibet Swap behavior using dexie v3 historical_trades API."""

import requests
import json
from collections import defaultdict

ASSET_ID = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
TICKER_ID = f"{ASSET_ID}_xch"
API_V1 = "https://api.dexie.space/v1"
API_V3 = "https://api.dexie.space/v3/prices"


def main():
    print("=" * 80)
    print("TIBET SWAP AMM BEHAVIOR ANALYSIS")
    print("=" * 80)

    # 1. Get recent trades via v3 historical_trades
    print("\n[1] Fetching historical trades via v3 API...")
    r = requests.get(f"{API_V3}/historical_trades", params={
        "ticker_id": TICKER_ID,
        "limit": 500,
    }, timeout=30)
    data = r.json()
    trades = data.get("result", [])
    print(f"    Got {len(trades)} trades")

    if not trades:
        print("    No trades found, trying alternative ticker format...")
        r = requests.get(f"{API_V3}/historical_trades", params={
            "ticker_id": f"xch_{ASSET_ID}",
            "limit": 500,
        }, timeout=30)
        data = r.json()
        trades = data.get("result", [])
        print(f"    Got {len(trades)} trades (alt format)")

    if trades:
        print(f"\n    Sample trade structure:")
        print(f"    Keys: {list(trades[0].keys())}")
        print(f"    First trade: {json.dumps(trades[0], indent=6)}")

    # 2. Get recent completed offers from v1 (just one page to be fast)
    print("\n[2] Fetching recent completed offers via v1 API (1 page)...")
    r = requests.get(f"{API_V1}/offers", params={
        "offered": ASSET_ID,
        "status": 4,
        "page": 1,
        "page_size": 50,
        "sort": "date_completed",
        "compact": "true"
    }, timeout=30)
    d1 = r.json()
    offers_offered = d1.get("offers", [])
    
    r = requests.get(f"{API_V1}/offers", params={
        "requested": ASSET_ID,
        "status": 4,
        "page": 1,
        "page_size": 50,
        "sort": "date_completed",
        "compact": "true"
    }, timeout=30)
    d2 = r.json()
    offers_requested = d2.get("offers", [])

    # Merge and dedup
    all_offers = {o["id"]: o for o in offers_offered + offers_requested}
    all_offers = sorted(all_offers.values(), key=lambda o: o.get("date_completed", ""), reverse=True)
    print(f"    Got {len(all_offers)} unique completed offers")

    # 3. Count Tibet vs others
    tibet = [o for o in all_offers if (o.get("known_taker") or {}).get("name") == "TibetSwap AMM"]
    others = [o for o in all_offers if (o.get("known_taker") or {}).get("name") != "TibetSwap AMM"]
    print(f"\n[3] TAKER BREAKDOWN")
    print(f"    Tibet Swap: {len(tibet)} ({len(tibet)/len(all_offers)*100:.0f}%)")
    print(f"    Others:     {len(others)} ({len(others)/len(all_offers)*100:.0f}%)")

    # Taker names
    taker_counts = defaultdict(int)
    for o in all_offers:
        name = (o.get("known_taker") or {}).get("name", "Unknown/Manual") or "Unknown/Manual"
        taker_counts[name] += 1
    print(f"\n    Taker breakdown:")
    for name, cnt in sorted(taker_counts.items(), key=lambda x: -x[1]):
        print(f"      {name}: {cnt}")

    # 4. Analyze Tibet trades
    print(f"\n[4] TIBET SWAP TRADES (most recent)")
    print(f"    {'Date':20s}  {'Dir':12s}  {'XCH':>8s}  {'wUSDC.b':>10s}  {'Price':>8s}  Block")
    print("    " + "-" * 76)
    for o in tibet[:30]:
        offered = o["offered"]
        requested = o["requested"]
        off_codes = [a["code"] for a in offered]
        if "XCH" in off_codes:
            xch = sum(a["amount"] for a in offered if a["code"] == "XCH")
            usdc = sum(a["amount"] for a in requested if a["code"] == "wUSDC.b")
            d = "ASK_TAKEN"
        else:
            xch = sum(a["amount"] for a in requested if a["code"] == "XCH")
            usdc = sum(a["amount"] for a in offered if a["code"] == "wUSDC.b")
            d = "BID_TAKEN"
        p = usdc / xch if xch > 0 else 0
        blk = o.get("spent_block_index", "?")
        print(f"    {o['date_completed'][:19]:20s}  {d:12s}  {xch:8.3f}  {usdc:10.3f}  {p:8.4f}  {blk}")

    # 5. Batch analysis
    print(f"\n[5] BATCH PATTERN (Tibet takes multiple offers in same block)")
    by_block = defaultdict(list)
    for o in tibet:
        blk = o.get("spent_block_index")
        if blk:
            offered = o["offered"]
            off_codes = [a["code"] for a in offered]
            if "XCH" in off_codes:
                xch = sum(a["amount"] for a in offered if a["code"] == "XCH")
                usdc = sum(a["amount"] for a in o["requested"] if a["code"] == "wUSDC.b")
                d = "ASK"
            else:
                xch = sum(a["amount"] for a in o["requested"] if a["code"] == "XCH")
                usdc = sum(a["amount"] for a in offered if a["code"] == "wUSDC.b")
                d = "BID"
            p = usdc/xch if xch else 0
            by_block[blk].append({"d": d, "xch": xch, "usdc": usdc, "price": p})

    for blk, ts in sorted(by_block.items(), reverse=True)[:10]:
        total_xch = sum(t["xch"] for t in ts)
        prices = [t["price"] for t in ts]
        dirs = set(t["d"] for t in ts)
        print(f"    Block {blk}: {len(ts):2d} fills, {total_xch:8.3f} XCH, "
              f"prices {min(prices):.4f}-{max(prices):.4f}, {dirs}")

    # 6. Get current AMM pricing via swap quote API
    print(f"\n[6] CURRENT TIBET SWAP AMM PRICING (via dexie swap/quote)")
    
    # Sell XCH -> wUSDC.b
    print("    What Tibet pays to buy our XCH (ASK side):")
    for size in [0.5, 1.0, 2.0, 2.5, 5.0, 10.0]:
        mojos = int(size * 1e12)
        try:
            r = requests.get(f"{API_V1}/swap/quote", params={
                "from": "xch",
                "to": ASSET_ID,
                "from_amount": str(mojos),
            }, timeout=10)
            q = r.json()
            if q.get("success"):
                to_amt = q["quote"]["to_amount"]
                usdc = to_amt / 1000  # wUSDC.b has 1000 mojos per unit
                eff = usdc / size
                print(f"      {size:6.1f} XCH → {usdc:10.3f} wUSDC.b  eff_price={eff:.4f}")
            else:
                print(f"      {size:6.1f} XCH → Error: {q}")
        except Exception as e:
            print(f"      {size:6.1f} XCH → {e}")

    # Buy XCH with wUSDC.b  
    print("    What Tibet charges to sell XCH to us (BID side):")
    for size in [0.5, 1.0, 2.0, 2.5, 5.0, 10.0]:
        mojos_xch = int(size * 1e12)
        try:
            r = requests.get(f"{API_V1}/swap/quote", params={
                "from": ASSET_ID,
                "to": "xch",
                "to_amount": str(mojos_xch),
            }, timeout=10)
            q = r.json()
            if q.get("success"):
                from_amt = q["quote"]["from_amount"]
                usdc = from_amt / 1000
                eff = usdc / size
                print(f"      {size:6.1f} XCH ← {usdc:10.3f} wUSDC.b  eff_price={eff:.4f}")
            else:
                print(f"      {size:6.1f} XCH ← Error: {q}")
        except Exception as e:
            print(f"      {size:6.1f} XCH ← {e}")

    # 7. Compare our tier prices vs Tibet thresholds
    print(f"\n[7] YOUR TIER PRICING vs TIBET TAKE THRESHOLDS")
    print("    Tibet will take your ASK if your_price < tibet_effective_sell_price")
    print("    Tibet will take your BID if your_price > tibet_effective_buy_price")
    print()
    print("    → To AVOID being picked off: price OUTSIDE the AMM spread")
    print("    → To USE Tibet as counterparty: price INSIDE the AMM spread")
    print("    → Key: check quotes at each tier SIZE because AMM has slippage")

    print("\n" + "=" * 80)


if __name__ == "__main__":
    main()
