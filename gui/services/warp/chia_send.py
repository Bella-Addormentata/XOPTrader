"""[WARP-LOST-SEND 2026-10-25] Will a Chia send warp made still land?

Job 4 on 2026-10-24: the claim's funding send (0.0001 XCH, fee 0) spent a
coin the trading engine's next offer also used.  The offer's spend landed at
block 9477776, and 53 seconds after the send the engine's stuck-transaction
cleanup deleted every unconfirmed transaction in the XCH wallet, the warp
send among them.  The job then waited two hours for a coin that could never
appear, failed, and Retry had nothing to do: the recorded tx id said
"already sent".

A recorded send is therefore JUDGED on every tick, from evidence the two
parties that matter give independently:

* the WALLET: does it still hold the transaction (it rebroadcasts what it
  holds), and does it call it confirmed?
* the CHAIN (coinset): is a coin the send spent already spent by something
  else (then this send can never confirm), did the expected output land
  (then it did confirm), and is the bundle still in the mempool?

Verdicts, most certain first:

``LANDED``   the output exists on chain (spent or not);
``DEAD``     an input is spent, at the network's confirmation depth, and
             the chain was asked and showed no output of ours: nothing can
             ever include this send;
``SHALLOW``  an input is spent, but too recently to rule out a reorg that
             puts it back: wait;
``ALIVE``    the wallet still holds it, or the mempool still has it;
``ORPHAN``   nowhere, with its inputs unspent: it MAY still surface, so a
             caller waits a bounded number of polls before treating it as
             dead;
``UNKNOWN``  a probe failed; wait.

Pure functions only.  The probes are injected, so every rule is testable
offline, and this module imports no chia library.
"""

from __future__ import annotations

import hashlib
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, Iterable, Optional, Sequence


class Verdict(str, Enum):
    LANDED = "landed"
    DEAD = "dead"
    SHALLOW = "shallow"
    ALIVE = "alive"
    ORPHAN = "orphan"
    UNKNOWN = "unknown"


@dataclass(frozen=True)
class SendEvidence:
    """What the probes said.  ``None`` means "could not tell"."""

    wallet_known: Optional[bool]        # the wallet still holds the record
    wallet_confirmed: bool              # ...and calls it confirmed
    removals: tuple                     # coin ids (hex, no 0x) it spent; () unknown
    removal_spent: Optional[bool]       # any input spent on chain, at depth
    removal_spent_depth: int            # confirmations of the deepest such spend
    output_landed: Optional[bool]       # the expected output exists on chain
    in_mempool: Optional[bool]          # coinset's mempool has the bundle
    min_depth: int = 1                  # the depth a spend needs to count


def _hx(value: Any) -> str:
    text = str(value or "").strip().lower()
    return text[2:] if text.startswith("0x") else text


def _int_to_bytes(value: int) -> bytes:
    """CLVM's minimal signed big-endian encoding (0 -> b""), as
    clvm_utils.int_to_bytes does it, repeated here so this module needs no
    chia library."""
    if value == 0:
        return b""
    return value.to_bytes((value.bit_length() + 8) >> 3, "big", signed=True)


def coin_id(parent_hex: str, puzzle_hash_hex: str, amount: int) -> str:
    """sha256(parent . puzzle_hash . int_to_bytes(amount)), hex."""
    return hashlib.sha256(
        bytes.fromhex(_hx(parent_hex)) + bytes.fromhex(_hx(puzzle_hash_hex))
        + _int_to_bytes(int(amount))
    ).hexdigest()


def removal_ids(record: Optional[dict]) -> tuple:
    """Coin ids a wallet transaction record (or mempool item) spent."""
    out = []
    for coin in (record or {}).get("removals") or []:
        if not isinstance(coin, dict):
            continue
        try:
            out.append(coin_id(coin.get("parent_coin_info"),
                               coin.get("puzzle_hash"), int(coin.get("amount"))))
        except (TypeError, ValueError):
            continue
    return tuple(out)


def gather(
    *,
    wallet_record: Callable[[], Optional[dict]],
    mempool_item: Callable[[], Optional[dict]],
    coin_spent: Callable[[str], Optional[int]],
    outputs_at: Callable[[], Iterable[Any]],
    known_removals: Sequence[str],
    out_amount: int,
    unique_output: bool,
    min_depth: int = 1,
) -> SendEvidence:
    """Run the probes, each tolerated to fail on its own.

    ``coin_spent(coin_id)`` answers the spend's confirmations (0 = unspent),
    or None when the coin is not indexed or the depth cannot be told.  A
    spend counts as proof only from ``min_depth`` confirmations (the
    network's confirmation threshold): a shallower one may yet be reorged
    out, which would make the judged send valid again -- and a cat_spend
    sent again on that proof would then burn twice.  ``outputs_at()`` lists
    coin records (``.coin.amount``, ``.coin.parent_coin_info``) at the
    expected output puzzle hash, spent ones included.  With
    ``unique_output`` the puzzle hash belongs to this job alone (an
    ephemeral key), so any coin of the right amount there is ours;
    otherwise the output must descend from one of our inputs.
    """
    wallet_known: Optional[bool] = None
    wallet_confirmed = False
    removals = tuple(_hx(r) for r in known_removals if r)
    try:
        rec = wallet_record()
        wallet_known = rec is not None
        if rec is not None:
            wallet_confirmed = bool(rec.get("confirmed"))
            if not removals:
                removals = removal_ids(rec)
    except Exception:  # noqa: BLE001 -- a failed probe is "unknown", not "no"
        wallet_known = None

    in_mempool: Optional[bool] = None
    try:
        item = mempool_item()
        in_mempool = item is not None
        if item is not None and not removals:
            removals = removal_ids(item)
    except Exception:  # noqa: BLE001
        in_mempool = None

    removal_spent: Optional[bool] = None
    deepest = 0
    if removals:
        seen_all = True
        for rid in removals:
            try:
                depth = coin_spent(rid)
            except Exception:  # noqa: BLE001
                depth = None
            if depth is None:
                seen_all = False
                continue
            depth = int(depth)
            if depth > deepest:
                deepest = depth
            if depth >= max(1, int(min_depth)):
                removal_spent = True
        if removal_spent is None and seen_all:
            removal_spent = False

    output_landed: Optional[bool] = None
    try:
        output_landed = False
        for rec in outputs_at():
            coin = getattr(rec, "coin", None)
            if coin is None or int(getattr(coin, "amount", -1)) != int(out_amount):
                continue
            if unique_output or _hx(getattr(coin, "parent_coin_info", "")) in removals:
                output_landed = True
                break
    except Exception:  # noqa: BLE001
        output_landed = None

    return SendEvidence(
        wallet_known=wallet_known, wallet_confirmed=wallet_confirmed,
        removals=removals, removal_spent=removal_spent,
        removal_spent_depth=deepest, output_landed=output_landed,
        in_mempool=in_mempool, min_depth=max(1, int(min_depth)),
    )


def judge(ev: SendEvidence) -> tuple:
    """``(Verdict, reason)`` for one recorded send.

    DEAD is the one verdict a caller acts on irreversibly (a cat_spend is
    sent again), so it needs the chain to have ANSWERED that no output of
    ours exists: a spent input with the output probe unanswered is UNKNOWN,
    and a wallet that calls the send confirmed is believed over a spent
    input (the input was spent by this very send).
    """
    if ev.output_landed:
        return Verdict.LANDED, "its output is on chain"
    if ev.wallet_known and ev.wallet_confirmed:
        return Verdict.LANDED, "the wallet calls it confirmed; awaiting the index"
    if ev.removal_spent:
        if ev.output_landed is False:
            return Verdict.DEAD, (
                "a coin it spent was spent by another transaction "
                f"({ev.removal_spent_depth} confirmations ago), so it can "
                "never confirm")
        return Verdict.UNKNOWN, (
            "a coin it spent is spent, but the chain did not answer whether "
            "our output exists")
    if ev.removal_spent_depth > 0:
        # Spent, but not yet past the confirmation threshold: a reorg could
        # still return the coin, and with it this send's validity.
        return Verdict.SHALLOW, (
            "a coin it spent was spent by another transaction "
            f"{ev.removal_spent_depth}/{ev.min_depth} confirmations ago; "
            "waiting for depth before treating it as dead")
    if ev.wallet_known:
        return Verdict.ALIVE, "the wallet still holds it unconfirmed"
    if ev.in_mempool:
        return Verdict.ALIVE, "the wallet no longer holds it, but the mempool does"
    if ev.wallet_known is False and ev.in_mempool is False:
        if ev.removal_spent is False:
            return Verdict.ORPHAN, (
                "gone from the wallet and the mempool while the coins it "
                "spent are still unspent")
        if not ev.removals:
            return Verdict.ORPHAN, (
                "gone from the wallet and the mempool, and the coins it "
                "spent are not known")
    return Verdict.UNKNOWN, "a probe did not answer"
