"""The wiring and the command line, end to end over a fake wallet RPC.

``WalletRpc`` extends the fake wallet from ``test_reference_wallet`` with the
two read calls the coin source makes, so everything from config.yaml to a
confirmed spend runs here exactly as it would against a wallet daemon, with the
simulator standing in for the chain.
"""

from __future__ import annotations

from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")
yaml = pytest.importorskip("yaml")

from gui.services.options import __main__ as cli  # noqa: E402
from gui.services.options import config, market, runtime, store  # noqa: E402
from gui.services.options.service import OptionsError  # noqa: E402
from gui.services.options.tests.sim_world import CAT, XCH, Party, World  # noqa: E402
from gui.services.options.tests.test_reference_wallet import FakeWallet, _hex  # noqa: E402


class WalletRpc(FakeWallet):
    """The fake wallet plus ``get_next_address``, coin listing and CAT wallets."""

    def __init__(self, world: World, party: Party, cat_asset: bytes | None = None) -> None:
        super().__init__(world, party)
        self.party = party
        self.cat_asset = cat_asset
        self.stranger_coins: list[Any] = []

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
        return {"asset_id": self.cat_asset.hex(), "wallet_id": body["wallet_id"]}


def _config(tmp_path, usd: bytes, **overrides) -> config.OptionsConfig:
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
    parsed = config.parse({"options": section})
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
    assert [event.kind for event in book.events()] == ["collateral_locked", "collateral_returned"]
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
    assert sold.detail == {"asset": None, "amount": DUST}
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
