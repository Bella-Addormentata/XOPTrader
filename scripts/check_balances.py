#!/usr/bin/env python3
"""Show wallets and balances."""
import subprocess, json, sys

r = subprocess.run(['chia', 'rpc', 'wallet', 'get_wallets'], input='{}', capture_output=True, text=True)
d = json.loads(r.stdout)

for w in d.get('wallets', []):
    wid = w['id']
    name = w['name']
    
    br = subprocess.run(
        ['chia', 'rpc', 'wallet', 'get_wallet_balance'],
        input=json.dumps({"wallet_id": wid}),
        capture_output=True, text=True,
        timeout=10
    )
    try:
        bd = json.loads(br.stdout)
    except json.JSONDecodeError:
        print(f"WID {wid:>2} | {name:30s} | (RPC error: {br.stderr.strip()[:60]})")
        continue
    if bd.get('success'):
        b = bd['wallet_balance']
        confirmed = b.get('confirmed_wallet_balance', 0)
        spendable = b.get('spendable_balance', 0)
        pending = b.get('pending_change', 0)
        
        if wid == 1:
            div = 1e12
        else:
            div = 1e3
        
        print(f"WID {wid:>2} | {name:30s} | confirmed: {confirmed/div:>14.6f} | spendable: {spendable/div:>14.6f} | pending: {pending/div:>10.6f}")
