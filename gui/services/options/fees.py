"""What a bundle costs to include, and the fee to offer for it.

A full node ranks transactions by fee per unit of *cost*, and when its mempool
is busy a transaction that pays nothing waits, or is turned away. So a fee is
sized from the bundle's cost, not chosen per transaction.

:func:`bundle_cost` adds the cost up the way the node does: what each puzzle
costs to run, a fixed charge for each coin created and each signature checked,
and a charge per byte. It is done with ``chia_wallet_sdk`` alone, so it needs no
node. Against chia-blockchain 2.7.5's own arithmetic it came out between 0.8%
and 2.1% high on every kind of bundle this package builds (mint, withdrawal,
cancel and clawback, over XCH and CAT collateral), which is the safe side to
err on.

:func:`fee_for` turns a cost and a rate into a fee under a cap.

THE RATE. The node's estimate alone is not enough to go by. The engine's fee
controller (``cpp/include/xop/strategy/fee_controller.hpp``) records what was
measured on this project's own wallet: with the mempool at capacity a node
admits a spend only at 5 mojos per unit of cost or more, the estimate it
publishes at that moment was 13 times below that, and the estimate reads zero
whenever there is room. Above the admission rate a spend confirms promptly
whatever it paid; below it, it does not confirm at all. Two rules follow, and
they are the small-scale form of what that controller does:

* :func:`pressure_rate`: when the estimate shows any pressure, pay the
  admission rate with a margin, not the estimate.
* :func:`raised_rate`: a transaction of ours that failed to confirm is the
  only evidence that counts, so the next one pays at least double what it paid.
  :func:`faded_rate` lets that fade again after some hours without a failure.
"""

from __future__ import annotations

import math
from collections.abc import Iterable
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from chia_wallet_sdk import CoinSpend

# Consensus constants (chia-blockchain's ConditionCost and COST_PER_BYTE).
_MAX_COST = 11_000_000_000
_COST_PER_BYTE = 12_000
_CREATE_COIN_COST = 1_800_000
_AGG_SIG_COST = 1_200_000
_CREATE_COIN = 51
_AGG_SIG_OPCODES = range(43, 51)  # AGG_SIG_PARENT .. AGG_SIG_ME
# Parent id, puzzle hash and amount, as each spend carries its coin.
_COIN_BYTES = 32 + 32 + 8


#: chia-blockchain ``mempool_manager.py`` ``nonzero_fee_minimum_fpc``: with the
#: mempool at capacity a spend below this many mojos per cost is refused.
ADMISSION_RATE = 5.0
#: Paid over the admission rate and over a node's estimate, as the engine does.
RATE_MARGIN = 1.10
#: The least it makes sense to pay once anything has to be paid at all.
PRESSURE_RATE = ADMISSION_RATE * RATE_MARGIN


class FeeError(ValueError):
    """A bundle's cost could not be worked out."""


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise FeeError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


def _opcode(condition: Any) -> Any:
    try:
        return condition.first().to_int()
    except Exception:  # an atom, or a non-integer head: not a condition that costs
        return None


def bundle_cost(coin_spends: Iterable[CoinSpend]) -> int:
    """The cost a node would charge ``coin_spends``, to within a few percent (high)."""
    clvm = _sdk().Clvm()
    total = 0
    for coin_spend in coin_spends:
        reveal, solution = bytes(coin_spend.puzzle_reveal), bytes(coin_spend.solution)
        try:
            output = clvm.deserialize(reveal).run(clvm.deserialize(solution), _MAX_COST, False)
            conditions = output.value.to_list() or []
        except Exception as exc:
            raise FeeError(f"a puzzle did not run: {exc}") from exc
        total += int(output.cost)
        for condition in conditions:
            opcode = _opcode(condition)
            if opcode == _CREATE_COIN:
                total += _CREATE_COIN_COST
            elif opcode in _AGG_SIG_OPCODES:
                total += _AGG_SIG_COST
        total += (len(reveal) + len(solution) + _COIN_BYTES) * _COST_PER_BYTE
    return total


def pressure_rate(estimate: float | None) -> float:
    """The rate to pay given a node's estimate, in mojos per cost.

    Nothing while the estimate is zero or unknown: the mempool has room and a
    spend gets in for free. Once it shows any pressure, at least the rate a
    full mempool admits, with the margin, because an estimate short of that
    buys nothing.
    """
    if estimate is None or not math.isfinite(estimate) or estimate <= 0:
        return 0.0
    return max(float(estimate), ADMISSION_RATE) * RATE_MARGIN


def raised_rate(paid_rate: float) -> float:
    """The rate to pay after a transaction that paid ``paid_rate`` did not confirm."""
    paid = paid_rate if math.isfinite(paid_rate) and paid_rate > 0 else 0.0
    return max(PRESSURE_RATE, 2.0 * paid)


def faded_rate(rate: float, idle_seconds: float, memory_seconds: float) -> float:
    """A raised rate after ``idle_seconds`` with no further failure.

    It halves once per ``memory_seconds``, and drops to nothing rather than
    below the rate a full mempool admits: under that it would buy nothing.
    """
    if rate <= 0 or memory_seconds <= 0:
        return max(rate, 0.0)
    for _ in range(int(max(idle_seconds, 0) // memory_seconds)):
        rate = rate / 2.0 if rate / 2.0 >= PRESSURE_RATE else 0.0
        if rate == 0.0:
            break
    return rate


def fee_for(cost: int, mojos_per_cost: float, max_fee_mojos: int) -> int:
    """The fee for a bundle of ``cost`` at ``mojos_per_cost``, never above the cap."""
    if cost <= 0 or mojos_per_cost <= 0 or max_fee_mojos <= 0:
        return 0
    return min(int(max_fee_mojos), math.ceil(cost * mojos_per_cost))
