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


def test_live_test_runs_mint_offer_withdraw_cancel(tmp_path, monkeypatch, capsys) -> None:
    world = World()
    maker = world.party(1)
    usd = world.issue_cat(world.party(1), 5)
    cfg = _config(tmp_path, usd)
    rpc = WalletRpc(world, maker)
    monkeypatch.setattr(cli, "wallet_rpc", lambda _cfg: rpc)
    monkeypatch.setattr(cli, "chain_reader", lambda _cfg: world.sim)
    monkeypatch.setattr(cli.time, "sleep", lambda _seconds: None)
    monkeypatch.setattr(cli.time, "time", lambda: float(world.now()))
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
