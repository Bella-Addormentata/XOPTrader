"""Unsigned coin spends for CHIP-0045 option contracts.

An option is a singleton that gives its owner the right to take one locked coin
(the *underlying*) in exchange for a fixed payment to the creator (the *strike*)
until an expiry time. A covered call locks XCH and strikes in a stablecoin; a
cash-secured put locks the stablecoin and strikes in XCH. After expiry the
creator can spend the locked coin again and the option is worthless.

Every function here is pure: it takes coins and keys the caller already knows
and returns coin spends with no signature. Nothing touches a wallet, a node or
the network, so each one runs unchanged against the SDK's simulator in tests.
Signing is :mod:`gui.services.options.signing`'s job.

PROVENANCE. The flows are ported from the Sage wallet
(``crates/sage-wallet/src/wallet/options.rs`` and ``wallet/offer/make_offer.rs``
at xch-dev/sage ``e2bbfbe``, Apache-2.0), which builds them on the
``chia-wallet-sdk`` action system. The same action system is exposed to Python,
so this is a port of Sage's ~400 lines of orchestration, not of any Chialisp:
the puzzles and their drivers stay inside the SDK. See THIRD_PARTY_NOTICES.md.

WHY THE REFERENCE WALLET CAN SIGN THESE. The only keys an option's spends ask
for are the standard-puzzle keys of ordinary addresses: the funding coins', the
option owner's and the creator's. :class:`Custody` therefore needs nothing but
the synthetic public key of each address, and one wallet address can play all
three roles.
"""

from __future__ import annotations

import dataclasses
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:  # the SDK ships type stubs; never imported at runtime here
    from chia_wallet_sdk import (
        Action,
        Cat,
        Clvm,
        Coin,
        CoinSpend,
        OptionContract,
        OptionUnderlying,
        Outputs,
        Program,
        PublicKey,
        Spend,
        Spends,
    )

# The amount of an option singleton. CHIP-0045 only needs it odd; every wallet
# that mints options uses 1.
OPTION_COIN_AMOUNT = 1


class OptionBuildError(ValueError):
    """The request cannot be turned into a valid spend. Nothing was built."""


def _sdk() -> Any:
    """Import ``chia_wallet_sdk`` on first use (see the package docstring)."""
    try:
        import chia_wallet_sdk
    except ImportError as exc:  # pragma: no cover - exercised only without the wheel
        raise OptionBuildError(
            "chia-wallet-sdk is not installed; option contracts are unavailable"
        ) from exc
    return chia_wallet_sdk


def _check_hash(value: bytes, what: str) -> None:
    if not isinstance(value, (bytes, bytearray)) or len(value) != 32:
        raise OptionBuildError(f"{what} must be 32 bytes")


# ---------------------------------------------------------------------------
# Terms
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Asset:
    """An amount of XCH (``asset_id`` is None) or of one CAT, in mojos."""

    asset_id: bytes | None
    amount: int

    def __post_init__(self) -> None:
        if self.asset_id is not None:
            _check_hash(self.asset_id, "asset_id")
        # bool is an int; a True amount is a caller bug, not one mojo.
        if isinstance(self.amount, bool) or not isinstance(self.amount, int):
            raise OptionBuildError("amount must be an integer number of mojos")
        if self.amount <= 0:
            raise OptionBuildError("amount must be positive")

    @property
    def is_xch(self) -> bool:
        return self.asset_id is None

    def sdk_id(self) -> Any:
        sdk = _sdk()
        return sdk.Id.xch() if self.asset_id is None else sdk.Id.existing(self.asset_id)

    def sdk_option_type(self) -> Any:
        sdk = _sdk()
        if self.asset_id is None:
            return sdk.OptionType.xch(self.amount)
        return sdk.OptionType.cat(self.asset_id, self.amount)


@dataclasses.dataclass(frozen=True)
class OptionTerms:
    """What an option contract fixes when it is minted. None of it can change."""

    underlying: Asset
    strike: Asset
    expiration_seconds: int
    creator_puzzle_hash: bytes

    def __post_init__(self) -> None:
        _check_hash(self.creator_puzzle_hash, "creator_puzzle_hash")
        if isinstance(self.expiration_seconds, bool) or not isinstance(
            self.expiration_seconds, int
        ):
            raise OptionBuildError("expiration_seconds must be an integer")
        if self.expiration_seconds <= 0:
            raise OptionBuildError("expiration_seconds must be positive")

    def underlying_puzzle(self, launcher_id: bytes) -> OptionUnderlying:
        """The puzzle that locks this option's underlying coin."""
        _check_hash(launcher_id, "launcher_id")
        return _sdk().OptionUnderlying(
            launcher_id,
            self.creator_puzzle_hash,
            self.expiration_seconds,
            self.underlying.amount,
            self.strike.sdk_option_type(),
        )


@dataclasses.dataclass(frozen=True)
class Locked:
    """The coin holding an option's underlying asset.

    ``cat`` is set when the underlying is a CAT: spending a CAT needs its lineage
    proof, which the plain coin does not carry.
    """

    coin: Coin
    cat: Cat | None = None


@dataclasses.dataclass(frozen=True)
class Funding:
    """Wallet coins a spend may draw on, and where its change goes.

    Every coin's p2 puzzle hash, and ``change_puzzle_hash`` itself, must be known
    to the :class:`Custody` passed alongside: the action system routes new
    singletons and intermediate coins through the change address.
    """

    change_puzzle_hash: bytes
    xch: Sequence[Coin] = ()
    cats: Sequence[Cat] = ()

    def __post_init__(self) -> None:
        _check_hash(self.change_puzzle_hash, "change_puzzle_hash")


# ---------------------------------------------------------------------------
# Custody: who authorises each coin
# ---------------------------------------------------------------------------


class Custody:
    """Builds the p2 spend for each coin the action system wants spent.

    Two kinds of coin appear in option spends: coins at an ordinary address
    (standard puzzle, one synthetic key) and, after expiry, the locked
    underlying coin, which the creator spends through the option's clawback
    path. Both end in a signature by a standard key.
    """

    def __init__(self) -> None:
        self._keys: dict[bytes, Any] = {}
        self._expired: dict[bytes, tuple[Any, Any]] = {}

    def add_key(self, synthetic_key: PublicKey) -> bytes:
        """Register an address by its synthetic public key; returns its puzzle hash."""
        puzzle_hash = bytes(_sdk().standard_puzzle_hash(synthetic_key))
        self._keys[puzzle_hash] = synthetic_key
        return puzzle_hash

    def add_expired_underlying(self, underlying: OptionUnderlying, creator_key: PublicKey) -> bytes:
        """Register an option's locked coin for a post-expiry clawback.

        ``creator_key`` must be the creator address's synthetic key. A wrong key
        would build a spend the chain rejects, so it is refused here instead.
        """
        if bytes(_sdk().standard_puzzle_hash(creator_key)) != bytes(underlying.creator_puzzle_hash):
            raise OptionBuildError("creator_key is not the key of the option's creator address")
        puzzle_hash = bytes(underlying.puzzle_hash())
        self._expired[puzzle_hash] = (underlying, creator_key)
        return puzzle_hash

    def knows(self, p2_puzzle_hash: bytes) -> bool:
        key = bytes(p2_puzzle_hash)
        return key in self._keys or key in self._expired

    def has_key(self, puzzle_hash: bytes) -> bool:
        """True if ``puzzle_hash`` is an ordinary address whose key is registered."""
        return bytes(puzzle_hash) in self._keys

    def spend(self, clvm: Clvm, p2_puzzle_hash: bytes, conditions: list[Program]) -> Spend:
        key = bytes(p2_puzzle_hash)
        if key in self._keys:
            return clvm.standard_spend(self._keys[key], clvm.delegated_spend(conditions))
        if key in self._expired:
            underlying, creator_key = self._expired[key]
            return underlying.clawback_spend(
                clvm.standard_spend(creator_key, clvm.delegated_spend(conditions))
            )
        raise OptionBuildError(f"no key for p2 puzzle hash {key.hex()}")


# ---------------------------------------------------------------------------
# Shared plumbing
# ---------------------------------------------------------------------------


def _check_fee(fee: int) -> None:
    if isinstance(fee, bool) or not isinstance(fee, int) or fee < 0:
        raise OptionBuildError("fee must be a non-negative integer number of mojos")


def _new_spends(clvm: Clvm, funding: Funding, custody: Custody) -> Spends:
    if not custody.knows(funding.change_puzzle_hash):
        raise OptionBuildError("the change address has no key in custody")
    spends = _sdk().Spends(clvm, funding.change_puzzle_hash)
    for coin in funding.xch:
        spends.add_xch(coin)
    for cat in funding.cats:
        spends.add_cat(cat)
    return spends


def _complete(clvm: Clvm, spends: Spends, actions: list[Action], custody: Custody) -> Outputs:
    """Apply the actions and insert the p2 spend of every coin they consume.

    ``AssertConcurrent`` is what Sage uses for every spend: each coin asserts the
    others are spent in the same block, so a bundle can never be split and
    included in part.
    """
    sdk = _sdk()
    try:
        deltas = spends.apply(actions)
        finished = spends.prepare(deltas, sdk.Relation.AssertConcurrent)
        for pending in finished.pending_spends():
            finished.insert(
                pending.coin().coin_id(),
                custody.spend(clvm, pending.p2_puzzle_hash(), pending.conditions()),
            )
        return finished.spend()
    except OptionBuildError:
        raise
    except Exception as exc:  # the SDK raises plain exceptions for driver errors
        raise OptionBuildError(f"could not build the spend: {exc}") from exc


def _hint(clvm: Clvm, puzzle_hash: bytes) -> Program:
    """The memo list that lets the receiving wallet find a coin: ``(puzzle_hash)``."""
    return clvm.alloc([bytes(puzzle_hash)])


def _same_coin(left: Coin, right: Coin) -> bool:
    return bytes(left.coin_id()) == bytes(right.coin_id())


def _check_locked(option: OptionContract, underlying: OptionUnderlying, locked: Locked) -> None:
    """Refuse a locked coin, or terms, that do not belong to this option.

    The option singleton commits to its underlying coin id and to the hash of the
    puzzle that pays the strike. Checking both here turns a mix-up between two
    options into an error at build time instead of a rejected transaction.
    """
    sdk = _sdk()
    info = option.info
    if bytes(underlying.launcher_id) != bytes(info.launcher_id):
        raise OptionBuildError("the terms belong to a different option")
    if bytes(locked.coin.coin_id()) != bytes(info.underlying_coin_id):
        raise OptionBuildError("the locked coin is not this option's underlying coin")
    if bytes(underlying.delegated_puzzle_hash()) != bytes(info.underlying_delegated_puzzle_hash):
        raise OptionBuildError("the strike or expiry does not match what the option commits to")
    expected = bytes(underlying.puzzle_hash())
    if locked.cat is None:
        actual = bytes(locked.coin.puzzle_hash)
    else:
        if not _same_coin(locked.cat.coin, locked.coin):
            raise OptionBuildError("locked.cat and locked.coin are different coins")
        actual = bytes(locked.cat.info.p2_puzzle_hash)
        if bytes(sdk.cat_puzzle_hash(locked.cat.info.asset_id, expected)) != bytes(
            locked.coin.puzzle_hash
        ):
            raise OptionBuildError("the locked CAT coin is not locked by this option's terms")
    if actual != expected:
        raise OptionBuildError("the locked coin is not locked by this option's terms")
    if int(locked.coin.amount) != int(underlying.amount):
        raise OptionBuildError("the locked coin's amount does not match the option's terms")


# ---------------------------------------------------------------------------
# Mint
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class MintResult:
    coin_spends: list[CoinSpend]
    option: OptionContract
    underlying: OptionUnderlying
    locked: Locked

    @property
    def launcher_id(self) -> bytes:
        return bytes(self.option.info.launcher_id)


def build_mint(
    terms: OptionTerms, funding: Funding, custody: Custody, *, fee: int = 0
) -> MintResult:
    """Mint an option: lock the underlying and create the singleton.

    The option is owned by ``funding.change_puzzle_hash``. The locked coin can
    then only leave through an exercise (which pays the strike to
    ``terms.creator_puzzle_hash``) or, after expiry, back to the creator.
    """
    _check_fee(fee)
    sdk = _sdk()
    clvm = sdk.Clvm()
    spends = _new_spends(clvm, funding, custody)
    actions = [
        sdk.Action.fee(fee),
        sdk.Action.mint_option(
            terms.creator_puzzle_hash,
            terms.expiration_seconds,
            terms.underlying.sdk_id(),
            terms.underlying.amount,
            terms.strike.sdk_option_type(),
            OPTION_COIN_AMOUNT,
        ),
    ]
    outputs = _complete(clvm, spends, actions, custody)
    # A minted asset is addressed by the index of the action that created it.
    option = outputs.option(sdk.Id.new(1))
    underlying = terms.underlying_puzzle(option.info.launcher_id)

    locked: Locked | None = None
    target = bytes(option.info.underlying_coin_id)
    if terms.underlying.is_xch:
        for coin in outputs.xch():
            if bytes(coin.coin_id()) == target:
                locked = Locked(coin=coin)
    else:
        for cat in outputs.cat(terms.underlying.sdk_id()):
            if bytes(cat.coin.coin_id()) == target:
                locked = Locked(coin=cat.coin, cat=cat)
    if locked is None:
        raise OptionBuildError("the mint did not create the option's underlying coin")
    _check_locked(option, underlying, locked)
    if bytes(option.info.p2_puzzle_hash) != bytes(funding.change_puzzle_hash):
        raise OptionBuildError("the minted option is not owned by the change address")
    return MintResult(
        coin_spends=clvm.coin_spends(), option=option, underlying=underlying, locked=locked
    )


# ---------------------------------------------------------------------------
# Offer
# ---------------------------------------------------------------------------


class OfferDraft:
    """An option offer waiting for its signature.

    The SDK objects that describe the requested payment live in the ``Clvm``
    allocator they were built in, so the draft keeps that allocator and finishes
    the offer itself once the caller has a signature for ``coin_spends``.
    """

    def __init__(
        self,
        clvm: Clvm,
        coin_spends: list[CoinSpend],
        requested_payments: Any,
        asset_info: Any,
        requested: Asset,
        expires_at_seconds: int | None,
    ) -> None:
        self._clvm = clvm
        self._requested_payments = requested_payments
        self._asset_info = asset_info
        self.coin_spends = coin_spends
        self.requested = requested
        self.expires_at_seconds = expires_at_seconds

    def encode(self, aggregated_signature: bytes) -> str:
        """Return the ``offer1...`` string for a signature over ``coin_spends``."""
        sdk = _sdk()
        try:
            bundle = sdk.SpendBundle(
                self.coin_spends, sdk.Signature.from_bytes(bytes(aggregated_signature))
            )
            offer = sdk.Offer.from_input_spend_bundle(
                self._clvm, bundle, self._requested_payments, self._asset_info
            )
            return str(sdk.encode_offer(offer.to_spend_bundle()))
        except Exception as exc:
            raise OptionBuildError(f"could not encode the offer: {exc}") from exc


def build_offer(
    option: OptionContract,
    requested: Asset,
    receive_puzzle_hash: bytes,
    custody: Custody,
    *,
    expires_at_seconds: int | None = None,
) -> OfferDraft:
    """Offer an option we own for ``requested``, paid to ``receive_puzzle_hash``.

    The offer spends nothing but the option coin and carries no fee, so making
    one costs nothing and moves nothing until somebody takes it. With
    ``expires_at_seconds`` the option's spend asserts
    ``ASSERT_BEFORE_SECONDS_ABSOLUTE``, so the chain itself refuses a late take.
    """
    _check_hash(receive_puzzle_hash, "receive_puzzle_hash")
    if expires_at_seconds is not None and (
        isinstance(expires_at_seconds, bool)
        or not isinstance(expires_at_seconds, int)
        or expires_at_seconds <= 0
    ):
        raise OptionBuildError("expires_at_seconds must be a positive integer")
    sdk = _sdk()
    clvm = sdk.Clvm()
    owner = bytes(option.info.p2_puzzle_hash)
    if not custody.knows(owner):
        raise OptionBuildError("the option's owner address has no key in custody")
    spends = sdk.Spends(clvm, owner)
    spends.add_option(option)

    nonce = sdk.Offer.nonce([option.coin.coin_id()])
    payment = sdk.NotarizedPayment(
        nonce,
        [sdk.Payment(receive_puzzle_hash, requested.amount, _hint(clvm, receive_puzzle_hash))],
    )
    requested_payments = sdk.RequestedPayments(clvm)
    asset_info = sdk.AssetInfo(clvm)
    if requested.asset_id is None:
        requested_payments.add_xch(payment)
    else:
        asset_info.insert_cat(requested.asset_id, sdk.CatAssetInfo(None))
        requested_payments.add_cat(requested.asset_id, payment)

    try:
        for assertion in requested_payments.assertions(asset_info):
            spends.add_required_condition(assertion)
    except Exception as exc:
        raise OptionBuildError(f"could not build the offer: {exc}") from exc
    if expires_at_seconds is not None:
        spends.add_required_condition(clvm.assert_before_seconds_absolute(expires_at_seconds))

    actions = [
        sdk.Action.fee(0),
        sdk.Action.send(
            sdk.Id.existing(option.info.launcher_id),
            sdk.Constants.settlement_payment_hash(),
            int(option.coin.amount),
            None,
        ),
    ]
    _complete(clvm, spends, actions, custody)
    return OfferDraft(
        clvm, clvm.coin_spends(), requested_payments, asset_info, requested, expires_at_seconds
    )


# ---------------------------------------------------------------------------
# Transfer (also how an open offer is cancelled)
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class TransferResult:
    coin_spends: list[CoinSpend]
    option: OptionContract


def build_transfer(
    option: OptionContract,
    to_puzzle_hash: bytes,
    funding: Funding,
    custody: Custody,
    *,
    fee: int = 0,
) -> TransferResult:
    """Move an option to ``to_puzzle_hash``.

    Sending it to our own address is how an open offer is cancelled: an offer is
    a signed spend of one particular option coin, and once that coin is spent
    the offer can never settle.
    """
    _check_fee(fee)
    _check_hash(to_puzzle_hash, "to_puzzle_hash")
    sdk = _sdk()
    clvm = sdk.Clvm()
    spends = _new_spends(clvm, funding, custody)
    spends.add_option(option)
    actions = [
        sdk.Action.fee(fee),
        sdk.Action.send(
            sdk.Id.existing(option.info.launcher_id),
            to_puzzle_hash,
            int(option.coin.amount),
            _hint(clvm, to_puzzle_hash),
        ),
    ]
    outputs = _complete(clvm, spends, actions, custody)
    moved = outputs.option(sdk.Id.existing(option.info.launcher_id))
    if bytes(moved.info.p2_puzzle_hash) != bytes(to_puzzle_hash):
        raise OptionBuildError("the transfer did not send the option to the requested address")
    return TransferResult(coin_spends=clvm.coin_spends(), option=moved)


# ---------------------------------------------------------------------------
# Exercise
# ---------------------------------------------------------------------------


def _release_underlying(
    clvm: Clvm,
    spends: Spends,
    option: OptionContract,
    underlying: OptionUnderlying,
    locked: Locked,
) -> None:
    """Spend the locked coin through its exercise path into the action system.

    The locked coin pays out to the offer settlement puzzle; that settlement coin
    and the option itself become inputs of ``spends``. The spend is only valid in
    a bundle that also melts the option and makes the strike payment.
    """
    sdk = _sdk()
    exercise = underlying.exercise_spend(
        clvm, option.info.inner_puzzle_hash(), int(option.coin.amount)
    )
    if locked.cat is None:
        clvm.spend_coin(locked.coin, exercise)
        spends.add_xch(
            sdk.Coin(
                locked.coin.coin_id(),
                sdk.Constants.settlement_payment_hash(),
                int(locked.coin.amount),
            )
        )
    else:
        children = clvm.spend_cats([sdk.CatSpend(locked.cat, exercise)])
        spends.add_cat(children[0])
    spends.add_option(option)


def _exercise_actions(
    clvm: Clvm,
    spends: Spends,
    option: OptionContract,
    underlying: OptionUnderlying,
    locked: Locked,
    fee: int,
) -> list[Action]:
    """Spend the locked coin through its exercise path and return the actions.

    Port of Sage's ``prepare_exercise``. The locked coin is spent to the offer
    settlement puzzle; that settlement coin is handed to the action system as an
    input, so the underlying asset flows on to the change address. The option is
    melted, which is what authorises the locked coin's spend, and the strike is
    paid to the creator as a notarized payment whose nonce is the launcher id.
    """
    sdk = _sdk()
    info = option.info
    _release_underlying(clvm, spends, option, underlying, locked)

    strike = underlying.strike_type
    creator = bytes(underlying.creator_puzzle_hash)
    as_xch = strike.to_xch()
    as_cat = strike.to_cat()
    if as_xch is not None:
        strike_id = sdk.Id.xch()
        payment = sdk.Payment(creator, int(as_xch.amount), None)
    elif as_cat is not None:
        strike_id = sdk.Id.existing(as_cat.asset_id)
        payment = sdk.Payment(creator, int(as_cat.amount), _hint(clvm, creator))
    else:
        raise OptionBuildError("only XCH and plain CAT strikes are supported")
    return [
        sdk.Action.fee(fee),
        sdk.Action.melt_singleton(sdk.Id.existing(info.launcher_id), int(option.coin.amount)),
        sdk.Action.settle(strike_id, sdk.NotarizedPayment(info.launcher_id, [payment])),
    ]


def build_exercise(
    option: OptionContract,
    underlying: OptionUnderlying,
    locked: Locked,
    funding: Funding,
    custody: Custody,
    *,
    fee: int = 0,
) -> list[CoinSpend]:
    """Exercise an option we own: pay the strike, receive the underlying.

    ``funding`` must cover the strike (and the fee). The underlying arrives at
    ``funding.change_puzzle_hash``. The chain refuses this after expiry.

    When we are also the creator the strike is paid to ourselves, which unwinds
    an unsold option, but :func:`build_cancel` does that without needing the
    strike amount on hand.
    """
    _check_fee(fee)
    _check_locked(option, underlying, locked)
    sdk = _sdk()
    clvm = sdk.Clvm()
    if not custody.knows(option.info.p2_puzzle_hash):
        raise OptionBuildError("the option's owner address has no key in custody")
    spends = _new_spends(clvm, funding, custody)
    try:
        actions = _exercise_actions(clvm, spends, option, underlying, locked, fee)
    except OptionBuildError:
        raise
    except Exception as exc:
        raise OptionBuildError(f"could not build the spend: {exc}") from exc
    _complete(clvm, spends, actions, custody)
    return clvm.coin_spends()


# ---------------------------------------------------------------------------
# Cancel (unwind an option we minted and still own)
# ---------------------------------------------------------------------------


def build_cancel(
    option: OptionContract,
    underlying: OptionUnderlying,
    locked: Locked,
    funding: Funding,
    custody: Custody,
    *,
    fee: int = 0,
) -> list[CoinSpend]:
    """Release the collateral of an option we created and still own, before expiry.

    This is an exercise in which we are both sides, so the strike never has to
    be funded. The strike payment is addressed to the creator, which is us; the
    coin it creates is spent again in the same bundle, and its value covers the
    coin that paid it. The chain only checks that a bundle balances as a whole,
    so one *seed* coin of the strike asset, of any amount, is enough to start
    the loop. The seed is returned in full.

    ``funding`` therefore needs one coin of the strike asset (its first XCH coin
    for an XCH strike, its first matching CAT for a CAT strike) and enough XCH
    for the fee. Everything lands at ``funding.change_puzzle_hash``.

    Sage does the same in ``cancel_options`` by rewriting the action system's
    internal state after the fact, which the Python bindings do not expose. Here
    the three spends of the loop are built directly and only the rest goes
    through the action system.

    Refused unless custody holds the creator's key: for anyone else the strike
    payment would be a real payment to a stranger.
    """
    _check_fee(fee)
    _check_locked(option, underlying, locked)
    sdk = _sdk()
    info = option.info
    creator = bytes(underlying.creator_puzzle_hash)
    change = bytes(funding.change_puzzle_hash)
    if not custody.has_key(creator):
        raise OptionBuildError(
            "only the option's creator can cancel it; anyone else must exercise and pay the strike"
        )
    if not custody.knows(info.p2_puzzle_hash):
        raise OptionBuildError("the option's owner address has no key in custody")
    if not custody.has_key(change):
        raise OptionBuildError("the change address has no key in custody")

    clvm = sdk.Clvm()
    settlement_hash = sdk.Constants.settlement_payment_hash()
    xch_funding = list(funding.xch)
    cat_funding = list(funding.cats)

    def tied() -> Program:
        # Each hand-built spend asserts the option coin is spent alongside it,
        # so no part of the loop can be included without the rest.
        return clvm.assert_concurrent_spend(option.coin.coin_id())

    strike = underlying.strike_type
    as_xch = strike.to_xch()
    as_cat = strike.to_cat()
    try:
        if as_xch is not None:
            amount = int(as_xch.amount)
            if not xch_funding:
                raise OptionBuildError("cancelling needs an XCH coin to seed the strike payment")
            seed = xch_funding.pop(0)
            if not custody.has_key(seed.puzzle_hash):
                raise OptionBuildError("the seed coin's address has no key in custody")
            payment = sdk.NotarizedPayment(info.launcher_id, [sdk.Payment(creator, amount, None)])
            settlement = sdk.Coin(seed.coin_id(), settlement_hash, amount)
            paid = sdk.Coin(settlement.coin_id(), creator, amount)
            returned = sdk.Coin(seed.coin_id(), change, int(seed.amount))
            clvm.spend_coin(
                seed,
                custody.spend(
                    clvm,
                    seed.puzzle_hash,
                    [
                        clvm.create_coin(settlement_hash, amount, None),
                        clvm.create_coin(change, int(seed.amount), None),
                        tied(),
                    ],
                ),
            )
            clvm.spend_settlement_coin(settlement, [payment])
            clvm.spend_coin(paid, custody.spend(clvm, creator, [tied()]))
            # The seed's value comes back as a coin the action system can use
            # for the fee and as the source of the melted option's change.
            xch_funding.insert(0, returned)
        elif as_cat is not None:
            amount = int(as_cat.amount)
            asset_id = bytes(as_cat.asset_id)
            index = next(
                (
                    position
                    for position, cat in enumerate(cat_funding)
                    if bytes(cat.info.asset_id) == asset_id
                ),
                None,
            )
            if index is None:
                raise OptionBuildError(
                    "cancelling needs a coin of the strike asset to seed the strike payment"
                )
            seed_cat = cat_funding.pop(index)
            if seed_cat.info.hidden_puzzle_hash is not None:
                raise OptionBuildError("revocable CATs are not supported")
            if not custody.has_key(seed_cat.info.p2_puzzle_hash):
                raise OptionBuildError("the seed coin's address has no key in custody")
            payment = sdk.NotarizedPayment(
                info.launcher_id, [sdk.Payment(creator, amount, _hint(clvm, creator))]
            )
            settlement_cat = seed_cat.child(settlement_hash, amount)
            paid_cat = settlement_cat.child(creator, amount)
            seed_conditions = [clvm.create_coin(settlement_hash, amount, None), tied()]
            if int(seed_cat.coin.amount) > 0:
                seed_conditions.append(
                    clvm.create_coin(change, int(seed_cat.coin.amount), _hint(clvm, change))
                )
            clvm.spend_cats(
                [
                    sdk.CatSpend(
                        seed_cat,
                        custody.spend(clvm, seed_cat.info.p2_puzzle_hash, seed_conditions),
                    ),
                    sdk.CatSpend(settlement_cat, clvm.settlement_spend([payment])),
                    sdk.CatSpend(paid_cat, custody.spend(clvm, creator, [tied()])),
                ]
            )
        else:
            raise OptionBuildError("only XCH and plain CAT strikes are supported")
    except OptionBuildError:
        raise
    except Exception as exc:
        raise OptionBuildError(f"could not build the spend: {exc}") from exc

    spends = _new_spends(
        clvm, Funding(change_puzzle_hash=change, xch=xch_funding, cats=cat_funding), custody
    )
    try:
        _release_underlying(clvm, spends, option, underlying, locked)
    except Exception as exc:
        raise OptionBuildError(f"could not build the spend: {exc}") from exc
    actions = [
        sdk.Action.fee(fee),
        sdk.Action.melt_singleton(sdk.Id.existing(info.launcher_id), int(option.coin.amount)),
    ]
    _complete(clvm, spends, actions, custody)
    return clvm.coin_spends()


# ---------------------------------------------------------------------------
# Clawback
# ---------------------------------------------------------------------------


def build_clawback(
    underlying: OptionUnderlying,
    locked: Locked,
    creator_key: PublicKey,
    funding: Funding,
    custody: Custody,
    *,
    fee: int = 0,
) -> list[CoinSpend]:
    """After expiry, return the locked coin to ``funding.change_puzzle_hash``.

    The spend asserts ``ASSERT_SECONDS_ABSOLUTE`` at the expiry, so the chain
    refuses it any earlier; nothing here consults a clock. For a CAT underlying
    a fee needs an XCH coin in ``funding``; for an XCH underlying the fee can
    come out of the locked coin itself.
    """
    _check_fee(fee)
    sdk = _sdk()
    expected = bytes(underlying.puzzle_hash())
    if locked.cat is None:
        actual = bytes(locked.coin.puzzle_hash)
    else:
        actual = bytes(locked.cat.info.p2_puzzle_hash)
    if actual != expected:
        raise OptionBuildError("the locked coin is not locked by these option terms")
    custody.add_expired_underlying(underlying, creator_key)

    clvm = sdk.Clvm()
    spends = _new_spends(clvm, funding, custody)
    if locked.cat is None:
        spends.add_xch(locked.coin)
    else:
        spends.add_cat(locked.cat)
    _complete(clvm, spends, [sdk.Action.fee(fee)], custody)
    return clvm.coin_spends()
