"""Tests for the warp Base (EVM) client.

The client takes an injected JSON-RPC ``caller``, so these tests drive it with
canned responses and never touch the network. The pure helpers (ABI encoding,
tip/fee math, receipt + ``MessageSent`` parsing) need no third-party import; the
selector-verification and signing tests ``importorskip`` ``eth_utils`` /
``eth_account`` so a minimal CI image skips rather than errors.
"""

from __future__ import annotations

import pytest

from gui.services.warp import constants as C
from gui.services.warp import evm

NET = C.MAINNET


# --------------------------------------------------------------------------- #
# Fake JSON-RPC transport (mirrors FakePoster in test_warp_clients.py).
# --------------------------------------------------------------------------- #

class FakeCaller:
    """Records (method, params) and returns a canned response per method.

    A response may be a plain value or a ``callable(params) -> value`` (use a
    callable that raises ``EvmRpcError`` to simulate a node ``error`` object, or
    any other exception to simulate a transport failure).
    """

    def __init__(self, responses: dict) -> None:
        self.responses = responses
        self.calls: list[tuple[str, list]] = []

    def __call__(self, method: str, params: list):
        self.calls.append((method, params))
        if method not in self.responses:
            raise AssertionError(f"unexpected method: {method}")
        r = self.responses[method]
        return r(params) if callable(r) else r

    def params_for(self, method: str) -> list:
        for m, params in self.calls:
            if m == method:
                return params
        raise AssertionError(f"method not called: {method}")


def _client(responses: dict) -> evm.EvmClient:
    return evm.EvmClient(NET, caller=FakeCaller(responses))


def _word(value: int) -> str:
    return "0x" + value.to_bytes(32, "big").hex()


# --------------------------------------------------------------------------- #
# Selectors + event topic anchored to keccak.
# --------------------------------------------------------------------------- #

def test_selectors_and_topic_match_keccak():
    keccak = pytest.importorskip("eth_utils").keccak
    cases = {
        "approve(address,uint256)": evm.SEL_APPROVE,
        "balanceOf(address)": evm.SEL_BALANCE_OF,
        "allowance(address,address)": evm.SEL_ALLOWANCE,
        "decimals()": evm.SEL_DECIMALS,
        "messageToll()": evm.SEL_MESSAGE_TOLL,
        "tip()": evm.SEL_TIP,
        "bridgeToChia(address,bytes32,uint256)": evm.SEL_BRIDGE_TO_CHIA,
        "receiveMessage(bytes32,bytes3,bytes32,address,bytes32[],bytes)": evm.SEL_RECEIVE_MESSAGE,
        "signatureThreshold()": evm.SEL_SIGNATURE_THRESHOLD,
    }
    for sig, selector in cases.items():
        assert keccak(text=sig)[:4] == selector, sig
    topic = keccak(text="MessageSent(bytes32,address,bytes3,bytes32,bytes32[])").hex()
    assert topic == evm.MESSAGE_SENT_TOPIC0


# --------------------------------------------------------------------------- #
# ABI encoding.
# --------------------------------------------------------------------------- #

def test_encode_approve_layout():
    data = evm.encode_approve(NET.erc20_bridge_address, 1_234_000_000)
    assert len(data) == 4 + 32 + 32
    assert data[:4] == evm.SEL_APPROVE
    # spender right-aligned in the first word, amount in the second.
    assert data[4:36] == b"\x00" * 12 + bytes.fromhex(NET.erc20_bridge_address[2:])
    assert int.from_bytes(data[36:68], "big") == 1_234_000_000


def test_encode_bridge_layout_round_trips():
    receiver = bytes.fromhex("ab" * 32)
    data = evm.encode_bridge_to_chia(NET.usdc_address, receiver, 1000)
    assert len(data) == 4 + 32 + 32 + 32
    assert data[:4] == evm.SEL_BRIDGE_TO_CHIA
    assert data[4:36] == b"\x00" * 12 + bytes.fromhex(NET.usdc_address[2:])
    assert data[36:68] == receiver
    assert int.from_bytes(data[68:100], "big") == 1000


def test_enc_address_rejects_bad_length():
    with pytest.raises(evm.EvmError):
        evm._enc_address("0x1234")


def test_enc_bytes32_rejects_bad_length():
    with pytest.raises(evm.EvmError):
        evm._enc_bytes32(b"\x00" * 31)


def test_enc_uint256_rejects_out_of_range():
    with pytest.raises(evm.EvmError):
        evm._enc_uint256(-1)
    with pytest.raises(evm.EvmError):
        evm._enc_uint256(2 ** 256)


# --------------------------------------------------------------------------- #
# Amount + tip math (mirrors the on-chain ERC20Bridge).
# --------------------------------------------------------------------------- #

def test_amount_conversions():
    assert evm.mojo_to_base_units(1000, NET) == 1_000_000       # 1 USDC
    assert evm.base_units_to_mojo(1_000_000, NET) == 1000
    assert evm.bridgeable_mojo(1_500_999, NET) == 1500          # floors sub-mojo dust


def test_post_tip_amount_matches_contract():
    assert evm.post_tip_amount(1000, 30) == 997                 # 0.3% of 1000 = 3
    assert evm.post_tip_amount(100, 30) == 99                   # tip floored up to 1
    with pytest.raises(evm.EvmError):
        evm.post_tip_amount(1, 30)                              # amount <= tip


# --------------------------------------------------------------------------- #
# Fee suggestion + bump.
# --------------------------------------------------------------------------- #

def test_suggest_fees():
    fees = evm.suggest_fees(100, 5)
    assert fees == evm.EIP1559Fees(max_fee_per_gas=205, max_priority_fee_per_gas=5)
    assert evm.suggest_fees(100, 5, base_fee_headroom=3).max_fee_per_gas == 305


def test_bump_fees_vectors_and_cap():
    base = evm.EIP1559Fees(1_000_000_000, 1_000_000)
    assert evm.bump_fees(base, 0) == base                       # first broadcast unchanged
    assert evm.bump_fees(base, 1) == evm.EIP1559Fees(1_300_000_000, 1_300_000)
    assert evm.bump_fees(base, 2) == evm.EIP1559Fees(1_690_000_000, 1_690_000)
    assert evm.bump_fees(base, 3) == evm.EIP1559Fees(2_197_000_000, 2_197_000)
    # Clamped at the cap: attempt 9 == attempt 3.
    assert evm.bump_fees(base, 9) == evm.bump_fees(base, 3)


# --------------------------------------------------------------------------- #
# Receipt + MessageSent parsing.
# --------------------------------------------------------------------------- #

def _message_sent_log(nonce_hex: str, *, address: str = NET.portal_address) -> dict:
    return {
        "address": address,
        "topics": ["0x" + evm.MESSAGE_SENT_TOPIC0, "0x" + nonce_hex],
    }


def test_parse_message_sent_nonce_preserves_leading_zeros():
    nonce = "00" * 30 + "002a"  # 32-byte word for ethNonce == 42
    receipt = {
        "logs": [
            {"address": "0x" + "11" * 20, "topics": ["0xdeadbeef"]},        # unrelated
            _message_sent_log(nonce),
        ]
    }
    assert evm.parse_message_sent_nonce(receipt, NET.portal_address) == nonce


def test_parse_message_sent_nonce_matches_portal_only():
    nonce = "00" * 31 + "07"
    # Right topic0 but emitted by some other contract -> ignored.
    receipt = {"logs": [_message_sent_log(nonce, address="0x" + "22" * 20)]}
    with pytest.raises(evm.EvmError):
        evm.parse_message_sent_nonce(receipt, NET.portal_address)


def test_parse_message_sent_nonce_missing_raises():
    with pytest.raises(evm.EvmError):
        evm.parse_message_sent_nonce({"logs": []}, NET.portal_address)


def test_receipt_status_helpers():
    assert evm.receipt_succeeded({"status": "0x1"})
    assert evm.receipt_reverted({"status": "0x0"})
    assert not evm.receipt_succeeded({"status": "0x0"})
    assert evm.receipt_status({}) is None


def test_confirmations():
    assert evm.confirmations(100, 100) == 1
    assert evm.confirmations(100, 109) == 10
    assert evm.confirmations(None, 100) == 0
    assert evm.confirmations(200, 100) == 0  # head behind (reorg); never negative


# --------------------------------------------------------------------------- #
# Client reads.
# --------------------------------------------------------------------------- #

def test_get_message_toll_reads_portal_live():
    c = _client({"eth_call": _word(NET.message_toll_wei)})
    assert c.get_message_toll() == NET.message_toll_wei
    call = c._caller.params_for("eth_call")
    assert call[0]["to"] == "0x" + NET.portal_address[2:].lower()
    assert call[0]["data"] == "0x" + evm.SEL_MESSAGE_TOLL.hex()


def test_get_allowance_and_balance():
    c = _client({"eth_call": lambda p: _word(500) if p[0]["data"].startswith("0xdd62ed3e") else _word(42)})
    owner = "0x" + "ab" * 20
    assert c.get_bridge_allowance(owner) == 500
    assert c.get_erc20_balance(NET.usdc_address, owner) == 42


def test_get_nonce_and_eth_balance_quantities():
    c = _client({"eth_getTransactionCount": "0x5", "eth_getBalance": "0x2386f26fc10000"})
    assert c.get_nonce("0x" + "ab" * 20) == 5
    assert c.get_eth_balance("0x" + "ab" * 20) == 0x2386F26FC10000
    assert c._caller.params_for("eth_getTransactionCount")[1] == "pending"


def test_eth_call_empty_result_raises():
    c = _client({"eth_call": "0x"})
    with pytest.raises(evm.EvmError):
        c.get_message_toll()


def test_get_fee_data_falls_back_when_priority_unsupported():
    def caller(method, params):
        if method == "eth_getBlockByNumber":
            return {"baseFeePerGas": "0x3b9aca00"}  # 1 gwei
        if method == "eth_maxPriorityFeePerGas":
            raise evm.EvmRpcError("method not found", code=-32601)
        raise AssertionError(method)

    c = evm.EvmClient(NET, caller=caller, default_priority_fee_wei=1_000_000)
    fees = c.get_fee_data()
    assert fees.max_priority_fee_per_gas == 1_000_000
    assert fees.max_fee_per_gas == 2 * 1_000_000_000 + 1_000_000


def test_transport_error_is_wrapped():
    def boom(_method, _params):
        raise ValueError("connection reset")

    c = evm.EvmClient(NET, caller=boom)
    with pytest.raises(evm.EvmError) as ei:
        c.get_block_number()
    assert "eth_blockNumber" in str(ei.value)


# --------------------------------------------------------------------------- #
# Idempotent broadcast.
# --------------------------------------------------------------------------- #

def test_send_raw_returns_node_hash_on_success():
    tx_hash = "0x" + "cd" * 32
    c = _client({"eth_sendRawTransaction": tx_hash})
    assert c.send_raw_transaction(b"\x02\x00\x01") == tx_hash


@pytest.mark.parametrize("message", ["already known", "nonce too low", "known transaction"])
def test_send_raw_benign_errors_are_idempotent(message):
    pytest.importorskip("eth_utils")
    raw = bytes.fromhex("02f8") + b"\x11" * 20

    def caller(_method, _params):
        raise evm.EvmRpcError(message)

    c = evm.EvmClient(NET, caller=caller)
    assert c.send_raw_transaction(raw) == evm.tx_hash_of(raw)


def test_send_raw_real_rejection_raises():
    def caller(_method, _params):
        raise evm.EvmRpcError("insufficient funds for gas * price + value")

    c = evm.EvmClient(NET, caller=caller)
    with pytest.raises(evm.EvmError) as ei:
        c.send_raw_transaction(b"\x02\x00")
    assert "insufficient funds" in str(ei.value)


# --------------------------------------------------------------------------- #
# Tx building + signing.
# --------------------------------------------------------------------------- #

def test_build_bridge_tx_shape():
    fees = evm.EIP1559Fees(10 ** 9, 10 ** 6)
    tx = evm.build_bridge_tx(
        NET, receiver_ph=bytes.fromhex("ab" * 32), mojo_amount=1000,
        toll_wei=NET.message_toll_wei, nonce=7, fees=fees, gas=300_000,
    )
    assert tx.to == bytes.fromhex(NET.erc20_bridge_address[2:])
    assert tx.value == NET.message_toll_wei
    assert tx.data[:4] == evm.SEL_BRIDGE_TO_CHIA
    assert tx.chain_id == NET.evm_chain_id
    # eth_account-shaped dict carries bytes `to` and a hex `data`.
    d = tx.as_dict()
    assert d["type"] == 2 and d["to"] == tx.to and d["data"].startswith("0x")


def test_build_approve_tx_shape():
    fees = evm.EIP1559Fees(10 ** 9, 10 ** 6)
    tx = evm.build_approve_tx(NET, amount_base_units=1_000_000, nonce=0, fees=fees, gas=80_000)
    assert tx.to == bytes.fromhex(NET.usdc_address[2:])
    assert tx.value == 0
    assert tx.data[:4] == evm.SEL_APPROVE


def test_sign_tx_is_deterministic_and_hash_matches_keccak():
    pytest.importorskip("eth_account")
    keccak = pytest.importorskip("eth_utils").keccak
    fees = evm.EIP1559Fees(10 ** 9, 10 ** 6)
    tx = evm.build_bridge_tx(
        NET, receiver_ph=bytes.fromhex("ab" * 32), mojo_amount=1000,
        toll_wei=NET.message_toll_wei, nonce=7, fees=fees, gas=300_000,
    )
    key = bytes(range(1, 33))
    signed = evm.sign_tx(tx, key)
    assert signed.raw[0] == 0x02          # EIP-1559 typed-envelope prefix
    assert signed.nonce == 7
    assert signed.tx_hash == "0x" + keccak(signed.raw).hex()
    # Deterministic (RFC 6979): identical inputs -> identical raw bytes.
    assert evm.sign_tx(tx, key).raw == signed.raw
    assert evm.tx_hash_of(signed.raw) == signed.tx_hash


def test_prepare_bridge_reads_live_toll_nonce_and_fees():
    def caller(method, params):
        return {
            "eth_call": _word(NET.message_toll_wei),                 # messageToll()
            "eth_getTransactionCount": "0x3",
            "eth_getBlockByNumber": {"baseFeePerGas": "0x3b9aca00"},
            "eth_maxPriorityFeePerGas": "0xf4240",                  # 1_000_000
            "eth_estimateGas": "0x30d40",                           # 200_000
        }[method]

    c = evm.EvmClient(NET, caller=caller)
    tx = c.prepare_bridge(owner="0x" + "ab" * 20, receiver_ph=bytes.fromhex("cd" * 32), mojo_amount=1000)
    assert tx.value == NET.message_toll_wei
    assert tx.nonce == 3
    assert tx.gas == 200_000 * 5 // 4          # estimate + 25% headroom
    assert tx.max_priority_fee_per_gas == 1_000_000


# --------------------------------------------------------------------------- #
# Outbound relay encoding (unwrap).
# --------------------------------------------------------------------------- #

def test_receive_message_calldata_matches_an_independent_encoder():
    """Byte-compare the hand-rolled head/tail layout against eth_abi.

    Two independent implementations agreeing is the check; a shape mistake in
    dynamic ABI encoding (offsets, padding) would produce calldata the Portal
    decodes as garbage -- and the relay burns gas to be told so.
    """
    eth_abi = pytest.importorskip("eth_abi")

    nonce = bytes(range(32))
    source = b"\x11" * 32
    dest = "0x" + "ab" * 20
    contents = [b"\x22" * 32, b"\x33" * 32, b"\x44" * 32]
    sigs = bytes(range(65)) * 6                    # threshold 6, v||r||s each

    ours = evm.encode_receive_message(nonce, b"xch", source, dest, contents, sigs)

    reference = evm.SEL_RECEIVE_MESSAGE + eth_abi.encode(
        ["bytes32", "bytes3", "bytes32", "address", "bytes32[]", "bytes"],
        [nonce, b"xch", source, bytes.fromhex("ab" * 20), contents, sigs],
    )
    assert ours == reference


def test_pack_validator_sigs_sorts_ascending_and_packs_v_first():
    """The Portal wants v||r||s in strictly ascending recovered-signer order."""
    lo = "0x" + "01" * 20
    hi = "0x" + "ff" * 20
    r1, s1 = b"\xaa" * 32, b"\xbb" * 32
    r2, s2 = b"\xcc" * 32, b"\xdd" * 32

    packed = evm.pack_validator_sigs([(hi, 28, r2, s2), (lo, 27, r1, s1)])

    assert len(packed) == 130
    assert packed[:65] == bytes([27]) + r1 + s1    # lower address first
    assert packed[65:] == bytes([28]) + r2 + s2
    assert packed[0] == 27, "v sits at +0, not after r||s"


def test_pack_validator_sigs_rejects_duplicates_and_bad_v():
    r, s = b"\x00" * 32, b"\x00" * 32
    with pytest.raises(evm.EvmError, match="duplicate"):
        evm.pack_validator_sigs([("0x" + "01" * 20, 27, r, s),
                                 ("0x" + "01" * 20, 28, r, s)])
    with pytest.raises(evm.EvmError, match="v must be 27 or 28"):
        evm.pack_validator_sigs([("0x" + "01" * 20, 1, r, s)])


def test_already_delivered_classification():
    """Only the Portal's own "!nonce" means delivered; usedNonces is written
    before the bridge call with no try/catch, so bridge-side reverts roll the
    nonce back and the relay stays retryable."""
    assert evm.is_already_delivered(evm.EvmRpcError("execution reverted: !nonce"))
    assert not evm.is_already_delivered(evm.EvmRpcError("execution reverted: !amnt"))
    assert not evm.is_already_delivered(evm.EvmRpcError("execution reverted: !sig"))


def test_message_type_hash_matches_keccak():
    keccak = pytest.importorskip("eth_utils").keccak
    assert evm.MESSAGE_TYPE_HASH == keccak(
        text="Message(bytes32 nonce,bytes3 source_chain,bytes32 source,"
             "address destination,bytes32[] contents)"
    )


def test_validator_message_digest_shape():
    """Structural checks; the end-to-end proof against a real delivered
    message's recovered signatures lives with the collector tests."""
    keccak = pytest.importorskip("eth_utils").keccak
    sep = b"\x77" * 32
    d1 = evm.validator_message_digest(
        sep, b"\x01" * 32, b"xch", b"\x02" * 32, "0x" + "ab" * 20, [b"\x03" * 32]
    )
    assert len(d1) == 32
    # Any field change must change the digest.
    d2 = evm.validator_message_digest(
        sep, b"\x01" * 32, b"xch", b"\x02" * 32, "0x" + "ab" * 20, [b"\x04" * 32]
    )
    assert d1 != d2
    # And the 0x1901 prefix binds the domain separator.
    d3 = evm.validator_message_digest(
        b"\x78" * 32, b"\x01" * 32, b"xch", b"\x02" * 32, "0x" + "ab" * 20, [b"\x03" * 32]
    )
    assert d1 != d3
