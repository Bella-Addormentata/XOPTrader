"""[OPTIONS-FENCE] The options sub-account fenced off from every spend.

Config (``engine_fence``, ``target``), the app's own wallet client and its
fence, the engine's fence read off its metrics, Live beside a fenced engine,
and the book's top-ups.
"""

from __future__ import annotations

import types

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import config, engine_guard, fence, runner  # noqa: E402
from gui.services.options.tests.test_config import _section  # noqa: E402
from gui.services.warp.wallet import WalletClient, WalletError  # noqa: E402

ADDRESS = "xch1kxxd76v9kwsxrcxgndn3mylnxjhd7klgdns7083zjl247ffge32sr5rsy2"
PUZZLE = bytes.fromhex("b18cdf6985b3a061e0c89b671d93f334aedf5be86ce1e79e2297d55f2528cc55")
USD = "fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d"
TRADING = {"wallet_host": "localhost", "wallet_port": 9256, "wallet_fingerprint": 481655774}


def _shared(**wallet) -> dict:
    return {
        "port": 9256,
        "cert_path": "c",
        "key_path": "k",
        "fingerprint": 481655774,
        "share_trading_wallet": True,
        "address": ADDRESS,
        **wallet,
    }


def _parse(wallet: dict, **section) -> config.OptionsConfig:
    parsed = config.parse({"options": _section(wallet=wallet, **section), "chia": TRADING})
    assert parsed is not None
    return parsed


# -- config ---------------------------------------------------------------------


def test_the_fence_is_off_by_default_and_needs_the_shared_wallet() -> None:
    assert not _parse(_shared()).wallet.engine_fence
    assert _parse(_shared(engine_fence=True)).wallet.engine_fence
    with pytest.raises(
        config.OptionsConfigError, match="needs options.wallet.share_trading_wallet"
    ):
        config.parse(
            {
                "options": _section(
                    wallet={"port": 9356, "cert_path": "c", "key_path": "k", "engine_fence": True}
                )
            }
        )


def test_the_fence_refuses_funding_from_the_rest_of_the_wallet() -> None:
    with pytest.raises(config.OptionsConfigError, match="cannot both be on"):
        _parse(_shared(engine_fence=True, fund_from_wallet=True))


def test_the_target_is_read_in_whole_units() -> None:
    parsed = _parse(_shared(engine_fence=True), target={"xch": 1.5, "quote": 2})
    assert parsed.target == config.TargetConfig(
        xch_mojos=1_500_000_000_000, quote_mojos=2_000, tolerance=0.2
    )
    assert _parse(_shared()).target is None
    with pytest.raises(config.OptionsConfigError, match="tolerance_pct must be below 100"):
        _parse(_shared(), target={"xch": 1, "tolerance_pct": 100})
    with pytest.raises(config.OptionsConfigError, match="must be a mapping"):
        _parse(_shared(), target=1.5)


# -- the app's own spends -------------------------------------------------------


def test_fenced_address_reads_the_switch_from_the_raw_config() -> None:
    root = {"options": {"enabled": True, "wallet": _shared(engine_fence=True)}}
    assert fence.fenced_address(root) == (ADDRESS, "xch")
    root["options"]["network"] = "testnet11"
    assert fence.fenced_address(root) == (ADDRESS, "txch")
    assert fence.fenced_address({"options": {"enabled": True, "wallet": _shared()}}) is None
    assert (
        fence.fenced_address({"options": {"enabled": False, "wallet": _shared(engine_fence=True)}})
        is None
    )
    assert fence.fenced_address({}) is None


class FakeWallet:
    """One XCH and one wUSDC.b coin at the options address, and one of each elsewhere."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, dict]] = []
        usd_wrapped = bytes(sdk.cat_puzzle_hash(bytes.fromhex(USD), PUZZLE))
        self.coins = {
            1: [("11" * 32, PUZZLE, 500), ("22" * 32, b"\x05" * 32, 900)],
            2: [("33" * 32, usd_wrapped, 1000), ("44" * 32, b"\x06" * 32, 70)],
        }
        self.fail_listing = False

    def __call__(self, url_or_endpoint: str, body: dict) -> dict:
        endpoint = url_or_endpoint.rsplit("/", 1)[-1]
        self.calls.append((endpoint, body))
        if endpoint == "get_wallets":
            return {
                "success": True,
                "wallets": [{"id": 1, "type": 0}, {"id": 2, "type": 6}, {"id": 3, "type": 10}],
            }
        if endpoint == "cat_get_asset_id":
            return {"success": True, "asset_id": "0x" + USD}
        if endpoint == "get_spendable_coins":
            if self.fail_listing:
                return {"success": False, "error": "wallet not synced"}
            return {
                "success": True,
                "confirmed_records": [
                    {
                        "coin": {
                            "parent_coin_info": "0x" + p,
                            "puzzle_hash": "0x" + ph.hex(),
                            "amount": a,
                        }
                    }
                    for p, ph, a in self.coins[int(body["wallet_id"])]
                ],
            }
        return {"success": True, "transaction_id": "0xabc"}

    def ids_at_the_address(self) -> set[str]:
        out = set()
        for coins in self.coins.values():
            for parent, ph, amount in coins[:1]:
                out.add("0x" + bytes(sdk.Coin(bytes.fromhex(parent), ph, amount).coin_id()).hex())
        return out


def _client(wallet: FakeWallet) -> WalletClient:
    client = WalletClient({"host": "localhost", "port": 9256}, poster=wallet)
    client.set_fence(fence.WalletFence(client._post, ADDRESS, "xch").coin_ids)
    return client


def test_every_spend_of_the_app_leaves_the_sub_account_alone() -> None:
    wallet = FakeWallet()
    client = _client(wallet)
    client._post("send_transaction", {"wallet_id": 1, "amount": 5, "address": "xch1x", "fee": 0})
    client._post("cat_spend", {"wallet_id": 2, "amount": 5, "inner_address": "xch1x", "fee": 0})
    client._post("take_offer", {"offer": "offer1x", "fee": 0, "excluded_coin_ids": ["0xAB"]})
    spends = [body for endpoint, body in wallet.calls if endpoint in fence.SPENDING]
    assert len(spends) == 3
    for body in spends:
        assert wallet.ids_at_the_address() <= set(body["excluded_coin_ids"])
    assert "0xab" in spends[2]["excluded_coin_ids"], "a caller's own exclusions stay"
    # Listing calls are not fenced, and the list is kept for a while.
    assert all(
        "excluded_coin_ids" not in body
        for endpoint, body in wallet.calls
        if endpoint == "get_wallets"
    )
    assert sum(1 for endpoint, _ in wallet.calls if endpoint == "get_wallets") == 1


def test_a_fence_that_cannot_list_its_coins_refuses_the_spend() -> None:
    wallet = FakeWallet()
    wallet.fail_listing = True
    client = _client(wallet)
    with pytest.raises(WalletError, match="send_transaction refused"):
        client._post(
            "send_transaction", {"wallet_id": 1, "amount": 5, "address": "xch1x", "fee": 0}
        )
    assert not any(endpoint == "send_transaction" for endpoint, _ in wallet.calls)
    bad = WalletClient({"host": "localhost", "port": 9256}, poster=FakeWallet())
    bad.set_fence(fence.WalletFence(bad._post, "xch1notanaddress", "xch").coin_ids)
    with pytest.raises(WalletError, match="not valid"):
        bad._post("cat_spend", {"wallet_id": 2, "amount": 1, "inner_address": "x", "fee": 0})


# -- the engine's fence, read off its metrics ---------------------------------------

METRICS = """# HELP xop_options_fence_enabled 1 when on
xop_options_fence_enabled 1
xop_options_fence_fresh 1
xop_options_fence_coins 3
xop_options_fence_age_seconds 12.5
"""


def test_the_engine_s_fence_is_read_from_its_metrics() -> None:
    state = engine_guard.parse_fence(METRICS)
    assert state.holds and state.coins == 3 and state.age_seconds == 12.5
    assert not engine_guard.parse_fence("xop_posting_gated 0\n").reported
    assert not engine_guard.parse_fence(METRICS.replace("fresh 1", "fresh 0")).holds
    assert not engine_guard.parse_fence(METRICS.replace("enabled 1", "enabled 0")).holds
    assert not engine_guard.parse_fence(METRICS.replace("12.5", "600")).holds, "stale refresh"
    unreachable = engine_guard.fence_state(
        "http://localhost:1/metrics", fetch=lambda url: (_ for _ in ()).throw(OSError("refused"))
    )
    assert not unreachable.reported and not unreachable.holds


UP = engine_guard.EngineState(running=True, certain=True, detail="xop_trader.exe process 7")


def test_a_signature_beside_a_running_engine_needs_its_fence(monkeypatch) -> None:
    signed: list[str] = []
    inner = types.SimpleNamespace(sign=lambda spends: signed.append("sign") or b"s")
    monkeypatch.setattr(engine_guard, "engine_state", lambda url: UP)
    monkeypatch.setattr(engine_guard, "fence_state", lambda url: engine_guard.parse_fence(METRICS))
    assert engine_guard.EngineCheckedWallet(inner, "u", fenced=True).sign([]) == b"s"
    with pytest.raises(engine_guard.EngineRunningError, match="refused, trading engine up"):
        engine_guard.EngineCheckedWallet(inner, "u").sign([])
    monkeypatch.setattr(
        engine_guard,
        "fence_state",
        lambda url: engine_guard.parse_fence(METRICS.replace("fresh 1", "fresh 0")),
    )
    with pytest.raises(engine_guard.EngineRunningError, match="without a working fence"):
        engine_guard.EngineCheckedWallet(inner, "u", fenced=True).sign([])
    assert signed == ["sign"]


def test_live_beside_a_fenced_engine(tmp_path) -> None:
    yaml = pytest.importorskip("yaml")
    root = {
        "chia": TRADING | {"wallet_fingerprint": 481655774},
        "options": {
            "enabled": True,
            "wallet": _shared(engine_fence=True),
            "quote_asset_id": USD,
            "contract_xch": 1,
            "database_path": ":memory:",
        },
    }
    path = tmp_path / "config.yaml"
    path.write_text(yaml.safe_dump(root), encoding="utf-8")
    holds = engine_guard.parse_fence(METRICS)
    broken = engine_guard.parse_fence("")

    run = runner.OptionsRunner(
        path,
        interval=0.05,
        build=lambda cfg: (_ for _ in ()).throw(RuntimeError("no build here")),
        engine_state=lambda url: UP,
        fence_state=lambda url: broken,
    )
    with pytest.raises(runner.RunnerError, match="Live refused"):
        run.start(runner.Mode.LIVE)

    from gui.services.options import store
    from gui.services.options.tests.test_runner import FakeBook

    made: list = []

    def build(cfg):
        book = FakeBook(cfg)
        made.append(book)
        return book, store.OptionStore(cfg.database_path), None

    run = runner.OptionsRunner(
        path, interval=0.05, build=build, engine_state=lambda url: UP, fence_state=lambda url: holds
    )
    try:
        run.start(runner.Mode.LIVE)
        from gui.services.options.tests.test_runner import _wait

        _wait(lambda: made and made[0].steps >= 1)
        assert "refused" not in run.snapshot().held
        # A fenced Live book does not hold the engine either.
        assert runner.engine_start_blocker(path) is None
    finally:
        run.stop()


# -- the book's top-ups -------------------------------------------------------------


class FakeCoins:
    def __init__(self) -> None:
        self.sent: list[tuple] = []
        self.confirmed: dict[str, bool] = {}

    def send_to_address(self, *, asset_id, amount, fee):
        self.sent.append((asset_id, amount, fee))
        tx = f"0xtx{len(self.sent)}"
        self.confirmed[tx] = False
        return tx

    def transaction_confirmed(self, tx):
        return self.confirmed[tx]


def _book(cfg, loose_xch: int, loose_quote: int):
    from gui.services.options import store
    from gui.services.options.service import OptionsBook

    book_store = store.OptionStore(":memory:")
    coins = FakeCoins()
    stub = types.SimpleNamespace(_config=cfg, _store=book_store, _coins=coins)
    stub._listed = lambda what, asset_id=None: (
        [types.SimpleNamespace(amount=loose_xch)]
        if asset_id is None
        else [types.SimpleNamespace(coin=types.SimpleNamespace(amount=loose_quote))]
    )
    stub._held = lambda asset_id: OptionsBook._held(stub, asset_id)
    stub._top_up_fee = lambda: OptionsBook._top_up_fee(stub)
    stub._TOP_UP_FEE = OptionsBook._TOP_UP_FEE
    stub._TOP_UP_FORGET_SECONDS = OptionsBook._TOP_UP_FORGET_SECONDS
    return stub, coins, lambda report, act=True: OptionsBook._top_up(stub, 100, report, act=act)


def test_the_book_tops_its_address_up_to_target_once_at_a_time() -> None:
    from gui.services.options.service import StepReport

    limits = dict(_section()["limits"], max_fee_mojos=10)
    cfg = _parse(
        _shared(engine_fence=True), target={"xch": 1.5, "quote": 2}, fee_mojos=5, limits=limits
    )
    stub, coins, top_up = _book(cfg, loose_xch=1_000_000_000_000, loose_quote=2_000)
    report = StepReport()
    top_up(report)
    # 1.0 of 1.5 XCH is below 80% of target; the quote asset is at target.
    assert coins.sent == [(None, 500_000_000_000, 5)]
    top_up(StepReport())
    assert len(coins.sent) == 1, "one top-up at a time: the first has not confirmed"
    coins.confirmed["0xtx1"] = True
    later = StepReport()
    top_up(later)
    assert any("confirmed" in line for line in later.actions)
    assert len(coins.sent) == 2, "still short (this fake's balance does not move): sent again"


def test_a_dry_run_only_says_and_no_fence_means_no_top_up() -> None:
    from gui.services.options.service import StepReport

    cfg = _parse(_shared(engine_fence=True), target={"xch": 1.5, "quote": 2})
    stub, coins, top_up = _book(cfg, loose_xch=0, loose_quote=0)
    report = StepReport()
    top_up(report, act=False)
    assert coins.sent == []
    assert sum("dry run: would top up" in line for line in report.actions) == 2

    plain = _parse(_shared(), target={"xch": 1.5, "quote": 2})
    stub, coins, top_up = _book(plain, loose_xch=0, loose_quote=0)
    top_up(StepReport())
    assert coins.sent == []


def test_locked_collateral_counts_toward_the_target() -> None:
    from gui.services.options.service import StepReport

    cfg = _parse(_shared(engine_fence=True), target={"xch": 1.5})
    stub, coins, top_up = _book(cfg, loose_xch=500_000_000_000, loose_quote=0)
    stub._store.add_contract(
        launcher_id=b"\x07" * 32,
        slot="C110@1",
        side="call",
        underlying_asset=None,
        underlying_amount=1_000_000_000_000,
        strike_asset=bytes.fromhex(USD),
        strike_amount=625,
        expiration=2_000_000_000,
        creator_puzzle_hash=PUZZLE,
        bundle=b"",
        deadline=1,
        now=0,
    )
    top_up(StepReport())
    assert coins.sent == [], "0.5 loose + 1.0 locked = the 1.5 XCH target"
