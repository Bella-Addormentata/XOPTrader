#!/usr/bin/env python3
"""Diagnostic: show wallet balances and pending offers."""
import json, ssl, urllib.request, os

ctx = ssl.create_default_context()
home = os.path.expanduser("~")
ctx.load_cert_chain(
    os.path.join(home, ".chia/mainnet/config/ssl/wallet/private_wallet.crt"),
    os.path.join(home, ".chia/mainnet/config/ssl/wallet/private_wallet.key"),
)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

KNOWN_ASSETS = {
    "xch": "XCH",
    "a628c1c2c6fcb74d53746157e438e108eab5c0bb3e5c80ff3b1663602f150340": "wUSDC.b",
    "8ebf855de6eb146db5602f0456d2f0cbe750d57f821b6f91a8592ee9f1d4cf31": "DBX",
    "6d95dae356e32a71db5ddcb42224754a02524c615c5fc35f568c2af04774e589": "BYC",
    "77c20977be5e09dcb29f00178c02550F5b227899de18be28b0fe3a6aa76e3a38": "wETH.b",
    "6fdb43e6e7b2a3ba7bf92e8e23e316323bf98e77d47b549e2d0db59a9e1d6cf8": "EURC",
    "509deafe3cd8bbfbb9ccce1d930e3d7b57b40c964fa33379b18d628175eb7a8f": "DBX(old)",
}

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

# XCH balance
b = rpc("get_wallet_balance", {"wallet_id": 1})
bal = b["wallet_balance"]
print("=== XCH Balance ===")
print(f"  Confirmed:      {bal['confirmed_wallet_balance'] / 1e12:.6f} XCH")
print(f"  Spendable:      {bal['spendable_balance'] / 1e12:.6f} XCH")
print(f"  Pending change: {bal['pending_change'] / 1e12:.6f} XCH")
print(f"  Unconfirmed:    {bal['unconfirmed_wallet_balance'] / 1e12:.6f} XCH")
print()

# All wallets
print("=== All Wallets ===")
wallets = rpc("get_wallets")
for w in wallets["wallets"]:
    wid = w["id"]
    try:
        b2 = rpc("get_wallet_balance", {"wallet_id": wid})
        bal2 = b2["wallet_balance"]
        conf = bal2["confirmed_wallet_balance"]
        spend = bal2["spendable_balance"]
        if conf > 0 or spend > 0:
            print(f"  WID {wid:2d} | {w['name']:30s} | conf={conf:>15,} spend={spend:>15,}")
    except Exception as e:
        print(f"  WID {wid:2d} | {w['name']:30s} | ERROR: {e}")
print()

# Pending offers
print("=== Pending Offers ===")
offers = rpc("get_all_offers", {"file_contents": True})
pending = [o for o in offers.get("trade_records", []) if o["status"] == 4]
print(f"Total pending: {len(pending)}")
total_xch_offered = 0
total_xch_requested = 0
for o in pending:
    summary = o.get("summary", {})
    offered = summary.get("offered", {})
    requested = summary.get("requested", {})

    def fmt_assets(d):
        parts = []
        for asset_id, amount in d.items():
            name = KNOWN_ASSETS.get(asset_id, asset_id[:8] + "...")
            if asset_id == "xch":
                parts.append(f"{amount / 1e12:.4f} {name}")
            else:
                parts.append(f"{amount / 1e3:.3f} {name}")
        return ", ".join(parts) if parts else "(none)"

    xch_off = offered.get("xch", 0)
    xch_req = requested.get("xch", 0)
    total_xch_offered += xch_off
    total_xch_requested += xch_req

    print(f"  {o['trade_id'][:16]}... offer={fmt_assets(offered):>30s}  want={fmt_assets(requested):>30s}")

print()
print(f"  XCH locked in offers (selling): {total_xch_offered / 1e12:.6f} XCH")
print(f"  XCH incoming (buying):          {total_xch_requested / 1e12:.6f} XCH")
print(f"  Net XCH exposure in offers:     {(total_xch_requested - total_xch_offered) / 1e12:.6f} XCH")
