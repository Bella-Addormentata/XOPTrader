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
show exactly what the bundle does. See ``docs/options-live-test.md``.
"""

from __future__ import annotations

import argparse
import logging
import sys
import time
from typing import Any

from gui.services.options import builder, chain, config, safety
from gui.services.options.reference_wallet import ReferenceWallet
from gui.services.options.runtime import _ADDITIONAL_DATA, build, chain_reader, wallet_rpc


def _load(path: str) -> config.OptionsConfig:
    import yaml

    with open(path, encoding="utf-8") as handle:
        root = yaml.safe_load(handle)
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


def _status(cfg: config.OptionsConfig) -> int:
    book, book_store, coins = build(cfg)
    del book
    print(f"network {cfg.network}   dry_run {cfg.dry_run}")
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
    pending = book_store.broadcasts("pending") + book_store.broadcasts("stalled")
    for row in pending:
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
            cfg.quote_asset_id, hedge_size_xch=cfg.market.hedge_size_xch
        ).read,
        book_store=store.OptionStore(cfg.database_path),
        quote_asset_id=cfg.quote_asset_id,
        quote_unit_mojos=cfg.quote_unit_mojos,
        rate=cfg.pricing.rate,
    )

    def number(value: float | None) -> str:
        return "     -" if value is None else f"{value:6.2f}"

    while True:
        report = watcher.scan()
        print(
            f"{time.strftime('%H:%M:%S')} listed {report.listed}, new {len(report.new)}, "
            f"closed {len(report.closed)}"
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


def _wait(reader: Any, coin_spends: list[Any], what: str, timeout: float) -> None:
    deadline = time.time() + timeout
    print(f"  waiting for the {what} to confirm", end="", flush=True)
    while time.time() < deadline:
        status = chain.bundle_status(reader, coin_spends)
        if status is chain.BundleStatus.CONFIRMED:
            print(" confirmed")
            return
        if status is chain.BundleStatus.CONFLICT:
            raise SystemExit(f"\nthe {what} was overtaken by another spend of the same coins")
        print(".", end="", flush=True)
        time.sleep(15)
    raise SystemExit(
        f"\nthe {what} did not confirm in {timeout:g} s. The wallet reports no verdict on a "
        "broadcast; check its full-node connection and the mempool, then run live-test again."
    )


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
    )
    _, _, coins = build(cfg, rpc=rpc, reader=reader)
    address = coins.puzzle_hash()
    amount = int(round(amount_xch * config.XCH_MOJOS))
    fee = cfg.fee_mojos
    print(f"network {cfg.network}; options address {coins.address()}")
    xch = coins.xch_coins()
    have = sum(int(coin.amount) for coin in xch)
    need = amount + builder.OPTION_COIN_AMOUNT + 3 * fee
    print(f"spendable at that address: {have} mojos; the test needs {need}")
    if have < need:
        raise SystemExit("send a little XCH to the options address and wait for it to confirm")

    print("1. asking the wallet for the address's public key")
    key = wallet.synthetic_key(address)
    custody = builder.Custody()
    custody.add_key(key)

    def send(coin_spends: list[Any], expected: safety.Expected, lock: bytes, what: str) -> None:
        effect = safety.analyse(coin_spends, ours=[address], locks=[lock])
        print(f"  the {what} does this: {effect.describe()}")
        safety.require(effect, expected)
        _confirm(f"  sign and send the {what}?", assume_yes)
        wallet.push(coin_spends, wallet.sign(coin_spends))
        _wait(reader, coin_spends, what, timeout)

    print(f"2. minting a contract over {amount_xch:g} XCH, expiring in 24 hours")
    terms = builder.OptionTerms(
        builder.Asset(None, amount),
        builder.Asset(None, amount),
        int(time.time()) + 24 * 3600,
        address,
    )
    funding = builder.Funding(address, xch=xch)
    minted = builder.build_mint(terms, funding, custody, fee=fee)
    lock = bytes(minted.underlying.puzzle_hash())
    print(f"  launcher id {minted.launcher_id.hex()}")
    send(
        minted.coin_spends,
        safety.Expected(
            wallet={None: -(amount + builder.OPTION_COIN_AMOUNT + fee)},
            locked={None: amount},
            other_max={None: builder.OPTION_COIN_AMOUNT},
            max_fee=fee,
        ),
        lock,
        "mint",
    )
    loaded = chain.load_option(reader, minted.launcher_id, now_seconds=int(time.time()))
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
        expires_at_seconds=int(time.time()) + 900,
    )
    safety.require(
        safety.analyse(draft.coin_spends, ours=[address], locks=[lock]),
        safety.Expected(other_max={None: builder.OPTION_COIN_AMOUNT}),
    )
    offer = draft.encode(wallet.sign(draft.coin_spends))
    print(f"  offer string ({len(offer)} characters):\n  {offer}")
    print("  to test a take, paste it into another wallet now; otherwise carry on")

    print("4. withdrawing the offer by moving the option to our own address")
    moved = builder.build_transfer(
        loaded.option,
        address,
        builder.Funding(address, xch=coins.xch_coins()[:1] if fee else []),
        custody,
        fee=fee,
    )
    try:
        send(
            moved.coin_spends,
            safety.Expected(
                wallet={None: -fee}, other_max={None: builder.OPTION_COIN_AMOUNT}, max_fee=fee
            ),
            lock,
            "withdrawal",
        )
    except SystemExit as stop:
        if "overtaken" not in str(stop):
            raise
        print("  the offer was taken first: the option is sold and the test ends here.")
        print("  its collateral comes back after expiry (run `step` then) unless it is exercised.")
        return 0
    status = chain.bundle_status(reader, draft.coin_spends)
    print(f"  the offer is now: {status.value} (conflict means it can never be taken)")

    print("5. cancelling the contract to get the collateral back")
    loaded = chain.load_option(reader, minted.launcher_id, now_seconds=int(time.time()))
    cancel = builder.build_cancel(
        loaded.option,
        loaded.underlying,
        loaded.locked,
        builder.Funding(address, xch=coins.xch_coins()),
        custody,
        fee=fee,
    )
    send(
        cancel,
        safety.Expected(
            wallet={None: amount + builder.OPTION_COIN_AMOUNT - fee},
            locked={None: -amount},
            max_fee=fee,
        ),
        lock,
        "cancel",
    )
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
        return _live_test(cfg, args.amount_xch, args.yes, args.timeout)
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
