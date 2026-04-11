#!/usr/bin/env python3
"""Diagnostic: transaction history, pending txns, and coin analysis."""
import json, ssl, urllib.request, os, datetime

ctx = ssl.create_default_context()
home = os.path.expanduser("~")
ctx.load_cert_chain(
    os.path.join(home, ".chia/mainnet/config/ssl/wallet/private_wallet.crt"),
    os.path.join(home, ".chia/mainnet/config/ssl/wallet/private_wallet.key"),
)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

def rpc(endpoint, data=None):
    if data is None:
        data = {}
    req = urllib.request.Request(
        f"https://localhost:9256/{endpoint}",
        data=json.dumps(data).encode(),
        headers={"Content-Type": "application/json"},
    )
    resp = urllib.request.urlopen(req, context=ctx)
    return json.loads(resp.read())

# Get recent transactions for XCH wallet
print("=== Recent XCH Transactions ===")
txns = rpc("get_transactions", {"wallet_id": 1, "start": 0, "end": 30, "sort_key": "RELEVANCE", "reverse": True})
for t in txns.get("transactions", []):
    ts = datetime.datetime.fromtimestamp(t.get("created_at_time", 0)).strftime("%m/%d %H:%M")
    amount = t.get("amount", 0) / 1e12
    fee = t.get("fee_amount", 0) / 1e12
    ttype = t.get("type", "?")
    confirmed = t.get("confirmed", False)
    status = "CONFIRMED" if confirmed else "PENDING"
    name = t.get("name", "?")[:16]
    # type: 0=incoming, 1=outgoing, 2=self, etc.
    type_names = {0: "INCOMING", 1: "OUTGOING", 2: "SELF/TRADE", 3: "INCOMING_TRADE", 4: "OUTGOING_TRADE"}
    type_name = type_names.get(ttype, f"TYPE_{ttype}")
    print(f"  {ts} {status:10s} {type_name:16s} amount={amount:>10.6f} fee={fee:.6f} id={name}")

print()

# Get pending coin count
print("=== Wallet Coin Summary ===")
try:
    coins = rpc("get_spendable_coins", {"wallet_id": 1})
    confirmed_coins = coins.get("confirmed_records", [])
    unconfirmed_adds = coins.get("unconfirmed_additions", [])
    unconfirmed_rems = coins.get("unconfirmed_removals", [])
    print(f"  Confirmed coin records: {len(confirmed_coins)}")
    print(f"  Unconfirmed additions:  {len(unconfirmed_adds)}")
    print(f"  Unconfirmed removals:   {len(unconfirmed_rems)}")
    
    total_confirmed = sum(c.get("coin", {}).get("amount", 0) for c in confirmed_coins)
    total_unadd = sum(c.get("coin", {}).get("amount", 0) for c in unconfirmed_adds)
    total_unrem = sum(c.get("coin", {}).get("amount", 0) for c in unconfirmed_rems)
    print(f"  Total in confirmed records: {total_confirmed / 1e12:.6f} XCH")
    print(f"  Total unconfirmed adds:     {total_unadd / 1e12:.6f} XCH")
    print(f"  Total unconfirmed removals: {total_unrem / 1e12:.6f} XCH")
    
    # Show individual coins
    print()
    print("  Confirmed coins:")
    for c in sorted(confirmed_coins, key=lambda x: x.get("coin", {}).get("amount", 0), reverse=True)[:15]:
        coin = c.get("coin", {})
        amt = coin.get("amount", 0)
        spent = c.get("spent", False)
        print(f"    {amt / 1e12:>12.6f} XCH  spent={spent}  id={coin.get('parent_coin_info', '?')[:16]}")
        
except Exception as e:
    print(f"  ERROR: {e}")

print()

# Check all offers with all statuses
print("=== All Recent Offers (last 30) ===")
offers = rpc("get_all_offers", {"start": 0, "end": 30, "sort_key": "RELEVANCE", "reverse": True})
status_map = {0: "PENDING_ACCEPT", 1: "PENDING_CONFIRM", 2: "PENDING_CANCEL", 3: "CANCELLED", 4: "PENDING", 5: "CONFIRMED", 6: "FAILED"}
for o in offers.get("trade_records", []):
    status = status_map.get(o["status"], str(o["status"]))
    ts = datetime.datetime.fromtimestamp(o.get("created_at_time", 0)).strftime("%m/%d %H:%M")
    summary = o.get("summary", {})
    offered = summary.get("offered", {})
    requested = summary.get("requested", {})
    
    def fmt(d):
        parts = []
        for k, v in d.items():
            if k == "xch":
                parts.append(f"{v/1e12:.4f}XCH")
            else:
                parts.append(f"{v/1e3:.1f}k_{k[:6]}")
        return ",".join(parts) if parts else "-"
    
    print(f"  {ts} {status:18s} offer={fmt(offered):>25s}  want={fmt(requested):>25s}")
