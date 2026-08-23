"""Base (EVM) client for the warp.green bridge deposit leg.

This is the Base-chain half of the bridge: it reads the hot wallet's ETH/USDC
balances and the ERC20Bridge allowance, reads the live ``Portal.messageToll()``
and ``ERC20Bridge.tip()``, builds and signs the two deposit transactions
(``approve`` then ``bridgeToChia``), broadcasts them idempotently, and reads back
receipts -- including parsing the ``MessageSent`` nonce that the Chia claim path
keys off. No funds move without a signed transaction the caller persisted first.

Design, matching the sibling thin clients (coinset/watcher/wallet):

* **Injected transport.** ``caller(method, params) -> result`` speaks JSON-RPC
  2.0 with the envelope already unwrapped (``result`` returned, ``error`` raised
  as :class:`EvmRpcError`). The default builds a ``requests`` session lazily, so
  importing this module pulls in nothing and the state machine's tests drive it
  with a canned fake.
* **Hand-rolled ABI.** Every call the bridge needs uses only static types
  (``address``/``uint256``/``bytes32``), so encoding is a 4-byte selector plus
  32-byte-padded words -- no ``eth_abi`` dependency. Selectors and the
  ``MessageSent`` topic are vendored constants, verified against ``keccak`` in
  the tests.
* **Signing stays lazy.** ``eth_account`` is imported only inside
  :func:`sign_tx`/:func:`tx_hash_of`; the pure builders and parsers need no
  third-party import and are fully unit-testable.

Everything hex-facing follows the warp-stack convention: lower-case without a
``0x`` prefix internally, re-prefixed with ``0x`` on the way to the RPC.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Optional

from .constants import WarpAsset, WarpNet

# (method, params) -> JSON-RPC ``result`` (envelope already unwrapped).
Caller = Callable[[str, list], Any]


class EvmError(RuntimeError):
    """An EVM RPC call failed at transport level or returned junk."""


class EvmRpcError(EvmError):
    """The node returned a JSON-RPC ``error`` object.

    Carries the node's ``message``/``code`` so idempotent broadcast can tell a
    benign "already known" from a real rejection.
    """

    def __init__(self, message: str, code: Any = None, data: Any = None) -> None:
        super().__init__(message)
        self.rpc_message = message
        self.code = code
        self.data = data


# --------------------------------------------------------------------------- #
# Function selectors + event topic (verified against keccak in the tests).
# --------------------------------------------------------------------------- #

SEL_APPROVE = bytes.fromhex("095ea7b3")           # approve(address,uint256)
SEL_BALANCE_OF = bytes.fromhex("70a08231")        # balanceOf(address)
SEL_ALLOWANCE = bytes.fromhex("dd62ed3e")         # allowance(address,address)
SEL_DECIMALS = bytes.fromhex("313ce567")          # decimals()
SEL_MESSAGE_TOLL = bytes.fromhex("79c06b8b")      # messageToll()
SEL_TIP = bytes.fromhex("2755cd2d")               # tip()
SEL_BRIDGE_TO_CHIA = bytes.fromhex("cdb50da7")    # bridgeToChia(address,bytes32,uint256)
# Outbound (unwrap) relay -- both verified against keccak in the tests, and
# SEL_RECEIVE_MESSAGE against the deployed selector noted in
# docs/warp-unwrap-design.md §2.2.
SEL_RECEIVE_MESSAGE = bytes.fromhex("b2e7bebb")   # receiveMessage(bytes32,bytes3,bytes32,address,bytes32[],bytes)
SEL_SIGNATURE_THRESHOLD = bytes.fromhex("a82f2e26")  # signatureThreshold()
SEL_EIP712_DOMAIN = bytes.fromhex("84b0196e")     # eip712Domain() (EIP-5267)
SEL_BURN_PUZZLE_HASH = bytes.fromhex("3f4710d3")  # burnPuzzleHash()
SEL_TRANSFER = bytes.fromhex("a9059cbb")          # transfer(address,uint256)

# MessageReceived(bytes32 indexed nonce, ...) -- the Portal's delivery event.
# Pinned against the real relay receipt in tests/fixtures_unwrap.json, not
# derived from a guessed event signature.
MESSAGE_RECEIVED_TOPIC0 = "08d1bf12867015b2874c8fcd6f1b0403eb05ca20867f81d40d3b232da098f9af"

# MessageSent(bytes32 indexed nonce, address, bytes3, bytes32, bytes32[])
MESSAGE_SENT_TOPIC0 = "ca4cf462dc4787a3aa57636ad2349b8fb4e4f2d2c0ef4ac57f85955f7251a7a8"

# Gas fallbacks used only when ``eth_estimateGas`` is unavailable; the happy path
# always estimates live and applies headroom.
_APPROVE_GAS_DEFAULT = 80_000
_BRIDGE_GAS_DEFAULT = 300_000
#: Public alias: the dry-run BRIDGING fallback signs at this limit when the
#: estimate cannot exist (see is_allowance_revert).  The signed transaction is
#: never broadcast in a rehearsal, so the figure only has to be plausible.
BRIDGE_GAS_DEFAULT = _BRIDGE_GAS_DEFAULT
# A real relay measured 145,195; 250k default only when estimation is down.
_RELAY_GAS_DEFAULT = 250_000

# Node error fragments that mean "this exact transaction is already in the
# mempool or already mined" -- rebroadcasting the stored raw is a no-op, so we
# report success and let the receipt poll disambiguate.
_BENIGN_BROADCAST = (
    "already known",
    "already imported",
    "known transaction",
    "already exists",
    "nonce too low",
    "transaction already in mempool",
)


# --------------------------------------------------------------------------- #
# Hex helpers.
# --------------------------------------------------------------------------- #

def _hx(value: Any) -> str:
    """Normalise hex to lower-case without ``0x``; ints -> hex; ``None`` -> ""."""
    if value is None:
        return ""
    if isinstance(value, int):
        return format(value, "x")
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower()


def _0x(value: Any) -> str:
    return "0x" + _hx(value)


def _qty(value: int) -> str:
    """Encode an integer as a minimal ``0x`` JSON-RPC quantity."""
    return "0x" + format(int(value), "x")


def _to_int(value: Any) -> Optional[int]:
    """Parse a JSON-RPC quantity/hex string to int; ``None``/"" -> ``None``."""
    if value is None:
        return None
    if isinstance(value, int):
        return value
    h = _hx(value)
    return int(h, 16) if h else None


# --------------------------------------------------------------------------- #
# ABI encoding (static types only).
# --------------------------------------------------------------------------- #

def _enc_address(addr: Any) -> bytes:
    b = bytes.fromhex(_hx(addr))
    if len(b) != 20:
        raise EvmError(f"address must be 20 bytes, got {len(b)}")
    return b"\x00" * 12 + b


def _enc_uint256(n: int) -> bytes:
    n = int(n)
    if n < 0 or n >= 2 ** 256:
        raise EvmError(f"uint256 out of range: {n}")
    return n.to_bytes(32, "big")


def _enc_bytes32(value: Any) -> bytes:
    b = value if isinstance(value, (bytes, bytearray)) else bytes.fromhex(_hx(value))
    b = bytes(b)
    if len(b) != 32:
        raise EvmError(f"bytes32 must be 32 bytes, got {len(b)}")
    return b


def encode_call(selector: bytes, *words: bytes) -> bytes:
    return selector + b"".join(words)


def encode_transfer(to: Any, amount_base_units: int) -> bytes:
    return encode_call(SEL_TRANSFER, _enc_address(to), _enc_uint256(amount_base_units))


def encode_approve(spender: Any, amount_base_units: int) -> bytes:
    return encode_call(SEL_APPROVE, _enc_address(spender), _enc_uint256(amount_base_units))


def encode_millieth_deposit() -> bytes:
    """``deposit()`` -- the classic WETH-style payable mint.

    keccak("deposit()")[:4] = d0e30db0.  MilliETH.sol mints msg.value/1e12
    units (1000 milliETH per ETH at 3 decimals) and REVERTS unless
    msg.value is a multiple of 1e12 wei; callers validate first.
    """
    return encode_call(bytes.fromhex("d0e30db0"))


def encode_millieth_withdraw(amount_units: int) -> bytes:
    """``withdraw(uint256)`` -- burn milliETH units, receive ETH.

    keccak("withdraw(uint256)")[:4] = 2e1a7d4d.  amount is in the token's
    own 3-decimal units (1 unit = 0.001 milliETH = 1e12 wei of ETH back).
    """
    return encode_call(bytes.fromhex("2e1a7d4d"), _enc_uint256(int(amount_units)))


def encode_bridge_to_chia(asset: Any, receiver_ph: Any, mojo_amount: int) -> bytes:
    return encode_call(
        SEL_BRIDGE_TO_CHIA,
        _enc_address(asset),
        _enc_bytes32(receiver_ph),
        _enc_uint256(mojo_amount),
    )


# --------------------------------------------------------------------------- #
# Outbound relay encoding (unwrap: Portal.receiveMessage).
#
# The only dynamic-ABI call in this module. Everything else is static words, so
# the head/tail layout is written out longhand here and byte-compared against
# eth_abi's independent encoder in the tests rather than pulling eth_abi into
# the runtime path.
# --------------------------------------------------------------------------- #

def pack_validator_sigs(sigs: list) -> bytes:
    """Pack validator signatures the way the Portal demands.

    ``sigs`` is a list of ``(signer_address, v, r, s)`` with ``v`` in
    {27, 28}, ``r``/``s`` 32 bytes each. The Portal requires (all verified
    [V] in docs/warp-unwrap-design.md §2.2):

    * layout ``v||r||s`` per signature -- NOT the usual r||s||v;
    * recovered signers in strictly ascending address order
      (``require(signer > lastSigner)``), so the pack sorts;
    * the exact length ``signatureThreshold * 65`` -- not a minimum -- which
      is the caller's check since only it knows the live threshold.
    """
    entries = []
    seen = set()
    for signer, v, r, s in sigs:
        addr = int(_hx(signer), 16)
        if addr in seen:
            raise EvmError(f"duplicate validator signature from {signer}")
        seen.add(addr)
        v = int(v)
        if v not in (27, 28):
            raise EvmError(f"signature v must be 27 or 28, got {v}")
        r, s = bytes(r), bytes(s)
        if len(r) != 32 or len(s) != 32:
            raise EvmError("signature r and s must be 32 bytes each")
        entries.append((addr, bytes([v]) + r + s))
    entries.sort(key=lambda e: e[0])
    return b"".join(chunk for _addr, chunk in entries)


def encode_receive_message(
    nonce: Any,
    source_chain: bytes,
    source: Any,
    destination: Any,
    contents: list,
    sigs_packed: bytes,
) -> bytes:
    """Calldata for ``Portal.receiveMessage(bytes32,bytes3,bytes32,address,bytes32[],bytes)``.

    ``source`` is the Chia-side sender (the bridging coin's parent lineage --
    a 32-byte word), ``destination`` the ERC20Bridge, ``contents`` the
    validator-padded 32-byte memo atoms. Head is six slots; ``contents`` and
    ``sigs`` are dynamic tails.
    """
    contents_words = [_enc_bytes32(c) for c in contents]
    head_size = 6 * 32
    contents_offset = head_size
    contents_size = 32 + 32 * len(contents_words)
    sigs_offset = contents_offset + contents_size

    chain = bytes(source_chain)
    if len(chain) != 3:
        raise EvmError(f"source_chain must be 3 bytes, got {len(chain)}")

    sigs = bytes(sigs_packed)
    if len(sigs) % 65 != 0:
        raise EvmError(f"packed sigs length {len(sigs)} is not a multiple of 65")
    padded_sigs = sigs + b"\x00" * ((32 - len(sigs) % 32) % 32)

    return (
        SEL_RECEIVE_MESSAGE
        + _enc_bytes32(nonce)
        + chain + b"\x00" * 29                    # bytes3, right-padded
        + _enc_bytes32(source)
        + _enc_address(destination)
        + _enc_uint256(contents_offset)
        + _enc_uint256(sigs_offset)
        + _enc_uint256(len(contents_words))
        + b"".join(contents_words)
        + _enc_uint256(len(sigs))
        + padded_sigs
    )


# keccak256("Message(bytes32 nonce,bytes3 source_chain,bytes32 source,
# address destination,bytes32[] contents)") -- anchored to keccak in the tests
# and to the deployed Portal per docs/warp-unwrap-design.md §2.2.
MESSAGE_TYPE_HASH = bytes.fromhex(
    "9972dc9e80132460f6459b361feb003781068b85cac2d95d54bc2150f439b824"
)


def validator_message_digest(
    domain_separator: bytes,
    nonce: Any,
    source_chain: bytes,
    source: Any,
    destination: Any,
    contents: list,
) -> bytes:
    """The EIP-712 digest the validators sign for an outbound message.

    ``domain_separator`` is read live from the Portal (it binds chainId and
    the verifying contract; hardcoding it would silently break on a portal
    redeploy). Unlike the inbound leg, nothing in this digest is mutable --
    outbound signatures never expire [V].

    Per EIP-712: value types pad to 32 (``bytes3`` right-pads), dynamic
    arrays hash to ``keccak(concat(elements))``, and the digest is
    ``keccak(0x1901 || domainSeparator || structHash)``.
    """
    from eth_utils import keccak

    sep = bytes(domain_separator)
    if len(sep) != 32:
        raise EvmError(f"domain separator must be 32 bytes, got {len(sep)}")
    chain = bytes(source_chain)
    if len(chain) != 3:
        raise EvmError(f"source_chain must be 3 bytes, got {len(chain)}")
    struct_hash = keccak(
        MESSAGE_TYPE_HASH
        + _enc_bytes32(nonce)
        + chain + b"\x00" * 29
        + _enc_bytes32(source)
        + _enc_address(destination)
        + keccak(b"".join(_enc_bytes32(c) for c in contents))
    )
    return keccak(b"\x19\x01" + sep + struct_hash)


def parse_message_received(receipt: dict, portal_address: Any, nonce: Any) -> bool:
    """Whether *receipt* carries the Portal's MessageReceived for *nonce*."""
    # _hx stringifies bytes into repr garbage; hex them first.
    want_nonce = nonce.hex() if isinstance(nonce, (bytes, bytearray)) else _hx(nonce)
    want_addr = _hx(portal_address)
    for log in receipt.get("logs") or []:
        if _hx(log.get("address", "")) != want_addr:
            continue
        topics = log.get("topics") or []
        if len(topics) >= 2 and _hx(topics[0]) == MESSAGE_RECEIVED_TOPIC0 \
                and _hx(topics[1]) == want_nonce:
            return True
    return False


def unwrap_post_tip_base_units(mojo_amount: int, tip_bps: int, decimals: int, cat_decimals: int) -> int:
    """What the receiver actually gets on Base: SCALE first, THEN tip.

    Proven by execution against two real unwraps: the ERC20Bridge scales
    CAT mojos to ERC-20 base units before taking its 30 bps tip, so the
    result is not integral in mojos and the minimum viable unwrap is ONE
    mojo (0.001 USDC -> 997 base units). This is the opposite order from
    the inbound model -- :func:`post_tip_amount` works in mojos, raises on
    1 mojo, and must never be reused here; a test pins the two apart.
    """
    mojo_amount = int(mojo_amount)
    if mojo_amount < 1:
        raise EvmError(f"unwrap amount must be at least 1 mojo, got {mojo_amount}")
    scaled = mojo_amount * 10 ** (int(decimals) - int(cat_decimals))
    tip = scaled * int(tip_bps) // 10_000
    if scaled <= tip:
        raise EvmError(f"amount {scaled} does not clear the tip {tip}")
    return scaled - tip


def decode_revert_reason(exc: BaseException) -> str:
    """The human string inside an ABI ``Error(string)`` revert blob, if any.

    Nodes differ: some put the reason in the message, some only in ``data``
    as ``0x08c379a0 || abi.encode(string)``. Falls back to ``str(exc)``.
    """
    data = getattr(exc, "data", None)
    if isinstance(data, str) and data.startswith("0x08c379a0"):
        try:
            blob = bytes.fromhex(data[2:])
            strlen = int.from_bytes(blob[36:68], "big")
            return blob[68:68 + strlen].decode("utf-8", "replace")
        except Exception:  # noqa: BLE001 -- fall through to the message
            pass
    return str(exc)


def is_already_delivered(exc: BaseException) -> bool:
    """Whether a relay revert means the message was already delivered.

    ``usedNonces[key] = true`` is written *before* the bridge call with no
    try/catch, so a bridge-side revert rolls the nonce write back and the
    relay is retryable. Only the Portal's own ``"!nonce"`` means someone
    (us or a third party) already delivered this message [V].
    """
    return "!nonce" in decode_revert_reason(exc)


# Node error fragments that are NODE/OPERATOR conditions, not a deterministic
# rejection of THIS message: they clear once connectivity or funding is
# restored, so they must never poison a message's skip-list counter.
_INFRA_RPC_FRAGMENTS = (
    "insufficient funds",
    "insufficient balance",
    "gas required exceeds",
    "intrinsic gas too low",
    "max fee per gas",
    "fee cap",
    "max priority fee",
    "replacement transaction underpriced",
    "nonce too low",
    "nonce too high",
    "already known",
    "timeout",
    "rate limit",
    "too many requests",
)


def is_infrastructure_error(exc: BaseException) -> bool:
    """Whether *exc* is a transport/node/operator condition, not a revert.

    A plain :class:`EvmError` (never reached the node, or junk response) is
    always infrastructure. An :class:`EvmRpcError` is infrastructure only when
    its message names a node/operator condition (an unfunded key, a fee/nonce
    problem, a rate limit) rather than a deterministic execution revert of
    this message. Callers use this to retry rather than skip-list a message
    that is perfectly deliverable once the transient condition clears.
    """
    if not isinstance(exc, EvmError):
        return False
    if not isinstance(exc, EvmRpcError):
        return True                       # transport / junk: never message-specific
    msg = str(getattr(exc, "rpc_message", "") or exc).lower()
    return any(frag in msg for frag in _INFRA_RPC_FRAGMENTS)


# --------------------------------------------------------------------------- #
# Amount conversions (USDC 6-dec base units <-> CAT 3-dec mojos).
# --------------------------------------------------------------------------- #

def _asset_decimals(net: WarpNet, asset: Optional["WarpAsset"]) -> int:
    return asset.erc20_decimals if asset is not None else net.usdc_decimals


def _mojo_factor(net: WarpNet, asset: Optional["WarpAsset"] = None) -> int:
    """ERC20 base units per wrapped-CAT mojo for *asset* (default: USDC).

    USDC is 6-decimal against 3-decimal CATs, so the factor is 1000;
    milliETH is itself 3-decimal, so its factor is 1. Passing the asset
    DESCRIPTOR rather than a bare address keeps the scale welded to the
    token -- mixing one asset's address with another's decimals would
    mis-size a bridge by three orders of magnitude.
    """
    decimals = _asset_decimals(net, asset)
    if decimals < net.cat_decimals:
        # 10 ** negative is a FLOAT in Python, which would silently turn
        # every amount conversion below into float arithmetic on money.
        # Assets are validated at engine construction too; this is the
        # last-resort guard for a descriptor built by hand.
        raise ValueError(
            f"asset decimals {decimals} < CAT decimals {net.cat_decimals}: "
            "wrapped amounts would lose precision and the factor would not "
            "be an integer"
        )
    return 10 ** (decimals - net.cat_decimals)


def mojo_to_base_units(mojo: int, net: WarpNet,
                       asset: Optional["WarpAsset"] = None) -> int:
    """CAT mojos -> ERC20 base units pulled by ``bridgeToChia`` (mojo * factor)."""
    return int(mojo) * _mojo_factor(net, asset)


def base_units_to_mojo(base_units: int, net: WarpNet,
                       asset: Optional["WarpAsset"] = None) -> int:
    """ERC20 base units -> CAT mojos (floor; sub-mojo dust is unbridgeable)."""
    return int(base_units) // _mojo_factor(net, asset)


def bridgeable_mojo(base_units: int, net: WarpNet,
                    asset: Optional["WarpAsset"] = None) -> int:
    """Largest whole CAT mojo amount fundable by ``base_units`` of the asset."""
    return base_units_to_mojo(base_units, net, asset)


def post_tip_amount(mojo: int, tip_bps: int) -> int:
    """Mirror the on-chain tip deduction: ``mojo - max(1, mojo*tip/10000)``.

    This reproduces ``ERC20Bridge`` exactly (a zero tip is floored to 1, and the
    contract requires ``amount > tip``), so the claim path can precompute the
    attested ``contents[2]`` and refuse to proceed if the watcher disagrees.
    """
    mojo = int(mojo)
    tip = (mojo * int(tip_bps)) // 10000
    if tip < 1:
        tip = 1
    if mojo <= tip:
        raise EvmError(f"amount {mojo} too small to survive tip {tip}")
    return mojo - tip


# --------------------------------------------------------------------------- #
# EIP-1559 fees.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class EIP1559Fees:
    max_fee_per_gas: int
    max_priority_fee_per_gas: int


def suggest_fees(
    base_fee_per_gas: int,
    priority_fee_per_gas: int,
    *,
    base_fee_headroom: int = 2,
) -> EIP1559Fees:
    """``maxFee = base*headroom + priority`` -- room for a couple of base bumps."""
    return EIP1559Fees(
        max_fee_per_gas=int(base_fee_per_gas) * int(base_fee_headroom) + int(priority_fee_per_gas),
        max_priority_fee_per_gas=int(priority_fee_per_gas),
    )


def bump_fees(fees: EIP1559Fees, attempt: int, *, cap: int = 3, num: int = 13, den: int = 10) -> EIP1559Fees:
    """Escalate both fee legs by ``(num/den)**attempt`` over the *original* fees.

    ``attempt`` is the replacement count (0 == the first broadcast, unchanged);
    it is clamped to ``cap`` so a stuck job cannot bid the fee up without bound.
    Because each result is derived from the same base ``fees``, consecutive
    attempts differ by the full ``num/den`` ratio -- comfortably above the node's
    10% replacement floor.
    """
    n = min(max(int(attempt), 0), cap)
    scale_num = num ** n
    scale_den = den ** n
    return EIP1559Fees(
        max_fee_per_gas=fees.max_fee_per_gas * scale_num // scale_den,
        max_priority_fee_per_gas=fees.max_priority_fee_per_gas * scale_num // scale_den,
    )


# --------------------------------------------------------------------------- #
# Receipt / log parsing (pure).
# --------------------------------------------------------------------------- #

#: Node wording for "this call reverts", as distinct from "the node is
#: unreachable". Deliberately narrow: a false positive here refuses to broadcast
#: a sound transaction, which surfaces to the operator, whereas a false negative
#: broadcasts one already known to fail and burns the gas.
_REVERT_MARKERS: tuple = (
    "execution reverted",
    "execution error",
    "always failing transaction",
)


def _is_execution_revert(exc: BaseException) -> bool:
    """Whether an RPC failure is the node rejecting the call, not the transport."""
    low = str(exc).lower()
    if any(marker in low for marker in _REVERT_MARKERS):
        return True
    data = getattr(exc, "data", None)
    return isinstance(data, str) and data.startswith("0x08c379a0")  # Error(string)


def is_allowance_revert(exc: BaseException) -> bool:
    """Whether an RPC failure is an execution revert at the token's
    allowance guard.

    The dry-run bridge preflight hits this by DESIGN: the approve is signed
    but never broadcast in a rehearsal, so the chain's allowance is still
    zero when ``bridgeToChia`` is estimated.  That specific revert proves
    the RPC round-tripped, the calldata decoded, and the bridge contract
    executed to the token ``transferFrom`` guard -- it is the rehearsal's
    success signal, not a failure.  Substring-matching the reason keeps
    every token wording covered (OpenZeppelin "transfer amount exceeds
    allowance" / "insufficient allowance", Circle's FiatToken) while any
    other revert -- toll, pause, receiver -- stays fatal.

    The reason is read through :func:`decode_revert_reason`, not ``str``:
    nodes differ on where they put it (message vs the ABI ``Error(string)``
    blob in ``data``), and matching the message alone silently re-breaks
    the dry run on reason-in-data providers.  Known residual limit: a token
    using OZ v5 *custom errors* (``ERC20InsufficientAllowance`` selector)
    carries no reason string at all and cannot be recognised here -- fine
    for USDC's FiatToken (require strings), worth revisiting if a
    custom-error token is ever bridged.
    """
    return (_is_execution_revert(exc)
            and "allowance" in decode_revert_reason(exc).lower())


def receipt_status(receipt: dict) -> Optional[int]:
    """1 == success, 0 == reverted, ``None`` == pre-Byzantium/absent."""
    return _to_int(receipt.get("status"))


def receipt_succeeded(receipt: dict) -> bool:
    return receipt_status(receipt) == 1


def receipt_reverted(receipt: dict) -> bool:
    return receipt_status(receipt) == 0


def receipt_block_number(receipt: dict) -> Optional[int]:
    return _to_int(receipt.get("blockNumber"))


def confirmations(receipt_block: Optional[int], head_block: Optional[int]) -> int:
    """Inclusive depth of ``receipt_block`` beneath ``head_block`` (0 if unmined)."""
    if receipt_block is None or head_block is None:
        return 0
    return max(0, int(head_block) - int(receipt_block) + 1)


def parse_message_sent_nonce(receipt: dict, portal_address: str) -> str:
    """Return the ``MessageSent`` nonce (32-byte hex) the Portal logged.

    The bridge calls ``Portal.sendMessage``, so the event is emitted by the
    **Portal** contract; we match both the emitting address and topic0 so a
    look-alike log from another contract can never be mistaken for the nonce.
    Leading zeros are preserved -- the nonce is used verbatim as a CLVM atom
    downstream, not re-encoded as an integer.
    """
    portal = _hx(portal_address)
    for log in receipt.get("logs") or []:
        if _hx(log.get("address")) != portal:
            continue
        topics = log.get("topics") or []
        if len(topics) < 2 or _hx(topics[0]) != MESSAGE_SENT_TOPIC0:
            continue
        nonce = _hx(topics[1])
        if len(nonce) != 64:
            raise EvmError(f"MessageSent nonce is not 32 bytes: {nonce!r}")
        return nonce
    raise EvmError("no MessageSent log from the portal in receipt")


# --------------------------------------------------------------------------- #
# Transactions.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class UnsignedTx:
    """A type-2 (EIP-1559) transaction, ready to sign.

    ``to`` is 20 raw bytes and ``data`` is raw bytes; :meth:`as_dict` renders the
    exact shape ``eth_account`` accepts (a lower-case hex string for ``to`` is
    rejected by its validator, so bytes are used).
    """

    chain_id: int
    nonce: int
    to: bytes
    value: int
    data: bytes
    gas: int
    max_fee_per_gas: int
    max_priority_fee_per_gas: int

    def as_dict(self) -> dict:
        return {
            "type": 2,
            "chainId": self.chain_id,
            "nonce": self.nonce,
            "to": self.to,
            "value": self.value,
            "data": "0x" + self.data.hex(),
            "gas": self.gas,
            "maxFeePerGas": self.max_fee_per_gas,
            "maxPriorityFeePerGas": self.max_priority_fee_per_gas,
        }


@dataclass(frozen=True)
class SignedTx:
    raw: bytes
    tx_hash: str  # 0x-prefixed
    nonce: int


def _addr_bytes(addr: str) -> bytes:
    b = bytes.fromhex(_hx(addr))
    if len(b) != 20:
        raise EvmError(f"address must be 20 bytes, got {len(b)}")
    return b


def build_approve_tx(
    net: WarpNet,
    *,
    amount_base_units: int,
    nonce: int,
    fees: EIP1559Fees,
    gas: int,
    spender: Optional[str] = None,
    asset: Optional["WarpAsset"] = None,
) -> UnsignedTx:
    """Sign-ready ``approve(bridge, amount)`` on the asset's token contract."""
    data = encode_approve(spender or net.erc20_bridge_address, amount_base_units)
    return UnsignedTx(
        chain_id=net.evm_chain_id,
        nonce=int(nonce),
        to=_addr_bytes(asset.erc20_address if asset else net.usdc_address),
        value=0,
        data=data,
        gas=int(gas),
        max_fee_per_gas=fees.max_fee_per_gas,
        max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
    )


def build_bridge_tx(
    net: WarpNet,
    *,
    receiver_ph: Any,
    mojo_amount: int,
    toll_wei: int,
    nonce: int,
    fees: EIP1559Fees,
    gas: int,
    asset: Optional["WarpAsset"] = None,
) -> UnsignedTx:
    """Sign-ready ``bridgeToChia`` on the ERC20Bridge; ``value`` is the exact toll."""
    data = encode_bridge_to_chia(
        asset.erc20_address if asset else net.usdc_address,
        receiver_ph, mojo_amount)
    return UnsignedTx(
        chain_id=net.evm_chain_id,
        nonce=int(nonce),
        to=_addr_bytes(net.erc20_bridge_address),
        value=int(toll_wei),
        data=data,
        gas=int(gas),
        max_fee_per_gas=fees.max_fee_per_gas,
        max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
    )


def sign_tx(unsigned: UnsignedTx, private_key: bytes) -> SignedTx:
    """Sign ``unsigned`` with a 32-byte key; returns the raw bytes + tx hash.

    Signing is deterministic (RFC 6979), so the same inputs yield the same raw
    bytes -- the state machine signs once, persists ``raw``, and rebroadcasts it
    idempotently rather than re-signing while a receipt is still possible.
    """
    from eth_account import Account  # lazy: only when actually signing

    signed = Account.sign_transaction(unsigned.as_dict(), private_key)
    raw = getattr(signed, "raw_transaction", None)
    if raw is None:  # eth_account < 0.13 spelling
        raw = signed.rawTransaction
    return SignedTx(
        raw=bytes(raw),
        tx_hash="0x" + bytes(signed.hash).hex(),
        nonce=unsigned.nonce,
    )


def tx_hash_of(raw: bytes) -> str:
    """The transaction hash of an already-signed raw transaction."""
    from eth_utils import keccak  # lazy

    return "0x" + keccak(bytes(raw)).hex()


# --------------------------------------------------------------------------- #
# Client.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class EvmBalances:
    eth_wei: int
    usdc_units: int


def _default_caller(url: str, timeout: float) -> Caller:
    import itertools  # stdlib
    import requests  # lazy: only when a real network call is made

    session = requests.Session()
    counter = itertools.count(1)

    def call(method: str, params: list) -> Any:
        body = {"jsonrpc": "2.0", "id": next(counter), "method": method, "params": list(params)}
        resp = session.post(url, json=body, timeout=timeout)
        resp.raise_for_status()
        data = resp.json()
        err = data.get("error")
        if err:
            if isinstance(err, dict):
                raise EvmRpcError(str(err.get("message") or err), err.get("code"), err.get("data"))
            raise EvmRpcError(str(err))
        return data.get("result")

    return call


class EvmClient:
    """Reads Base state and builds/broadcasts the two deposit transactions."""

    def __init__(
        self,
        net: WarpNet,
        *,
        rpc_url: Optional[str] = None,
        caller: Optional[Caller] = None,
        timeout: float = 30.0,
        default_priority_fee_wei: int = 1_000_000,
    ) -> None:
        self._net = net
        url = (rpc_url or net.evm_default_rpc_url or "").rstrip("/")
        self._url = url
        self._timeout = timeout
        self._default_priority_fee_wei = int(default_priority_fee_wei)
        self._caller = caller or _default_caller(url, timeout)

    @property
    def net(self) -> WarpNet:
        return self._net

    # -- transport ----------------------------------------------------------- #

    def _call(self, method: str, params: list) -> Any:
        try:
            return self._caller(method, params)
        except EvmError:
            raise
        except Exception as exc:  # noqa: BLE001 -- unify transport failures
            raise EvmError(f"{method} request failed: {exc}") from exc

    def _call_quantity(self, method: str, params: list) -> int:
        return _to_int(self._call(method, params)) or 0

    def _eth_call(self, to: str, data: bytes, *, block: str = "latest") -> bytes:
        res = self._call("eth_call", [{"to": _0x(to), "data": "0x" + data.hex()}, block])
        h = _hx(res)
        return bytes.fromhex(h) if h else b""

    def _call_uint(self, to: str, data: bytes) -> int:
        raw = self._eth_call(to, data)
        if not raw:
            raise EvmError("empty eth_call result (reverted view or wrong contract)")
        return int.from_bytes(raw, "big")

    # -- chain / balances ---------------------------------------------------- #

    def get_chain_id(self) -> int:
        return self._call_quantity("eth_chainId", [])

    def get_block_number(self) -> int:
        return self._call_quantity("eth_blockNumber", [])

    def get_eth_balance(self, address: str) -> int:
        return self._call_quantity("eth_getBalance", [_0x(address), "latest"])

    def get_erc20_balance(self, token: str, owner: str) -> int:
        return self._call_uint(token, encode_call(SEL_BALANCE_OF, _enc_address(owner)))

    def get_erc20_decimals(self, token: str) -> int:
        return self._call_uint(token, SEL_DECIMALS)

    def get_allowance(self, token: str, owner: str, spender: str) -> int:
        return self._call_uint(
            token, encode_call(SEL_ALLOWANCE, _enc_address(owner), _enc_address(spender))
        )

    def get_balances(self, owner: str) -> EvmBalances:
        usdc = self.get_erc20_balance(self._net.usdc_address, owner) if self._net.usdc_address else 0
        return EvmBalances(eth_wei=self.get_eth_balance(owner), usdc_units=usdc)

    def get_bridge_allowance(self, owner: str,
                             asset: Optional["WarpAsset"] = None) -> int:
        token = asset.erc20_address if asset else self._net.usdc_address
        return self.get_allowance(token, owner, self._net.erc20_bridge_address)

    # -- live protocol parameters ------------------------------------------- #

    def get_message_toll(self) -> int:
        """``Portal.messageToll()`` -- owner-mutable, so always read live."""
        return self._call_uint(self._net.portal_address, SEL_MESSAGE_TOLL)

    def get_tip_bps(self) -> int:
        """``ERC20Bridge.tip()`` in basis points (immutable)."""
        return self._call_uint(self._net.erc20_bridge_address, SEL_TIP)

    def get_signature_threshold(self) -> int:
        """``Portal.signatureThreshold()`` -- owner-mutable, so always read live.

        The relay's signature blob must be exactly ``threshold * 65`` bytes --
        the contract checks equality, not a minimum -- so a stale count makes
        every relay revert. Read it at gate time, never cached across a job.
        """
        return self._call_uint(self._net.portal_address, SEL_SIGNATURE_THRESHOLD)

    def get_burn_puzzle_hash(self) -> bytes:
        """``ERC20Bridge.burnPuzzleHash()`` -- the live half of the burn anchor."""
        raw = self._eth_call(self._net.erc20_bridge_address, SEL_BURN_PUZZLE_HASH)
        if len(raw) != 32:
            raise EvmError(f"burnPuzzleHash() returned {len(raw)} bytes")
        return raw

    def verify_eip712_domain(self) -> bytes:
        """Read the Portal's EIP-712 domain live and return the separator.

        Via ``eip712Domain()`` (EIP-5267) -- the three dedicated separator
        getters all revert on the deployed Portal, proven by execution. The
        decoded name/version/chainId/verifyingContract must equal the
        deployment constants and the reconstructed separator must equal the
        anchored one; any mismatch means a portal redeploy, and every gate
        downstream of this must fail closed.
        """
        from eth_utils import keccak

        net = self._net
        raw = self._eth_call(net.portal_address, SEL_EIP712_DOMAIN)
        if len(raw) < 5 * 32:
            raise EvmError(f"eip712Domain() returned {len(raw)} bytes")
        # head: fields(bytes1), name offset, version offset, chainId,
        # verifyingContract, salt, extensions offset.
        chain_id = int.from_bytes(raw[3 * 32:4 * 32], "big")
        contract = raw[4 * 32 + 12:5 * 32]

        def _dyn_string(offset_slot: int) -> str:
            off = int.from_bytes(raw[offset_slot * 32:(offset_slot + 1) * 32], "big")
            n = int.from_bytes(raw[off:off + 32], "big")
            return raw[off + 32:off + 32 + n].decode("utf-8", "replace")

        name, version = _dyn_string(1), _dyn_string(2)
        if (
            name != net.eip712_name
            or version != net.eip712_version
            or chain_id != net.evm_chain_id
            or contract.hex() != net.portal_address[2:].lower()
        ):
            raise EvmError(
                f"portal EIP-712 domain mismatch: ({name!r}, {version!r}, "
                f"{chain_id}, 0x{contract.hex()}) -- a portal redeploy; "
                "refusing every unwrap gate"
            )
        type_hash = keccak(
            text="EIP712Domain(string name,string version,"
                 "uint256 chainId,address verifyingContract)"
        )
        sep = keccak(
            type_hash + keccak(text=name) + keccak(text=version)
            + chain_id.to_bytes(32, "big") + b"\x00" * 12 + contract
        )
        if sep.hex() != net.eip712_domain_separator:
            raise EvmError("reconstructed EIP-712 separator does not match the anchor")
        return sep

    # -- fees / nonce -------------------------------------------------------- #

    def get_nonce(self, address: str, *, pending: bool = True) -> int:
        tag = "pending" if pending else "latest"
        return self._call_quantity("eth_getTransactionCount", [_0x(address), tag])

    def get_fee_data(self, *, base_fee_headroom: int = 2) -> EIP1559Fees:
        block = self._call("eth_getBlockByNumber", ["latest", False]) or {}
        base = _to_int(block.get("baseFeePerGas")) or 0
        try:
            prio = self._call_quantity("eth_maxPriorityFeePerGas", [])
        except EvmError:
            prio = self._default_priority_fee_wei
        if prio <= 0:
            prio = self._default_priority_fee_wei
        return suggest_fees(base, prio, base_fee_headroom=base_fee_headroom)

    def estimate_gas(
        self,
        *,
        from_address: str,
        to: str,
        value: int,
        data: bytes,
        headroom_num: int = 5,
        headroom_den: int = 4,
        default: Optional[int] = None,
    ) -> int:
        call = {
            "from": _0x(from_address),
            "to": _0x(to),
            "value": _qty(value),
            "data": "0x" + data.hex(),
        }
        try:
            est = self._call_quantity("eth_estimateGas", [call])
        except EvmError as exc:
            # [WARP-ESTIMATE-REVERT 2026-08-08] An execution revert during
            # estimation is the node saying this transaction will fail. Falling
            # back to a fixed limit and broadcasting anyway spends the gas to
            # be told the same thing on chain. A transport or node error says
            # nothing about the transaction, so the fallback still applies
            # there -- which is the case `default` was added for.
            if _is_execution_revert(exc):
                raise
            if default is None:
                raise
            return default
        return est * headroom_num // headroom_den

    # -- high-level tx preparation ------------------------------------------ #

    def prepare_approve(self, *, owner: str, amount_base_units: int,
                        gas: Optional[int] = None,
                        asset: Optional["WarpAsset"] = None) -> UnsignedTx:
        nonce = self.get_nonce(owner)
        fees = self.get_fee_data()
        token = asset.erc20_address if asset else self._net.usdc_address
        if gas is None:
            data = encode_approve(self._net.erc20_bridge_address, amount_base_units)
            gas = self.estimate_gas(
                from_address=owner, to=token, value=0, data=data,
                default=_APPROVE_GAS_DEFAULT,
            )
        return build_approve_tx(
            self._net, amount_base_units=amount_base_units, nonce=nonce,
            fees=fees, gas=gas, asset=asset,
        )

    def prepare_relay(
        self,
        *,
        owner: str,
        calldata: bytes,
        nonce: Optional[int] = None,
        fees: Optional[EIP1559Fees] = None,
    ) -> UnsignedTx:
        """Sign-ready ``Portal.receiveMessage`` -- non-payable, value 0.

        Unlike the bridge, a relay replacement may safely take a FRESH nonce:
        double delivery is impossible (``usedNonces`` makes the second attempt
        revert ``!nonce``), so pinning is an optimisation, not a safety rail.
        """
        if nonce is None:
            nonce = self.get_nonce(owner)
        if fees is None:
            fees = self.get_fee_data()
        gas = self.estimate_gas(
            from_address=owner, to=self._net.portal_address, value=0,
            data=calldata, default=_RELAY_GAS_DEFAULT,
        )
        return UnsignedTx(
            chain_id=self._net.evm_chain_id,
            nonce=int(nonce),
            to=_addr_bytes(self._net.portal_address),
            value=0,
            data=calldata,
            gas=gas,
            max_fee_per_gas=fees.max_fee_per_gas,
            max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
        )

    def prepare_bridge(
        self,
        *,
        owner: str,
        receiver_ph: Any,
        mojo_amount: int,
        gas: Optional[int] = None,
        toll_wei: Optional[int] = None,
        nonce: Optional[int] = None,
        fees: Optional[EIP1559Fees] = None,
        asset: Optional["WarpAsset"] = None,
    ) -> UnsignedTx:
        """Build an unsigned bridgeToChia.

        ``nonce`` and ``fees`` are overridable so a stuck transaction can be
        re-signed as a *replacement*: same nonce, higher fee. Left unset they
        are read live, which is what a first broadcast wants.
        """
        toll = toll_wei if toll_wei is not None else self.get_message_toll()
        if nonce is None:
            nonce = self.get_nonce(owner)
        if fees is None:
            fees = self.get_fee_data()
        if gas is None:
            # The SAME asset feeds the gas estimate and the built tx; a
            # mismatch would estimate one token and bridge another.
            data = encode_bridge_to_chia(
                asset.erc20_address if asset else self._net.usdc_address,
                receiver_ph, mojo_amount)
            gas = self.estimate_gas(
                from_address=owner, to=self._net.erc20_bridge_address, value=toll, data=data,
                default=_BRIDGE_GAS_DEFAULT,
            )
        return build_bridge_tx(
            self._net, receiver_ph=receiver_ph, mojo_amount=mojo_amount, toll_wei=toll,
            nonce=nonce, fees=fees, gas=gas, asset=asset,
        )

    # -- broadcast / receipts ------------------------------------------------ #

    def send_raw_transaction(self, raw: bytes) -> str:
        """Broadcast a signed transaction; idempotent on rebroadcast.

        A node error that means "already in the mempool / already mined" is not
        an error for us -- we return the transaction's own hash so the caller
        keeps polling the receipt. Any other rejection propagates.
        """
        try:
            res = self._call("eth_sendRawTransaction", ["0x" + bytes(raw).hex()])
            return _0x(res)
        except EvmRpcError as exc:
            msg = (exc.rpc_message or "").lower()
            if any(s in msg for s in _BENIGN_BROADCAST):
                return tx_hash_of(raw)
            raise EvmError(f"sendRawTransaction rejected: {exc.rpc_message}") from exc

    def get_transaction_receipt(self, tx_hash: str) -> Optional[dict]:
        res = self._call("eth_getTransactionReceipt", [_0x(tx_hash)])
        return res if isinstance(res, dict) else None

    def get_confirmations(self, tx_hash: str) -> int:
        receipt = self.get_transaction_receipt(tx_hash)
        if not receipt:
            return 0
        block = receipt_block_number(receipt)
        if block is None:
            return 0
        return confirmations(block, self.get_block_number())
