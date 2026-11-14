"""A simulated chain with wallets, for the option tests.

Everything runs on ``chia_wallet_sdk.Simulator``: an in-memory chain that runs
each puzzle and checks conditions, timelocks and the aggregate signature. No
network, no node, no funds.

The parties here hold their own secret keys, which production code never does.
That is the point of the split in :mod:`gui.services.options`: the builder only
ever sees public keys, so the same spends can be signed here by
``LocalKeySigner`` and in production by the reference wallet.
"""

from __future__ import annotations

from typing import Any

import chia_wallet_sdk as sdk

from gui.services.options import builder, chain, signing

XCH = 10**12  # mojos per XCH
CAT = 10**3  # mojos per CAT unit (wUSDC.b has three decimals)

ADDITIONAL_DATA = signing.TESTNET11_AGG_SIG_ME_ADDITIONAL_DATA


class RejectedError(AssertionError):
    """The simulated chain refused a transaction."""


class Party:
    """One address on the simulated chain and the key that controls it."""

    def __init__(self, world: World, xch_mojos: int) -> None:
        pair = world.sim.bls(xch_mojos)
        self.world = world
        self.sk = pair.sk
        self.pk = pair.pk
        self.puzzle_hash = bytes(pair.puzzle_hash)

    def custody(self) -> builder.Custody:
        custody = builder.Custody()
        custody.add_key(self.pk)
        return custody

    def xch_coins(self) -> list[Any]:
        return list(self.world.sim.unspent_coins(self.puzzle_hash, False))

    def cats(self, asset_id: bytes) -> list[Any]:
        puzzle_hash = sdk.cat_puzzle_hash(asset_id, self.puzzle_hash)
        return [
            chain.load_cat(self.world.sim, coin.coin_id())
            for coin in self.world.sim.unspent_coins(puzzle_hash, False)
        ]

    def balance(self, asset_id: bytes | None = None) -> int:
        if asset_id is None:
            return sum(int(coin.amount) for coin in self.xch_coins())
        return sum(int(cat.coin.amount) for cat in self.cats(asset_id))

    def funding(self, asset_id: bytes | None = None) -> builder.Funding:
        """All of this party's XCH, plus all of one CAT when ``asset_id`` is given."""
        return builder.Funding(
            change_puzzle_hash=self.puzzle_hash,
            xch=self.xch_coins(),
            cats=self.cats(asset_id) if asset_id is not None else (),
        )


class World:
    def __init__(self) -> None:
        self.sim = sdk.Simulator()
        self._keys: list[Any] = []

    def party(self, xch: int) -> Party:
        party = Party(self, xch * XCH)
        self._keys.append(party.sk)
        return party

    def signer(self) -> signing.LocalKeySigner:
        return signing.LocalKeySigner(self._keys, ADDITIONAL_DATA)

    def now(self) -> int:
        return int(self.sim.next_timestamp())

    def advance(self, seconds: int) -> None:
        self.sim.pass_time(seconds)

    def push(self, coin_spends: list[Any], signature: bytes | None = None) -> None:
        """Sign (unless a signature is given) and submit; raise if the chain refuses."""
        if signature is None:
            signature = self.signer().sign(coin_spends)
        self.push_bundle(sdk.SpendBundle(coin_spends, sdk.Signature.from_bytes(signature)))

    def push_bundle(self, bundle: Any) -> None:
        try:
            self.sim.new_transaction(bundle)
        except Exception as exc:
            raise RejectedError(str(exc)) from exc

    def issue_cat(self, party: Party, units: int) -> bytes:
        """Issue a new CAT to ``party``; returns its asset id."""
        clvm = sdk.Clvm()
        spends = sdk.Spends(clvm, party.puzzle_hash)
        for coin in party.xch_coins():
            spends.add_xch(coin)
        custody = party.custody()
        deltas = spends.apply([sdk.Action.single_issue_cat(None, units * CAT)])
        finished = spends.prepare(deltas, sdk.Relation.AssertConcurrent)
        for pending in finished.pending_spends():
            finished.insert(
                pending.coin().coin_id(),
                custody.spend(clvm, pending.p2_puzzle_hash(), pending.conditions()),
            )
        outputs = finished.spend()
        asset_id = bytes(outputs.cat(outputs.cats()[0])[0].info.asset_id)
        self.push(clvm.coin_spends())
        return asset_id

    def send_cat(self, sender: Party, to_puzzle_hash: bytes, asset_id: bytes, mojos: int) -> None:
        """Send ``mojos`` of a CAT from ``sender`` to an address."""
        clvm = sdk.Clvm()
        spends = sdk.Spends(clvm, sender.puzzle_hash)
        for coin in sender.xch_coins():
            spends.add_xch(coin)
        for cat in sender.cats(asset_id):
            spends.add_cat(cat)
        custody = sender.custody()
        deltas = spends.apply(
            [
                sdk.Action.send(
                    sdk.Id.existing(asset_id), to_puzzle_hash, mojos, clvm.alloc([to_puzzle_hash])
                )
            ]
        )
        finished = spends.prepare(deltas, sdk.Relation.AssertConcurrent)
        for pending in finished.pending_spends():
            finished.insert(
                pending.coin().coin_id(),
                custody.spend(clvm, pending.p2_puzzle_hash(), pending.conditions()),
            )
        finished.spend()
        self.push(clvm.coin_spends())

    def take(self, taker: Party, offer_text: str, *, fee: int = 0) -> None:
        """Accept an offer as ``taker``. Port of Sage's ``take_offer``."""
        clvm = sdk.Clvm()
        offer = sdk.Offer.from_spend_bundle(clvm, sdk.decode_offer(offer_text))
        spends = sdk.Spends(clvm, taker.puzzle_hash)
        spends.add_offered_coins(offer)
        for coin in taker.xch_coins():
            spends.add_xch(coin)
        requested = offer.requested_payments()
        for asset_id in requested.cat_asset_ids():
            for cat in taker.cats(bytes(asset_id)):
                spends.add_cat(cat)
        custody = taker.custody()
        deltas = spends.apply([sdk.Action.fee(fee), *requested.actions()])
        finished = spends.prepare(deltas, sdk.Relation.AssertConcurrent)
        for pending in finished.pending_spends():
            finished.insert(
                pending.coin().coin_id(),
                custody.spend(clvm, pending.p2_puzzle_hash(), pending.conditions()),
            )
        finished.spend()
        coin_spends = clvm.coin_spends()
        signature = self.signer().sign(coin_spends)
        self.push_bundle(
            offer.take(sdk.SpendBundle(coin_spends, sdk.Signature.from_bytes(signature)))
        )

    def load(self, launcher_id: bytes) -> chain.LoadedOption:
        return chain.load_option(self.sim, launcher_id, now_seconds=self.now())
