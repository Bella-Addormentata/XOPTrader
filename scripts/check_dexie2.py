#!/usr/bin/env python3
"""Check dexie prices and order books for our trading pairs."""
import urllib.request, json

ASSETS = {
    "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d": "wUSDC.b",
    "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac": "BYC",
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20": "DBX",
}

# 1. Get tickers
print("=== DEXIE TICKERS (our assets) ===")
try:
    url = "https://api.dexie.space/v2/prices/tickers"
    req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        data = json.loads(resp.read())
        tickers = data.get("tickers", [])
        for t in tickers:
            base_id = t.get("base_id", "")
            target_id = t.get("target_id", "")
            for aid, name in ASSETS.items():
                if aid == base_id or aid == target_id:
                    base_name = t.get("base_currency", base_id[:12])
                    tgt_name = t.get("target_currency", target_id[:12])
                    print(f"  {base_name}/{tgt_name}: last={t.get('last_price','?')} bid={t.get('bid','?')} ask={t.get('ask','?')} vol24h={t.get('base_volume','?')}")
                    break
except Exception as e:
    print(f"  Error: {e}")

# 2. Get order books via dexie offers API
print()
print("=== ORDER BOOKS ===")
for aid, name in ASSETS.items():
    print(f"\n--- XCH/{name} (asset {aid[:16]}...) ---")
    
    # Bids: people offering XCH wanting CAT
    try:
        url = f"https://dexie.space/v1/offers?offered=xch&requested={aid}&page_size=10&sort=price_desc&status=4"
        req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
            offers = data.get("offers", [])
            total = data.get("count", len(offers))
            print(f"  BIDS (buy {name}): {total} total")
            for o in offers[:5]:
                print(f"    {json.dumps({k:o[k] for k in ['price','offered_amount','requested_amount'] if k in o})}")
    except Exception as e:
        # Try v1 API
        try:
            url = f"https://api.dexie.space/v1/offers?offered=xch&requested={aid}&page_size=10&sort=price_desc&status=4"
            req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = json.loads(resp.read())
                offers = data.get("offers", [])
                print(f"  BIDS (buy {name}): {len(offers)} offers")
                for o in offers[:5]:
                    print(f"    price={o.get('price','?')}")
        except Exception as e2:
            print(f"  Bids error (v1 too): {e} / {e2}")
    
    # Asks: people offering CAT wanting XCH
    try:
        url = f"https://dexie.space/v1/offers?offered={aid}&requested=xch&page_size=10&sort=price_asc&status=4"
        req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read())
            offers = data.get("offers", [])
            total = data.get("count", len(offers))
            print(f"  ASKS (sell {name}): {total} total")
            for o in offers[:5]:
                print(f"    {json.dumps({k:o[k] for k in ['price','offered_amount','requested_amount'] if k in o})}")
    except Exception as e:
        try:
            url = f"https://api.dexie.space/v1/offers?offered={aid}&requested=xch&page_size=10&sort=price_asc&status=4"
            req = urllib.request.Request(url, headers={"User-Agent": "XOPTrader/0.7.36"})
            with urllib.request.urlopen(req, timeout=10) as resp:
                data = json.loads(resp.read())
                offers = data.get("offers", [])
                print(f"  ASKS (sell {name}): {len(offers)} offers")
                for o in offers[:5]:
                    print(f"    price={o.get('price','?')}")
        except Exception as e2:
            print(f"  Asks error: {e} / {e2}")
