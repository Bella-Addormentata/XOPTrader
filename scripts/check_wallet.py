"""Quick wallet state check."""
import requests, warnings, time
warnings.filterwarnings("ignore")

WALLET_CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.key",
)
FN_CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\full_node\private_full_node.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\full_node\private_full_node.key",
)

now = int(time.time())

# Blockchain state
r = requests.post("https://localhost:8555/get_blockchain_state", json={}, verify=False, cert=FN_CERT, timeout=10)
peak = r.json().get("blockchain_state", {}).get("peak", {})
print(f"Peak height: {peak.get('height')}  age: {now - (peak.get('timestamp') or now)}s")

# Wallet balance
r = requests.post("https://localhost:9256/get_wallet_balance", json={"wallet_id": 1}, verify=False, cert=WALLET_CERT, timeout=10)
b = r.json().get("wallet_balance", {})
sp = b.get("spendable_balance", 0) / 1e12
pc = b.get("pending_change", 0) / 1e12
conf = b.get("confirmed_wallet_balance", 0) / 1e12
print(f"Balance: confirmed={conf:.6f}  spendable={sp:.6f}  pending_change={pc:.6f}")

# Unconfirmed transactions
r = requests.post("https://localhost:9256/get_transactions", json={"wallet_id": 1, "start": 0, "end": 50}, verify=False, cert=WALLET_CERT, timeout=10)
txns = r.json().get("transactions", [])
unconf = [t for t in txns if not t.get("confirmed", True)]
print(f"\nUnconfirmed transactions: {len(unconf)}")
for t in unconf:
    name = t.get("name", "?")[:20]
    typ = t.get("type", -1)
    amt = t.get("amount", 0) / 1e12
    age = now - t.get("created_at_time", now)
    has_sb = bool(t.get("spend_bundle"))
    print(f"  tx={name}  type={typ}  amount={amt:.6f}  age={age}s  spend_bundle={has_sb}")
