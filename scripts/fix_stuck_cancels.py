"""Clear stuck unconfirmed transactions and re-cancel offers with a tiny fee."""
import requests
import warnings
import time

warnings.filterwarnings("ignore")

CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.key",
)
BASE = "https://localhost:9256"
FEE = 100_000_000  # 0.0001 XCH


def rpc(endpoint, data=None):
    r = requests.post(f"{BASE}/{endpoint}", json=data or {}, verify=False, cert=CERT, timeout=30)
    return r.json()


def show_balance():
    b = rpc("get_wallet_balance", {"wallet_id": 1}).get("wallet_balance", {})
    sp = b.get("spendable_balance", 0) / 1e12
    pc = b.get("pending_change", 0) / 1e12
    conf = b.get("confirmed_wallet_balance", 0) / 1e12
    print(f"  Balance: confirmed={conf:.6f}  spendable={sp:.6f}  pending_change={pc:.6f}")
    return pc


# Step 1: Delete unconfirmed transactions
print("=== Step 1: Delete unconfirmed transactions ===")
r = rpc("delete_unconfirmed_transactions", {"wallet_id": 1})
print(f"  Result: success={r.get('success')}")
time.sleep(2)
pc = show_balance()
print()

# Step 2: Check for any remaining active offers
print("=== Step 2: Check remaining offers ===")
r = rpc("get_all_offers", {"start": 0, "end": 50, "exclude_my_offers": False,
         "exclude_taken_offers": True, "include_completed": False})
offers = r.get("trade_records", [])
active = [o for o in offers if o.get("status") in [None, "PENDING_ACCEPT"]]
# Filter to only truly active (status field varies)
print(f"  Total offers returned: {len(offers)}")
for o in offers[:5]:
    tid = o.get("trade_id", "?")[:16]
    status = o.get("status")
    print(f"    offer={tid}  status={status}")

if not offers:
    print("  No active offers remain - cancels already took effect!")
    show_balance()
else:
    # Step 3: Re-cancel with fee
    print(f"\n=== Step 3: Cancel {len(offers)} offers with fee={FEE/1e12} XCH ===")
    r = rpc("cancel_offers", {"secure": True, "fee": FEE, "batch_fee": FEE})
    print(f"  Result: success={r.get('success')}")
    if not r.get("success"):
        print(f"  Error: {r.get('error', 'unknown')}")
    time.sleep(3)
    show_balance()

print("\n=== Done ===")
