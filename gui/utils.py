"""Shared utility functions for XOPTrader GUI.

Centralises mojo-to-XCH conversion logic so that all widgets use
a single, tested implementation.

ISO/IEC 5055 -- bounded arithmetic, explicit integer division constant.
"""

from __future__ import annotations

# 1 XCH = 10^12 mojos.
MOJOS_PER_XCH: int = 1_000_000_000_000


def mojos_to_xch(mojos: int, decimals: int = 4) -> str:
    """Convert mojos to a formatted XCH string with thousand separators.

    Parameters
    ----------
    mojos:
        Amount in mojos (smallest Chia unit).
    decimals:
        Number of decimal places in the formatted output.
        Use 4 for compact display; 12 for full mojo precision.

    Returns
    -------
    Formatted string such as ``"1,234.5678"``.
    """
    xch: float = mojos / MOJOS_PER_XCH
    return f"{xch:,.{decimals}f}"


def mojos_to_xch_float(mojos: int) -> float:
    """Convert mojos to XCH as a raw float.

    Parameters
    ----------
    mojos:
        Amount in mojos (smallest Chia unit).

    Returns
    -------
    Equivalent value in XCH.
    """
    return mojos / MOJOS_PER_XCH
