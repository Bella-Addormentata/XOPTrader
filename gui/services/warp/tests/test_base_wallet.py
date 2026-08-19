"""Base hot-wallet management: the money math and the refusal rails."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services import basewallet  # noqa: E402
from gui.services.basewallet import BaseWallet, BaseWalletError  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import keystore  # noqa: E402

NET = C.MAINNET


class MemSecrets:
    def __init__(self) -> None:
        self.data: dict = {}
        self.writes = 0

    def read(self) -> dict:
        import copy

        return copy.deepcopy(self.data)

    def write(self, d: dict) -> None:
        import copy

        self.data = copy.deepcopy(d)
        self.writes += 1


class FakeEvm:
    def __init__(self) -> None:
        self.eth = 10 ** 16               # 0.01 ETH
        self.usdc = 5_000_000             # 5 USDC
        self.millieth = 2_500             # 2.5 milliETH (3-decimal units)
        self.sent: list = []
        self.fees = SimpleNamespace(max_fee_per_gas=1_000_000_000,
                                    max_priority_fee_per_gas=1_000_000)

    def get_eth_balance(self, addr):
        return self.eth

    def get_erc20_balance(self, token, addr):
        if str(token).lower() == C.MAINNET.milli_eth_address.lower():
            return self.millieth
        return self.usdc

    def get_nonce(self, addr, *, pending=True):
        return 7

    def get_fee_data(self, **kw):
        return self.fees

    def estimate_gas(self, **kw):
        return 60_000

    def send_raw_transaction(self, raw):
        self.sent.append(bytes(raw))
        return "0x" + "aa" * 32


class NullProtector:
    def protect(self, data: bytes, entropy: bytes = b"") -> bytes:
        return b"NP" + data

    def unprotect(self, data: bytes, entropy: bytes = b"") -> bytes:
        assert data[:2] == b"NP"
        return data[2:]


def _wallet(*, with_key=True):
    io, ev = MemSecrets(), FakeEvm()
    prot = NullProtector()
    w = BaseWallet(NET, ev, io, protector=prot)
    if with_key:
        w.create_wallet()
    return w, io, ev


# --------------------------------------------------------------------------- #
# Creation and the backup nag.
# --------------------------------------------------------------------------- #

def test_create_refuses_to_overwrite_an_existing_key():
    w, io, _ = _wallet()
    with pytest.raises(BaseWalletError, match="destroying money"):
        w.create_wallet()
    assert io.data["warp"]["evm_key_backup_confirmed"] is False
    w.mark_backup_confirmed()
    assert io.data["warp"]["evm_key_backup_confirmed"] is True


def test_info_reads_balances_for_the_active_address():
    w, _io, ev = _wallet()
    info = w.info()
    assert info.eth_wei == ev.eth and info.usdc_micros == ev.usdc
    assert info.address.startswith("0x") and not info.backup_confirmed


def test_recovery_key_is_a_scrub_capable_copy_of_the_active_key():
    w, _io, _ev = _wallet()
    active = w.active_key()

    address, recovery = w.recovery_key()

    assert address == active.address
    assert isinstance(recovery, bytearray)
    assert bytes(recovery) == active.private_key
    recovery[:] = b"\x00" * len(recovery)
    assert w.active_key().private_key == active.private_key


# --------------------------------------------------------------------------- #
# Transfers: validation and balance rails.
# --------------------------------------------------------------------------- #

def test_destination_validation_mirrors_the_unwrap_rules():
    for bad, why in [("0x1234", "20-byte"), ("0x" + "00" * 20, "zero"),
                     ("0xAb" + "cd" * 19, "EIP-55")]:
        with pytest.raises(BaseWalletError, match=why):
            basewallet.validate_destination(bad)
    # No checksum information in uniform case.
    assert basewallet.validate_destination("0x" + "ab" * 20)
    assert basewallet.validate_destination("0x" + "AB" * 20)


def test_send_eth_reserves_worst_case_gas():
    w, _io, ev = _wallet()
    ev.eth = 21_000 * 1_000_000_000 + 100          # gas budget + 100 wei
    with pytest.raises(BaseWalletError, match="worst-case gas"):
        w.send_eth("0x" + "ab" * 20, 200)
    tx = w.send_eth("0x" + "ab" * 20, 100)
    assert tx.startswith("0x") and len(ev.sent) == 1


def test_send_usdc_checks_the_balance_and_moves_nothing_on_refusal():
    w, _io, ev = _wallet()
    with pytest.raises(BaseWalletError, match="exceeds the USDC balance"):
        w.send_usdc("0x" + "ab" * 20, ev.usdc + 1)
    assert ev.sent == []
    w.send_usdc("0x" + "ab" * 20, ev.usdc)
    assert len(ev.sent) == 1


# --------------------------------------------------------------------------- #
# Rotation.
# --------------------------------------------------------------------------- #

def test_rotation_refuses_while_a_warp_job_is_open():
    w, io, ev = _wallet()
    with pytest.raises(BaseWalletError, match="frozen against the current key"):
        w.rotate(open_job_check=lambda: SimpleNamespace(id=9))
    assert ev.sent == [] and len(io.data["warp"].get("retired_keys") or []) == 0


def test_rotation_archives_sweeps_and_reserves_gas():
    w, io, ev = _wallet()
    old_addr = w.active_key().address

    out = w.rotate(open_job_check=lambda: None)

    # Old key archived, never deleted; new key active; backup nag reset.
    retired = io.data["warp"]["retired_keys"]
    assert len(retired) == 1 and retired[0]["address"] == old_addr
    assert w.active_key().address == out["new_address"] != old_addr
    assert io.data["warp"]["evm_key_backup_confirmed"] is False

    # Two sweeps: USDC first, then ETH minus BOTH worst-case gas reserves.
    assert len(out["sweep_txs"]) == 2 and len(ev.sent) == 2
    assert out["swept_usdc_micros"] == ev.usdc
    reserve = 60_000 * 1_000_000_000 + 21_000 * 1_000_000_000
    assert out["swept_eth_wei"] == ev.eth - reserve


def test_rotation_persists_the_new_key_before_broadcasting():
    """A crash after the sweep with only the old key recorded would leave the
    funds at an address whose key was never saved -- the write must precede
    the broadcasts."""
    w, io, ev = _wallet()
    order: list = []
    real_write, real_send = io.write, ev.send_raw_transaction
    io.write = lambda d: (order.append("write"), real_write(d))[1]
    ev.send_raw_transaction = lambda raw: (order.append("send"), real_send(raw))[1]

    w.rotate(open_job_check=lambda: None)

    assert order.index("write") < order.index("send")


def test_rotation_with_dust_only_skips_the_eth_sweep():
    w, io, ev = _wallet()
    ev.usdc = 0
    ev.eth = 1000                                   # far below any gas reserve
    out = w.rotate(open_job_check=lambda: None)
    assert out["sweep_txs"] == [] and out["swept_eth_wei"] == 0
    assert len(io.data["warp"]["retired_keys"]) == 1
# --------------------------------------------------------------------------- #
# ETH <-> milliETH conversion (MilliETH.sol wrapper).
# --------------------------------------------------------------------------- #

def test_millieth_encoders_are_selector_anchored():
    """WETH-style selectors, pinned so a refactor cannot silently re-derive
    them wrong: deposit() = d0e30db0, withdraw(uint256) = 2e1a7d4d."""
    from gui.services.warp import evm

    assert evm.encode_millieth_deposit() == bytes.fromhex("d0e30db0")
    data = evm.encode_millieth_withdraw(1_234)
    assert data[:4] == bytes.fromhex("2e1a7d4d")
    assert len(data) == 4 + 32
    assert int.from_bytes(data[4:], "big") == 1_234


def test_wrap_eth_enforces_the_contract_granularity():
    """MilliETH.deposit() reverts on amounts not divisible by 1e12 wei;
    the wallet refuses first, and nothing is broadcast."""
    w, _io, ev = _wallet()
    with pytest.raises(BaseWalletError, match="granularity"):
        w.wrap_eth(10 ** 12 + 1)
    assert ev.sent == []
    tx = w.wrap_eth(2 * 10 ** 12)
    assert tx.startswith("0x") and len(ev.sent) == 1


def test_wrap_eth_never_wraps_the_relay_gas_reserve():
    """Wrapping the gas floor into an ERC-20 would strand the wallet unable
    to move anything -- including unwrapping the milliETH back."""
    w, _io, ev = _wallet()
    # Balance covers the amount + worst-case gas but NOT the reserve on top.
    amount = 5 * 10 ** 12
    gas_cost = 60_000 * ev.fees.max_fee_per_gas
    ev.eth = amount + gas_cost + 100
    with pytest.raises(BaseWalletError, match="relay-gas reserve"):
        w.wrap_eth(amount, reserve_wei=200)
    assert ev.sent == []
    # With the reserve satisfied the same wrap goes through.
    ev.eth = amount + gas_cost + 200
    assert w.wrap_eth(amount, reserve_wei=200).startswith("0x")


def test_wrap_eth_refuses_nonpositive_amounts():
    w, _io, ev = _wallet()
    for bad in (0, -(10 ** 12)):
        with pytest.raises(BaseWalletError, match="positive"):
            w.wrap_eth(bad)
    assert ev.sent == []


def test_unwrap_millieth_checks_the_token_balance():
    w, _io, ev = _wallet()
    with pytest.raises(BaseWalletError, match="exceeds the milliETH balance"):
        w.unwrap_millieth(ev.millieth + 1)
    assert ev.sent == []
    tx = w.unwrap_millieth(ev.millieth)
    assert tx.startswith("0x") and len(ev.sent) == 1


def test_unwrap_millieth_needs_gas_headroom():
    """The refund replenishes gas, but THIS transaction still needs to be
    payable up front."""
    w, _io, ev = _wallet()
    ev.eth = 0
    with pytest.raises(BaseWalletError, match="worst-case gas"):
        w.unwrap_millieth(100)
    assert ev.sent == []


def test_info_reports_the_millieth_balance():
    w, _io, ev = _wallet()
    info = w.info()
    assert info.millieth_units == ev.millieth
    assert info.usdc_micros == ev.usdc, "USDC read must stay token-keyed"

