#!/usr/bin/env python3
"""Check current pending offers from the Chia wallet."""
import subprocess, json, sys
from datetime import datetime

KNOWN_ASSETS = {
    "xch": ("XCH", 1e12),
    "a628c1c2c6fcb74d53746157e438e108eab5c0bb3e5c80ff3b1a6d8ee": ("wUSDC.b", 1e3),
    "6d95dae356e32a71db5ddcb42224754a02524c615c5fc35f568c2af04e": ("BYC", 1e3),
    "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38c7acdbdf60f": ("DBX", 1e3),
}

result = subprocess.run(
    ['chia', 'rpc', 'wallet', 'get_all_offers'],
    input=json.dumps({
        "start": 0, "end": 50,
        "exclude_my_offers": False,
        "exclude_taken_offers": True,
        "sort_key": "RELEVANCE",
        "reverse": True
    }),
    capture_output=True, text=True
)
data = json.loads(result.stdout)

pending = [o for o in data.get('trade_records', []) if o['status'] == 'PENDING_ACCEPT']
print(f"Total pending offers: {len(pending)}")
print()

def fmt_asset(asset_id, amount):
    amount = int(amount) if isinstance(amount, str) else amount
    for key in KNOWN_ASSETS:
        if asset_id == key or asset_id.startswith(key[:20]):
            name, divisor = KNOWN_ASSETS[key]
            return f"{amount/divisor:.6f} {name}"
    if asset_id == 'xch':
        return f"{amount/1e12:.6f} XCH"
    return f"{amount/1e3:.3f} {asset_id[:12]}..."

# Group by pair
pairs = {}
for o in pending:
    summary = o.get('summary', {})
    offered = summary.get('offered', {})
    requested = summary.get('requested', {})
    ts = o.get('created_at_time', 0)
    time_str = datetime.fromtimestamp(ts).strftime('%H:%M:%S') if ts else '?'
    
    off_parts = [fmt_asset(a, amt) for a, amt in offered.items()]
    req_parts = [fmt_asset(a, amt) for a, amt in requested.items()]
    
    # Determine direction
    offering_xch = any('XCH' in p for p in off_parts)
    wanting_xch = any('XCH' in p for p in req_parts)
    
    if offering_xch:
        side = "BID (buying CAT)"
    elif wanting_xch:
        side = "ASK (selling CAT)"
    else:
        side = "CAT/CAT"
    
    # Compute implied price if XCH is involved
    price_info = ""
    for a, amt in offered.items():
        if a == 'xch':
            xch_amt = int(amt) / 1e12
            for ra, ramt in requested.items():
                if ra != 'xch':
                    cat_amt = int(ramt) / 1e3
                    if cat_amt > 0:
                        price_info = f" @ {xch_amt/cat_amt:.6f} XCH/unit"
    for a, amt in requested.items():
        if a == 'xch':
            xch_amt = int(amt) / 1e12
            for oa, oamt in offered.items():
                if oa != 'xch':
                    cat_amt = int(oamt) / 1e3
                    if cat_amt > 0:
                        price_info = f" @ {xch_amt/cat_amt:.6f} XCH/unit"
    
    print(f"  [{time_str}] {side}: {' + '.join(off_parts)} -> {' + '.join(req_parts)}{price_info}")

print()

# Also check wallet balances
print("=" * 60)
print("Wallet Balances:")
for wid in [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]:
    try:
        r = subprocess.run(
            ['chia', 'rpc', 'wallet', 'get_wallet_balance'],
            input=json.dumps({"wallet_id": wid}),
            capture_output=True, text=True
        )
        d = json.loads(r.stdout)
        if d.get('success'):
            b = d['wallet_balance']
            confirmed = b.get('confirmed_wallet_balance', 0)
            spendable = b.get('spendable_balance', 0)
            pending_change = b.get('pending_change', 0)
            name = b.get('asset_id', 'XCH') if wid > 1 else 'XCH'
            # Look up name
            for key, (nm, div) in KNOWN_ASSETS.items():
                if name == key or (isinstance(name, str) and name.startswith(key[:20])):
                    name = nm
                    break
            if wid == 1:
                div = 1e12
                name = 'XCH'
            else:
                div = 1e3
            print(f"  WID {wid} ({name}): confirmed={confirmed/div:.6f}, spendable={spendable/div:.6f}, pending_change={pending_change/div:.6f}")
    except:
        break
