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


# --------------------------------------------------------------------------- #
# Outbound (unwrap): ECDSA signatures over the EIP-712 digest.
#
# Same relays, same event shape, same HRP-``s`` content encoding -- but the
# payload is a 65-byte v||r||s secp256k1 signature, the author check is an
# ecrecover against ``evm_validator_addresses``, and there is no ``c`` tag
# gate: it is present but EMPTY for EVM destinations, so filtering on it
# would drop every event. Proven by fetching all ten validators' events for
# a real unwrap with this module's own fetcher.
# --------------------------------------------------------------------------- #

ECDSA_SIG_LEN = 65


def outbound_routing_tag(nonce: bytes) -> str:
    """The ``r`` tag for an unwrap: xch -> bse, nonce = the bridging coin id.

    Deliberately NOT :func:`routing_tag_for`, which hardcodes
    ``net.source_chain`` (the EVM chain) as the source and is inbound-only.
    """
    return encode_routing_tag(b"xch", b"bse", nonce)


def decode_ecdsa_sig_content(content: str) -> bytes:
    """Decode an ``s``-prefixed bech32m content to a 65-byte v||r||s signature."""
    hrp, raw = cu.bech32m_decode_bytes(content.strip(), max_length=_SIG_BECH32_MAX)
    if raw is None or hrp != HRP_SIG:
        raise NostrError("content is not a valid 's'-prefixed bech32m signature")
    if len(raw) != ECDSA_SIG_LEN:
        raise NostrError(f"signature is {len(raw)} bytes, expected {ECDSA_SIG_LEN}")
    return raw


def recover_evm_signer(net: WarpNet, sig: bytes, digest: bytes) -> Optional[str]:
    """The checksummed validator address a v||r||s signature recovers to.

    ``None`` unless the recovered address is one of
    ``net.evm_validator_addresses`` -- an unknown signer is an ignored event,
    not an error. Malformed signatures return ``None`` rather than raising.
    """
    try:
        from eth_keys import keys

        v, r, s = sig[0], sig[1:33], sig[33:65]
        if v not in (27, 28):
            return None
        signature = keys.Signature(
            vrs=(v - 27, int.from_bytes(r, "big"), int.from_bytes(s, "big"))
        )
        addr = signature.recover_public_key_from_msg_hash(bytes(digest)).to_address()
    except Exception:  # noqa: BLE001 -- malformed => not a valid signature
        return None
    for known in net.evm_validator_addresses:
        if known.lower() == addr.lower():
            return known
    return None


def collect_ecdsa_from_events(
    net: WarpNet,
    events: Sequence[dict],
    *,
    digest: bytes,
    routing_tag: str,
    have: Optional[Dict[str, bytes]] = None,
) -> Dict[str, bytes]:
    """Fold verified outbound signatures into ``{checksummed_address: sig65}``.

    Pure, like :func:`collect_from_events`. Keyed by recovered address rather
    than validator index: the relay orders by address, and unlike the BLS
    path nothing downstream needs the index. First signature per address
    wins. No ``c``-tag gate, by design.
    """
    collected: Dict[str, bytes] = dict(have or {})
    for event in events:
        if not isinstance(event, dict) or event.get("kind") != SIG_EVENT_KIND:
            continue
        if validator_index_for_pubkey(net, event.get("pubkey", "")) is None:
            continue
        if _first_tag(event.get("tags") or [], HRP_ROUTING) != routing_tag:
            continue
        try:
            sig = decode_ecdsa_sig_content(str(event.get("content", "")))
        except NostrError:
            continue
        addr = recover_evm_signer(net, sig, digest)
        if addr is not None:
            collected.setdefault(addr, sig)
    return collected


def ecdsa_sigs_for_relay(collected: Dict[str, bytes], threshold: int) -> list:
    """``(address, v, r, s)`` tuples for exactly ``threshold`` signatures.

    Takes the ``threshold`` lowest addresses -- any subset verifies, and a
    deterministic choice keeps a resumed job's calldata identical. Raises
    while short, which the caller treats as still-collecting.
    """
    if len(collected) < threshold:
        raise NostrError(
            f"have {len(collected)} of {threshold} validator signatures"
        )
    chosen = sorted(collected.items(), key=lambda kv: int(kv[0], 16))[:threshold]
    return [
        (addr, sig[0], sig[1:33], sig[33:65])
        for addr, sig in chosen
    ]


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
class EcdsaSigResult:
    """Outcome of an outbound (ECDSA) collection pass, keyed by address."""

    collected: Dict[str, bytes]
    threshold: int

    @property
    def count(self) -> int:
        return len(self.collected)

    @property
    def complete(self) -> bool:
        return len(self.collected) >= self.threshold


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


# --------------------------------------------------------------------------- #
# Altruistic-relay heartbeat: BIP340-signed NIP-01 events.
#
# The "online right now" half of the liveness signal. The on-chain half
# (relayer.recent_third_party_relays) is unforgeable but lagging; a heartbeat
# is fresh but only a *claim*. The two compose: a heartbeat whose relayer
# address also appears in the on-chain third-party evidence is a proven
# volunteer saying it is still running. Spoofing someone else's address gains
# nothing -- claiming an address does not make the claimant relay.
#
# Every consumed event is fully verified (id recomputed, BIP340 signature
# checked, timestamp bounded) -- the same never-trust-the-relay rule as the
# signature collectors above.
# --------------------------------------------------------------------------- #

HEARTBEAT_KIND = 1
HEARTBEAT_TAG = "warp-altruistic-relay-v1"
_HEARTBEAT_KEY_DOMAIN = b"xoptrader-nostr-heartbeat-v1"
#: Domain tag for the EVM-key binding signature (see heartbeat_binding_digest).
_HEARTBEAT_BINDING_DOMAIN = b"xoptrader-heartbeat-binding-v1"
#: Reject events stamped further into the future than honest clock skew allows;
#: without this a spoofer could post-date one event and stay "online" forever.
_HEARTBEAT_MAX_FUTURE_SKEW_S = 300.0

# (relay_url, signed_event, timeout_s) -> accepted (the relay's OK verdict).
RelayPublisher = Callable[[str, dict, float], bool]


def heartbeat_keypair(evm_private_key: bytes) -> Tuple[bytes, str]:
    """Deterministic Nostr identity derived from -- never equal to -- the relay key.

    ``sha256(domain || key || counter)``: stable across restarts (consumers can
    recognise a repeat volunteer by npub) without ever signing Nostr events
    with a funds-holding secp256k1 scalar. Returns ``(seckey32, pubkey_hex)``.
    """
    import hashlib

    from . import schnorr

    counter = 0
    while True:
        candidate = hashlib.sha256(
            _HEARTBEAT_KEY_DOMAIN + bytes(evm_private_key)
            + counter.to_bytes(4, "big")
        ).digest()
        if 1 <= int.from_bytes(candidate, "big") <= schnorr.N - 1:
            return candidate, schnorr.pubkey_gen(candidate).hex()
        counter += 1


def nip01_event_id(
    pubkey_hex: str, created_at: int, kind: int, tags: list, content: str
) -> str:
    """The NIP-01 event id: sha256 of the canonical serialization array."""
    import hashlib
    import json

    payload = json.dumps(
        [0, pubkey_hex, int(created_at), int(kind), tags, content],
        separators=(",", ":"),
        ensure_ascii=False,
    )
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def heartbeat_binding_digest(relayer_address: str, nostr_pubkey_hex: str) -> bytes:
    """The digest the relayer's EVM key signs to bind its Nostr identity.

    Without this binding a heartbeat proved only that SOME Nostr key signed
    the content -- any attacker could mint a fresh Nostr key, name a
    previously-proven EVM address in the content, and light up
    ``online_proven``. The binding signature (recoverable ECDSA over this
    digest) proves the holder of the claimed EVM key authorised exactly this
    Nostr identity; freshness still comes from the BIP340-signed
    ``created_at``, so replaying an old binding gains nothing.
    """
    import hashlib

    return hashlib.sha256(
        _HEARTBEAT_BINDING_DOMAIN
        + str(relayer_address).lower().encode()
        + bytes.fromhex(nostr_pubkey_hex)
    ).digest()


def _sign_binding(evm_private_key: bytes, digest: bytes) -> str:
    """r||s||v (65 bytes, v in {0,1}) hex over the binding digest."""
    from eth_keys import keys

    sig = keys.PrivateKey(bytes(evm_private_key)).sign_msg_hash(digest)
    return sig.to_bytes().hex()


def _recover_binding(evm_sig_hex: str, digest: bytes) -> Optional[str]:
    """The lowercase address a binding signature recovers to, or ``None``."""
    try:
        from eth_keys import keys

        raw = bytes.fromhex(str(evm_sig_hex))
        if len(raw) != 65:
            return None
        sig = keys.Signature(signature_bytes=raw)
        return sig.recover_public_key_from_msg_hash(digest).to_address().lower()
    except Exception:  # noqa: BLE001 -- malformed => not a valid binding
        return None


def build_heartbeat_event(
    net: WarpNet,
    evm_private_key: bytes,
    relayer_address: str,
    *,
    created_at: float,
    aux_rand: Optional[bytes] = None,
) -> dict:
    """A complete, signed heartbeat event ready for ``["EVENT", ...]``.

    ``relayer_address`` is the EVM address the volunteer relays from -- the
    join key against the on-chain altruism evidence. The content carries an
    EVM-key binding signature over the Nostr identity, so the claim is
    verifiable, not merely stated (see :func:`heartbeat_binding_digest`).
    """
    import json
    import os

    from . import schnorr

    seckey, pubkey_hex = heartbeat_keypair(evm_private_key)
    binding = _sign_binding(
        evm_private_key,
        heartbeat_binding_digest(relayer_address, pubkey_hex),
    )
    content = json.dumps(
        {
            "evm_sig": binding,
            "net": net.name,
            "relayer": str(relayer_address).lower(),
            "v": 2,
        },
        separators=(",", ":"),
        sort_keys=True,
    )
    tags = [["t", HEARTBEAT_TAG]]
    created = int(created_at)
    event_id = nip01_event_id(pubkey_hex, created, HEARTBEAT_KIND, tags, content)
    sig = schnorr.sign(
        bytes.fromhex(event_id), seckey, aux_rand if aux_rand is not None else os.urandom(32)
    )
    return {
        "id": event_id,
        "pubkey": pubkey_hex,
        "created_at": created,
        "kind": HEARTBEAT_KIND,
        "tags": tags,
        "content": content,
        "sig": sig.hex(),
    }


def verify_heartbeat_event(
    net: WarpNet, event: dict, *, now: float, max_age_s: float
) -> Optional[dict]:
    """Full verification of one heartbeat; ``None`` means ignore the event.

    Gates, in order: shape, kind, tag, recomputed id, BIP340 signature over
    the id, bounded timestamp (neither stale nor post-dated), parseable
    content for this network with an EVM-shaped relayer address.
    """
    import json

    from . import schnorr

    if not isinstance(event, dict) or event.get("kind") != HEARTBEAT_KIND:
        return None
    if _first_tag(event.get("tags") or [], "t") != HEARTBEAT_TAG:
        return None
    try:
        pubkey = str(event["pubkey"])
        created = int(event["created_at"])
        content = str(event["content"])
        want_id = nip01_event_id(
            pubkey, created, HEARTBEAT_KIND, event.get("tags") or [], content
        )
        if want_id != str(event.get("id", "")).lower():
            return None
        if not schnorr.verify(
            bytes.fromhex(want_id), bytes.fromhex(pubkey),
            bytes.fromhex(str(event["sig"])),
        ):
            return None
    except (KeyError, ValueError, TypeError):
        return None
    if created < now - max_age_s or created > now + _HEARTBEAT_MAX_FUTURE_SKEW_S:
        return None
    try:
        body = json.loads(content)
    except ValueError:
        return None
    if not isinstance(body, dict) or body.get("net") != net.name:
        return None
    relayer = str(body.get("relayer") or "").lower()
    if not (relayer.startswith("0x") and len(relayer) == 42):
        return None
    try:
        bytes.fromhex(relayer[2:])
    except ValueError:
        return None
    # The EVM-key binding: ``bound`` is True only when the content's evm_sig
    # recovers to exactly the claimed relayer address over this event's Nostr
    # identity. An unbound event remains a liveness CLAIM (online_now), but
    # must never feed online_proven -- otherwise anyone could mint a Nostr
    # key and name someone else's proven address.
    bound = False
    evm_sig = body.get("evm_sig")
    if evm_sig:
        recovered = _recover_binding(
            str(evm_sig), heartbeat_binding_digest(relayer, pubkey.lower())
        )
        bound = recovered == relayer
    return {
        "relayer": relayer,
        "pubkey": pubkey.lower(),
        "seen_at": created,
        "bound": bound,
    }


def heartbeat_filter(*, since: float) -> dict:
    """The REQ filter consumers use to find recent heartbeats."""
    return {
        "kinds": [HEARTBEAT_KIND],
        "#t": [HEARTBEAT_TAG],
        "since": int(since),
    }


def _default_publisher() -> RelayPublisher:
    import json

    import websocket  # websocket-client; lazy so the GUI boots without it

    def publish(relay_url: str, event: dict, timeout: float) -> bool:
        ws = websocket.create_connection(relay_url, timeout=timeout)
        try:
            ws.send(json.dumps(["EVENT", event]))
            while True:
                try:
                    raw = ws.recv()
                except Exception:  # noqa: BLE001 -- timeout/closed => no verdict
                    return False
                if not raw:
                    return False
                try:
                    msg = json.loads(raw)
                except Exception:  # noqa: BLE001 -- skip non-JSON frames
                    continue
                if (
                    isinstance(msg, list) and len(msg) >= 3
                    and msg[0] == "OK" and msg[1] == event.get("id")
                ):
                    return bool(msg[2])
        finally:
            try:
                ws.close()
            except Exception:  # noqa: BLE001
                pass

    return publish


def publish_heartbeat(
    net: WarpNet,
    evm_private_key: bytes,
    relayer_address: str,
    *,
    created_at: float,
    publisher: Optional[RelayPublisher] = None,
    per_relay_timeout: float = 6.0,
) -> int:
    """Publish one heartbeat to every configured relay; count acceptances.

    Best-effort by contract: a dead relay is skipped, and the function never
    raises -- liveness advertising must not be able to break the sweep loop
    that feeds it.
    """
    event = build_heartbeat_event(
        net, evm_private_key, relayer_address, created_at=created_at
    )
    publish = publisher or _default_publisher()
    accepted = 0
    for relay in net.nostr_relays:
        try:
            if publish(relay, event, per_relay_timeout):
                accepted += 1
        except Exception:  # noqa: BLE001 -- a dead relay must not abort the pass
            continue
    return accepted


def fetch_recent_heartbeats(
    net: WarpNet,
    *,
    now: float,
    max_age_s: float = 900.0,
    fetcher: Optional[RelayFetcher] = None,
    per_relay_timeout: float = 6.0,
) -> List[dict]:
    """Verified recent heartbeats across all relays, newest first.

    Deduplicated by Nostr pubkey (a volunteer restarting its sweep loop posts
    repeatedly; only the freshest matters). Never raises.
    """
    fetch = fetcher or _default_fetcher()
    filt = heartbeat_filter(since=now - max_age_s)
    best: Dict[str, dict] = {}
    for relay in net.nostr_relays:
        try:
            events = fetch(relay, filt, per_relay_timeout)
        except Exception:  # noqa: BLE001 -- a dead relay must not abort the pass
            continue
        for event in events or []:
            hb = verify_heartbeat_event(net, event, now=now, max_age_s=max_age_s)
            if hb is None:
                continue
            prior = best.get(hb["pubkey"])
            if prior is None or hb["seen_at"] > prior["seen_at"]:
                best[hb["pubkey"]] = hb
    return sorted(best.values(), key=lambda h: -h["seen_at"])


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

    def collect_ecdsa(
        self,
        *,
        nonce: bytes,
        digest: bytes,
        threshold: int,
        have: Optional[Dict[str, bytes]] = None,
        deadline_s: float = 20.0,
        relay_offset: int = 0,
    ) -> "EcdsaSigResult":
        """The outbound sweep: same relay loop, ECDSA gates, no coin tag.

        ``threshold`` comes from the live ``Portal.signatureThreshold()`` read
        rather than the BLS constant -- the two quorums are independent and
        the EVM one is owner-mutable. No portal-freshness logic on purpose:
        outbound signatures never expire.
        """
        net = self._net
        collected: Dict[str, bytes] = dict(have or {})
        routing_tag = outbound_routing_tag(nonce)
        filt = build_filter(net, routing_tag)

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
            collected = collect_ecdsa_from_events(
                net, events, digest=digest, routing_tag=routing_tag, have=collected,
            )

        return EcdsaSigResult(collected=collected, threshold=int(threshold))
