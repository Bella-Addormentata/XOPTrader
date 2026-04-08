"""Cancel all offers and split coins for XOPTrader."""
import requests
import json
import warnings
import time
import sys

warnings.filterwarnings("ignore")
CERT = (
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.crt",
    r"C:\Users\dorkm\.chia\mainnet\config\ssl\wallet\private_wallet.key",
)
URL = "https://localhost:9256"


def rpc(endpoint, payload):
    r = requests.post(f"{URL}/{endpoint}", json=payload, verify=False, cert=CERT, timeout=30)
    return r.json()


def get_balance():
    d = rpc("get_wallet_balance", {"wallet_id": 1})
    b = d["wallet_balance"]
    return {
        "confirmed": b["confirmed_wallet_balance"] / 1e12,
        "spendable": b["spendable_balance"] / 1e12,
        "pending_change": b["pending_change"] / 1e12,
    }


def get_spendable_coins():
    d = rpc("get_spendable_coins", {"wallet_id": 1, "min_coin_amount": 0})
    coins = d.get("confirmed_records", [])
    coins.sort(key=lambda c: c["coin"]["amount"], reverse=True)
    return coins


def cancel_all():
    print("=== Step 1: Cancel ALL offers (fee=0, secure=true) ===")
    d = rpc("cancel_offers", {"fee": 0, "secure": True})
    success = d.get("success", False)
    print(f"  Result: {'OK' if success else 'FAILED'}")
    if not success:
        print(f"  Error: {json.dumps(d, indent=2)}")
    return success


def wait_for_coins(target_spendable=100.0, max_wait=300, poll_interval=15):
    print(f"\n=== Step 2: Waiting for coins to unlock (target >{target_spendable:.0f} XCH spendable) ===")
    start = time.time()
    while time.time() - start < max_wait:
        bal = get_balance()
        elapsed = int(time.time() - start)
        print(f"  [{elapsed:>3d}s] confirmed={bal['confirmed']:.6f}  spendable={bal['spendable']:.6f}  pending_change={bal['pending_change']:.6f}")
        if bal["spendable"] >= target_spendable and bal["pending_change"] == 0:
            print(f"  Coins unlocked!")
            return True
        time.sleep(poll_interval)
    print(f"  Timed out after {max_wait}s")
    return False


def split_coins(target_coin_xch=5.0, max_coins=25):
    print(f"\n=== Step 3: Split large coins into ~{target_coin_xch} XCH each ===")
    coins = get_spendable_coins()
    print(f"  Current coins: {len(coins)}")
    for i, c in enumerate(coins[:10]):
        amt = c["coin"]["amount"] / 1e12
        print(f"    #{i+1}: {amt:.6f} XCH")

    # Find coins larger than 2x target that should be split
    target_mojos = int(target_coin_xch * 1e12)
    large_coins = [c for c in coins if c["coin"]["amount"] > target_mojos * 2]

    if not large_coins:
        print("  No large coins need splitting!")
        return True

    # Get our own address for self-send
    d = rpc("get_next_address", {"wallet_id": 1, "new_address": False})
    address = d.get("address")
    if not address:
        print(f"  Failed to get address: {d}")
        return False
    print(f"  Self-send address: {address[:20]}...")

    # Calculate how many splits needed from largest coins
    total_to_split = sum(c["coin"]["amount"] for c in large_coins) / 1e12
    num_splits = min(int(total_to_split / target_coin_xch) - 1, max_coins)
    print(f"  Large coins total: {total_to_split:.6f} XCH")
    print(f"  Will create {num_splits} self-sends of {target_coin_xch} XCH each")

    sent = 0
    for i in range(num_splits):
        d = rpc("send_transaction", {
            "wallet_id": 1,
            "amount": target_mojos,
            "address": address,
            "fee": 0,
        })
        success = d.get("success", False)
        tx_id = d.get("transaction", {}).get("name", "?")[:16] if success else "N/A"
        status = "OK" if success else "FAIL"
        if not success:
            error = d.get("error", "unknown")
            print(f"  Split #{i+1}/{num_splits}: {status} - {error}")
            if "INVALID_FEE_TOO_CLOSE_TO_ZERO" in str(error) or "not enough" in str(error).lower():
                print("  Ran out of spendable balance, stopping splits")
                break
            continue
        sent += 1
        print(f"  Split #{i+1}/{num_splits}: {status} txid={tx_id}")

    print(f"\n  Sent {sent}/{num_splits} split transactions")
    return sent > 0


def verify():
    print("\n=== Step 4: Verify final state ===")
    bal = get_balance()
    print(f"  Balance: confirmed={bal['confirmed']:.6f}  spendable={bal['spendable']:.6f}  pending_change={bal['pending_change']:.6f}")
    coins = get_spendable_coins()
    print(f"  Spendable coins: {len(coins)}")
    for i, c in enumerate(coins[:30]):
        amt = c["coin"]["amount"] / 1e12
        print(f"    #{i+1}: {amt:.6f} XCH")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"

    if mode in ("cancel", "all"):
        cancel_all()

    if mode in ("wait", "all"):
        ok = wait_for_coins(target_spendable=100.0)
        if not ok and mode == "all":
            print("\nCoins haven't unlocked yet. Run again with 'wait' to keep polling,")
            print("or 'split' once spendable > 100 XCH.")
            sys.exit(1)

    if mode in ("split", "all"):
        split_coins(target_coin_xch=5.0, max_coins=25)

    if mode in ("verify", "all"):
        verify()
