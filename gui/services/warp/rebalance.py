"""Target-band rebalancer: keep the Base hot wallet at operator-set targets.

The trading side already maintains per-pair inventory around ratio targets;
this is the same idea one level up, allocating capital between Base reserves
and Chia trading float. The operator states a target and tolerance per
managed asset (``warp.rebalance``); everything above the band's top is moved
toward Chia (USDC bridges as wUSDC.b; excess ETH wraps into milliETH, ready
to bridge once the pipeline is multi-asset), and a deficit below the band's
bottom is pulled back (USDC unwraps from wUSDC.b to the hot wallet; ETH
refuels by unwrapping held milliETH).

Design rules, in the codebase's established shapes:

* **Opt-in and fail-closed** ([WARP-CAP-FAIL-OPEN]): everything off unless
  ``warp.rebalance.enabled`` is true, and an enabled config must state a
  positive target and a tolerance in (0, 100] for every managed asset --
  a malformed block refuses to parse rather than defaulting.
* **Deadband, rebalance to target**: an action fires only OUTSIDE the band
  and always moves the balance back to the *target*, not the band edge, so
  round-trip costs (~0.6% tips + tolls) are paid rarely, never in a loop.
* **One action per plan, one plan per cooldown**: the planner returns at
  most one action; the worker executes it only while no warp job is active
  and enforces a cooldown between actions. Gas safety outranks everything:
  an ETH deficit is always the first action considered.
* **Own-wallet destinations only**: the auto-unwrap receiver is ALWAYS the
  hot wallet address, taken from the engine, never from config -- an edited
  YAML must not be able to redirect an automatic unwrap.
* All existing rails stay in force underneath: max_auto_bridge / max_unwrap
  caps, the wrap granularity + relay-gas floor, the unmined-transaction
  guard, and the single-active-job slot.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional

#: ETH wrap granularity (MilliETH.deposit() reverts on anything finer).
_WRAP_GRANULARITY_WEI = 10 ** 12
#: One wmilliETH.b/milliETH unit in wei.
_WEI_PER_MILLIETH_UNIT = 10 ** 12
#: wUSDC.b mojos per USDC micro-unit factor (6 ERC-20 decimals -> 3 CAT).
_USDC_MICROS_PER_MOJO = 1000


class RebalanceConfigError(ValueError):
    """A malformed warp.rebalance block; refuses to parse (fail-closed)."""


@dataclass(frozen=True)
class AssetBand:
    target: int          # asset base units (micros / wei)
    tolerance_pct: float

    @property
    def low(self) -> int:
        return int(self.target * (1 - self.tolerance_pct / 100.0))

    @property
    def high(self) -> int:
        return int(self.target * (1 + self.tolerance_pct / 100.0))


@dataclass(frozen=True)
class RebalanceParams:
    enabled: bool = False
    usdc: Optional[AssetBand] = None   # micros
    eth: Optional[AssetBand] = None    # wei
    cooldown_s: int = 600


@dataclass(frozen=True)
class RebalanceAction:
    kind: str    # "unwrap_millieth" | "unwrap_usdc" | "wrap_eth" | "bridge_usdc"
    amount: int  # unwrap_millieth: units; unwrap_usdc: CAT mojos; else base units
    reason: str


def parse_rebalance_config(
    warp_cfg: dict, *, min_gas_wei: int
) -> RebalanceParams:
    """Parse ``warp.rebalance`` fail-closed.

    Absent block or ``enabled: false`` -> inert params. Enabled with a
    malformed asset block -> :class:`RebalanceConfigError`, never a silent
    default: an unbounded or mis-scaled band moves real money.
    """
    if warp_cfg is None:
        return RebalanceParams()
    if not isinstance(warp_cfg, dict):
        raise RebalanceConfigError("the warp config block must be a mapping")
    raw = warp_cfg.get("rebalance")
    if raw is None:
        return RebalanceParams()
    # A present-but-falsey block ([], "", 0) is malformed, not absent --
    # it must banner, not silently disable.
    if not isinstance(raw, dict):
        raise RebalanceConfigError("warp.rebalance must be a mapping")
    # Strict boolean, same rule as _parse_config_bool: a YAML-quoted
    # "false" is a non-empty string and bool() would fail OPEN into
    # money-moving behaviour.
    raw_enabled = raw.get("enabled", False)
    if isinstance(raw_enabled, bool):
        enabled = raw_enabled
    elif isinstance(raw_enabled, int) and raw_enabled in (0, 1):
        enabled = bool(raw_enabled)
    elif (isinstance(raw_enabled, str)
          and raw_enabled.strip().lower() in ("true", "false", "")):
        enabled = raw_enabled.strip().lower() == "true"
    elif raw_enabled is None:
        enabled = False
    else:
        raise RebalanceConfigError(
            f"warp.rebalance.enabled must be a boolean (got {raw_enabled!r})"
        )
    if not enabled:
        return RebalanceParams()

    def band(key: str, scale: int) -> Optional[AssetBand]:
        block = raw.get(key)
        if block is None:
            return None
        if not isinstance(block, dict):
            raise RebalanceConfigError(
                f"warp.rebalance.{key} must be a mapping with target and "
                "tolerance_pct"
            )
        try:
            target = float(block["target"])
            tol = float(block["tolerance_pct"])
        except (KeyError, TypeError, ValueError) as exc:
            raise RebalanceConfigError(
                f"warp.rebalance.{key} requires numeric target and "
                f"tolerance_pct ({exc})"
            ) from None
        if not (math.isfinite(target) and math.isfinite(tol)):
            raise RebalanceConfigError(
                f"warp.rebalance.{key} target/tolerance_pct must be finite"
            )
        if target <= 0:
            raise RebalanceConfigError(
                f"warp.rebalance.{key}.target must be positive"
            )
        if not 0 < tol <= 100:
            raise RebalanceConfigError(
                f"warp.rebalance.{key}.tolerance_pct must be in (0, 100]"
            )
        scaled = int(target * scale)
        if scaled < 1:
            raise RebalanceConfigError(
                f"warp.rebalance.{key}.target of {target} is below one base "
                "unit after scaling; a zero-unit band would rebalance the "
                "whole balance to zero"
            )
        return AssetBand(target=scaled, tolerance_pct=tol)

    usdc = band("usdc", 10 ** 6)
    eth = band("eth", 10 ** 18)
    if usdc is None and eth is None:
        raise RebalanceConfigError(
            "warp.rebalance.enabled is true but no asset block (usdc/eth) "
            "is configured; state at least one target or disable it"
        )
    if eth is not None and eth.low <= min_gas_wei:
        raise RebalanceConfigError(
            f"warp.rebalance.eth band bottom ({eth.low} wei) must stay above "
            f"the relay-gas floor ({min_gas_wei} wei): the ETH target doubles "
            "as the gas reserve"
        )
    raw_cooldown = raw.get("cooldown_s", 600)
    try:
        cooldown = int(raw_cooldown)
    except (TypeError, ValueError):
        raise RebalanceConfigError(
            f"warp.rebalance.cooldown_s must be an integer number of seconds "
            f"(got {raw_cooldown!r})"
        ) from None
    if cooldown < 60:
        raise RebalanceConfigError(
            "warp.rebalance.cooldown_s must be at least 60 seconds"
        )
    return RebalanceParams(
        enabled=True, usdc=usdc, eth=eth, cooldown_s=cooldown
    )


def plan(
    *,
    params: RebalanceParams,
    eth_wei: int,
    usdc_micros: int,
    millieth_units: int,
    max_bridge_micros: int,
    max_unwrap_micros: int,
    min_bridge_micros: int,
) -> Optional[RebalanceAction]:
    """At most one action moving one balance back to its target.

    Pure and side-effect free: the caller supplies observed balances and the
    existing pipeline caps, and enforces every execution-time rail (job slot,
    unmined guard, cooldown). Priority is gas safety first (ETH deficit),
    then restoring the USDC reserve, then shedding excess.
    """
    if not params.enabled:
        return None

    # 1. ETH deficit: the wallet is running out of gas. Refuel from held
    #    milliETH (the reason the wmilliETH.b market exists). Chia-side pull
    #    arrives with the multi-asset burn pipeline.
    if params.eth is not None and eth_wei < params.eth.low:
        deficit_units = -(-(params.eth.target - eth_wei) // _WEI_PER_MILLIETH_UNIT)
        units = min(int(deficit_units), int(millieth_units))
        if units >= 1:
            return RebalanceAction(
                "unwrap_millieth", units,
                f"ETH {eth_wei} below band {params.eth.low}; unwrapping "
                f"{units} milliETH units toward target {params.eth.target}",
            )

    # 2. USDC deficit: pull the reserve back from Chia (wUSDC.b unwrap to
    #    the hot wallet). Clamped to the operator's unwrap cap; below one
    #    CAT mojo there is nothing to move.
    if params.usdc is not None and usdc_micros < params.usdc.low:
        deficit = params.usdc.target - usdc_micros
        if max_unwrap_micros > 0:
            deficit = min(deficit, max_unwrap_micros)
        mojos = deficit // _USDC_MICROS_PER_MOJO
        if mojos >= 1:
            return RebalanceAction(
                "unwrap_usdc", int(mojos),
                f"USDC {usdc_micros} below band {params.usdc.low}; unwrapping "
                f"{mojos} mojos toward target {params.usdc.target}",
            )

    # 3. ETH excess: wrap everything above target into milliETH (floored to
    #    the contract granularity). The wallet-side wrap re-checks the
    #    relay-gas floor with worst-case gas on top.
    if params.eth is not None and eth_wei > params.eth.high:
        excess = eth_wei - params.eth.target
        excess -= excess % _WRAP_GRANULARITY_WEI
        if excess >= _WRAP_GRANULARITY_WEI:
            return RebalanceAction(
                "wrap_eth", int(excess),
                f"ETH {eth_wei} above band {params.eth.high}; wrapping "
                f"{excess} wei toward target {params.eth.target}",
            )

    # 4. USDC excess: bridge it to Chia as trading float. Honors both the
    #    auto floor (no dust jobs) and the blast-radius cap.
    if params.usdc is not None and usdc_micros > params.usdc.high:
        excess = usdc_micros - params.usdc.target
        if max_bridge_micros > 0:
            excess = min(excess, max_bridge_micros)
        # The protocol floor is one CAT mojo (1000 micros), not one micro:
        # a sub-mojo job would sit pending forever, squatting the single
        # active-job slot.
        if excess >= max(min_bridge_micros, _USDC_MICROS_PER_MOJO):
            return RebalanceAction(
                "bridge_usdc", int(excess),
                f"USDC {usdc_micros} above band {params.usdc.high}; bridging "
                f"{excess} micros toward target {params.usdc.target}",
            )

    return None
