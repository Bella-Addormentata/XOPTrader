"""What a bundle costs, and the fee offered for it."""

from __future__ import annotations

from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, fees  # noqa: E402
from gui.services.options.tests.sim_world import CAT, XCH, Party, World  # noqa: E402

DAY = 86400


def _bundles() -> dict[str, list[Any]]:
    """One bundle of every kind the book sends, over XCH and CAT collateral."""
    world = World()
    maker, taker = world.party(100), world.party(100)
    usd = world.issue_cat(taker, 5000)
    world.send_cat(taker, maker.puzzle_hash, usd, 1000 * CAT)
    expiry = world.now() + DAY

    def custody(party: Party) -> builder.Custody:
        return party.custody()

    call = builder.build_mint(
        builder.OptionTerms(
            builder.Asset(None, 20 * XCH), builder.Asset(usd, 24 * CAT), expiry, maker.puzzle_hash
        ),
        maker.funding(),
        custody(maker),
    )
    bundles = {"mint, XCH call": call.coin_spends}
    world.push(call.coin_spends)
    put = builder.build_mint(
        builder.OptionTerms(
            builder.Asset(usd, 20 * CAT), builder.Asset(None, 20 * XCH), expiry, maker.puzzle_hash
        ),
        maker.funding(usd),
        custody(maker),
    )
    bundles["mint, CAT put"] = put.coin_spends
    world.push(put.coin_spends)
    bundles["withdrawal"] = builder.build_transfer(
        call.option, maker.puzzle_hash, builder.Funding(maker.puzzle_hash), custody(maker)
    ).coin_spends
    bundles["cancel, XCH call"] = builder.build_cancel(
        call.option, call.underlying, call.locked, maker.funding(usd), custody(maker)
    )
    bundles["cancel, CAT put"] = builder.build_cancel(
        put.option, put.underlying, put.locked, maker.funding(), custody(maker)
    )
    world.advance(2 * DAY)
    for name, minted in (("clawback, XCH call", call), ("clawback, CAT put", put)):
        bundles[name] = builder.build_clawback(
            minted.underlying,
            minted.locked,
            maker.pk,
            builder.Funding(change_puzzle_hash=maker.puzzle_hash),
            custody(maker),
        )
    return bundles


# What chia-blockchain 2.7.5 charges each of them (get_conditions_from_spendbundle
# with mainnet constants). The sizes do not depend on the keys, so they hold
# for any run.
EXACT = {
    "mint, XCH call": 53_365_217,
    "mint, CAT put": 86_453_988,
    "withdrawal": 34_147_420,
    "cancel, XCH call": 165_153_492,
    "cancel, CAT put": 139_914_031,
    "clawback, XCH call": 13_165_371,
    "clawback, CAT put": 38_473_913,
}


def test_the_cost_estimate_is_close_to_what_a_node_charges_and_never_under() -> None:
    bundles = _bundles()
    assert set(bundles) == set(EXACT)
    for name, coin_spends in bundles.items():
        cost = fees.bundle_cost(coin_spends)
        # Slightly high is the safe side: the fee offered is never short.
        assert EXACT[name] <= cost <= EXACT[name] * 1.03, (name, cost)


def test_the_recorded_costs_are_chia_blockchain_s_own() -> None:
    chia_rs = pytest.importorskip("chia_rs")
    constants = pytest.importorskip("chia.consensus.default_constants").DEFAULT_CONSTANTS
    for name, coin_spends in _bundles().items():
        bundle = sdk.SpendBundle(coin_spends, sdk.Signature.infinity())
        conditions = chia_rs.get_conditions_from_spendbundle(
            chia_rs.SpendBundle.from_bytes(bytes(bundle.to_bytes())),
            11_000_000_000,
            constants,
            8_000_000,
        )
        assert int(conditions.cost) == EXACT[name], name


def test_a_fee_is_cost_times_rate_rounded_up_and_capped() -> None:
    assert fees.fee_for(53_365_217, 1, 10**9) == 53_365_217
    assert fees.fee_for(53_365_217, 5, 10**9) == 266_826_085
    assert fees.fee_for(53_365_217, 5, 100_000_000) == 100_000_000  # the cap binds
    assert fees.fee_for(10, 0.25, 10**9) == 3  # rounded up, never short
    # No rate, no cost or no allowance: no fee.
    assert fees.fee_for(53_365_217, 0, 10**9) == 0
    assert fees.fee_for(0, 5, 10**9) == 0
    assert fees.fee_for(53_365_217, 5, 0) == 0


def test_a_bundle_that_does_not_run_cannot_be_priced() -> None:
    coin = sdk.Coin(b"\x01" * 32, b"\x02" * 32, 5)
    broken = sdk.CoinSpend(coin, b"\xff\xff", b"\x80")
    with pytest.raises(fees.FeeError, match="did not run"):
        fees.bundle_cost([broken])
