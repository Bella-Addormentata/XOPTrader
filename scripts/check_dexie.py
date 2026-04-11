#!/usr/bin/env python3
"""Check dexie prices and our offers on dexie."""
import urllib.request, json

# Our asset IDs
ASSETS = {
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": "wUSDC.b",
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": "BYC",
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20": "DBX",
}

print("=== DEXIE TICKERS ===")
try:
    url = "https://api.dexie.space/v2/prices/tickers"
    req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        data = json.loads(resp.read())
        for t in data:
            base = t.get("base_currency", "")
            target = t.get("target_currency", "")
            # Check if any of our assets are involved
            for aid, name in ASSETS.items():
                if aid in base or aid in target or name.lower() in base.lower() or name.lower() in target.lower():
                    print(f"  {t.get('ticker_id','?'):70s} last={t.get('last_price','?'):>12s} bid={t.get('bid','?'):>12s} ask={t.get('ask','?'):>12s}")
                    break
except Exception as e:
    print(f"  Error: {e}")

# Check offers on dexie for our pairs
print()
print("=== DEXIE ACTIVE OFFERS (our pairs) ===")
for aid, name in ASSETS.items():
    print(f"\n--- XCH/{name} ---")
    try:
        # Get bids (people offering XCH, wanting CAT)
        url = f"https://api.dexie.space/v2/offers?offered=xch&requested={aid}&page_size=5&sort=price&compact=true&status=4"
        req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
            offers = data.get("offers", [])
            print(f"  BIDS (buy {name} with XCH): {len(offers)} shown / {data.get('count',0)} total")
            for o in offers[:5]:
                price = o.get("price", "?")
                offered_str = o.get("offered", [{}])
                requested_str = o.get("requested", [{}])
                print(f"    price={price}")
    except Exception as e:
        print(f"  Bids error: {e}")
    
    try:
        # Get asks (people offering CAT, wanting XCH)
        url = f"https://api.dexie.space/v2/offers?offered={aid}&requested=xch&page_size=5&sort=price&compact=true&status=4"
        req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
            offers = data.get("offers", [])
            print(f"  ASKS (sell {name} for XCH): {len(offers)} shown / {data.get('count',0)} total")
            for o in offers[:5]:
                price = o.get("price", "?")
                print(f"    price={price}")
    except Exception as e:
        print(f"  Asks error: {e}")
