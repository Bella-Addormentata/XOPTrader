"""The options book: one idempotent step that keeps a grid of contracts on offer.

``OptionsBook.step()`` is a pure state machine in warp's sense: every client it
needs is injected, it holds no thread and no timer, and running it twice does
nothing the first run did not already do. A shell (:mod:`runtime`) decides how
often to call it.

One step, in order:

1. **Settle broadcasts.** Each bundle we pushed is looked up on chain
   (:func:`chain.bundle_status`), because the wallet's ``push_tx`` reports
   nothing. A confirmed mint opens its contract; a mint that lost its coins to
   something else failed and locked nothing. A bundle still pending is pushed
   again, which is harmless. Past its deadline it is flagged as stalled and
   nothing else changes: it is still signed and may yet confirm, so its coins
   stay set aside and it keeps being pushed until the chain shows it confirmed
   or overtaken.
2. **Reconcile contracts** against the chain. An option we no longer own was
   sold (the offer that was taken tells us the premium). Collateral gone from a
   sold contract means it was exercised. An expired contract whose collateral
   is still locked is clawed back, including one whose cancel was still in
   flight at expiry: that cancel can never confirm, so the clawback is sent
   beside it and the chain takes one of the two.
3. **Read the market** and refuse to quote from one that is stale, crossed, too
   wide, or whose spot lies outside what Dexie would actually fill.
4. **Plan** (:func:`quoting.plan`) and act: retire one stale contract, mint one
   wanted contract, and refresh the offers on everything unsold.

Every bundle passes :func:`safety.require` against what the operation is
declared to do before the wallet is asked to sign it, and is written to the
store before it is pushed. What a chain verdict changes is written in one
transaction, so a crash leaves a verdict unread, never half applied.

DRY RUN builds and checks the next mint and reports it. It signs nothing,
pushes nothing, uploads nothing and writes no contract. That holds over a book
that already has contracts: the chain is still read and the store brought up to
date, but nothing pending is pushed again and no clawback is sent; the step
says what it would have done. Collateral past expiry waits safely, since only
its creator can move it.

ONE AT A TIME. A step holds the store's acting lock. A second process over the
same database skips its step instead of planning from the same rows.

THE CHAIN IS ASKED AS LITTLE AS POSSIBLE. The node behind the chain reader is
usually coinset.org, so the book reads through :class:`chain.CachedChain` and
decides, each step, whether to look at the chain at all (:meth:`OptionsBook._look`).
It looks when something may have happened: the wallet's own coins changed (a
premium or a strike payment arrived, a mint or a cancel confirmed), a
transaction of ours is waiting, a contract has just reached expiry, or the
routine interval has passed. Otherwise the step runs on what the last look
found and makes no request. A look is one request for every coin being watched,
plus one for each spend not read before. Acting on a view that has gone stale
cannot lose money, because every spend is judged by the chain itself: an offer
for a coin already sold cannot be taken, and a cancel of one never confirms and
is settled as overtaken at the next look.

TRANSACTIONS THAT RUN OUT. Every transaction carries a time after which the
chain refuses it (``builder``'s ``valid_until_seconds``): ``confirm_timeout_seconds``
for a mint, a cancel or a clawback, and for a withdrawal the moment the last
offer it was sent to kill expires. One that has not confirmed by then never
will, so once the chain's own clock is past that time it is given up without
another transaction: its coins and its slot are free, a lapsed mint is simply
minted again at the current strike, a contract whose withdrawal or cancel
lapsed goes back on offer, and a lapsed clawback is signed again.

The clock that decides is the chain's, read 32 blocks below the peak, never the
local one and never the tip's. A single block at the tip can carry a timestamp
well ahead of its neighbours and then be reorganised away, taking the "too
late" with it and leaving the old bundle free to confirm beside its
replacement. 32 blocks is the depth the engine waits before retiring an expired
offer (``offer_expiry.hpp``), about ten minutes. And no deadline is set at all
when the chain reader cannot tell the chain's time: a bundle that could run out
but never be seen to would hold its coins for ever.

WITHDRAWALS ARE RARE BY CONSTRUCTION. Spending the option coin to kill a stale
offer is an on-chain transaction, so it is kept for when it matters
(``withdraw_pct``), skipped when the stale offer will run out before a
withdrawal could confirm (``withdraw_min_life_seconds``), followed by a pause
in quoting that contract (``withdraw_cooldown_seconds``) so a trending market
cannot turn it into a loop, and capped across the book
(``max_withdrawals_per_hour``). With the cap spent, a contract that would need
one is not re-quoted until its stale offers have run out by themselves.

FEES. ``fee_mojos`` is a fixed fee per transaction, or ``auto``: each bundle is
priced from its own cost, never above ``limits.max_fee_mojos``. The rate is the
larger of two things (:mod:`fees` says why). What the wallet's node estimates,
lifted to the rate a full mempool admits as soon as it shows any pressure. And
what the book has learned: a transaction of ours that ran out unconfirmed is
proof its fee was too low, so the rate is raised to at least double what it
paid, kept in the database, and allowed to fade after some hours without
another failure. Since a lapsed transaction is always signed again, raising
the rate is also how a stuck one gets a better fee.

TIME. Expiry on chain is judged against the previous transaction block's
timestamp, which trails the clock. This book uses the clock. The difference
only ever makes it early, and an early clawback is simply refused by the node
and pushed again on the next step.
"""

from __future__ import annotations

import dataclasses
import logging
import time
from collections.abc import Callable
from typing import TYPE_CHECKING, Any, Protocol

from gui.services.options import builder, chain, fees, quoting, safety, signing, store
from gui.services.options.config import OptionsConfig
from gui.services.options.desk import DeskError
from gui.services.options.market import Market, MarketError
from gui.services.options.splash import SplashError

if TYPE_CHECKING:
    from chia_wallet_sdk import Cat, Coin, CoinSpend, PublicKey

log = logging.getLogger(__name__)

#: ``extra`` on a log record whose words a :class:`StepReport` also carries.
#: The command line prints the report, so its console handler drops these and
#: only the log file keeps them (with a time on each line).
REPORTED = {"options_report": True}

#: Step verdicts, as the step journal (``store.step_log``) keeps them.
WALLET_NOT_READY = "wallet not ready"
ENGINE_REFUSED = "refused: trading engine may be running"
STEP_FAILED = "step failed"

#: A pending bundle is pushed again once it is this old, in case the first push
#: never reached a node.
_REPUSH_AFTER_SECONDS = 120

#: A bundle is given up once the chain's clock, read this many blocks below
#: the peak, has passed the time it stopped being valid: the block that put it
#: out of time is then buried that deep. A depth, not a number of seconds,
#: because timestamps only have to increase and one block at the tip can carry
#: the clock minutes forward and then be reorganised away. 32 is what the
#: engine waits before retiring an expired offer (kExpiredRetireDepthBlocks,
#: cpp/include/xop/execution/offer_expiry.hpp).
_LAPSE_DEPTH_BLOCKS = 32

#: About how long 32 blocks take. The chain's clock is not asked before a
#: bundle is this far past its time by our own.
_LAPSE_AFTER_SECONDS = 600

#: How long the wallet's fee estimate is used before it is asked for again.
_FEE_RATE_SECONDS = 300

#: A fee rate raised after a failure halves once per this long without another.
_FEE_MEMORY_SECONDS = 6 * 3600


class OptionsError(RuntimeError):
    """The book could not act. The step stops; the next one tries again."""


class Wallet(Protocol):
    """What the book needs from a signer. ``ReferenceWallet`` provides it."""

    def synthetic_key(self, puzzle_hash: bytes) -> PublicKey:
        """The synthetic public key of one of the wallet's own addresses."""

    def sign(self, coin_spends: Any) -> bytes:
        """The aggregated signature for ``coin_spends``. Broadcasts nothing."""

    def push(self, coin_spends: Any, aggregated_signature: bytes) -> bytes:
        """Hand a signed bundle to a node. No verdict comes back."""

    def not_ready(self, *, select_key: bool = True) -> str | None:
        """Why the wallet cannot be relied on right now, or None.

        Selects the configured key first unless ``select_key`` is False, in
        which case a wallet on another key is simply reported.
        """

    def fee_rate(self) -> int | None:
        """Mojos per unit of cost for prompt inclusion, or None if it cannot say."""


class CoinSource(Protocol):
    """The options address and the spendable coins sitting at it."""

    def puzzle_hash(self) -> bytes:
        """The one address used as creator, owner and change."""

    def xch_coins(self) -> list[Coin]:
        """Spendable XCH coins at that address."""

    def wallet_xch_coins(self) -> list[Coin]:
        """Spendable XCH coins at any address of the wallet, that one included.

        Asked only with ``options.wallet.fund_from_wallet``. The wallet already
        leaves out coins its own resting offers and pending transactions hold.
        """

    def wallet_cat_coins(self, asset_id: bytes) -> list[Coin]:
        """Spendable coins of one CAT at any address of the wallet, without lineage.

        Asked only with ``fund_from_wallet``; :meth:`cat_of` then loads the few
        that are chosen, so the chain is not read for every one.
        """

    def cat_of(self, coin: Coin) -> Cat:
        """A CAT coin from :meth:`wallet_cat_coins`, with its lineage proof."""

    def cats(self, asset_id: bytes) -> list[Cat]:
        """Spendable coins of one CAT at that address, with lineage proofs."""

    def held(self, asset_ids: Any) -> Any:
        """Ids of the spendable coins there, in XCH and each given CAT.

        Asked of the wallet alone, every step, to notice that something
        happened without asking the chain.
        """


class Desk(Protocol):
    """Where offers are advertised. ``DeskClient`` provides it."""

    def upload_offer(self, offer: str) -> Any:
        """List an offer; returns a record with an ``offer_id``."""


class Splash(Protocol):
    """A Splash! daemon offers are handed to. ``SplashClient`` provides it."""

    def broadcast(self, offer: str) -> None:
        """Hand one offer over; raises ``SplashError`` if it was not taken."""


@dataclasses.dataclass
class StepReport:
    """What one step saw and did, for the log and the GUI."""

    at: int = 0
    dry_run: bool = False
    market: Market | None = None
    market_error: str = ""
    blocked: str = ""
    net_delta_xch: float = 0.0
    chain_looked: str = ""  # why this step read the chain afresh, or "" if it did not
    chain_requests: int = 0  # requests the step made of the chain's node
    actions: list[str] = dataclasses.field(default_factory=list)
    problems: list[str] = dataclasses.field(default_factory=list)
    #: The options address against options.target, per asset ("XCH", "quote
    #: asset"): (held, target) in mojos, collateral in open contracts counted.
    #: Filled by a step that checked its top-ups.
    sub_account: dict[str, tuple[int, int]] = dataclasses.field(default_factory=dict)

    def did(self, text: str) -> None:
        self.actions.append(text)
        log.info("[options] %s", text, extra=REPORTED)

    def problem(self, text: str) -> None:
        self.problems.append(text)
        log.warning("[options] %s", text, extra=REPORTED)


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise OptionsError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


def _paying(fee: int) -> str:
    return f", fee {fee} mojos" if fee else ""


def _cover(coins: list[Coin], short: int) -> list[Coin]:
    """Coins from the rest of the wallet covering ``short``, or [] if they cannot.

    The smallest single coin that covers it, so a large trading coin is not
    taken for a fee or a seed; failing that, the largest first.
    """
    covering = [coin for coin in coins if int(coin.amount) >= short]
    if covering:
        return [min(covering, key=lambda item: int(item.amount))]
    chosen: list[Coin] = []
    total = 0
    for coin in sorted(coins, key=lambda item: int(item.amount), reverse=True):
        chosen.append(coin)
        total += int(coin.amount)
        if total >= short:
            return chosen
    return []


def pack_bundle(coin_spends: list[CoinSpend], signature: bytes) -> bytes:
    """A signed bundle as the store keeps it."""
    sdk = _sdk()
    return bytes(sdk.SpendBundle(coin_spends, sdk.Signature.from_bytes(signature)).to_bytes())


def unpack_bundle(bundle: bytes) -> tuple[list[CoinSpend], bytes]:
    """The coin spends and aggregated signature of a stored bundle."""
    parsed = _sdk().SpendBundle.from_bytes(bytes(bundle))
    return list(parsed.coin_spends), bytes(parsed.aggregated_signature.to_bytes())


def reserved_coin_ids(book_store: store.OptionStore) -> set[bytes]:
    """Coins an unresolved broadcast already spends; they must not fund another.

    A stalled broadcast counts. Its bundle is still signed and can still
    confirm, so handing its coins to a second bundle would put two
    conflicting spends in flight. They are freed by chain evidence only: the
    bundle confirms, or one of its coins is spent some other way. The wallet
    cannot leave them out itself: it does not know about bundles pushed
    around it, and lists a coin as spendable until its spend confirms.
    """
    return {
        bytes(spend.coin.coin_id())
        for row in book_store.unresolved()
        for spend in unpack_bundle(row.bundle)[0]
    }


#: [OPTIONS-FENCE] A top-up whose outcome is not known yet.
_SEND_UNCERTAIN = "uncertain"


class OptionsBook:
    def __init__(
        self,
        config: OptionsConfig,
        *,
        book_store: store.OptionStore,
        wallet: Wallet,
        coins: CoinSource,
        chain_reader: chain.ChainReader,
        market: Callable[[], Market],
        desk: Desk | None = None,
        splash: Splash | None = None,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self._config = config
        self._store = book_store
        self._wallet = wallet
        self._coins = coins
        self._chain = chain.CachedChain.over(chain_reader, book_store, clock=clock)
        self._market = market
        self._desk = desk
        self._splash = splash
        self._splash_down = False  # the daemon failed once this step: ask no more
        self._not_ready: str | None = None  # why the wallet was not ready, last asked
        self._clock = clock
        self._keys: dict[bytes, Any] = {}
        #: Addresses of this wallet, other than the options address, whose coins
        #: have funded a bundle (``options.wallet.fund_from_wallet``). They are the
        #: key's own, so their keys go in custody and the safety check counts
        #: them as ours.
        self._funding_addresses: set[bytes] = set()
        self._loaded: dict[bytes, chain.LoadedOption] = {}
        self._spot = 0.0
        self._looked_at: int | None = None  # when the chain was last read afresh
        self._look_again_until = 0
        self._held: Any = None  # the wallet's coins as last listed
        self._chain_time: int | None = None  # the chain's clock, read before this step's look
        self._chain_time_asked_at: int | None = None
        self._rate: tuple[int, int] | None = None  # (when asked, mojos per cost)
        self._withdrawn_at: dict[bytes, int] = {}
        #: Seconds between steps, set by whatever steps the book on a timer
        #: (``run``, the app's Options page). The step journal keeps it, so the
        #: report can tell a long interval from a book that was not running.
        self.step_interval: float | None = None

    @property
    def chain(self) -> chain.CachedChain:
        """The reader the book reads the chain through, cache and all."""
        return self._chain

    # -- when to look at the chain --------------------------------------------

    def _held_now(self, report: StepReport) -> Any:
        try:
            return frozenset(self._coins.held((self._config.quote_asset_id,)))
        except (RuntimeError, OSError, ValueError) as exc:
            # The wallet being away must not stop the chain being read.
            report.problem(f"could not list the wallet's coins: {exc}")
            return None

    def _look(self, now: int, report: StepReport, *, always: bool = False) -> None:
        """Start a fresh look at the chain if something may have happened.

        Reasons, in order: the caller insists; a transaction of ours may have
        run out of time; nothing has been read yet; the wallet's coins changed
        (a payment arrived or a spend of ours confirmed); a transaction of ours
        is waiting and ``chain_poll_seconds`` have passed; a contract reached
        expiry since the last look; or ``chain_refresh_seconds`` have passed.
        With none of them, this step costs the chain's node nothing.
        """
        held = self._held_now(report)
        changed = held is not None and self._held is not None and held != self._held
        if held is not None:
            self._held = held
        fast = self._config.chain_poll_seconds
        if changed:
            # The node asked may be a block behind the wallet that noticed.
            self._look_again_until = now + 3 * fast
        # The clock is read before the look, so that "still unspent" in the
        # look is proof a timed-out transaction can no longer confirm.
        timed_out = self._read_chain_time(now, report)
        reason = ""
        if always:
            reason = "asked to"
        elif timed_out:
            reason = "a transaction ran out of time"
        elif self._looked_at is None:
            reason = "first look"
        elif changed:
            reason = "the wallet's coins changed"
        else:
            age = now - self._looked_at
            waiting = any(row.status == "pending" for row in self._store.unresolved())
            if (waiting or now < self._look_again_until) and age >= fast:
                reason = "a transaction is waiting" if waiting else "following a change"
            elif any(
                self._looked_at < item.expiration <= now
                for item in self._store.contracts(store.ACTIVE)
            ):
                reason = "a contract reached expiry"
            elif age >= self._config.chain_refresh_seconds:
                reason = "routine"
        if reason:
            self._chain.begin()
            self._looked_at = now
        report.chain_looked = reason

    def _read_chain_time(self, now: int, report: StepReport) -> bool:
        """Read the chain's clock if a transaction may have run out of time.

        Only asked when some unresolved bundle's ``valid_until`` is, by our own
        clock, comfortably behind us, and then at most once a minute. True if
        the chain has a time to give.
        """
        self._chain_time = None
        due = [
            row
            for row in self._store.unresolved()
            if row.valid_until is not None and now >= row.valid_until + _LAPSE_AFTER_SECONDS
        ]
        asked = self._chain_time_asked_at
        if not due or (asked is not None and now - asked < 60):
            return False
        self._chain_time_asked_at = now
        try:
            self._chain_time = self._chain.chain_time(_LAPSE_DEPTH_BLOCKS)
        except chain.ChainError as exc:
            report.problem(f"could not read the chain's clock: {exc}")
        return self._chain_time is not None

    # -- keys and coins -----------------------------------------------------

    def _address(self) -> bytes:
        return bytes(self._coins.puzzle_hash())

    def _key(self, puzzle_hash: bytes) -> Any:
        puzzle_hash = bytes(puzzle_hash)
        if puzzle_hash not in self._keys:
            self._keys[puzzle_hash] = self._wallet.synthetic_key(puzzle_hash)
        return self._keys[puzzle_hash]

    def _custody(self, *extra: bytes) -> builder.Custody:
        custody = builder.Custody()
        wanted = {self._address(), *self._funding_addresses, *(bytes(item) for item in extra)}
        for puzzle_hash in wanted:
            custody.add_key(self._key(puzzle_hash))
        return custody

    def _ours(self) -> set[bytes]:
        mine = {self._address(), *self._funding_addresses}
        mine.update(item.creator_puzzle_hash for item in self._store.contracts(store.ACTIVE))
        return mine

    def _reserved(self) -> set[bytes]:
        """Coins an unresolved broadcast already spends: see :func:`reserved_coin_ids`."""
        return reserved_coin_ids(self._store)

    def _listed(self, what: str, asset_id: bytes | None = None) -> list[Any]:
        """The wallet's coins, or :class:`builder.OptionBuildError` if it will not say.

        A wallet call that fails half way through a step is one transaction not
        built. It must not take the rest of the step with it: the offers still
        have to be kept fresh.
        """
        try:
            return self._coins.xch_coins() if asset_id is None else self._coins.cats(asset_id)
        except (RuntimeError, OSError, ValueError) as exc:
            raise builder.OptionBuildError(
                f"could not list the wallet's {what} coins: {exc}"
            ) from exc

    def _xch(self, needed: int) -> list[Coin]:
        """The fewest spendable XCH coins that cover ``needed`` mojos.

        The options address's own coins first. With ``fund_from_wallet``, what
        they leave short comes from the rest of the wallet: the smallest one
        coin that covers it, so a large trading coin is not swept into a fee,
        else the largest first. Change always goes to the options address.
        """
        if needed <= 0:
            return []
        reserved = self._reserved()
        coins = [coin for coin in self._listed("XCH") if bytes(coin.coin_id()) not in reserved]
        chosen: list[Coin] = []
        total = 0
        for coin in sorted(coins, key=lambda item: int(item.amount), reverse=True):
            chosen.append(coin)
            total += int(coin.amount)
            if total >= needed:
                return chosen
        if self._config.wallet.fund_from_wallet:
            topped_up = self._from_wallet(needed - total, reserved)
            if topped_up:
                self._funding_addresses.update(bytes(coin.puzzle_hash) for coin in topped_up)
                return chosen + topped_up
        where = "at the options address" + (
            " or elsewhere in the wallet" if self._config.wallet.fund_from_wallet else ""
        )
        waiting = " (the rest is set aside for transactions still waiting)" if reserved else ""
        raise builder.OptionBuildError(
            f"not enough XCH free {where}: need {needed} mojos, have {total}{waiting}"
        )

    def _from_wallet(self, short: int, reserved: set[bytes]) -> list[Coin]:
        """Coins elsewhere in the wallet that cover ``short`` mojos, or [] if they cannot."""
        address = self._address()
        try:
            listed = self._coins.wallet_xch_coins()
        except (RuntimeError, OSError, ValueError) as exc:
            raise builder.OptionBuildError(f"could not list the wallet's XCH coins: {exc}") from exc
        others = [
            coin
            for coin in listed
            if bytes(coin.puzzle_hash) != address and bytes(coin.coin_id()) not in reserved
        ]
        return _cover(others, short)

    def _cats(self, asset_id: bytes, needed: int) -> list[Cat]:
        """The fewest spendable coins of one CAT covering ``needed``; as :meth:`_xch` for XCH."""
        reserved = self._reserved()
        cats = [
            cat
            for cat in self._listed("quote asset", asset_id)
            if bytes(cat.coin.coin_id()) not in reserved
        ]
        chosen: list[Cat] = []
        total = 0
        for cat in sorted(cats, key=lambda item: int(item.coin.amount), reverse=True):
            chosen.append(cat)
            total += int(cat.coin.amount)
            if total >= needed:
                return chosen
        if self._config.wallet.fund_from_wallet:
            own = {bytes(cat.coin.coin_id()) for cat in cats}
            try:
                listed = self._coins.wallet_cat_coins(asset_id)
                picked = _cover(
                    [coin for coin in listed if bytes(coin.coin_id()) not in own | reserved],
                    needed - total,
                )
                topped_up = [self._coins.cat_of(coin) for coin in picked]
            except (RuntimeError, OSError, ValueError, chain.ChainError) as exc:
                raise builder.OptionBuildError(
                    f"could not list the wallet's quote asset coins: {exc}"
                ) from exc
            if topped_up:
                self._funding_addresses.update(bytes(cat.info.p2_puzzle_hash) for cat in topped_up)
                return chosen + topped_up
        where = "at the options address" + (
            " or elsewhere in the wallet" if self._config.wallet.fund_from_wallet else ""
        )
        raise builder.OptionBuildError(
            f"not enough of the quote asset {where}: need {needed}, have {total}"
        )

    # -- signing and broadcasting -------------------------------------------

    def _check(self, coin_spends: list[CoinSpend], expected: safety.Expected, lock: bytes) -> None:
        effect = safety.analyse(coin_spends, ours=self._ours(), locks=[lock])
        safety.require(effect, expected)

    def _broadcast(
        self,
        launcher_id: bytes,
        kind: str,
        coin_spends: list[CoinSpend],
        now: int,
        *,
        contract_status: str | None = None,
        valid_until: int | None = None,
        fee: int = 0,
    ) -> None:
        """Sign, record, then push. The record comes first."""
        signature = self._wallet.sign(coin_spends)
        self._store.add_broadcast(
            launcher_id=launcher_id,
            kind=kind,
            bundle=pack_bundle(coin_spends, signature),
            deadline=now + self._config.confirm_timeout_seconds,
            contract_status=contract_status,
            valid_until=valid_until,
            fee=fee,
            now=now,
        )
        self._wallet.push(coin_spends, signature)

    def _valid_until(self, at: int) -> int | None:
        """``at`` as a bundle's deadline, or None when it could never be seen to pass."""
        return int(at) if self._chain.tells_time else None

    # -- fees ---------------------------------------------------------------

    def _estimate(self, now: int, report: StepReport) -> int | None:
        """The wallet's node's estimate in mojos per cost, asked every few minutes."""
        if self._rate is None or now - self._rate[0] >= _FEE_RATE_SECONDS:
            rate = self._wallet.fee_rate()
            if rate is None:
                report.problem("the wallet gave no fee estimate; going by what was learned")
                return None  # not remembered: asked again next time
            self._rate = (now, int(rate))
        return self._rate[1]

    def _learned_rate(self, now: int) -> float:
        """The rate past failures taught, faded by the time since the last one."""
        try:
            rate = float(self._store.recall("fee_rate") or 0.0)
            since = int(self._store.recall("fee_rate_at") or now)
        except ValueError:
            return 0.0
        faded = fees.faded_rate(rate, now - since, _FEE_MEMORY_SECONDS)
        if faded != rate:
            periods = (now - since) // _FEE_MEMORY_SECONDS
            self._store.remember("fee_rate", repr(faded))
            self._store.remember("fee_rate_at", str(since + periods * _FEE_MEMORY_SECONDS))
        return faded

    def _fee_rate(self, now: int, report: StepReport) -> float:
        """Mojos per unit of cost to pay now: the estimate's floor, or what was learned."""
        return max(fees.pressure_rate(self._estimate(now, report)), self._learned_rate(now))

    def _learn_from_failure(self, row: store.Broadcast, now: int, report: StepReport) -> None:
        """A transaction ran out unconfirmed: most likely its fee was too low. Pay more.

        Two lapses say little about fees and teach nothing. A withdrawal has
        only as long as the offer it kills, which can be three minutes: missing
        that is ordinary. And when the wallet cannot give an estimate at all it
        has no node to ask, which is reason enough for a bundle not to arrive.
        Anything else counts even while the estimate reads nothing, because an
        estimate is the thing a failure calls into doubt.
        """
        if not self._config.fee_auto or self._config.limits.max_fee_mojos <= 0:
            return
        if row.kind == store.WITHDRAW:
            return
        if self._estimate(now, report) is None:
            report.problem(
                f"the {row.kind} that ran out is not taken as a sign of low fees: "
                "the wallet has no node to ask"
            )
            return
        try:
            cost = fees.bundle_cost(unpack_bundle(row.bundle)[0])
        except fees.FeeError:
            return
        paid = (row.fee or 0) / cost if cost > 0 else 0.0
        learned = self._learned_rate(now)
        raised = max(learned, fees.raised_rate(paid))
        self._store.remember("fee_rate", repr(raised))
        self._store.remember("fee_rate_at", str(now))
        if raised > learned:
            report.problem(
                f"fee rate raised to {raised:g} mojos per cost: a {row.kind} that paid "
                f"{paid:g} did not confirm"
            )

    def _priced(self, build: Callable[[int], Any], now: int, report: StepReport) -> tuple[Any, int]:
        """Build a bundle carrying the fee it should pay; returns it and the fee.

        ``build(fee)`` returns a tuple whose first item is the coin spends. With
        a fixed ``fee_mojos`` it is called once. With ``auto`` the bundle is
        built without a fee to learn what it costs, priced at the current rate
        under ``limits.max_fee_mojos``, and built again. Paying a fee can pull
        in another coin and so raise the cost, hence one further pass.
        """
        if not self._config.fee_auto:
            return build(self._config.fee_mojos), self._config.fee_mojos
        cap = self._config.limits.max_fee_mojos
        rate = self._fee_rate(now, report) if cap > 0 else 0.0
        built, fee = build(0), 0
        for _ in range(2):
            try:
                cost = fees.bundle_cost(built[0])
            except fees.FeeError as exc:
                raise builder.OptionBuildError(f"could not price the bundle: {exc}") from exc
            wanted = fees.fee_for(cost, rate, cap)
            if wanted <= fee:
                break
            built, fee = build(wanted), wanted
        if rate > 0 and fee >= cap:
            report.problem(
                f"a fee was held to limits.max_fee_mojos ({cap}); at {rate:g} mojos per cost "
                "it wanted more, and may not get in"
            )
        return built, fee

    # -- step 1: broadcasts -------------------------------------------------

    def _settle_broadcasts(self, now: int, report: StepReport, *, act: bool) -> None:
        for row in self._store.unresolved():
            coin_spends, signature = unpack_bundle(row.bundle)
            try:
                status = chain.bundle_status(self._chain, coin_spends)
            except chain.ChainError as exc:
                report.problem(f"could not check a {row.kind} broadcast: {exc}")
                continue
            name = row.launcher_id.hex()[:10]
            if status is chain.BundleStatus.CONFIRMED:
                self._confirmed(row, now, report)
                continue
            if status is chain.BundleStatus.CONFLICT:
                self._overtaken(row, now, report)
                continue
            if (
                row.valid_until is not None
                and self._chain_time is not None
                and self._chain_time >= row.valid_until
            ):
                # Unspent in a look begun after the chain's clock, read 32
                # blocks down, had passed the time this bundle refuses to be
                # included from: it can never confirm, short of a reorg that
                # deep. Its coins and its slot are free again.
                self._overtaken(row, now, report, lapsed=True)
                continue
            if row.status == "pending" and now >= row.deadline:
                # A flag for the operator, nothing more. The bundle is still
                # signed and can still confirm, so it keeps its coins and keeps
                # being pushed; only the chain resolves it.
                self._store.settle_broadcast(
                    row.id,
                    "stalled",
                    event="broadcast_stalled",
                    detail={"kind": row.kind},
                    now=now,
                )
            if row.status == "stalled" or now >= row.deadline:
                report.problem(f"{row.kind} of {name} has not confirmed; check the wallet's node")
            if act and now - row.created_at >= _REPUSH_AFTER_SECONDS:
                try:
                    self._wallet.push(coin_spends, signature)
                except signing.SigningError as exc:
                    report.problem(f"could not re-push the {row.kind} of {name}: {exc}")

    def _confirmed(self, row: store.Broadcast, now: int, report: StepReport) -> None:
        """Apply a confirmed broadcast: its verdict and its contract in one write."""
        contract = self._store.contract(row.launcher_id)
        if contract is None:
            self._store.settle_broadcast(row.id, "confirmed", now=now)
            return
        name = row.launcher_id.hex()[:10]
        asset = None if contract.underlying_asset is None else contract.underlying_asset.hex()
        # The fee the confirmed bundle paid, in XCH mojos (None if recorded before fees were).
        detail = {"asset": asset, "amount": contract.underlying_amount, "fee": row.fee}
        if row.kind == store.MINT:
            self._store.settle_broadcast(
                row.id,
                "confirmed",
                contract_status=store.OPEN,
                event="collateral_locked",
                detail=detail,
                now=now,
            )
            report.did(f"mint of {contract.slot} ({name}) confirmed")
        elif row.kind in (store.CANCEL, store.CLAWBACK):
            self._store.settle_broadcast(
                row.id,
                "confirmed",
                contract_status=store.CLOSED,
                kill_offers=True,
                event="collateral_returned",
                detail={**detail, "by": row.kind},
                now=now,
            )
            report.did(f"{row.kind} of {contract.slot} ({name}) confirmed")
        else:  # a withdrawal: the option coin moved to ourselves
            self._store.settle_broadcast(
                row.id,
                "confirmed",
                kill_offers=True,
                event="offers_withdrawn",
                detail={"fee": row.fee},
                now=now,
            )
            report.did(f"offers for {contract.slot} ({name}) withdrawn")

    def _overtaken(
        self, row: store.Broadcast, now: int, report: StepReport, *, lapsed: bool = False
    ) -> None:
        """Apply a broadcast that can never confirm.

        Either one of its coins went elsewhere, or (``lapsed``) the chain's
        clock passed the time it was valid until. The books change the same
        way; only what the operator is told differs.
        """
        name = row.launcher_id.hex()[:10]
        why = "lapsed" if lapsed else "overtaken"
        if lapsed:
            self._learn_from_failure(row, now, report)
        if row.kind == store.MINT:
            self._store.settle_broadcast(
                row.id,
                "conflict",
                contract_status=store.FAILED,
                event="mint_failed",
                detail={"reason": why},
                now=now,
            )
            if lapsed:
                report.problem(
                    f"mint of {name} did not confirm in time and no longer can; "
                    "nothing is locked, and its slot will be minted again"
                )
            else:
                report.problem(f"mint of {name} lost its coins to another spend")
            return
        # The coin moved some other way; reconciliation reads how. The contract
        # goes back to where it stood, unless another cancel or clawback of it
        # is still in flight (a clawback sent after a cancel missed expiry):
        # then that one settles it.
        back = None
        rival = None
        if row.kind in (store.CANCEL, store.CLAWBACK):
            for other in self._store.unresolved():
                same = other.launcher_id == row.launcher_id and other.id != row.id
                if same and other.kind in (store.CANCEL, store.CLAWBACK):
                    rival = other
            if rival is None:
                # Where it stood before. A clawback follows a sale, or an expiry
                # nobody bought: only the first of those was ever sold.
                sold = any(event.kind == "sold" for event in self._store.events(row.launcher_id))
                back = store.SOLD if row.kind == store.CLAWBACK and sold else store.OPEN
        self._store.settle_broadcast(
            row.id,
            "conflict",
            contract_status=back,
            only_from=store.CLOSING,
            event="broadcast_lapsed" if lapsed else None,
            detail={"kind": row.kind} if lapsed else None,
            now=now,
        )
        if rival is not None:
            report.did(f"{row.kind} of {name} gave way to the {rival.kind}")
        elif lapsed and row.kind == store.WITHDRAW:
            # The offers it was sent to kill have run out by themselves.
            report.did(f"withdrawal for {name} did not confirm in time; it is back on offer")
        elif lapsed:
            report.problem(f"{row.kind} of {name} did not confirm in time and no longer can")
        else:
            report.problem(f"{row.kind} of {name} was overtaken on chain")

    # -- step 2: contracts --------------------------------------------------

    def _reconcile(self, now: int, report: StepReport, *, act: bool) -> None:
        self._loaded = {}
        unresolved = self._store.unresolved()
        busy = {row.launcher_id for row in unresolved}
        clawing = {row.launcher_id for row in unresolved if row.kind == store.CLAWBACK}
        ours = self._ours()
        for contract in self._store.contracts((store.OPEN, store.SOLD, store.CLOSING)):
            name = contract.launcher_id.hex()[:10]
            try:
                loaded = chain.load_option(self._chain, contract.launcher_id, now_seconds=now)
            except chain.ChainError as exc:
                report.problem(f"could not read {name} from the chain: {exc}")
                continue
            except chain.InvalidOptionError as exc:
                report.problem(f"{name} no longer validates on chain: {exc}")
                continue
            self._loaded[contract.launcher_id] = loaded
            expired = loaded.status is chain.OptionStatus.EXPIRED  # collateral still locked

            if contract.status == store.CLOSING:
                # Its cancel or clawback settles it, with one exception. A
                # cancel still in flight at expiry can never confirm, and the
                # collateral would stay locked behind it. Send the clawback
                # beside it: the chain takes one and the other is overtaken.
                if expired and contract.launcher_id not in clawing:
                    self._clawback(contract, loaded, now, report, act=act)
                continue

            mine = loaded.owner_puzzle_hash in ours
            if contract.status == store.OPEN and not mine:
                if loaded.option is None and contract.launcher_id in busy:
                    continue  # melted, perhaps by our own cancel; its broadcast says which
                # Somebody else holds it, or held it just long enough to
                # exercise it between two steps. Either way one of our offers
                # was taken first, and the premium is booked from that offer.
                if not self._sold(contract, now, report):
                    continue
                contract = dataclasses.replace(contract, status=store.SOLD)

            if loaded.locked is None:
                if contract.launcher_id in busy:
                    continue  # our own cancel or clawback; the broadcast settles it
                strike = None if contract.strike_asset is None else contract.strike_asset.hex()
                self._store.set_status(
                    contract.launcher_id,
                    store.EXERCISED,
                    event="exercised",
                    detail={"strike_asset": strike, "strike_amount": contract.strike_amount},
                    kill_offers=True,
                    now=now,
                )
                report.did(f"{contract.slot} ({name}) was exercised")
            elif expired and contract.launcher_id not in busy:
                self._clawback(contract, loaded, now, report, act=act)

        for offer in self._store.offers(status="live"):
            if offer.expires_at is not None and offer.expires_at <= now:
                self._store.set_offer(offer.id, status="dead")

    def _sold(self, contract: store.Contract, now: int, report: StepReport) -> bool:
        """Record a sale: the taken offer says what was paid. False if it could not be read."""
        name = contract.launcher_id.hex()[:10]
        taken: store.OfferRow | None = None
        unread = False
        # Newest first: the offer a taker is likeliest to have held.
        for offer in reversed(self._store.offers(contract.launcher_id)):
            if offer.status == "taken":
                taken = offer
                break
            try:
                status = chain.bundle_status(self._chain, unpack_bundle(offer.bundle)[0])
            except chain.ChainError:
                unread = True
                continue
            if status is chain.BundleStatus.CONFIRMED:
                taken = offer
                break
        if taken is None and unread:
            # Booking the sale now would book it without its premium for good.
            report.problem(f"{contract.slot} ({name}) was sold; its offers could not be read yet")
            return False
        price: dict[str, Any] = {}
        if taken is not None:
            asset = None if taken.price_asset is None else taken.price_asset.hex()
            # offer_id finds how that price was worked out, in the quotes table.
            price = {"asset": asset, "amount": taken.price_amount, "offer_id": taken.id}
        self._store.set_status(
            contract.launcher_id,
            store.SOLD,
            event="sold",
            detail=price,
            taken_offer=None if taken is None else taken.id,
            kill_offers=True,
            now=now,
        )
        report.did(f"{contract.slot} ({name}) was sold")
        return True

    def _clawback(
        self,
        contract: store.Contract,
        loaded: chain.LoadedOption,
        now: int,
        report: StepReport,
        *,
        act: bool,
    ) -> None:
        if loaded.locked is None:
            return
        name = contract.launcher_id.hex()[:10]
        if not act:
            prefix = "dry run: " if report.dry_run else ""
            report.did(f"{prefix}would claw back {contract.slot} ({name})")
            return
        amount = contract.underlying_amount
        locked = loaded.locked
        # A clawback can always be signed again, so it runs out like the rest:
        # one whose fee was too small is replaced, at a better one, not waited for.
        valid_until = self._valid_until(now + self._config.confirm_timeout_seconds)

        def build(fee: int) -> tuple[list[CoinSpend], safety.Expected]:
            # An XCH underlying pays its own fee, unless the fee is the larger
            # of the two; a CAT always needs an XCH coin for one.
            other = contract.underlying_asset is not None or fee >= amount
            funding_xch = self._xch(fee) if other else []
            coin_spends = builder.build_clawback(
                loaded.underlying,
                locked,
                self._key(contract.creator_puzzle_hash),
                builder.Funding(change_puzzle_hash=self._address(), xch=funding_xch),
                self._custody(contract.creator_puzzle_hash),
                fee=fee,
                valid_until_seconds=valid_until,
            )
            if contract.underlying_asset is None:
                wallet = {None: amount - fee}
            else:
                wallet = {contract.underlying_asset: amount, None: -fee}
            return coin_spends, safety.Expected(
                wallet=wallet, locked={contract.underlying_asset: -amount}, max_fee=fee
            )

        try:
            (coin_spends, expected), fee = self._priced(build, now, report)
            self._check(coin_spends, expected, bytes(loaded.underlying.puzzle_hash()))
            self._broadcast(
                contract.launcher_id,
                store.CLAWBACK,
                coin_spends,
                now,
                contract_status=store.CLOSING,
                valid_until=valid_until,
                fee=fee,
            )
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            report.problem(f"clawback of {name} not sent: {exc}")
            return
        report.did(f"clawback of {contract.slot} ({name}) sent{_paying(fee)}")

    # -- step 4: acting on the plan -----------------------------------------

    def _cancel(self, contract: store.Contract, now: int, report: StepReport) -> None:
        loaded = self._loaded.get(contract.launcher_id)
        if loaded is None or loaded.option is None or loaded.locked is None:
            return
        name = contract.launcher_id.hex()[:10]
        amount = contract.underlying_amount
        option, locked = loaded.option, loaded.locked
        # Unconfirmed by then, it can no longer confirm, and the contract goes
        # back on the book to be looked at afresh.
        valid_until = self._valid_until(now + self._config.confirm_timeout_seconds)

        def build(fee: int) -> tuple[list[CoinSpend], safety.Expected]:
            # The strike asset needs one seed coin of any size; XCH also pays the fee.
            if contract.strike_asset is None:
                xch, cats = self._xch(max(fee, 1)), []
            else:
                xch, cats = self._xch(fee), self._cats(contract.strike_asset, 1)[:1]
            coin_spends = builder.build_cancel(
                option,
                loaded.underlying,
                locked,
                builder.Funding(change_puzzle_hash=self._address(), xch=xch, cats=cats),
                self._custody(contract.creator_puzzle_hash),
                fee=fee,
                valid_until_seconds=valid_until,
            )
            melted = builder.OPTION_COIN_AMOUNT
            if contract.underlying_asset is None:
                wallet = {None: amount + melted - fee}
            else:
                wallet = {contract.underlying_asset: amount, None: melted - fee}
            return coin_spends, safety.Expected(
                wallet=wallet, locked={contract.underlying_asset: -amount}, max_fee=fee
            )

        try:
            (coin_spends, expected), fee = self._priced(build, now, report)
            self._check(coin_spends, expected, bytes(loaded.underlying.puzzle_hash()))
            self._broadcast(
                contract.launcher_id,
                store.CANCEL,
                coin_spends,
                now,
                contract_status=store.CLOSING,
                valid_until=valid_until,
                fee=fee,
            )
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            # Left alone, it simply stops being offered and is clawed back at expiry.
            report.problem(f"could not retire {contract.slot} ({name}): {exc}")
            return
        report.did(f"retiring {contract.slot} ({name}){_paying(fee)}")

    def _build_mint(
        self, wanted: quoting.Wanted, fee: int, valid_until: int | None
    ) -> tuple[list[CoinSpend], builder.MintResult, safety.Expected]:
        amount = wanted.underlying.amount
        singleton = builder.OPTION_COIN_AMOUNT
        address = self._address()
        if wanted.underlying.is_xch:
            funding = builder.Funding(address, xch=self._xch(amount + singleton + fee))
            wallet = {None: -(amount + singleton + fee)}
        else:
            asset_id = wanted.underlying.asset_id
            assert asset_id is not None
            funding = builder.Funding(
                address, xch=self._xch(singleton + fee), cats=self._cats(asset_id, amount)
            )
            wallet = {None: -(singleton + fee), asset_id: -amount}
        minted = builder.build_mint(
            wanted.terms(address),
            funding,
            self._custody(),
            fee=fee,
            valid_until_seconds=valid_until,
        )
        expected = safety.Expected(
            wallet=wallet,
            locked={wanted.underlying.asset_id: amount},
            other_max={None: singleton},
            max_fee=fee,
        )
        return minted.coin_spends, minted, expected

    def _mint(self, plan: quoting.Plan, now: int, report: StepReport) -> None:
        # A mint is struck from the spot of the moment. Unconfirmed after this
        # long it can no longer confirm, and the slot is minted afresh.
        valid_until = self._valid_until(now + self._config.confirm_timeout_seconds)
        for wanted in plan.mint:
            try:
                (_, minted, expected), fee = self._priced(
                    lambda fee, wanted=wanted: self._build_mint(wanted, fee, valid_until),
                    now,
                    report,
                )
                self._check(minted.coin_spends, expected, bytes(minted.underlying.puzzle_hash()))
            except builder.OptionBuildError as exc:
                report.problem(f"cannot mint {wanted.slot}: {exc}")
                continue
            except signing.SigningError as exc:  # the wallet would not give the key
                report.problem(f"cannot mint {wanted.slot}: {exc}")
                return
            except safety.SafetyError as exc:
                report.problem(f"mint of {wanted.slot} refused by the safety check: {exc}")
                return
            label = (
                f"{wanted.slot} strike {wanted.strike:g} "
                f"({minted.launcher_id.hex()[:10]}){_paying(fee)}"
            )
            if self._config.dry_run:
                report.did(f"dry run: would mint {label}")
                return
            try:
                signature = self._wallet.sign(minted.coin_spends)
                self._store.add_contract(
                    launcher_id=minted.launcher_id,
                    slot=wanted.slot,
                    side=wanted.side,
                    underlying_asset=wanted.underlying.asset_id,
                    underlying_amount=wanted.underlying.amount,
                    strike_asset=wanted.strike_asset.asset_id,
                    strike_amount=wanted.strike_asset.amount,
                    expiration=wanted.expiration,
                    creator_puzzle_hash=self._address(),
                    bundle=pack_bundle(minted.coin_spends, signature),
                    deadline=now + self._config.confirm_timeout_seconds,
                    valid_until=valid_until,
                    fee=fee,
                    now=now,
                )
                self._wallet.push(minted.coin_spends, signature)
            except signing.SigningError as exc:
                report.problem(f"mint of {wanted.slot} not sent: {exc}")
                return
            report.did(f"minting {label}")
            return  # one mint a step: the next waits for this one's change

    def _offer(
        self,
        contract: store.Contract,
        ask: int,
        now: int,
        report: StepReport,
        quote: quoting.AskQuote | None = None,
    ) -> None:
        loaded = self._loaded.get(contract.launcher_id)
        if loaded is None or loaded.option is None:
            return
        live = [
            offer
            for offer in self._store.offers(contract.launcher_id, "live")
            if offer.expires_at is None or offer.expires_at > now
        ]
        name = contract.launcher_id.hex()[:10]
        sent = self._withdrawn_at.get(contract.launcher_id)
        if sent is not None and now - sent < self._config.withdraw_cooldown_seconds:
            # Its offers were just withdrawn because the price ran away from
            # them. Quoting again at once is how one withdrawal becomes a run
            # of them, so it sits out for a while with nothing on offer.
            return
        if live:
            newest = live[-1]
            notional = (
                quoting.contract_size_xch(contract) * self._spot * self._config.quote_unit_mojos
            )
            gap = notional * self._config.withdraw_pct / 100.0
            # Offers the ask has left far enough behind to be worth killing,
            # and with long enough left to live that a withdrawal, which takes
            # a block or two to confirm, could get there first.
            stale = [
                offer
                for offer in live
                if notional > 0
                and ask - offer.price_amount >= gap
                and (
                    offer.expires_at is None
                    or offer.expires_at - now > self._config.withdraw_min_life_seconds
                )
            ]
            if stale:
                since = now - 3600
                if self._store.withdrawals_since(since) < self._config.max_withdrawals_per_hour:
                    # A new offer would not withdraw the cheaper one. Spend the coin.
                    self._withdraw(contract, loaded, now, report)
                else:
                    # Out of withdrawals for this hour. Posting a dearer offer
                    # beside the stale one helps nobody, and every new offer in
                    # a market moving this fast is another that may need one.
                    report.problem(
                        f"the hour's withdrawals are used up; {contract.slot} ({name}) is not "
                        "re-quoted until its stale offer runs out"
                    )
                return
            fresh = (
                newest.expires_at is None
                or newest.expires_at - now > self._config.refresh_before_seconds
            )
            moved = abs(ask - newest.price_amount) / newest.price_amount * 100.0
            if fresh and moved < self._config.reprice_pct:
                # It stands. If the Splash daemon has not taken it yet, try again.
                if newest.splash_at is None:
                    self._announce(newest.id, newest.offer_text, contract, now, report)
                return
        address = self._address()
        try:
            draft = builder.build_offer(
                loaded.option,
                builder.Asset(self._config.quote_asset_id, ask),
                address,
                # The key of whichever of our addresses holds the option: after
                # options.wallet.address is changed, that is the old one.
                self._custody(loaded.owner_puzzle_hash),
                expires_at_seconds=now + self._config.offer_ttl_seconds,
            )
            self._check(
                draft.coin_spends,
                safety.Expected(other_max={None: builder.OPTION_COIN_AMOUNT}),
                bytes(loaded.underlying.puzzle_hash()),
            )
            signature = self._wallet.sign(draft.coin_spends)
            text = draft.encode(signature)
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            report.problem(f"offer for {contract.slot} ({name}) not made: {exc}")
            return
        offer_id = self._store.add_offer(
            launcher_id=contract.launcher_id,
            price_asset=self._config.quote_asset_id,
            price_amount=ask,
            expires_at=draft.expires_at_seconds,
            bundle=pack_bundle(draft.coin_spends, signature),
            offer_text=text,
            quote=None if quote is None or quote.units != ask else quote.as_record(),
            now=now,
        )
        price = ask / self._config.quote_unit_mojos
        how = ""
        if quote is not None:
            floor = "on the floor; " if quote.set_by == "floor" else ""
            how = f" ({floor}model value {quote.model:.4g} per XCH)"
        report.did(f"offering {contract.slot} ({name}) at {price:g}{how}")
        if self._desk is not None and self._config.upload_offers:
            try:
                upload = self._desk.upload_offer(text)
                self._store.set_offer(offer_id, desk_offer_id=str(upload.offer_id))
            except DeskError as exc:
                # The offer is valid without the site; it just is not advertised.
                report.problem(f"offer for {contract.slot} ({name}) not listed: {exc}")
        self._announce(offer_id, text, contract, now, report)

    def _announce(
        self, offer_id: int, text: str, contract: store.Contract, now: int, report: StepReport
    ) -> None:
        """Hand an offer to the Splash daemon, if one is configured.

        Once per offer: the network keeps no history, but each offer is re-made
        before it runs out and the new one is announced in its turn. Only the
        offer that currently stands for a contract is ever sent, never one the
        ask has left behind. An offer the daemon did not take is tried again on
        later steps while it stands; after one failure nothing more is sent in
        the same step, so a daemon that is down costs one request, not one for
        every contract.
        """
        if self._splash is None or self._splash_down:
            return
        try:
            self._splash.broadcast(text)
        except SplashError as exc:
            self._splash_down = True
            # The offer is valid without it; it just is not announced there.
            name = contract.launcher_id.hex()[:10]
            report.problem(
                f"offer for {contract.slot} ({name}) not announced on Splash: {exc}. "
                "It will be tried again while the offer stands"
            )
            return
        self._store.set_offer(offer_id, splash_at=now)

    def _withdraw(
        self,
        contract: store.Contract,
        loaded: chain.LoadedOption,
        now: int,
        report: StepReport,
    ) -> bool:
        """Spend the option coin back to ourselves, which kills every open offer."""
        option = loaded.option
        if option is None:
            return False
        name = contract.launcher_id.hex()[:10]
        address = self._address()
        # A withdrawal is only worth having while an offer it kills is alive.
        # Once the last of them has run out it must not confirm any more, so
        # that the contract can go back on offer without waiting for it.
        ends = [
            offer.expires_at
            for offer in self._store.offers(contract.launcher_id, "live")
            if offer.expires_at is None or offer.expires_at > now
        ]
        if ends and all(end is not None for end in ends):
            valid_until = self._valid_until(max(end for end in ends if end is not None))
        else:
            valid_until = self._valid_until(now + self._config.confirm_timeout_seconds)

        def build(fee: int) -> tuple[list[CoinSpend], safety.Expected]:
            moved = builder.build_transfer(
                option,
                address,
                builder.Funding(address, xch=self._xch(fee)),
                self._custody(loaded.owner_puzzle_hash),
                fee=fee,
                valid_until_seconds=valid_until,
            )
            return moved.coin_spends, safety.Expected(
                wallet={None: -fee},
                other_max={None: builder.OPTION_COIN_AMOUNT},
                max_fee=fee,
            )

        try:
            (coin_spends, expected), fee = self._priced(build, now, report)
            self._check(coin_spends, expected, bytes(loaded.underlying.puzzle_hash()))
            self._broadcast(
                contract.launcher_id,
                store.WITHDRAW,
                coin_spends,
                now,
                valid_until=valid_until,
                fee=fee,
            )
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            report.problem(f"could not withdraw offers for {contract.slot} ({name}): {exc}")
            return False
        self._withdrawn_at[contract.launcher_id] = now
        report.did(f"withdrawing offers for {contract.slot} ({name}){_paying(fee)}")
        return True

    # -- public -------------------------------------------------------------

    def step(self) -> StepReport:
        now = int(self._clock())
        report = StepReport(at=now, dry_run=self._config.dry_run)
        asked = self._chain.requests
        try:
            with self._store.acting():
                self._step(now, report)
        except store.BusyError as exc:
            report.problem(f"step skipped: {exc}")
        except Exception as exc:
            # The caller decides what a failure means; the journal keeps that
            # the book was running and failing, not idle.
            self.journal(STEP_FAILED, f"{type(exc).__name__}: {exc}", now=now)
            raise
        report.chain_requests = self._chain.requests - asked
        return report

    def wallet_not_ready(self) -> str | None:
        """Why the wallet cannot be relied on right now, or None. Changes nothing."""
        return self._wallet.not_ready(select_key=False)

    def _wallet_ready(self, report: StepReport) -> bool:
        """Select the configured key and see that the wallet is synced.

        A wallet on another key would hand over that key's address and coins,
        and one that is behind lists coins already spent: either way a
        transaction built from what it says is wasted.
        """
        why = self._wallet.not_ready()
        if why is not None:
            report.problem(why)
        self._not_ready = why
        return why is None

    def journal(
        self, verdict: str, detail: str = "", market: Market | None = None, *, now: int
    ) -> None:
        """Keep what a step saw and made of it in the step journal, if it is news.

        Never fails a step: the journal is a record, not something to act on.
        """
        try:
            self._store.log_step(
                verdict,
                detail=detail,
                spot=None if market is None else market.spot,
                sigma=None if market is None else market.sigma,
                bid=None if market is None else market.bid,
                ask=None if market is None else market.ask,
                spot_at=None if market is None else market.as_of,
                interval=self.step_interval,
                now=now,
            )
        except Exception as exc:  # noqa: BLE001 - a full disk must not stop the book
            log.warning("[options] the step journal could not be written: %s", exc)

    def _step(self, now: int, report: StepReport) -> None:
        act = not self._config.dry_run
        self._splash_down = False
        if not self._wallet_ready(report):
            report.problem("nothing done this step")
            self.journal(WALLET_NOT_READY, self._not_ready or "", now=now)
            return
        self._look(now, report)
        self._settle_broadcasts(now, report, act=act)
        self._reconcile(now, report, act=act)
        self._top_up(now, report, act=act)

        market: Market | None = None
        try:
            market = self._market()
            market.require_usable(
                now=now,
                max_age_seconds=self._config.market.max_age_seconds,
                max_spread_pct=self._config.market.max_spread_pct,
                require_book=self._config.market.require_book,
            )
        except MarketError as exc:
            # No price, no quotes. Offers already out expire on their own.
            report.market_error = str(exc)
            report.problem(f"not quoting: {exc}")
            self.journal(f"not quoting: {exc.reason}", str(exc), market, now=now)
            return
        report.market = market
        self._spot = market.spot
        book = (
            "no book"
            if market.bid is None or market.ask is None
            else (f"{market.bid:.4f}/{market.ask:.4f}")
        )
        log.info(
            "[options] market: spot %.4f sigma %.2f dexie %s",
            market.spot,
            market.sigma,
            book,
            extra=REPORTED,
        )
        self.journal(store.QUOTING, market=market, now=now)

        plan = quoting.plan(self._config, market, self._store.contracts(), now)
        report.blocked = plan.blocked
        report.net_delta_xch = plan.net_delta_xch
        if plan.blocked:
            report.problem(plan.blocked)

        if not act:
            self._mint(plan, now, report)
            return

        busy = {row.launcher_id for row in self._store.unresolved()}
        for launcher_id in plan.retire[:1]:
            contract = self._store.contract(launcher_id)
            if contract is not None and launcher_id not in busy:
                self._cancel(contract, now, report)
                busy.add(launcher_id)
        self._mint(plan, now, report)
        self._withdrawn_at = self._store.withdrawals()
        for launcher_id, ask in plan.asks.items():
            contract = self._store.contract(launcher_id)
            if contract is None or contract.status != store.OPEN or launcher_id in busy:
                continue
            self._offer(contract, ask, now, report, plan.quotes.get(launcher_id))

    # -- [OPTIONS-FENCE] the sub-account's target ------------------------------------

    #: A top-up's fee with ``fee_mojos: auto``: a plain wallet send, priced high
    #: enough to confirm promptly, never above ``limits.max_fee_mojos``.
    _TOP_UP_FEE = 10_000_000
    #: A top-up the wallet cannot account for after this long is forgotten, so
    #: a send deleted as stuck does not stop top-ups for good.
    _TOP_UP_FORGET_SECONDS = 3600

    def _top_up_fee(self) -> int:
        if not self._config.fee_auto:
            return int(self._config.fee_mojos)
        cap = self._config.limits.max_fee_mojos
        return min(self._TOP_UP_FEE, cap) if cap > 0 else self._TOP_UP_FEE

    def _held(self, asset_id: bytes | None) -> int:
        """The sub-account in one asset: its loose coins plus the collateral its contracts lock."""
        if asset_id is None:
            loose = sum(int(coin.amount) for coin in self._listed("XCH"))
        else:
            loose = sum(int(cat.coin.amount) for cat in self._listed("quote asset", asset_id))
        locked = sum(
            int(item.underlying_amount)
            for item in self._store.contracts((store.OPEN, store.SOLD))
            if (item.underlying_asset is None) == (asset_id is None)
            and (asset_id is None or bytes(item.underlying_asset) == bytes(asset_id))
        )
        return loose + locked

    def _top_up(self, now: int, report: StepReport, *, act: bool) -> None:
        """Keep the options sub-account at ``options.target`` with ordinary wallet sends.

        Only with ``options.wallet.engine_fence``: the fence is what makes the
        address a sub-account. One top-up per asset at a time; the next waits
        until the wallet says the last one confirmed. Collateral locked in open
        contracts counts as held, so a mint is not a shortfall.
        """
        target = self._config.target
        if target is None or not self._config.wallet.engine_fence:
            return
        if self._store.unresolved() or self._store.contracts((store.MINTING, store.CLOSING)):
            # [review] A mint or cancel in flight still has its inputs in the
            # listing and its collateral in the record: counted now it would be
            # counted twice. Wait for it to settle.
            return
        for asset_id, want, what in (
            (None, target.xch_mojos, "XCH"),
            (self._config.quote_asset_id, target.quote_mojos, "quote asset"),
        ):
            if want <= 0:
                continue
            key = "topup_pending:" + ("xch" if asset_id is None else bytes(asset_id).hex())
            pending, _, sent_at = (self._store.recall(key) or "").partition("|")
            if pending == _SEND_UNCERTAIN:
                # [review] A send whose outcome is unknown (timed out, or the
                # app stopped mid-send) is never sent again on a guess: wait
                # until the address shows it, or an hour has passed.
                if now - int(sent_at or now) <= self._TOP_UP_FORGET_SECONDS:
                    try:
                        if self._held(asset_id) >= want * (1.0 - target.tolerance):
                            self._store.remember(key, "")
                    except builder.OptionBuildError as exc:
                        # The address cannot be read this step: keep waiting.
                        log.debug("[options] %s top-up still uncertain: %s", what, exc)
                    continue
                self._store.remember(key, "")
                report.problem(f"an uncertain {what} top-up from an hour ago is forgotten")
                pending = ""
            if pending:
                try:
                    confirmed = self._coins.transaction_confirmed(pending)
                except Exception as exc:  # noqa: BLE001 - one asset's check must not stop the step
                    # A send the wallet no longer knows (deleted as stuck) must
                    # not hold the sub-account forever: after an hour, forget it.
                    if now - int(sent_at or now) > self._TOP_UP_FORGET_SECONDS:
                        self._store.remember(key, "")
                        report.problem(
                            f"the {what} top-up {pending[:12]} was never seen to confirm; "
                            f"forgotten ({exc})"
                        )
                    else:
                        report.problem(f"could not check the {what} top-up {pending[:12]}: {exc}")
                    continue
                if not confirmed:
                    if now - int(sent_at or now) > self._TOP_UP_FORGET_SECONDS:
                        report.problem(
                            f"the {what} top-up {pending[:12]} has not confirmed in an hour"
                        )
                    continue
                self._store.remember(key, "")
                self._store.add_event(
                    "topup_confirmed", {"asset": what, "transaction": pending}, now=now
                )
                report.did(f"top-up of {what} confirmed")
            try:
                held = self._held(asset_id)
            except builder.OptionBuildError as exc:
                report.problem(f"top-up of {what} skipped: {exc}")
                continue
            report.sub_account[what] = (held, want)
            if held >= want * (1.0 - target.tolerance):
                continue
            short = want - held
            if not act:
                report.did(f"dry run: would top up the options address by {short} {what} mojos")
                continue
            fee = self._top_up_fee()
            # [review] The intent is written before the send: a timeout or a
            # crash after the wallet accepted it must not lead to a second one.
            self._store.remember(key, f"{_SEND_UNCERTAIN}|{now}")
            try:
                transaction = self._coins.send_to_address(asset_id=asset_id, amount=short, fee=fee)
            except Exception as exc:  # noqa: BLE001 - reported; the rest of the step goes on
                report.problem(
                    f"top-up of {what} may not have been sent ({exc}); no new one until the "
                    "address shows it or an hour has passed"
                )
                continue
            self._store.remember(key, f"{transaction or 'unknown'}|{now}")
            self._store.add_event(
                "topup_sent",
                {"asset": what, "amount": short, "fee": fee, "transaction": transaction},
                now=now,
            )
            report.did(
                f"topping up the options address by {short} {what} mojos ({transaction[:12]})"
            )

    def refresh(self) -> StepReport:
        """Bring the book up to date with the chain. Signs nothing and sends nothing."""
        now = int(self._clock())
        report = StepReport(at=now, dry_run=self._config.dry_run)
        asked = self._chain.requests
        try:
            with self._store.acting():
                if self._wallet_ready(report):
                    self._look(now, report, always=True)
                    self._settle_broadcasts(now, report, act=False)
                    self._reconcile(now, report, act=False)
        except store.BusyError as exc:
            report.problem(f"not refreshed: {exc}")
        report.chain_requests = self._chain.requests - asked
        return report

    def withdraw_all(self) -> StepReport:
        """Kill switch: take every open offer off the market by spending its coin."""
        now = int(self._clock())
        report = StepReport(at=now, dry_run=self._config.dry_run)
        asked = self._chain.requests
        try:
            with self._store.acting():
                self._withdraw_all(now, report)
        except store.BusyError as exc:
            report.problem(f"nothing withdrawn: {exc}")
        report.chain_requests = self._chain.requests - asked
        return report

    def _withdraw_all(self, now: int, report: StepReport) -> None:
        act = not self._config.dry_run
        # A kill switch does not wait for a wallet that is catching up: the
        # option coins it spends are read from the chain, not from the wallet.
        self._wallet_ready(report)
        self._look(now, report, always=True)  # a kill switch acts on what is true now
        self._reconcile(now, report, act=act)
        busy = {row.launcher_id for row in self._store.unresolved()}
        found = False
        for contract in self._store.contracts((store.OPEN,)):
            loaded = self._loaded.get(contract.launcher_id)
            if loaded is None or contract.launcher_id in busy:
                continue
            if not self._store.offers(contract.launcher_id, "live"):
                continue
            found = True
            if act:
                self._withdraw(contract, loaded, now, report)
            else:
                name = contract.launcher_id.hex()[:10]
                report.did(f"dry run: would withdraw offers for {contract.slot} ({name})")
        if not act and not found:
            report.problem("dry run: no live offers on record, so there is nothing to withdraw")
        elif not act:
            report.problem("dry run: nothing was sent; set dry_run: false to withdraw them")
