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
adapts a full-node RPC client, which is coinset.org or the operator's own node.

:func:`bundle_status` reads the outcome of a broadcast, or of an offer, from the
chain, which is the only place the reference wallet lets us learn it.

ASKING AS LITTLE AS POSSIBLE. The node behind a reader is usually somebody
else's (coinset.org), so every request is one to avoid. :class:`CachedChain`
wraps a reader and makes three savings:

* What is settled is never asked for again. A coin that was spent stays spent,
  and how it was spent does not change. Both are kept on disk, and trusted
  without another look once the spend is old enough that a reorg cannot undo
  it.
* What can still change is asked for together. The coins a caller looked at
  last time are fetched in ONE request the next time it looks.
* The caller decides when to look (:meth:`CachedChain.begin`). Between looks
  every answer comes from the last one, at no cost.
"""

from __future__ import annotations

import dataclasses
import enum
import time
from collections.abc import Callable, Iterable, Sequence
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

    def coin_spend(self, coin_id: bytes) -> Any:
        """A spent coin's ``.coin``, ``.puzzle_reveal`` and ``.solution``, or None."""


class NodeChainReader:
    """Adapts a full-node RPC client to :class:`ChainReader`.

    ``client`` is ``gui.services.warp.coinset.CoinsetClient`` or anything shaped
    like it, pointed at coinset.org or at the operator's own full node; the RPC
    is the same on both. Only two endpoints are used:

    * ``get_coin_records_by_names(names, include_spent=True) -> records``: any
      number of coins in one request. A coin the node does not know is absent
      from the answer rather than an error, on coinset and on a real node alike.
    * ``get_puzzle_and_solution(hex, height) -> solution``.

    Records have ``.coin`` (hex ``parent_coin_info`` / ``puzzle_hash``, int
    ``amount``), ``.confirmed_block_index``, ``.spent_block_index`` and
    ``.spent``; solutions have hex ``.puzzle_reveal`` and ``.solution``. A client
    with only ``get_coin_record_by_name(hex) -> record | None`` is still served,
    one request per coin.
    """

    def __init__(self, client: Any) -> None:
        self._client = client

    @staticmethod
    def _coin(record: Any) -> Coin:
        coin = record.coin
        return _sdk().Coin(
            _unhex(coin.parent_coin_info), _unhex(coin.puzzle_hash), int(coin.amount)
        )

    def _state(self, record: Any) -> Any:
        spent = bool(getattr(record, "spent", False)) or int(record.spent_block_index or 0) > 0
        return _sdk().CoinState(
            self._coin(record),
            int(record.spent_block_index) if spent else None,
            int(record.confirmed_block_index),
        )

    def coin_states(self, coin_ids: Iterable[bytes]) -> dict[bytes, Any]:
        """The state of each coin the node knows, in one request where it can."""
        wanted = [bytes(coin_id) for coin_id in coin_ids]
        if not wanted:
            return {}
        together = getattr(self._client, "get_coin_records_by_names", None)
        try:
            if together is not None:
                records = together([coin_id.hex() for coin_id in wanted], include_spent=True)
            else:
                looked_up = (
                    self._client.get_coin_record_by_name(coin_id.hex()) for coin_id in wanted
                )
                records = [record for record in looked_up if record is not None]
        except Exception as exc:
            raise ChainError(f"coin record lookup failed: {exc}") from exc
        states = {}
        for record in records:
            state = self._state(record)
            states[bytes(state.coin.coin_id())] = state
        # A node answers for the names it was asked; anything else is not ours.
        return {coin_id: states[coin_id] for coin_id in wanted if coin_id in states}

    def coin_state(self, coin_id: bytes) -> Any:
        return self.coin_states([coin_id]).get(bytes(coin_id))

    def spend_at(self, coin: Coin, spent_height: int) -> Any:
        """How a coin known to be spent at ``spent_height`` was spent. One request."""
        try:
            solution = self._client.get_puzzle_and_solution(
                bytes(coin.coin_id()).hex(), int(spent_height)
            )
        except Exception as exc:
            raise ChainError(f"puzzle and solution lookup failed: {exc}") from exc
        return _sdk().CoinSpend(coin, _unhex(solution.puzzle_reveal), _unhex(solution.solution))

    def coin_spend(self, coin_id: bytes) -> Any:
        state = self.coin_state(coin_id)
        if state is None or state.spent_height is None:
            return None
        return self.spend_at(state.coin, int(state.spent_height))

    def chain_time(self) -> int | None:
        """The timestamp of the node's latest transaction block, or None if it cannot say.

        The clock timelocks are judged by. One request, two when the node's
        peak is not a transaction block.
        """
        ask = getattr(self._client, "get_chain_time", None)
        if ask is None:
            return None
        try:
            stamp = ask()
        except Exception as exc:
            raise ChainError(f"chain time lookup failed: {exc}") from exc
        return None if stamp is None else int(stamp)


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
# Asking as little as possible
# ---------------------------------------------------------------------------


class FactStore(Protocol):
    """Where what was read about spent coins is kept. ``store.OptionStore`` provides it."""

    def chain_fact(self, coin_id: bytes) -> Any:
        """The kept ``store.ChainFact`` for ``coin_id``, or None."""

    def remember_chain_fact(self, fact: Any) -> None:
        """Keep a fact, replacing any kept before for the same coin."""


class _Memory:
    """A :class:`FactStore` that lasts as long as the process."""

    def __init__(self) -> None:
        self._facts: dict[bytes, Any] = {}

    def chain_fact(self, coin_id: bytes) -> Any:
        return self._facts.get(bytes(coin_id))

    def remember_chain_fact(self, fact: Any) -> None:
        self._facts[bytes(fact.coin_id)] = fact


class CachedChain:
    """A :class:`ChainReader` that asks its node only for what may have changed.

    LOOKS. :meth:`begin` starts a look. Until the next one, a coin's state is
    whatever this look found, so calling :meth:`coin_state` a hundred times
    costs one request at most. The first lookup of a look fetches, in a single
    request, every coin that was asked about during the look before: the
    reader learns what its caller watches by being asked. A caller that never
    calls :meth:`begin` gets one long look, which is right for a one-shot
    command.

    SETTLED. The first time a coin is seen spent, the fact is written down with
    the time. Seen spent again at the same height ``final_after_seconds`` or
    more later (fifteen minutes by default, some forty blocks: beyond any reorg
    seen on mainnet) it is final: its state and its spend are answered from
    ``facts`` and never requested again, by this process or the next. Until
    then it is re-read with everything else at each look, which costs nothing
    extra, and a spend that moved to another height is read again. How a coin
    was spent is kept from the first time it is read, so even an unsettled
    spend is fetched once.

    ``requests`` counts the round trips actually made, so a caller can report
    them and a test can hold them to a number.
    """

    def __init__(
        self,
        inner: Any,
        facts: FactStore | None = None,
        *,
        clock: Callable[[], float] = time.time,
        final_after_seconds: float = 900.0,
        batch_size: int = 200,
    ) -> None:
        self._inner = inner
        self._facts: FactStore = _Memory() if facts is None else facts
        self._clock = clock
        self._final_after = float(final_after_seconds)
        self._batch = max(1, int(batch_size))
        self._final: dict[bytes, Any] = {}  # coin id -> a final store.ChainFact
        self._spent: dict[bytes, Any] = {}  # coin id -> a fact not yet final
        self._seen: dict[bytes, Any] = {}  # this look: coin id -> state, or None
        self._asked: set[bytes] = set()  # coins not final that were asked about this look
        self._due: set[bytes] = set()  # to fetch together at the next lookup
        self.requests = 0

    @classmethod
    def over(
        cls, reader: Any, facts: FactStore | None, *, clock: Callable[[], float] = time.time
    ) -> CachedChain:
        """``reader`` itself if it already caches, else a cache around it."""
        return reader if isinstance(reader, cls) else cls(reader, facts, clock=clock)

    # -- looks ---------------------------------------------------------------

    def begin(self) -> None:
        """Start a new look: what can still change is read afresh, together."""
        self._due |= self._asked
        self._asked = set()
        self._seen = {}

    def chain_time(self) -> int | None:
        """The chain's own clock, asked afresh every time, or None if the node cannot say.

        Ask it BEFORE starting the look whose answers it is to be compared
        with. "The chain had reached this time, and after that this coin was
        still unspent" is proof a timelocked spend can no longer confirm; the
        other order is not.
        """
        ask = getattr(self._inner, "chain_time", None)
        if ask is None:
            return None
        self.requests += 1
        return ask()

    def want(self, coin_ids: Iterable[bytes]) -> None:
        """Say which coins are about to be asked for, so they travel together."""
        for coin_id in coin_ids:
            coin_id = bytes(coin_id)
            if coin_id not in self._seen and self._settled(coin_id) is None:
                self._due.add(coin_id)

    # -- kept facts ------------------------------------------------------------

    def _settled(self, coin_id: bytes) -> Any:
        """The final fact about ``coin_id``, or None while it can still change."""
        fact = self._final.get(coin_id)
        if fact is None and coin_id not in self._seen and coin_id not in self._spent:
            kept = self._facts.chain_fact(coin_id)
            if kept is not None and kept.final:
                self._final[coin_id] = fact = kept
        return fact

    def _keep(self, fact: Any) -> None:
        self._facts.remember_chain_fact(fact)
        if fact.final:
            self._final[fact.coin_id] = fact
            self._spent.pop(fact.coin_id, None)
            self._seen.pop(fact.coin_id, None)
            self._asked.discard(fact.coin_id)
        else:
            self._spent[fact.coin_id] = fact

    @staticmethod
    def _coin(fact: Any) -> Coin:
        return _sdk().Coin(fact.parent, fact.puzzle_hash, int(fact.amount))

    def _state(self, fact: Any) -> Any:
        return _sdk().CoinState(self._coin(fact), int(fact.spent_height), fact.created_height)

    # -- reading ---------------------------------------------------------------

    def _observe(self, coin_id: bytes, state: Any, now: float) -> None:
        from gui.services.options.store import ChainFact  # a plain record; no cycle at import

        self._seen[coin_id] = state
        if state is None or state.spent_height is None:
            # Unspent, or gone. Whatever was kept about a spend no longer stands;
            # it is overwritten if the coin is ever seen spent again.
            self._spent.pop(coin_id, None)
            return
        height = int(state.spent_height)
        fact = self._spent.get(coin_id) or self._facts.chain_fact(coin_id)
        if fact is None or int(fact.spent_height) != height:
            # First sight, or a reorg moved the spend: start counting from now.
            fact = ChainFact(
                coin_id=coin_id,
                parent=bytes(state.coin.parent_coin_info),
                puzzle_hash=bytes(state.coin.puzzle_hash),
                amount=int(state.coin.amount),
                created_height=None if state.created_height is None else int(state.created_height),
                spent_height=height,
                seen_at=int(now),
            )
            self._keep(fact)
        elif now - fact.seen_at >= self._final_after:
            self._keep(dataclasses.replace(fact, final=True))
        else:
            self._spent[coin_id] = fact

    def _fetch(self, coin_ids: set[bytes]) -> None:
        wanted = sorted(coin_id for coin_id in coin_ids if coin_id not in self._final)
        together = getattr(self._inner, "coin_states", None)
        now = self._clock()
        for start in range(0, len(wanted), self._batch):
            chunk = wanted[start : start + self._batch]
            if together is not None:
                self.requests += 1
                found = together(chunk)
            else:
                self.requests += len(chunk)
                found = {coin_id: self._inner.coin_state(coin_id) for coin_id in chunk}
            for coin_id in chunk:
                self._observe(coin_id, found.get(coin_id), now)

    def coin_state(self, coin_id: bytes) -> Any:
        coin_id = bytes(coin_id)
        fact = self._settled(coin_id)
        if fact is not None:
            return self._state(fact)
        self._asked.add(coin_id)
        if coin_id not in self._seen:
            due, self._due = self._due | {coin_id}, set()
            self._fetch(due)
            fact = self._final.get(coin_id)
            if fact is not None:  # this very read made it final
                return self._state(fact)
        return self._seen[coin_id]

    def coin_spend(self, coin_id: bytes) -> Any:
        coin_id = bytes(coin_id)
        state = self.coin_state(coin_id)
        if state is None or state.spent_height is None:
            return None
        fact = self._final.get(coin_id) or self._spent.get(coin_id)
        if fact is None or int(fact.spent_height) != int(state.spent_height):
            return None  # not reachable: a spent coin always has a fact by now
        if fact.puzzle_reveal is None:
            exact = getattr(self._inner, "spend_at", None)
            self.requests += 1
            if exact is not None:
                spend = exact(state.coin, int(state.spent_height))
            else:
                spend = self._inner.coin_spend(coin_id)
            if spend is None:
                return None
            fact = dataclasses.replace(
                fact, puzzle_reveal=bytes(spend.puzzle_reveal), solution=bytes(spend.solution)
            )
            self._keep(fact)
        return _sdk().CoinSpend(self._coin(fact), fact.puzzle_reveal, fact.solution)


# ---------------------------------------------------------------------------
# CAT coins
# ---------------------------------------------------------------------------


def load_cat(chain: ChainReader, coin_id: bytes) -> Cat:
    """Rebuild a spendable CAT (coin, lineage proof, asset id) from its parent's spend."""
    state = _state(chain, coin_id, "the CAT coin")
    if state is None:
        raise ChainError(f"coin {bytes(coin_id).hex()} is not on chain")
    return load_cat_coin(chain, state.coin)


def load_cat_coin(chain: ChainReader, coin: Coin) -> Cat:
    """:func:`load_cat` for a coin already in hand, as a wallet reports its own.

    Only the parent's spend is read. That spend is settled, so a caching reader
    answers it from memory ever after and a wallet's CAT coins cost nothing to
    list again.
    """
    sdk = _sdk()
    coin_id = bytes(coin.coin_id())
    parent = _spend(chain, coin.parent_coin_info, "the CAT coin's parent")
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
# Outcome of a broadcast
# ---------------------------------------------------------------------------


class BundleStatus(enum.Enum):
    PENDING = "pending"  # at least one of its coins is still unspent
    CONFIRMED = "confirmed"  # every coin was spent exactly as the bundle spends it
    CONFLICT = "conflict"  # a coin was spent some other way; the bundle can never confirm


def _tree_hash(clvm: Any, serialized: bytes) -> bytes:
    """A program's tree hash, whichever serialization the node chose to return."""
    try:
        return bytes(clvm.deserialize(bytes(serialized)).tree_hash())
    except Exception:
        return bytes(clvm.deserialize_with_backrefs(bytes(serialized)).tree_hash())


def bundle_status(chain: ChainReader, coin_spends: Sequence[CoinSpend]) -> BundleStatus:
    """What became of ``coin_spends``, read from the chain.

    The reference wallet's ``push_tx`` hands a bundle to its full-node peer and
    returns; a rejection is never reported back. So the outcome of a broadcast
    has to be read from the chain: a bundle has confirmed when each of its coins
    is spent with the very puzzle and solution the bundle carries.

    The same test tells an offer's fate. An offer is a set of signed coin
    spends, so ``CONFIRMED`` on ``OfferDraft.coin_spends`` means it was taken,
    and ``CONFLICT`` means the option coin went somewhere else first (a newer
    quote was taken, or we cancelled, exercised or transferred it).

    ``PENDING`` covers both "not yet mined" and "never accepted"; only time and
    ``CONFLICT`` separate them, so callers need their own deadline.
    """
    if not coin_spends:
        raise ValueError("no coin spends to look up")
    sdk = _sdk()
    clvm = sdk.Clvm()
    together = getattr(chain, "want", None)
    if together is not None:
        together(bytes(coin_spend.coin.coin_id()) for coin_spend in coin_spends)
    unspent = False
    for coin_spend in coin_spends:
        coin_id = bytes(coin_spend.coin.coin_id())
        state = _state(chain, coin_id, "a coin of the bundle")
        if state is None or state.spent_height is None:
            unspent = True
            continue
        on_chain = _spend(chain, coin_id, "a coin of the bundle")
        if on_chain is None:
            raise ChainError("a spent coin's spend is not available")
        try:
            same = _tree_hash(clvm, on_chain.puzzle_reveal) == _tree_hash(
                clvm, coin_spend.puzzle_reveal
            ) and _tree_hash(clvm, on_chain.solution) == _tree_hash(clvm, coin_spend.solution)
        except Exception as exc:
            raise ChainError(f"could not compare a spend with the chain: {exc}") from exc
        if not same:
            return BundleStatus.CONFLICT
    return BundleStatus.PENDING if unspent else BundleStatus.CONFIRMED


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
