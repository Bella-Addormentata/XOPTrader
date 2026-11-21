"""The Circuit page's figures, from the answers the live server gave on 2026-11-21.

``fixtures/`` holds those answers as the probe saved them on the trading PC
(``vaults.json`` cut to 15 of the 214 vaults: every flagged one, the ten
closest to liquidation and two with no debt). NOW is when they were read.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

from gui.services.circuit import model

FIXTURES = Path(__file__).parent / "fixtures"
#: 2026-11-21 18:22:40 UTC: the oracle answer's cutoff plus its price delay.
NOW = 1_795_285_360


def _answers() -> dict:
    return {
        path.stem: json.loads(path.read_text(encoding="utf-8")) for path in FIXTURES.glob("*.json")
    }


@pytest.fixture
def answers() -> dict:
    return _answers()


@pytest.fixture
def view(answers):
    return model.build_view(answers, now=NOW)


def test_every_saved_answer_builds_its_section(view) -> None:
    assert view.problems == {}
    for field in ("oracle", "protocol", "vaults", "treasury", "bills", "announcers"):
        assert getattr(view, field) is not None, field
    assert view.recharge_auctions is not None and view.surplus_auctions == ()


def test_the_price_the_protocol_uses_and_the_one_coming(view) -> None:
    oracle = view.oracle
    assert oracle.protocol_price == model.PricePoint(0.94, 1_795_271_101)
    assert oracle.matured == model.PricePoint(0.94, 1_795_271_101)
    assert oracle.price_delay_s == 3600
    # Announced at 17:43:57 UTC, usable an hour later (18:43:57).
    assert oracle.queued == ((model.PricePoint(0.96, 1_795_283_037), 1_795_286_637),)
    assert view.price_usd == 0.94


def test_the_statutes_say_what_borrowing_costs(view) -> None:
    protocol = view.protocol
    assert protocol.status == "ok"
    assert protocol.block_issuance == 0 and not protocol.emergency
    # Per-minute discount factors: 10000000801 and 10000000746.
    assert protocol.stability_fee_apy == pytest.approx(0.0430, abs=5e-5)
    assert protocol.savings_apy == pytest.approx(0.0400, abs=5e-5)
    assert protocol.liquidation_ratio_pct == 166
    assert protocol.liquidation_penalty_pct == 13
    assert protocol.collateral_locked_xch == pytest.approx(67_657.120, abs=1e-3)
    assert protocol.collateral_cap_xch == 1_000_000
    assert protocol.price_updates == 3085


def test_the_statutes_come_in_the_protocol_s_order(view) -> None:
    statutes = view.protocol.statutes
    assert len(statutes) == 44
    assert [index for index, _name, _value in statutes] == list(range(44))
    assert statutes[0][1] == "ORACLE_LAUNCHER_ID"
    assert statutes[-1] == (43, "BLOCK_ISSUANCE", 0)


def test_the_emergency_switch_is_seen(answers) -> None:
    answers["statutes"]["implemented_statutes"]["BLOCK_ISSUANCE"] = 1
    assert model.build_view(answers, now=NOW).protocol.emergency


def test_the_vaults_are_counted_and_sorted(view) -> None:
    vaults = view.vaults
    assert vaults.count == 15
    assert vaults.with_debt == 13
    assert vaults.debt_byc == pytest.approx(15_268.374)
    assert [row.name[:8] for row in vaults.nearing] == ["58b72f09"]
    assert vaults.waiting == ()
    assert [row.name[:8] for row in vaults.bad_debt] == ["3a5e4e71", "185a2766"]
    assert vaults.bad_debt_byc == pytest.approx(23.989)
    # Seized vaults are not "closest to liquidation": they are past it.
    closest = vaults.closest
    assert len(closest) == model.CLOSEST_VAULTS
    assert [row.liquidation_usd for row in closest] == [
        0.81,
        0.78,
        0.75,
        0.75,
        0.73,
        0.72,
        0.68,
        0.68,
    ]
    first = closest[0]
    assert first.state == "nearing liquidation"
    assert first.collateral_xch == 1294
    assert first.debt_byc == pytest.approx(636.330)
    assert first.ratio == pytest.approx(1.9115, abs=1e-4)
    assert view.drop_to_liquidation(first) == pytest.approx(1 - 0.81 / 0.94)


def test_a_liquidatable_vault_nobody_has_started_an_auction_for_is_listed_once(answers) -> None:
    (vault,) = [v for v in answers["vaults"] if v["name"].startswith("b33cc50c")]
    vault.update(is_liquidatable=True, is_startable=True, liquidation_price=99)
    vaults = model.build_view(answers, now=NOW).vaults
    assert [row.name[:8] for row in vaults.waiting] == ["b33cc50c"]
    assert vaults.waiting[0].state == "auction can be started"
    # Not again among the closest, which still lists eight others.
    assert "b33cc50c" not in [row.name[:8] for row in vaults.closest]
    assert len(vaults.closest) == model.CLOSEST_VAULTS


def test_a_running_liquidation_auction_says_its_price_and_time_left(answers) -> None:
    vaults = model.build_view(answers, now=NOW + 60, answered_at={"vaults": NOW}).vaults
    (auction,) = vaults.in_auction
    assert auction.name.startswith("5a51ec09")
    assert auction.state == "auction running, open to bids"
    assert auction.collateral_xch == pytest.approx(2_808.665, abs=1e-3)
    assert auction.debt_byc == pytest.approx(1_278.980)
    # 110% of the $0.94 the protocol uses: the auction has just started.
    assert auction.auction_usd == 1.03
    # 905 s left when the answer came, whatever the time now.
    assert auction.auction_ends_at == NOW + 905


def test_a_vault_with_no_debt_has_no_ratio(view) -> None:
    # The server says "inf" for these; the page says nothing.
    raw = [vault for vault in _answers()["vaults"] if vault["debt"] == 0]
    assert raw and all(vault["collateral_ratio"] == "inf" for vault in raw)
    rows = [model._vault_row(vault, NOW) for vault in raw]
    assert all(row.ratio is None and row.liquidation_usd is None for row in rows)
    assert not {row.name for row in rows} & {row.name for row in view.vaults.closest}


def test_all_the_collateral_against_all_the_debt(view) -> None:
    expected = view.vaults.collateral_xch * 0.94 / view.vaults.debt_byc
    assert view.collateral_ratio() == pytest.approx(expected)


def test_the_treasury(view) -> None:
    treasury = view.treasury
    assert treasury.balance_byc == pytest.approx(968.519)
    assert (treasury.minimum_byc, treasury.maximum_byc) == (0, 950)
    assert treasury.coins == 3 and treasury.is_ring and treasury.can_rebalance
    assert not treasury.can_start_recharge and not treasury.can_start_surplus


def test_the_recharge_auction_on_standby(view) -> None:
    (row,) = view.recharge_auctions
    assert row.kind == "Recharge" and row.status == "STANDBY"
    assert row.detail == "not started" and not row.can_be_settled


def test_a_recharge_auction_with_a_bid_and_one_without() -> None:
    base = {"name": "aa" * 32, "status": "RUNNING", "expired": False, "can_be_settled": False}
    bid = {
        "bid_expires_at": NOW + 600,
        "bid_expires_in": 600,
        "byc_bid_amount": 5_000_000,
        "crt_bid_amount": 120_000_000,
        "target_puzzle_hash": "bb" * 32,
        "timestamp": NOW,
    }
    params = {"auction_expires_at": NOW + 3600}
    rows = model.auctions_from(
        [
            dict(base, last_bid=bid, auction_params=params),
            dict(base, last_bid=None, auction_params=params),
        ],
        "recharge",
        NOW,
    )
    assert rows[0].detail == "last bid 5,000 BYC for 120,000 CRT; the bid stands until 18:32 UTC"
    assert rows[1].detail == "no bids yet; ends 19:22 UTC"


def test_a_surplus_auction_from_the_schema() -> None:
    auction = {
        "name": "cc" * 32,
        "launcher_id": "dd" * 32,
        "byc_lot_amount": 50_000,
        "crt_bid_amount": 1_500_000,
        "last_bid": {"bid_expires_at": NOW + 1200, "bid_expires_in": 1200, "timestamp": NOW},
        "expired": False,
        "can_be_settled": False,
        "status": "RUNNING",
    }
    (row,) = model.auctions_from([auction], "surplus", NOW)
    assert row.kind == "Surplus" and row.status == "RUNNING"
    assert row.detail == "50 BYC lot; best bid 1,500 CRT, standing until 18:42 UTC"
    no_bid = dict(auction, last_bid=None)
    assert model.auctions_from([no_bid], "surplus", NOW)[0].detail == "50 BYC lot; no bids yet"


def test_the_governance_coin(view) -> None:
    (row,) = view.bills
    assert row.crt == 31_000_000
    assert row.proposal == "no bill"
    assert row.status == "last step: implement_bill"
    assert row.exit == "can leave governance"


def test_a_bill_being_voted_on() -> None:
    coin = {
        "name": "ee" * 32,
        "amount": 40_000_000_000,
        "bill": {"statute_name": "STABILITY_FEE_DF", "value": 10000000900},
        "status": {"status": "VETOABLE", "status_expires_at": NOW + 1800},
        "exitable_at": NOW + 3 * 86_400,
        "operation": "propose",
        "timestamp": NOW,
        "spent": False,
    }
    (row,) = model.bills_from([coin, dict(coin, spent=True)], NOW)
    assert row.proposal == "STABILITY_FEE_DF = 10000000900"
    assert row.status == "VETOABLE until 18:52 UTC"
    assert row.exit == "can leave in 3 days"


def test_the_oracle_s_announcers(view) -> None:
    rows = view.announcers
    assert len(rows) == 4
    assert {row.price_usd for row in rows} == {0.97}
    assert all(row.state == "valid" for row in rows)
    assert rows[0].expires == "expires in 8 min"
    assert rows[0].deposit_xch == pytest.approx(5.300, abs=1e-3)


def test_an_announcer_in_trouble() -> None:
    announcer = {
        "launcher_id": "ff" * 32,
        "approved": False,
        "valid": False,
        "penalizable": True,
        "in_cooldown": True,
        "price": 120,
        "deposit": 0,
        "expires_at": NOW - 300,
        "spent": False,
    }
    (row,) = model.announcers_from([announcer], NOW)
    assert row.state == "not valid, not approved, can be penalised, in cooldown"
    assert row.expires == "expired 5 min ago"


# -- answers that are missing, failed or of another shape ------------------------


def test_a_read_not_made_yet_leaves_its_section_empty_without_a_note() -> None:
    view = model.build_view({}, now=NOW)
    assert view.oracle is None and view.vaults is None and view.problems == {}


def test_a_failed_read_says_why() -> None:
    view = model.build_view({}, now=NOW, errors={"vaults": "POST /vaults: HTTP 503: down"})
    assert view.vaults is None
    assert view.problems == {"vaults": "POST /vaults: HTTP 503: down"}


def test_an_older_answer_stays_with_a_note_when_the_latest_read_failed(answers) -> None:
    view = model.build_view(answers, now=NOW, errors={"treasury": "POST /treasury: no answer"})
    assert view.treasury is not None
    assert view.problems == {"treasury": "POST /treasury: no answer"}


@pytest.mark.parametrize(
    ("read", "answer"),
    [
        ("oracle", {}),
        ("oracle", {"price_delay": "soon"}),
        ("statutes", {"implemented_statutes": []}),
        ("vaults", {"vaults": []}),
        ("vaults", [{"name": "x", "debt": "lots", "collateral": 1}]),
        ("treasury", []),
        ("recharge_auctions", {}),
        ("surplus_auctions", [{"name": "x"}]),
        ("bills", [{"name": "x"}]),
        ("announcers", [{"launcher_id": "x", "price": True}]),
    ],
)
def test_an_answer_of_another_shape_is_a_note_not_a_crash(answers, read, answer) -> None:
    changed = copy.deepcopy(answers)
    changed[read] = answer
    view = model.build_view(changed, now=NOW)
    assert read in view.problems
    assert view.problems[read].startswith("the answer has an unexpected shape")
    # The other sections are untouched.
    others = set(view.problems) - {read}
    assert not others


# -- formatting ---------------------------------------------------------------------


@pytest.mark.parametrize(
    ("value", "text"),
    [
        (0, "0"),
        (0.0004, "0"),
        (5.3004, "5.3"),
        (636.33, "636.33"),
        (968.519, "968.519"),
        (1000, "1,000"),
        (18_537.813, "18,538"),
    ],
)
def test_amounts(value, text) -> None:
    assert model.fmt_units(value) == text


@pytest.mark.parametrize(
    ("seconds", "text"),
    [
        (0, "0 s"),
        (89, "89 s"),
        (90, "2 min"),
        (1281, "21 min"),
        (5399, "90 min"),
        (5400, "1 h 30 min"),
        (7200, "2 h"),
        (172_799, "47 h 59 min"),
        (172_800, "2 days"),
    ],
)
def test_durations(seconds, text) -> None:
    assert model.fmt_duration(seconds) == text


def test_times() -> None:
    assert model.fmt_from_now(NOW + 1281, NOW) == "in 21 min"
    assert model.fmt_from_now(NOW - 300, NOW) == "5 min ago"
    assert model.fmt_utc(NOW, NOW) == "18:22 UTC"
    assert model.fmt_utc(NOW - 2 * 86_400, NOW) == "2026-11-19 18:22 UTC"


def test_ids_rates_and_shares() -> None:
    name = "58b72f094d3521e10c4034f37ee473877b2d92ffffedc59a25d75e79977c5bf1"
    assert model.short_id(name) == "58b72f09...5bf1"
    assert model.short_id("abc") == "abc"
    assert model.fmt_usd(0.94) == "$0.94"
    assert model.fmt_pct(1.9115) == "191%"
    assert model.fmt_pct(0.04301, 2) == "4.30%"
    for junk in (None, True, "10000000801", 0, -5):
        assert model.apy_from_df(junk) is None
    assert model.apy_from_df(10**10) == 0
