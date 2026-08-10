#!/usr/bin/env python3
"""Relay one stuck warp.green xch -> bse message (altruistic, trustless).

Delivery can only pay the message's own attested receiver -- a relayer's sole
power is spending its own gas (~145k, sub-cent on Base). Default is a full
rehearsal: fetch, verify signatures, preflight, sign -- and stop. Broadcasting
is a deliberate second step:

    .venv/Scripts/python.exe scripts/warp_relay_once.py                # rehearse oldest (most stuck)
    .venv/Scripts/python.exe scripts/warp_relay_once.py --nonce <hex>  # rehearse a specific one
    .venv/Scripts/python.exe scripts/warp_relay_once.py --broadcast    # actually deliver

Requires warp.evm_private_key_dpapi (or warp.relay_private_key_dpapi, which
wins when present -- a dedicated gas-only key is the recommended hygiene) in
secrets.yaml, and a little Base ETH at that address for --broadcast.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--nonce", help="specific message nonce (hex); default: oldest stuck")
    ap.add_argument("--broadcast", action="store_true",
                    help="actually send the relay transaction (spends gas)")
    ap.add_argument("--grace-min", type=float, default=30.0,
                    help="only touch messages stuck at least this long")
    args = ap.parse_args()

    import requests
    import yaml

    from gui.services.warp import constants as C
    from gui.services.warp import keystore, nostr, relayer
    from gui.services.warp.evm import EvmClient

    net = C.MAINNET
    from gui.services.warp.watcher import _messages_from

    def watcher_fetch(path: str) -> list:
        r = requests.get(f"{net.watcher_api_url.rstrip('/')}{path}", timeout=20)
        r.raise_for_status()
        # Normalise the same way WatcherClient does: the API may return a bare
        # list, {"messages": [...]}, or a single dict -- fetch_stuck_messages
        # expects a list of message dicts, so a raw .json() crashes on the
        # documented non-list shapes.
        return _messages_from(r.json())

    stuck = relayer.fetch_stuck_messages(
        watcher_fetch, grace_s=args.grace_min * 60.0
    )
    if args.nonce:
        want = args.nonce.removeprefix("0x").lower()
        stuck = [m for m in stuck if m.nonce.hex() == want]
    if not stuck:
        print("no matching stuck messages -- nothing to relay")
        return 0
    msg = stuck[0]
    print(f"candidate : nonce {msg.nonce.hex()}")
    print(f"            destination {msg.destination}, stuck ~{msg.age_s/3600:.1f}h")

    evm_client = EvmClient(net, rpc_url=net.evm_default_rpc_url)
    collector = nostr.NostrSigCollector(net)

    calldata = relayer.build_relay_calldata(evm_client, collector, net, msg)
    print(f"signatures: quorum collected and verified; calldata {len(calldata)} bytes")

    reason = relayer.preflight(evm_client, net, calldata)
    if reason is not None:
        print(f"preflight : {reason} -- NOT relaying")
        return 1
    print("preflight : simulation succeeds")

    secrets = yaml.safe_load((_REPO / "secrets.yaml").read_text(encoding="utf-8")) or {}
    warp_sec = secrets.get("warp") or {}
    blob = warp_sec.get("relay_private_key_dpapi") or warp_sec.get("evm_private_key_dpapi")
    if not blob:
        print("no DPAPI key in secrets.yaml -- rehearsal complete, cannot sign")
        return 0
    key = keystore.load_evm_key(blob)
    print(f"relayer   : {key.address}")

    from gui.services.warp import evm as evm_mod

    unsigned = evm_client.prepare_relay(owner=key.address, calldata=calldata)
    signed = evm_mod.sign_tx(unsigned, key.private_key)
    max_cost_eth = unsigned.gas * unsigned.max_fee_per_gas / 1e18
    print(f"signed    : {signed.tx_hash} (max gas cost ~{max_cost_eth:.8f} ETH)")

    if not args.broadcast:
        print("rehearsal complete -- rerun with --broadcast to deliver")
        return 0

    evm_client.send_raw_transaction(signed.raw)
    print(f"BROADCAST : {signed.tx_hash}")
    print("verify    : watcher will show destination_transaction_hash shortly;")
    print(f"            https://basescan.org/tx/{signed.tx_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
