"""What a bundle actually does to our money, worked out before it is signed.

The reference wallet's ``execute_signing_instructions`` signs whatever it is
given, for every coin in the wallet. :mod:`reference_wallet` already refuses
anything the *wallet* adds to what we asked. This module guards the other side:
a bug in our own builder. It reads a bundle the way the chain will -- by running
the puzzles -- and reports, per asset, what leaves our addresses, what arrives,
what ends up locked in option contracts, what goes to anybody else, and the fee.
The caller states what the operation is supposed to do and :func:`require`
refuses the bundle if the two differ by a single mojo.

It is deliberately independent of :mod:`builder`: it never looks at how the
bundle was made, only at the coins it spends and creates.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Iterable, Sequence
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from chia_wallet_sdk import CoinSpend

_MAX_COST = 11_000_000_000
_CREATE_COIN = 51
_RESERVE_FEE = 52

# An asset is ``None`` for XCH or a CAT's 32-byte asset id.
Asset = bytes | None


class SafetyError(RuntimeError):
    """The bundle does not do what the operation declared. It must not be signed."""


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise SafetyError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


@dataclasses.dataclass(frozen=True)
class Effect:
    """The net result of a bundle, in mojos per asset.

    ``wallet`` and ``locked`` are signed changes (negative means it left).
    ``other`` is what was paid to puzzle hashes that are neither ours nor a
    lock we named, and is never negative. ``fee`` is the XCH the bundle leaves
    for the farmer.
    """

    wallet: dict[Asset, int]
    locked: dict[Asset, int]
    other: dict[Asset, int]
    fee: int

    def describe(self) -> str:
        def side(values: dict[Asset, int]) -> str:
            if not values:
                return "nothing"
            return ", ".join(
                f"{amount:+d} {'XCH' if asset is None else asset.hex()[:8]} mojos"
                for asset, amount in sorted(values.items(), key=lambda item: item[0] or b"")
            )

        return (
            f"wallet {side(self.wallet)}; locked {side(self.locked)}; "
            f"to others {side(self.other)}; fee {self.fee} mojos"
        )


def _conditions(clvm: Any, coin_spend: CoinSpend) -> list[Any]:
    coin_id = bytes(coin_spend.coin.coin_id()).hex()
    try:
        output = clvm.deserialize(bytes(coin_spend.puzzle_reveal)).run(
            clvm.deserialize(bytes(coin_spend.solution)), _MAX_COST, False
        )
        conditions = output.value.to_list()
    except Exception as exc:
        raise SafetyError(f"coin {coin_id}: the puzzle did not run: {exc}") from exc
    if conditions is None:
        raise SafetyError(f"coin {coin_id}: the puzzle did not return a condition list")
    return list(conditions)


def _opcode(condition: Any) -> Any:
    try:
        return condition.first().to_int()
    except Exception:
        return None


def analyse(
    coin_spends: Sequence[CoinSpend],
    *,
    ours: Iterable[bytes],
    locks: Iterable[bytes] = (),
) -> Effect:
    """Work out what ``coin_spends`` does.

    ``ours`` are the p2 puzzle hashes of our own addresses. ``locks`` are the
    puzzle hashes of option lock puzzles whose collateral is ours to count: the
    one a mint is about to fill, or the one a cancel or clawback empties.

    A CAT is attributed to the address inside its CAT wrapper, so a wUSDC.b
    coin at our address counts as ours. Coins created and spent inside the
    bundle cancel out.
    """
    sdk = _sdk()
    clvm = sdk.Clvm()
    ours = {bytes(item) for item in ours}
    locks = {bytes(item) for item in locks}

    # coin id -> (asset, owner puzzle hash, amount)
    spent: dict[bytes, tuple[Asset, bytes, int]] = {}
    created: dict[bytes, tuple[Asset, bytes, int]] = {}
    reserved_fee = 0

    for coin_spend in coin_spends:
        coin = coin_spend.coin
        coin_id = bytes(coin.coin_id())
        if coin_id in spent:
            raise SafetyError(f"coin {coin_id.hex()} is spent twice in the bundle")
        try:
            puzzle = clvm.deserialize(bytes(coin_spend.puzzle_reveal)).puzzle()
            if bytes(puzzle.puzzle_hash) != bytes(coin.puzzle_hash):
                raise SafetyError(f"coin {coin_id.hex()}: the puzzle reveal is not the coin's")
            solution = clvm.deserialize(bytes(coin_spend.solution))
            cat_info = puzzle.parse_cat_info()
        except SafetyError:
            raise
        except Exception as exc:
            raise SafetyError(f"coin {coin_id.hex()}: the spend is unreadable: {exc}") from exc

        conditions = _conditions(clvm, coin_spend)
        if cat_info is not None:
            asset: Asset = bytes(cat_info.info.asset_id)
            spent[coin_id] = (asset, bytes(cat_info.info.p2_puzzle_hash), int(coin.amount))
            try:
                children = puzzle.parse_child_cats(coin, solution) or []
            except Exception as exc:
                raise SafetyError(
                    f"coin {coin_id.hex()}: the CAT's outputs are unreadable: {exc}"
                ) from exc
            for child in children:
                created[bytes(child.coin.coin_id())] = (
                    asset,
                    bytes(child.info.p2_puzzle_hash),
                    int(child.coin.amount),
                )
            continue

        spent[coin_id] = (None, bytes(coin.puzzle_hash), int(coin.amount))
        for condition in conditions:
            opcode = _opcode(condition)
            if opcode == _RESERVE_FEE:
                parsed_fee = condition.parse_reserve_fee()
                if parsed_fee is None:
                    raise SafetyError(f"coin {coin_id.hex()}: malformed RESERVE_FEE")
                reserved_fee += int(parsed_fee.amount)
            if opcode != _CREATE_COIN:
                continue
            try:
                parsed = condition.parse_create_coin()
            except Exception:
                parsed = None
            if parsed is None:
                # ``(51 () -113)`` melts a singleton and creates nothing.
                continue
            amount = int(parsed.amount)
            if amount < 0:
                continue
            child = sdk.Coin(coin_id, bytes(parsed.puzzle_hash), amount)
            created[bytes(child.coin_id())] = (None, bytes(parsed.puzzle_hash), amount)

    wallet: dict[Asset, int] = {}
    locked: dict[Asset, int] = {}
    other: dict[Asset, int] = {}
    totals: dict[Asset, int] = {}

    def book(asset: Asset, owner: bytes, amount: int, sign: int) -> None:
        totals[asset] = totals.get(asset, 0) - sign * amount
        if owner in ours:
            wallet[asset] = wallet.get(asset, 0) + sign * amount
        elif owner in locks:
            locked[asset] = locked.get(asset, 0) + sign * amount
        elif sign > 0:
            other[asset] = other.get(asset, 0) + amount
        # Coins spent from somebody else's puzzle (a settlement coin, a taker's
        # coin) only show up through what they pay out.

    for coin_id, (asset, owner, amount) in spent.items():
        if coin_id not in created:
            book(asset, owner, amount, -1)
    for coin_id, (asset, owner, amount) in created.items():
        if coin_id not in spent:
            book(asset, owner, amount, +1)

    fee = totals.get(None, 0)
    if fee < reserved_fee:
        raise SafetyError("the bundle reserves a larger fee than it leaves")
    return Effect(
        wallet={asset: amount for asset, amount in wallet.items() if amount},
        locked={asset: amount for asset, amount in locked.items() if amount},
        other={asset: amount for asset, amount in other.items() if amount},
        fee=fee,
    )


@dataclasses.dataclass(frozen=True)
class Expected:
    """What an operation is supposed to do. Every figure is in mojos.

    ``wallet`` and ``locked`` must match exactly. ``other_max`` bounds what may
    be paid to anybody else, per asset; an asset that is not listed may not be
    paid out at all. ``max_fee`` bounds the fee.
    """

    wallet: dict[Asset, int] = dataclasses.field(default_factory=dict)
    locked: dict[Asset, int] = dataclasses.field(default_factory=dict)
    other_max: dict[Asset, int] = dataclasses.field(default_factory=dict)
    max_fee: int = 0


def _clean(values: dict[Asset, int]) -> dict[Asset, int]:
    return {asset: int(amount) for asset, amount in values.items() if int(amount)}


def require(effect: Effect, expected: Expected) -> None:
    """Raise :class:`SafetyError` unless ``effect`` is exactly what was declared."""
    problems = []
    if effect.wallet != _clean(expected.wallet):
        problems.append("the change to the wallet is not the declared one")
    if effect.locked != _clean(expected.locked):
        problems.append("the change to locked collateral is not the declared one")
    for asset, amount in effect.other.items():
        if amount > int(expected.other_max.get(asset, 0)):
            name = "XCH" if asset is None else asset.hex()[:8]
            problems.append(f"{amount} mojos of {name} go to an address that is not ours")
    if effect.fee > int(expected.max_fee):
        problems.append(f"the fee is {effect.fee} mojos, above the {expected.max_fee} allowed")
    if effect.fee < 0:
        problems.append("the bundle creates more XCH than it spends")
    if problems:
        raise SafetyError("; ".join(problems) + f" (bundle: {effect.describe()})")
