"""The grid: what gets minted, retired and asked."""

from __future__ import annotations

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import config, market, quoting, store  # noqa: E402

USD = bytes.fromhex("fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d")
XCH = config.XCH_MOJOS
NOW = 1_000_000
DAY = 86400
NEAR, FAR = NOW + 7 * DAY, NOW + 30 * DAY
ME = b"\x33" * 32


def _config(**overrides) -> config.OptionsConfig:
    section = {
        "enabled": True,
        "wallet": {"port": 9356, "cert_path": "c", "key_path": "k"},
        "quote_asset_id": USD.hex(),
        "contract_xch": 20,
        "expiries": [NEAR, FAR],
        "call_strikes_pct": [100, 110],
        "put_strikes_pct": [90],
        "strike_step": 0.05,
        "limits": {
            "max_locked_xch": "unlimited",
            "max_locked_quote": "unlimited",
            "max_contracts": "unlimited",
            "max_contracts_per_expiry": "unlimited",
        },
    }
    for key, value in overrides.items():
        if isinstance(value, dict) and isinstance(section.get(key), dict):
            section[key] = {**section[key], **value}
        else:
            section[key] = value
    parsed = config.parse({"options": section})
    assert parsed is not None
    return parsed


def _market(spot: float = 1.10) -> market.Market:
    return market.Market(spot=spot, sigma=0.86, bid=spot * 0.98, ask=spot * 1.02, as_of=NOW)


def _contract(wanted: quoting.Wanted, status: str, tag: int) -> store.Contract:
    return store.Contract(
        launcher_id=bytes([tag]) * 32,
        slot=wanted.slot,
        side=wanted.side,
        underlying_asset=wanted.underlying.asset_id,
        underlying_amount=wanted.underlying.amount,
        strike_asset=wanted.strike_asset.asset_id,
        strike_amount=wanted.strike_asset.amount,
        expiration=wanted.expiration,
        creator_puzzle_hash=ME,
        status=status,
        created_at=NOW,
        updated_at=NOW,
    )


@pytest.mark.parametrize(
    "spot, percent, step, strike",
    [
        (1.10, 110, 0.05, 1.20),
        (1.10, 90, 0.05, 1.00),
        (1.13, 100, 0.05, 1.15),
        (0.01, 50, 0.05, 0.05),
    ],
)
def test_strikes_snap_to_the_grid(spot, percent, step, strike) -> None:
    assert quoting.strike_for(spot, percent, step) == pytest.approx(strike)


def test_an_empty_book_wants_the_whole_grid_nearest_first() -> None:
    plan = quoting.plan(_config(), _market(), [], NOW)

    assert len(plan.mint) == 6  # two expiries x (two calls + one put)
    assert [item.expiration for item in plan.mint] == [NEAR] * 3 + [FAR] * 3
    # Within an expiry, nearest the money first.
    assert [(item.side, item.strike) for item in plan.mint[:3]] == [
        ("call", 1.10),
        ("call", 1.20),
        ("put", 1.00),
    ]
    assert plan.retire == () and plan.asks == {} and plan.blocked == ""


def test_a_call_locks_xch_and_a_put_locks_the_quote_asset() -> None:
    plan = quoting.plan(_config(), _market(), [], NOW)
    call = next(item for item in plan.mint if item.side == "call" and item.strike == 1.20)
    put = next(item for item in plan.mint if item.side == "put")

    assert (call.underlying.asset_id, call.underlying.amount) == (None, 20 * XCH)
    assert (call.strike_asset.asset_id, call.strike_asset.amount) == (USD, 24_000)
    assert (put.underlying.asset_id, put.underlying.amount) == (USD, 20_000)
    assert (put.strike_asset.asset_id, put.strike_asset.amount) == (None, 20 * XCH)
    terms = call.terms(ME)
    assert terms.creator_puzzle_hash == ME and terms.expiration_seconds == NEAR


def test_an_unsold_contract_fills_its_slot_and_gets_an_ask() -> None:
    cfg = _config()
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    held = [_contract(wanted[0], store.OPEN, 1), _contract(wanted[1], store.MINTING, 2)]

    plan = quoting.plan(cfg, _market(), held, NOW)

    assert {item.slot for item in plan.mint} == {item.slot for item in wanted[2:]}
    assert set(plan.asks) == {held[0].launcher_id}  # nothing to offer until the mint confirms
    assert plan.asks[held[0].launcher_id] > 0


def test_a_sold_contract_frees_its_slot_but_still_counts() -> None:
    cfg = _config(limits={"max_contracts": 6})
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    sold = _contract(wanted[0], store.SOLD, 1)

    plan = quoting.plan(cfg, _market(), [sold], NOW)

    assert wanted[0].slot in {item.slot for item in plan.mint}
    assert len(plan.mint) == 5  # six allowed, one already out there
    assert plan.blocked == "options.limits.max_contracts reached"


def test_unset_limits_block_all_minting() -> None:
    cfg = _config(limits={"max_locked_quote": None})
    plan = quoting.plan(cfg, _market(), [], NOW)
    assert plan.mint == ()
    assert "not set" in plan.blocked


def test_collateral_limits_are_per_asset() -> None:
    cfg = _config(limits={"max_locked_xch": 45, "max_locked_quote": 25})
    plan = quoting.plan(cfg, _market(), [], NOW)
    assert [item.side for item in plan.mint] == ["call", "call", "put"]  # 40 XCH and 20 quote


def test_per_expiry_limit() -> None:
    cfg = _config(limits={"max_contracts_per_expiry": 1})
    plan = quoting.plan(cfg, _market(), [], NOW)
    assert [item.expiration for item in plan.mint] == [NEAR, FAR]


def test_expiries_inside_the_stop_window_are_not_quoted() -> None:
    cfg = _config(stop_before_expiry_seconds=8 * DAY)
    plan = quoting.plan(cfg, _market(), [], NOW)
    assert {item.expiration for item in plan.mint} == {FAR}


def test_contracts_near_expiry_or_out_of_place_are_retired() -> None:
    cfg = _config(strike_drift_pct=10)
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    held = [_contract(item, store.OPEN, index + 1) for index, item in enumerate(wanted[:3])]

    moved = quoting.plan(cfg, _market(spot=1.60), held, NOW)
    assert set(moved.retire) == {item.launcher_id for item in held}
    assert moved.asks == {}

    late = quoting.plan(cfg, _market(), held, NEAR - 3600)
    assert set(late.retire) == {item.launcher_id for item in held}


def test_a_minting_contract_is_never_retired() -> None:
    cfg = _config(strike_drift_pct=10)
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    plan = quoting.plan(cfg, _market(spot=1.60), [_contract(wanted[0], store.MINTING, 1)], NOW)
    assert plan.retire == ()


def test_sold_calls_and_puts_lean_the_book_opposite_ways() -> None:
    cfg = _config()
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    call = _contract(next(i for i in wanted if i.side == "call"), store.SOLD, 1)
    put = _contract(next(i for i in wanted if i.side == "put"), store.SOLD, 2)

    short_call = quoting.plan(cfg, _market(), [call], NOW).net_delta_xch
    short_put = quoting.plan(cfg, _market(), [put], NOW).net_delta_xch

    assert -20 < short_call < 0  # upside given away
    assert 0 < short_put < 20  # downside taken on
    both = quoting.plan(cfg, _market(), [call, put], NOW).net_delta_xch
    assert both == pytest.approx(short_call + short_put)


def test_steering_cheapens_the_side_that_rebalances() -> None:
    cfg = _config(pricing={"steer_strength": 0.5, "steer_scale_xch": 10, "markup_fraction": 0.2})
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    call = _contract(next(i for i in wanted if i.side == "call"), store.OPEN, 1)
    put = _contract(next(i for i in wanted if i.side == "put"), store.OPEN, 2)
    sold_puts = [
        _contract(next(i for i in wanted if i.side == "put"), store.SOLD, tag) for tag in (3, 4, 5)
    ]

    flat = quoting.plan(cfg, _market(), [call, put], NOW).asks
    long_book = quoting.plan(cfg, _market(), [call, put, *sold_puts], NOW).asks

    # Long after selling puts: calls shed exposure, so they get cheaper; more puts dearer.
    assert long_book[call.launcher_id] < flat[call.launcher_id]
    assert long_book[put.launcher_id] > flat[put.launcher_id]


def test_steering_never_goes_below_model_plus_carry() -> None:
    cfg = _config(pricing={"steer_strength": 0.9, "steer_scale_xch": 1, "markup_fraction": 0.5})
    plain = _config(pricing={"markup_fraction": 0.0, "steer_strength": 0.0})
    wanted = quoting.plan(cfg, _market(), [], NOW).mint
    call = _contract(next(i for i in wanted if i.side == "call"), store.OPEN, 1)
    sold_puts = [
        _contract(next(i for i in wanted if i.side == "put"), store.SOLD, tag)
        for tag in range(2, 12)
    ]

    steered = quoting.plan(cfg, _market(), [call, *sold_puts], NOW).asks[call.launcher_id]
    floor = quoting.plan(plain, _market(), [call], NOW).asks[call.launcher_id]

    assert steered >= floor


def test_steering_never_discounts_the_minimum_premium() -> None:
    # A far out-of-the-money call a few hours from expiry: the model value and
    # carry are next to nothing, so the whole ask is the minimum premium.
    cfg = _config(
        call_strikes_pct=[300],
        stop_before_expiry_seconds=60,
        pricing={"min_premium": 0.01, "steer_strength": 0.25},
    )
    soon = NEAR - 6 * 3600
    wanted = quoting.plan(cfg, _market(), [], soon).mint
    call = _contract(next(i for i in wanted if i.side == "call"), store.OPEN, 1)
    floor = round(0.01 * 20 * 1000)  # per XCH, for 20 XCH, in quote mojos

    flat = quoting.ask_mojos(call, _market(), cfg, soon, 0.0)
    cheaper = quoting.ask_mojos(call, _market(), cfg, soon, 1.0)  # long book: calls discounted
    dearer = quoting.ask_mojos(call, _market(), cfg, soon, -1.0)

    assert flat == floor
    assert cheaper == floor  # was 0.0075 per XCH before the floor was put back
    assert dearer > floor


def test_ask_scales_with_size_and_rounds_up() -> None:
    cfg = _config()
    wanted = quoting.plan(cfg, _market(), [], NOW).mint[0]
    contract = _contract(wanted, store.OPEN, 1)
    ask = quoting.ask_mojos(contract, _market(), cfg, NOW, 0.0)
    assert quoting.contract_size_xch(contract) == 20
    assert quoting.contract_strike(contract, 1000) == pytest.approx(1.10)
    assert isinstance(ask, int) and ask > 20 * 1000 * 0.03  # well above a few cents per XCH


def test_a_contract_too_small_to_strike_blocks_minting_instead_of_failing() -> None:
    # 0.0001 XCH at a strike near 1.10 is 0.11 thousandths of the quote asset:
    # no whole number of its mojos. That used to raise from inside the plan and
    # take the whole step with it, offers and all.
    cfg = _config(contract_xch=0.0001)

    plan = quoting.plan(cfg, _market(), [], NOW)

    assert plan.mint == () and "contract_xch is too small" in plan.blocked

    # A contract already in such a slot, written when the size was larger, is
    # not retired for it, and still gets its ask.
    was = quoting.plan(_config(), _market(), [], NOW).mint[0]
    plan = quoting.plan(cfg, _market(), [_contract(was, store.OPEN, 1)], NOW)
    assert plan.retire == () and list(plan.asks) == [bytes([1]) * 32]
