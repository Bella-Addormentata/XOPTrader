"""The same flows, signed by the reference wallet's own code.

``test_reference_wallet.py`` uses a fake wallet, which proves our client's logic
but not that the fake behaves like the real thing. This module closes that gap
without a wallet daemon: it calls chia-blockchain's actual RPC handlers
(``WalletRpcApi.gather_signing_info`` and friends, wrapped in the same
``marshal`` the RPC server applies, so the JSON goes through the real request
and response classes) on top of the real ``WalletSigner``, with keys derived the
way the wallet derives them.

What is real: request/response parsing, target extraction, key-hint lookup,
hardened and unhardened derivation, synthetic-key signing, ``push_tx`` parsing.
What is stubbed: the wallet's database (a dict of derivation records), its
keychain (one in-memory root key) and its full-node peer (the SDK simulator).

SKIPPED IN CI. chia-blockchain is not a dependency of XOPTrader and is not in
the lock files, so this runs only where it happens to be installed. Last run
by hand against chia-blockchain 2.7.5.
"""

from __future__ import annotations

import asyncio
from types import SimpleNamespace
from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")
pytest.importorskip("chia.wallet.wallet_signer")

from chia.rpc.util import marshal  # noqa: E402
from chia.util.bech32m import decode_puzzle_hash  # noqa: E402
from chia.wallet.derivation_record import DerivationRecord  # noqa: E402
from chia.wallet.derive_keys import _derive_path, _derive_path_unhardened  # noqa: E402
from chia.wallet.puzzles.p2_delegated_puzzle_or_hidden_puzzle import (  # noqa: E402
    DEFAULT_HIDDEN_PUZZLE_HASH,
    calculate_synthetic_secret_key,
    puzzle_hash_for_pk,
)
from chia.wallet.util.wallet_types import WalletType  # noqa: E402
from chia.wallet.wallet_rpc_api import WalletRpcApi  # noqa: E402
from chia.wallet.wallet_signer import WalletSigner  # noqa: E402
from chia_rs import AugSchemeMPL  # noqa: E402
from chia_rs.sized_bytes import bytes32  # noqa: E402
from chia_rs.sized_ints import uint32  # noqa: E402

from gui.services.options import builder, signing  # noqa: E402
from gui.services.options.chain import OptionStatus  # noqa: E402
from gui.services.options.reference_wallet import ReferenceWallet  # noqa: E402
from gui.services.options.tests.sim_world import (  # noqa: E402
    ADDITIONAL_DATA,
    CAT,
    XCH,
    World,
)

_WALLET_PATH = [12381, 8444, 2]


class _PuzzleStore:
    """The three lookups ``WalletSigner`` makes, over an in-memory address list."""

    def __init__(self, records: list[DerivationRecord]) -> None:
        self._by_hash = {bytes(record.puzzle_hash): record for record in records}
        self._by_key = {bytes(record.pubkey): record for record in records}

    async def record_for_puzzle_hash(self, puzzle_hash: bytes32) -> Any:
        return self._by_hash.get(bytes(puzzle_hash))

    async def index_for_puzzle_hash(self, puzzle_hash: bytes32) -> Any:
        record = self._by_hash.get(bytes(puzzle_hash))
        return None if record is None else record.index

    async def index_for_pubkey(self, pubkey: Any) -> Any:
        record = self._by_key.get(bytes(pubkey))
        return None if record is None else record.index


class RealWallet:
    """chia-blockchain's wallet RPC handlers over a stubbed database and node."""

    def __init__(self, world: World, *, addresses: int = 4) -> None:
        self.world = world
        self.root = AugSchemeMPL.key_gen(bytes([7]) * 32)
        self.calls: list[str] = []
        self._child_keys: dict[bytes, Any] = {}
        self.unhardened: list[bytes] = []
        self.hardened: list[bytes] = []
        records = []
        for index in range(addresses):
            for hardened, derive, bucket in (
                (False, _derive_path_unhardened, self.unhardened),
                (True, _derive_path, self.hardened),
            ):
                child = derive(self.root, [*_WALLET_PATH, index])
                puzzle_hash = puzzle_hash_for_pk(child.get_g1())
                records.append(
                    DerivationRecord(
                        uint32(index),
                        puzzle_hash,
                        child.get_g1(),
                        WalletType.STANDARD_WALLET,
                        uint32(1),
                        hardened,
                    )
                )
                self._child_keys[bytes(puzzle_hash)] = child
                bucket.append(bytes(puzzle_hash))

        signer = WalletSigner(
            root_pubkey=self.root.get_g1(),
            root_private_key=self.root,
            puzzle_store=_PuzzleStore(records),  # type: ignore[arg-type]
            max_block_cost_clvm=11_000_000_000,
            agg_sig_me_additional_data=bytes32(ADDITIONAL_DATA),
            spend_bundle_push=self._never,
        )
        manager = SimpleNamespace(
            signer=signer,
            get_private_key=self._private_key,
            main_wallet=SimpleNamespace(
                convert_secret_key_to_synthetic=lambda key: calculate_synthetic_secret_key(
                    key, DEFAULT_HIDDEN_PUZZLE_HASH
                )
            ),
        )
        self._api = SimpleNamespace(
            service=SimpleNamespace(
                wallet_state_manager=manager,
                server=SimpleNamespace(get_connections=lambda node_type: ["peer"]),
                push_tx=self._push,
            )
        )

    async def _never(self, bundle: Any) -> None:  # pragma: no cover - not on our path
        raise AssertionError("submit_transactions is not used")

    async def _private_key(self, puzzle_hash: bytes32) -> Any:
        try:
            return self._child_keys[bytes(puzzle_hash)]
        except KeyError:
            raise ValueError(f"Puzzle hash {puzzle_hash.hex()} is not in the wallet") from None

    async def _push(self, bundle: Any) -> None:
        self.world.push_bundle(sdk.SpendBundle.from_bytes(bytes(bundle)))

    def __call__(self, endpoint: str, body: dict) -> dict:
        """One RPC call, as the wallet's HTTP layer would answer it."""
        self.calls.append(endpoint)
        handler = marshal(getattr(WalletRpcApi, endpoint))
        try:
            response = asyncio.run(handler(self._api, body))
        except Exception as exc:  # the RPC server reports handler errors this way
            return {"success": False, "error": str(exc)}
        return {**response, "success": True}

    def fund(self, puzzle_hash: bytes, xch: int) -> None:
        self.world.sim.new_coin(puzzle_hash, xch * XCH)

    def coins(self, puzzle_hash: bytes) -> list[Any]:
        return list(self.world.sim.unspent_coins(puzzle_hash, False))

    def balance(self, puzzle_hash: bytes) -> int:
        return sum(int(coin.amount) for coin in self.coins(puzzle_hash))


def _setup():
    world = World()
    wallet = RealWallet(world)
    reference = ReferenceWallet(wallet, agg_sig_me_additional_data=ADDITIONAL_DATA)
    return world, wallet, reference


def _custody(reference: ReferenceWallet, puzzle_hash: bytes) -> builder.Custody:
    custody = builder.Custody()
    assert custody.add_key(reference.synthetic_key(puzzle_hash)) == puzzle_hash
    return custody


def test_real_wallet_reveals_the_key_of_its_own_address() -> None:
    _, wallet, reference = _setup()
    for address in (wallet.unhardened[0], wallet.hardened[1]):
        key = reference.synthetic_key(address)
        assert bytes(sdk.standard_puzzle_hash(key)) == address
    # Chia's address encoding and the SDK's agree.
    encoded = sdk.Address(wallet.unhardened[0], "xch").encode()
    assert bytes(decode_puzzle_hash(encoded)) == wallet.unhardened[0]


def test_real_wallet_refuses_a_foreign_address() -> None:
    world, _, reference = _setup()
    stranger = world.party(1)
    with pytest.raises(signing.SigningError, match="not in the wallet"):
        reference.synthetic_key(stranger.puzzle_hash)


@pytest.mark.parametrize("kind", ["unhardened", "hardened"])
def test_real_signer_carries_a_call_from_mint_to_clawback(kind: str) -> None:
    """Mint, offer, take, expiry, clawback: every maker signature from WalletSigner."""
    world, wallet, reference = _setup()
    address = getattr(wallet, kind)[0]
    wallet.fund(address, 150)
    taker = world.party(10)
    usd = world.issue_cat(taker, 1000)
    custody = _custody(reference, address)

    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH), builder.Asset(usd, 120 * CAT), world.now() + 3600, address
    )
    funding = builder.Funding(change_puzzle_hash=address, xch=wallet.coins(address))
    minted = builder.build_mint(terms, funding, custody, fee=1000)
    reference.push(minted.coin_spends, reference.sign(minted.coin_spends))
    assert wallet.balance(address) == 50 * XCH - 1000 - builder.OPTION_COIN_AMOUNT

    loaded = world.load(minted.launcher_id)
    assert loaded.status is OptionStatus.LIVE and loaded.terms == terms
    draft = builder.build_offer(
        loaded.option,
        builder.Asset(usd, 2100),
        address,
        custody,
        expires_at_seconds=world.now() + 900,
    )
    world.take(taker, draft.encode(reference.sign(draft.coin_spends)))
    assert world.load(minted.launcher_id).owner_puzzle_hash == taker.puzzle_hash

    world.advance(3600)
    expired = world.load(minted.launcher_id)
    clawback = builder.build_clawback(
        expired.underlying,
        expired.locked,
        reference.synthetic_key(address),
        builder.Funding(change_puzzle_hash=address),
        custody,
        fee=1000,
    )
    reference.push(clawback, reference.sign(clawback))

    assert wallet.balance(address) == 150 * XCH - 2000 - builder.OPTION_COIN_AMOUNT
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED
    assert set(wallet.calls) == {
        "sign_message_by_address",
        "gather_signing_info",
        "execute_signing_instructions",
        "push_tx",
    }


def test_real_signer_cancels_an_offer_and_unwinds_by_self_exercise() -> None:
    world, wallet, reference = _setup()
    address = wallet.unhardened[2]
    wallet.fund(address, 150)
    taker = world.party(10)
    custody = _custody(reference, address)

    # XCH on both legs keeps this to wallet-held coins only.
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH), builder.Asset(None, 30 * XCH), world.now() + 3600, address
    )
    minted = builder.build_mint(
        terms, builder.Funding(change_puzzle_hash=address, xch=wallet.coins(address)), custody
    )
    reference.push(minted.coin_spends, reference.sign(minted.coin_spends))
    draft = builder.build_offer(minted.option, builder.Asset(None, 2 * XCH), address, custody)
    offer = draft.encode(reference.sign(draft.coin_spends))

    moved = builder.build_transfer(
        minted.option,
        address,
        builder.Funding(change_puzzle_hash=address, xch=wallet.coins(address)),
        custody,
        fee=500,
    )
    reference.push(moved.coin_spends, reference.sign(moved.coin_spends))
    with pytest.raises(AssertionError, match="DoubleSpend"):
        world.take(taker, offer)

    loaded = world.load(minted.launcher_id)
    unwind = builder.build_exercise(
        loaded.option,
        loaded.underlying,
        loaded.locked,
        builder.Funding(change_puzzle_hash=address, xch=wallet.coins(address)),
        custody,
        fee=500,
    )
    reference.push(unwind, reference.sign(unwind))

    assert wallet.balance(address) == 150 * XCH - 1000
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED


def test_real_signer_cancels_a_call_without_the_strike() -> None:
    """The hand-built strike loop of ``build_cancel`` signs like everything else."""
    from gui.services.options import chain

    world, wallet, reference = _setup()
    address = wallet.unhardened[1]
    wallet.fund(address, 150)
    rich = world.party(10)
    usd = world.issue_cat(rich, 1000)
    world.send_cat(rich, address, usd, 1)
    custody = _custody(reference, address)

    def funding() -> builder.Funding:
        cats = [
            chain.load_cat(world.sim, coin.coin_id())
            for coin in world.sim.unspent_coins(sdk.cat_puzzle_hash(usd, address), False)
        ]
        return builder.Funding(change_puzzle_hash=address, xch=wallet.coins(address), cats=cats)

    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH), builder.Asset(usd, 120 * CAT), world.now() + 3600, address
    )
    minted = builder.build_mint(terms, funding(), custody)
    reference.push(minted.coin_spends, reference.sign(minted.coin_spends))

    cancel = builder.build_cancel(
        minted.option, minted.underlying, minted.locked, funding(), custody, fee=1000
    )
    reference.push(cancel, reference.sign(cancel))

    assert wallet.balance(address) == 150 * XCH - 1000
    assert sum(int(cat.coin.amount) for cat in funding().cats) == 1
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED


def test_real_signer_will_not_sign_for_coins_it_does_not_own() -> None:
    world, _, reference = _setup()
    stranger = world.party(20)
    terms = builder.OptionTerms(
        builder.Asset(None, 5 * XCH),
        builder.Asset(None, 6 * XCH),
        world.now() + 3600,
        stranger.puzzle_hash,
    )
    spends = builder.build_mint(terms, stranger.funding(), stranger.custody()).coin_spends
    with pytest.raises(signing.SigningError, match="execute_signing_instructions"):
        reference.sign(spends)
