"""Show spendable coin breakdown."""
import requests, warnings
warnings.filterwarnings("ignore")
CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.key",
)
r = requests.post("https://localhost:9256/get_spendable_coins", json={"wallet_id": 1}, verify=False, cert=CERT, timeout=10)
coins = r.json().get("confirmed_records", [])
coins.sort(key=lambda c: c.get("coin", {}).get("amount", 0), reverse=True)
total = sum(c.get("coin", {}).get("amount", 0) for c in coins) / 1e12
print(f"Spendable coins: {len(coins)}  total: {total:.6f} XCH\n")
for i, c in enumerate(coins[:30]):
    amt = c.get("coin", {}).get("amount", 0)
    print(f"  #{i+1}: {amt/1e12:.6f} XCH  ({amt} mojos)")
if len(coins) > 30:
    print(f"  ... and {len(coins)-30} more")
