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
from contextlib import nullcontext
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, List, Optional

_log = logging.getLogger(__name__)


def _io_transaction(secrets_io: Any):
    """The secrets file's read-modify-write lock, or a no-op for fakes.

    Real wiring (``_SecretsFileIO``) exposes ``transaction()`` -> the shared
    per-file lock; injected test doubles need not, and run single-threaded."""
    txn = getattr(secrets_io, "transaction", None)
    return txn() if callable(txn) else nullcontext()

_ETH_TRANSFER_GAS = 21_000
_ERC20_TRANSFER_GAS_DEFAULT = 80_000
#: deposit()/withdraw() on MilliETH: mint/burn + ETH transfer; estimated
#: first, this is only the estimator fallback.
_MILLIETH_GAS_DEFAULT = 90_000


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
    millieth_units: int  # MilliETH ERC-20, 3-decimal units
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

        with _io_transaction(self._io):
            secrets = self._io.read()
            warp = self._warp_section(secrets)
            if warp.get("evm_private_key_dpapi"):
                raise BaseWalletError(
                    "an active wallet already exists; use rotation, which "
                    "archives it -- overwriting a key is destroying money"
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
        with _io_transaction(self._io):
            secrets = self._io.read()
            self._warp_section(secrets)["evm_key_backup_confirmed"] = True
            self._io.write(secrets)

    def recovery_key(self) -> tuple[str, bytearray]:
        """Return the active address and a scrub-capable private-key copy.

        This is the only backend path used by the GUI backup dialog. The
        caller must keep the key out of snapshots and logs, then overwrite the
        returned bytearray after the modal closes.
        """
        key = self.active_key()
        return key.address, bytearray(key.private_key)

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
            millieth_units=int(
                self._evm.get_erc20_balance(
                    self._net.milli_eth_address, key.address
                )
            ),
            retired_count=len(secrets.get("retired_keys") or []),
            backup_confirmed=bool(secrets.get("evm_key_backup_confirmed")),
        )

    def erc20_balance(self, token_address: str, holder: str) -> int:
        """One raw ERC-20 balance read. Touches no key material.

        Exists so summary paths that already KNOW the address can read a
        single token balance without info()'s full sweep (secrets read +
        DPAPI + ETH + USDC + milliETH RPCs).
        """
        return int(self._evm.get_erc20_balance(token_address, holder))

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

    # -- ETH <-> milliETH conversion (MilliETH.sol, no fees) ------------------ #

    #: MilliETH's conversion granularity: deposit() reverts unless msg.value
    #: is a multiple of 1e12 wei (18-decimal ETH -> 6 effective decimals:
    #: the 1000x ratio plus the token's own 3 decimals).
    WRAP_GRANULARITY_WEI = 10 ** 12

    def wrap_eth(self, amount_wei: int, *, reserve_wei: int = 0) -> str:
        """ETH -> milliETH via ``MilliETH.deposit()`` (1000/ETH, no fee).

        ``reserve_wei`` is the ETH that must REMAIN after amount plus
        worst-case gas: the relay-gas floor.  Wrapping the gas reserve into
        an ERC-20 would strand the wallet unable to move anything at all --
        including unwrapping the milliETH back.
        """
        from .warp import evm

        key = self.active_key()
        amount_wei = int(amount_wei)
        if amount_wei <= 0:
            raise BaseWalletError("amount must be positive")
        if amount_wei % self.WRAP_GRANULARITY_WEI:
            raise BaseWalletError(
                "amount must be a multiple of 0.000001 ETH (1e12 wei) -- "
                "MilliETH's conversion granularity; the contract reverts "
                "on anything finer"
            )
        if not getattr(self._net, "milli_eth_address", ""):
            raise BaseWalletError("milliETH is not configured on this network")
        data = evm.encode_millieth_deposit()
        gas = self._evm.estimate_gas(
            from_address=key.address, to=self._net.milli_eth_address,
            value=amount_wei, data=data, default=_MILLIETH_GAS_DEFAULT,
        )
        unsigned = self._prepare(key, to=self._net.milli_eth_address,
                                 value=amount_wei, data=data, gas=gas)
        cost = amount_wei + unsigned.gas * unsigned.max_fee_per_gas
        balance = int(self._evm.get_eth_balance(key.address))
        if balance - cost < int(reserve_wei):
            raise BaseWalletError(
                f"wrapping {amount_wei} wei would leave less than the "
                f"{reserve_wei} wei relay-gas reserve (balance {balance}, "
                "worst-case cost includes gas); wrap less or top up"
            )
        signed = evm.sign_tx(unsigned, key.private_key)
        self._evm.send_raw_transaction(signed.raw)
        return signed.tx_hash

    def unwrap_millieth(self, amount_units: int) -> str:
        """milliETH -> ETH via ``withdraw(uint256)`` (units are 3-decimal).

        The refund replenishes the gas reserve, so no reserve gate here --
        only that the wallet can pay THIS transaction's worst-case gas.
        """
        from .warp import evm

        key = self.active_key()
        amount_units = int(amount_units)
        if amount_units <= 0:
            raise BaseWalletError("amount must be positive")
        if not getattr(self._net, "milli_eth_address", ""):
            raise BaseWalletError("milliETH is not configured on this network")
        held = int(self._evm.get_erc20_balance(
            self._net.milli_eth_address, key.address))
        if amount_units > held:
            raise BaseWalletError(
                f"amount {amount_units} exceeds the milliETH balance {held}"
            )
        data = evm.encode_millieth_withdraw(amount_units)
        gas = self._evm.estimate_gas(
            from_address=key.address, to=self._net.milli_eth_address,
            value=0, data=data, default=_MILLIETH_GAS_DEFAULT,
        )
        unsigned = self._prepare(key, to=self._net.milli_eth_address,
                                 value=0, data=data, gas=gas)
        if unsigned.gas * unsigned.max_fee_per_gas > int(
                self._evm.get_eth_balance(key.address)):
            raise BaseWalletError("worst-case gas exceeds the ETH balance")
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
        # Wrapping introduced a third asset; a rotation that ignored it
        # would archive the key with its whole milliETH balance stranded
        # and invisible in-app.
        millieth = int(self._evm.get_erc20_balance(
            self._net.milli_eth_address, old.address))

        new_key = keystore.new_evm_key()
        txs: List[str] = []
        nonce = self._evm.get_nonce(old.address)
        fees = self._evm.get_fee_data()
        reserved = 0

        millieth_unsigned = None
        if millieth > 0:
            m_data = evm.encode_transfer(new_key.address, millieth)
            m_gas = self._evm.estimate_gas(
                from_address=old.address, to=self._net.milli_eth_address,
                value=0, data=m_data, default=_ERC20_TRANSFER_GAS_DEFAULT,
            )
            m_gas_cost = m_gas * fees.max_fee_per_gas
            # Same refuse-before-the-key-swap rule as USDC below: an
            # underfunded sweep after the swap strands the balance at an
            # address the GUI can no longer spend from.
            if eth < reserved + m_gas_cost:
                raise BaseWalletError(
                    f"cannot rotate: sweeping {millieth} milliETH units "
                    f"needs ~{m_gas_cost} wei of gas but the wallet holds "
                    f"only {eth} wei of ETH. Fund the wallet with a little "
                    "Base ETH first (or unwrap some milliETH), then rotate."
                )
            millieth_unsigned = evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(self._net.milli_eth_address[2:]), value=0,
                data=m_data, gas=m_gas,
                max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            )
            reserved += m_gas_cost
            nonce += 1

        usdc_unsigned = None
        if usdc > 0:
            data = evm.encode_transfer(new_key.address, usdc)
            gas = self._evm.estimate_gas(
                from_address=old.address, to=self._net.usdc_address, value=0,
                data=data, default=_ERC20_TRANSFER_GAS_DEFAULT,
            )
            usdc_gas_cost = gas * fees.max_fee_per_gas
            # Refuse BEFORE the irreversible key swap if the wallet cannot pay
            # the USDC sweep's gas: otherwise we would archive the old key,
            # then have the node reject the sweep for insufficient gas, leaving
            # the whole USDC balance stranded at an address the GUI can no
            # longer spend from. A wallet with USDC but ~no ETH (the state
            # unwraps leave behind) is exactly this case.
            if eth < reserved + usdc_gas_cost:
                raise BaseWalletError(
                    f"cannot rotate: sweeping {usdc} USDC needs ~{usdc_gas_cost} "
                    f"wei of gas (on top of {reserved} already reserved) but "
                    f"the wallet holds only {eth} wei of ETH. Fund the wallet "
                    "with a little Base ETH first, then rotate."
                )
            usdc_unsigned = evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(self._net.usdc_address[2:]), value=0,
                data=data, gas=gas, max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            )
            reserved += usdc_gas_cost
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
        # the funds at an address whose key was never saved. The read-modify-
        # write runs under the shared file lock so a concurrent settings save
        # cannot merge onto a stale snapshot and write the old key back.
        with _io_transaction(self._io):
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

        for unsigned in (millieth_unsigned, usdc_unsigned, eth_unsigned):
            if unsigned is None:
                continue
            signed = evm.sign_tx(unsigned, old.private_key)
            self._evm.send_raw_transaction(signed.raw)
            txs.append(signed.tx_hash)

        _log.info("rotated hot wallet %s -> %s (%d sweep txs)",
                  old.address, new_key.address, len(txs))
        return {"old_address": old.address, "new_address": new_key.address,
                "sweep_txs": txs, "swept_usdc_micros": usdc,
                "swept_millieth_units": millieth,
                "swept_eth_wei": max(0, eth_amount)}

    # -- retired-key recovery ------------------------------------------------ #

    def retired_balances(self) -> List[dict]:
        """Per archived key: its address and current ETH/USDC balances.

        The in-app view of what a past rotation may have left behind (a crash
        between the key swap and a sweep broadcast, or a partial sweep). Read
        side only; never raises on a single unreadable balance.
        """
        out: List[dict] = []
        warp = self._io.read().get("warp") or {}
        for entry in warp.get("retired_keys") or []:
            addr = str(entry.get("address") or "")
            row = {"address": addr, "retired_at": entry.get("retired_at")}
            try:
                row["eth_wei"] = int(self._evm.get_eth_balance(addr))
                row["usdc_micros"] = int(
                    self._evm.get_erc20_balance(self._net.usdc_address, addr)
                )
                row["millieth_units"] = int(
                    self._evm.get_erc20_balance(
                        self._net.milli_eth_address, addr)
                )
            except Exception as exc:  # noqa: BLE001 -- a bad row must not hide the rest
                row["error"] = str(exc)
            out.append(row)
        return out

    def recover_retired(self, address: str) -> dict:
        """Sweep a retired key's balance back to the active key.

        The in-app escape from a strand: whenever the retired address still
        holds enough ETH to pay every transfer's gas, this moves its
        milliETH, USDC, and then its ETH to the current active address. It
        cannot conjure gas -- an address holding tokens but zero ETH must be
        funded with a little ETH first (send some to it), after which this
        sweeps everything. All sweeps are preflighted (gas estimated and
        affordability checked) BEFORE anything is broadcast, so a refusal
        always means nothing moved -- a gas shortfall can never strand a
        half-swept key. (An RPC failure mid-broadcast can still leave some
        sweeps pending, as anywhere; retries are nonce-safe because nonces
        are pending-aware.) Signs with the retired key loaded from its
        archived blob; the archive is untouched.
        """
        from .warp import evm, keystore

        address = str(address).strip()
        warp = self._io.read().get("warp") or {}
        entry = next(
            (e for e in (warp.get("retired_keys") or [])
             if str(e.get("address") or "").lower() == address.lower()),
            None,
        )
        if entry is None:
            raise BaseWalletError(f"no retired key archived for {address}")
        old = keystore.load_evm_key(entry["blob"], protector=self._protector)
        dest = self.active_key().address
        if old.address.lower() == dest.lower():
            raise BaseWalletError(
                "the retired address is the active address; nothing to recover"
            )

        eth = int(self._evm.get_eth_balance(old.address))
        usdc = int(self._evm.get_erc20_balance(self._net.usdc_address, old.address))
        millieth = int(self._evm.get_erc20_balance(
            self._net.milli_eth_address, old.address))
        fees = self._evm.get_fee_data()
        nonce = self._evm.get_nonce(old.address)
        reserved = 0
        # Preflight-then-broadcast, exactly like rotate(): every sweep is
        # built and its gas checked cumulatively BEFORE the first broadcast.
        # Broadcasting as we go would let a later gas refusal report the
        # whole action failed after milliETH already moved -- and a retry
        # against the pending tx could read stale balances and enqueue a
        # duplicate transfer at the next nonce.
        unsigned_txs: List[object] = []

        if millieth > 0:
            m_data = evm.encode_transfer(dest, millieth)
            m_gas = self._evm.estimate_gas(
                from_address=old.address, to=self._net.milli_eth_address,
                value=0, data=m_data, default=_ERC20_TRANSFER_GAS_DEFAULT,
            )
            m_gas_cost = m_gas * fees.max_fee_per_gas
            if eth < m_gas_cost:
                raise BaseWalletError(
                    f"retired {address} holds {millieth} milliETH units but "
                    f"only {eth} wei ETH (~{m_gas_cost} needed for gas). "
                    "Send it a little Base ETH, then recover again."
                )
            unsigned_txs.append(evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(self._net.milli_eth_address[2:]), value=0,
                data=m_data, gas=m_gas,
                max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            ))
            reserved += m_gas_cost
            nonce += 1

        if usdc > 0:
            data = evm.encode_transfer(dest, usdc)
            gas = self._evm.estimate_gas(
                from_address=old.address, to=self._net.usdc_address, value=0,
                data=data, default=_ERC20_TRANSFER_GAS_DEFAULT,
            )
            usdc_gas_cost = gas * fees.max_fee_per_gas
            if eth < reserved + usdc_gas_cost:
                raise BaseWalletError(
                    f"retired {address} holds {usdc} USDC but only {eth} wei ETH "
                    f"(~{usdc_gas_cost} more needed for gas). Send it a little "
                    "Base ETH, then recover again."
                )
            unsigned_txs.append(evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(self._net.usdc_address[2:]), value=0,
                data=data, gas=gas, max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            ))
            reserved += usdc_gas_cost
            nonce += 1

        eth_amount = eth - reserved - _ETH_TRANSFER_GAS * fees.max_fee_per_gas
        if eth_amount > 0:
            unsigned_txs.append(evm.UnsignedTx(
                chain_id=self._net.evm_chain_id, nonce=nonce,
                to=bytes.fromhex(dest[2:]), value=eth_amount, data=b"",
                gas=_ETH_TRANSFER_GAS, max_fee_per_gas=fees.max_fee_per_gas,
                max_priority_fee_per_gas=fees.max_priority_fee_per_gas,
            ))

        if not unsigned_txs:
            raise BaseWalletError(
                f"retired {address} holds nothing sweepable "
                f"(ETH {eth} wei, USDC {usdc}, milliETH {millieth})"
            )

        # Full preflight passed: sign and broadcast in order.
        txs: List[str] = []
        for unsigned in unsigned_txs:
            signed = evm.sign_tx(unsigned, old.private_key)
            self._evm.send_raw_transaction(signed.raw)
            txs.append(signed.tx_hash)
        _log.info("recovered retired %s -> %s (%d txs)", address, dest, len(txs))
        return {"from": old.address, "to": dest, "sweep_txs": txs,
                "swept_usdc_micros": usdc,
                "swept_millieth_units": millieth,
                "swept_eth_wei": max(0, eth_amount)}
