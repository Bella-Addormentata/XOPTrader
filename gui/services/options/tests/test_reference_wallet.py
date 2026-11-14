"""Signing option spends through the reference wallet's RPC.

``FakeWallet`` stands in for the wallet daemon. It speaks the JSON that
chia-blockchain 2.7.5's ``gather_signing_info``, ``execute_signing_instructions``,
``sign_message_by_address`` and ``push_tx`` speak (the shapes were generated from
that release's own request classes), signs with keys for the addresses it owns,
and broadcasts to the simulated chain.

Two things are under test. First, that a whole option lifecycle can be signed
with nothing but those RPCs: XOPTrader holds no secret key anywhere in these
tests. Second, that :class:`ReferenceWallet` does not take the wallet's word for
anything: each ``test_wallet_that_...`` case has the fake misbehave in one way
and expects a refusal before any signature is used or anything is broadcast.

Fidelity to the real signer is covered separately in
``test_reference_wallet_real_signer.py``, which runs only where chia-blockchain
is installed.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import pytest

sdk = pytest.importorskip("chia_wallet_sdk")

from gui.services.options import builder, signing  # noqa: E402
from gui.services.options.chain import OptionStatus  # noqa: E402
from gui.services.options.reference_wallet import (  # noqa: E402
    ReferenceWallet,
    coin_from_json,
    spend_to_json,
)
from gui.services.options.tests.sim_world import (  # noqa: E402
    ADDITIONAL_DATA,
    CAT,
    XCH,
    Party,
    World,
)


def _hex(value: bytes) -> str:
    return "0x" + bytes(value).hex()


def _unhex(value: str) -> bytes:
    return bytes.fromhex(value[2:] if value.startswith("0x") else value)


def _fingerprint(public_key: Any) -> bytes:
    return int(public_key.fingerprint()).to_bytes(4, "big")


class FakeWallet:
    """The reference wallet's signer RPCs, for the addresses of ``parties``.

    ``tamper`` hooks let a test corrupt one response; they receive the honest
    response dict and return what the "wallet" sends instead.
    """

    def __init__(self, world: World, *parties: Party) -> None:
        self.world = world
        self.by_address = {party.puzzle_hash: party for party in parties}
        self.by_fingerprint = {_fingerprint(party.pk): party for party in parties}
        self.calls: list[tuple[str, dict]] = []
        self.tamper: dict[str, Callable[[dict], Any]] = {}

    def endpoints(self) -> list[str]:
        return [endpoint for endpoint, _ in self.calls]

    def __call__(self, endpoint: str, body: dict) -> dict:
        self.calls.append((endpoint, body))
        response = getattr(self, "_" + endpoint)(body)
        response.setdefault("success", True)
        if endpoint in self.tamper:
            return self.tamper[endpoint](response)
        return response

    def _sign_message_by_address(self, body: dict) -> dict:
        puzzle_hash = bytes(sdk.Address.decode(body["address"]).puzzle_hash)
        party = self.by_address.get(puzzle_hash)
        if party is None:
            # The real wallet raises; its RPC layer turns that into success=false.
            return {"success": False, "error": f"Puzzle hash {puzzle_hash.hex()} not found"}
        signature = party.sk.sign(b"message framing is the wallet's business")
        return {
            "pubkey": _hex(party.pk.to_bytes()),
            "signature": _hex(signature.to_bytes()),
            "signing_mode": "BLS_MESSAGE_AUGMENTATION_CHIP_0002",
        }

    def _gather_signing_info(self, body: dict) -> dict:
        coin_spends = [
            sdk.CoinSpend(
                sdk.Coin(
                    _unhex(spend["coin"]["parent_coin_id"]),
                    _unhex(spend["coin"]["puzzle_hash"]),
                    int(spend["coin"]["amount"]),
                ),
                _unhex(spend["puzzle"]),
                _unhex(spend["solution"]),
            )
            for spend in body["spends"]
        ]
        targets, path_hints = [], []
        for item in signing.required_signatures(coin_spends, ADDITIONAL_DATA):
            fingerprint = _fingerprint(sdk.PublicKey.from_bytes(item.public_key))
            targets.append(
                {
                    "fingerprint": _hex(fingerprint),
                    "message": _hex(item.message),
                    "hook": _hex(item.hook),
                }
            )
            if fingerprint in self.by_fingerprint:
                path_hints.append(
                    {"root_fingerprint": _hex(b"\x00\x00\x00\x01"), "path": [12381, 8444, 2, 0]}
                )
        return {
            "signing_instructions": {
                "key_hints": {"sum_hints": [], "path_hints": path_hints},
                "targets": targets,
            }
        }

    def _execute_signing_instructions(self, body: dict) -> dict:
        assert body["partial_allowed"] is False
        by_hook: dict[str, list[Any]] = {}
        for target in body["signing_instructions"]["targets"]:
            party = self.by_fingerprint.get(_unhex(target["fingerprint"]))
            if party is None:
                return {"success": False, "error": "Pubkey not found (or path/sum hinted to)"}
            by_hook.setdefault(target["hook"], []).append(party.sk.sign(_unhex(target["message"])))
        # Like the real signer, one aggregated response per hook.
        return {
            "signing_responses": [
                {"signature": _hex(sdk.Signature.aggregate(group).to_bytes()), "hook": hook}
                for hook, group in by_hook.items()
            ]
        }

    def _push_tx(self, body: dict) -> dict:
        self.world.push_bundle(sdk.SpendBundle.from_bytes(_unhex(body["spend_bundle"])))
        return {}


def _setup(*, stranger_funds: bool = False):
    world = World()
    maker, taker = world.party(150), world.party(10)
    usd = world.issue_cat(taker, 1000)
    wallet = FakeWallet(world, maker)
    reference = ReferenceWallet(wallet, agg_sig_me_additional_data=ADDITIONAL_DATA)
    return world, maker, taker, usd, wallet, reference


def _custody_from_wallet(reference: ReferenceWallet, puzzle_hash: bytes) -> builder.Custody:
    """Custody built the production way: the key comes from the wallet, not from us."""
    custody = builder.Custody()
    assert custody.add_key(reference.synthetic_key(puzzle_hash)) == puzzle_hash
    return custody


def _mint_spends(world: World, maker: Party, usd: bytes, custody: builder.Custody | None = None):
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(usd, 120 * CAT),
        world.now() + 3600,
        maker.puzzle_hash,
    )
    return builder.build_mint(terms, maker.funding(), custody or maker.custody())


# ---------------------------------------------------------------------------
# Honest wallet
# ---------------------------------------------------------------------------


def test_key_lookup_returns_the_address_key() -> None:
    _, maker, _, _, wallet, reference = _setup()

    key = reference.synthetic_key(maker.puzzle_hash)

    assert bytes(key.to_bytes()) == bytes(maker.pk.to_bytes())
    ((endpoint, body),) = wallet.calls
    assert endpoint == "sign_message_by_address"
    assert bytes(sdk.Address.decode(body["address"]).puzzle_hash) == maker.puzzle_hash
    assert body["address"].startswith("xch1")


def test_key_lookup_for_an_address_the_wallet_does_not_own_fails() -> None:
    _, _, taker, _, _, reference = _setup()
    with pytest.raises(signing.SigningError, match="sign_message_by_address"):
        reference.synthetic_key(taker.puzzle_hash)


def test_wallet_signs_a_mint_and_the_chain_accepts_it() -> None:
    world, maker, _, usd, wallet, reference = _setup()
    minted = _mint_spends(world, maker, usd)

    signature = reference.sign(minted.coin_spends)

    assert wallet.endpoints() == ["gather_signing_info", "execute_signing_instructions"]
    sent = wallet.calls[0][1]["spends"]
    assert sent == [spend_to_json(spend) for spend in minted.coin_spends]
    assert set(sent[0]) == {"coin", "puzzle", "solution"}
    assert set(sent[0]["coin"]) == {"parent_coin_id", "puzzle_hash", "amount"}
    # The instructions go back to the wallet exactly as it issued them.
    assert wallet.calls[1][1]["signing_instructions"]["targets"]
    world.push(minted.coin_spends, signature)
    assert world.load(minted.launcher_id).status is OptionStatus.LIVE


def test_whole_maker_lifecycle_with_no_key_outside_the_wallet() -> None:
    """Mint, offer, get taken, claw back: every maker signature via wallet RPC."""
    world, maker, taker, usd, wallet, reference = _setup()
    custody = _custody_from_wallet(reference, maker.puzzle_hash)

    minted = _mint_spends(world, maker, usd, custody)
    name = reference.push(minted.coin_spends, reference.sign(minted.coin_spends))
    assert len(name) == 32

    loaded = world.load(minted.launcher_id)
    draft = builder.build_offer(
        loaded.option,
        builder.Asset(usd, 2100),
        maker.puzzle_hash,
        custody,
        expires_at_seconds=world.now() + 900,
    )
    offer = draft.encode(reference.sign(draft.coin_spends))
    world.take(taker, offer)
    assert maker.balance(usd) == 2100

    world.advance(3600)
    expired = world.load(minted.launcher_id)
    assert expired.status is OptionStatus.EXPIRED
    clawback = builder.build_clawback(
        expired.underlying,
        expired.locked,
        reference.synthetic_key(maker.puzzle_hash),
        builder.Funding(change_puzzle_hash=maker.puzzle_hash),
        custody,
    )
    reference.push(clawback, reference.sign(clawback))

    assert maker.balance() == 150 * XCH - builder.OPTION_COIN_AMOUNT
    assert world.load(minted.launcher_id).status is OptionStatus.CLOSED
    assert wallet.endpoints().count("push_tx") == 2


def test_bundle_needing_no_signature_does_not_touch_the_wallet() -> None:
    _, _, _, _, wallet, reference = _setup()
    assert reference.sign([]) == signing.empty_signature()
    assert wallet.calls == []


# ---------------------------------------------------------------------------
# Wallet that cannot or will not sign
# ---------------------------------------------------------------------------


def test_wallet_without_the_key_cannot_sign() -> None:
    world, _, taker, usd, wallet, reference = _setup()
    # Spends funded from the taker's address, which this wallet does not own.
    terms = builder.OptionTerms(
        builder.Asset(None, 5 * XCH),
        builder.Asset(usd, 6 * CAT),
        world.now() + 3600,
        taker.puzzle_hash,
    )
    spends = builder.build_mint(terms, taker.funding(), taker.custody()).coin_spends

    with pytest.raises(signing.SigningError, match="execute_signing_instructions"):
        reference.sign(spends)


def test_transport_failure_is_a_signing_error() -> None:
    world, maker, _, usd, _, _ = _setup()

    def broken(endpoint: str, body: dict) -> dict:
        raise ConnectionError("wallet daemon is not running")

    reference = ReferenceWallet(broken, agg_sig_me_additional_data=ADDITIONAL_DATA)
    with pytest.raises(signing.SigningError, match="not running"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)


def test_old_wallet_without_signer_rpcs_is_a_signing_error() -> None:
    world, maker, _, usd, _, _ = _setup()

    def old(endpoint: str, body: dict) -> dict:
        return {"success": False, "error": f"unknown endpoint /{endpoint}"}

    reference = ReferenceWallet(old, agg_sig_me_additional_data=ADDITIONAL_DATA)
    with pytest.raises(signing.SigningError, match="unknown endpoint"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)


# ---------------------------------------------------------------------------
# Wallet that misbehaves: nothing it says is taken on trust
# ---------------------------------------------------------------------------


def test_wallet_that_returns_another_key_is_refused() -> None:
    _, maker, taker, _, wallet, reference = _setup()
    wallet.tamper["sign_message_by_address"] = lambda response: {
        **response,
        "pubkey": _hex(taker.pk.to_bytes()),
    }
    with pytest.raises(signing.SigningError, match="does not belong"):
        reference.synthetic_key(maker.puzzle_hash)


def test_wallet_that_adds_a_signing_target_is_refused() -> None:
    world, maker, _, usd, wallet, reference = _setup()

    def add_target(response: dict) -> dict:
        targets = response["signing_instructions"]["targets"]
        extra = signing.RequiredSignature(bytes(maker.pk.to_bytes()), b"pay the attacker")
        targets.append(
            {
                "fingerprint": targets[0]["fingerprint"],
                "message": _hex(extra.message),
                "hook": _hex(extra.hook),
            }
        )
        return response

    wallet.tamper["gather_signing_info"] = add_target
    with pytest.raises(signing.SigningError, match="targets differ"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)
    # Refused before anything was signed.
    assert "execute_signing_instructions" not in wallet.endpoints()


def test_wallet_that_drops_a_signing_target_is_refused() -> None:
    world, maker, _, usd, wallet, reference = _setup()

    def drop_target(response: dict) -> dict:
        response["signing_instructions"]["targets"].pop()
        return response

    wallet.tamper["gather_signing_info"] = drop_target
    with pytest.raises(signing.SigningError, match="targets differ"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)
    assert "execute_signing_instructions" not in wallet.endpoints()


def test_wallet_that_rewrites_a_message_is_refused() -> None:
    world, maker, _, usd, wallet, reference = _setup()

    def rewrite(response: dict) -> dict:
        response["signing_instructions"]["targets"][0]["message"] = _hex(b"something else")
        return response

    wallet.tamper["gather_signing_info"] = rewrite
    with pytest.raises(signing.SigningError, match="targets differ"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)
    assert "execute_signing_instructions" not in wallet.endpoints()


def test_wallet_that_signs_with_the_wrong_key_is_refused() -> None:
    world, maker, taker, usd, wallet, reference = _setup()
    spends = _mint_spends(world, maker, usd).coin_spends
    required = signing.required_signatures(spends, ADDITIONAL_DATA)

    def wrong_key(response: dict) -> dict:
        return {
            "signing_responses": [
                {"signature": _hex(taker.sk.sign(item.message).to_bytes()), "hook": _hex(item.hook)}
                for item in required
            ],
            "success": True,
        }

    wallet.tamper["execute_signing_instructions"] = wrong_key
    with pytest.raises(signing.SigningError, match="does not cover"):
        reference.sign(spends)


def test_wallet_that_returns_only_some_signatures_is_refused() -> None:
    world, maker, _, usd, wallet, reference = _setup()

    def partial(response: dict) -> dict:
        response["signing_responses"] = response["signing_responses"][:1]
        return response

    wallet.tamper["execute_signing_instructions"] = partial
    with pytest.raises(signing.SigningError, match="does not cover"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)


def test_wallet_that_signs_something_unrequested_is_refused() -> None:
    world, maker, _, usd, wallet, reference = _setup()

    def unrequested(response: dict) -> dict:
        response["signing_responses"].append(
            {"signature": _hex(maker.sk.sign(b"extra").to_bytes()), "hook": _hex(b"\x09" * 32)}
        )
        return response

    wallet.tamper["execute_signing_instructions"] = unrequested
    with pytest.raises(signing.SigningError, match="not requested"):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)


@pytest.mark.parametrize(
    "response",
    [
        {"success": True},
        {"success": True, "signing_instructions": "nope"},
        {"success": True, "signing_instructions": {"key_hints": {}}},
        {"success": True, "signing_instructions": {"targets": [{"hook": 5, "message": "0x00"}]}},
        {"success": True, "signing_instructions": {"targets": [{"hook": "zz", "message": "0x"}]}},
        ["not", "a", "dict"],
    ],
)
def test_wallet_that_returns_nonsense_is_refused(response) -> None:
    world, maker, _, usd, _, _ = _setup()
    reference = ReferenceWallet(lambda e, b: response, agg_sig_me_additional_data=ADDITIONAL_DATA)
    with pytest.raises(signing.SigningError):
        reference.sign(_mint_spends(world, maker, usd).coin_spends)


# ---------------------------------------------------------------------------
# Broadcast
# ---------------------------------------------------------------------------


def test_push_refuses_a_signature_that_does_not_cover_the_bundle() -> None:
    world, maker, taker, usd, wallet, reference = _setup()
    spends = _mint_spends(world, maker, usd).coin_spends
    wrong = signing.LocalKeySigner([maker.sk], signing.MAINNET_AGG_SIG_ME_ADDITIONAL_DATA)

    with pytest.raises(signing.SigningError, match="refusing to broadcast"):
        reference.push(spends, wrong.sign(spends))
    with pytest.raises(signing.SigningError, match="refusing to broadcast"):
        reference.push(spends, signing.empty_signature())

    assert wallet.calls == []
    assert maker.balance() == 150 * XCH


def test_push_reports_a_chain_rejection() -> None:
    world, maker, _, usd, _, reference = _setup()
    spends = _mint_spends(world, maker, usd).coin_spends
    signature = reference.sign(spends)
    reference.push(spends, signature)
    # The same coins again: the simulated node refuses, as a real one would.
    with pytest.raises(signing.SigningError, match="push_tx"):
        reference.push(spends, signature)


# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------


def test_coin_from_json_accepts_a_coin_or_a_record() -> None:
    coin = {"parent_coin_info": "0x" + "01" * 32, "puzzle_hash": "02" * 32, "amount": 7}

    bare = coin_from_json(coin)
    wrapped = coin_from_json({"coin": coin, "confirmed_block_index": 5})

    assert bytes(bare.parent_coin_info) == b"\x01" * 32
    assert bytes(bare.puzzle_hash) == b"\x02" * 32
    assert int(bare.amount) == 7
    assert bytes(wrapped.coin_id()) == bytes(bare.coin_id())


@pytest.mark.parametrize(
    "record",
    [
        {},
        {"coin": "nope"},
        {"parent_coin_info": "0x01", "puzzle_hash": 5, "amount": 1},
        {"parent_coin_info": "zz" * 32, "puzzle_hash": "02" * 32, "amount": 1},
        {"parent_coin_info": "01" * 32, "puzzle_hash": "02" * 32},
    ],
)
def test_coin_from_json_rejects_malformed_coins(record) -> None:
    with pytest.raises(signing.SigningError):
        coin_from_json(record)


def test_a_wallet_coin_round_trips_into_a_spend() -> None:
    """A coin as the wallet RPC reports it is enough to fund a mint."""
    world, maker, _, usd, _, reference = _setup()
    (coin,) = maker.xch_coins()
    as_reported = {
        "coin": {
            "parent_coin_info": _hex(coin.parent_coin_info),
            "puzzle_hash": _hex(coin.puzzle_hash),
            "amount": int(coin.amount),
        }
    }
    funding = builder.Funding(
        change_puzzle_hash=maker.puzzle_hash, xch=[coin_from_json(as_reported)]
    )
    terms = builder.OptionTerms(
        builder.Asset(None, 100 * XCH),
        builder.Asset(usd, 120 * CAT),
        world.now() + 3600,
        maker.puzzle_hash,
    )
    minted = builder.build_mint(terms, funding, _custody_from_wallet(reference, maker.puzzle_hash))
    reference.push(minted.coin_spends, reference.sign(minted.coin_spends))
    assert world.load(minted.launcher_id).status is OptionStatus.LIVE
