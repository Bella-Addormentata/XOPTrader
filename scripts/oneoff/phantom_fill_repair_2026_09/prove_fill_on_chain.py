# prove_fill_on_chain.py -- READ-ONLY on-chain proof of a booked maker fill.
#
# Header note (2026-09-26): everything below this note is the scratch script
# prove_trade.py, unchanged, as used during the phantom-fill repair of
# trade_log 1900-1902.  It proved trade 1903 (0x476a29ba40...) a REAL take --
# both maker coins spent together at block 9,340,539, and that block holds the
# settlement coin for exactly the 1 XCH offered -- and it read the wallet
# status of the three phantoms (1900 CONFIRMED, 1901 CONFIRMED, 1902
# CANCELLED on that day).
#
# It makes read-only RPC calls to the LOCAL Chia services only: the wallet's
# get_offer (port 9256) and the full node's get_coin_records_by_names and
# get_coin_records_by_parent_ids (port 8555), authenticated with the private
# certificates under ~/.chia/mainnet/config/ssl (it does not verify the local
# services' TLS certificates).  It never opens the XOPTrader database, never
# signs, and never submits a transaction.  It applies the rule v0.10.26 uses:
# every maker coin spent in one block AND that block holds the take's mark.
#
# Usage:    python prove_fill_on_chain.py <trade_id>
# Verdicts: Settled (a real take); Dead (the maker coins were not all spent in
#           one block, or were spent together with no take's mark); Live (every
#           maker coin unspent); Unknown (a maker coin the node does not know).
#
# The code below is kept byte-for-byte as it was run, so it is not restyled:
# ruff: noqa
"""READ-ONLY on-chain proof of a booked maker fill, the rule v0.10.26 applies:
every maker coin spent in one block AND that block holds the take's own mark
(a child of a maker coin, created and spent in the take block, for exactly an
offered amount).  Calls only get_offer (wallet), get_coin_records_by_names and
get_coin_records_by_parent_ids (full node).  Usage: python prove_trade.py <trade_id>"""
import hashlib
import json
import ssl
import sys
import urllib.request
from pathlib import Path

SSL_DIR = Path.home() / ".chia" / "mainnet" / "config" / "ssl"


def rpc(port, service, endpoint, payload):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.load_cert_chain(SSL_DIR / service / f"private_{service}.crt", SSL_DIR / service / f"private_{service}.key")
    req = urllib.request.Request(f"https://localhost:{port}/{endpoint}", data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, context=ctx, timeout=60) as r:
        out = json.loads(r.read())
    if not out.get("success", False):
        raise RuntimeError(f"{endpoint} refused: {out.get('error')}")
    return out


def int_to_bytes(v: int) -> bytes:
    if v == 0:
        return b""
    return v.to_bytes((v.bit_length() + 8) >> 3, "big", signed=True)


def coin_name(parent: str, ph: str, amount: int) -> str:
    h = hashlib.sha256(bytes.fromhex(parent.removeprefix("0x")) + bytes.fromhex(ph.removeprefix("0x"))
                       + int_to_bytes(amount)).hexdigest()
    return "0x" + h


trade_id = sys.argv[1]
rec = rpc(9256, "wallet", "get_offer", {"trade_id": trade_id, "file_contents": False})["trade_record"]
print("wallet status:", rec.get("status"), "confirmed_at_index:", rec.get("confirmed_at_index"),
      "is_my_offer:", rec.get("is_my_offer"))
summary = rec.get("summary", {})
print("summary offered:", summary.get("offered"), "requested:", summary.get("requested"))
coins = rec.get("coins_of_interest", [])
names = [coin_name(c["parent_coin_info"], c["puzzle_hash"], int(c["amount"])) for c in coins]
print(f"maker coins: {len(names)}")
recs = rpc(8555, "full_node", "get_coin_records_by_names",
           {"names": names, "include_spent_coins": True})["coin_records"]
by_name = {coin_name(r["coin"]["parent_coin_info"], r["coin"]["puzzle_hash"], int(r["coin"]["amount"])): r
           for r in recs}
heights = []
for n, c in zip(names, coins):
    r = by_name.get(n)
    if r is None:
        print(f"  {n[:14]} amount={c['amount']}: NOT FOUND on the node")
        heights.append(None)
        continue
    sp = r.get("spent_block_index", 0)
    print(f"  {n[:14]} amount={c['amount']}: confirmed={r.get('confirmed_block_index')} "
          f"spent={bool(r.get('spent'))} spent_block={sp}")
    heights.append(sp if r.get("spent") else 0)
spent = [h for h in heights if h]
if None in heights:
    print("VERDICT: Unknown (a maker coin the node does not know)")
    sys.exit(0)
if not spent:
    print("VERDICT: Live (every maker coin unspent)")
    sys.exit(0)
if len(spent) != len(heights) or len(set(spent)) != 1:
    print(f"VERDICT: Dead (not every maker coin spent in one block: spent heights {sorted(set(spent))}, "
          f"{heights.count(0)} unspent)")
    sys.exit(0)
h = spent[0]
offered_amounts = set()
for asset, amt in (summary.get("offered") or {}).items():
    offered_amounts.add(int(amt))
children = rpc(8555, "full_node", "get_coin_records_by_parent_ids",
               {"parent_ids": names, "include_spent_coins": True, "start_height": h, "end_height": h + 1})["coin_records"]
marks = [c for c in children if c.get("confirmed_block_index") == h and c.get("spent")
         and c.get("spent_block_index") == h and int(c["coin"]["amount"]) in offered_amounts]
print(f"all {len(heights)} maker coins spent together at block {h}; children in that block: {len(children)}; "
      f"settlement-coin marks (created+spent at {h}, offered amount {sorted(offered_amounts)}): {len(marks)}")
print("VERDICT:", "Settled (a real take)" if marks else "Dead (spent together, no take's mark)")
