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
        self.sent: list = []
        self.fees = SimpleNamespace(max_fee_per_gas=1_000_000_000,
                                    max_priority_fee_per_gas=1_000_000)

    def get_eth_balance(self, addr):
        return self.eth

    def get_erc20_balance(self, token, addr):
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
