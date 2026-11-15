"""Run the options book from a terminal.

    python -m gui.services.options --config config.yaml status
    python -m gui.services.options --config config.yaml step
    python -m gui.services.options --config config.yaml run --interval 60
    python -m gui.services.options --config config.yaml withdraw-all
    python -m gui.services.options --config config.yaml monitor --interval 300
    python -m gui.services.options --config config.yaml live-test

``status``, ``monitor`` and a dry-run ``step`` sign nothing. ``live-test`` is the
guided first run against a real wallet: it mints one dust-sized contract, offers
it, withdraws the offer and cancels the contract, pausing before each spend to
show exactly what the bundle does. Its contract and every bundle it signs are
written to the book's database before they are sent, so a test that is
interrupted, times out or has its offer taken is picked up by running
``live-test`` again. See ``docs/options-live-test.md``.
"""

from __future__ import annotations

import argparse
import dataclasses
import logging
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from gui.services.options import builder, chain, config, fees, safety, store
from gui.services.options.reference_wallet import ReferenceWallet
from gui.services.options.runtime import (
    _ADDITIONAL_DATA,
    build,
    chain_reader,
    chain_source,
    wallet_rpc,
)
from gui.services.options.service import pack_bundle, unpack_bundle

#: The slot a live-test contract is stored under. It is in no grid, so a live
#: book that finds one still open retires it, and claws it back once expired.
LIVE_TEST_SLOT = "live-test"

_POLL_SECONDS = 15
_REPUSH_EVERY_POLLS = 8  # two minutes: the first push may never have reached a node
# With ``fee_mojos: auto`` the test pays one flat fee per spend, sized for the
# costliest of them (the cancel, about 165 million), so its sums stay simple.
_LIVE_TEST_COST = 170_000_000


def _load(path: str) -> config.OptionsConfig:
    # With secrets.yaml from the same folder laid over it, as the GUI and the
    # engine read it: that is where certificate paths and API keys are kept.
    from gui.services.config_split import load_merged

    root = load_merged(Path(path))
    parsed = config.parse(root)
    if parsed is None:
        raise SystemExit(f"{path}: options are not enabled (options.enabled: true to turn on)")
    return parsed


def _print_report(report: Any) -> None:
    if report.market is not None:
        view = report.market
        book = (
            "no book" if view.bid is None or view.ask is None else f"{view.bid:.4f}/{view.ask:.4f}"
        )
        print(f"market: spot {view.spot:.4f}  sigma {view.sigma:.2f}  dexie {book}")
    if report.dry_run:
        print("DRY RUN: nothing is signed, sent or listed")
    for action in report.actions:
        print(f"  did: {action}")
    for problem in report.problems:
        print(f"  note: {problem}")
    if not report.actions and not report.problems:
        print("  nothing to do")
    if report.market is not None:
        print(f"net delta of sold contracts: {report.net_delta_xch:+.2f} XCH")
    looked = f" ({report.chain_looked})" if report.chain_looked else ""
    print(f"chain requests this step: {report.chain_requests}{looked}")


def _learned_rate(book_store: store.OptionStore) -> float:
    """The fee rate the book has learned from transactions that ran out, as last stored."""
    try:
        return float(book_store.recall("fee_rate") or 0.0)
    except ValueError:
        return 0.0


def _status(cfg: config.OptionsConfig) -> int:
    book, book_store, coins = build(cfg)
    print(f"network {cfg.network}   dry_run {cfg.dry_run}")
    key = (
        f"logged in to {cfg.wallet.fingerprint}"
        if cfg.wallet.fingerprint is not None
        else "no fingerprint set, so whichever key it has open"
    )
    print(f"wallet: {book.wallet_not_ready() or f'synced ({key})'}")
    print(f"chain is read from {chain_source(cfg)}")
    if cfg.fee_auto:
        print(
            f"fees: auto, at most {cfg.limits.max_fee_mojos} mojos a transaction "
            f"(rate learned from failures: {_learned_rate(book_store):g} mojos per cost)"
        )
    else:
        print(f"fees: {cfg.fee_mojos} mojos a transaction")
    if cfg.splash.enabled:
        live = book_store.offers(status="live")
        taken = sum(offer.splash_at is not None for offer in live)
        print(
            f"splash: new offers are handed to the daemon at {cfg.splash.submit_url} "
            f"({taken} of {len(live)} live offers taken by it)"
        )
    else:
        print("splash: off")
    print(f"options address {coins.address()}")
    xch = sum(int(coin.amount) for coin in coins.xch_coins())
    quote = sum(int(cat.coin.amount) for cat in coins.cats(cfg.quote_asset_id))
    print(
        f"  spendable there: {xch / config.XCH_MOJOS:g} XCH, {quote / cfg.quote_unit_mojos:g} quote"
    )
    contracts = book_store.contracts()
    print(f"{len(contracts)} contract(s)")
    for item in contracts:
        live = len(book_store.offers(item.launcher_id, "live"))
        print(f"  {item.slot:<22} {item.status:<10} {item.launcher_id.hex()}  live offers: {live}")
    for row in book_store.unresolved():
        print(f"  waiting: {row.kind} of {row.launcher_id.hex()[:10]} ({row.status})")
    return 0


def _step(cfg: config.OptionsConfig) -> int:
    book, _, _ = build(cfg)
    _print_report(book.step())
    return 0


def _run(cfg: config.OptionsConfig, interval: float) -> int:
    book, _, _ = build(cfg)
    print(f"stepping every {interval:g} s; Ctrl-C to stop (offers already out expire on their own)")
    try:
        while True:
            try:
                _print_report(book.step())
            except Exception as exc:  # noqa: BLE001 - keep the loop alive; the next step retries
                logging.getLogger(__name__).exception("step failed: %s", exc)
            time.sleep(interval)
    except KeyboardInterrupt:
        print("stopped")
        return 0


def _monitor(cfg: config.OptionsConfig, interval: float) -> int:
    """Read-only: record what the desk lists and what becomes of it."""
    from gui.services.options import desk, market, monitor, store

    if cfg.network != config.MAINNET:
        raise SystemExit("the desk lists mainnet options only")
    watcher = monitor.DeskMonitor(
        desk_client=desk.DeskClient(),
        chain_reader=chain_reader(cfg),
        market=market.MarketSource(
            cfg.quote_asset_id,
            hedge_size_xch=cfg.market.hedge_size_xch,
            coingecko_api_key=cfg.coingecko_api_key,
        ).read,
        book_store=store.OptionStore(cfg.database_path),
        quote_asset_id=cfg.quote_asset_id,
        quote_unit_mojos=cfg.quote_unit_mojos,
        rate=cfg.pricing.rate,
        max_spot_age_seconds=cfg.market.max_age_seconds,
    )

    def number(value: float | None) -> str:
        return "     -" if value is None else f"{value:6.2f}"

    while True:
        report = watcher.scan()
        print(
            f"{time.strftime('%H:%M:%S')} listed {report.listed}, new {len(report.new)}, "
            f"closed {len(report.closed)}, chain requests {report.chain_requests}"
        )
        if report.market_error:
            print(f"  no market view: {report.market_error}")
        for item in report.new:
            terms = (
                "other assets"
                if item.side is None
                else (f"{item.side} {item.size_xch:g} XCH @ {item.strike:.3f}")
            )
            print(
                f"  new {item.offer_id[:8]} {terms:<26} ask {number(item.ask)}  "
                f"site {number(item.site_premium)}  ours {number(item.our_value)}  "
                f"edge after hedge {number(item.edge)}"
            )
        for offer_id, outcome in report.closed.items():
            print(f"  {offer_id[:8]} {outcome}")
        for problem in report.problems:
            print(f"  note: {problem}")
        if interval <= 0:
            return 0
        try:
            time.sleep(interval)
        except KeyboardInterrupt:
            print("stopped")
            return 0


def _withdraw_all(cfg: config.OptionsConfig) -> int:
    book, _, _ = build(cfg)
    _print_report(book.withdraw_all())
    return 0


# ---------------------------------------------------------------------------
# live-test
# ---------------------------------------------------------------------------


def _confirm(prompt: str, assume_yes: bool) -> None:
    if assume_yes:
        print(f"{prompt} yes (--yes)")
        return
    if input(f"{prompt} [type yes] ").strip().lower() != "yes":
        raise SystemExit("stopped; nothing further was sent")


@dataclasses.dataclass
class _LiveTest:
    """What every stage of the guided test needs."""

    cfg: config.OptionsConfig
    wallet: ReferenceWallet
    reader: Any
    book: Any
    book_store: store.OptionStore
    coins: Any
    address: bytes
    key: Any
    custody: builder.Custody
    assume_yes: bool
    timeout: float
    fee: int = 0  # paid by each of the test's spends

    @staticmethod
    def now() -> int:
        return int(time.time())

    def deadline(self) -> int:
        return self.now() + self.cfg.confirm_timeout_seconds

    def valid_until(self) -> int | None:
        """The time a spend signed now stops being valid, as the book sets it.

        Unconfirmed by then it never can be, so it is given up (and teaches the
        fee rate) instead of holding its coins for ever. None when the chain's
        time cannot be read, since such a spend could never be seen to lapse.
        """
        return self.deadline() if self.reader.tells_time else None

    def free_xch(self) -> list[Any]:
        """The address's XCH coins that no transaction still waiting already spends.

        The wallet lists a coin as spendable until its spend confirms, and it
        does not know about bundles pushed through it. Funding the test from a
        coin a waiting mint of the book's is counting on would knock that mint
        out.
        """
        reserved = {
            bytes(spend.coin.coin_id())
            for row in self.book_store.unresolved()
            for spend in unpack_bundle(row.bundle)[0]
        }
        return [coin for coin in self.coins.xch_coins() if bytes(coin.coin_id()) not in reserved]

    def settle(self) -> None:
        """Bring the stored book up to date with the chain. Sends nothing."""
        for problem in self.book.refresh().problems:
            print(f"  note: {problem}")

    def wait(self, coin_spends: list[Any], signature: bytes, what: str) -> bool:
        """Wait for a pushed bundle. True once it confirms, False if it was overtaken."""
        deadline = time.time() + self.timeout
        print(f"  waiting for the {what} to confirm", end="", flush=True)
        polls = 0
        while time.time() < deadline:
            self.reader.begin()  # each poll reads afresh: one request for the bundle's coins
            try:
                status = chain.bundle_status(self.reader, coin_spends)
            except chain.ChainError:
                status = chain.BundleStatus.PENDING  # a failed lookup decides nothing
            if status is chain.BundleStatus.CONFIRMED:
                print(" confirmed")
                return True
            if status is chain.BundleStatus.CONFLICT:
                print(" overtaken")
                return False
            print(".", end="", flush=True)
            time.sleep(_POLL_SECONDS)
            polls += 1
            if polls % _REPUSH_EVERY_POLLS == 0:
                self.wallet.push(coin_spends, signature)
        raise SystemExit(
            f"\nthe {what} did not confirm in {self.timeout:g} s. The wallet reports no verdict "
            "on a broadcast; check its full-node connection and the mempool. The bundle is on "
            "record: run live-test again to pick it up."
        )

    def send(
        self,
        coin_spends: list[Any],
        expected: safety.Expected,
        lock: bytes,
        what: str,
        record: Callable[[bytes], Any],
    ) -> bool:
        """Show a bundle, ask, sign, write it down, then push it and wait.

        ``record`` stores the signed bundle; it runs before the push, so
        nothing is ever on its way to the chain that the database does not
        know. True once the bundle confirms, False if it was overtaken.
        """
        effect = safety.analyse(coin_spends, ours=[self.address], locks=[lock])
        print(f"  the {what} does this: {effect.describe()}")
        safety.require(effect, expected)
        _confirm(f"  sign and send the {what}?", self.assume_yes)
        signature = self.wallet.sign(coin_spends)
        record(pack_bundle(coin_spends, signature))
        self.wallet.push(coin_spends, signature)
        confirmed = self.wait(coin_spends, signature, what)
        self.settle()
        return confirmed


def _cancel_stage(test: _LiveTest, contract: store.Contract) -> None:
    loaded = chain.load_option(test.reader, contract.launcher_id, now_seconds=test.now())
    fee = test.fee
    amount = contract.underlying_amount
    valid_until = test.valid_until()
    cancel = builder.build_cancel(
        loaded.option,
        loaded.underlying,
        loaded.locked,
        builder.Funding(test.address, xch=test.free_xch()),
        test.custody,
        fee=fee,
        valid_until_seconds=valid_until,
    )

    def record(bundle: bytes) -> None:
        test.book_store.add_broadcast(
            launcher_id=contract.launcher_id,
            kind=store.CANCEL,
            bundle=bundle,
            deadline=test.deadline(),
            contract_status=store.CLOSING,
            valid_until=valid_until,
            fee=fee,
            now=test.now(),
        )

    confirmed = test.send(
        cancel,
        safety.Expected(
            wallet={None: amount + builder.OPTION_COIN_AMOUNT - fee},
            locked={None: -amount},
            max_fee=fee,
        ),
        bytes(loaded.underlying.puzzle_hash()),
        "cancel",
        record,
    )
    if not confirmed:
        raise SystemExit("the cancel was overtaken by another spend; run live-test again")


def _clawback_stage(test: _LiveTest, contract: store.Contract) -> None:
    loaded = chain.load_option(test.reader, contract.launcher_id, now_seconds=test.now())
    fee = test.fee
    amount = contract.underlying_amount
    valid_until = test.valid_until()
    # XCH collateral pays its own fee when it can. The test's dust collateral
    # is smaller than a fee under pressure, and then other coins make it up.
    extra = test.free_xch() if fee >= amount else []
    clawback = builder.build_clawback(
        loaded.underlying,
        loaded.locked,
        test.key,
        builder.Funding(change_puzzle_hash=test.address, xch=extra),
        test.custody,
        fee=fee,
        valid_until_seconds=valid_until,
    )

    def record(bundle: bytes) -> None:
        test.book_store.add_broadcast(
            launcher_id=contract.launcher_id,
            kind=store.CLAWBACK,
            bundle=bundle,
            deadline=test.deadline(),
            contract_status=store.CLOSING,
            valid_until=valid_until,
            fee=fee,
            now=test.now(),
        )

    confirmed = test.send(
        clawback,
        safety.Expected(wallet={None: amount - fee}, locked={None: -amount}, max_fee=fee),
        bytes(loaded.underlying.puzzle_hash()),
        "clawback",
        record,
    )
    if not confirmed:
        raise SystemExit("the clawback was overtaken by another spend; run live-test again")


def _finish_live_test(test: _LiveTest, contract: store.Contract) -> None:
    """Pick up a test contract an earlier run left behind, from what the chain shows."""
    launcher_id = contract.launcher_id
    print(f"an earlier live test left contract {launcher_id.hex()} unfinished; picking it up")
    test.settle()
    expired = test.now() >= contract.expiration
    # Whatever of its own is still in flight was approved when it was signed:
    # send it again and wait. A cancel past expiry is the exception. It can
    # never confirm, and the clawback below takes its place.
    for row in test.book_store.unresolved():
        if row.launcher_id != launcher_id or (row.kind == store.CANCEL and expired):
            continue
        if row.valid_until is not None and test.now() >= row.valid_until:
            # Settled above if the chain already shows it out of time; if not,
            # it soon will, and pushing it again cannot help.
            raise SystemExit(
                f"  its {row.kind} ran out of time unconfirmed and can no longer confirm. It is "
                "given up once the chain is 32 blocks past its deadline, about a quarter of an "
                "hour after it: run live-test again then, and it is made afresh at the fee "
                "rate that failure taught."
            )
        coin_spends, signature = unpack_bundle(row.bundle)
        print(f"  its {row.kind} was signed but has not confirmed; sending it again")
        test.wallet.push(coin_spends, signature)
        test.wait(coin_spends, signature, row.kind)
        test.settle()

    current = test.book_store.contract(launcher_id)
    if current is None or current.status not in store.ACTIVE:
        status = "gone" if current is None else current.status
        print(f"  it is {status}; it locks nothing any more")
        return
    loaded = chain.load_option(test.reader, launcher_id, now_seconds=test.now())
    if loaded.status is chain.OptionStatus.EXPIRED:
        print("  it has expired with its collateral still locked; clawing it back")
        _clawback_stage(test, current)
    elif loaded.locked is not None and loaded.owner_puzzle_hash == test.address:
        print("  it is live and still ours; cancelling it to get the collateral back")
        _cancel_stage(test, current)
    elif loaded.locked is not None:
        when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(current.expiration))
        print(f"  it was sold. Its collateral can be clawed back after {when}, unless the")
        print("  buyer exercises it first: run live-test again then.")
        return
    final = test.book_store.contract(launcher_id)
    print(f"  it is now {'gone' if final is None else final.status}")


def _live_test(
    cfg: config.OptionsConfig, amount_xch: float, assume_yes: bool, timeout: float
) -> int:
    """Mint, offer, withdraw and cancel one dust contract against the real wallet.

    The contract locks XCH and is struck in XCH, so the whole test needs nothing
    but a little XCH at the options address and returns all of it but the fees.
    """
    rpc = wallet_rpc(cfg)
    reader = chain_reader(cfg)
    wallet = ReferenceWallet(
        rpc,
        agg_sig_me_additional_data=_ADDITIONAL_DATA[cfg.network],
        address_prefix=cfg.address_prefix,
        fingerprint=cfg.wallet.fingerprint,
    )
    why = wallet.not_ready()
    if why is not None:
        raise SystemExit(f"{why}; run the test again once it is ready")
    book, book_store, coins = build(cfg, rpc=rpc, reader=reader, clock=time.time)
    address = coins.puzzle_hash()
    print(f"network {cfg.network}; options address {coins.address()}")
    print(f"chain is read from {chain_source(cfg)}")
    print("1. asking the wallet for the address's public key")
    key = wallet.synthetic_key(address)
    custody = builder.Custody()
    custody.add_key(key)
    fee = cfg.fee_mojos
    if cfg.fee_auto:
        estimate = wallet.fee_rate()
        rate = max(fees.pressure_rate(estimate), _learned_rate(book_store))
        fee = fees.fee_for(_LIVE_TEST_COST, rate, cfg.limits.max_fee_mojos)
        said = (
            "no estimate from the wallet"
            if estimate is None
            else f"the wallet estimates {estimate}"
        )
        print(
            f"fee per spend: {fee} mojos (auto: {said}, paying {rate:g} mojos per cost, "
            f"capped at {cfg.limits.max_fee_mojos})"
        )
    test = _LiveTest(
        cfg=cfg,
        wallet=wallet,
        reader=book.chain,
        book=book,
        book_store=book_store,
        coins=coins,
        address=address,
        key=key,
        custody=custody,
        assume_yes=assume_yes,
        timeout=timeout,
        fee=fee,
    )
    try:
        # Held throughout: a book stepping over the same database must not act
        # on the test contract, or spend its coins, half way through.
        with book_store.acting():
            for left in book_store.contracts(store.ACTIVE):
                if left.slot == LIVE_TEST_SLOT:
                    _finish_live_test(test, left)
            return _run_live_test(test, amount_xch)
    except store.BusyError as exc:
        raise SystemExit(f"{exc}. Stop the other process, then run live-test again.") from exc


def _run_live_test(test: _LiveTest, amount_xch: float) -> int:
    coins, address, custody = test.coins, test.address, test.custody
    amount = int(round(amount_xch * config.XCH_MOJOS))
    fee = test.fee
    xch = test.free_xch()
    have = sum(int(coin.amount) for coin in xch)
    # Three fees, or one spare mojo when there is no fee: the cancel needs an
    # XCH coin to start from, and a fee-less mint could use the address's last.
    need = amount + builder.OPTION_COIN_AMOUNT + max(3 * fee, 1)
    print(f"free at that address: {have} mojos; the test needs {need}")
    if have < need:
        raise SystemExit(
            "send a little XCH to the options address and wait for it to confirm "
            "(coins a transaction still waiting already spends are not counted)"
        )

    print(f"2. minting a contract over {amount_xch:g} XCH, expiring in 24 hours")
    expiry = test.now() + 24 * 3600
    terms = builder.OptionTerms(
        builder.Asset(None, amount), builder.Asset(None, amount), expiry, address
    )
    mint_until = test.valid_until()
    minted = builder.build_mint(
        terms, builder.Funding(address, xch=xch), custody, fee=fee, valid_until_seconds=mint_until
    )
    launcher_id = bytes(minted.launcher_id)
    lock = bytes(minted.underlying.puzzle_hash())
    print(f"  launcher id {launcher_id.hex()}")

    def record_mint(bundle: bytes) -> None:
        test.book_store.add_contract(
            launcher_id=launcher_id,
            slot=LIVE_TEST_SLOT,
            side="call",
            underlying_asset=None,
            underlying_amount=amount,
            strike_asset=None,
            strike_amount=amount,
            expiration=expiry,
            creator_puzzle_hash=address,
            bundle=bundle,
            deadline=test.deadline(),
            valid_until=mint_until,
            fee=fee,
            now=test.now(),
        )

    confirmed = test.send(
        minted.coin_spends,
        safety.Expected(
            wallet={None: -(amount + builder.OPTION_COIN_AMOUNT + fee)},
            locked={None: amount},
            other_max={None: builder.OPTION_COIN_AMOUNT},
            max_fee=fee,
        ),
        lock,
        "mint",
        record_mint,
    )
    if not confirmed:
        raise SystemExit("the mint was overtaken by another spend of its coins; nothing is locked")
    loaded = chain.load_option(test.reader, launcher_id, now_seconds=test.now())
    print(
        f"  read back from chain: {loaded.status.value}, owned by us: "
        f"{loaded.owner_puzzle_hash == address}"
    )

    print("3. making a 15 minute offer for it (signed, not sent anywhere)")
    draft = builder.build_offer(
        loaded.option,
        builder.Asset(None, amount),
        address,
        custody,
        expires_at_seconds=test.now() + 900,
    )
    safety.require(
        safety.analyse(draft.coin_spends, ours=[address], locks=[lock]),
        safety.Expected(other_max={None: builder.OPTION_COIN_AMOUNT}),
    )
    signature = test.wallet.sign(draft.coin_spends)
    offer = draft.encode(signature)
    # On record like any other offer, so a take is booked with what it paid.
    test.book_store.add_offer(
        launcher_id=launcher_id,
        price_asset=None,
        price_amount=amount,
        expires_at=draft.expires_at_seconds,
        bundle=pack_bundle(draft.coin_spends, signature),
        offer_text=offer,
        now=test.now(),
    )
    print(f"  offer string ({len(offer)} characters):\n  {offer}")
    print("  to test a take, paste it into another wallet now; otherwise carry on")

    print("4. withdrawing the offer by moving the option to our own address")
    withdraw_until = test.valid_until()
    moved = builder.build_transfer(
        loaded.option,
        address,
        builder.Funding(address, xch=test.free_xch() if fee else []),
        custody,
        fee=fee,
        valid_until_seconds=withdraw_until,
    )

    def record_withdrawal(bundle: bytes) -> None:
        test.book_store.add_broadcast(
            launcher_id=launcher_id,
            kind=store.WITHDRAW,
            bundle=bundle,
            deadline=test.deadline(),
            valid_until=withdraw_until,
            fee=fee,
            now=test.now(),
        )

    withdrawn = test.send(
        moved.coin_spends,
        safety.Expected(
            wallet={None: -fee}, other_max={None: builder.OPTION_COIN_AMOUNT}, max_fee=fee
        ),
        lock,
        "withdrawal",
        record_withdrawal,
    )
    contract = test.book_store.contract(launcher_id)
    assert contract is not None
    if not withdrawn:
        if contract.status != store.SOLD:
            raise SystemExit("the withdrawal was overtaken by another spend; run live-test again")
        when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(expiry))
        print("  the offer was taken first: the option is sold and the test ends here.")
        print(f"  its collateral can be clawed back after {when}, unless the buyer exercises")
        print("  it first: run live-test again then.")
        return 0
    status = chain.bundle_status(test.reader, draft.coin_spends)
    print(f"  the offer is now: {status.value} (conflict means it can never be taken)")

    print("5. cancelling the contract to get the collateral back")
    _cancel_stage(test, contract)
    after = sum(int(coin.amount) for coin in coins.xch_coins())
    print(f"done. Spendable now: {after} mojos (started with {have}; fees paid: {have - after})")
    print("Now check the wallet's own balance and transaction list agree with this.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m gui.services.options", description=__doc__)
    parser.add_argument("--config", default="config.yaml")
    parser.add_argument("--verbose", action="store_true")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="address, funds and contracts")
    commands.add_parser("step", help="run one step and print what it did")
    run = commands.add_parser("run", help="step in a loop")
    run.add_argument("--interval", type=float, default=60.0)
    commands.add_parser("withdraw-all", help="take every open offer off the market")
    watch = commands.add_parser("monitor", help="read-only: log what the desk lists")
    watch.add_argument("--interval", type=float, default=0.0, help="seconds; 0 scans once")
    test = commands.add_parser("live-test", help="guided dust test against the real wallet")
    test.add_argument("--amount-xch", type=float, default=0.0001)
    test.add_argument("--yes", action="store_true", help="do not pause before each spend")
    test.add_argument("--timeout", type=float, default=900.0)
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO if args.verbose else logging.WARNING)
    try:
        cfg = _load(args.config)
        if args.command == "status":
            return _status(cfg)
        if args.command == "step":
            return _step(cfg)
        if args.command == "run":
            return _run(cfg, max(5.0, args.interval))
        if args.command == "withdraw-all":
            return _withdraw_all(cfg)
        if args.command == "monitor":
            return _monitor(cfg, args.interval)
        try:
            return _live_test(cfg, args.amount_xch, args.yes, args.timeout)
        except KeyboardInterrupt:
            print("\ninterrupted. What was sent is on record: run live-test again to finish it.")
            return 130
    except config.OptionsConfigError as exc:
        print(f"config error: {exc}", file=sys.stderr)
        return 2
    except (
        builder.OptionBuildError,
        safety.SafetyError,
        chain.ChainError,
        chain.InvalidOptionError,
        RuntimeError,
    ) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
