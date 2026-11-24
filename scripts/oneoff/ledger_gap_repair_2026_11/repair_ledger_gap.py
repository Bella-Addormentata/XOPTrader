#!/usr/bin/env python3
"""Repair the trading ledger's 2026-11 gap.  ONE-OFF.

    python repair_ledger_gap.py --db data/xop_trader.db --config config.yaml           # dry run
    python repair_ledger_gap.py --db data/xop_trader.db --config config.yaml --apply   # write
    python repair_ledger_gap.py --db data/xop_trader.db --config config.yaml --check   # after

The dry run and --check only read: the engine database (read-only URI) and
the local Chia wallet's RPC (get_wallets, get_coin_records, get_all_offers,
get_offer, get_transactions).  --apply writes, and refuses unless the engine
(xop_trader.exe) and the GUI (xop_trader_gui.exe) are stopped: it backs the
database up with the SQLite backup API first, then appends everything in ONE
transaction.  Run --apply in the stop before the first start of the release
that books takes on settlement, so no engine runs between the repair and the
fix.  docs/LEDGER-GAP-REPAIR-2026-11.md has the runbook and the reasoning.

WHAT WAS FOUND (read-only trace, 2026-11-24).  From the XCH re-tie of
2026-09-03 (adjust:xch:9241973, where the gap was exactly 0) the ledger drifted
to 17.388 XCH above the wallet, 833.855 DBX and 10.013 BYC below it and
5.761 wUSDC.b above it (wallet = every coin except the options address's).
Every mojo of it is one of these items, and this script posts each one:

  A  takes booked that never settled -- the wallet holds each FAILED or
     CANCELLED and no coin of it was paid to us.  REVERSAL of every leg
     (policy s.2 "Reversal"), and the taker_fills row is archived in
     offer_closure_events ('phantom_take_correction') and deleted.
  B  the BYC auto-adjusts posted after the XCH re-tie.  They net to exactly
     the BYC legs of the takes in A booked before them, less the BYC paid by
     the fill in C that they also absorbed.  One CORRECTION per adjust
     (policy s.2 "Correcting an auto-adjust"); the script refuses unless they
     net to exactly that.
  C  trades that settled but were never booked: takes (payment coin in the
     wallet at the take's height; 'take' legs at the wallet's amounts, fee 0
     because the wallet's record does not state ours, plus a taker_fills row)
     and our own offers taken while offer_log called them cancelled ('fill'
     legs only: trade_log and offer_log are left as they are, a documented
     residual -- unknown cost is never fabricated, policy s.6).
  D  XCH between the trading addresses and the options address that the
     options ingest never booked: a send it did not book (options_withdrawal)
     and every return (options_deposit), USD at the engine's own XCH mark of
     the nearest snapshot (Net Deposits read flow_usd=).
  E  Dexie reward coins never booked, valued at the px_usd_per_unit of the
     nearest reward the engine did book.
  F  the XCH network fees paid outside fills and takes (cancels, coin splits):
     measured from the coin store, and cross-checked against what is left of
     the XCH gap after A-E.  One 'fee' event.
  Any other external coin left over is reported, not booked: on 2026-11-24
  a 13-mojo BYC coin, and 9 mojos of wUSDC.b that sit at the options address.

Nothing else is changed: inventory_state is re-tied by the engine's Step 11 at
boot (policy s.2 "The other stores").
"""

from __future__ import annotations

import argparse
import bisect
import collections
import datetime as dt
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import sys
import warnings

import requests
import yaml

warnings.filterwarnings("ignore")

TAG = "ledger_gap_repair_2026_11"
RETIE_EVENT_ID = "adjust:xch:9241973"     # the XCH re-tie the gap is measured from
XCH = "xch"
BYC = "ae1536f56760e471ad85ead45f00d680ff9cca73b8cc3407be778f1c0c606eac"
DBX = "db1a9020d48d9d4ad22631b66ab4b9ebd3637ef7758ad38881348c5d24c38f20"
FEE_MATCH_TOLERANCE = 10_000_000_000      # 0.01 XCH: measured fees vs the remainder
FEE_PER_HEIGHT_MAX = 1_000_000_000        # 0.001 XCH: the most one block's fees may be
REWARD_MAX_MOJOS = 20_000                 # accounting.reward_max_mojos_per_coin's default
REWARD_LONE_MAX_MOJOS = 2_000             # accounting::kRewardLoneCoinMaxMojos
REWARD_BURST_MIN_COINS = 5                # accounting::kRewardBurstMinCoins
GIVE = 50                                 # blocks a payment coin may trail its trade
PHANTOM_WINDOW = 4_608                    # blocks after a take's booking its payment is looked for
MOJOS_PER_XCH = 10 ** 12

TAKER_COLUMNS = ("taken_at", "block_height", "strategy", "trade_id", "counterparty_offer_id",
                 "pair_name", "we_bought_base", "base_asset", "base_delta_mojos", "quote_asset",
                 "quote_delta_mojos", "price_mojos", "fee_mojos")
TAKER_INSERT = ("INSERT OR IGNORE INTO taker_fills (taken_at, block_height, strategy, trade_id, "
                "counterparty_offer_id, pair_name, we_bought_base, base_asset, base_delta_mojos, "
                "quote_asset, quote_delta_mojos, price_mojos, fee_mojos) "
                "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)")

SSL = pathlib.Path.home() / ".chia" / "mainnet" / "config" / "ssl" / "wallet"
WALLET_URL = "https://localhost:9256/"
LEDGER_QUERY = ("SELECT id, entry_time, event_type, event_id, leg, asset_id, delta_mojos, "
                "pair_name, block_height, note FROM ledger_entries ORDER BY id")


def units(asset: str) -> int:
    return MOJOS_PER_XCH if asset == XCH else 10 ** 3


def fmt(asset: str, mojos: int) -> str:
    return f"{mojos / units(asset):+,.6f}"


def norm(asset_or_id: str) -> str:
    s = str(asset_or_id or "").strip().lower()
    return s[2:] if s.startswith("0x") else s


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


# ---------------------------------------------------------------- wallet RPC
class Wallet:
    """Read-only calls to the local wallet daemon."""

    def __init__(self) -> None:
        self.cert = (str(SSL / "private_wallet.crt"), str(SSL / "private_wallet.key"))

    def rpc(self, endpoint: str, body: dict) -> dict:
        # The daemon's certificate is signed by the node's own private CA and
        # names no host; the client certificate is what authenticates.
        r = requests.post(WALLET_URL + endpoint, json=body, cert=self.cert,
                          verify=False, timeout=120)  # noqa: S501 -- local daemon, see above
        out = r.json()
        if not out.get("success", False):
            raise SystemExit(f"wallet {endpoint} refused: {out.get('error')}")
        return out

    def wallet_ids(self) -> dict:
        """asset id (normalised, 'xch' for the standard wallet) -> wallet id."""
        ids = {}
        for w in self.rpc("get_wallets", {})["wallets"]:
            if w["type"] == 0:
                ids[XCH] = w["id"]
            elif w["type"] == 6:
                asset = norm(w.get("data", "")[:64])
                if asset:
                    ids[asset] = w["id"]
        return ids

    def coins(self, wallet_id: int) -> list:
        out, offset = [], 0
        while True:
            r = self.rpc("get_coin_records", {"wallet_id": wallet_id, "offset": offset,
                                              "limit": 1000, "include_total_count": True})
            rows = r.get("coin_records") or []
            for c in rows:
                out.append({"id": norm(c["id"]), "parent": norm(c["parent_coin_info"]),
                            "ph": norm(c["puzzle_hash"]), "amount": int(str(c["amount"])),
                            "conf": int(c.get("confirmed_height") or 0),
                            "spent": int(c.get("spent_height") or 0)})
            offset += len(rows)
            if not rows or offset >= int(r.get("total_count") or 0):
                return out

    def trades(self) -> list:
        """Every trade record with a confirmation height, newest first."""
        out, start = [], 0
        while True:
            body = {"start": start, "end": start + 200, "include_completed": True,
                    "file_contents": False}
            rows = self.rpc("get_all_offers", body).get("trade_records") or []
            done = False
            for rec in rows:
                if int(rec.get("confirmed_at_index") or 0) == 0:
                    done = True
                    continue
                out.append(rec)
            start += len(rows)
            if done or not rows:
                return out

    def offer(self, trade_id: str) -> dict:
        body = {"trade_id": trade_id, "file_contents": False}
        return self.rpc("get_offer", body)["trade_record"]

    def incoming_txs(self, wallet_id: int, since: int) -> list:
        out, start = [], 0
        while True:
            # RELEVANCE with reverse false: unconfirmed rows first, then confirmed
            # rows newest first (rpc/wallet_requests.hpp make_get_transactions_request).
            body = {"wallet_id": wallet_id, "start": start, "end": start + 500,
                    "sort_key": "RELEVANCE", "reverse": False}
            rows = self.rpc("get_transactions", body).get("transactions") or []
            for t in rows:
                if t.get("type") == 0 and t.get("confirmed"):
                    out.append({"name": norm(t["name"]), "amount": int(str(t["amount"])),
                                "height": int(t.get("confirmed_at_height") or 0)})
            start += len(rows)
            heights = [int(t.get("confirmed_at_height") or 0) for t in rows if t.get("confirmed")]
            oldest = min(heights, default=0)
            if len(rows) < 500 or (oldest and oldest < since):
                return out


# ---------------------------------------------------------------- bech32m
CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def puzzle_hash_of(address: str) -> str:
    hrp, data = address[: address.rfind("1")], address[address.rfind("1") + 1:]
    values = [CHARSET.index(c) for c in data]
    gen = [0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3]
    chk = 1
    for v in [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp] + values:
        top = chk >> 25
        chk = ((chk & 0x1FFFFFF) << 5) ^ v
        for i in range(5):
            if (top >> i) & 1:
                chk ^= gen[i]
    if chk != 0x2BC830A3:
        raise SystemExit("options address: bad bech32m checksum")
    acc, bits, out = 0, 0, []
    for v in values[:-6]:
        acc = (acc << 5) | v
        bits += 5
        while bits >= 8:
            bits -= 8
            out.append((acc >> bits) & 0xFF)
    return bytes(out).hex()


# ---------------------------------------------------------------- helpers
def summary_side(rec: dict, key: str) -> dict:
    side = (rec.get("summary") or {}).get(key) or {}
    out = {}
    for asset, amount in side.items():
        text = str(amount)
        if not text.isdigit():
            raise SystemExit(f"trade {rec.get('trade_id')}: unreadable amount {amount!r}")
        out[norm(asset)] = int(text)
    return out


def receive_and_pay(rec: dict) -> tuple[dict, dict]:
    """What a trade paid us and what we paid, from OUR side."""
    offered, requested = summary_side(rec, "offered"), summary_side(rec, "requested")
    return (requested, offered) if rec.get("is_my_offer") else (offered, requested)


class CoinPool:
    """Our coins, consumable: one coin vouches for one receipt."""

    def __init__(self, coins_by_asset: dict) -> None:
        self.free = collections.Counter()
        for asset, coins in coins_by_asset.items():
            for c in coins:
                self.free[(asset, c["conf"], c["amount"])] += 1

    def take(self, asset: str, height: int, amount: int, give: int = 0) -> int | None:
        for h in range(height, height + give + 1):
            if self.free[(asset, h, amount)] > 0:
                self.free[(asset, h, amount)] -= 1
                return h
        return None


def nearest(rows: list, height: int) -> float | None:
    """The value of the row nearest `height`; `rows` sorted by height, values > 0."""
    if not rows:
        return None
    i = bisect.bisect_left(rows, (height, float("-inf")))
    near = [rows[j] for j in (i - 1, i) if 0 <= j < len(rows)]
    return min(near, key=lambda r: abs(r[0] - height))[1]


def pair_for(pairs: list, assets: set) -> tuple[str, str, str]:
    """The configured pair (name, base, quote) whose two assets are `assets`."""
    for name, base, quote in pairs:
        if {base, quote} == assets:
            return name, base, quote
    raise SystemExit(f"no configured pair trades {sorted(assets)}")


def engine_price(base: str, base_mojos: int, quote: str, quote_mojos: int) -> int:
    """execution::price_from_legs: quote units per base unit, in 1e12 fixed point."""
    return round(quote_mojos * units(base) * MOJOS_PER_XCH / (base_mojos * units(quote)))


# ---------------------------------------------------------------- the plan
def proven_trades(trades: list, coins: dict, booked: dict) -> tuple[dict, CoinPool]:
    """The CONFIRMED trades whose payment coin is in our store, by trade id,
    and the pool of the coins none of them claimed.

    Booked trades claim their coins first, so an unbooked twin never takes a
    booked trade's payment; a coin vouches for one receipt only."""
    pool = CoinPool(coins)
    real = {}
    order = sorted(trades, key=lambda t: (norm(t["trade_id"]) not in booked,
                                          int(t["confirmed_at_index"])))
    for t in order:
        if t.get("status") != "CONFIRMED":
            continue
        recv, _ = receive_and_pay(t)
        h = int(t["confirmed_at_index"])
        claims, ok = [], bool(recv)
        for asset, amount in recv.items():
            at = pool.take(asset, h, amount, GIVE) if asset in coins else None
            if at is None:
                ok = False
                break
            claims.append((asset, at, amount))
        if not ok:
            for asset, at, amount in claims:
                pool.free[(asset, at, amount)] += 1
            continue
        # The block the payment landed in, which is the block of the spend:
        # the wallet's confirmed_at_index can lag it by a block.
        real[norm(t["trade_id"])] = dict(t, settled_at=max(at for _, at, _ in claims))
    return real, pool


def build_plan(db: sqlite3.Connection, wallet: Wallet, options_address: str,
               pairs: list) -> dict:
    snapshot = ledger_snapshot(db)
    led = db.execute(LEDGER_QUERY).fetchall()
    if any(TAG in (r[9] or "") for r in led):
        raise SystemExit(f"the ledger already holds rows tagged {TAG}: the repair has run")
    retie = [r for r in led if r[3] == RETIE_EVENT_ID]
    if len(retie) != 1:
        raise SystemExit(f"re-tie {RETIE_EVENT_ID} not found exactly once")
    retie_id, retie_h = retie[0][0], int(retie[0][8])

    ids = wallet.wallet_ids()
    assets = [a for a in ids if a == XCH or any(norm(r[5]) == a for r in led)]
    coins = {a: wallet.coins(ids[a]) for a in assets}
    trades = wallet.trades()
    booked = collections.defaultdict(list)
    for r in led:
        for h in re.findall(r"[0-9a-fA-F]{64}", r[3] or ""):
            booked[h.lower()].append(r)
    real, unclaimed = proven_trades(trades, coins, booked)

    entries, taker_rows = [], []

    def post(kind, et, eid, leg, asset, delta, pair, height, note):
        entries.append({"kind": kind, "event_type": et, "event_id": eid, "leg": leg,
                        "asset_id": asset, "delta_mojos": int(delta), "pair_name": pair or "",
                        "block_height": int(height or 0), "note": f"{note}; {TAG}"})

    # A. takes booked since the re-tie that never settled -------------------
    phantoms = []
    takes = collections.defaultdict(list)
    for r in led:
        if r[0] > retie_id and r[2] == "take":
            takes[r[3]].append(r)
    for eid, legs in takes.items():
        tid = norm(eid.split(":", 1)[1])
        if tid in real:
            continue
        rec = wallet.offer("0x" + tid)
        status = rec.get("status")
        if status not in ("FAILED", "CANCELLED"):
            raise SystemExit(f"{eid}: booked but not proven settled, and the wallet says "
                             f"{status} -- resolve it first")
        # The wallet's status is its bookkeeping, not proof: an insecure cancel
        # writes CANCELLED whatever happened.  A reversal also needs the
        # payment to be absent: no coin of any amount the take would have paid
        # us, from its booking to a day after, that no proven trade claimed.
        recv, _ = receive_and_pay(rec)
        if not recv:
            raise SystemExit(f"{eid}: its wallet record states no amounts, so its payment "
                             "cannot be checked -- resolve it by hand")
        h0 = min(int(r[8] or 0) for r in legs)
        payable = [asset for asset, amount in recv.items()
                   if any(unclaimed.free[(asset, h, amount)] > 0
                          for h in range(h0 - GIVE, h0 + PHANTOM_WINDOW + 1))]
        if len(payable) == len(recv):
            raise SystemExit(f"{eid}: the wallet holds it {status}, but a coin that could be "
                             f"its payment is in the store and unclaimed ({sorted(recv.items())} "
                             f"from block {h0}) -- resolve it by hand")
        phantoms.append((eid, tid, status, legs))
        for (rid, _t, _et, _eid, leg, asset, delta, pair, h, _note) in legs:
            post("A", "reversal", "reversal:" + eid, leg, norm(asset), -delta, pair, h,
                 f"reverses ledger_entries id={rid} ({eid} leg={leg}); the take never "
                 f"settled: the wallet holds it {status} and no coin of it was paid to us")

    # C. settled trades never booked, since the re-tie ----------------------
    missed = []
    for tid, t in real.items():
        h = int(t["settled_at"])   # the block its payment landed in
        if h <= retie_h or tid in booked:
            continue
        recv, paid = receive_and_pay(t)
        if len(recv) != 1 or len(paid) != 1:
            raise SystemExit(f"trade 0x{tid}: not a two-asset trade, book it by hand")
        name, base, quote = pair_for(pairs, set(recv) | set(paid))
        missed.append(t)
        if t.get("is_my_offer"):
            et, eid = "fill", "0x" + tid
            why = ("settled fill the engine recorded as cancelled: its payment coin is in "
                   "the wallet at this height")
        else:
            et, eid = "take", "take:0x" + tid
            why = ("settled take the engine never recorded: its payment coin is in the wallet "
                   "at this height; fee not in the wallet's record")
        legs = {a: v for a, v in recv.items()} | {a: -v for a, v in paid.items()}
        for asset, amount in legs.items():
            post("C", et, eid, "base" if asset == base else "quote", asset, amount, name, h, why)
        if et == "take":
            created = int(t.get("created_at_time") or 0)
            taker_rows.append({
                "taken_at": (dt.datetime.fromtimestamp(created, dt.UTC)
                             .strftime("%Y-%m-%dT%H:%M:%S.000Z") if created else utc_now()),
                "block_height": h, "strategy": "repair_2026_11", "trade_id": "0x" + tid,
                "counterparty_offer_id": "", "pair_name": name,
                "we_bought_base": 1 if legs[base] > 0 else 0, "base_asset": base,
                "base_delta_mojos": legs[base], "quote_asset": quote,
                "quote_delta_mojos": legs[quote],
                "price_mojos": engine_price(base, abs(legs[base]), quote, abs(legs[quote])),
                "fee_mojos": 0})

    # B. BYC auto-adjusts after the re-tie ----------------------------------
    byc_adjusts = [r for r in led if r[0] > retie_id and r[2] == "adjust" and norm(r[5]) == BYC
                   and (r[3] or "").startswith("adjust:")]
    if byc_adjusts:
        last_id, last_h = byc_adjusts[-1][0], int(byc_adjusts[-1][8] or 0)
        absorbed = 0
        for _eid, _tid, _status, legs in phantoms:
            for r in legs:
                if norm(r[5]) == BYC and r[0] < last_id:
                    absorbed -= r[6]     # the adjusts put back what the phantom leg took out
        for t in missed:
            if int(t["confirmed_at_index"]) <= last_h:
                recv, paid = receive_and_pay(t)
                absorbed += recv.get(BYC, 0) - paid.get(BYC, 0)
        total = sum(r[6] for r in byc_adjusts)
        if total != absorbed:
            raise SystemExit(f"BYC auto-adjusts net {total} mojos, but the items they absorbed "
                             f"net {absorbed}: the corrections would not be exact")
        for (rid, _t, _et, eid, _leg, _asset, delta, pair, h, _note) in byc_adjusts:
            post("B", "adjust", "correction:" + eid, "adjust", BYC, -delta, pair, h,
                 f"explains {-delta:+d} of adjust ledger_entries id={rid} ({eid}): the BYC "
                 f"auto-adjusts after {RETIE_EVENT_ID} net to exactly the BYC legs of the takes "
                 "reversed here that never settled and of the settled trades booked here; the "
                 "rest of each was transient and reversed by the next; 0 is still unexplained")

    # D. options address round trips ----------------------------------------
    oph = puzzle_hash_of(options_address)
    xch_coins = coins[XCH]
    byid = {c["id"]: c for c in xch_coins}
    snaps = sorted((int(h), float(v)) for h, v in db.execute(
        "SELECT block_height, AVG(xch_usd_rate) FROM snapshots WHERE xch_usd_rate > 0 "
        "GROUP BY block_height"))
    # The top-ups the options ingest booked (options_topup:<tx>); a fence
    # event's legs are what the fence held, not a send, and may be 0.
    booked_sends = collections.Counter(-r[6] for r in led
                                       if r[2] == "options_withdrawal" and r[4] == "options"
                                       and norm(r[5]) == XCH and r[6] != 0
                                       and (r[3] or "").startswith("options_topup:"))
    for c in sorted(xch_coins, key=lambda c: c["conf"]):
        parent = byid.get(c["parent"])
        if parent is None or (c["ph"] == oph) == (parent["ph"] == oph):
            continue    # not a crossing between the trading addresses and the options one
        px = nearest(snaps, c["conf"])
        if px is None:
            raise SystemExit(f"no XCH price near block {c['conf']}")
        if c["ph"] == oph:
            if booked_sends[c["amount"]] > 0:
                booked_sends[c["amount"]] -= 1
                continue
            usd = -c["amount"] / MOJOS_PER_XCH * px
            post("D", "options_withdrawal", "options_send:0x" + c["id"], "options", XCH,
                 -c["amount"], "", c["conf"],
                 f"options sub-account withdrawal; flow_usd={usd:.10f}; "
                 f"px_usd_per_unit={px:.10f}; ref=a send the options ingest never booked")
        else:
            usd = c["amount"] / MOJOS_PER_XCH * px
            post("D", "options_deposit", "options_return:0x" + c["id"], "options", XCH,
                 c["amount"], "", c["conf"],
                 f"options sub-account deposit; flow_usd={usd:.10f}; "
                 f"px_usd_per_unit={px:.10f}; ref=XCH back from the options address")
    if any(n > 0 for n in booked_sends.values()):
        raise SystemExit(f"booked options sends with no coin to match: {dict(booked_sends)}")

    # E. external coins left over: rewards (DBX), anything else reported -----
    rewards_booked = collections.Counter()
    reward_px = []
    for r in led:
        if r[2] == "reward":
            rewards_booked[(int(r[8] or 0), r[6])] += 1
            m = re.search(r"px_usd_per_unit=([0-9.]+)", r[9] or "")
            if m and float(m.group(1)) > 0:
                reward_px.append((int(r[8] or 0), float(m.group(1))))
    reward_px.sort()
    leftovers = {}
    confirmed_ids = {norm(t["trade_id"]) for t in trades if t.get("status") == "CONFIRMED"}
    for asset, cs in coins.items():
        if asset == XCH:
            continue    # XCH: no reward stream; options flows are D, the rest is F
        last = [int(r[8] or 0) for r in led
                if norm(r[5]) == asset and r[2] in ("adjust", "opening")]
        since = max([*last, retie_h])
        ours = {c["id"] for c in cs}
        left = collections.Counter((c["conf"], c["amount"]) for c in cs
                                   if c["conf"] > since and c["parent"] not in ours)
        for t in trades:   # every CONFIRMED trade's receipt, booked or not
            if t.get("status") != "CONFIRMED":
                continue
            recv, _ = receive_and_pay(t)
            if asset in recv:
                h0 = int(t["confirmed_at_index"])
                for k in ((h0 + d, recv[asset]) for d in range(GIVE + 1)):
                    if left[k] > 0:
                        left[k] -= 1
                        break
        for r in led:      # booked receipts a stale wallet status hides (PENDING_CANCEL)
            if r[2] not in ("fill", "take") or norm(r[5]) != asset or r[6] <= 0:
                continue
            hexes = re.findall(r"[0-9a-fA-F]{64}", r[3] or "")
            if hexes and hexes[0].lower() in confirmed_ids:
                continue   # its receipt came off above, with its wallet record
            h0 = int(r[8] or 0)
            for k in ((h0 + d, r[6]) for d in range(-GIVE, GIVE + 1)):
                if left[k] > 0:
                    left[k] -= 1
                    break
        if asset != DBX:
            leftovers[asset] = sorted(k for k, n in left.items() for _ in range(max(n, 0)))
            continue
        # Reward-sized receipts per block, booked or not (the engine's count):
        # a coin above the old ceiling is a reward only inside a payout burst.
        in_block = collections.Counter()
        for (h, amount), n in left.items():
            if 0 < amount <= REWARD_MAX_MOJOS:
                in_block[h] += max(n, 0)
        for key, n in list(left.items()):
            left[key] -= min(n, rewards_booked[key])
        names = collections.defaultdict(list)
        for tx in wallet.incoming_txs(ids[asset], since):
            names[(tx["height"], tx["amount"])].append(tx["name"])
        booked_names = {norm(r[3].split(":", 1)[1]) for r in led if r[2] == "reward"}
        too_big = []
        for (h, amount), n in sorted(left.items()):
            if amount > REWARD_MAX_MOJOS or (amount > REWARD_LONE_MAX_MOJOS
                                             and in_block[h] < REWARD_BURST_MIN_COINS):
                too_big += [(h, amount)] * max(n, 0)   # not a reward by the engine's own rule
                continue
            for _ in range(n):
                free = [x for x in names.get((h, amount), []) if x not in booked_names]
                if not free:
                    raise SystemExit(f"reward coin {amount} at {h} has no unbooked wallet tx")
                name = free[0]
                booked_names.add(name)
                px = nearest(reward_px, h)
                if px is None:
                    raise SystemExit("no reward price in the ledger to value a missed reward")
                usd = amount / 1e3 * px
                post("E", "reward", "reward:0x" + name, "reward", asset, amount, "", h,
                     f"dexie liquidity reward; fmv_usd={usd:.10f}; px_usd_per_unit={px:.10f}; "
                     f"wallet_tx=0x{name}; booked late by the repair")
        leftovers[asset] = too_big

    # F. XCH network fees paid outside fills and takes ----------------------
    kids = collections.defaultdict(int)
    for c in xch_coins:
        kids[c["parent"]] += c["amount"]
    spent = [c for c in xch_coins if c["ph"] != oph and c["spent"] > retie_h]
    left_by_height = collections.Counter()
    for c in spent:
        left_by_height[c["spent"]] += c["amount"] - kids.get(c["id"], 0)
    left_wallet = sum(left_by_height.values())
    end_h = max((c["spent"] for c in spent), default=retie_h)
    paid_xch = 0
    for t in real.values():
        if int(t["confirmed_at_index"]) > retie_h:
            _, paid = receive_and_pay(t)
            paid_xch += paid.get(XCH, 0)
            left_by_height[int(t["settled_at"])] -= paid.get(XCH, 0)
    # A network fee is small.  XCH that left a trading coin at one height,
    # beyond the XCH legs of the trades settled there, by more than
    # FEE_PER_HEIGHT_MAX is a payment to someone, not a fee: refuse to call
    # it one.
    not_fees = {h: v for h, v in left_by_height.items() if v > FEE_PER_HEIGHT_MAX}
    if not_fees:
        listed = ", ".join(f"block {h}: {v / MOJOS_PER_XCH:.6f} XCH"
                           for h, v in sorted(not_fees.items())[:20])
        raise SystemExit(f"{len(not_fees)} XCH outflow(s) are not trades and too large to be "
                         f"network fees -- explain them before repairing: {listed}")
    # Fee legs of events reversed already (the 2026-09 phantom fills) or here
    # were never paid.
    reversed_events = ({e["event_id"] for e in entries if e["event_type"] == "reversal"}
                       | {r[3] for r in led if r[2] == "reversal"})
    booked_fees = -sum(r[6] for r in led if r[0] > retie_id and norm(r[5]) == XCH
                       and r[4] == "fee" and r[2] in ("fill", "take", "options_withdrawal")
                       and "reversal:" + r[3] not in reversed_events)
    measured = left_wallet - paid_xch - booked_fees
    post("F", "fee", f"fee:network:{retie_h}-{end_h}", "fee", XCH, -measured, "", end_h,
         "network fees paid outside fills and takes (cancels, coin splits) from "
         f"{RETIE_EVENT_ID} to block {end_h}, measured from the coin store: {left_wallet} "
         f"mojos left trading coins, {paid_xch} of them trades' XCH legs and {booked_fees} "
         "fees already booked; ACCOUNTING-POLICY s.8 known gap")
    return {"entries": entries, "taker_rows": taker_rows, "snapshot": snapshot,
            "phantoms": [(e, t, s) for e, t, s, _ in phantoms],
            "missed": [norm(t["trade_id"]) for t in missed], "coins": coins, "oph": oph,
            "led": led, "measured_fee": measured, "leftovers": leftovers}


def engine_view_gap(led: list, coins: dict, oph: str) -> collections.Counter:
    """ledger - wallet for each asset, the options address left out (XCH)."""
    gap = collections.Counter()
    for r in led:
        gap[norm(r[5])] += r[6]
    for asset, cs in coins.items():
        held = sum(c["amount"] for c in cs
                   if c["spent"] == 0 and not (asset == XCH and c["ph"] == oph))
        gap[asset] -= held
    return gap


def report(plan: dict) -> dict:
    entries = plan["entries"]
    effect = collections.Counter()
    by_kind = collections.defaultdict(collections.Counter)
    for e in entries:
        effect[e["asset_id"]] += e["delta_mojos"]
        by_kind[e["kind"]][e["asset_id"]] += e["delta_mojos"]
    gap = engine_view_gap(plan["led"], plan["coins"], plan["oph"])
    print(f"{len(entries)} ledger rows; {len(plan['phantoms'])} takes to reverse; "
          f"{len(plan['missed'])} trades to book; {len(plan['taker_rows'])} taker_fills rows")
    for kind in sorted(by_kind):
        print(f"  {kind}: " + ", ".join(f"{a[:8]} {fmt(a, v)}" for a, v in by_kind[kind].items()))
    for asset, coins_left in plan["leftovers"].items():
        if coins_left:
            total = sum(a for _, a in coins_left)
            print(f"  not booked: {len(coins_left)} external {asset[:8]} coin(s) of unknown "
                  f"origin, {fmt(asset, total)} (at the options address, or dust)")
    print("CAT figures count the options address's coins too; XCH leaves them out.")
    print("asset      ledger-wallet now   repair effect     after")
    for asset in sorted(plan["coins"]):
        print(f"  {asset[:8]:8} {fmt(asset, gap[asset]):>18} {fmt(asset, effect[asset]):>16} "
              f"{fmt(asset, gap[asset] + effect[asset]):>12}")
    return {a: gap[a] + effect[a] for a in plan["coins"]}


# ---------------------------------------------------------------- apply/check
# The engine, and the GUI under every name it is installed as (gui/main.py).
WRITER_EXES = ("xop_trader.exe", "xop_trader_gui.exe", "xoptrader-gui.exe", "xoptrader_gui.exe")
# Command-line marks of a GUI run from source (python -m gui.main, gui\main.py,
# or the scripts\_run_gui.py launcher) and of the options runner.  Not
# "xop_trader": this script's own --db argument names the database.
WRITER_COMMANDS = ("gui.main", "gui\\main", "gui/main", "_run_gui", "gui.services.options")
# python.exe, pythonw.exe, python3.13.exe, py.exe, pyw.exe.
PYTHON_EXE = re.compile(r"^(pythonw?[0-9.]*|pyw?)\.exe$")


def writers_running(processes: list) -> list:
    """The engine, the GUI (installed, or run from source as python gui.main)
    and the options book's own runner, among (name, command line) pairs.  A
    Python process whose command line cannot be read could be either of the
    last two, so it counts too: fail closed, as the September repair does."""
    found = []
    for name, command in processes:
        exe = (name or "").lower()
        if exe in WRITER_EXES:
            found.append(exe)
        elif PYTHON_EXE.match(exe):
            if not (command or "").strip():
                found.append(f"{exe}: a Python process whose command line cannot be read")
            elif any(w in command.lower() for w in WRITER_COMMANDS):
                found.append(f"{exe}: {command}")
    return found


def require_stopped() -> None:
    """Refuse unless every writer of the database and the wallet is stopped.
    Fails closed: a process list that cannot be read or trusted refuses too."""
    if os.name != "nt":
        raise SystemExit("--apply runs on the trading PC (Windows) only")
    root = pathlib.Path(os.environ.get("SystemRoot", r"C:\Windows"))
    shell = root / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    query = ("[Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
             "Get-CimInstance Win32_Process | "
             "Select-Object ProcessId, ParentProcessId, Name, CommandLine | "
             "ConvertTo-Json -Compress")
    try:
        out = subprocess.run(  # noqa: S603
            [str(shell), "-NoProfile", "-NonInteractive", "-Command", query],
            capture_output=True, check=True, timeout=120).stdout
        rows = json.loads(out.decode("utf-8-sig", "replace"))
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        raise SystemExit(f"the process list could not be read ({exc}); not applying") from exc
    if isinstance(rows, dict):
        rows = [rows]
    if not isinstance(rows, list) or not rows:
        raise SystemExit("the process list came back empty; not applying")
    by_pid = {r.get("ProcessId"): r for r in rows if isinstance(r, dict)}
    if os.getpid() not in by_pid:
        raise SystemExit("the process list does not include this process; not applying")
    # This process, and the Python launchers above it: a venv's python.exe
    # starts the interpreter as its child, with the same command line.
    mine = {os.getpid()}
    parent = by_pid[os.getpid()].get("ParentProcessId")
    while (parent in by_pid and parent not in mine
           and PYTHON_EXE.match((by_pid[parent].get("Name") or "").lower())):
        mine.add(parent)
        parent = by_pid[parent].get("ParentProcessId")
    running = writers_running([(r.get("Name"), r.get("CommandLine"))
                               for pid, r in by_pid.items() if pid not in mine])
    if running:
        raise SystemExit("stop these first: " + "; ".join(running))


def backup(db_path: pathlib.Path) -> pathlib.Path:
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%d_%H%M%S")
    dest = db_path.parent / "backups" / f"xop_trader_pre_{TAG}_{stamp}.db"
    dest.parent.mkdir(parents=True, exist_ok=True)
    src = sqlite3.connect(f"file:{db_path.as_posix()}?mode=ro", uri=True)
    dst = sqlite3.connect(dest.as_posix())
    with dst:
        src.backup(dst)
    src.close()
    dst.close()
    return dest


def ledger_snapshot(con: sqlite3.Connection) -> tuple:
    """What the plan was built on: the ledger's and taker_fills' size and last id."""
    return (con.execute("SELECT COUNT(*), COALESCE(MAX(id), 0) FROM ledger_entries").fetchone()
            + con.execute("SELECT COUNT(*), COALESCE(MAX(id), 0) FROM taker_fills").fetchone())


def apply(db_path: pathlib.Path, plan: dict) -> None:
    require_stopped()
    dest = backup(db_path)
    print(f"backup: {dest}")
    now = utc_now()
    con = sqlite3.connect(db_path.as_posix(), isolation_level=None)
    try:
        con.execute("BEGIN IMMEDIATE")
        # Inside the write lock: the books must be the ones the plan read.
        if ledger_snapshot(con) != plan["snapshot"]:
            raise SystemExit("the ledger changed since the plan was made (an engine ran?) -- "
                             "nothing written; run the dry run again")
        for e in plan["entries"]:
            cur = con.execute(
                "INSERT OR IGNORE INTO ledger_entries (entry_time, event_type, event_id, leg, "
                "asset_id, delta_mojos, pair_name, block_height, note) VALUES (?,?,?,?,?,?,?,?,?)",
                (now, e["event_type"], e["event_id"], e["leg"], e["asset_id"], e["delta_mojos"],
                 e["pair_name"], e["block_height"], e["note"]))
            if cur.rowcount != 1:
                raise SystemExit(f"row not written (already there?): {e['event_id']} "
                                 f"{e['leg']} {e['asset_id']}")
        cols = [c[1] for c in con.execute("PRAGMA table_info(taker_fills)")]
        # A trade id as either spelling, "0x<id>" or "<id>", in any case: a
        # row written the other way must not stay behind in the history.
        spelled = "SELECT * FROM taker_fills WHERE lower(trade_id) IN (?, ?)"
        for eid, tid, status in plan["phantoms"]:
            rows = con.execute(spelled, ("0x" + tid, tid)).fetchall()
            for row in rows:
                archived = dict(zip(cols, row, strict=True))
                con.execute(
                    "INSERT INTO offer_closure_events (offer_id, pair_name, event_type, "
                    "previous_status, observed_status, closure_reason, resolved_block, fee_mojos) "
                    "VALUES (?,?,?,?,?,?,?,?)",
                    ("0x" + tid, archived.get("pair_name"), "phantom_take_correction", "taken",
                     status.lower(),
                     f"never settled; reversal:{eid}; archived taker_fills row: "
                     f"{json.dumps(archived)}; {TAG}",
                     archived.get("block_height"), archived.get("fee_mojos")))
            con.execute("DELETE FROM taker_fills WHERE lower(trade_id) IN (?, ?)",
                        ("0x" + tid, tid))
        for t in plan["taker_rows"]:
            bare = norm(t["trade_id"])
            if con.execute(spelled, ("0x" + bare, bare)).fetchall():
                raise SystemExit(f"taker_fills already holds {t['trade_id']} (in some spelling) "
                                 "-- nothing written; run the dry run again")
            cur = con.execute(TAKER_INSERT, tuple(t[c] for c in TAKER_COLUMNS))
            if cur.rowcount != 1:
                raise SystemExit(f"taker_fills row not written (already there?): {t['trade_id']}")
        con.execute("COMMIT")
    except BaseException:
        con.execute("ROLLBACK")
        raise
    finally:
        con.close()
    print(f"applied {len(plan['entries'])} ledger rows; archived and removed "
          f"{len(plan['phantoms'])} taker_fills rows; added {len(plan['taker_rows'])}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--db", required=True, type=pathlib.Path)
    ap.add_argument("--config", required=True, type=pathlib.Path)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--apply", action="store_true")
    mode.add_argument("--check", action="store_true")
    ap.add_argument("--plan-out", type=pathlib.Path, help="write the planned rows as JSON")
    args = ap.parse_args()

    cfg = yaml.safe_load(args.config.read_text(encoding="utf-8")) or {}
    address = (((cfg.get("options") or {}).get("wallet") or {}).get("address") or "").strip()
    if not address:
        raise SystemExit("options.wallet.address is not set in the config")
    wallet = Wallet()

    if args.check:
        db = sqlite3.connect(f"file:{args.db.as_posix()}?mode=ro", uri=True)
        led = db.execute(LEDGER_QUERY).fetchall()
        ids = wallet.wallet_ids()
        coins = {a: wallet.coins(ids[a]) for a in ids
                 if a == XCH or any(norm(r[5]) == a for r in led)}
        tagged = [r for r in led if TAG in (r[9] or "")]
        gap = engine_view_gap(led, coins, puzzle_hash_of(address))
        print(f"{len(tagged)} repair rows in the ledger")
        print("CAT figures count the options address's coins too; XCH leaves them out.")
        for asset in sorted(coins):
            print(f"  {asset[:8]:8} ledger-wallet {fmt(asset, gap[asset])}")
        return

    if args.apply:
        require_stopped()   # before planning: the plan is read with every writer stopped
    pairs = [(str(p.get("name")), norm(p.get("base_asset_id")), norm(p.get("quote_asset_id")))
             for p in (cfg.get("pairs") or [])]
    db = sqlite3.connect(f"file:{args.db.as_posix()}?mode=ro", uri=True)
    plan = build_plan(db, wallet, address, pairs)
    after = report(plan)
    db.close()
    xch_left = after.get(XCH, 0)
    print(f"measured network fees: {plan['measured_fee'] / MOJOS_PER_XCH:.6f} XCH; "
          f"XCH left after every item: {xch_left / MOJOS_PER_XCH:+.6f}")
    if abs(xch_left) > FEE_MATCH_TOLERANCE:
        print("WARNING: what is left of the XCH gap is outside the fee tolerance -- something "
              "unbooked is in flight (a fill since the engine stopped?) or an item is missing")
    if args.plan_out:
        rows = {"ledger_entries": plan["entries"], "taker_fills": plan["taker_rows"]}
        args.plan_out.write_text(json.dumps(rows, indent=1), encoding="utf-8")
    if args.apply:
        if abs(xch_left) > FEE_MATCH_TOLERANCE:
            raise SystemExit("refusing to apply: the XCH remainder is outside the fee tolerance")
        apply(args.db, plan)


if __name__ == "__main__":
    sys.exit(main())
