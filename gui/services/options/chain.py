"""Read an option contract back from the chain, and check it is what it claims.

The reference wallet does not track option singletons or the coins they lock, so
XOPTrader has to recover an option's state from chain data: after a restart for
one it minted, and before paying for one somebody else is offering.

:func:`load_option` follows the singleton from its launcher to its current coin
and applies the validity checks CHIP-0045 requires of any wallet or display
service. They matter because an option's value is entirely in the coin it
locks: a singleton with the right inner puzzle but no real coin behind it, or a
coin locked on other terms, is worth nothing however it is priced. Every check
is a hash comparison against data the option itself commits to, so nothing here
trusts the party offering it.

:func:`load_cat` rebuilds the lineage proof needed to spend a CAT coin, which a
wallet RPC's coin record does not carry.

Chain access is injected as :class:`ChainReader`. The SDK's ``Simulator``
already has that shape, which is what the tests use; :class:`NodeChainReader`
adapts a full-node style client such as warp's ``CoinsetClient``.
"""

from __future__ import annotations

import dataclasses
import enum
from typing import TYPE_CHECKING, Any, Protocol

from gui.services.options.builder import Asset, Locked, OptionTerms

if TYPE_CHECKING:
    from chia_wallet_sdk import Cat, Coin, CoinSpend, OptionContract, OptionUnderlying

_MAX_COST = 11_000_000_000
# A singleton changes coin every time it is transferred or offered. An option
# lives for weeks at most; a lineage this long is a loop or an attack.
_MAX_LINEAGE = 10_000


class ChainError(RuntimeError):
    """Chain data was missing or unreadable. Says nothing about the option itself."""


class InvalidOptionError(ValueError):
    """The option fails a CHIP-0045 validity check and must not be traded."""


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise ChainError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


class ChainReader(Protocol):
    """The two lookups reading an option needs."""

    def coin_state(self, coin_id: bytes) -> Any:
        """The coin with ``.coin``, ``.created_height`` and ``.spent_height``, or None."""
        ...

    def coin_spend(self, coin_id: bytes) -> Any:
        """A spent coin's ``.coin``, ``.puzzle_reveal`` and ``.solution``, or None."""
        ...


class NodeChainReader:
    """Adapts a full-node style client to :class:`ChainReader`.

    ``client`` needs ``get_coin_record_by_name(hex) -> record | None`` and
    ``get_puzzle_and_solution(hex, height) -> solution``, as
    ``gui.services.warp.coinset.CoinsetClient`` provides: records with ``.coin``
    (hex ``parent_coin_info`` / ``puzzle_hash``, int ``amount``),
    ``.confirmed_block_index``, ``.spent_block_index`` and ``.spent``; solutions
    with hex ``.puzzle_reveal`` and ``.solution``.
    """

    def __init__(self, client: Any) -> None:
        self._client = client

    def _record(self, coin_id: bytes) -> Any:
        try:
            return self._client.get_coin_record_by_name(bytes(coin_id).hex())
        except Exception as exc:
            raise ChainError(f"coin record lookup failed: {exc}") from exc

    @staticmethod
    def _coin(record: Any) -> Coin:
        coin = record.coin
        return _sdk().Coin(
            _unhex(coin.parent_coin_info), _unhex(coin.puzzle_hash), int(coin.amount)
        )

    def coin_state(self, coin_id: bytes) -> Any:
        record = self._record(coin_id)
        if record is None:
            return None
        spent = bool(getattr(record, "spent", False)) or int(record.spent_block_index or 0) > 0
        return _sdk().CoinState(
            self._coin(record),
            int(record.spent_block_index) if spent else None,
            int(record.confirmed_block_index),
        )

    def coin_spend(self, coin_id: bytes) -> Any:
        record = self._record(coin_id)
        if record is None:
            return None
        if not (bool(getattr(record, "spent", False)) or int(record.spent_block_index or 0) > 0):
            return None
        try:
            solution = self._client.get_puzzle_and_solution(
                bytes(coin_id).hex(), int(record.spent_block_index)
            )
        except Exception as exc:
            raise ChainError(f"puzzle and solution lookup failed: {exc}") from exc
        return _sdk().CoinSpend(
            self._coin(record), _unhex(solution.puzzle_reveal), _unhex(solution.solution)
        )


def _unhex(value: Any) -> bytes:
    text = str(value)
    if text[:2].lower() == "0x":
        text = text[2:]
    try:
        return bytes.fromhex(text)
    except ValueError as exc:
        raise ChainError("the node returned malformed hex") from exc


def _state(chain: ChainReader, coin_id: bytes, what: str) -> Any:
    try:
        return chain.coin_state(bytes(coin_id))
    except ChainError:
        raise
    except Exception as exc:
        raise ChainError(f"could not look up {what}: {exc}") from exc


def _spend(chain: ChainReader, coin_id: bytes, what: str) -> Any:
    try:
        return chain.coin_spend(bytes(coin_id))
    except ChainError:
        raise
    except Exception as exc:
        raise ChainError(f"could not fetch the spend of {what}: {exc}") from exc


# ---------------------------------------------------------------------------
# CAT coins
# ---------------------------------------------------------------------------


def load_cat(chain: ChainReader, coin_id: bytes) -> Cat:
    """Rebuild a spendable CAT (coin, lineage proof, asset id) from its parent's spend."""
    sdk = _sdk()
    state = _state(chain, coin_id, "the CAT coin")
    if state is None:
        raise ChainError(f"coin {bytes(coin_id).hex()} is not on chain")
    parent = _spend(chain, state.coin.parent_coin_info, "the CAT coin's parent")
    if parent is None:
        raise ChainError("the CAT coin's parent spend is not available")
    clvm = sdk.Clvm()
    try:
        puzzle = clvm.deserialize(bytes(parent.puzzle_reveal)).puzzle()
        children = puzzle.parse_child_cats(parent.coin, clvm.deserialize(bytes(parent.solution)))
    except Exception as exc:
        raise ChainError(f"could not parse the CAT coin's parent: {exc}") from exc
    for cat in children or []:
        if bytes(cat.coin.coin_id()) == bytes(coin_id):
            return cat
    raise ChainError(f"coin {bytes(coin_id).hex()} is not a CAT coin")


# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------


class OptionStatus(enum.Enum):
    LIVE = "live"  # exercisable now
    EXPIRED = "expired"  # past expiry, collateral still locked: creator can claw back
    CLOSED = "closed"  # collateral gone: exercised, cancelled or clawed back


@dataclasses.dataclass(frozen=True)
class LoadedOption:
    """An option as the chain currently has it.

    ``option`` is None once the singleton has been melted, and ``locked`` is
    None once the underlying coin has been spent.
    """

    launcher_id: bytes
    terms: OptionTerms
    underlying: OptionUnderlying
    status: OptionStatus
    option: OptionContract | None
    locked: Locked | None

    @property
    def owner_puzzle_hash(self) -> bytes | None:
        return None if self.option is None else bytes(self.option.info.p2_puzzle_hash)


def _strike_asset(strike_type: Any) -> Asset:
    as_xch = strike_type.to_xch()
    if as_xch is not None:
        return Asset(None, int(as_xch.amount))
    as_cat = strike_type.to_cat()
    if as_cat is not None:
        return Asset(bytes(as_cat.asset_id), int(as_cat.amount))
    raise InvalidOptionError("only XCH and plain CAT strikes are supported")


def _creator_hint(sdk: Any, clvm: Any, parent: CoinSpend, launcher: Coin) -> bytes:
    """The creator address, from the memo on the coin that created the launcher.

    A memo is only a claim. It is believed because the underlying coin's puzzle
    hash, checked by the caller, commits to the creator: a false hint produces a
    different hash and the option is rejected.
    """
    try:
        output = clvm.deserialize(bytes(parent.puzzle_reveal)).run(
            clvm.deserialize(bytes(parent.solution)), _MAX_COST, False
        )
        conditions = output.value.to_list() or []
    except Exception as exc:
        raise ChainError(f"could not run the launcher's parent: {exc}") from exc
    for condition in conditions:
        create = condition.parse_create_coin()
        if create is None:
            continue
        if bytes(create.puzzle_hash) != bytes(launcher.puzzle_hash):
            continue
        if int(create.amount) != int(launcher.amount):
            continue
        memos = None if create.memos is None else create.memos.to_list()
        if not memos:
            break
        hint = memos[0].to_atom()
        if hint is None or len(hint) != 32:
            break
        return bytes(hint)
    raise InvalidOptionError("the launcher carries no creator hint")


def load_option(chain: ChainReader, launcher_id: bytes, *, now_seconds: int) -> LoadedOption:
    """Load and validate the option with ``launcher_id``.

    Raises :class:`InvalidOptionError` if any CHIP-0045 check fails and
    :class:`ChainError` if chain data could not be read. ``now_seconds`` only
    decides between ``LIVE`` and ``EXPIRED``; pass the chain's latest
    transaction-block timestamp, not a local clock, where the difference
    matters.
    """
    sdk = _sdk()
    clvm = sdk.Clvm()
    launcher_id = bytes(launcher_id)

    # 1. The launcher: a real singleton launcher whose solution carries the
    #    option metadata (expiry and strike).
    launch = _spend(chain, launcher_id, "the launcher")
    if launch is None:
        raise InvalidOptionError("no spent launcher coin with that id")
    if bytes(launch.coin.coin_id()) != launcher_id:
        raise ChainError("the node returned a different coin for the launcher id")
    if bytes(launch.coin.puzzle_hash) != bytes(sdk.Constants.singleton_launcher_hash()):
        raise InvalidOptionError("the launcher id is not a singleton launcher")
    try:
        fields = clvm.deserialize(bytes(launch.solution)).to_list()
    except Exception as exc:
        raise InvalidOptionError(f"the launcher solution is unreadable: {exc}") from exc
    if not fields or len(fields) < 3:
        raise InvalidOptionError("the launcher solution is malformed")
    singleton_puzzle_hash = fields[0].to_atom()
    singleton_amount = fields[1].to_int()
    metadata = fields[2].parse_option_metadata()
    if singleton_puzzle_hash is None or len(singleton_puzzle_hash) != 32 or not singleton_amount:
        raise InvalidOptionError("the launcher solution is malformed")
    if metadata is None:
        raise InvalidOptionError("the launcher carries no option metadata")
    strike = _strike_asset(metadata.strike_type)
    expiration = int(metadata.expiration_seconds)

    # 2. The creator, as hinted by the spend that created the launcher.
    parent = _spend(chain, launch.coin.parent_coin_info, "the launcher's parent")
    if parent is None:
        raise ChainError("the launcher's parent spend is not available")
    creator = _creator_hint(sdk, clvm, parent, launch.coin)

    # 3. Follow the singleton to its current coin.
    coin = sdk.Coin(launcher_id, bytes(singleton_puzzle_hash), int(singleton_amount))
    option: Any = None
    committed_coin_id: bytes | None = None
    committed_delegated: bytes | None = None
    melted = False
    for _ in range(_MAX_LINEAGE):
        state = _state(chain, coin.coin_id(), "the option coin")
        if state is None:
            raise ChainError("an option coin in the lineage is not on chain")
        if state.spent_height is None:
            break
        spend = _spend(chain, coin.coin_id(), "the option coin")
        if spend is None:
            raise ChainError("the option coin's spend is not available")
        try:
            puzzle = clvm.deserialize(bytes(spend.puzzle_reveal)).puzzle()
            solution = clvm.deserialize(bytes(spend.solution))
            parsed = puzzle.parse_option(spend.coin, solution)
            child = puzzle.parse_child_option(spend.coin, solution)
        except Exception as exc:
            raise InvalidOptionError(f"the option coin does not parse: {exc}") from exc
        if parsed is None:
            raise InvalidOptionError("the launcher did not create an option contract")
        info = parsed.option.info
        if bytes(info.launcher_id) != launcher_id:
            raise InvalidOptionError("the option contract belongs to a different launcher")
        committed_coin_id = bytes(info.underlying_coin_id)
        committed_delegated = bytes(info.underlying_delegated_puzzle_hash)
        if child is None:
            melted = True
            option = None
            break
        option = child
        coin = child.coin
    else:
        raise InvalidOptionError("the option's lineage is implausibly long")
    if committed_coin_id is None or committed_delegated is None:
        # Wallets spend the eve coin in the mint itself. One that never did has
        # revealed no puzzle, so its commitments cannot be read from the chain.
        raise InvalidOptionError("the option coin has never been spent; its terms are not on chain")

    # 4. The underlying coin the singleton commits to.
    locked_state = _state(chain, committed_coin_id, "the underlying coin")
    if locked_state is None:
        raise InvalidOptionError("the underlying coin does not exist")
    locked_coin = locked_state.coin
    if bytes(locked_coin.coin_id()) != committed_coin_id:
        raise ChainError("the node returned a different coin for the underlying coin id")

    # 5. The underlying asset: XCH if the coin sits directly at the option's
    #    lock puzzle, otherwise it must be a plain CAT wrapping that puzzle.
    amount = int(locked_coin.amount)
    if amount <= 0:
        raise InvalidOptionError("the underlying coin is empty")
    as_xch = sdk.OptionUnderlying(
        launcher_id, creator, expiration, amount, strike.sdk_option_type()
    )
    lock_hash = bytes(as_xch.puzzle_hash())
    locked: Locked
    if bytes(locked_coin.puzzle_hash) == lock_hash:
        underlying_asset = Asset(None, amount)
        locked = Locked(coin=locked_coin)
    else:
        try:
            cat = load_cat(chain, committed_coin_id)
        except ChainError as exc:
            raise InvalidOptionError(
                f"the underlying coin is not locked by the option's terms: {exc}"
            ) from exc
        if cat.info.hidden_puzzle_hash is not None:
            raise InvalidOptionError("revocable CAT underlyings are not supported")
        if bytes(cat.info.p2_puzzle_hash) != lock_hash:
            raise InvalidOptionError("the underlying CAT is not locked by the option's terms")
        underlying_asset = Asset(bytes(cat.info.asset_id), amount)
        locked = Locked(coin=locked_coin, cat=cat)

    # 6. The strike payment the singleton precommits to.
    if bytes(as_xch.delegated_puzzle_hash()) != committed_delegated:
        raise InvalidOptionError("the option does not commit to the strike in its metadata")

    terms = OptionTerms(
        underlying=underlying_asset,
        strike=strike,
        expiration_seconds=expiration,
        creator_puzzle_hash=creator,
    )
    spent = locked_state.spent_height is not None
    if spent or melted:
        status = OptionStatus.CLOSED
    elif int(now_seconds) >= expiration:
        status = OptionStatus.EXPIRED
    else:
        status = OptionStatus.LIVE
    return LoadedOption(
        launcher_id=launcher_id,
        terms=terms,
        underlying=as_xch,
        status=status,
        option=None if melted else option,
        locked=None if spent else locked,
    )
