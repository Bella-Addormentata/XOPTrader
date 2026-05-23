"""Diagnostic for the Target Portfolio Allocation panel.

Dumps:
  * Raw `get_wallets` response (name, type, data field).
  * Resolved asset_id per wallet via wallet_service.WalletService.
  * Enabled pairs from config.yaml with base/quote asset_ids.
  * Computed asset_id -> symbol map.
  * Symbol resolution for each wallet (XCH / matched CAT symbol / None).

Run from repo root:
    .venv\\Scripts\\python.exe scripts\\debug_wallet_allocation.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from gui.services.config_split import load_merged  # noqa: E402
from gui.services.wallet_service import WalletService  # noqa: E402


def main() -> int:
    cfg_path = ROOT / "config.yaml"
    cfg = load_merged(cfg_path)

    svc = WalletService(cfg)
    balances = svc.fetch_balances()

    print("=" * 70)
    print("WALLET BALANCES (from WalletService.fetch_balances)")
    print("=" * 70)
    for name, data in balances.items():
        print(f"  {name!r}")
        print(f"    wallet_id   : {data.get('wallet_id')}")
        print(f"    wallet_type : {data.get('wallet_type')}")
        print(f"    asset_id    : {data.get('asset_id', '')!r}")
        print(f"    confirmed   : {data.get('confirmed')}")
    if not balances:
        print("  (no balances returned -- check certs / fingerprint / daemon)")

    print()
    print("=" * 70)
    print("ENABLED PAIRS (from config.yaml)")
    print("=" * 70)
    pairs = cfg.get("pairs", []) or []
    asset_id_map: dict[str, str] = {}
    for pair in pairs:
        if not pair.get("enabled", True):
            continue
        pair_name = str(pair.get("name", ""))
        base_id = str(pair.get("base_asset_id", "") or "").lower()
        quote_id = str(pair.get("quote_asset_id", "") or "").lower()
        print(f"  {pair_name}  base={base_id[:16]}.. quote={quote_id[:16]}..")
        if "/" in pair_name:
            base_sym, quote_sym = pair_name.split("/", 1)
            if base_id and base_id != "xch":
                asset_id_map[base_id] = base_sym.upper()
            if quote_id and quote_id != "xch":
                asset_id_map[quote_id] = quote_sym.upper()

    print()
    print("=" * 70)
    print("ASSET-ID -> SYMBOL MAP")
    print("=" * 70)
    print(json.dumps(asset_id_map, indent=2))

    print()
    print("=" * 70)
    print("WALLET -> RESOLVED SYMBOL")
    print("=" * 70)
    for name, data in balances.items():
        wt = int(data.get("wallet_type", -1))
        if wt == 0:
            sym = "XCH"
        else:
            aid = str(data.get("asset_id", "") or "").lower()
            sym = asset_id_map.get(aid)
        marker = "OK" if sym else "MISS"
        print(f"  [{marker}]  {name!r:40s} -> {sym}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
