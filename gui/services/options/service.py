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
   again, which is harmless, and past its deadline it is flagged.
2. **Reconcile contracts** against the chain. An option we no longer own was
   sold (the offer that was taken tells us the premium). Collateral gone from a
   sold contract means it was exercised. An expired contract whose collateral
   is still locked is clawed back.
3. **Read the market** and refuse to quote from one that is stale, crossed, too
   wide, or whose spot lies outside what Dexie would actually fill.
4. **Plan** (:func:`quoting.plan`) and act: retire one stale contract, mint one
   wanted contract, and refresh the offers on everything unsold.

Every bundle passes :func:`safety.require` against what the operation is
declared to do before the wallet is asked to sign it, and is written to the
store before it is pushed.

DRY RUN builds and checks the next mint and reports it. It signs nothing,
pushes nothing, uploads nothing and writes no contract.

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

from gui.services.options import builder, chain, quoting, safety, signing, store
from gui.services.options.config import OptionsConfig
from gui.services.options.desk import DeskError
from gui.services.options.market import Market, MarketError

if TYPE_CHECKING:
    from chia_wallet_sdk import Cat, Coin, CoinSpend, PublicKey

log = logging.getLogger(__name__)

#: A pending bundle is pushed again once it is this old, in case the first push
#: never reached a node.
_REPUSH_AFTER_SECONDS = 120


class OptionsError(RuntimeError):
    """The book could not act. The step stops; the next one tries again."""


class Wallet(Protocol):
    """What the book needs from a signer. ``ReferenceWallet`` provides it."""

    def synthetic_key(self, puzzle_hash: bytes) -> PublicKey: ...

    def sign(self, coin_spends: Any) -> bytes: ...

    def push(self, coin_spends: Any, aggregated_signature: bytes) -> bytes: ...


class CoinSource(Protocol):
    """The options address and the spendable coins sitting at it."""

    def puzzle_hash(self) -> bytes: ...

    def xch_coins(self) -> list[Coin]: ...

    def cats(self, asset_id: bytes) -> list[Cat]: ...


class Desk(Protocol):
    def upload_offer(self, offer: str) -> Any: ...


@dataclasses.dataclass
class StepReport:
    """What one step saw and did, for the log and the GUI."""

    at: int = 0
    dry_run: bool = False
    market: Market | None = None
    market_error: str = ""
    blocked: str = ""
    net_delta_xch: float = 0.0
    actions: list[str] = dataclasses.field(default_factory=list)
    problems: list[str] = dataclasses.field(default_factory=list)

    def did(self, text: str) -> None:
        self.actions.append(text)
        log.info("[options] %s", text)

    def problem(self, text: str) -> None:
        self.problems.append(text)
        log.warning("[options] %s", text)


def _sdk() -> Any:
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise OptionsError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


def _pack(coin_spends: list[CoinSpend], signature: bytes) -> bytes:
    sdk = _sdk()
    return bytes(sdk.SpendBundle(coin_spends, sdk.Signature.from_bytes(signature)).to_bytes())


def _unpack(bundle: bytes) -> tuple[list[CoinSpend], bytes]:
    parsed = _sdk().SpendBundle.from_bytes(bytes(bundle))
    return list(parsed.coin_spends), bytes(parsed.aggregated_signature.to_bytes())


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
        clock: Callable[[], float] = time.time,
    ) -> None:
        self._config = config
        self._store = book_store
        self._wallet = wallet
        self._coins = coins
        self._chain = chain_reader
        self._market = market
        self._desk = desk
        self._clock = clock
        self._keys: dict[bytes, Any] = {}
        self._loaded: dict[bytes, chain.LoadedOption] = {}
        self._spot = 0.0

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
        for puzzle_hash in {self._address(), *(bytes(item) for item in extra)}:
            custody.add_key(self._key(puzzle_hash))
        return custody

    def _ours(self) -> set[bytes]:
        mine = {self._address()}
        mine.update(item.creator_puzzle_hash for item in self._store.contracts(store.ACTIVE))
        return mine

    def _reserved(self) -> set[bytes]:
        """Coins a pending broadcast already spends; they must not fund another."""
        taken: set[bytes] = set()
        for row in self._store.broadcasts("pending"):
            for spend in _unpack(row.bundle)[0]:
                taken.add(bytes(spend.coin.coin_id()))
        return taken

    def _xch(self, needed: int) -> list[Coin]:
        """The fewest spendable XCH coins that cover ``needed`` mojos."""
        reserved = self._reserved()
        coins = [coin for coin in self._coins.xch_coins() if bytes(coin.coin_id()) not in reserved]
        if needed <= 0 or not coins:
            return []
        chosen: list[Coin] = []
        total = 0
        for coin in sorted(coins, key=lambda item: int(item.amount), reverse=True):
            chosen.append(coin)
            total += int(coin.amount)
            if total >= needed:
                return chosen
        raise builder.OptionBuildError(
            f"not enough XCH at the options address: need {needed} mojos, have {total}"
        )

    def _cats(self, asset_id: bytes, needed: int) -> list[Cat]:
        reserved = self._reserved()
        cats = [
            cat for cat in self._coins.cats(asset_id) if bytes(cat.coin.coin_id()) not in reserved
        ]
        chosen: list[Cat] = []
        total = 0
        for cat in sorted(cats, key=lambda item: int(item.coin.amount), reverse=True):
            chosen.append(cat)
            total += int(cat.coin.amount)
            if total >= needed:
                return chosen
        raise builder.OptionBuildError(
            f"not enough of the quote asset at the options address: need {needed}, have {total}"
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
    ) -> None:
        """Sign, record, then push. The record comes first."""
        signature = self._wallet.sign(coin_spends)
        self._store.add_broadcast(
            launcher_id=launcher_id,
            kind=kind,
            bundle=_pack(coin_spends, signature),
            deadline=now + self._config.confirm_timeout_seconds,
            contract_status=contract_status,
            now=now,
        )
        self._wallet.push(coin_spends, signature)

    # -- step 1: broadcasts -------------------------------------------------

    def _settle_broadcasts(self, now: int, report: StepReport) -> None:
        rows = self._store.broadcasts("pending") + self._store.broadcasts("stalled")
        for row in rows:
            coin_spends, signature = _unpack(row.bundle)
            try:
                status = chain.bundle_status(self._chain, coin_spends)
            except chain.ChainError as exc:
                report.problem(f"could not check a {row.kind} broadcast: {exc}")
                continue
            name = row.launcher_id.hex()[:10]
            if status is chain.BundleStatus.CONFIRMED:
                self._store.set_broadcast(row.id, "confirmed")
                self._confirmed(row, now, report)
            elif status is chain.BundleStatus.CONFLICT:
                self._store.set_broadcast(row.id, "conflict")
                if row.kind == store.MINT:
                    self._store.set_status(
                        row.launcher_id, store.FAILED, event="mint_failed", now=now
                    )
                    report.problem(f"mint of {name} lost its coins to another spend")
                else:
                    # The coin moved some other way; reconciliation reads how.
                    contract = self._store.contract(row.launcher_id)
                    back = store.SOLD if row.kind == store.CLAWBACK else store.OPEN
                    if contract is not None and contract.status == store.CLOSING:
                        self._store.set_status(row.launcher_id, back, now=now)
                    report.problem(f"{row.kind} of {name} was overtaken on chain")
            elif row.status == "pending" and now >= row.deadline:
                self._store.set_broadcast(row.id, "stalled")
                self._store.add_event(
                    "broadcast_stalled", {"kind": row.kind}, launcher_id=row.launcher_id, now=now
                )
                report.problem(f"{row.kind} of {name} has not confirmed; check the wallet's node")
            elif row.status == "pending" and now - row.created_at >= _REPUSH_AFTER_SECONDS:
                try:
                    self._wallet.push(coin_spends, signature)
                except signing.SigningError as exc:
                    report.problem(f"could not re-push the {row.kind} of {name}: {exc}")

    def _confirmed(self, row: store.Broadcast, now: int, report: StepReport) -> None:
        contract = self._store.contract(row.launcher_id)
        if contract is None:
            return
        name = row.launcher_id.hex()[:10]
        asset = None if contract.underlying_asset is None else contract.underlying_asset.hex()
        detail = {"asset": asset, "amount": contract.underlying_amount}
        if row.kind == store.MINT:
            self._store.set_status(
                row.launcher_id, store.OPEN, event="collateral_locked", detail=detail, now=now
            )
            report.did(f"mint of {contract.slot} ({name}) confirmed")
        elif row.kind in (store.CANCEL, store.CLAWBACK):
            self._store.kill_offers(row.launcher_id)
            self._store.set_status(
                row.launcher_id,
                store.CLOSED,
                event="collateral_returned",
                detail={**detail, "by": row.kind},
                now=now,
            )
            report.did(f"{row.kind} of {contract.slot} ({name}) confirmed")
        elif row.kind == store.WITHDRAW:
            self._store.kill_offers(row.launcher_id)
            report.did(f"offers for {contract.slot} ({name}) withdrawn")

    # -- step 2: contracts --------------------------------------------------

    def _reconcile(self, now: int, report: StepReport) -> None:
        self._loaded = {}
        busy = {row.launcher_id for row in self._store.broadcasts("pending")}
        busy |= {row.launcher_id for row in self._store.broadcasts("stalled")}
        for contract in self._store.contracts((store.OPEN, store.SOLD)):
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
            mine = loaded.owner_puzzle_hash in self._ours()

            if contract.status == store.OPEN and loaded.option is not None and not mine:
                self._sold(contract, now, report)
                contract = dataclasses.replace(contract, status=store.SOLD)

            if loaded.locked is None:
                if contract.launcher_id in busy:
                    continue  # our own cancel or clawback; the broadcast settles it
                self._store.kill_offers(contract.launcher_id)
                strike = None if contract.strike_asset is None else contract.strike_asset.hex()
                self._store.set_status(
                    contract.launcher_id,
                    store.EXERCISED,
                    event="exercised",
                    detail={"strike_asset": strike, "strike_amount": contract.strike_amount},
                    now=now,
                )
                report.did(f"{contract.slot} ({name}) was exercised")
            elif loaded.status is chain.OptionStatus.EXPIRED and contract.launcher_id not in busy:
                self._clawback(contract, loaded, now, report)

        for offer in self._store.offers(status="live"):
            if offer.expires_at is not None and offer.expires_at <= now:
                self._store.set_offer(offer.id, status="dead")

    def _sold(self, contract: store.Contract, now: int, report: StepReport) -> None:
        """Record a sale: the taken offer says what was paid."""
        price: dict[str, Any] = {}
        for offer in self._store.offers(contract.launcher_id):
            if offer.status == "taken":
                continue
            try:
                taken = (
                    chain.bundle_status(self._chain, _unpack(offer.bundle)[0])
                    is chain.BundleStatus.CONFIRMED
                )
            except chain.ChainError:
                taken = False
            if taken:
                self._store.set_offer(offer.id, status="taken")
                asset = None if offer.price_asset is None else offer.price_asset.hex()
                price = {"asset": asset, "amount": offer.price_amount}
        self._store.kill_offers(contract.launcher_id)
        self._store.set_status(
            contract.launcher_id, store.SOLD, event="sold", detail=price, now=now
        )
        report.did(f"{contract.slot} ({contract.launcher_id.hex()[:10]}) was sold")

    def _clawback(
        self, contract: store.Contract, loaded: chain.LoadedOption, now: int, report: StepReport
    ) -> None:
        if loaded.locked is None:
            return
        name = contract.launcher_id.hex()[:10]
        fee = self._config.fee_mojos
        amount = contract.underlying_amount
        try:
            # An XCH underlying pays its own fee; a CAT needs an XCH coin for one.
            funding_xch = self._xch(fee) if contract.underlying_asset is not None else []
            coin_spends = builder.build_clawback(
                loaded.underlying,
                loaded.locked,
                self._key(contract.creator_puzzle_hash),
                builder.Funding(change_puzzle_hash=self._address(), xch=funding_xch),
                self._custody(contract.creator_puzzle_hash),
                fee=fee,
            )
            if contract.underlying_asset is None:
                wallet = {None: amount - fee}
            else:
                wallet = {contract.underlying_asset: amount, None: -fee}
            self._check(
                coin_spends,
                safety.Expected(
                    wallet=wallet, locked={contract.underlying_asset: -amount}, max_fee=fee
                ),
                bytes(loaded.underlying.puzzle_hash()),
            )
            self._broadcast(
                contract.launcher_id,
                store.CLAWBACK,
                coin_spends,
                now,
                contract_status=store.CLOSING,
            )
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            report.problem(f"clawback of {name} not sent: {exc}")
            return
        report.did(f"clawback of {contract.slot} ({name}) sent")

    # -- step 4: acting on the plan -----------------------------------------

    def _cancel(self, contract: store.Contract, now: int, report: StepReport) -> None:
        loaded = self._loaded.get(contract.launcher_id)
        if loaded is None or loaded.option is None or loaded.locked is None:
            return
        name = contract.launcher_id.hex()[:10]
        fee = self._config.fee_mojos
        amount = contract.underlying_amount
        try:
            # The strike asset needs one seed coin of any size; XCH also pays the fee.
            if contract.strike_asset is None:
                xch, cats = self._xch(max(fee, 1)), []
            else:
                xch, cats = self._xch(fee), self._cats(contract.strike_asset, 1)[:1]
            coin_spends = builder.build_cancel(
                loaded.option,
                loaded.underlying,
                loaded.locked,
                builder.Funding(change_puzzle_hash=self._address(), xch=xch, cats=cats),
                self._custody(contract.creator_puzzle_hash),
                fee=fee,
            )
            melted = builder.OPTION_COIN_AMOUNT
            if contract.underlying_asset is None:
                wallet = {None: amount + melted - fee}
            else:
                wallet = {contract.underlying_asset: amount, None: melted - fee}
            self._check(
                coin_spends,
                safety.Expected(
                    wallet=wallet, locked={contract.underlying_asset: -amount}, max_fee=fee
                ),
                bytes(loaded.underlying.puzzle_hash()),
            )
            self._broadcast(
                contract.launcher_id, store.CANCEL, coin_spends, now, contract_status=store.CLOSING
            )
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            # Left alone, it simply stops being offered and is clawed back at expiry.
            report.problem(f"could not retire {contract.slot} ({name}): {exc}")
            return
        report.did(f"retiring {contract.slot} ({name})")

    def _build_mint(self, wanted: quoting.Wanted) -> tuple[builder.MintResult, safety.Expected]:
        fee = self._config.fee_mojos
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
        minted = builder.build_mint(wanted.terms(address), funding, self._custody(), fee=fee)
        expected = safety.Expected(
            wallet=wallet,
            locked={wanted.underlying.asset_id: amount},
            other_max={None: singleton},
            max_fee=fee,
        )
        return minted, expected

    def _mint(self, plan: quoting.Plan, now: int, report: StepReport) -> None:
        for wanted in plan.mint:
            try:
                minted, expected = self._build_mint(wanted)
                self._check(minted.coin_spends, expected, bytes(minted.underlying.puzzle_hash()))
            except builder.OptionBuildError as exc:
                report.problem(f"cannot mint {wanted.slot}: {exc}")
                continue
            except safety.SafetyError as exc:
                report.problem(f"mint of {wanted.slot} refused by the safety check: {exc}")
                return
            label = f"{wanted.slot} strike {wanted.strike:g} ({minted.launcher_id.hex()[:10]})"
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
                    bundle=_pack(minted.coin_spends, signature),
                    deadline=now + self._config.confirm_timeout_seconds,
                    now=now,
                )
                self._wallet.push(minted.coin_spends, signature)
            except signing.SigningError as exc:
                report.problem(f"mint of {wanted.slot} not sent: {exc}")
                return
            report.did(f"minting {label}")
            return  # one mint a step: the next waits for this one's change

    def _offer(self, contract: store.Contract, ask: int, now: int, report: StepReport) -> None:
        loaded = self._loaded.get(contract.launcher_id)
        if loaded is None or loaded.option is None:
            return
        live = [
            offer
            for offer in self._store.offers(contract.launcher_id, "live")
            if offer.expires_at is None or offer.expires_at > now
        ]
        name = contract.launcher_id.hex()[:10]
        if live:
            cheapest = min(offer.price_amount for offer in live)
            newest = live[-1]
            notional = (
                quoting.contract_size_xch(contract) * self._spot * self._config.quote_unit_mojos
            )
            if notional > 0 and (ask - cheapest) / notional * 100.0 >= self._config.withdraw_pct:
                # A new offer would not withdraw the cheaper one. Spend the coin.
                self._withdraw(contract, loaded, now, report)
                return
            fresh = (
                newest.expires_at is None
                or newest.expires_at - now > self._config.refresh_before_seconds
            )
            moved = abs(ask - newest.price_amount) / newest.price_amount * 100.0
            if fresh and moved < self._config.reprice_pct:
                return
        address = self._address()
        try:
            draft = builder.build_offer(
                loaded.option,
                builder.Asset(self._config.quote_asset_id, ask),
                address,
                self._custody(),
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
            bundle=_pack(draft.coin_spends, signature),
            offer_text=text,
            now=now,
        )
        price = ask / self._config.quote_unit_mojos
        report.did(f"offering {contract.slot} ({name}) at {price:g}")
        if self._desk is not None and self._config.upload_offers:
            try:
                upload = self._desk.upload_offer(text)
                self._store.set_offer(offer_id, desk_offer_id=str(upload.offer_id))
            except DeskError as exc:
                # The offer is valid without the site; it just is not advertised.
                report.problem(f"offer for {contract.slot} ({name}) not listed: {exc}")

    def _withdraw(
        self,
        contract: store.Contract,
        loaded: chain.LoadedOption,
        now: int,
        report: StepReport,
    ) -> bool:
        """Spend the option coin back to ourselves, which kills every open offer."""
        if loaded.option is None:
            return False
        name = contract.launcher_id.hex()[:10]
        fee = self._config.fee_mojos
        address = self._address()
        try:
            moved = builder.build_transfer(
                loaded.option,
                address,
                builder.Funding(address, xch=self._xch(fee)),
                self._custody(),
                fee=fee,
            )
            self._check(
                moved.coin_spends,
                safety.Expected(
                    wallet={None: -fee},
                    other_max={None: builder.OPTION_COIN_AMOUNT},
                    max_fee=fee,
                ),
                bytes(loaded.underlying.puzzle_hash()),
            )
            self._broadcast(contract.launcher_id, store.WITHDRAW, moved.coin_spends, now)
        except (builder.OptionBuildError, safety.SafetyError, signing.SigningError) as exc:
            report.problem(f"could not withdraw offers for {contract.slot} ({name}): {exc}")
            return False
        report.did(f"withdrawing offers for {contract.slot} ({name})")
        return True

    # -- public -------------------------------------------------------------

    def step(self) -> StepReport:
        now = int(self._clock())
        report = StepReport(at=now, dry_run=self._config.dry_run)
        self._settle_broadcasts(now, report)
        self._reconcile(now, report)

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
            return report
        report.market = market
        self._spot = market.spot

        plan = quoting.plan(self._config, market, self._store.contracts(), now)
        report.blocked = plan.blocked
        report.net_delta_xch = plan.net_delta_xch
        if plan.blocked:
            report.problem(plan.blocked)

        if self._config.dry_run:
            self._mint(plan, now, report)
            return report

        busy = {row.launcher_id for row in self._store.broadcasts("pending")}
        for launcher_id in plan.retire[:1]:
            contract = self._store.contract(launcher_id)
            if contract is not None and launcher_id not in busy:
                self._cancel(contract, now, report)
                busy.add(launcher_id)
        self._mint(plan, now, report)
        for launcher_id, ask in plan.asks.items():
            contract = self._store.contract(launcher_id)
            if contract is None or contract.status != store.OPEN or launcher_id in busy:
                continue
            self._offer(contract, ask, now, report)
        return report

    def withdraw_all(self) -> StepReport:
        """Kill switch: take every open offer off the market by spending its coin."""
        now = int(self._clock())
        report = StepReport(at=now, dry_run=self._config.dry_run)
        if self._config.dry_run:
            report.problem("dry run: nothing is on offer, so there is nothing to withdraw")
            return report
        self._reconcile(now, report)
        busy = {row.launcher_id for row in self._store.broadcasts("pending")}
        for contract in self._store.contracts((store.OPEN,)):
            loaded = self._loaded.get(contract.launcher_id)
            if loaded is None or contract.launcher_id in busy:
                continue
            if not self._store.offers(contract.launcher_id, "live"):
                continue
            self._withdraw(contract, loaded, now, report)
        return report
