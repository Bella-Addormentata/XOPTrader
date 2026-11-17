"""The wiring and the command line, end to end over a fake wallet RPC.

``WalletRpc`` extends the fake wallet from ``test_reference_wallet`` with the
two read calls the coin source makes, so everything from config.yaml to a
confirmed spend runs here exactly as it would against a wallet daemon, with the
simulator standing in for the chain.
"""

from __future__ import annotations

import http.server
import json
import math
import shutil
import ssl
import subprocess
import threading
import types
from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")
yaml = pytest.importorskip("yaml")

from gui.services.options import __main__ as cli  # noqa: E402
from gui.services.options import (  # noqa: E402
    builder,
    chain,
    config,
    market,
    runtime,
    splash,
    store,
)
from gui.services.options.service import OptionsError  # noqa: E402
from gui.services.options.tests.sim_world import CAT, XCH, Party, SimChain, World  # noqa: E402
from gui.services.options.tests.test_reference_wallet import FakeWallet, _hex  # noqa: E402


class WalletRpc(FakeWallet):
    """The fake wallet plus ``get_next_address``, coin listing and CAT wallets."""

    def __init__(self, world: World, party: Party, cat_asset: bytes | None = None) -> None:
        super().__init__(world, party)
        self.party = party
        self.cat_asset = cat_asset
        self.stranger_coins: list[Any] = []
        self.asset_prefix = "0x"

    def _get_next_address(self, body: dict) -> dict:
        assert body == {"wallet_id": 1, "new_address": False}
        return {"address": sdk.Address(self.party.puzzle_hash, "txch").encode()}

    @staticmethod
    def _record(coin: Any) -> dict:
        return {
            "coin": {
                "parent_coin_info": _hex(coin.parent_coin_info),
                "puzzle_hash": _hex(coin.puzzle_hash),
                "amount": int(coin.amount),
            }
        }

    def _get_spendable_coins(self, body: dict) -> dict:
        if body["wallet_id"] == 1:
            coins = self.party.xch_coins() + self.stranger_coins
        else:
            wrapped = sdk.cat_puzzle_hash(self.cat_asset, self.party.puzzle_hash)
            coins = list(self.world.sim.unspent_coins(wrapped, False))
        return {"confirmed_records": [self._record(coin) for coin in coins]}

    def _get_wallets(self, body: dict) -> dict:
        assert body == {"type": 6}
        return {"wallets": [] if self.cat_asset is None else [{"id": 2}, {"id": "bad"}]}

    def _cat_get_asset_id(self, body: dict) -> dict:
        assert self.cat_asset is not None
        # As chia-blockchain 2.7.4 answers on mainnet (seen 2026-11-15): 0x-prefixed.
        return {
            "asset_id": self.asset_prefix + self.cat_asset.hex(),
            "wallet_id": body["wallet_id"],
        }


def _config(tmp_path, usd: bytes, chia: dict | None = None, **overrides) -> config.OptionsConfig:
    section = {
        "enabled": True,
        "dry_run": False,
        "network": "testnet11",
        "wallet": {"port": 9356, "cert_path": "c", "key_path": "k"},
        "quote_asset_id": usd.hex(),
        "contract_xch": 20,
        "database_path": str(tmp_path / "options_jobs.db"),
    }
    section.update(overrides)
    parsed = config.parse({"options": section, **({"chia": chia} if chia else {})})
    assert parsed is not None
    return parsed


def test_coin_source_lists_only_coins_at_the_options_address(tmp_path) -> None:
    world = World()
    maker, other = world.party(100), world.party(5)
    usd = world.issue_cat(other, 500)
    world.send_cat(other, maker.puzzle_hash, usd, 40 * CAT)
    rpc = WalletRpc(world, maker, usd)
    rpc.stranger_coins = other.xch_coins()  # elsewhere in the same wallet

    source = runtime.WalletCoinSource(rpc, world.sim, prefix="txch")

    assert source.puzzle_hash() == maker.puzzle_hash
    assert source.address().startswith("txch1")
    assert [bytes(c.coin_id()) for c in source.xch_coins()] == [
        bytes(c.coin_id()) for c in maker.xch_coins()
    ]
    (cat,) = source.cats(usd)
    assert int(cat.coin.amount) == 40 * CAT and cat.lineage_proof is not None
    assert source.cats(b"\x09" * 32) == []  # no wallet for that asset


@pytest.mark.parametrize("prefix", ["0x", "0X", ""])
def test_the_quote_asset_s_wallet_is_found_however_its_id_is_written(prefix) -> None:
    # The wallet answered "0x..." and bytes.fromhex refused it: the book never
    # found its quote asset's wallet, so showed 0 quote and could not seed a
    # cancel of a call struck in it.
    world = World()
    maker, other = world.party(1), world.party(1)
    usd = world.issue_cat(other, 500)
    world.send_cat(other, maker.puzzle_hash, usd, 7 * CAT)
    rpc = WalletRpc(world, maker, usd)
    rpc.asset_prefix = prefix
    source = runtime.WalletCoinSource(rpc, world.sim, prefix="txch")

    (cat,) = source.cats(usd)

    assert int(cat.coin.amount) == 7 * CAT


def test_coin_source_lists_the_whole_wallet_only_when_asked_to(tmp_path) -> None:
    world = World()
    maker, other = world.party(100), world.party(5)
    usd = world.issue_cat(other, 500)
    world.send_cat(other, maker.puzzle_hash, usd, 40 * CAT)
    rpc = WalletRpc(world, maker, usd)
    rpc.stranger_coins = other.xch_coins()  # elsewhere in the same wallet
    source = runtime.WalletCoinSource(rpc, world.sim, prefix="txch")

    mine = {bytes(c.coin_id()) for c in source.xch_coins()}
    everything = {bytes(c.coin_id()) for c in source.wallet_xch_coins()}

    assert mine == {bytes(c.coin_id()) for c in maker.xch_coins()}
    assert everything == mine | {bytes(c.coin_id()) for c in other.xch_coins()}
    (raw,) = source.wallet_cat_coins(usd)  # the CAT wallet's coins, before any chain read
    cat = source.cat_of(raw)
    assert int(cat.coin.amount) == 40 * CAT and cat.lineage_proof is not None
    assert source.wallet_cat_coins(b"\x09" * 32) == []  # no wallet for that asset


def test_coin_source_lists_coins_without_the_chain_and_reads_a_cat_s_parent_once() -> None:
    world = World()
    maker, other = world.party(100), world.party(5)
    usd = world.issue_cat(other, 500)
    world.send_cat(other, maker.puzzle_hash, usd, 40 * CAT)
    rpc = WalletRpc(world, maker, usd)
    reader = chain.CachedChain(world.sim)
    source = runtime.WalletCoinSource(rpc, reader, prefix="txch")

    held = source.held((usd,))

    # Every spendable coin at the address, XCH and CAT, from the wallet alone.
    assert len(held) == len(maker.xch_coins()) + 1 and reader.requests == 0
    assert source.held((b"\x09" * 32,)) == frozenset(
        bytes(coin.coin_id()) for coin in maker.xch_coins()
    )

    (cat,) = source.cats(usd)
    asked = reader.requests
    assert cat.lineage_proof is not None and asked > 0
    reader.begin()
    source.cats(usd)
    # A later look re-checks that the parent's spend still stands; it is not read again.
    assert reader.requests - asked == 1

    # The wallet list is not asked for again on every step for an asset it lacks.
    listed = rpc.endpoints().count("get_wallets")
    for _ in range(5):
        source.held((b"\x09" * 32,))
    assert rpc.endpoints().count("get_wallets") == listed


def test_the_chain_is_read_from_the_operator_s_node_when_one_is_configured(
    tmp_path, monkeypatch
) -> None:
    world = World()
    maker = world.party(1)
    usd = world.issue_cat(world.party(1), 5)
    (coin,) = maker.xch_coins()
    asked: list[tuple[str, dict]] = []

    def node(url: str, body: dict) -> dict:
        # What chia-blockchain's own full node answers on this endpoint.
        asked.append((url, body))
        assert body == {"names": ["0x" + bytes(coin.coin_id()).hex()], "include_spent_coins": True}
        return {
            "success": True,
            "coin_records": [
                {
                    "coin": {
                        "parent_coin_info": "0x" + bytes(coin.parent_coin_info).hex(),
                        "puzzle_hash": "0x" + bytes(coin.puzzle_hash).hex(),
                        "amount": int(coin.amount),
                    },
                    "confirmed_block_index": 7,
                    "spent_block_index": 0,
                    "spent": False,
                    "coinbase": False,
                    "timestamp": 1,
                }
            ],
        }

    certs: list[tuple[str, str, str]] = []

    def poster(cert: str, key: str, timeout: float, ca: str = ""):
        certs.append((cert, key, ca))
        return node

    monkeypatch.setattr(runtime, "_node_poster", poster)
    cfg = _config(tmp_path, usd, node={"port": 8666, "cert_path": "/n/c.crt", "key_path": "/n/k"})

    reader = runtime.chain_reader(cfg)
    state = reader.coin_state(coin.coin_id())

    assert certs == [("/n/c.crt", "/n/k", "")]
    assert [url for url, _ in asked] == ["https://localhost:8666/get_coin_records_by_names"]
    assert state.spent_height is None and state.created_height == 7
    assert "your full node at localhost:8666 (on this machine" in runtime.chain_source(cfg)
    assert "coinset is not used" in runtime.chain_source(cfg)

    # A node on another machine is reached with its certificate authority.
    away = {
        "host": "10.0.0.9",
        "cert_path": "/n/c.crt",
        "key_path": "/n/k",
        "ca_cert_path": "/n/ca",
    }
    far = _config(tmp_path, usd, node=away)
    runtime.chain_reader(far)
    assert certs[-1] == ("/n/c.crt", "/n/k", "/n/ca")
    assert "your full node at 10.0.0.9:8555 (certificate checked" in runtime.chain_source(far)
    assert runtime.chain_source(_config(tmp_path, usd)).startswith("coinset (https://testnet11.")


def test_coin_source_refuses_an_address_for_another_network() -> None:
    world = World()
    maker = world.party(1)
    mainnet = sdk.Address(maker.puzzle_hash, "xch").encode()
    source = runtime.WalletCoinSource(
        WalletRpc(world, maker), world.sim, address=mainnet, prefix="txch"
    )
    with pytest.raises(OptionsError, match="xch address"):
        source.puzzle_hash()
    with pytest.raises(OptionsError, match="not valid"):
        runtime.WalletCoinSource(
            WalletRpc(world, maker), world.sim, address="nonsense", prefix="txch"
        ).puzzle_hash()


def test_build_wires_a_book_that_reaches_the_dry_run(tmp_path) -> None:
    world = World()
    maker = world.party(100)
    usd = world.issue_cat(maker, 500)
    cfg = _config(tmp_path, usd, dry_run=True)

    def no_feed() -> market.Market:
        raise market.MarketError("no feed in this test")

    book, book_store, coins = runtime.build(
        cfg,
        rpc=WalletRpc(world, maker, usd),
        reader=world.sim,
        market_reader=no_feed,
        clock=world.now,
    )

    assert coins.puzzle_hash() == maker.puzzle_hash
    assert book_store.contracts() == []
    report = book.step()
    assert report.dry_run and report.market_error == "no feed in this test"
    assert (tmp_path / "options_jobs.db").exists()


def test_build_gives_the_book_a_splash_daemon_only_when_switched_on(tmp_path) -> None:
    world = World()
    maker = world.party(100)
    usd = world.issue_cat(maker, 500)

    def built(**overrides):
        cfg = _config(tmp_path, usd, **overrides)
        book, _, _ = runtime.build(
            cfg, rpc=WalletRpc(world, maker, usd), reader=world.sim, clock=world.now
        )
        return book._splash  # noqa: SLF001 - the wiring is what is under test

    assert built() is None
    assert built(splash={"submit_url": "http://127.0.0.1:4000"}) is None  # an address is not a yes
    client = built(splash={"enabled": True, "submit_url": "http://127.0.0.1:4321"})
    assert isinstance(client, splash.SplashClient)
    assert client.submit_url == "http://127.0.0.1:4321"


DUST = 10**8  # 0.0001 XCH, the live test's default size


def _live(tmp_path, monkeypatch, maker_mojos: int = XCH):
    """A world, a funded options address and the CLI wired to both."""
    world = World()
    maker = Party(world, maker_mojos)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd)
    rpc = WalletRpc(world, maker)
    monkeypatch.setattr(cli, "wallet_rpc", lambda _cfg: rpc)
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: world.sim)
    # Waiting is what moves the simulated chain's clock.
    monkeypatch.setattr(cli.time, "sleep", lambda seconds: world.advance(int(seconds)))
    monkeypatch.setattr(cli.time, "time", lambda: float(world.now()))
    return world, maker, cfg, rpc


def _test_contracts(cfg: config.OptionsConfig) -> tuple[store.OptionStore, list[store.Contract]]:
    book = store.OptionStore(cfg.database_path)
    return book, [item for item in book.contracts() if item.slot == cli.LIVE_TEST_SLOT]


def test_live_test_runs_mint_offer_withdraw_cancel(tmp_path, monkeypatch, capsys) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    before = maker.balance()

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    out = capsys.readouterr().out
    assert "launcher id" in out and "offer1" in out
    assert "the offer is now: conflict" in out
    assert rpc.endpoints().count("push_tx") == 3
    assert rpc.rejected == []
    # Everything came back: no fee was configured.
    assert maker.balance() == before
    assert "fees paid: 0" in out


def test_live_test_puts_its_contract_and_every_bundle_on_record(tmp_path, monkeypatch) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    book, (contract,) = _test_contracts(cfg)
    assert contract.status == store.CLOSED
    assert (contract.underlying_asset, contract.underlying_amount) == (None, DUST)
    assert [(row.kind, row.status) for row in book.broadcasts(None)] == [
        (store.MINT, "confirmed"),
        (store.WITHDRAW, "confirmed"),
        (store.CANCEL, "confirmed"),
    ]
    assert [event.kind for event in book.events()] == [
        "collateral_locked",
        "offers_withdrawn",
        "collateral_returned",
    ]
    (offer,) = book.offers()
    assert offer.status == "dead" and offer.price_amount == DUST
    assert book.unresolved() == []


def test_live_test_keeps_a_mojo_back_for_the_cancel_when_there_is_no_fee(
    tmp_path, monkeypatch
) -> None:
    # Exactly the amount and the option coin: a fee-less mint would use the
    # last mojo at the address and the cancel would have no coin to start from.
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch, DUST + 1)
    with pytest.raises(SystemExit, match="send a little XCH"):
        cli._live_test(cfg, 0.0001, True, 60.0)
    assert "push_tx" not in rpc.endpoints()
    assert maker.balance() == DUST + 1


def test_live_test_completes_on_the_least_it_asks_for(tmp_path, monkeypatch) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch, DUST + 2)

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    assert maker.balance() == DUST + 2
    _, (contract,) = _test_contracts(cfg)
    assert contract.status == store.CLOSED


def test_an_interrupted_live_test_is_picked_up_by_the_next_run(
    tmp_path, monkeypatch, capsys
) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    before = maker.balance()
    answers = iter(["yes", "no"])  # approve the mint, then walk away
    monkeypatch.setattr("builtins.input", lambda _prompt: next(answers))

    with pytest.raises(SystemExit, match="nothing further was sent"):
        cli._live_test(cfg, 0.0001, False, 60.0)

    # The collateral is locked, and the book knows exactly where.
    book, (contract,) = _test_contracts(cfg)
    assert contract.status == store.OPEN
    assert maker.balance() == before - DUST - 1
    assert len(book.offers(contract.launcher_id, "live")) == 1
    book.close()
    capsys.readouterr()

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    out = capsys.readouterr().out
    assert f"left contract {contract.launcher_id.hex()} unfinished" in out
    assert "still ours; cancelling" in out
    book, contracts = _test_contracts(cfg)
    # The old contract was cancelled, then a whole new test ran.
    assert [item.status for item in contracts] == [store.CLOSED, store.CLOSED]
    assert maker.balance() == before
    assert book.unresolved() == []


# -- options.wallet.share_trading_wallet --------------------------------------

_SHARED_WALLET = {
    "port": 9256,
    "cert_path": "c",
    "key_path": "k",
    "fingerprint": 1234567890,
    "share_trading_wallet": True,
}


def _engine(monkeypatch, *states: cli.engine_guard.EngineState) -> list[str]:
    """Make the engine check answer ``states`` in turn (the last one repeats)."""
    asked: list[str] = []
    answers = list(states)

    def state(url: str) -> cli.engine_guard.EngineState:
        asked.append(url)
        return answers.pop(0) if len(answers) > 1 else answers[0]

    monkeypatch.setattr(cli.engine_guard, "engine_state", state)
    return asked


_UP = cli.engine_guard.EngineState(running=True, certain=True, detail="xop_trader.exe process 1")
_DOWN = cli.engine_guard.EngineState(running=False, certain=True, detail="nothing there")
_UNSURE = cli.engine_guard.EngineState(running=False, certain=False, detail="tasklist failed")


def _shared_wallet(maker: Party) -> dict:
    return {**_SHARED_WALLET, "address": sdk.Address(maker.puzzle_hash, "txch").encode()}


_TRADING = {"wallet_host": "localhost", "wallet_port": 9256, "wallet_fingerprint": 1234567890}


def _shared_live(tmp_path, monkeypatch):
    world, maker, _, rpc = _live(tmp_path, monkeypatch)
    usd = world.issue_cat(world.party(1), 5)
    return world, maker, _config(tmp_path, usd, chia=_TRADING, wallet=_shared_wallet(maker)), rpc


@pytest.mark.parametrize("engine", [_UP, _UNSURE])
def test_a_shared_wallet_live_test_signs_nothing_while_the_engine_may_be_up(
    tmp_path, monkeypatch, capsys, engine
) -> None:
    world, maker, cfg, rpc = _shared_live(tmp_path, monkeypatch)
    before = maker.balance()
    asked = _engine(monkeypatch, engine)

    with pytest.raises(SystemExit, match="refused, trading engine up: live-test"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    assert asked == ["http://localhost:9090/metrics"]
    assert "push_tx" not in rpc.endpoints() and "gather_signing_info" not in rpc.endpoints()
    assert maker.balance() == before
    assert "share_trading_wallet is ON" in capsys.readouterr().out


def test_a_shared_wallet_live_test_runs_with_the_engine_down_and_asks_at_every_spend(
    tmp_path, monkeypatch, capsys
) -> None:
    world, maker, cfg, rpc = _shared_live(tmp_path, monkeypatch)
    before = maker.balance()
    asked = _engine(monkeypatch, _DOWN)

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    assert maker.balance() == before
    # Once at the start; before each of the three spends' prompts; and at every
    # signature (three spends and the offer) and every push (at least three).
    assert len(asked) >= 1 + 3 + 4 + 3
    out = capsys.readouterr().out
    assert out.count("share_trading_wallet is ON") == 4
    assert "trading engine: not running (nothing there)" in out


def test_an_engine_started_part_way_through_a_shared_live_test_stops_the_next_signature(
    tmp_path, monkeypatch
) -> None:
    world, maker, cfg, rpc = _shared_live(tmp_path, monkeypatch)

    def state(url: str) -> cli.engine_guard.EngineState:
        # Up as soon as the mint has gone out: before the offer is signed.
        return _UP if rpc.endpoints().count("push_tx") else _DOWN

    monkeypatch.setattr(cli.engine_guard, "engine_state", state)

    with pytest.raises(cli.engine_guard.EngineRunningError, match="refused, trading engine up"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    assert rpc.endpoints().count("push_tx") == 1  # the mint only
    _, (contract,) = _test_contracts(cfg)
    assert contract.status == store.OPEN  # on record, for the next run to finish
    assert _test_contracts(cfg)[0].offers() == []  # the offer was never signed


def test_build_checks_for_the_engine_at_every_signature_only_when_shared(tmp_path) -> None:
    world = World()
    maker = world.party(1)
    usd = world.issue_cat(maker, 5)
    rpc = WalletRpc(world, maker, usd)

    def wallet_of(cfg: config.OptionsConfig) -> Any:
        book, _, _ = runtime.build(cfg, rpc=rpc, reader=world.sim, clock=world.now)
        return book._wallet  # noqa: SLF001 - the wiring is what is under test

    shared = _config(tmp_path, usd, chia=_TRADING, wallet=_shared_wallet(maker))
    assert isinstance(wallet_of(shared), cli.engine_guard.EngineCheckedWallet)
    assert not isinstance(wallet_of(_config(tmp_path, usd)), cli.engine_guard.EngineCheckedWallet)


def test_a_shared_wallet_step_rehearses_with_the_engine_up_but_does_not_act(
    tmp_path, monkeypatch, capsys
) -> None:
    world = World()
    maker = world.party(100)
    usd = world.issue_cat(maker, 500)
    rpc = WalletRpc(world, maker, usd)
    monkeypatch.setattr(runtime, "wallet_rpc", lambda _cfg: rpc)
    monkeypatch.setattr(runtime, "chain_reader", lambda _cfg: world.sim)
    _engine(monkeypatch, _UP)
    stepped: list[bool] = []
    monkeypatch.setattr(
        cli,
        "_print_report",
        lambda report: stepped.append(report.dry_run),
    )
    monkeypatch.setattr(
        runtime.market,
        "MarketSource",
        lambda *a, **k: type("NoFeed", (), {"read": staticmethod(_no_feed)})(),
    )

    rehearsal = _config(tmp_path, usd, chia=_TRADING, dry_run=True, wallet=_shared_wallet(maker))
    assert cli._step(rehearsal) == 0 and stepped == [True]

    live = _config(tmp_path, usd, chia=_TRADING, dry_run=False, wallet=_shared_wallet(maker))
    assert cli._step(live) == 3 and stepped == [True]  # refused: no step was taken
    assert cli._withdraw_all(live) == 3
    out = capsys.readouterr().out
    assert "refused, trading engine up: a live step" in out
    assert "refused, trading engine up: withdraw-all" in out
    assert "push_tx" not in rpc.endpoints()


def _no_feed() -> market.Market:
    raise market.MarketError("no feed in this test")


def test_run_stamps_each_step_with_the_time_and_stops_cleanly(tmp_path, monkeypatch, capsys):
    usd = b"\x07" * 32
    cfg = _config(tmp_path, usd, dry_run=True)
    book = types.SimpleNamespace()
    monkeypatch.setattr(cli, "build", lambda _cfg: (book, None, None))
    steps: list[int] = []
    monkeypatch.setattr(cli, "_guarded_step", lambda *_: steps.append(1) or True)

    def sleep(seconds: float) -> None:
        if len(steps) >= 2:
            raise KeyboardInterrupt

    monkeypatch.setattr(cli.time, "sleep", sleep)

    assert cli._run(cfg, 60.0) == 0

    out = capsys.readouterr().out
    assert len(steps) == 2 and out.count("--- step at 20") == 2 and out.endswith("stopped\n")
    assert book.step_interval == 60.0  # the journal keeps it, for the report


def test_without_the_opt_in_the_engine_is_never_asked_about(tmp_path, monkeypatch, capsys) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    asked = _engine(monkeypatch, _UP)

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    assert asked == []
    assert "share_trading_wallet" not in capsys.readouterr().out


def test_a_live_test_whose_mint_never_confirms_is_picked_up(tmp_path, monkeypatch, capsys) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    before = maker.balance()
    rpc._push_tx = lambda body: {}  # the wallet has no node to hand it to

    with pytest.raises(SystemExit, match="did not confirm in 60 s"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    book, (contract,) = _test_contracts(cfg)
    assert contract.status == store.MINTING
    assert [row.kind for row in book.unresolved()] == [store.MINT]
    assert maker.balance() == before
    book.close()
    capsys.readouterr()

    del rpc._push_tx  # the node is back
    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    out = capsys.readouterr().out
    assert "its mint was signed but has not confirmed; sending it again" in out
    book, contracts = _test_contracts(cfg)
    assert [item.status for item in contracts] == [store.CLOSED, store.CLOSED]
    assert maker.balance() == before


def test_live_test_pushes_again_while_it_waits(tmp_path, monkeypatch) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    real_push, pushes = rpc._push_tx, []

    def flaky(body: dict) -> dict:
        pushes.append(1)
        return {} if len(pushes) == 1 else real_push(body)  # the first one is lost

    rpc._push_tx = flaky

    assert cli._live_test(cfg, 0.0001, True, 600.0) == 0

    assert len(pushes) == 4  # the mint twice, then the withdrawal and the cancel
    _, (contract,) = _test_contracts(cfg)
    assert contract.status == store.CLOSED


def test_a_live_test_offer_that_is_taken_is_booked_and_clawed_back_later(
    tmp_path, monkeypatch, capsys
) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    taker = world.party(1)
    before = maker.balance()

    def take_it_first(prompt: str, assume_yes: bool) -> None:
        if "withdrawal" in prompt:
            book = store.OptionStore(cfg.database_path)
            world.take(taker, book.offers()[-1].offer_text)
            book.close()

    monkeypatch.setattr(cli, "_confirm", take_it_first)

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    out = capsys.readouterr().out
    assert "the offer was taken first" in out and "run live-test again then" in out
    book, (contract,) = _test_contracts(cfg)
    assert contract.status == store.SOLD
    (sold,) = [event for event in book.events() if event.kind == "sold"]
    (taken,) = [offer for offer in book.offers() if offer.status == "taken"]
    assert sold.detail == {"asset": None, "amount": DUST, "offer_id": taken.id}
    assert maker.balance() == before - 1  # the premium came in for the collateral locked
    book.close()

    # Before expiry there is nothing to do but say so, and a new test can run.
    monkeypatch.setattr(cli, "_confirm", lambda prompt, assume_yes: None)
    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0
    assert "it was sold. Its collateral can be clawed back after" in capsys.readouterr().out
    book, contracts = _test_contracts(cfg)
    assert [item.status for item in contracts] == [store.SOLD, store.CLOSED]
    book.close()

    world.advance(24 * 3600 + 60)
    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    assert "it has expired with its collateral still locked; clawing it back" in (
        capsys.readouterr().out
    )
    book, contracts = _test_contracts(cfg)
    assert [item.status for item in contracts] == [store.CLOSED] * 3
    returned = [
        event.detail["by"] for event in book.events() if event.kind == "collateral_returned"
    ]
    assert returned.count(store.CLAWBACK) == 1
    # Collateral and premium are both home; the buyer keeps the one-mojo option coin.
    assert maker.balance() == before + DUST - 1


def test_live_test_pays_the_estimated_fee_on_each_spend(tmp_path, monkeypatch, capsys) -> None:
    world, maker, _, rpc = _live(tmp_path, monkeypatch)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd, fee_mojos="auto", limits={"max_fee_mojos": 10**9})
    rpc.fee_per_cost = 1
    before = maker.balance()

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    out = capsys.readouterr().out
    # One flat fee per spend, sized for the costliest of the three, at the
    # rate a full mempool admits: an estimate of 1 would not get in.
    flat = math.ceil(170_000_000 * 5.5)
    assert f"fee per spend: {flat} mojos (auto: the wallet estimates 1, paying 5.5" in out
    assert maker.balance() == before - 3 * flat
    assert f"fees paid: {3 * flat}" in out
    assert rpc.rejected == []


def test_live_test_with_no_estimate_pays_no_fee(tmp_path, monkeypatch, capsys) -> None:
    world, maker, _, rpc = _live(tmp_path, monkeypatch)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd, fee_mojos="auto", limits={"max_fee_mojos": 10**9})
    rpc.fee_per_cost = None  # the wallet cannot say
    before = maker.balance()

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    assert "fee per spend: 0 mojos (auto: no estimate from the wallet" in capsys.readouterr().out
    assert maker.balance() == before


def test_live_test_will_not_run_beside_another_process(tmp_path, monkeypatch) -> None:
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    other = store.OptionStore(cfg.database_path)

    with other.acting(), pytest.raises(SystemExit, match="another process is acting"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    assert "push_tx" not in rpc.endpoints()


def test_live_test_stops_without_funds(tmp_path, monkeypatch) -> None:
    world = World()
    maker = world.party(1)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd)
    monkeypatch.setattr(cli, "wallet_rpc", lambda _cfg: WalletRpc(world, maker))
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: world.sim)
    with pytest.raises(SystemExit, match="send a little XCH"):
        cli._live_test(cfg, 5.0, True, 60.0)
    assert maker.balance() == 1 * XCH


def test_live_test_asks_before_each_spend(tmp_path, monkeypatch) -> None:
    world = World()
    maker = world.party(1)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd)
    rpc = WalletRpc(world, maker)
    monkeypatch.setattr(cli, "wallet_rpc", lambda _cfg: rpc)
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: world.sim)
    monkeypatch.setattr("builtins.input", lambda _prompt: "no")

    with pytest.raises(SystemExit, match="nothing further was sent"):
        cli._live_test(cfg, 0.0001, False, 60.0)

    assert "push_tx" not in rpc.endpoints()
    assert "execute_signing_instructions" not in rpc.endpoints()


def test_cli_reports_disabled_and_bad_config(tmp_path, capsys) -> None:
    off = tmp_path / "off.yaml"
    off.write_text(yaml.safe_dump({"options": {"enabled": False}}))
    with pytest.raises(SystemExit, match="not enabled"):
        cli.main(["--config", str(off), "status"])

    bad = tmp_path / "bad.yaml"
    bad.write_text(yaml.safe_dump({"options": {"enabled": True, "wallet": {"mode": "trading"}}}))
    assert cli.main(["--config", str(bad), "status"]) == 2
    assert "not supported yet" in capsys.readouterr().err


def test_cli_reads_secrets_yaml_over_the_config_as_the_app_does(tmp_path) -> None:
    # Certificate paths and API keys live in secrets.yaml, beside config.yaml.
    section = {
        "enabled": True,
        "network": "testnet11",
        "wallet": {"port": 9356},
        "quote_asset_id": "ab" * 32,
        "contract_xch": 20,
    }
    path = tmp_path / "config.yaml"
    path.write_text(yaml.safe_dump({"options": section}))
    with pytest.raises(config.OptionsConfigError, match="cert_path and key_path are required"):
        cli._load(str(path))

    secrets = {
        "options": {"wallet": {"cert_path": "c", "key_path": "k", "fingerprint": 42}},
        "coingecko": {"api_key": "CG-secret"},
    }
    (tmp_path / "secrets.yaml").write_text(yaml.safe_dump(secrets))

    cfg = cli._load(str(path))
    assert (cfg.wallet.port, cfg.wallet.cert_path, cfg.wallet.fingerprint) == (9356, "c", 42)
    assert cfg.coingecko_api_key == "CG-secret"


def test_live_test_will_not_start_on_a_wallet_that_is_not_ready(tmp_path, monkeypatch) -> None:
    world = World()
    maker = world.party(3)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd)
    rpc = WalletRpc(world, maker)
    rpc.sync = {"synced": False, "syncing": True}
    monkeypatch.setattr(cli, "wallet_rpc", lambda _cfg: rpc)
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: world.sim)

    with pytest.raises(SystemExit, match="still syncing"):
        cli._live_test(cfg, 0.001, True, 60.0)
    assert "push_tx" not in rpc.endpoints()


def test_status_prints_the_book(tmp_path, monkeypatch, capsys) -> None:
    world = World()
    maker = world.party(3)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd)
    rpc = WalletRpc(world, maker)
    monkeypatch.setattr(
        cli, "build", lambda _cfg: runtime.build(cfg, rpc=rpc, reader=world.sim, clock=world.now)
    )
    book_store = store.OptionStore(cfg.database_path)
    book_store.add_contract(
        launcher_id=b"\x11" * 32,
        slot="C110@5",
        side="call",
        underlying_asset=None,
        underlying_amount=1,
        strike_asset=usd,
        strike_amount=1,
        expiration=5,
        creator_puzzle_hash=maker.puzzle_hash,
        bundle=b"x",
        deadline=9,
    )
    book_store.close()

    assert cli._status(cfg) == 0

    out = capsys.readouterr().out
    assert "3 XCH" in out and "C110@5" in out and "minting" in out and "waiting: mint" in out
    assert "chain is read from coinset" in out
    assert "fees: 0 mojos a transaction" in out
    assert "wallet: synced (no fingerprint set, so whichever key it has open)" in out
    assert "splash: off" in out

    rpc.sync = {"synced": False, "syncing": True}
    assert cli._status(cfg) == 0
    assert "wallet: the options wallet is still syncing" in capsys.readouterr().out
    # Looking is all status does: the wallet's key is never selected by it.
    assert "log_in" not in rpc.endpoints()


def test_status_reports_a_wallet_on_another_key_without_switching_it(
    tmp_path, monkeypatch, capsys
) -> None:
    world = World()
    maker = world.party(3)
    usd = world.issue_cat(world.party(1), 5)
    wallet = {"port": 9356, "cert_path": "c", "key_path": "k", "fingerprint": 42}
    cfg = _config(tmp_path, usd, wallet=wallet)
    rpc = WalletRpc(world, maker)
    rpc.logged_in = 7  # the operator has another key open
    monkeypatch.setattr(
        cli, "build", lambda _cfg: runtime.build(cfg, rpc=rpc, reader=world.sim, clock=world.now)
    )

    assert cli._status(cfg) == 0

    out = capsys.readouterr().out
    assert "wallet: the options wallet has another key open (7)" in out
    assert "log_in" not in rpc.endpoints() and rpc.logged_in == 7

    rpc.logged_in = 42
    assert cli._status(cfg) == 0
    assert "wallet: synced (logged in to 42)" in capsys.readouterr().out
    assert "log_in" not in rpc.endpoints()


def test_status_says_where_offers_are_announced_and_how_many_were_taken(
    tmp_path, monkeypatch, capsys
) -> None:
    world = World()
    maker = world.party(3)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd, splash={"enabled": True, "submit_url": "http://127.0.0.1:4321"})
    rpc = WalletRpc(world, maker)
    monkeypatch.setattr(
        cli, "build", lambda _cfg: runtime.build(cfg, rpc=rpc, reader=world.sim, clock=world.now)
    )
    book_store = store.OptionStore(cfg.database_path)
    book_store.add_contract(
        launcher_id=b"\x11" * 32,
        slot="C110@5",
        side="call",
        underlying_asset=None,
        underlying_amount=1,
        strike_asset=usd,
        strike_amount=1,
        expiration=5,
        creator_puzzle_hash=maker.puzzle_hash,
        bundle=b"x",
        deadline=9,
    )
    ids = [
        book_store.add_offer(
            launcher_id=b"\x11" * 32,
            price_asset=usd,
            price_amount=price,
            expires_at=None,
            bundle=b"o",
            offer_text=f"offer1{price}",
            now=1,
        )
        for price in (5, 6, 7)
    ]
    book_store.set_offer(ids[0], splash_at=2)
    book_store.set_offer(ids[1], splash_at=2)
    book_store.set_offer(ids[1], status="dead")  # no longer live: not counted
    book_store.close()

    assert cli._status(cfg) == 0

    out = capsys.readouterr().out
    assert "splash: new offers are handed to the daemon at http://127.0.0.1:4321" in out
    assert "(1 of 2 live offers taken by it)" in out


# ---------------------------------------------------------------------------
# The node's certificate, over real TLS
# ---------------------------------------------------------------------------


def _openssl(folder, *args: str) -> None:
    # A fixed command over files this test made, found on PATH like any tool.
    command = ["openssl", *args]
    subprocess.run(command, cwd=folder, check=True, capture_output=True)  # noqa: S603


def _authority(folder, name: str) -> None:
    """A certificate authority, and a certificate it issued to "Chia", as Chia's are."""
    key = ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes"]
    _openssl(
        folder, "req", "-x509", *key, "-keyout", f"{name}_ca.key", "-out", f"{name}_ca.crt",
        "-subj", f"/CN={name} CA", "-days", "2", "-addext", "basicConstraints=critical,CA:TRUE",
    )  # fmt: skip
    for who in ("node", "client"):
        _openssl(
            folder, "req", *key, "-keyout", f"{name}_{who}.key", "-out", f"{name}_{who}.csr",
            "-subj", "/CN=Chia",
        )  # fmt: skip
        _openssl(
            folder, "x509", "-req", "-in", f"{name}_{who}.csr", "-CA", f"{name}_ca.crt",
            "-CAkey", f"{name}_ca.key", "-CAcreateserial", "-out", f"{name}_{who}.crt",
            "-days", "2",
        )  # fmt: skip


class _Rpc(http.server.BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802 - the name http.server looks for
        self.rfile.read(int(self.headers.get("Content-Length") or 0))
        body = json.dumps({"success": True, "peak": 7}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args: Any) -> None:
        pass


def _serve(folder, name: str, clients: str):
    """A node RPC on loopback presenting ``name``'s certificate, wanting ``clients``' one."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(folder / f"{name}_node.crt", folder / f"{name}_node.key")
    context.verify_mode = ssl.CERT_REQUIRED  # as a Chia node does
    context.load_verify_locations(folder / f"{clients}_ca.crt")
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Rpc)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


@pytest.mark.skipif(shutil.which("openssl") is None, reason="needs the openssl command")
def test_a_node_s_certificate_is_checked_against_its_own_authority(tmp_path) -> None:
    requests = pytest.importorskip("requests")
    _authority(tmp_path, "ours")
    _authority(tmp_path, "theirs")
    cert, key = str(tmp_path / "ours_client.crt"), str(tmp_path / "ours_client.key")
    ours = str(tmp_path / "ours_ca.crt")
    node = _serve(tmp_path, "ours", clients="ours")
    # Someone in between: any certificate will do for a caller that does not look.
    between = _serve(tmp_path, "theirs", clients="ours")
    try:
        real = f"https://127.0.0.1:{node.server_address[1]}/get_blockchain_state"
        fake = f"https://127.0.0.1:{between.server_address[1]}/get_blockchain_state"
        checked = runtime._node_poster(cert, key, 10.0, ours)

        # The node's certificate is issued to "Chia", not to 127.0.0.1, and is
        # accepted all the same: it is the authority that is checked.
        assert checked(real, {}) == {"success": True, "peak": 7}
        with pytest.raises(requests.exceptions.SSLError):
            checked(fake, {})

        # Unchecked, as is allowed for a node on this machine only, the
        # impostor is believed. That is what the authority is for.
        unchecked = runtime._node_poster(cert, key, 10.0)
        assert unchecked(fake, {}) == {"success": True, "peak": 7}
    finally:
        node.shutdown()
        between.shutdown()


# ---------------------------------------------------------------------------
# Review round 3
# ---------------------------------------------------------------------------


def _waiting_mint(cfg: config.OptionsConfig, world: World, maker: Party) -> None:
    """Put a book mint on record that spends every coin the maker has, unconfirmed."""
    address = maker.puzzle_hash
    terms = builder.OptionTerms(
        builder.Asset(None, 1000), builder.Asset(None, 1000), world.now() + 86400, address
    )
    minted = builder.build_mint(
        terms, builder.Funding(address, xch=maker.xch_coins()), maker.custody()
    )
    book = store.OptionStore(cfg.database_path)
    book.add_contract(
        launcher_id=bytes(minted.launcher_id),
        slot="C110@1",
        side="call",
        underlying_asset=None,
        underlying_amount=1000,
        strike_asset=None,
        strike_amount=1000,
        expiration=world.now() + 86400,
        creator_puzzle_hash=address,
        bundle=cli.pack_bundle(minted.coin_spends, bytes([0xC0]) + bytes(95)),
        deadline=world.now() + 1800,
        now=world.now(),
    )
    book.close()


def test_live_test_leaves_alone_the_coins_a_waiting_transaction_spends(
    tmp_path, monkeypatch
) -> None:
    # The wallet lists a coin as spendable until its spend confirms. A book
    # mint still waiting is counting on its coin; the test must not take it.
    world, maker, cfg, rpc = _live(tmp_path, monkeypatch)
    _waiting_mint(cfg, world, maker)

    with pytest.raises(SystemExit, match="still waiting already spends are not counted"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    assert "push_tx" not in rpc.endpoints()
    assert "execute_signing_instructions" not in rpc.endpoints()


def _timed(tmp_path, monkeypatch, **overrides):
    """As ``_live``, over a chain reader that can tell the chain's time."""
    world, maker, _, rpc = _live(tmp_path, monkeypatch)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd, **overrides)
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: SimChain(world.sim))
    return world, maker, cfg, rpc


def test_live_test_spends_carry_a_deadline_and_their_fee_on_record(tmp_path, monkeypatch) -> None:
    world, maker, cfg, rpc = _timed(
        tmp_path, monkeypatch, fee_mojos="auto", limits={"max_fee_mojos": 10**9}
    )
    rpc.fee_per_cost = 1
    flat = math.ceil(170_000_000 * 5.5)

    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    book, (contract,) = _test_contracts(cfg)
    rows = book.broadcasts(None)
    assert [row.kind for row in rows] == [store.MINT, store.WITHDRAW, store.CANCEL]
    for row in rows:
        assert row.valid_until == row.created_at + cfg.confirm_timeout_seconds
        assert row.fee == flat
    assert contract.status == store.CLOSED


def test_a_live_test_mint_that_runs_out_is_given_up_and_made_again(
    tmp_path, monkeypatch, capsys
) -> None:
    world, maker, cfg, rpc = _timed(tmp_path, monkeypatch, confirm_timeout_seconds=600)
    before = maker.balance()
    rpc._push_tx = lambda body: {}  # no node hears the mint
    with pytest.raises(SystemExit, match="did not confirm in 60 s"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    # Past its deadline, but the chain is not yet 32 blocks past it: nothing is
    # pushed again, and the operator is told when to come back.
    world.advance(700)
    with pytest.raises(SystemExit, match="ran out of time unconfirmed"):
        cli._live_test(cfg, 0.0001, True, 60.0)

    # Later it is given up from the chain's clock, and the test starts afresh.
    del rpc._push_tx
    world.advance(700)
    capsys.readouterr()
    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0

    book, contracts = _test_contracts(cfg)
    lapsed = [event.detail for event in book.events() if event.kind == "mint_failed"]
    assert lapsed == [{"reason": "lapsed"}]
    assert [item.status for item in contracts][-1] == store.CLOSED
    assert book.unresolved() == []
    assert maker.balance() == before


def test_a_live_test_clawback_pays_a_fee_larger_than_its_collateral(
    tmp_path, monkeypatch, capsys
) -> None:
    # Under pressure the flat fee is 935 million mojos; the test's collateral
    # is 100 million. The clawback must draw the difference from other coins.
    world, maker, _, rpc = _live(tmp_path, monkeypatch)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd, fee_mojos="auto", limits={"max_fee_mojos": 10**9})
    rpc.fee_per_cost = 1
    flat = math.ceil(170_000_000 * 5.5)
    assert flat > DUST
    taker = world.party(1)
    before = maker.balance()

    def take_it_first(prompt: str, assume_yes: bool) -> None:
        if "withdrawal" in prompt:
            book = store.OptionStore(cfg.database_path)
            world.take(taker, book.offers()[-1].offer_text)
            book.close()

    monkeypatch.setattr(cli, "_confirm", take_it_first)
    assert cli._live_test(cfg, 0.0001, True, 60.0) == 0  # sold before it could be withdrawn
    monkeypatch.setattr(cli, "_confirm", lambda prompt, assume_yes: None)

    world.advance(24 * 3600 + 60)
    capsys.readouterr()
    cli._live_test(cfg, 0.0001, True, 60.0)

    assert "clawing it back" in capsys.readouterr().out
    book, contracts = _test_contracts(cfg)
    assert contracts[0].status == store.CLOSED
    assert [e.detail["by"] for e in book.events() if e.kind == "collateral_returned"][0] == (
        store.CLAWBACK
    )
    # Collateral and premium are home, less the option coin the buyer keeps and
    # five fees: the mint's, the clawback's, and the three of the fresh test
    # that ran once the old contract was dealt with.
    assert maker.balance() == before + DUST - 1 - 5 * flat


def test_the_options_address_is_the_book_s_own_across_restarts(tmp_path) -> None:
    world = World()
    maker, other = world.party(5), world.party(1)
    rpc = WalletRpc(world, maker)
    book = store.OptionStore(tmp_path / "options_jobs.db")

    first = runtime.WalletCoinSource(rpc, world.sim, prefix="txch", memory=book)
    assert first.puzzle_hash() == maker.puzzle_hash
    assert len(first.xch_coins()) == 1

    # The wallet's current address moves on (something asked it for a new one).
    rpc.party = other
    rpc.calls.clear()
    again = runtime.WalletCoinSource(rpc, world.sim, prefix="txch", memory=book)

    # A new process still uses the address its funds and its options are at,
    # and does not ask the wallet which address is current.
    assert again.puzzle_hash() == maker.puzzle_hash
    assert "get_next_address" not in rpc.endpoints()

    # An address set in the config wins, and is what is remembered from then on.
    chosen = sdk.Address(other.puzzle_hash, "txch").encode()
    set_ = runtime.WalletCoinSource(rpc, world.sim, address=chosen, prefix="txch", memory=book)
    assert set_.puzzle_hash() == other.puzzle_hash
    assert book.recall("options_address") == chosen


def test_the_monitor_sends_the_coingecko_key_and_checks_the_price_s_age(
    tmp_path, monkeypatch
) -> None:
    from gui.services.options import desk as desk_module
    from gui.services.options import market as market_module
    from gui.services.options import monitor as monitor_module

    world = World()
    usd = world.issue_cat(world.party(1), 5)
    section = {
        "enabled": True,
        "wallet": {"port": 9356, "cert_path": "c", "key_path": "k"},
        "quote_asset_id": usd.hex(),
        "contract_xch": 20,
        "database_path": str(tmp_path / "options_jobs.db"),
        "market": {"max_age_seconds": 321},
    }
    cfg = config.parse({"options": section, "coingecko": {"api_key": "CG-secret"}})
    assert cfg is not None
    made: dict[str, Any] = {}

    class Source:
        def __init__(self, asset_id: bytes, **kwargs: Any) -> None:
            made["source"] = kwargs

        def read(self) -> Any:
            raise market_module.MarketError("not asked in this test")

    class Watcher:
        def __init__(self, **kwargs: Any) -> None:
            made["watcher"] = kwargs

        def scan(self) -> Any:
            return monitor_module.ScanReport()

    monkeypatch.setattr(market_module, "MarketSource", Source)
    monkeypatch.setattr(monitor_module, "DeskMonitor", Watcher)
    monkeypatch.setattr(desk_module, "DeskClient", object)
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: world.sim)

    assert cli._monitor(cfg, 0) == 0

    assert made["source"]["coingecko_api_key"] == "CG-secret"
    assert made["watcher"]["max_spot_age_seconds"] == 321


# ---------------------------------------------------------------------------
# Review round 4
# ---------------------------------------------------------------------------


def test_status_does_not_take_an_address_from_another_key(tmp_path, monkeypatch, capsys) -> None:
    # A fresh database, no address set, and the wallet open on some other key.
    # Asking for "the current address" now would be that key's, and it would be
    # kept for good.
    world = World()
    maker = world.party(3)
    usd = world.issue_cat(world.party(1), 5)
    wallet = {"port": 9356, "cert_path": "c", "key_path": "k", "fingerprint": 42}
    cfg = _config(tmp_path, usd, wallet=wallet)
    rpc = WalletRpc(world, maker)
    rpc.logged_in = 7
    monkeypatch.setattr(
        cli, "build", lambda _cfg: runtime.build(cfg, rpc=rpc, reader=world.sim, clock=world.now)
    )

    assert cli._status(cfg) == 0

    out = capsys.readouterr().out
    assert "another key open (7)" in out
    assert "options address: not chosen yet" in out
    assert "get_next_address" not in rpc.endpoints()
    book = store.OptionStore(cfg.database_path)
    assert book.recall("options_address:42") is None and book.recall("options_address") is None
    book.close()

    # On the right key it is asked, shown, and kept under that key's name.
    rpc.logged_in = 42
    assert cli._status(cfg) == 0
    assert "options address txch1" in capsys.readouterr().out
    book = store.OptionStore(cfg.database_path)
    assert book.recall("options_address:42") == sdk.Address(maker.puzzle_hash, "txch").encode()
    book.close()


def test_an_address_kept_for_one_key_is_not_used_for_another(tmp_path) -> None:
    world = World()
    first, second = world.party(5), world.party(1)
    rpc = WalletRpc(world, first)
    book = store.OptionStore(tmp_path / "options_jobs.db")
    one = runtime.WalletCoinSource(rpc, world.sim, prefix="txch", memory=book, fingerprint=42)
    assert one.puzzle_hash() == first.puzzle_hash

    rpc.party = second  # the wallet is now on the other key
    other = runtime.WalletCoinSource(rpc, world.sim, prefix="txch", memory=book, fingerprint=43)

    assert other.known_address() == ""
    assert other.puzzle_hash() == second.puzzle_hash
    again = runtime.WalletCoinSource(rpc, world.sim, prefix="txch", memory=book, fingerprint=42)
    assert again.puzzle_hash() == first.puzzle_hash


def test_an_ipv6_host_is_put_in_brackets_in_the_address_it_is_reached_at(
    tmp_path, monkeypatch
) -> None:
    world = World()
    usd = world.issue_cat(world.party(1), 5)
    asked: list[str] = []

    def poster(cert: str, key: str, timeout: float, ca: str = ""):
        def post(url: str, body: dict) -> dict:
            asked.append(url)
            return {"success": True, "coin_records": []}

        return post

    monkeypatch.setattr(runtime, "_node_poster", poster)
    cfg = _config(
        tmp_path,
        usd,
        node={"host": "::1", "cert_path": "c", "key_path": "k"},
        wallet={"host": "::1", "port": 9356, "cert_path": "c", "key_path": "k"},
    )

    runtime.chain_reader(cfg).coin_state(b"\x01" * 32)

    assert asked == ["https://[::1]:8555/get_coin_records_by_names"]
    assert "your full node at [::1]:8555" in runtime.chain_source(cfg)
    # The wallet's RPC is reached the same way.
    assert runtime.wallet_rpc(cfg).__self__._base == "https://[::1]:9356"  # noqa: SLF001
    for host, written in (("localhost", "localhost"), ("10.0.0.9", "10.0.0.9"), ("[::1]", "[::1]")):
        assert runtime._url_host(host) == written
