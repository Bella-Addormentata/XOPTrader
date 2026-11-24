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


def test_an_ask_on_the_floor_is_not_pushed_a_whole_unit_up_by_float_error() -> None:
    # Seen live on 2026-11-15: a 0.5 XCH call at 1.40 with spot 1.15, two days
    # out, sat on min_premium 0.01. The sums came to 0.010000000000000002 per
    # XCH, 5.000000000000001 mojos, and rounding up listed it at 6 (0.006
    # wUSDC.b) instead of 5, flipping between the two and re-quoting each time.
    assert 0.010000000000000002 * 0.5 * 1000 > 5  # the float error itself
    assert quoting.quote_units(0.010000000000000002, 0.5, 1000) == 5
    # A real fraction of a mojo is still rounded up, never down.
    assert quoting.quote_units(0.0100001, 0.5, 1000) == 6
    assert quoting.quote_units(0.0000001, 0.5, 1000) == 1  # never free


def test_the_floor_ask_stays_put_as_the_model_drifts() -> None:
    from gui.services.options import pricing

    expiry, start = 1794924000, 1794776500
    asks = set()
    for now in range(start, start + 3600, 60):
        quote = pricing.ask_price(
            1.15,
            1.40,
            expiry - now,
            call=True,
            sigma=0.99,
            sigma_markup=0.02,
            lock_rate=0.10,
            markup_fraction=0.03,
            min_premium=0.01,
        )
        per_xch = max(quote.model + quote.carry, 0.01, quote.model + quote.carry + quote.markup)
        asks.add(quoting.quote_units(per_xch, 0.5, 1000))
    assert asks == {5}


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


def test_a_small_contract_s_target_is_the_strike_it_will_really_have() -> None:
    # 0.001 XCH at 0.70 is 0.7 thousandths of the quote asset, which rounds to
    # one: a strike of 1.00. A target that kept 0.70 would see its own contract
    # as 43% adrift, retire it and mint it again, without end.
    cfg = _config(contract_xch=0.001, call_strikes_pct=[100], put_strikes_pct=[], expiries=[FAR])
    view = _market(0.70)

    (wanted,) = quoting.plan(cfg, view, [], NOW).mint
    assert wanted.strike_asset.amount == 1 and wanted.strike == pytest.approx(1.0)

    minted = _contract(wanted, store.OPEN, 1)
    plan = quoting.plan(cfg, view, [minted], NOW)
    assert plan.retire == () and plan.mint == ()
    assert list(plan.asks) == [minted.launcher_id]


# ---------------------------------------------------------------------------
# [LADDER] contract_xch: auto -- 1, 10, 100, ... XCH side by side
# ---------------------------------------------------------------------------


def _ladder(**limits) -> config.OptionsConfig:
    """Two call strikes on two dates, no puts, sized by the ladder."""
    caps = {
        "max_locked_xch": 14,
        "max_locked_quote": 1,
        "max_contracts": 8,
        "max_contracts_per_expiry": 4,
        **limits,
    }
    return _config(contract_xch="auto", put_strikes_pct=[], limits=caps)


def _sizes(plan: quoting.Plan) -> list[tuple[int, int, float]]:
    return [(item.size_mojos // XCH, item.expiration, item.strike) for item in plan.mint]


def test_the_ladder_fills_the_smallest_size_everywhere_first() -> None:
    plan = quoting.plan(_ladder(), _market(), [], NOW)

    # The whole 1 XCH grid, nearest date and nearest the money first, then
    # the 10 XCH call that still fits in 14 XCH.
    assert _sizes(plan) == [
        (1, NEAR, 1.10),
        (1, NEAR, 1.20),
        (1, FAR, 1.10),
        (1, FAR, 1.20),
        (10, NEAR, 1.10),
    ]
    assert plan.blocked == "", "the cap stopping a larger size is the ladder working"
    assert plan.mint[-1].underlying == quoting.Asset(None, 10 * XCH)
    assert plan.mint[-1].strike_asset.amount == 11_000, "10 XCH at 1.10, in mojos"


@pytest.mark.parametrize(
    ("limits", "puts"),
    [
        ({"max_locked_xch": 14, "max_contracts": 8, "max_contracts_per_expiry": 4}, []),
        ({"max_locked_xch": 444, "max_contracts": 12, "max_contracts_per_expiry": 6}, []),
        (
            {
                "max_locked_xch": 1000,
                "max_contracts": "unlimited",
                "max_contracts_per_expiry": "unlimited",
            },
            [],
        ),
        (
            {
                "max_locked_xch": 44,
                "max_locked_quote": 10_000,
                "max_contracts": 16,
                "max_contracts_per_expiry": 8,
            },
            [90, 80],
        ),
        (
            {
                "max_locked_xch": 144,
                "max_locked_quote": "unlimited",
                "max_contracts": 16,
                "max_contracts_per_expiry": 8,
            },
            [90, 80],
        ),
    ],
)
def test_the_settings_preview_matches_what_the_planner_mints(limits, puts) -> None:
    """[review #261] config.call_ladder is what Settings and `status` show: on an empty
    book it must come to the calls the planner mints, size by size."""
    cfg = _config(
        contract_xch="auto", put_strikes_pct=puts, limits={"max_locked_quote": 1, **limits}
    )
    minted: dict[int, int] = {}
    for item in quoting.plan(cfg, _market(), [], NOW).mint:
        if item.side == "call":
            minted[item.size_mojos // XCH] = minted.get(item.size_mojos // XCH, 0) + 1
    preview = config.call_ladder(cfg.limits, 2, 2, put_strikes=len(puts))
    assert dict(preview) == minted


def test_one_slot_holds_a_contract_of_each_size() -> None:
    cfg = _ladder(max_locked_xch=44)
    small, *_ = quoting.plan(cfg, _market(), [], NOW).mint
    held = [_contract(small, store.OPEN, 1)]

    plan = quoting.plan(cfg, _market(), held, NOW)

    assert small.slot in {item.slot for item in plan.mint if item.size_mojos == 10 * XCH}
    assert small.slot not in {item.slot for item in plan.mint if item.size_mojos == XCH}
    assert list(plan.asks) == [held[0].launcher_id] and plan.retire == ()


def test_a_limit_that_stops_the_smallest_size_is_said() -> None:
    """[LADDER] Max XCH locked used to stop mints without a word."""
    short = quoting.plan(_ladder(max_locked_xch=2), _market(), [], NOW)
    assert _sizes(short) == [(1, NEAR, 1.10), (1, NEAR, 1.20)]
    assert short.blocked == "options.limits.max_locked_xch reached"

    none = quoting.plan(_ladder(max_locked_xch=0.5), _market(), [], NOW)
    assert none.mint == () and none.blocked == "options.limits.max_locked_xch reached"

    crowded = quoting.plan(_ladder(max_contracts_per_expiry=1), _market(), [], NOW)
    assert _sizes(crowded) == [(1, NEAR, 1.10), (1, FAR, 1.10)]
    assert crowded.blocked == "options.limits.max_contracts_per_expiry reached"

    few = quoting.plan(_ladder(max_contracts=3), _market(), [], NOW)
    assert len(few.mint) == 3 and few.blocked == "options.limits.max_contracts reached"

    puts = _config(contract_xch="auto", limits={"max_locked_quote": 0.5})
    assert quoting.plan(puts, _market(), [], NOW).blocked == (
        "options.limits.max_locked_quote reached"
    )


def test_a_sold_small_contract_is_refilled_first_when_there_is_room() -> None:
    """A sold contract frees its slot but its collateral stays locked until expiry."""
    counts = {"max_contracts": 12, "max_contracts_per_expiry": 6}
    cfg = _ladder(max_locked_xch=44, **counts)
    grid = quoting.plan(cfg, _market(), [], NOW).mint
    assert [item.size_mojos // XCH for item in grid] == [1] * 4 + [10] * 4
    held = [
        _contract(item, store.SOLD if index == 0 else store.OPEN, index + 1)
        for index, item in enumerate(grid)
    ]

    full = quoting.plan(cfg, _market(), held, NOW)
    assert full.mint == (), "44 XCH are locked: the refill waits for room"
    assert full.blocked == "options.limits.max_locked_xch reached"
    assert full.retire == (), "a larger contract is never cancelled to make room"

    room = quoting.plan(_ladder(max_locked_xch=45, **counts), _market(), held, NOW)
    assert _sizes(room) == [(1, NEAR, 1.10)], "the 1 XCH refill, the same slot"
    assert room.mint[0].slot == grid[0].slot


def test_a_size_off_the_ladder_is_retired_and_ladder_sizes_are_kept() -> None:
    """[LADDER] The 0.5 XCH calls of the live test: cancelled once auto is on."""
    old = quoting.plan(_config(contract_xch=0.5, put_strikes_pct=[]), _market(), [], NOW).mint
    cfg = _ladder(max_locked_xch=44)
    new = quoting.plan(cfg, _market(), [], NOW).mint
    held = [
        _contract(old[0], store.OPEN, 1),
        _contract(old[1], store.SOLD, 2),
        _contract(new[0], store.OPEN, 3),
        _contract(new[-1], store.OPEN, 4),  # 10 XCH
    ]

    plan = quoting.plan(cfg, _market(), held, NOW)

    assert plan.retire == (held[0].launcher_id,), "unsold and off the ladder"
    assert set(plan.asks) == {held[2].launcher_id, held[3].launcher_id}


def test_a_lower_cap_stops_new_mints_and_keeps_what_is_on_offer() -> None:
    big = quoting.plan(_ladder(max_locked_xch=44), _market(), [], NOW).mint[-1]
    assert big.size_mojos == 10 * XCH
    held = [_contract(big, store.OPEN, 1)]

    plan = quoting.plan(_ladder(max_locked_xch=4), _market(), held, NOW)

    assert plan.retire == () and list(plan.asks) == [held[0].launcher_id]
    assert plan.mint == () and plan.blocked == "options.limits.max_locked_xch reached"


def test_an_unlimited_cap_keeps_its_side_at_the_smallest_size() -> None:
    """[LADDER] With nothing to size from, the ladder would climb to its top at once."""
    plan = quoting.plan(_config(contract_xch="auto"), _market(), [], NOW)
    assert len(plan.mint) == 6 and {item.size_mojos for item in plan.mint} == {XCH}
    assert plan.blocked == ""


def test_puts_climb_the_ladder_on_their_own_cap() -> None:
    """A put locks the quote asset: max_locked_quote decides how far up puts go."""
    cfg = _config(
        contract_xch="auto",
        call_strikes_pct=[],
        put_strikes_pct=[90],
        limits={"max_locked_xch": 1, "max_locked_quote": 22},
    )
    plan = quoting.plan(cfg, _market(), [], NOW)
    # At spot 1.10 a 90% put strikes at 1.00: 1 quote unit per XCH.
    assert _sizes(plan) == [(1, NEAR, 1.00), (1, FAR, 1.00), (10, NEAR, 1.00), (10, FAR, 1.00)]
    assert plan.mint[-1].underlying == quoting.Asset(USD, 10_000)
    assert plan.blocked == ""


# ---------------------------------------------------------------------------
# The record kept with each offer
# ---------------------------------------------------------------------------


def _held(
    strike_pct: int, days: float, spot: float = 1.15, **pricing: float
) -> tuple[config.OptionsConfig, store.Contract]:
    """An unsold 0.5 XCH call of the grid, as the first live run held them."""
    expiry = NOW + int(days * DAY)
    cfg = _config(
        contract_xch=0.5,
        expiries=[expiry],
        call_strikes_pct=[strike_pct],
        put_strikes_pct=[],
        pricing=pricing,
    )
    (wanted,) = quoting.plan(cfg, _market(spot), [], NOW).mint
    return cfg, _contract(wanted, store.OPEN, 7)


def test_an_ask_on_the_floor_says_so_and_keeps_the_model_value_apart() -> None:
    # The first live run's C120: 0.5 XCH at 1.40, under two days out.
    cfg, contract = _held(120, 1.7)
    view = market.Market(spot=1.15, sigma=0.99, bid=1.14, ask=1.17, as_of=NOW)

    quote = quoting.ask_quote(contract, view, cfg, NOW, 0.0)

    assert quote.strike == pytest.approx(1.40)
    assert quote.set_by == "floor"
    assert quote.per_xch == pytest.approx(cfg.pricing.min_premium)
    # The model value is a small part of the ask. The markup carries it the
    # rest of the way to the minimum premium, and the record adds up to it.
    assert quote.model + quote.carry < cfg.pricing.min_premium / 10
    assert quote.model + quote.carry + quote.markup == pytest.approx(cfg.pricing.min_premium)
    assert quote.units == quoting.ask_mojos(contract, view, cfg, NOW, 0.0) == 5
    assert (quote.spot, quote.sigma, quote.floor) == (1.15, 0.99, cfg.pricing.min_premium)


def test_an_ask_above_the_floor_is_set_by_the_model() -> None:
    cfg, contract = _held(105, 14)
    view = market.Market(spot=1.15, sigma=0.99, bid=1.14, ask=1.17, as_of=NOW)

    quote = quoting.ask_quote(contract, view, cfg, NOW, 0.0)

    assert quote.set_by == "model"
    assert quote.per_xch == pytest.approx(quote.model + quote.carry + quote.markup)
    assert quote.per_xch > cfg.pricing.min_premium
    record = quote.as_record()
    assert record["set_by"] == "model" and record["units"] == quote.units


def test_steering_that_lifts_a_floor_ask_above_the_floor_is_the_model_s_doing() -> None:
    # A book leaning short makes calls dearer. Steering scales the top-up to
    # the minimum premium as well, so this ask ends up above the floor, and
    # the record still adds up to what was asked.
    cfg, contract = _held(120, 1.7, steer_strength=0.5)
    view = market.Market(spot=1.15, sigma=0.99, bid=1.14, ask=1.17, as_of=NOW)

    quote = quoting.ask_quote(contract, view, cfg, NOW, -1.0)

    assert quote.set_by == "model"
    assert quote.per_xch > cfg.pricing.min_premium
    assert quote.per_xch == pytest.approx(quote.model + quote.carry + quote.markup)
    assert quote.units == quoting.ask_mojos(contract, view, cfg, NOW, -1.0) > 5


def test_steering_that_lowers_a_model_ask_onto_the_floor_is_the_floor_s_doing() -> None:
    # A book leaning long makes calls cheaper, here down to the minimum premium.
    cfg, contract = _held(120, 7, markup_fraction=0.5, steer_strength=0.9)
    view = market.Market(spot=1.15, sigma=0.99, bid=1.14, ask=1.17, as_of=NOW)
    assert quoting.ask_quote(contract, view, cfg, NOW, 0.0).set_by == "model"

    quote = quoting.ask_quote(contract, view, cfg, NOW, 1.0)

    assert quote.set_by == "floor"
    assert quote.per_xch == pytest.approx(cfg.pricing.min_premium)
    assert quote.model + quote.carry + quote.markup < cfg.pricing.min_premium


def test_the_plan_carries_the_quote_behind_every_ask() -> None:
    cfg, contract = _held(105, 14)

    plan = quoting.plan(cfg, _market(1.15), [contract], NOW)

    assert set(plan.quotes) == set(plan.asks) == {contract.launcher_id}
    assert plan.quotes[contract.launcher_id].units == plan.asks[contract.launcher_id]


def test_rolling_expiries_build_the_grid_from_the_step_s_own_time() -> None:
    """[ROLLING] The grid follows the clock: no list of dates to keep up to date."""
    cfg = _config(expiries="weekly", put_strikes_pct=[])
    first = quoting.plan(cfg, _market(), [], NOW)
    later = quoting.plan(cfg, _market(), [], NOW + 14 * DAY)
    first_expiries = {item.expiration for item in first.mint}
    later_expiries = {item.expiration for item in later.mint}
    assert len(first_expiries) == 2 and len(later_expiries) == 2
    assert first_expiries.isdisjoint(later_expiries), "two weeks on, two new Fridays"
    assert all(expiry > NOW for expiry in first_expiries)
