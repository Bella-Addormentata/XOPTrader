"""Nostr signature-collection client for the warp.green bridge.

warp.green validators do not expose a signing API. Instead each validator runs a
Nostr bot that, once it observes and attests to a bridge message, publishes a
kind-1 event whose *content* is its BLS signature over the message digest. To
claim a bridged deposit on Chia the bot must gather ``signature_threshold``
(6-of-10 on mainnet) of these signatures and aggregate them into the portal
spend.

Wire format (from docs.warp.green "Collecting Signatures"), all bech32m:

* ``r`` tag  -- routing key: ``source_chain(3) + destination_chain(3) + nonce(32)``
  concatenated then bech32m-encoded with HRP ``r``. Chia's chain id is ``xch``
  (``0x786368``) on both mainnet and testnet -- warp chain ids identify the chain
  *role*, not the instance, which is why the testnet reuses ``bse``/``xch``.
* ``c`` tag  -- the portal singleton *coin id* the signature is valid for,
  bech32m HRP ``c``. Validators re-sign when the portal advances, so pinning the
  current portal coin id here is the freshness mechanism: when the portal moves
  the previously collected signatures no longer match and must be re-collected.
* content    -- the 96-byte BLS G2 signature, bech32m HRP ``s``.

Security model: we never trust the Nostr layer. An event is accepted only if its
author pubkey maps to a known validator index *and* its signature BLS-verifies
against that validator's G1 key over the exact digest we computed locally (which
commits to the current portal coin id). A forged or replayed signature fails that
BLS check, so no secp256k1 / Nostr-event-signature verification is needed. A
wrong local digest simply collects nothing (visibly stuck), never something
wrong -- funds only ever move on a real, verified quorum.

Transport is injected (``fetcher``) so the state machine and its tests need no
network; the default fetcher speaks the Nostr relay protocol over
``websocket-client`` and is imported lazily.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

from . import clvm_utils as cu
from .constants import WarpNet

# --------------------------------------------------------------------------- #
# Protocol constants.
# --------------------------------------------------------------------------- #

SIG_EVENT_KIND = 1
HRP_ROUTING = "r"
HRP_COIN = "c"
HRP_SIG = "s"

# warp chain id for the Chia role; identical on mainnet and testnet (0x786368).
CHIA_CHAIN_ID = b"xch"

G2_SIG_LEN = 96  # a serialized BLS G2 signature
# 96 bytes bech32m-encode to ~162 chars, well over bech32m's default 90 cap.
_SIG_BECH32_MAX = 256

# (relay_url, subscription_filter, timeout_s) -> list of event dicts.
RelayFetcher = Callable[[str, dict, float], List[dict]]


class NostrError(RuntimeError):
    """A Nostr signature could not be decoded or a relay call failed."""


# --------------------------------------------------------------------------- #
# Small helpers.
# --------------------------------------------------------------------------- #

def _as_bytes(value: Any) -> bytes:
    """Coerce bytes or a (``0x``-optional) hex string to bytes."""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return bytes.fromhex(s)


# --------------------------------------------------------------------------- #
# bech32m tag / content codecs.
# --------------------------------------------------------------------------- #

def encode_routing_tag(
    source_chain: bytes, destination_chain: bytes, nonce: bytes
) -> str:
    """Build the ``r`` routing tag value for a message.

    ``source_chain`` and ``destination_chain`` are the 3-byte warp chain ids;
    ``nonce`` is the 32-byte on-chain message nonce (leading zeros preserved).
    """
    payload = bytes(source_chain) + bytes(destination_chain) + _as_bytes(nonce)
    return cu.bech32m_encode_bytes(HRP_ROUTING, payload)


def encode_coin_tag(coin_id: bytes) -> str:
    """Build the ``c`` tag value for a portal singleton coin id."""
    return cu.bech32m_encode_bytes(HRP_COIN, _as_bytes(coin_id))


def encode_sig_content(sig: bytes) -> str:
    """bech32m-encode a 96-byte BLS G2 signature as event content (HRP ``s``)."""
    sig = _as_bytes(sig)
    if len(sig) != G2_SIG_LEN:
        raise NostrError(f"signature is {len(sig)} bytes, expected {G2_SIG_LEN}")
    return cu.bech32m_encode_bytes(HRP_SIG, sig)


def decode_sig_content(content: str) -> bytes:
    """Decode an ``s``-prefixed bech32m event content to a 96-byte signature.

    Raises :class:`NostrError` on a wrong HRP, bad checksum, or wrong length --
    the caller treats that as "ignore this event", not a hard failure.
    """
    hrp, raw = cu.bech32m_decode_bytes(content.strip(), max_length=_SIG_BECH32_MAX)
    if raw is None or hrp != HRP_SIG:
        raise NostrError("content is not a valid 's'-prefixed bech32m signature")
    if len(raw) != G2_SIG_LEN:
        raise NostrError(f"signature is {len(raw)} bytes, expected {G2_SIG_LEN}")
    return raw


def routing_tag_for(
    net: WarpNet, nonce: bytes, *, destination_chain: bytes = CHIA_CHAIN_ID
) -> str:
    """The ``r`` tag for a deposit bridged from ``net.source_chain`` to Chia."""
    return encode_routing_tag(net.source_chain.encode(), destination_chain, nonce)


def coin_tag_for(portal_coin_id: bytes) -> str:
    """The ``c`` tag for the portal coin id the signatures must be valid for."""
    return encode_coin_tag(portal_coin_id)


# --------------------------------------------------------------------------- #
# Validator identity + BLS verification.
# --------------------------------------------------------------------------- #

def validator_index_for_pubkey(net: WarpNet, pubkey_hex: str) -> Optional[int]:
    """Map a Nostr author pubkey to its validator index, or ``None`` if unknown.

    The Nostr keys and BLS keys are index-aligned in :mod:`constants`, so the
    returned index selects the G1 key that must have produced the signature.
    """
    want = str(pubkey_hex).strip().lower()
    if not want:
        return None
    for i, key in enumerate(net.nostr_validator_keys):
        if key.lower() == want:
            return i
    return None


def verify_validator_sig(
    net: WarpNet, index: int, sig: bytes, digest: bytes
) -> bool:
    """BLS-verify ``sig`` against validator ``index``'s G1 key over ``digest``.

    ``chia_rs`` is imported lazily. Malformed signature/key bytes verify as
    ``False`` rather than raising, so a junk event is simply ignored.
    """
    from chia_rs import AugSchemeMPL, G1Element, G2Element

    if not 0 <= index < len(net.validator_bls_keys):
        return False
    try:
        g1 = G1Element.from_bytes(bytes.fromhex(net.validator_bls_keys[index]))
        g2 = G2Element.from_bytes(_as_bytes(sig))
    except Exception:  # noqa: BLE001 -- malformed point => not a valid signature
        return False
    try:
        return bool(AugSchemeMPL.verify(g1, bytes(digest), g2))
    except Exception:  # noqa: BLE001
        return False


# --------------------------------------------------------------------------- #
# Event parsing.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class ValidatorSig:
    """A validator signature that passed BLS verification."""

    index: int
    pubkey: str      # Nostr x-only author key (hex)
    sig: bytes       # 96-byte BLS G2 signature


def _first_tag(tags: Sequence, key: str) -> Optional[str]:
    """Return the value of the first ``[key, value, ...]`` tag, if present."""
    for tag in tags or ():
        if isinstance(tag, (list, tuple)) and len(tag) >= 2 and tag[0] == key:
            return tag[1]
    return None


def parse_signature_event(
    net: WarpNet,
    event: dict,
    *,
    digest: bytes,
    routing_tag: Optional[str] = None,
    coin_tag: Optional[str] = None,
) -> Optional[ValidatorSig]:
    """Validate one relay event, returning a verified :class:`ValidatorSig`.

    Returns ``None`` (ignore the event) unless every gate passes: kind-1, a known
    validator author, matching ``r``/``c`` tags when supplied, a well-formed
    96-byte signature, and a successful BLS verification over ``digest``.
    """
    if not isinstance(event, dict) or event.get("kind") != SIG_EVENT_KIND:
        return None
    index = validator_index_for_pubkey(net, event.get("pubkey", ""))
    if index is None:
        return None
    tags = event.get("tags") or []
    if routing_tag is not None and _first_tag(tags, HRP_ROUTING) != routing_tag:
        return None
    if coin_tag is not None and _first_tag(tags, HRP_COIN) != coin_tag:
        return None
    try:
        sig = decode_sig_content(str(event.get("content", "")))
    except NostrError:
        return None
    if not verify_validator_sig(net, index, sig, digest):
        return None
    return ValidatorSig(index=index, pubkey=str(event["pubkey"]).lower(), sig=sig)


def collect_from_events(
    net: WarpNet,
    events: Sequence[dict],
    *,
    digest: bytes,
    routing_tag: Optional[str] = None,
    coin_tag: Optional[str] = None,
    have: Optional[Dict[int, bytes]] = None,
) -> Dict[int, bytes]:
    """Fold verified signatures from ``events`` into ``{index: sig}``.

    Pure and side-effect-free (the unit-test seam). First verified signature per
    validator index wins; later duplicates are ignored.
    """
    collected: Dict[int, bytes] = dict(have or {})
    for event in events:
        vs = parse_signature_event(
            net, event, digest=digest, routing_tag=routing_tag, coin_tag=coin_tag
        )
        if vs is not None:
            collected.setdefault(vs.index, vs.sig)
    return collected


# --------------------------------------------------------------------------- #
# Collected-signature -> driver inputs.
# --------------------------------------------------------------------------- #

def sig_switches_for(
    net: WarpNet, collected: Dict[int, bytes]
) -> Tuple[List[bool], List[bytes]]:
    """Convert ``{index: sig}`` to the driver's ``(sig_switches, validator_sigs)``.

    ``sig_switches`` is a bool per curried validator (including the infinity pad
    key), and ``validator_sigs`` are the signatures for the set bits in ascending
    index order. BLS aggregation is commutative, so the order only has to agree
    with the switch bitmask, which it does by construction.
    """
    n = len(net.validator_bls_keys)
    switches = [False] * n
    sigs: List[bytes] = []
    for i in sorted(collected):
        if 0 <= i < n:
            switches[i] = True
            sigs.append(collected[i])
    return switches, sigs


# --------------------------------------------------------------------------- #
# Filter + result types.
# --------------------------------------------------------------------------- #

def build_filter(
    net: WarpNet,
    routing_tag: str,
    *,
    coin_tag: Optional[str] = None,
    authors: bool = True,
) -> dict:
    """Build the Nostr REQ filter for a message's signatures.

    Constrains to kind-1 events tagged with the message's ``r`` (and portal
    ``c``) key, optionally from the known validator authors. The BLS check is the
    real gate; the author constraint just trims relay noise.
    """
    filt: dict = {"kinds": [SIG_EVENT_KIND], "#" + HRP_ROUTING: [routing_tag]}
    if coin_tag is not None:
        filt["#" + HRP_COIN] = [coin_tag]
    if authors:
        filt["authors"] = list(net.nostr_validator_keys)
    return filt


@dataclass(frozen=True)
class SigResult:
    """Outcome of a collection pass."""

    collected: Dict[int, bytes]
    threshold: int

    @property
    def count(self) -> int:
        return len(self.collected)

    @property
    def complete(self) -> bool:
        return self.count >= self.threshold


# --------------------------------------------------------------------------- #
# Default relay transport (lazy websocket-client).
# --------------------------------------------------------------------------- #

def _default_fetcher() -> RelayFetcher:
    import json

    import websocket  # websocket-client; lazy so the GUI boots without it

    def fetch(relay_url: str, filt: dict, timeout: float) -> List[dict]:
        events: List[dict] = []
        ws = websocket.create_connection(relay_url, timeout=timeout)
        try:
            sub_id = "warp"
            ws.send(json.dumps(["REQ", sub_id, filt]))
            while True:
                try:
                    raw = ws.recv()
                except Exception:  # noqa: BLE001 -- timeout / closed => stop reading
                    break
                if not raw:
                    break
                try:
                    msg = json.loads(raw)
                except Exception:  # noqa: BLE001 -- skip non-JSON frames
                    continue
                if not isinstance(msg, list) or not msg:
                    continue
                kind = msg[0]
                if kind == "EVENT" and len(msg) >= 3 and isinstance(msg[2], dict):
                    events.append(msg[2])
                elif kind in ("EOSE", "CLOSED"):
                    break
            try:
                ws.send(json.dumps(["CLOSE", sub_id]))
            except Exception:  # noqa: BLE001
                pass
        finally:
            try:
                ws.close()
            except Exception:  # noqa: BLE001
                pass
        return events

    return fetch


class NostrSigCollector:
    """Collects validator signatures across relays until quorum or a deadline.

    Injected ``fetcher`` keeps the network out of the state machine and its
    tests. :meth:`collect` is resumable -- pass the previously persisted
    ``{index: sig}`` as ``have`` and it accumulates, so the service can persist
    after every tick and stop the moment ``signature_threshold`` is reached.
    """

    def __init__(
        self,
        net: WarpNet,
        *,
        fetcher: Optional[RelayFetcher] = None,
        per_relay_timeout: float = 6.0,
        clock: Optional[Callable[[], float]] = None,
    ) -> None:
        self._net = net
        self._fetcher = fetcher or _default_fetcher()
        self._per_relay_timeout = per_relay_timeout
        self._clock = clock

    def _now(self) -> float:
        if self._clock is not None:
            return self._clock()
        import time

        return time.monotonic()

    def collect(
        self,
        *,
        nonce: bytes,
        portal_coin_id: bytes,
        digest: bytes,
        have: Optional[Dict[int, bytes]] = None,
        deadline_s: float = 20.0,
        relay_offset: int = 0,
    ) -> SigResult:
        """Sweep relays (round-robin from ``relay_offset``) accumulating signatures.

        Stops early once quorum is reached or the wall-clock budget is spent; a
        relay that errors or returns nothing is skipped. Never raises on relay
        failure -- an empty pass just means "keep polling next tick".
        """
        net = self._net
        collected: Dict[int, bytes] = dict(have or {})
        routing_tag = routing_tag_for(net, nonce)
        coin_tag = coin_tag_for(portal_coin_id)
        filt = build_filter(net, routing_tag, coin_tag=coin_tag)
        threshold = net.signature_threshold

        relays = list(net.nostr_relays)
        if relays and relay_offset:
            offset = relay_offset % len(relays)
            relays = relays[offset:] + relays[:offset]

        start = self._now()
        for relay in relays:
            if len(collected) >= threshold:
                break
            elapsed = self._now() - start
            if elapsed >= deadline_s:
                break
            per = min(self._per_relay_timeout, max(1.0, deadline_s - elapsed))
            try:
                events = self._fetcher(relay, filt, per)
            except Exception:  # noqa: BLE001 -- a dead relay must not abort the sweep
                continue
            collected = collect_from_events(
                net, events, digest=digest, routing_tag=routing_tag,
                coin_tag=coin_tag, have=collected,
            )

        return SigResult(collected=collected, threshold=threshold)
