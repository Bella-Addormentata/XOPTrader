"""Base hot-wallet management: create, send, rotate -- the Coinbase on-ramp.

The hot wallet is the intermediary between an exchange and the bridge: ETH and
USDC come in from Coinbase, the warp engine spends them, unwraps land back
here, and the surplus goes out to Coinbase. This module gives that wallet an
operable surface without ever connecting an external signer -- keys stay
DPAPI-wrapped in secrets.yaml and sign locally, same as the bridge.

Rules that carry scars from the bridge work:

* **Rotation archives, never deletes.** An in-flight unwrap or a Coinbase
  deposit sent after rotation lands at the OLD address; a destroyed key is
  destroyed money. Retired blobs stay in ``warp.retired_keys`` forever.
* **Rotation refuses while any warp job is open.** Jobs are frozen against
  the hot wallet; the binding guard would (correctly) strand them.
* **The sweep reserves both transactions' worst-case gas up front.** USDC
  moves first, then ETH minus the reserved costs; sub-reserve dust stays at
  the archived key rather than risking an underfunded sweep.
* **EIP-55 on every destination**, same rule as the unwrap receiver: mixed
  case must checksum; all-lower/all-upper carry no checksum information.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, List, Optional

_log = logging.getLogger(__name__)

_ETH_TRANSFER_GAS = 21_000
_ERC20_TRANSFER_GAS_DEFAULT = 80_000


class BaseWalletError(RuntimeError):
    """A wallet operation refused or failed; the message is operator-facing."""


def validate_destination(address: str) -> str:
    """The unwrap receiver's validation rules, reused verbatim in spirit."""
    address = str(address).strip()
    if not (address.startswith("0x") and len(address) == 42):
        raise BaseWalletError("destination must be a 0x-prefixed 20-byte address")
    try:
        raw = bytes.fromhex(address[2:])
    except ValueError as exc:
        raise BaseWalletError("destination is not valid hex") from exc
    if raw == b"\x00" * 20:
        raise BaseWalletError("destination is the zero address")
    hex_part = address[2:]
    if hex_part != hex_part.lower() and hex_part != hex_part.upper():
        from eth_utils import is_checksum_address

        if not is_checksum_address(address):
            raise BaseWalletError(
                "destination mixed-case checksum (EIP-55) is invalid -- "
                "a typo would send the funds to a stranger"
            )
    return address


@dataclass(frozen=True)
class WalletInfo:
    address: str
    eth_wei: int
    usdc_micros: int
    retired_count: int
    backup_confirmed: bool


class BaseWallet:
    """One active DPAPI key plus its retired ancestors, over the shared client.

    ``secrets_io`` is an object with ``read() -> dict`` and ``write(dict)``
    (injected so tests never touch the real secrets.yaml); the live wiring
    uses the GUI's comment-preserving YAML round-trip.
    """

    def __init__(self, net: Any, evm_client: Any, secrets_io: Any,
                 *, protector: Any = None) -> None:
        self._net = net
        self._evm = evm_client
        self._io = secrets_io
        self._protector = protector

    # -- key material -------------------------------------------------------- #

    def _warp_section(self, secrets: dict) -> dict:
        return secrets.setdefault("warp", {})

    def active_key(self):
        from .warp import keystore

        blob = (self._io.read().get("warp") or {}).get("evm_private_key_dpapi")
        if not blob:
            raise BaseWalletError("no active wallet key in secrets.yaml")
        return keystore.load_evm_key(blob, protector=self._protector)

    def create_wallet(self) -> str:
        """Generate the first active key; refuses to overwrite an existing one.

        Returns the address only. The private key is never returned to the
        GUI layer -- the operator's offline backup comes from the runbook's
        console flow, and ``mark_backup_confirmed`` records that it happened.
        """
        from .warp import keystore

        secrets = self._io.read()
        warp = self._warp_section(secrets)
        if warp.get("evm_private_key_dpapi"):
            raise BaseWalletError(
                "an active wallet already exists; use rotation, which archives "
                "it -- overwriting a key is destroying money"
            )
        key = keystore.new_evm_key()
        warp["evm_private_key_dpapi"] = keystore.protect_evm_key(
            key, protector=self._protector
        )
        warp["evm_key_backup_confirmed"] = False
        self._io.write(secrets)
        _log.info("base wallet created: %s", key.address)
        return key.address

    def mark_backup_confirmed(self) -> None:
        secrets = self._io.read()
        self._warp_section(secrets)["evm_key_backup_confirmed"] = True
        self._io.write(secrets)

    # -- read side ----------------------------------------------------------- #

    def info(self) -> WalletInfo:
        key = self.active_key()
        secrets = self._io.read().get("warp") or {}
        return WalletInfo(
            address=key.address,
            eth_wei=int(self._evm.get_eth_balance(key.address)),
            usdc_micros=int(
                self._evm.get_erc20_balance(self._net.usdc_address, key.address)
            ),
            retired_count=len(secrets.get("retired_keys") or []),
            backup_confirmed=bool(secrets.get("evm_key_backup_confirmed")),
        )

    # -- transfers ----------------------------------------------------------- #

    def _prepare(self, key, *, to: str, value: int, data: bytes, gas: int,
                 nonce: Optional[int] = None):
        from .warp import evm

        n = self._evm.get_nonce(key.address) if nonce is None else nonce
        fees = self._evm.get_fee_data()
        return evm.UnsignedTx(
            chain_id=self._net.evm_chain_id, nonce=int(n),
            to=bytes.fromhex(to[2:]), value=int(value), data=data,
            gas=int(gas), max_fee_per_gas=fees.max_fee_per_gas,
            max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
        )

    def send_eth(self, destination: str, amount_wei: int) -> str:
        from .warp import evm

        destination = validate_destination(destination)
        key = self.active_key()
        amount_wei = int(amount_wei)
        if amount_wei <= 0:
            raise BaseWalletError("amount must be positive")
        unsigned = self._prepare(key, to=destination, value=amount_wei,
                                 data=b"", gas=_ETH_TRANSFER_GAS)
        cost = amount_wei + unsigned.gas * unsigned.max_fee_per_gas
        if cost > self._evm.get_eth_balance(key.address):
            raise BaseWalletError("amount + worst-case gas exceeds the balance")
        signed = evm.sign_tx(unsigned, key.private_key)
        self._evm.send_raw_transaction(signed.raw)
        return signed.tx_hash

    def send_usdc(self, destination: str, amount_micros: int) -> str:
        from .warp import evm

        destination = validate_destination(destination)
        key = self.active_key()
        amount_micros = int(amount_micros)
        if amount_micros <= 0:
            raise BaseWalletError("amount must be positive")
        held = int(self._evm.get_erc20_balance(self._net.usdc_address, key.address))
        if amount_micros > held:
            raise BaseWalletError(
                f"amount {amount_micros} exceeds the USDC balance {held}"
            )
        data = evm.encode_transfer(destination, amount_micros)
        gas = self._evm.estimate_gas(
            from_address=key.address, to=self._net.usdc_address, value=0,
            data=data, default=_ERC20_TRANSFER_GAS_DEFAULT,
        )
        unsigned = self._prepare(key, to=self._net.usdc_address, value=0,
                                 data=data, gas=gas)
        signed = evm.sign_tx(unsigned, key.private_key)
        self._evm.send_raw_transaction(signed.raw)
        return signed.tx_hash

    # -- rotation ------------------------------------------------------------ #

    def rotate(self, *, open_job_check) -> dict:
        """New key; sweep USDC then ETH to it; archive the old blob.

        ``open_job_check()`` must return the open warp job or ``None`` --
        injected because the wallet must not import the job store. Both sweep
        transactions are signed against consecutive nonces with worst-case gas
        reserved up front; whatever dust the reserve strands stays at the
        archived key, which is recoverable, unlike an underfunded sweep.
        """
        from .warp import evm, keystore

        open_job = open_job_check()
        if open_job is not None:
            raise BaseWalletError(
                f"cannot rotate: warp job {getattr(open_job, 'id', '?')} is "
                "open and frozen against the current key; resolve it first"
            )

        old = self.active_key()
        eth = int(self._evm.get_eth_balance(old.address))
        usdc = int(self._evm.get_erc20_balance(self._net.usdc_address, old.address))

        new_key = keystore.new_evm_key()
        txs: List[str] = []
        nonce = self._evm.get_nonce(old.address)
        fees = self._evm.get_fee_data()
        reserved = 0

        usdc_unsigned = None
        if usdc > 0:
            data = evm.encode_transfer(new_key.address, usdc)
            gas = self._evm.estimate_gas(
                from_address=old.address, to=self._net.usdc_address, value=0,
                data=data, default=_ERC20_TRANSFER_GAS_DEFAULT,
            )
            usdc_unsigned = evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(self._net.usdc_address[2:]), value=0,
                data=data, gas=gas, max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            )
            reserved += gas * fees.max_fee_per_gas
            nonce += 1

        eth_amount = eth - reserved - _ETH_TRANSFER_GAS * fees.max_fee_per_gas
        eth_unsigned = None
        if eth_amount > 0:
            eth_unsigned = evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(new_key.address[2:]), value=eth_amount,
                data=b"", gas=_ETH_TRANSFER_GAS,
                max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            )

        # Persist the NEW key and archive the old BEFORE broadcasting: a crash
        # after the sweep with only the old key on disk loses nothing, but a
        # crash after broadcasting with only the old key recorded would leave
        # the funds at an address whose key was never saved.
        secrets = self._io.read()
        warp = self._warp_section(secrets)
        warp.setdefault("retired_keys", []).append({
            "blob": warp["evm_private_key_dpapi"],
            "address": old.address,
            "retired_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        })
        warp["evm_private_key_dpapi"] = keystore.protect_evm_key(
            new_key, protector=self._protector
        )
        warp["evm_key_backup_confirmed"] = False
        self._io.write(secrets)

        for unsigned in (usdc_unsigned, eth_unsigned):
            if unsigned is None:
                continue
            signed = evm.sign_tx(unsigned, old.private_key)
            self._evm.send_raw_transaction(signed.raw)
            txs.append(signed.tx_hash)

        _log.info("rotated hot wallet %s -> %s (%d sweep txs)",
                  old.address, new_key.address, len(txs))
        return {"old_address": old.address, "new_address": new_key.address,
                "sweep_txs": txs, "swept_usdc_micros": usdc,
                "swept_eth_wei": max(0, eth_amount)}
