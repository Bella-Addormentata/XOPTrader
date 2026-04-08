"""Manually split the 135 XCH coin into ~20 trading-sized coins (5 XCH each)."""
import requests, warnings, time
warnings.filterwarnings("ignore")

CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.key",
)
BASE = "https://localhost:9256"
TARGET_XCH = 5.0
TARGET_MOJOS = int(TARGET_XCH * 1e12)
BATCH_SIZE = 10  # Max splits per transaction


def rpc(endpoint, data=None):
    r = requests.post(f"{BASE}/{endpoint}", json=data or {}, verify=False, cert=CERT, timeout=30)
    return r.json()


# Get a fresh address for self-sends
r = rpc("get_next_address", {"wallet_id": 1, "new_address": True})
addr = r.get("address")
print(f"Self-send address: {addr[:20]}...")

# Check current spendable coins
r = rpc("get_spendable_coins", {"wallet_id": 1})
coins = r.get("confirmed_records", [])
coins.sort(key=lambda c: c["coin"]["amount"], reverse=True)

# Find coins larger than 2x target that should be split
large_coins = [c for c in coins if c["coin"]["amount"] > TARGET_MOJOS * 2]
print(f"\nLarge coins to split: {len(large_coins)}")
for c in large_coins:
    amt = c["coin"]["amount"]
    num_splits = min(amt // TARGET_MOJOS, BATCH_SIZE)
    print(f"  {amt/1e12:.6f} XCH -> {num_splits} splits of {TARGET_XCH} XCH")

if not large_coins:
    print("No coins need splitting!")
    exit(0)

# Split in batches
for c in large_coins:
    amt = c["coin"]["amount"]
    num_splits = min(amt // TARGET_MOJOS, BATCH_SIZE)
    
    # Create self-send transactions
    additions = []
    for i in range(num_splits):
        additions.append({"amount": TARGET_MOJOS, "puzzle_hash": None})
    
    # Use send_transaction_multi for multiple outputs
    print(f"\nSplitting {amt/1e12:.6f} XCH into {num_splits} coins of {TARGET_XCH} XCH...")
    
    # Actually, the simplest approach: multiple send_transaction calls  
    # Each one will carve a 5 XCH piece off the large coin
    for i in range(min(num_splits, BATCH_SIZE)):
        r = rpc("send_transaction", {
            "wallet_id": 1,
            "address": addr,
            "amount": TARGET_MOJOS,
            "fee": 0
        })
        success = r.get("success", False)
        tx_id = r.get("transaction_id", "?")[:16] if r.get("transaction_id") else "?"
        if success:
            print(f"  Split {i+1}/{num_splits}: sent {TARGET_XCH} XCH -> {tx_id}")
        else:
            err = r.get("error", "unknown")
            print(f"  Split {i+1}/{num_splits}: FAILED - {err}")
            if "not enough" in str(err).lower() or "insufficient" in str(err).lower():
                print("  Stopping splits (insufficient funds for more)")
                break

print("\n=== Split transactions submitted ===")
# Show new balance
time.sleep(2)
b = rpc("get_wallet_balance", {"wallet_id": 1}).get("wallet_balance", {})
sp = b.get("spendable_balance", 0) / 1e12
pc = b.get("pending_change", 0) / 1e12
print(f"Balance: spendable={sp:.6f}  pending_change={pc:.6f}")
