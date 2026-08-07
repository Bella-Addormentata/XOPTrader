"""Thin coinset.org HTTP client (Chia full-node RPC) for the warp claim path.

The Chia side of the bridge needs only a handful of read endpoints plus
``push_tx`` -- no ``chia-blockchain`` node, no wallet for reads. coinset.org
mirrors the standard full-node RPC: every call is an HTTPS POST with a JSON body
and returns ``{"success": true, ...}``.

The network transport is injected (``poster``) so the state machine and its tests
can drive this client with canned responses; the default poster builds a
``requests`` session lazily, so importing this module costs nothing and pulls in
no third-party dependency until a real call is made.

All coin/puzzle-hash hex is normalised to lower-case **without** a ``0x`` prefix
on the way out (matching the rest of the warp stack) and re-prefixed with ``0x``
on the way in (matching the RPC's expectation), so callers never juggle prefixes.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable, Optional

# (url, json_body) -> parsed JSON dict.
Poster = Callable[[str, dict], dict]


class CoinsetError(RuntimeError):
    """A coinset RPC call failed at transport level or returned success=false."""


# --------------------------------------------------------------------------- #
# Value types (transport shapes; drivers convert to chia_rs.Coin as needed).
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class Coin:
    """A Chia coin as returned by the RPC (hex fields, no ``0x`` prefix)."""

    parent_coin_info: str
    puzzle_hash: str
    amount: int


@dataclass(frozen=True)
class CoinRecord:
    coin: Coin
    confirmed_block_index: int
    spent_block_index: int
    spent: bool
    coinbase: bool
    timestamp: Optional[int]


@dataclass(frozen=True)
class CoinSolution:
    """A spent coin's puzzle reveal and solution (hex, no ``0x`` prefix)."""

    coin: Coin
    puzzle_reveal: str
    solution: str


# --------------------------------------------------------------------------- #
# Hex helpers.
# --------------------------------------------------------------------------- #

def _hx(value: Any) -> str:
    """Normalise a hex string to lower-case without ``0x``; ``None`` -> ""."""
    if value is None:
        return ""
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower()


def _0x(value: str) -> str:
    """Prefix a (possibly already-prefixed) hex string with ``0x`` for the RPC."""
    return "0x" + _hx(value)


def _coin(d: dict) -> Coin:
    return Coin(
        parent_coin_info=_hx(d.get("parent_coin_info")),
        puzzle_hash=_hx(d.get("puzzle_hash")),
        amount=int(d.get("amount", 0)),
    )


def _coin_record(d: dict) -> CoinRecord:
    spent_index = int(d.get("spent_block_index", 0) or 0)
    return CoinRecord(
        coin=_coin(d.get("coin", {})),
        confirmed_block_index=int(d.get("confirmed_block_index", 0) or 0),
        spent_block_index=spent_index,
        # Some nodes drop the deprecated "spent" bool; derive it from the index.
        spent=bool(d.get("spent")) or spent_index > 0,
        coinbase=bool(d.get("coinbase")),
        timestamp=(int(d["timestamp"]) if d.get("timestamp") is not None else None),
    )


# --------------------------------------------------------------------------- #
# Client.
# --------------------------------------------------------------------------- #

def _default_poster(timeout: float) -> Poster:
    import requests  # lazy: only when a real network call is made

    session = requests.Session()

    def post(url: str, body: dict) -> dict:
        resp = session.post(url, json=body, timeout=timeout)
        resp.raise_for_status()
        return resp.json()

    return post


class CoinsetClient:
    """Minimal client for the coinset endpoints the claim path uses."""

    def __init__(
        self,
        base_url: str,
        *,
        poster: Optional[Poster] = None,
        timeout: float = 30.0,
    ) -> None:
        self._base = base_url.rstrip("/")
        self._timeout = timeout
        self._poster = poster or _default_poster(timeout)

    def _post(self, endpoint: str, body: dict) -> dict:
        url = f"{self._base}/{endpoint}"
        try:
            data = self._poster(url, body)
        except CoinsetError:
            raise
        except Exception as exc:  # noqa: BLE001 -- unify transport failures
            raise CoinsetError(f"{endpoint} request failed: {exc}") from exc
        if not isinstance(data, dict):
            raise CoinsetError(f"{endpoint}: unexpected response {type(data).__name__}")
        if data.get("success") is False:
            raise CoinsetError(f"{endpoint} failed: {data.get('error') or 'unknown'}")
        return data

    # -- reads --------------------------------------------------------------- #

    def get_blockchain_state(self) -> dict:
        return self._post("get_blockchain_state", {}).get("blockchain_state", {}) or {}

    def get_peak_height(self) -> int:
        state = self.get_blockchain_state()
        peak = state.get("peak") or {}
        return int(peak.get("height", 0) or 0)

    def get_coin_record_by_name(self, name: str) -> Optional[CoinRecord]:
        data = self._post("get_coin_record_by_name", {"name": _0x(name)})
        record = data.get("coin_record")
        return _coin_record(record) if record else None

    def get_coin_records_by_puzzle_hash(
        self,
        puzzle_hash: str,
        *,
        include_spent: bool = False,
        start_height: Optional[int] = None,
        end_height: Optional[int] = None,
    ) -> list[CoinRecord]:
        body: dict[str, Any] = {
            "puzzle_hash": _0x(puzzle_hash),
            "include_spent_coins": include_spent,
        }
        if start_height is not None:
            body["start_height"] = int(start_height)
        if end_height is not None:
            body["end_height"] = int(end_height)
        data = self._post("get_coin_records_by_puzzle_hash", body)
        return [_coin_record(r) for r in (data.get("coin_records") or [])]

    def get_puzzle_and_solution(self, coin_id: str, height: int) -> CoinSolution:
        data = self._post(
            "get_puzzle_and_solution",
            {"coin_id": _0x(coin_id), "height": int(height)},
        )
        sol = data.get("coin_solution")
        if not sol:
            raise CoinsetError(
                f"get_puzzle_and_solution: coin {_hx(coin_id)} not spent at "
                f"height {height}"
            )
        return CoinSolution(
            coin=_coin(sol.get("coin", {})),
            puzzle_reveal=_hx(sol.get("puzzle_reveal")),
            solution=_hx(sol.get("solution")),
        )

    # -- write --------------------------------------------------------------- #

    def push_tx(self, spend_bundle: dict) -> str:
        """Submit a spend bundle; returns the node's status string.

        A mempool rejection surfaces as :class:`CoinsetError` (``success=false``);
        an already-known bundle is reported by the node as success and must be
        treated as idempotent by the caller.
        """
        data = self._post("push_tx", {"spend_bundle": spend_bundle})
        return str(data.get("status", "SUCCESS"))
