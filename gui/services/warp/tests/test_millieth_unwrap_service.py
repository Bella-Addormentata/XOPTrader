"""[MILLIETH-TOPUP review] A real WarpEngine wmilliETH.b unwrap, end to end.

The top-up tests substitute the engine's request_unwrap, and the unwrap
service tests use a wUSDC.b-only wallet, so neither can catch the asset
plumbing in between. These drive ``request_unwrap(asset="milliETH")``
through the gates and the ``cat_spend`` against a wallet holding BOTH CATs:
the right CAT wallet, the right liquidity token, the milliETH amount
columns, the asset's own cap, and its own offer-locked balance.
"""

from __future__ import annotations

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from gui.services.warp import drivers  # noqa: E402
from gui.services.warp import service as S  # noqa: E402
from gui.services.warp.jobs import JobStatus  # noqa: E402

from .test_warp_service import build, default_params, new_store  # noqa: E402
from .test_warp_unwrap_service import (  # noqa: E402
    NET,
    RECEIVER,
    UnwrapFakeEvm,
    UnwrapFakeWallet,
)

USDC_WID, MILLI_WID = 3, 4
MILLI = NET.asset("milliETH")


class TwoCatWallet(UnwrapFakeWallet):
    """wUSDC.b in wallet 3 and wmilliETH.b in wallet 4."""

    def __init__(self) -> None:
        super().__init__()
        self.balances[MILLI_WID] = {"confirmed_wallet_balance": 20_000,
                                    "spendable_balance": 20_000}

    def get_wallets(self, *, wallet_type=None):
        return [{"id": USDC_WID, "type": 6, "name": "wUSDC.b"},
                {"id": MILLI_WID, "type": 6, "name": "wmilliETH.b"}]

    def cat_get_asset_id(self, wallet_id):
        return "0x" + (NET.expected_asset_id if int(wallet_id) == USDC_WID
                       else MILLI.expected_asset_id)

    def get_transactions(self, wallet_id=1, *, start=0, end=50, reverse=True,
                         confirmed=None):
        if int(wallet_id) == MILLI_WID:
            return []
        return super().get_transactions(wallet_id, start=start, end=end,
                                        reverse=reverse, confirmed=confirmed)


class RecordingEvm(UnwrapFakeEvm):
    def __init__(self) -> None:
        super().__init__()
        self.erc20_queries: list = []

    def get_erc20_balance(self, token, owner):
        self.erc20_queries.append((str(token).lower(), str(owner).lower()))
        return super().get_erc20_balance(token, owner)


def _build(**over):
    over.setdefault("max_unwrap_micros", 100_000_000)          # $100
    over.setdefault("max_unwrap_millieth_mojos", 10_000)       # 10 wmilliETH.b
    store = new_store()
    evm, wallet = RecordingEvm(), TwoCatWallet()
    engine, _ctx = build(store, params=default_params(**over),
                         evm=evm, wallet=wallet)
    return store, engine, evm, wallet


def test_a_millieth_unwrap_burns_from_its_own_wallet_against_its_own_token():
    store, engine, evm, wallet = _build()
    engine.request_unwrap(5_000, RECEIVER, asset="milliETH")
    job = store.get_active_job()
    assert job.state["asset"] == "milliETH"
    # 3 ERC-20 decimals = 3 CAT decimals: one base unit per mojo, not the
    # x1000 a USDC job records.
    assert (job.amount_mojos, job.amount_usdc_micros) == (5_000, 5_000)

    assert engine.step()["status"] == JobStatus.BURN_SENT
    job = store.get_active_job()
    assert job.state["cat_wallet_id"] == MILLI_WID
    assert (MILLI.erc20_address.lower(),
            NET.erc20_bridge_address.lower()) in evm.erc20_queries, \
        "liquidity is the milliETH token's, not USDC's"
    assert job.state["burn_inner_ph"] == drivers.expected_burn_inner_puzzle_hash(
        NET, bytes.fromhex(RECEIVER[2:]), asset=MILLI).hex()

    engine.step()                                             # the cat_spend
    assert [(w, a) for w, a, _addr, _fee in wallet.cat_spends] == [
        (MILLI_WID, 5_000)]
    assert wallet.cat_spends[0][2] == store.get_active_job().state["burn_address"]


def test_each_asset_has_its_own_cap():
    # No milliETH cap stated: refused, whatever the USDC cap says.
    store, engine, _evm, _w = _build(max_unwrap_millieth_mojos=-1)
    with pytest.raises(S.WarpError, match="max_unwrap_millieth is not set"):
        engine.request_unwrap(5_000, RECEIVER, asset="milliETH")
    assert store.get_active_job() is None
    # Its own cap, in its own units: 2,000 mojos refuses 5,000.
    store, engine, _evm, _w = _build(max_unwrap_millieth_mojos=2_000)
    with pytest.raises(S.WarpError, match="refusing rather than clamping"):
        engine.request_unwrap(5_000, RECEIVER, asset="milliETH")
    # And the USDC cap does not bind it: 150,000 mojos (150 wmilliETH.b) is
    # fine with milliETH unlimited although $100 caps USDC.
    store, engine, _evm, _w = _build(max_unwrap_millieth_mojos=0)
    engine.request_unwrap(150_000, RECEIVER, asset="milliETH")
    assert store.get_active_job().amount_mojos == 150_000


def test_offer_locked_millieth_pends_even_with_usdc_spendable():
    store, engine, _evm, wallet = _build()
    wallet.balances[MILLI_WID] = {"confirmed_wallet_balance": 20_000,
                                  "spendable_balance": 4_999}
    engine.request_unwrap(5_000, RECEIVER, asset="milliETH")
    out = engine.step()
    assert out["status"] == JobStatus.UNWRAP_CHECKS
    assert out["retry_count"] == 0, "a healthy pend, not an error"
    assert wallet.cat_spends == []
    wallet.balances[MILLI_WID]["spendable_balance"] = 20_000  # offers closed
    assert engine.step()["status"] == JobStatus.BURN_SENT
