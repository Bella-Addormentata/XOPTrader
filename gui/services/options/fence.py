"""[OPTIONS-FENCE] The options sub-account's coins, kept out of the app's own wallet spends.

With ``options.wallet.engine_fence`` the options address is a sub-account of
the trading wallet: the engine sends its coins as ``excluded_coin_ids`` on
every wallet call that selects coins (``cpp/include/xop/rpc/coin_fence.hpp``).
The app spends from the same wallet too -- bridge sends and claims, Dexie
takes, the options book's own top-ups -- through
:class:`gui.services.warp.wallet.WalletClient`. This module gives that client
the same list, so nothing the app sends can use a coin of the sub-account.

Which coins: every spendable coin of the wallet whose puzzle hash is the
options address's, or a CAT's wrapping of it (``chia_wallet_sdk``'s
``cat_puzzle_hash``, the one the book builds with). Listed from the wallet
itself, so a CAT wallet opened later is covered at the next refresh.

Fail-closed: a fence that is on but cannot list its coins raises
:class:`FenceUnavailableError`, and the client refuses the spend.

[AUTO-FENCE] With ``engine_fence: auto`` the fence is up only while the options
book is in use. The book (:mod:`runner`) keeps :data:`FLAG_NAME` beside the
engine's database while it is switched on, has a contract open or a
transaction unsettled; the engine follows the flag, and so does this client
(``WalletFence(active=...)``). A flag that cannot be read counts as up.
"""

from __future__ import annotations

import logging
import os
import re
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

#: [AUTO-FENCE] The flag the engine follows with ``engine_fence: auto``: kept
#: beside its database, where the Dexie switch's pause.flag is.
FLAG_NAME = "options_fence.flag"
#: The engine's database when config.yaml names none (EngineBridge's default).
_DEFAULT_ENGINE_DB = "data/xop_trader.db"

#: Wallet RPC endpoints that make the wallet select coins.
SPENDING = frozenset(
    {
        "send_transaction",
        "send_transaction_multi",
        "cat_spend",
        "take_offer",
        "create_offer_for_ids",
        "cancel_offer",
        "cancel_offers",
        "split_coins",
        "combine_coins",
        "select_coins",
        "create_signed_transaction",
    }
)

#: Wallet types whose coins can sit at the options address.
_STANDARD_WALLET = 0
_CAT_WALLET = 6

WalletRpc = Callable[[str, dict], dict]


class FenceUnavailableError(RuntimeError):
    """The fence is on and its coins are not known: nothing may be spent."""


def fenced_address(root: Any) -> tuple[str, str] | None:
    """``(address, prefix)`` when ``options.wallet.engine_fence`` is on, else None.

    Read straight from the config mapping, without the options parser: a
    bridge send must not fail on an options setting it has nothing to do
    with. On with no address is still on (an empty address, which the fence
    refuses to list).
    """
    if not isinstance(root, dict):
        return None
    options = root.get("options")
    if not isinstance(options, dict) or options.get("enabled") is not True:
        return None
    wallet = options.get("wallet")
    if not isinstance(wallet, dict):
        return None
    if wallet.get("engine_fence") is not True and not follows_book(root):
        return None
    if wallet.get("share_trading_wallet") is not True:
        return None
    prefix = "txch" if str(options.get("network") or "mainnet").strip() == "testnet11" else "xch"
    return str(wallet.get("address") or "").strip(), prefix


def follows_book(root: Any) -> bool:
    """[AUTO-FENCE] Whether ``options.wallet.engine_fence`` is ``auto`` in the config mapping."""
    if not isinstance(root, dict):
        return False
    options = root.get("options")
    wallet = options.get("wallet") if isinstance(options, dict) else None
    raw = wallet.get("engine_fence") if isinstance(wallet, dict) else None
    return isinstance(raw, str) and raw.strip().lower() == "auto"


#: ``${VAR}`` and ``${VAR:-default}``, as the engine expands a configured path
#: (config.cpp expand_env_vars).
_ENV_REF = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}")


def _engine_path(text: str) -> str:
    """A configured path as the engine reads it: ``${VAR}`` references, blanks, then ``~``.

    An unset ``${VAR}`` with no default is left as written: the engine refuses
    such a config and does not start, so there is no fence to follow.
    """

    def ref(match: re.Match[str]) -> str:
        value = os.environ.get(match.group(1))
        if value is not None:
            return value
        return match.group(2) if match.group(2) is not None else match.group(0)

    expanded = _ENV_REF.sub(ref, text).strip()
    if expanded.startswith("~"):
        home = os.environ.get("HOME") or os.environ.get("USERPROFILE")
        if home:
            return home + expanded[1:]
    return expanded


def flag_path(home: Path | str | None, root: Any = None) -> Path:
    """[AUTO-FENCE] Where the engine looks for :data:`FLAG_NAME`: beside its database.

    ``database.path`` of the config mapping ``root`` (``data/xop_trader.db``
    when unset), expanded as the engine expands it, relative to ``home``,
    config.yaml's folder: the engine's working directory when the app starts
    it, and the folder the app's own pause flag is written in (EngineBridge).
    """
    raw = ""
    if isinstance(root, dict) and isinstance(root.get("database"), dict):
        raw = str(root["database"].get("path") or "")
    database = Path(_engine_path(raw) or _DEFAULT_ENGINE_DB)
    if not database.is_absolute() and home is not None:
        database = Path(home) / database
    return database.parent / FLAG_NAME


def flag_up(path: Path) -> bool:
    """Whether the flag is there. One that cannot be read counts as there: the fence fails up."""
    try:
        return Path(path).exists()
    except OSError:
        return True


def _exists(path: Path) -> bool:
    """Whether the flag is there, strictly: one that cannot be read is not known to be up."""
    try:
        return path.exists()
    except OSError:
        return False


def flag_owner(path: Path) -> int | None:
    """The process the flag was last raised by, or None when it does not say or cannot be read."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except OSError:
        return None
    for line in text.splitlines():
        key, _, value = line.partition("=")
        if key.strip() == "pid":
            try:
                return int(value.strip())
            except ValueError:
                return None
    return None


def process_gone(pid: int) -> bool:
    """Whether no process has ``pid`` any more. False when that cannot be told.

    Observes only: on Windows ``OpenProcess(SYNCHRONIZE)`` and a zero wait,
    never ``os.kill``, which is TerminateProcess there.
    """
    if pid <= 0 or pid == os.getpid():
        return False
    if sys.platform == "win32":
        import ctypes
        import ctypes.wintypes as wintypes

        synchronize = 0x00100000
        wait_object_0 = 0x00000000
        error_invalid_parameter = 87  # no process has this id
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.OpenProcess(synchronize, False, pid)
        if not handle:
            return ctypes.get_last_error() == error_invalid_parameter
        try:
            return kernel32.WaitForSingleObject(handle, 0) == wait_object_0
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, 0)  # signal 0 delivers nothing on POSIX
    except ProcessLookupError:
        return True
    except OSError:
        return False  # someone else's process, alive
    return False


def raise_flag(path: Path, why: str) -> bool:
    """Raise the flag as this process's, or refresh it; whether it is up afterwards.

    Written every time, so the flag names the process that acted last: the
    app lowers a flag only once that process is gone (:func:`process_gone`).
    False when it could not be written and is not there: the caller must not
    act Live, since nothing else then keeps the app's own spends off the
    options address's coins.
    """
    path = Path(path)
    was = _exists(path)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            f"pid={os.getpid()}\nwhy=the options book is in use: {why}\n", encoding="utf-8"
        )
    except OSError as exc:
        log.warning("[options] the fence flag %s could not be written: %s", path, exc)
        return _exists(path)
    if not was:
        log.info("[options] fence flag raised (%s): %s", why, path)
    return True


def lower_flag(path: Path) -> bool:
    """Remove the flag; whether it is gone afterwards. Never raises: a failure is logged.

    A flag that cannot be removed stays up, which keeps the fence up: the
    safe side.
    """
    path = Path(path)
    try:
        if path.exists():
            path.unlink()
            log.info("[options] fence flag lowered, the book is off with nothing open: %s", path)
        return True
    except OSError as exc:
        log.warning("[options] the fence flag %s could not be removed: %s", path, exc)
        return False


def set_flag(path: Path, up: bool, why: str = "") -> bool:
    """Raise (:func:`raise_flag`) or lower (:func:`lower_flag`) the flag; whether it moved."""
    path = Path(path)
    was = _exists(path)
    if up:
        return raise_flag(path, why) and not was
    return lower_flag(path) and was


def _sdk() -> Any:
    import chia_wallet_sdk

    return chia_wallet_sdk


def _unhex(value: Any) -> bytes:
    text = str(value or "")
    return bytes.fromhex(text[2:] if text[:2].lower() == "0x" else text)


class WalletFence:
    """The sub-account's coin ids, listed from the wallet and kept for ``max_age``."""

    def __init__(
        self,
        rpc: WalletRpc,
        address: str,
        prefix: str = "xch",
        *,
        max_age: float = 0.0,
        clock: Callable[[], float] = time.monotonic,
        active: Callable[[], bool] | None = None,
    ) -> None:
        self._rpc = rpc
        self._address = address
        self._prefix = prefix
        self._max_age = max_age
        self._clock = clock
        #: [AUTO-FENCE] Whether the fence is up just now; None = always.
        self._active = active
        self._ids: list[str] | None = None
        self._at = 0.0

    def puzzle_hash(self) -> bytes:
        try:
            decoded = _sdk().Address.decode(self._address)
        except Exception as exc:  # noqa: BLE001 - any decode failure fails closed
            raise FenceUnavailableError(
                f"options.wallet.address {self._address!r} is not valid"
            ) from exc
        if decoded.prefix != self._prefix:
            raise FenceUnavailableError(
                f"options.wallet.address is a {decoded.prefix} address; expected {self._prefix}"
            )
        return bytes(decoded.puzzle_hash)

    def identity(self, network: str) -> str:
        """"<puzzle hash>@<network>": what the engine publishes for the address it fences."""
        return f"{self.puzzle_hash().hex()}@{network}"

    def puzzle_hashes(self) -> set[bytes]:
        """The address and its wrapping under every CAT wallet the wallet has open."""
        sdk = _sdk()
        inner = self.puzzle_hash()
        out = {inner}
        for wallet in self._wallets():
            if int(wallet.get("type", -1)) == _CAT_WALLET:
                asset = self._rpc("cat_get_asset_id", {"wallet_id": int(wallet["id"])}).get(
                    "asset_id"
                )
                out.add(bytes(sdk.cat_puzzle_hash(_unhex(asset), inner)))
        return out

    def conflicting_offers(self) -> list[str]:
        """Our resting offers that hold a coin of the sub-account (made before the fence).

        Such an offer can still be taken or cancelled with a fee, spending the
        coin under the book: while any is out the book must not act beside the
        engine. Raises FenceUnavailableError when the wallet will not say.
        """
        try:
            wanted = self.puzzle_hashes()
            answer = self._rpc(
                "get_all_offers",
                {
                    "start": 0,
                    "end": 1000,
                    "exclude_my_offers": False,
                    "exclude_taken_offers": True,
                    "include_completed": False,
                },
            )
        except FenceUnavailableError:
            raise
        except Exception as exc:  # noqa: BLE001 - fail closed
            raise FenceUnavailableError(f"the wallet's offers could not be read: {exc}") from exc
        records = answer.get("trade_records")
        if not isinstance(records, list):
            raise FenceUnavailableError("the wallet's offers could not be read (no trade_records)")
        held: list[str] = []
        for record in records:
            if not isinstance(record, dict) or not record.get("is_my_offer"):
                continue
            if record.get("status") not in ("PENDING_ACCEPT", "PENDING_CONFIRM", "PENDING_CANCEL"):
                continue
            for coin in record.get("coins_of_interest") or []:
                if _unhex(coin.get("puzzle_hash")) in wanted:
                    held.append(str(record.get("trade_id") or "?")[:12])
                    break
        return held

    def _wallets(self) -> list[dict]:
        answer = self._rpc("get_wallets", {})
        wallets = answer.get("wallets")
        if not isinstance(wallets, list):
            raise FenceUnavailableError("the wallet did not list its wallets")
        return wallets

    def coin_ids(self) -> list[str]:
        """0x-hex ids of every spendable coin at the sub-account. Raises FenceUnavailableError.

        [AUTO-FENCE] None while the fence is down: the options address's
        coins are then the wallet's like any other.
        """
        if self._active is not None and not self._active():
            return []
        now = self._clock()
        if self._ids is not None and self._max_age > 0 and now - self._at <= self._max_age:
            return list(self._ids)
        try:
            ids = self._list()
        except FenceUnavailableError:
            self._ids = None
            raise
        except Exception as exc:  # noqa: BLE001 - fail closed on anything
            self._ids = None
            raise FenceUnavailableError(
                f"the options sub-account's coins could not be listed: {exc}"
            ) from exc
        self._ids, self._at = ids, now
        return list(ids)

    def _list(self) -> list[str]:
        sdk = _sdk()
        inner = self.puzzle_hash()
        ids: list[str] = []
        for wallet in self._wallets():
            kind = int(wallet.get("type", -1))
            if kind not in (_STANDARD_WALLET, _CAT_WALLET):
                continue
            wallet_id = int(wallet["id"])
            if kind == _STANDARD_WALLET:
                wanted = inner
            else:
                asset = self._rpc("cat_get_asset_id", {"wallet_id": wallet_id}).get("asset_id")
                wanted = bytes(sdk.cat_puzzle_hash(_unhex(asset), inner))
            coins = self._rpc("get_spendable_coins", {"wallet_id": wallet_id})
            records = coins.get("confirmed_records")
            if not isinstance(records, list):
                raise FenceUnavailableError(
                    f"the wallet did not list wallet {wallet_id}'s spendable coins"
                )
            for record in records:
                coin = record.get("coin", record)
                puzzle_hash = _unhex(coin.get("puzzle_hash"))
                if puzzle_hash != wanted:
                    continue
                made = sdk.Coin(
                    _unhex(coin.get("parent_coin_info")), puzzle_hash, int(coin["amount"])
                )
                ids.append("0x" + bytes(made.coin_id()).hex())
        return ids


def merge_exclusions(body: dict, ids: list[str]) -> dict:
    """``body`` with ``ids`` merged into its top-level ``excluded_coin_ids`` (a copy)."""
    if not ids:
        return body
    out = dict(body)
    seen: set[str] = set()
    merged: list[str] = []
    for item in list(out.get("excluded_coin_ids") or []) + list(ids):
        text = str(item).lower()
        text = text if text.startswith("0x") else "0x" + text
        if text not in seen:
            seen.add(text)
            merged.append(text)
    out["excluded_coin_ids"] = merged
    return out
