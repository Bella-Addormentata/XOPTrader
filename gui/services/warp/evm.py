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

from .constants import WarpNet

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

# MessageSent(bytes32 indexed nonce, address, bytes3, bytes32, bytes32[])
MESSAGE_SENT_TOPIC0 = "ca4cf462dc4787a3aa57636ad2349b8fb4e4f2d2c0ef4ac57f85955f7251a7a8"

# Gas fallbacks used only when ``eth_estimateGas`` is unavailable; the happy path
# always estimates live and applies headroom.
_APPROVE_GAS_DEFAULT = 80_000
_BRIDGE_GAS_DEFAULT = 300_000

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


def encode_approve(spender: Any, amount_base_units: int) -> bytes:
    return encode_call(SEL_APPROVE, _enc_address(spender), _enc_uint256(amount_base_units))


def encode_bridge_to_chia(asset: Any, receiver_ph: Any, mojo_amount: int) -> bytes:
    return encode_call(
        SEL_BRIDGE_TO_CHIA,
        _enc_address(asset),
        _enc_bytes32(receiver_ph),
        _enc_uint256(mojo_amount),
    )


# --------------------------------------------------------------------------- #
# Amount conversions (USDC 6-dec base units <-> CAT 3-dec mojos).
# --------------------------------------------------------------------------- #

def _mojo_factor(net: WarpNet) -> int:
    return 10 ** (net.usdc_decimals - net.cat_decimals)


def mojo_to_base_units(mojo: int, net: WarpNet) -> int:
    """CAT mojos -> ERC20 base units pulled by ``bridgeToChia`` (mojo * factor)."""
    return int(mojo) * _mojo_factor(net)


def base_units_to_mojo(base_units: int, net: WarpNet) -> int:
    """ERC20 base units -> CAT mojos (floor; sub-mojo dust is unbridgeable)."""
    return int(base_units) // _mojo_factor(net)


def bridgeable_mojo(base_units: int, net: WarpNet) -> int:
    """Largest whole CAT mojo amount fundable by ``base_units`` of USDC."""
    return base_units_to_mojo(base_units, net)


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
) -> UnsignedTx:
    """Sign-ready ``approve(bridge, amount)`` on the USDC token contract."""
    data = encode_approve(spender or net.erc20_bridge_address, amount_base_units)
    return UnsignedTx(
        chain_id=net.evm_chain_id,
        nonce=int(nonce),
        to=_addr_bytes(net.usdc_address),
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
    asset: Optional[str] = None,
) -> UnsignedTx:
    """Sign-ready ``bridgeToChia`` on the ERC20Bridge; ``value`` is the exact toll."""
    data = encode_bridge_to_chia(asset or net.usdc_address, receiver_ph, mojo_amount)
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

    def get_bridge_allowance(self, owner: str) -> int:
        return self.get_allowance(self._net.usdc_address, owner, self._net.erc20_bridge_address)

    # -- live protocol parameters ------------------------------------------- #

    def get_message_toll(self) -> int:
        """``Portal.messageToll()`` -- owner-mutable, so always read live."""
        return self._call_uint(self._net.portal_address, SEL_MESSAGE_TOLL)

    def get_tip_bps(self) -> int:
        """``ERC20Bridge.tip()`` in basis points (immutable)."""
        return self._call_uint(self._net.erc20_bridge_address, SEL_TIP)

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

    def prepare_approve(self, *, owner: str, amount_base_units: int, gas: Optional[int] = None) -> UnsignedTx:
        nonce = self.get_nonce(owner)
        fees = self.get_fee_data()
        if gas is None:
            data = encode_approve(self._net.erc20_bridge_address, amount_base_units)
            gas = self.estimate_gas(
                from_address=owner, to=self._net.usdc_address, value=0, data=data,
                default=_APPROVE_GAS_DEFAULT,
            )
        return build_approve_tx(
            self._net, amount_base_units=amount_base_units, nonce=nonce, fees=fees, gas=gas
        )

    def prepare_bridge(
        self,
        *,
        owner: str,
        receiver_ph: Any,
        mojo_amount: int,
        gas: Optional[int] = None,
        toll_wei: Optional[int] = None,
    ) -> UnsignedTx:
        toll = toll_wei if toll_wei is not None else self.get_message_toll()
        nonce = self.get_nonce(owner)
        fees = self.get_fee_data()
        if gas is None:
            data = encode_bridge_to_chia(self._net.usdc_address, receiver_ph, mojo_amount)
            gas = self.estimate_gas(
                from_address=owner, to=self._net.erc20_bridge_address, value=toll, data=data,
                default=_BRIDGE_GAS_DEFAULT,
            )
        return build_bridge_tx(
            self._net, receiver_ph=receiver_ph, mojo_amount=mojo_amount, toll_wei=toll,
            nonce=nonce, fees=fees, gas=gas,
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
