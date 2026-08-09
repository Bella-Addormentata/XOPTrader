"""Altruistic relay: deliver other people's stuck xch -> bse messages.

The warp.green protocol has no relayer network -- ``Portal.receiveMessage`` is
non-payable and whoever submits it pays the gas -- so a sender who holds only
Chia assets can burn but never deliver. Delivery, however, is trustless by
construction: the receiver is attested in the signed contents, the Portal
verifies exact validator signatures, and ``usedNonces`` prevents doubles. A
relayer can do nothing except pay a stranger's gas (measured live: one stuck
message carried 10-of-6 signatures on Nostr and needed only the ~145k-gas
broadcast).

Design rules, each carrying a lesson from the bridge work:

* **Grace period.** ~98% of messages are delivered by their own senders within
  minutes; racing them just burns gas on ``!nonce`` reverts. Only messages
  stuck longer than ``grace_s`` are touched.
* **Preflight is mandatory.** Messages target arbitrary contracts; a
  destination that always reverts is a money pit for a naive retry loop. Every
  candidate is simulated with a free ``eth_call`` first, and repeated failures
  land on a skip-list.
* **Budget cap.** A hard daily gas-spend ceiling, because altruism should not
  have an unbounded blast radius.
* **Watcher fields are verified, not trusted.** The digest is built from the
  watcher's message fields, but the validator signatures only ecrecover if
  those fields are what was actually signed -- a lying watcher produces a
  digest no signature matches, and the candidate is skipped, not paid for.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional

from .constants import WarpNet

_log = logging.getLogger(__name__)

#: Sweep no more than this many candidates per pass; the stuck tail is tiny
#: (~2% of ~25 messages/day) and a bounded pass keeps a bug's cost bounded.
MAX_CANDIDATES_PER_SWEEP = 5

#: Consecutive preflight/broadcast failures before a nonce is skipped forever.
SKIP_AFTER_FAILURES = 3


class RelayError(RuntimeError):
    """A relay attempt failed in a way the caller should see."""


@dataclass(frozen=True)
class StuckMessage:
    """One undelivered xch -> bse message, as reported by the watcher."""

    nonce: bytes
    source: bytes
    destination: str          # 0x-prefixed EVM address
    contents: List[bytes]
    age_s: float

    @classmethod
    def from_watcher(cls, msg: dict, *, now: Optional[float] = None) -> "StuckMessage":
        raw_contents = msg.get("contents")
        if isinstance(raw_contents, str):
            # The watcher may serialise the list; keep parsing strict and
            # local rather than eval'ing arbitrary text.
            import ast

            raw_contents = ast.literal_eval(raw_contents)
        try:
            ts = float(msg.get("source_timestamp") or 0)
        except (TypeError, ValueError):
            ts = 0.0
        now = time.time() if now is None else now
        return cls(
            nonce=bytes.fromhex(str(msg["nonce"])),
            source=bytes.fromhex(str(msg["source"])),
            destination="0x" + str(msg["destination"]).removeprefix("0x"),
            contents=[bytes.fromhex(str(c).removeprefix("0x")) for c in raw_contents],
            age_s=max(0.0, now - ts) if ts else float("inf"),
        )


def fetch_stuck_messages(
    fetcher: Callable[[str], list],
    *,
    grace_s: float,
    now: Optional[float] = None,
) -> List[StuckMessage]:
    """Undelivered xch messages older than the grace period, oldest first.

    ``fetcher`` takes a URL path+query and returns parsed JSON (injected so
    tests never touch the network). Delivered-detection is the watcher's
    ``destination_transaction_hash`` -- the field our own unwrap jobs poll.
    """
    raw = fetcher("/messages?source_chain=xch&limit=100") or []
    out: List[StuckMessage] = []
    for msg in raw:
        if msg.get("destination_transaction_hash"):
            continue
        if str(msg.get("status") or "") != "sent":
            continue
        try:
            m = StuckMessage.from_watcher(msg, now=now)
        except (KeyError, ValueError, TypeError, SyntaxError, OverflowError) as exc:
            # Hostile/garbled watcher rows must skip THIS message, not abort
            # discovery of every other stuck message. TypeError covers a
            # non-str field reaching bytes.fromhex/float; OverflowError a
            # pathological timestamp.
            _log.warning("relayer: unparseable watcher message skipped: %s", exc)
            continue
        if m.age_s >= grace_s:
            out.append(m)
    out.sort(key=lambda m: -m.age_s)
    return out


def build_relay_calldata(evm_client, collector, net: WarpNet, msg: StuckMessage) -> bytes:
    """Digest -> signatures -> packed calldata for one stuck message.

    Raises :class:`RelayError` when the signature quorum is not on Nostr --
    which means NO relayer can deliver it, not just us.
    """
    from . import evm, nostr

    separator = evm_client.verify_eip712_domain()
    digest = evm.validator_message_digest(
        separator, msg.nonce, b"xch", msg.source, msg.destination, msg.contents
    )
    threshold = int(evm_client.get_signature_threshold())
    result = collector.collect_ecdsa(
        nonce=msg.nonce, digest=digest, threshold=threshold, deadline_s=25.0
    )
    if not result.complete:
        raise RelayError(
            f"nonce {msg.nonce.hex()[:16]}...: {result.count}/{threshold} "
            "signatures on Nostr; not deliverable by anyone yet"
        )
    packed = evm.pack_validator_sigs(
        nostr.ecdsa_sigs_for_relay(result.collected, threshold)
    )
    return evm.encode_receive_message(
        msg.nonce, b"xch", msg.source, msg.destination, msg.contents, packed
    )


def preflight(evm_client, net: WarpNet, calldata: bytes) -> Optional[str]:
    """Simulate the relay; ``None`` when it would succeed, else the reason.

    Free, and load-bearing: it converts "pay to find out" into "know before
    paying" everywhere except the one-block race window.
    """
    from . import evm

    try:
        evm_client._eth_call(net.portal_address, calldata)
        return None
    except evm.EvmError as exc:
        reason = evm.decode_revert_reason(exc)
        if evm.is_already_delivered(exc):
            return "already delivered"
        return f"would revert: {reason}"


@dataclass
class AltruisticRelayer:
    """Opt-in sweeper that delivers the network's stranded-message tail.

    Deliberately NOT built on the WarpJob machine: jobs enforce one active
    row (right for our own money), while the sweeper handles N strangers'
    messages with no funds of ours at risk beyond capped gas.
    """

    net: WarpNet
    evm_client: Any
    collector: Any
    watcher_fetch: Callable[[str], list]
    evm_key: Any                      # keystore.EvmKey -- ideally gas-only
    grace_s: float = 1800.0           # 30 min: never race the owner
    daily_gas_budget_wei: int = 200_000_000_000_000   # 0.0002 ETH/day
    clock: Callable[[], float] = time.time
    #: Optional mutable dict owned by a longer-lived caller (the Qt worker),
    #: so the daily-budget ledger, failure counts, and skip-list SURVIVE an
    #: engine rebuild on config reload -- otherwise the documented hard daily
    #: ceiling would silently reset every reload and the skip-list would
    #: forget its poison messages.
    persistent: Optional[Dict[str, Any]] = None

    _failures: Dict[str, int] = field(default_factory=dict)
    _skiplist: set = field(default_factory=set)
    _spent_today_wei: int = 0
    _budget_day: Optional[int] = None

    def __post_init__(self) -> None:
        if self.persistent is not None:
            # Adopt the shared containers by reference (mutations persist) and
            # load the scalar budget state.
            self._failures = self.persistent.setdefault("failures", {})
            self._skiplist = self.persistent.setdefault("skiplist", set())
            budget = self.persistent.setdefault("budget", {})
            self._budget_day = budget.get("day")
            self._spent_today_wei = int(budget.get("spent", 0))

    def _save_budget(self) -> None:
        if self.persistent is not None:
            self.persistent["budget"] = {
                "day": self._budget_day, "spent": self._spent_today_wei,
            }

    def _budget_remaining(self) -> int:
        day = int(self.clock() // 86400)
        if day != self._budget_day:
            self._budget_day, self._spent_today_wei = day, 0
            self._save_budget()
        return self.daily_gas_budget_wei - self._spent_today_wei

    def _note_failure(self, key: str) -> None:
        self._failures[key] = self._failures.get(key, 0) + 1
        if self._failures[key] >= SKIP_AFTER_FAILURES:
            self._skiplist.add(key)

    def sweep(self, *, broadcast: bool = True) -> List[dict]:
        """One pass; returns a report entry per candidate considered.

        ``broadcast=False`` runs everything except the send -- the same
        rehearsal cut the bridge uses. One malformed/hostile candidate can
        never abort the pass: each is processed inside its own guard.
        """
        reports: List[dict] = []
        fetched = fetch_stuck_messages(
            self.watcher_fetch, grace_s=self.grace_s, now=self.clock()
        )
        actioned = 0
        for msg in fetched:
            key = msg.nonce.hex()
            age = int(msg.age_s) if msg.age_s != float("inf") else None
            entry: dict = {"nonce": key, "destination": msg.destination,
                           "age_s": age, "action": None}
            # Skip-listed messages are reported but never consume an action
            # slot: a handful of permanently-stuck ones at the front of the
            # (oldest-first) list must not occlude fresh, deliverable ones.
            if key in self._skiplist:
                entry["action"] = "skiplisted"
                reports.append(entry)
                continue
            if actioned >= MAX_CANDIDATES_PER_SWEEP:
                break
            reports.append(entry)
            actioned += 1
            try:
                self._consider(msg, entry, broadcast=broadcast)
            except Exception as exc:  # noqa: BLE001 -- one bad message, not the pass
                entry["action"] = f"error: {exc}"
                _log.warning("relayer: candidate %s failed: %s", key[:16], exc)
        return reports

    def _consider(self, msg: "StuckMessage", entry: dict, *, broadcast: bool) -> None:
        """Process one non-skiplisted candidate, mutating ``entry['action']``."""
        from . import evm

        key = msg.nonce.hex()
        if self._budget_remaining() <= 0:
            entry["action"] = "budget exhausted"
            return
        try:
            calldata = build_relay_calldata(
                self.evm_client, self.collector, self.net, msg
            )
        except RelayError as exc:
            entry["action"] = f"skipped: {exc}"
            return

        reason = preflight(self.evm_client, self.net, calldata)
        if reason is not None:
            entry["action"] = f"preflight: {reason}"
            if reason != "already delivered":
                self._note_failure(key)
            return

        # prepare_relay re-estimates gas, which itself can revert '!nonce' if
        # the message was delivered between preflight and here -- that is a
        # lost race, not our failure, and must not poison the skip-list.
        try:
            unsigned = self.evm_client.prepare_relay(
                owner=self.evm_key.address, calldata=calldata
            )
        except evm.EvmError as exc:
            if evm.is_already_delivered(exc):
                entry["action"] = "raced: delivered by someone else"
                return
            raise
        max_cost = unsigned.gas * unsigned.max_fee_per_gas
        if max_cost > self._budget_remaining():
            entry["action"] = "budget exhausted"
            return
        signed = evm.sign_tx(unsigned, self.evm_key.private_key)
        entry["tx_hash"] = signed.tx_hash
        if not broadcast:
            entry["action"] = "signed, not broadcast (rehearsal)"
            return
        try:
            self.evm_client.send_raw_transaction(signed.raw)
        except evm.EvmError as exc:
            if evm.is_already_delivered(exc):
                entry["action"] = "raced: delivered by someone else"
                return
            entry["action"] = f"broadcast failed: {evm.decode_revert_reason(exc)}"
            self._note_failure(key)
            return
        self._spent_today_wei += max_cost
        self._save_budget()
        entry["action"] = "relayed"
        _log.info("relayer: delivered stranger's message %s via %s",
                  key[:16], signed.tx_hash)


def recent_third_party_relays(
    watcher_fetch: Callable[[str], list],
    evm_client,
    *,
    portal_address: Optional[str] = None,
    grace_s: float = 1800.0,
    lookback: int = 100,
) -> List[dict]:
    """Altruism evidence derived from the chain, verified not merely believed.

    A message that sat stuck past the grace period and was then delivered by
    an address that is not its attested receiver was relayed by a volunteer.
    To be evidence rather than a watcher's say-so, EACH claim is checked
    against the chain before it counts:

    * the delivery tx must actually be a call to the Portal (``tx.to`` ==
      ``net.portal_address``) -- otherwise the watcher could point at any
      unrelated Base tx from any address to manufacture a fake volunteer;
    * the attested receiver must be determinable and must NOT be the sender.
      When contents are malformed/serialised such that the receiver cannot be
      read, the row is dropped (fail closed), never counted as altruism.

    This backs the GUI's "proven volunteer online" badge, which an operator
    uses to decide a gasless Chia-only burn is safe -- so an unverifiable
    row must not inflate it.
    """
    if portal_address is None:
        net = getattr(evm_client, "net", None)
        portal_address = getattr(net, "portal_address", "") or ""
    portal = str(portal_address).lower()
    raw = watcher_fetch(f"/messages?source_chain=xch&limit={lookback}") or []
    out: List[dict] = []
    for msg in raw:
        tx_hash = msg.get("destination_transaction_hash")
        if not tx_hash:
            continue
        try:
            src_ts = float(msg.get("source_timestamp") or 0)
            dst_ts = float(msg.get("destination_timestamp") or 0)
        except (TypeError, ValueError):
            continue
        if not src_ts or not dst_ts or (dst_ts - src_ts) < grace_s:
            continue          # delivered promptly -> almost surely the owner
        try:
            tx = evm_client._call("eth_getTransactionByHash", [str(tx_hash)])
            sender = str((tx or {}).get("from") or "").lower()
            to = str((tx or {}).get("to") or "").lower()
        except Exception:  # noqa: BLE001 -- evidence gathering must not raise
            continue
        if not sender or not to:
            continue
        # The delivery must be a Portal call; anything else is not a relay of
        # this message (fail closed if we cannot confirm the Portal address).
        if not portal or to != portal:
            continue
        receiver = ""
        contents = msg.get("contents")
        if isinstance(contents, list) and len(contents) >= 2 and contents[1]:
            receiver = str(contents[1])[-40:].lower()
        if not receiver:
            continue          # receiver unreadable -> cannot prove third-party
        if sender.removeprefix("0x").endswith(receiver):
            continue          # the receiver delivered their own -> not altruism
        out.append({
            "nonce": str(msg.get("nonce")),
            "relayer": sender,
            "stuck_for_s": int(dst_ts - src_ts),
            "delivered_at": dst_ts,
        })
    return out
