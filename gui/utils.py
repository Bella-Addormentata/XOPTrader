"""Shared utility functions for XOPTrader GUI.

Centralises mojo-to-display-unit conversion logic so that all widgets
use a single, tested implementation.

ISO/IEC 5055 -- bounded arithmetic, explicit integer division constant.
"""

from __future__ import annotations

from typing import Any, Mapping, Optional

# 1 XCH = 10^12 mojos.  CAT tokens use 10^3 mojos per unit.
MOJOS_PER_XCH: int = 1_000_000_000_000
MOJOS_PER_CAT: int = 1_000

# Shown in place of a numeric value the database does not have (SQL NULL).
# Deliberately distinct from ``0`` so a backfilled fill with no recorded
# cost basis is never misread as a genuine zero-profit trade.
NULL_DISPLAY: str = "—"  # em dash


# ---------------------------------------------------------------------------
# NULL-safe row accessors
# ---------------------------------------------------------------------------
#
# sqlite3 maps a SQL NULL to Python ``None``.  Widgets read rows produced by
# ``SELECT *``, so a nullable column is PRESENT in the dict with the value
# ``None`` -- and ``dict.get(key, default)`` returns ``None``, never the
# default, because the key exists.  Any arithmetic or numeric formatting on
# that result then raises TypeError.  ``trade_log.cost_basis_mojos`` and
# ``trade_log.realized_pnl_mojos`` are nullable by design (rows backfilled
# from chain history carry no basis), so this is a live crash, not a theory.
# Use these helpers instead of ``.get(key, default)`` on any DB row.


def num(row: Mapping[str, Any], key: str, default: float = 0) -> Any:
    """Return a numeric column from *row*, substituting *default* for NULL.

    Unlike ``row.get(key, default)`` this also returns *default* when the
    key exists but holds ``None`` (SQL NULL).

    Parameters
    ----------
    row:
        Database row rendered as a mapping (e.g. ``dict(sqlite3.Row)``).
    key:
        Column name.
    default:
        Value substituted when the column is missing or NULL.
    """
    value = row.get(key, default)
    return default if value is None else value


def text(row: Mapping[str, Any], key: str, default: str = "") -> str:
    """Return a string column from *row*, substituting *default* for NULL.

    Guards the same missing-vs-NULL distinction as :func:`num` and always
    returns a ``str`` so ``.lower()`` / slicing on the result is safe.
    """
    value = row.get(key, default)
    if value is None:
        return default
    return value if isinstance(value, str) else str(value)


def opt_num(row: Mapping[str, Any], key: str) -> Optional[float]:
    """Return a numeric column, or ``None`` when it is missing or NULL.

    Use when NULL must stay visible to the caller (to render
    :data:`NULL_DISPLAY` rather than a misleading ``0``).
    """
    value = row.get(key)
    return None if value is None else value


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


# Recognised stablecoin symbols whose quote prices map 1 : 1 to USD.
_STABLECOINS: frozenset[str] = frozenset({
    "WUSDC.B", "WUSDC", "USDS", "USDT",
})


def load_offer_sizing():
    """Import scripts/offer_sizing.py by path and return the module.

    The advisory offer-sizing calculator is shared with the
    recommend_offer_sizes.py CLI; scripts/ is not a package, so it is
    loaded by file path, registered in ``sys.modules`` (dataclass
    processing requires the entry to exist during exec) and cached there
    for every GUI caller.  The calculator is read-only against
    config.yaml, the engine database and dexie.
    """
    import importlib.util
    import sys
    from pathlib import Path

    name = "xop_offer_sizing"
    existing = sys.modules.get(name)
    if existing is not None:
        return existing
    path = Path(__file__).resolve().parents[1] / "scripts" / "offer_sizing.py"
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(name, None)
        raise
    return module


def is_stablecoin_quoted(pair_name: str) -> bool:
    """Return ``True`` when the quote asset of *pair_name* is a stablecoin."""
    parts = pair_name.split("/")
    quote = parts[1].strip().upper() if len(parts) > 1 else ""
    return quote in _STABLECOINS


def format_price(price_mojos: int, pair_name: str) -> str:
    """Format a price for display, using ``$`` notation for stablecoin pairs.

    For stablecoin-quoted pairs the value is shown as ``$X.XXXX``.
    For other pairs the standard mojo-to-XCH conversion is used.
    """
    value: float = price_mojos / MOJOS_PER_XCH
    if is_stablecoin_quoted(pair_name):
        return f"${value:,.4f}"
    return f"{value:,.4f}"


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
