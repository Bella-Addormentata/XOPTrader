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
        self.unmined = 0                  # broadcast-but-unmined txs (see get_nonce)
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
        # self.unmined models broadcast-but-unmined txs: the pending tag
        # sees them, the latest tag does not. An int applies to every
        # address; a dict is per-address (negative = incoherent reading).
        u = (self.unmined.get(addr, 0) if isinstance(self.unmined, dict)
             else self.unmined)
        return 7 + (u if pending else 0)

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

    # Three sweeps: milliETH, then USDC, then ETH minus ALL worst-case
    # gas reserves (two ERC-20 transfers + the ETH transfer).
    assert len(out["sweep_txs"]) == 3 and len(ev.sent) == 3
    assert out["swept_usdc_micros"] == ev.usdc
    assert out["swept_millieth_units"] == ev.millieth
    reserve = 2 * 60_000 * 1_000_000_000 + 21_000 * 1_000_000_000
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
    ev.millieth = 0
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


def test_rotation_refuses_pre_swap_when_millieth_gas_is_unaffordable():
    """Wrap-then-rotate with near-zero ETH is exactly the strand the
    refuse-before-the-key-swap rule exists for: archiving the key first
    would leave the whole milliETH balance at an address the GUI can no
    longer spend from."""
    w, io, ev = _wallet()
    ev.usdc = 0
    ev.eth = 1000                     # cannot pay the milliETH sweep's gas
    assert ev.millieth > 0
    with pytest.raises(BaseWalletError, match="milliETH"):
        w.rotate(open_job_check=lambda: None)
    assert io.data["warp"].get("retired_keys") in (None, []), \
        "the key swap must NOT have happened"
    assert ev.sent == []


def test_retired_balances_and_recovery_carry_millieth():
    w, io, ev = _wallet()
    w.rotate(open_job_check=lambda: None)           # archives key #1

    rows = w.retired_balances()
    assert rows and rows[0]["millieth_units"] == ev.millieth

    out = w.recover_retired(rows[0]["address"])
    assert out["swept_millieth_units"] == ev.millieth
    # milliETH sweep + USDC sweep + ETH sweep from the retired key.
    assert len(out["sweep_txs"]) == 3


def test_recovery_preflights_all_sweeps_before_broadcasting_any():
    """The retired key can afford the milliETH sweep's gas but not the
    USDC sweep's on top of it: recovery must refuse WITHOUT broadcasting
    the milliETH transfer. A partial sweep reported as failure invites a
    retry that reads stale balances against the pending tx and enqueues
    a duplicate transfer at the next nonce."""
    w, io, ev = _wallet()
    w.rotate(open_job_check=lambda: None)
    old_addr = w.retired_balances()[0]["address"]
    ev.sent.clear()
    ev.usdc = 3_000_000
    ev.millieth = 2_500
    # Exactly ONE ERC-20 sweep's worth of gas -- not two.
    ev.eth = 60_000 * ev.fees.max_fee_per_gas
    with pytest.raises(BaseWalletError, match="USDC"):
        w.recover_retired(old_addr)
    assert ev.sent == [], "nothing may broadcast when the full preflight fails"


def test_money_moving_actions_refuse_while_a_wallet_tx_is_unmined():
    """Between broadcast and mining, latest-block balances lie (the GUI
    re-enables actions as soon as a tx broadcasts). A wrap-then-rotate in
    that window would read zero milliETH and archive the key the tokens
    are about to land on; two quick wraps would both validate against the
    same ETH balance and cumulatively breach the relay-gas floor. The
    pending-vs-latest nonce gap must refuse all of it, statelessly."""
    w, io, ev = _wallet()
    ev.unmined = 1
    dest = "0x" + "ab" * 20
    for label, act in (
        ("wrap", lambda: w.wrap_eth(10 ** 12)),
        ("unwrap", lambda: w.unwrap_millieth(1)),
        ("rotate", lambda: w.rotate(open_job_check=lambda: None)),
        ("send_eth", lambda: w.send_eth(dest, 10 ** 12)),
        ("send_usdc", lambda: w.send_usdc(dest, 1)),
    ):
        with pytest.raises(BaseWalletError, match="not yet mined"):
            act()
    assert ev.sent == [], "nothing may broadcast on stale balances"
    assert (io.data["warp"].get("retired_keys") or []) == [], \
        "the key swap must not have happened"


def test_recovery_refuses_while_the_retired_key_has_unmined_txs():
    """A recovery retried while a prior attempt's sweep is still pending
    would read stale balances and enqueue duplicate transfers -- the
    settled-state guard closes that window too."""
    w, io, ev = _wallet()
    w.rotate(open_job_check=lambda: None)
    addr = w.retired_balances()[0]["address"]
    ev.sent.clear()
    ev.unmined = 1
    with pytest.raises(BaseWalletError, match="not yet mined"):
        w.recover_retired(addr)
    assert ev.sent == []


def test_rotate_refuses_while_a_retired_key_recovery_is_settling():
    """recover_retired() sweeps INTO the active key without bumping its
    nonce; rotating before those sweeps mine would archive the active key
    and re-strand the recovered funds on it. Rotation must watch the
    retired keys' outgoing nonces too."""
    w, io, ev = _wallet()
    out = w.rotate(open_job_check=lambda: None)     # retires key #1
    retired = out["old_address"]
    ev.sent.clear()
    ev.unmined = {retired: 1}                       # a recovery in flight
    with pytest.raises(BaseWalletError, match="still settling"):
        w.rotate(open_job_check=lambda: None)
    assert ev.sent == []
    assert len(io.data["warp"]["retired_keys"]) == 1, \
        "the second key swap must not have happened"


def test_an_incoherent_nonce_reading_fails_closed():
    """A provider answering the pending tag with null (coerced to 0)
    would make the gap negative and silently disarm the guard forever;
    it must refuse instead."""
    w, io, ev = _wallet()
    ev.unmined = {w.active_key().address: -1}
    with pytest.raises(BaseWalletError, match="incoherent"):
        w.wrap_eth(10 ** 12)
    assert ev.sent == []
