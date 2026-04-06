"""Quick wallet balance check."""
import json
import os
import ssl
import urllib.request

home = os.path.expanduser("~")
ctx = ssl.create_default_context()
ctx.load_cert_chain(
    os.path.join(home, ".chia/mainnet/config/ssl/wallet/private_wallet.crt"),
    os.path.join(home, ".chia/mainnet/config/ssl/wallet/private_wallet.key"),
)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

def query(endpoint, payload):
    data = json.dumps(payload).encode()
    req = urllib.request.Request(
        f"https://localhost:9256/{endpoint}",
        data=data,
        headers={"Content-Type": "application/json"},
    )
    resp = urllib.request.urlopen(req, context=ctx)
    return json.loads(resp.read())

# XCH wallet (wallet_id=1)
result = query("get_wallet_balance", {"wallet_id": 1})
bal = result["wallet_balance"]
print("=== XCH Wallet ===")
for key in ("confirmed_wallet_balance", "spendable_balance", "pending_change",
            "unconfirmed_wallet_balance", "max_send_amount"):
    v = bal[key]
    print(f"  {key:30s} {v:>20,} mojos  ({v / 1e12:.6f} XCH)")

# Check pending offers
print("\n=== Pending Offers ===")
offers = query("get_all_offers", {"start": 0, "end": 50, "file_contents": False,
                                   "exclude_my_offers": False,
                                   "include_completed": False})
pending = [o for o in offers.get("trade_records", []) if o.get("status") == "PENDING_ACCEPT"]
print(f"  Pending offers: {len(pending)}")
for o in pending:
    tid = o.get("trade_id", "?")[:12]
    summary = o.get("summary", {})
    offered = summary.get("offered", {})
    requested = summary.get("requested", {})
    print(f"  [{tid}...] offered={offered} requested={requested}")
