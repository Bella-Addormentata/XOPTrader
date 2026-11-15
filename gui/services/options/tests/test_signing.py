"""What a bundle needs signed, and whether a signature covers it.

``required_signatures`` and ``verify`` are XOPTrader's own view of a bundle,
independent of whoever signs it. These tests pin that view: it finds every
signature a real option spend needs, it rejects a signature that covers less or
something else, and it refuses the signature kinds an option spend never uses.
"""

from __future__ import annotations

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, signing  # noqa: E402
from gui.services.options.tests.sim_world import ADDITIONAL_DATA, CAT, XCH, World  # noqa: E402


def _mint_spends(world: World):
    maker = world.party(150)
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(b"\x07" * 32, 120 * CAT),
        world.now() + 3600,
        maker.puzzle_hash,
    )
    return maker, builder.build_mint(terms, maker.funding(), maker.custody()).coin_spends


def _quoted(conditions) -> tuple[bytes, bytes]:
    """A puzzle that returns ``conditions`` whatever its solution: ``(q . conditions)``."""
    clvm = sdk.Clvm()
    puzzle = clvm.pair(clvm.int(1), clvm.list([build(clvm) for build in conditions]))
    return bytes(puzzle.serialize()), bytes(clvm.nil().serialize())


def _spend_with(conditions):
    reveal, solution = _quoted(conditions)
    clvm = sdk.Clvm()
    coin = sdk.Coin(b"\x01" * 32, bytes(clvm.deserialize(reveal).tree_hash()), 1)
    return sdk.CoinSpend(coin, reveal, solution)


def test_mint_needs_one_signature_per_wallet_coin_spent() -> None:
    world = World()
    maker, coin_spends = _mint_spends(world)

    required = signing.required_signatures(coin_spends, ADDITIONAL_DATA)

    # The funding coin and the new option coin; the launcher needs none.
    assert len(coin_spends) == 3
    assert len(required) == 2
    assert {item.public_key for item in required} == {bytes(maker.pk.to_bytes())}
    # AGG_SIG_ME binds each message to its coin and to the network.
    spent = {bytes(spend.coin.coin_id()) for spend in coin_spends}
    for item in required:
        assert item.message.endswith(ADDITIONAL_DATA)
        assert item.message[-64:-32] in spent


def test_local_signature_verifies_and_the_chain_accepts_it() -> None:
    world = World()
    _, coin_spends = _mint_spends(world)
    required = signing.required_signatures(coin_spends, ADDITIONAL_DATA)

    signature = world.signer().sign(coin_spends)

    assert signing.verify(required, signature)
    world.push(coin_spends, signature)


def test_signature_for_another_network_does_not_verify() -> None:
    world = World()
    maker, coin_spends = _mint_spends(world)
    mainnet = signing.LocalKeySigner([maker.sk], signing.MAINNET_AGG_SIG_ME_ADDITIONAL_DATA)

    signature = mainnet.sign(coin_spends)

    assert not signing.verify(signing.required_signatures(coin_spends, ADDITIONAL_DATA), signature)
    assert signing.verify(
        signing.required_signatures(coin_spends, signing.MAINNET_AGG_SIG_ME_ADDITIONAL_DATA),
        signature,
    )


def test_partial_signature_does_not_verify() -> None:
    world = World()
    maker, coin_spends = _mint_spends(world)
    required = signing.required_signatures(coin_spends, ADDITIONAL_DATA)

    only_first = bytes(maker.sk.sign(required[0].message).to_bytes())

    assert not signing.verify(required, only_first)
    assert signing.verify(required[:1], only_first)


def test_signature_by_another_key_does_not_verify() -> None:
    world = World()
    _, coin_spends = _mint_spends(world)
    stranger = world.party(1)
    required = signing.required_signatures(coin_spends, ADDITIONAL_DATA)

    forged = sdk.Signature.aggregate([stranger.sk.sign(item.message) for item in required])

    assert not signing.verify(required, bytes(forged.to_bytes()))


def test_garbage_is_not_a_signature() -> None:
    world = World()
    _, coin_spends = _mint_spends(world)
    required = signing.required_signatures(coin_spends, ADDITIONAL_DATA)
    assert not signing.verify(required, b"\x00" * 96)
    assert not signing.verify(required, b"short")
    assert not signing.verify(required, signing.empty_signature())


def test_bundle_with_no_signatures_takes_the_empty_signature() -> None:
    spend = _spend_with([lambda clvm: clvm.create_coin(b"\x02" * 32, 1, None)])

    assert signing.required_signatures([spend], ADDITIONAL_DATA) == []
    assert signing.empty_signature() == bytes(sdk.Signature.infinity().to_bytes())
    assert signing.verify([], signing.empty_signature())
    assert signing.LocalKeySigner([], ADDITIONAL_DATA).sign([spend]) == signing.empty_signature()
    # A real signature where none is needed is not "close enough".
    key = sdk.SecretKey.from_seed(b"\x05" * 32)
    assert not signing.verify([], bytes(key.sign(b"anything").to_bytes()))


def test_agg_sig_me_message_is_puzzle_message_coin_id_then_network() -> None:
    key = sdk.SecretKey.from_seed(b"\x05" * 32).public_key()
    spend = _spend_with([lambda clvm: clvm.agg_sig_me(key, b"hello")])

    (required,) = signing.required_signatures([spend], ADDITIONAL_DATA)

    assert required.public_key == bytes(key.to_bytes())
    assert required.message == b"hello" + bytes(spend.coin.coin_id()) + ADDITIONAL_DATA


@pytest.mark.parametrize(
    "kind",
    [
        "agg_sig_unsafe",
        "agg_sig_parent",
        "agg_sig_puzzle",
        "agg_sig_amount",
        "agg_sig_puzzle_amount",
        "agg_sig_parent_amount",
        "agg_sig_parent_puzzle",
    ],
)
def test_other_signature_kinds_are_refused(kind: str) -> None:
    """An option spend only ever asks for AGG_SIG_ME; anything else is not ours."""
    key = sdk.SecretKey.from_seed(b"\x05" * 32).public_key()
    spend = _spend_with([lambda clvm: getattr(clvm, kind)(key, b"hello")])
    with pytest.raises(signing.SigningError, match="only AGG_SIG_ME"):
        signing.required_signatures([spend], ADDITIONAL_DATA)


def test_puzzle_that_does_not_run_is_an_error() -> None:
    clvm = sdk.Clvm()
    # ``(x)`` always raises.
    reveal = bytes(clvm.list([clvm.int(8)]).serialize())
    coin = sdk.Coin(b"\x01" * 32, bytes(clvm.deserialize(reveal).tree_hash()), 1)
    spend = sdk.CoinSpend(coin, reveal, bytes(clvm.nil().serialize()))
    with pytest.raises(signing.SigningError, match="did not run"):
        signing.required_signatures([spend], ADDITIONAL_DATA)


def test_additional_data_must_be_a_hash() -> None:
    with pytest.raises(signing.SigningError):
        signing.required_signatures([], b"\x00" * 31)


def test_local_signer_without_the_key_fails() -> None:
    world = World()
    _, coin_spends = _mint_spends(world)
    with pytest.raises(signing.SigningError, match="no secret key"):
        signing.LocalKeySigner([], ADDITIONAL_DATA).sign(coin_spends)


def test_hook_matches_the_signer_protocol() -> None:
    """The reference wallet identifies a target as sha256(public key + message)."""
    import hashlib

    item = signing.RequiredSignature(public_key=b"\x0a" * 48, message=b"msg")
    assert item.hook == hashlib.sha256(b"\x0a" * 48 + b"msg").digest()
