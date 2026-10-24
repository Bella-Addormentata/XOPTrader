"""[MILLIETH-TOPUP 2026-10-24] The ETH fee reserve topped up from wmilliETH.b.

When ETH on Base falls below its band, the rebalancer's sources are, in
order: milliETH already on Base (withdrawn to ETH), wmilliETH.b on Chia
(unwrapped to Base), and -- only with ``buy_on_dexie`` -- one Dexie ask for
wmilliETH.b bought with XCH. These tests pin the worker side: the live-test
gate, the unwrap request, and the Dexie buy's checks and bookkeeping.
"""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")
pytest.importorskip("PySide6")

from gui.services.warp import jobs as J  # noqa: E402
from gui.services.warp import rebalance as rb  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402

WM = "f322a205c034fe28681829fa5a2e483ac421f0952eb1292945c8db06e0a471a6"
WM_FP = "v1:0xf2d5d8ec69e2faed5eb4de90749c87ee314a4b12:3:3:" + WM
MAKER_TID = "aa" * 32
OUR_TID = "dd" * 32

ETH_BAND = rb.AssetBand(target=10 ** 16, tolerance_pct=20)        # 0.01 ETH


def _params(*, buy=False):
    return rb.RebalanceParams(
        enabled=True, eth=ETH_BAND,
        wmillieth=rb.WmilliethSource(enabled=True, buy_on_dexie=buy),
    )


class FakeWallet:
    def __init__(self, *, wm_spendable=0, xch_spendable=50 * 10 ** 12):
        self.wm_spendable = wm_spendable
        self.wm_confirmed = wm_spendable
        self.xch_spendable = xch_spendable
        self.taken: list = []
        self.trade_status = "PENDING_CONFIRM"
        self.summary = {"offered": {WM: 547},
                        "requested": {"xch": 1_827_700_000_000}}
        self.valid = True

    def log_in(self):
        return {}

    def get_wallet_balance(self, wallet_id):
        if wallet_id == 1:
            return {"spendable_balance": self.xch_spendable}
        return {"spendable_balance": self.wm_spendable,
                "confirmed_wallet_balance": self.wm_confirmed}

    def get_offer_summary(self, offer):
        return self.summary

    def check_offer_validity(self, offer):
        return self.valid

    def take_offer(self, offer, *, fee_mojos=0):
        self.taken.append((offer, fee_mojos))
        return {"trade_id": "0x" + OUR_TID, "status": "PENDING_CONFIRM"}

    def get_offer(self, trade_id):
        return {"trade_id": trade_id, "status": self.trade_status}

    def get_my_open_trade_ids(self):
        return set()


class FakeEngine:
    def __init__(self, *, eth_wei=10 ** 15, wm_spendable=0, proven=True):
        from gui.services.warp import constants

        self._net = constants.MAINNET
        self._params = SimpleNamespace(
            enabled=True, dry_run=False, max_micros=500_000_000,
            max_unwrap_micros=500_000_000, min_micros=1_000_000,
            max_unwrap_millieth_mojos=0,                     # unlimited
        )
        self._store = J.WarpJobStore(":memory:")
        if proven:
            job = self._store.create_job(
                "mainnet", status=JobStatus.UNWRAP_CHECKS,
                state={"direction": "out", "manual": True,
                       "asset": "milliETH", "asset_fingerprint": WM_FP})
            self._store.update_job(job.id, status=JobStatus.COMPLETED,
                                   expected_status=JobStatus.UNWRAP_CHECKS)
        self._hot_cache = {"eth_wei": eth_wei, "usdc_micros": 0,
                           "millieth_units": 0, "error": None}
        self._hot_address = "0x" + "ab" * 20
        self._evm = SimpleNamespace(
            get_nonce=lambda addr, pending=True: 7,
            # 90k gas x 0.1 gwei: 9e12 wei, far below the test balances.
            get_fee_data=lambda: SimpleNamespace(max_fee_per_gas=10 ** 8))
        self._wallet = FakeWallet(wm_spendable=wm_spendable)
        self.cat_wallets = [9]
        self.calls: list = []

    def _resolve_cat_wallet_id(self, asset_id=None):
        return 9

    def _cat_wallet_ids(self, asset_id=None):
        return list(self.cat_wallets)

    def refresh_hot_wallet(self):
        return self._hot_cache

    def request_unwrap(self, mojos, receiver, external_relay=False, *,
                       automatic=False, rebalance_target_micros=0,
                       asset="USDC", rebalance_eth_target_wei=0):
        self.calls.append(("unwrap", mojos, receiver, external_relay,
                           automatic, asset, rebalance_eth_target_wei))


def _worker(params, config=None):
    w = S._WarpWorker.__new__(S._WarpWorker)
    w._rebalance_params = params
    w._rebalance_error = ""
    w._rebalance_next_ok = 0.0
    w._rebalance_last = ""
    w._millieth_unwrap_proven = False
    w._config = config or {"strategy": {"fee_reserve_xch": 1.0}}
    return w


def _http(monkeypatch, *, eth_usd=3950.0, xch_usd=1.2, asks=None):
    book = {"offers": asks if asks is not None else [{
        "id": "dx1", "trade_id": MAKER_TID, "offer": "offer1qqexample",
        "status": 0, "offered": [{"id": WM, "amount": 0.547}],
        "requested": [{"id": "xch", "amount": 1.8277}]}]}

    def get_json(url, params, headers):
        if "coingecko" in url:
            return {"ethereum": {"usd": eth_usd}, "chia": {"usd": xch_usd}}
        return book

    monkeypatch.setattr(S, "_http_get_json", get_json)


# -- unwrap -------------------------------------------------------------------

def test_a_breached_band_unwraps_held_wmillieth_by_external_relay():
    w, eng = _worker(_params()), FakeEngine(wm_spendable=5_000)
    w._maybe_rebalance(eng)
    assert len(eng.calls) == 1
    kind, mojos, receiver, ext, auto, asset, target = eng.calls[0]
    assert (kind, receiver, ext, auto, asset) == (
        "unwrap", eng._hot_address, True, True, "milliETH")
    assert target == ETH_BAND.target
    # 0.009 ETH short = 9 milliETH = 9,000 base units, grossed for the
    # 30 bps tip, but only 5,000 mojos are spendable.
    assert mojos == 5_000


def test_the_top_up_waits_for_a_manual_unwrap_to_have_completed():
    w, eng = _worker(_params()), FakeEngine(wm_spendable=5_000, proven=False)
    w._maybe_rebalance(eng)
    assert eng.calls == []
    assert "manual wmilliETH.b unwrap" in w._rebalance_last
    assert w._rebalance_next_ok == 0.0, "a gate skip arms no cooldown"


def test_a_healthy_band_reads_no_chia_balance():
    w, eng = _worker(_params()), FakeEngine(eth_wei=10 ** 16, wm_spendable=5)
    eng._wallet.get_wallet_balance = None          # would raise if called
    w._maybe_rebalance(eng)
    assert eng.calls == []


def test_no_cat_wallet_reads_as_zero_but_an_rpc_failure_does_not():
    w, eng = _worker(_params()), FakeEngine()
    eng.cat_wallets = []
    assert w._chia_millieth_spendable(eng) == 0

    eng.cat_wallets = [9, 12]                 # ambiguous: never "none held"
    with pytest.raises(S.WarpTerminal):
        w._chia_millieth_spendable(eng)

    def rpc_down(wallet_id):
        raise RuntimeError("wallet RPC down")

    eng2 = FakeEngine()
    eng2._wallet.get_wallet_balance = rpc_down
    with pytest.raises(RuntimeError):
        w._chia_millieth_spendable(eng2)


# -- Dexie buy -----------------------------------------------------------------

def test_with_none_held_it_buys_one_verified_ask_and_records_it(monkeypatch):
    # XCH at $1.20, ETH at $3,950: 3.29 XCH per wmilliETH.b; the ask is
    # 3.341, 1.5% over -- inside the 3% default.
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == [("offer1qqexample", 100_000_000)]
    takes = eng._store.list_takes()
    assert len(takes) == 1 and takes[0].trade_id == OUR_TID
    assert (takes[0].cat_mojos, takes[0].xch_mojos, takes[0].fee_mojos) == (
        547, 1_827_700_000_000, 100_000_000)
    assert takes[0].status == J.TAKE_SUBMITTED
    assert "took Dexie offer" in w._rebalance_last


def test_no_second_buy_while_the_first_is_unconfirmed(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    w._rebalance_next_ok = 0.0                      # past the cooldown
    w._maybe_rebalance(eng)
    assert len(eng._wallet.taken) == 1
    assert "not confirmed yet" in w._rebalance_last


def test_takes_settle_from_the_wallets_trade_record(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    eng._wallet.trade_status = "CONFIRMED"
    assert w._settle_dexie_takes(eng) is False
    take = eng._store.get_take(OUR_TID)
    assert take.status == J.TAKE_CONFIRMED and take.confirmed_at


def test_an_unreadable_take_stays_in_flight_then_fails_after_an_hour(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)

    def lost(trade_id):
        raise RuntimeError("trade not found")

    eng._wallet.get_offer = lost
    assert w._settle_dexie_takes(eng) is True
    monkeypatch.setattr(S, "_age_s", lambda iso: S._TAKE_UNKNOWN_FAIL_S + 1)
    assert w._settle_dexie_takes(eng) is False
    assert eng._store.get_take(OUR_TID).status == J.TAKE_FAILED


def test_takes_settle_even_with_the_source_switched_off(monkeypatch):
    """Unticking the source must not strand a submitted take unbooked."""
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    w._rebalance_params = rb.RebalanceParams()        # rebalancer now off
    eng._wallet.trade_status = "CONFIRMED"
    w._rebalance_tick(eng)
    assert eng._store.get_take(OUR_TID).status == J.TAKE_CONFIRMED


def test_a_failed_take_settles_failed(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    eng._wallet.trade_status = "FAILED"
    assert w._settle_dexie_takes(eng) is False
    assert eng._store.get_take(OUR_TID).status == J.TAKE_FAILED


def test_an_overpriced_book_buys_nothing(monkeypatch):
    _http(monkeypatch, xch_usd=1.3)       # 3.04 XCH each: the ask is +10%
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == []
    assert "nothing bought" in w._rebalance_last


def test_an_offer_the_wallet_disagrees_with_is_refused(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    eng._wallet.summary = {"offered": {WM: 500},
                           "requested": {"xch": 1_827_700_000_000}}
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == []
    assert w._rebalance_last.startswith("rebalance failed")
    assert w._rebalance_next_ok > 0, "a refusal cools down"


def test_the_buy_never_spends_the_engines_xch_fee_reserve(monkeypatch):
    _http(monkeypatch)
    w = _worker(_params(buy=True), {"strategy": {"fee_reserve_xch": 2.0}})
    eng = FakeEngine()
    eng._wallet.xch_spendable = 3 * 10 ** 12        # 3 - 1.83 < 2 reserved
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == []
    assert "fee reserve" in w._rebalance_last


def test_no_reference_price_means_no_buy(monkeypatch):
    monkeypatch.setattr(S, "_http_get_json", lambda url, p, h: {})
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == []
    assert "not buying blind" in w._rebalance_last


def test_no_buy_while_wmillieth_is_held_but_locked_in_offers(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    eng._wallet.wm_confirmed = 10_000         # enough held, all of it locked
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == [] and eng.calls == []
    assert "locked in open offers" in w._rebalance_last
    assert w._rebalance_next_ok == 0.0, "a wait arms no cooldown"


def test_buying_is_off_unless_asked_for():
    w, eng = _worker(_params(buy=False)), FakeEngine()
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == [] and eng.calls == []


@pytest.mark.parametrize("raw,mojos", [
    (None, 10 ** 12), ({}, 10 ** 12), ({"fee_reserve_xch": 2.5}, 25 * 10 ** 11),
    ({"fee_reserve_xch": "x"}, 10 ** 12), ({"fee_reserve_xch": float("nan")}, 10 ** 12),
    ({"fee_reserve_xch": -1}, 10 ** 12),
])
def test_the_engines_fee_reserve_is_read_fail_safe(raw, mojos):
    cfg = None if raw is None else {"strategy": raw}
    assert S._engine_xch_fee_reserve_mojos(cfg) == mojos


# -- the takes table ------------------------------------------------------------

def test_a_take_settles_exactly_once():
    store = J.WarpJobStore(":memory:")
    store.record_take(trade_id="0x" + OUR_TID.upper(), asset_id=WM.upper(),
                      cat_mojos=547, xch_mojos=1, fee_mojos=2)
    take = store.get_take(OUR_TID)
    assert take.trade_id == OUR_TID and take.asset_id == WM
    store.settle_take(OUR_TID, J.TAKE_CONFIRMED)
    with pytest.raises(J.StaleJobError):
        store.settle_take(OUR_TID, J.TAKE_FAILED)
    with pytest.raises(J.WarpJobStoreError):
        store.settle_take(OUR_TID, J.TAKE_SUBMITTED)
    assert [t.trade_id for t in store.list_takes(status=J.TAKE_CONFIRMED)] == [
        OUR_TID]


# -- review round 1 (PR #186) ---------------------------------------------------

def test_the_intent_is_recorded_before_the_take(monkeypatch):
    """A lost take response must still leave a row that blocks a second buy."""
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    seen = {}

    def lost_response(offer, *, fee_mojos=0):
        seen["rows"] = [(t.status, t.offer_id)
                        for t in eng._store.list_takes()]
        raise RuntimeError("connection reset after send")

    eng._wallet.take_offer = lost_response
    w._maybe_rebalance(eng)
    assert seen["rows"] == [(J.TAKE_SUBMITTING, MAKER_TID)]
    w._rebalance_next_ok = 0.0
    w._maybe_rebalance(eng)                       # no second buy
    assert "not confirmed yet" in w._rebalance_last
    # An hour later, unresolved, it settles as failed.
    monkeypatch.setattr(S, "_age_s", lambda iso: S._TAKE_UNKNOWN_FAIL_S + 1)
    assert w._settle_dexie_takes(eng) is False
    assert eng._store.list_takes()[0].status == J.TAKE_FAILED


def test_an_accepted_take_is_rekeyed_to_the_wallet_trade_id(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    take = eng._store.get_take(OUR_TID)
    assert (take.status, take.offer_id) == (J.TAKE_SUBMITTED, MAKER_TID)


def test_the_chia_balance_is_not_read_while_base_has_millieth(monkeypatch):
    w, eng = _worker(_params()), FakeEngine()
    # 50 units = 5e13 wei, above the 9e12-wei withdraw at 0.1 gwei.
    eng._hot_cache["millieth_units"] = 50
    eng._cat_wallet_ids = None                    # would raise if consulted
    calls = []

    class _BaseWallet:
        def unwrap_millieth(self, units):
            calls.append(units)

    w._base_wallet = _BaseWallet
    w._maybe_rebalance(eng)
    assert calls == [50]


def test_the_live_test_proof_is_found_beyond_the_newest_jobs():
    w, eng = _worker(_params()), FakeEngine(proven=True)
    for _ in range(3):
        eng._store.create_job("mainnet", status=JobStatus.UNWRAP_CHECKS,
                              state={"direction": "out"})
        job = eng._store.get_active_job()
        eng._store.update_job(job.id, status=JobStatus.CANCELLED,
                              expected_status=JobStatus.UNWRAP_CHECKS)
    assert w._millieth_unwrap_is_proven(eng)


def test_our_open_offers_are_listed_across_every_page():
    from gui.services.warp.wallet import WalletClient, WalletError

    pages = []

    def poster(url, body):
        pages.append((body["start"], body["end"]))
        n = 2 if body["start"] == 0 else 1
        return {"success": True, "trade_records": [
            {"trade_id": f"0x{body['start'] + i:064x}", "is_my_offer": True}
            for i in range(n)]}

    client = WalletClient({}, poster=poster)
    ids = client.get_my_open_trade_ids(page=2)
    assert pages == [(0, 2), (2, 4)]
    assert len(ids) == 3

    def failing(url, body):
        if body["start"] > 0:
            return {"success": False, "error": "busy"}
        return {"success": True, "trade_records": [
            {"trade_id": "0x" + "1" * 64, "is_my_offer": True}] * 2}

    with pytest.raises(WalletError):
        WalletClient({}, poster=failing).get_my_open_trade_ids(page=2)


def test_an_offer_already_attempted_is_never_taken_again(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()

    def refused(offer, *, fee_mojos=0):
        raise RuntimeError("wallet refused the take")

    eng._wallet.take_offer = refused
    w._maybe_rebalance(eng)
    monkeypatch.setattr(S, "_age_s", lambda iso: S._TAKE_UNKNOWN_FAIL_S + 1)
    w._settle_dexie_takes(eng)                     # the attempt -> failed
    eng._wallet.take_offer = FakeWallet().take_offer
    w._rebalance_next_ok = 0.0
    w._maybe_rebalance(eng)
    assert "nothing bought" in w._rebalance_last, "the only ask was tried"



# -- review round 6 (PR #186): the summary's missed issues ---------------------

def test_a_numeric_confirmed_status_settles_with_its_block(monkeypatch):
    _http(monkeypatch)
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    eng._wallet.get_offer = lambda tid: {"status": 4, "confirmed_at_index": 9_400_123}
    assert w._settle_dexie_takes(eng) is False
    take = eng._store.get_take(OUR_TID)
    assert take.status == J.TAKE_CONFIRMED
    assert take.confirmed_height == 9_400_123


def test_numeric_failure_statuses_settle_failed(monkeypatch):
    for status in (3, 5, "5"):
        _http(monkeypatch)
        w, eng = _worker(_params(buy=True)), FakeEngine()
        w._maybe_rebalance(eng)
        eng._wallet.get_offer = lambda tid, s=status: {"status": s}
        assert w._settle_dexie_takes(eng) is False
        assert eng._store.get_take(OUR_TID).status == J.TAKE_FAILED


def test_a_balance_reply_missing_a_field_is_not_zero():
    w, eng = _worker(_params()), FakeEngine()
    eng._wallet.get_wallet_balance = lambda wid: {"spendable_balance": 0}
    with pytest.raises(S.WarpError):
        w._chia_millieth_balances(eng)


def test_the_take_fee_counts_against_max_xch_per_buy(monkeypatch):
    """An ask priced exactly at the cap would debit cap + fee: refused."""
    _http(monkeypatch, xch_usd=1.2)
    params = rb.RebalanceParams(
        enabled=True, eth=ETH_BAND,
        wmillieth=rb.WmilliethSource(
            enabled=True, buy_on_dexie=True,
            max_spend_xch_mojos=1_827_700_000_000,      # exactly the ask
            take_fee_mojos=100_000_000))
    w, eng = _worker(params), FakeEngine()
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == []
    assert "nothing bought" in w._rebalance_last


@pytest.mark.parametrize("raw,mojos", [
    ("2.0005", 2_000), (2.0005, 2_000), ("2", 2_000), (0.0019, 1),
])
def test_the_unwrap_cap_floors_to_whole_mojos(raw, mojos):
    """[review] A cap never rounds UP into a larger burn than stated."""
    assert S._parse_millieth_unwrap_cap({"max_unwrap_millieth": raw}) == mojos


def test_a_cap_below_one_mojo_is_refused():
    with pytest.raises(S.WarpError):
        S._parse_millieth_unwrap_cap({"max_unwrap_millieth": 0.0006})


def test_no_top_up_burn_without_gas_for_the_final_withdraw():
    """[review] External relay delivers milliETH; turning it into ETH is a
    withdraw() the hot wallet pays for. Below that, nothing is burnt."""
    w, eng = _worker(_params()), FakeEngine(eth_wei=10 ** 12, wm_spendable=5_000)
    eng._evm.get_fee_data = lambda: SimpleNamespace(max_fee_per_gas=10 ** 8)
    w._maybe_rebalance(eng)                   # 1e12 < 9e12 worst case
    assert eng.calls == []
    assert "withdraw()" in w._rebalance_last


def test_unknown_base_fees_block_the_burn():
    w, eng = _worker(_params()), FakeEngine(wm_spendable=5_000)

    def down():
        raise RuntimeError("rpc down")

    eng._evm.get_fee_data = down
    w._maybe_rebalance(eng)
    assert eng.calls == []


def test_an_unreadable_trade_record_raises():
    from gui.services.warp.wallet import WalletClient, WalletError

    for reply in ({"success": True}, {"success": True, "trade_record": {}},
                  {"success": True, "trade_record": {"status": ""}}):
        client = WalletClient({}, poster=lambda url, body, r=reply: r)
        with pytest.raises(WalletError):
            client.get_offer("aa" * 32)


def test_the_minimums_cover_the_gas_of_turning_a_top_up_into_eth():
    """[review] 0.1 gwei: a withdraw costs 9e12 wei (9 units), so 10 units;
    a burn pays withdraw + relay (3.4e13 wei) from post-tip proceeds."""
    assert S._millieth_topup_minimums(10 ** 8, 10 ** 12) == (10, 36)
    units, mojos = S._millieth_topup_minimums(3 * 10 ** 9, 10 ** 12)
    assert units * 10 ** 12 > 90_000 * 3 * 10 ** 9
    assert (mojos * 9_970 // 10_000) * 10 ** 12 > 340_000 * 3 * 10 ** 9


def test_held_millieth_below_the_minimum_is_not_withdrawn():
    w, eng = _worker(_params()), FakeEngine(wm_spendable=5_000)
    eng._hot_cache["millieth_units"] = 5          # 5e12 wei < 9e12 of gas
    calls = []

    class _BaseWallet:
        def unwrap_millieth(self, units):
            calls.append(units)

    w._base_wallet = _BaseWallet
    w._maybe_rebalance(eng)
    assert calls == []
    assert eng.calls and eng.calls[0][0] == "unwrap"   # Chia instead


def test_a_cap_below_the_minimum_burns_nothing_and_says_why():
    w, eng = _worker(_params()), FakeEngine(wm_spendable=5_000)
    eng._params.max_unwrap_millieth_mojos = 1
    w._maybe_rebalance(eng)
    assert eng.calls == []
    assert "max_unwrap_millieth (1 mojos)" in w._rebalance_last


def test_a_dexie_buy_skips_asks_too_small_to_unwrap(monkeypatch):
    _http(monkeypatch, asks=[{
        "id": "dx1", "trade_id": MAKER_TID, "offer": "offer1qqexample",
        "status": 0, "offered": [{"id": WM, "amount": 0.002}],
        "requested": [{"id": "xch", "amount": 0.0067}]}])
    w, eng = _worker(_params(buy=True)), FakeEngine()
    w._maybe_rebalance(eng)
    assert eng._wallet.taken == []
    assert "nothing bought" in w._rebalance_last
