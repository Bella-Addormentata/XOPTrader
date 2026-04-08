"""Shared utility functions for XOPTrader GUI.

Centralises mojo-to-display-unit conversion logic so that all widgets
use a single, tested implementation.

ISO/IEC 5055 -- bounded arithmetic, explicit integer division constant.
"""

from __future__ import annotations

# 1 XCH = 10^12 mojos.  CAT tokens use 10^3 mojos per unit.
MOJOS_PER_XCH: int = 1_000_000_000_000
MOJOS_PER_CAT: int = 1_000


def mojos_per_unit_for_pair(pair_name: str, which: str = "base") -> int:
    """Return the mojos-per-unit divisor for an asset in a pair.

    Parameters
    ----------
    pair_name:
        Pair label such as ``"XCH/wUSDC.b"`` or ``"BYC/wUSDC.b"``.
    which:
        ``"base"`` for the left asset, ``"quote"`` for the right asset.

    Returns
    -------
    ``MOJOS_PER_XCH`` (10^12) if the asset is XCH, else
    ``MOJOS_PER_CAT`` (10^3) for CAT tokens.
    """
    parts = pair_name.split("/")
    if which == "base":
        token = parts[0].strip().upper() if parts else ""
    else:
        token = parts[1].strip().upper() if len(parts) > 1 else ""
    return MOJOS_PER_XCH if token == "XCH" else MOJOS_PER_CAT


def mojos_to_xch(mojos: int, decimals: int = 4,
                 mojos_per_unit: int = MOJOS_PER_XCH) -> str:
    """Convert mojos to a formatted display string with thousand separators.

    Parameters
    ----------
    mojos:
        Amount in mojos (smallest on-chain unit).
    decimals:
        Number of decimal places in the formatted output.
        Use 4 for compact display; 12 for full mojo precision.
    mojos_per_unit:
        Divisor for the asset.  Use ``MOJOS_PER_XCH`` (10^12) for XCH,
        ``MOJOS_PER_CAT`` (10^3) for CAT tokens like BYC/wUSDC.b.

    Returns
    -------
    Formatted string such as ``"1,234.5678"``.
    """
    value: float = mojos / mojos_per_unit
    return f"{value:,.{decimals}f}"


def mojos_to_xch_float(mojos: int,
                       mojos_per_unit: int = MOJOS_PER_XCH) -> float:
    """Convert mojos to a raw float in display units.

    Parameters
    ----------
    mojos:
        Amount in mojos (smallest on-chain unit).
    mojos_per_unit:
        Divisor for the asset.

    Returns
    -------
    Equivalent value in display units.
    """
    return mojos / mojos_per_unit
