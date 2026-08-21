"""CLVM puzzle/solution builders for the warp.green USDC(Base) -> wUSDC.b claim.

This is a faithful port of warpdotgreen ``cli/drivers/portal.py`` +
``cli/drivers/wrapped_assets.py`` (the mint path) and warp-ui
``erc20bridge.tsx::mintCATs`` (the claim assembly), rebuilt on the thin
:mod:`gui.services.warp.clvm_utils` stack so the bot needs neither
``chia-blockchain`` nor Node. Nothing here evaluates CLVM -- every value is
*constructed* and hashed; the one place we read an existing on-chain puzzle
(the portal parent's lineage proof) is handled by the caller and passed in.

The claim is a five-spend bundle, pushed atomically:

1. **portal singleton** -- reveals the validator BLS set + a sig-switch bitmask;
   recreates itself (amount 1) and creates the message coin (amount 0).
2. **message coin** -- consumed by the minter coin, tying the attestation to it.
3. **cat_minter coin** -- a plain XCH coin of the post-tip amount whose puzzle
   authorises minting; emits the eve CAT.
4. **eve CAT** -- CAT v2 with the wrapped-asset TAIL revealed, paying the
   receiver puzzle hash the operator chose.
5. **security coin** -- an ephemeral-key coin the bot funds with
   ``post_tip + claim_fee``; it creates the minter coin, reserves the fee, and
   asserts the eve CAT is spent in the same bundle. Losing any race leaves funds
   stuck-not-lost: the attested message is claimable forever and we hold the
   ephemeral key, so :func:`build_sweep_bundle` recovers the funding coin.

Funds never move on a mismatch: :func:`build_claim_bundle` refuses to build
unless the reconstructed portal puzzle hash matches the on-chain portal coin,
the derived cat_minter hash matches the attested ``destination``, and the
derived wrapped-TAIL matches the configured asset id.

``chia_rs`` (BLS) is imported lazily inside the few functions that sign or
aggregate, so importing this module costs nothing until a claim is actually
built.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

from . import clvm_utils as cu
from .constants import WarpNet

SExp = cu.SExp

# --------------------------------------------------------------------------- #
# Vendored puzzle reveals (hash-asserted on load) + their tree hashes.
# --------------------------------------------------------------------------- #

SINGLETON_MOD = cu.load_puzzle("singleton_top_layer_v1_1")
SINGLETON_MOD_HASH = cu.puzzle_hash("singleton_top_layer_v1_1")
SINGLETON_LAUNCHER_HASH = cu.SINGLETON_LAUNCHER_PUZZLE_HASH

CAT_MOD = cu.load_puzzle("cat_v2")
CAT_MOD_HASH = cu.puzzle_hash("cat_v2")

MESSAGE_COIN_MOD = cu.load_puzzle("message_coin")
PORTAL_RECEIVER_MOD = cu.load_puzzle("portal_receiver")

WRAPPED_TAIL_MOD = cu.load_puzzle("wrapped_tail")
CAT_MINTER_MOD = cu.load_puzzle("cat_minter")
CAT_MINT_AND_PAYOUT_MOD = cu.load_puzzle("cat_mint_and_payout")
CAT_MINT_AND_PAYOUT_MOD_HASH = cu.puzzle_hash("cat_mint_and_payout")
CAT_BURNER_MOD = cu.load_puzzle("cat_burner")
BURN_INNER_PUZZLE_MOD = cu.load_puzzle("burn_inner_puzzle")
BURN_INNER_PUZZLE_MOD_HASH = cu.puzzle_hash("burn_inner_puzzle")

BRIDGING_PUZZLE = cu.load_puzzle("bridging_puzzle")
BRIDGING_PUZZLE_HASH = cu.puzzle_hash("bridging_puzzle")

P2_DELEGATED_MOD = cu.load_puzzle("p2_delegated_puzzle_or_hidden_puzzle")
P2_M_OF_N_MOD = cu.load_puzzle("p2_m_of_n_delegate_direct")

# CLVM condition opcodes we emit.
CREATE_COIN = 51
RESERVE_FEE = 52
ASSERT_CONCURRENT_SPEND = 0x40  # 64


class WarpDriverError(RuntimeError):
    """A claim/sweep bundle could not be built safely (fail-closed anchor)."""


# --------------------------------------------------------------------------- #
# Value types.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class Coin:
    """A Chia coin, bytes-native (drivers work in bytes; coinset speaks hex)."""

    parent_coin_info: bytes
    puzzle_hash: bytes
    amount: int

    def name(self) -> bytes:
        return cu.coin_name(self.parent_coin_info, self.puzzle_hash, self.amount)

    def as_program(self) -> SExp:
        return SExp.to([self.parent_coin_info, self.puzzle_hash, self.amount])


@dataclass(frozen=True)
class CoinSpend:
    coin: Coin
    puzzle_reveal: object  # SExp
    solution: object       # SExp

    def to_json(self) -> dict:
        return {
            "coin": {
                "parent_coin_info": "0x" + self.coin.parent_coin_info.hex(),
                "puzzle_hash": "0x" + self.coin.puzzle_hash.hex(),
                "amount": self.coin.amount,
            },
            "puzzle_reveal": "0x" + cu.program_to_bytes(self.puzzle_reveal).hex(),
            "solution": "0x" + cu.program_to_bytes(self.solution).hex(),
        }


@dataclass(frozen=True)
class SpendBundle:
    coin_spends: List[CoinSpend]
    aggregated_signature: bytes

    def to_json(self) -> dict:
        return {
            "coin_spends": [cs.to_json() for cs in self.coin_spends],
            "aggregated_signature": "0x" + self.aggregated_signature.hex(),
        }


@dataclass(frozen=True)
class ClaimRequest:
    """Everything needed to build a claim, resolved from chain by ``claim.py``."""

    net: WarpNet
    # Portal singleton (current unspent) + its lineage.
    portal_coin: Coin
    portal_parent_parent_info: bytes
    portal_parent_inner_puzzle_hash: Optional[bytes]  # None => parent is launcher (eve)
    last_chains_and_nonces: Sequence[Tuple[bytes, bytes]]
    # Validator attestation.
    validator_sigs: Sequence[bytes]  # 96-byte G2, one per set bit in sig_switches
    sig_switches: Sequence[bool]     # index-ordered over the curried validator set
    # Attested message.
    nonce: bytes
    source: bytes                    # ERC20 bridge address (leading zeros stripped internally)
    destination: bytes               # attested message.destination == cat_minter puzzle hash
    contents: Sequence[bytes]        # [erc20_contract_b32, receiver_ph, amount_b32]
    # Funding.
    security_coin: Coin              # bot-funded ephemeral coin, amount == post_tip + claim_fee
    ephemeral_sk: bytes              # 32-byte BLS private key controlling security_coin
    claim_fee: int
    # The wrapped-CAT id THIS job expects, so the anchor below checks the
    # asset the job was created for rather than a single global constant.
    # Empty keeps the legacy behaviour (the network's USDC anchor).
    expected_asset_id: str = ""


@dataclass(frozen=True)
class ClaimBundle:
    bundle: SpendBundle
    message_coin_id: bytes
    minter_coin_id: bytes
    eve_cat_coin_id: bytes
    final_cat_coin_id: bytes  # the coin the receiver ends up owning; poll this for COMPLETED
    security_coin_id: bytes


# --------------------------------------------------------------------------- #
# Small helpers.
# --------------------------------------------------------------------------- #

def _strip_source(source: bytes) -> bytes:
    """Strip leading 0x00 bytes -- the 32-byte-padded EVM address -> 20 bytes.

    Matches ``xch_follower.signMessage`` and warp-ui's inner-solution builder;
    for the mainnet Base bridge (``0x8412..``) there are no leading zeros so this
    is a no-op, but it keeps the digest, message-coin, cat_minter, and portal
    inner solution all committed to an identical source.
    """
    i = 0
    while i < len(source) and source[i] == 0:
        i += 1
    return source[i:]


def _as_keys(pubkeys: Sequence) -> list:
    return [bytes.fromhex(k) if isinstance(k, str) else bytes(k) for k in pubkeys]


def _tree_hashes():
    """Pre-hashed atoms used by :func:`curry_hashes`; cheap, computed once."""
    return None


# --- puzzle-hash-only currying (predict a coin id from an inner puzzle hash) - #

_NULL_H = cu.sha256(b"\x01")           # sha256tree(())
_Q_H = cu.sha256(b"\x01" + b"\x01")    # sha256tree(1) == quote atom
_A_H = cu.sha256(b"\x01" + b"\x02")    # sha256tree(2) == apply atom
_C_H = cu.sha256(b"\x01" + b"\x04")    # sha256tree(4) == cons atom
_ONE_H = _Q_H                          # environment terminator is the atom 1


def _h_atom(atom: bytes) -> bytes:
    return cu.sha256(b"\x01" + atom)


def _h_pair(left: bytes, right: bytes) -> bytes:
    return cu.sha256(b"\x02" + left + right)


def curry_hashes(mod_hash: bytes, *arg_hashes: bytes) -> bytes:
    """Tree hash of ``curry(mod, *args)`` given only the arg *tree hashes*.

    Lets us predict the final receiver-CAT coin id from the receiver's puzzle
    hash alone (we never see the receiver's full puzzle). Verified against
    :func:`curry` + :func:`clvm_utils.sha256tree` in the driver tests.
    """
    fixed = _ONE_H
    for arg_hash in reversed(arg_hashes):
        quoted = _h_pair(_Q_H, arg_hash)
        fixed = _h_pair(_C_H, _h_pair(quoted, _h_pair(fixed, _NULL_H)))
    quoted_mod = _h_pair(_Q_H, mod_hash)
    return _h_pair(_A_H, _h_pair(quoted_mod, _h_pair(fixed, _NULL_H)))


# --------------------------------------------------------------------------- #
# Singleton (port of chia singleton_top_layer_v1_1 curry/solution).
# --------------------------------------------------------------------------- #

def _singleton_struct(launcher_id: bytes):
    return (SINGLETON_MOD_HASH, (launcher_id, SINGLETON_LAUNCHER_HASH))


def puzzle_for_singleton(launcher_id: bytes, inner_puzzle) -> SExp:
    return cu.curry(SINGLETON_MOD, _singleton_struct(launcher_id), inner_puzzle)


def make_lineage_proof(
    parent_parent_info: bytes,
    parent_inner_puzzle_hash: Optional[bytes],
    parent_amount: int = 1,
) -> list:
    """Singleton lineage proof: 3-tuple for a normal parent, 2-tuple for the eve.

    ``parent_inner_puzzle_hash is None`` means the parent is the launcher itself.
    """
    if parent_inner_puzzle_hash is None:
        return [parent_parent_info, parent_amount]
    return [parent_parent_info, parent_inner_puzzle_hash, parent_amount]


def solution_for_singleton(lineage_proof, amount: int, inner_solution) -> SExp:
    return SExp.to([lineage_proof, amount, inner_solution])


# --------------------------------------------------------------------------- #
# Message coin (drivers/portal.py).
# --------------------------------------------------------------------------- #

def get_message_coin_puzzle_1st_curry(launcher_id: bytes) -> SExp:
    return cu.curry(MESSAGE_COIN_MOD, _singleton_struct(launcher_id))


def get_message_coin_puzzle(
    launcher_id: bytes,
    source_chain: bytes,
    source: bytes,
    nonce: bytes,
    destination: bytes,
    message_hash: bytes,
) -> SExp:
    return cu.curry(
        get_message_coin_puzzle_1st_curry(launcher_id),
        (source_chain, nonce),
        _strip_source(source),
        destination,
        message_hash,
    )


def get_message_coin_solution(
    receiver_coin: Coin,
    parent_parent_info: bytes,
    parent_inner_puzzle_hash: bytes,
    message_coin_id: bytes,
) -> SExp:
    return SExp.to([
        (receiver_coin.parent_coin_info, receiver_coin.amount),
        (parent_parent_info, parent_inner_puzzle_hash),
        message_coin_id,
    ])


# --------------------------------------------------------------------------- #
# Portal receiver (drivers/portal.py + drivers/multisig.py).
# --------------------------------------------------------------------------- #

def get_mofn_delegate_direct_puzzle(threshold: int, pubkeys: Sequence) -> SExp:
    return cu.curry(P2_M_OF_N_MOD, cu.int_to_bytes(threshold), _as_keys(pubkeys))


def get_portal_update_puzzle_hash(net: WarpNet) -> bytes:
    """Tree hash of the portal's multisig update puzzle (curried into receiver)."""
    return cu.sha256tree(
        get_mofn_delegate_direct_puzzle(net.multisig_threshold, net.multisig_bls_keys)
    )


def get_portal_receiver_inner_puzzle(
    launcher_id: bytes,
    signature_threshold: int,
    signature_pubkeys: Sequence,
    update_puzzle_hash: bytes,
    last_chains_and_nonces: Sequence[Tuple[bytes, bytes]] = (),
) -> SExp:
    first_curry = cu.curry(
        PORTAL_RECEIVER_MOD,
        (signature_threshold, _as_keys(signature_pubkeys)),  # VALIDATOR_INFO
        cu.sha256tree(get_message_coin_puzzle_1st_curry(launcher_id)),
        update_puzzle_hash,
    )
    return cu.curry(
        first_curry,
        cu.sha256tree(first_curry),  # SELF_HASH
        [(chain, nonce) for chain, nonce in last_chains_and_nonces],
    )


def get_sigs_switch(sig_switches: Sequence[bool]) -> int:
    """Bitmask of which curried validators signed (index 0 = least-significant)."""
    if not sig_switches:
        return 0
    return int("".join("1" if x else "0" for x in sig_switches)[::-1], 2)


def get_portal_receiver_inner_solution(
    source_chain: bytes,
    nonce: bytes,
    source: bytes,
    destination: bytes,
    contents: Sequence[bytes],
    sigs_switch: int,
) -> SExp:
    """Single-message portal spend: recreate self (1) + create message coin (0).

    warp's relay batches multiple pending messages here; a bot claim always has
    exactly one, so this builds the one-message form.
    """
    return SExp.to([
        0,  # no update puzzle
        [(source_chain, nonce)],
        [[sigs_switch, _strip_source(source), destination, list(contents)]],
    ])


# --------------------------------------------------------------------------- #
# Validator signed digest (xch_follower.signMessage).
# --------------------------------------------------------------------------- #

def get_validator_message_digest(
    source_chain: bytes,
    nonce: bytes,
    source: bytes,
    destination: bytes,
    contents: Sequence[bytes],
    portal_coin_id: bytes,
    agg_sig_extra_data: bytes,
) -> bytes:
    """96-byte digest each validator BLS-signs (AGG_SIG_UNSAFE in the portal).

    ``sha256tree([source_chain, nonce, source_stripped, destination, contents])
    + portal_coin_id + agg_sig_extra_data``. Keyed to the *current* portal coin
    id, so when the portal advances the sigs must be recollected.
    """
    tree = cu.sha256tree(SExp.to([
        source_chain,
        nonce,
        _strip_source(source),
        destination,
        list(contents),
    ]))
    return tree + portal_coin_id + agg_sig_extra_data


# --------------------------------------------------------------------------- #
# CAT minter + wrapped TAIL (drivers/wrapped_assets.py).
# --------------------------------------------------------------------------- #

def get_cat_burner_puzzle(destination_chain: bytes, destination: bytes) -> SExp:
    return cu.curry(
        CAT_BURNER_MOD,
        CAT_MOD_HASH,
        BURN_INNER_PUZZLE_MOD_HASH,
        BRIDGING_PUZZLE_HASH,
        destination_chain,
        _strip_source(destination),
    )


def get_cat_minter_puzzle(launcher_id: bytes, source_chain: bytes, source: bytes) -> SExp:
    source = _strip_source(source)
    return cu.curry(
        CAT_MINTER_MOD,
        cu.sha256tree(get_message_coin_puzzle_1st_curry(launcher_id)),
        CAT_MOD_HASH,
        cu.sha256tree(WRAPPED_TAIL_MOD),
        CAT_MINT_AND_PAYOUT_MOD_HASH,
        cu.raw_hash(b"\x01", cu.sha256tree(get_cat_burner_puzzle(source_chain, source))),
        BURN_INNER_PUZZLE_MOD_HASH,
        source_chain,
        source,
    )


def get_cat_burn_inner_puzzle_first_curry(
    destination_chain: bytes,
    destination: bytes,
    source_chain_token_contract_address: bytes,
) -> SExp:
    return cu.curry(
        BURN_INNER_PUZZLE_MOD,
        cu.sha256tree(get_cat_burner_puzzle(destination_chain, destination)),
        source_chain_token_contract_address,
    )


def get_wrapped_tail(
    launcher_id: bytes,
    source_chain: bytes,
    source: bytes,
    source_chain_token_contract_address: bytes,
) -> SExp:
    source = _strip_source(source)
    token = source_chain_token_contract_address
    if len(token) < 32:
        token = b"\x00" * (32 - len(token)) + token
    return cu.curry(
        WRAPPED_TAIL_MOD,
        cu.sha256tree(get_cat_minter_puzzle(launcher_id, source_chain, source)),
        cu.sha256tree(get_cat_burn_inner_puzzle_first_curry(source_chain, source, token)),
    )


def derive_wrapped_asset_id(net: WarpNet, token_address: str = "") -> bytes:
    """Re-derive the wrapped-asset (CAT) id for this deployment, fully offline.

    ``token_address`` selects which ERC-20's wrapped TAIL to derive; empty
    keeps the legacy USDC behaviour. Per-asset ids live in ``net.assets``.

    Everything the TAIL commits to is a fixed deployment constant -- the portal
    launcher id, the warp source chain, the Base ERC20Bridge address, and the
    source token contract -- so this needs no chain reads, no attestation, and
    no job. That is what lets :func:`verify_wrapped_asset_anchor` run at engine
    construction, *before* a single client is built or a single wei moves.

    Identical by construction to the derivation inside
    :func:`build_claim_bundle`: there, ``erc20_contract`` comes from the attested
    ``contents[0]`` (32-byte padded) rather than from ``net``, and
    :func:`get_wrapped_tail` left-pads a short token to 32 bytes, so a 20-byte
    ``net.usdc_address`` and the padded attested value curry to the same TAIL.
    ``_h_message_sent`` separately anchors that the attested source token equals
    ``net.usdc_address``, so the two can never diverge unnoticed.
    """
    token = (token_address or net.usdc_address).strip()
    if token[:2] in ("0x", "0X"):
        token = token[2:]
    # bytes.fromhex() tolerates ASCII whitespace between byte pairs, so a
    # mangled "f2 D5 ..." would decode to 20 bytes here yet be invalid for
    # every later EVM call -- require exactly 40 hex digits, nothing else.
    if len(token) != 40 or any(
        c not in "0123456789abcdefABCDEF" for c in token
    ):
        raise WarpDriverError(
            f"malformed ERC-20 token address {token_address!r}: expected "
            "exactly 40 hex digits (plus optional 0x prefix)"
        )
    token_bytes = bytes.fromhex(token)
    tail = get_wrapped_tail(
        bytes.fromhex(net.portal_launcher_id),
        net.source_chain.encode(),
        bytes.fromhex(net.erc20_bridge_address[2:]),
        token_bytes,
    )
    return cu.sha256tree(tail)


# --------------------------------------------------------------------------- #
# Burn bundle (unwrap: wUSDC.b -> USDC; drivers/wrapped_assets.py burn path).
#
# Every structural fact here was pinned by evaluating the vendored bytecode
# and replaying a real mainnet unwrap (the fixture in
# tests/fixtures_unwrap.json): curry orders, solution arities, the
# announcement interlock, and the offline nonce.
# --------------------------------------------------------------------------- #

def _pad_token(token: bytes) -> bytes:
    """Left-pad the source token contract to 32 bytes.

    The burn inner puzzle's first curry commits the 32-byte form -- proven by
    uncurrying the real burn CAT's inner puzzle, whose curried argument equals
    the padded hash and NOT the 20-byte variant. Mirrors get_wrapped_tail.
    """
    if len(token) < 32:
        return b"\x00" * (32 - len(token)) + token
    return token


def get_cat_burn_inner_puzzle(
    destination_chain: bytes,
    destination: bytes,
    source_chain_token_contract_address: bytes,
    receiver: bytes,
    bridge_toll_mojos: int,
) -> SExp:
    """The burn CAT's inner puzzle: second curry over the first.

    ``receiver`` is the Base address the released USDC pays -- curried in, so
    even a third party who pushes the burn pays *us*. ``bridge_toll_mojos``
    (BRIDGE_FEE upstream) must equal the cat_burner coin's amount: the
    announcement interlock computes the cat_burner coin id with it, so a
    mismatch makes the bundle structurally unspendable.
    """
    first = get_cat_burn_inner_puzzle_first_curry(
        destination_chain,
        destination,
        _pad_token(source_chain_token_contract_address),
    )
    return cu.curry(first, receiver, bridge_toll_mojos)


def get_burn_inner_puzzle_solution(
    cat_burner_parent_id: bytes,
    my_coin_id: bytes,
    tail_reveal,
) -> SExp:
    """Arity 3: (cat_burner_parent_id, my_coin_id, tail_reveal).

    ``cat_burner_parent_id`` is the SECURITY coin's id (the cat_burner coin's
    parent), NOT the source CAT's -- the two parent fields in this bundle are
    an easy swap and a swapped pair builds an unspendable bundle (announcement
    mismatch). The vendored bytecode raises on a non-32-byte parent id.
    """
    if len(cat_burner_parent_id) != 32:
        raise WarpDriverError(
            f"cat_burner parent id must be 32 bytes, got {len(cat_burner_parent_id)}"
        )
    if len(my_coin_id) != 32:
        raise WarpDriverError(f"my_coin_id must be 32 bytes, got {len(my_coin_id)}")
    return SExp.to([cat_burner_parent_id, my_coin_id, tail_reveal])


def get_cat_burner_puzzle_solution(
    cat_parent_info: bytes,
    tail_hash: bytes,
    cat_amount: int,
    source_chain_token_contract_address: bytes,
    receiver: bytes,
    my_coin: Coin,
) -> SExp:
    """Arity 8; the second field is the PRE-HASHED ``sha256(0x01 || tail_hash)``.

    ``cat_parent_info`` here is the SOURCE CAT coin's id (the burn CAT's
    parent) -- the mirror of the swap hazard documented on the burn inner
    solution. The puzzle re-derives the burn CAT's full coin id from these
    fields via the same curry-hash chain as :func:`curry_hashes`, so any
    disagreement with the coin actually on chain fails the announcement.
    """
    return get_cat_burner_puzzle_solution_prehashed(
        cat_parent_info,
        cu.raw_hash(b"\x01", tail_hash),
        cat_amount,
        source_chain_token_contract_address,
        receiver,
        my_coin,
    )


def get_cat_burner_puzzle_solution_prehashed(
    cat_parent_info: bytes,
    tail_hash_treehash: bytes,
    cat_amount: int,
    source_chain_token_contract_address: bytes,
    receiver: bytes,
    my_coin: Coin,
) -> SExp:
    """As above, with the TAIL's tree hash supplied directly.

    The puzzle only ever sees ``sha256(0x01 || tail_hash)``; splitting the
    builder lets the golden tests reproduce a real third-party unwrap whose
    TAIL preimage we do not hold.
    """
    return SExp.to([
        cat_parent_info,
        tail_hash_treehash,
        cat_amount,
        _pad_token(source_chain_token_contract_address),
        receiver,
        my_coin.amount,
        my_coin.puzzle_hash,
        my_coin.name(),
    ])


@dataclass(frozen=True)
class BurnRequest:
    """Everything :func:`build_burn_bundle` needs, resolved by the caller."""

    net: object                       # WarpNet
    burn_cat_coin: Coin               # the coin cat_spend created at the burn inner PH
    cat_lineage_proof: object         # CAT lineage proof list for burn_cat_coin's parent
    security_coin: Coin               # ephemeral toll coin (amount == chia_toll_mojos)
    security_sk: bytes                # its raw BLS secret key
    receiver: bytes                   # 20-byte Base address the USDC pays


@dataclass(frozen=True)
class BurnBundle:
    bundle: SpendBundle
    nonce: bytes                      # the bridging coin's id, computed OFFLINE
    cat_burner_coin: Coin
    bridging_coin: Coin


def expected_burn_inner_puzzle_hash(net, receiver: bytes) -> bytes:
    """Where ``cat_spend`` must deliver the wUSDC.b -- THE commit point.

    Once a coin sits at this inner puzzle hash its only possible future is
    destruction into a message paying ``receiver``. Every gate runs before
    the spend that targets this.
    """
    inner = get_cat_burn_inner_puzzle(
        b"bse",
        bytes.fromhex(net.erc20_bridge_address[2:]),
        bytes.fromhex(net.usdc_address[2:]),
        receiver,
        net.chia_toll_mojos,
    )
    return cu.sha256tree(inner)


def expected_burn_cat_puzzle_hash(net, receiver: bytes) -> bytes:
    """The full cat_v2 outer hash the burn CAT sits at -- what coin scans match."""
    inner = get_cat_burn_inner_puzzle(
        b"bse",
        bytes.fromhex(net.erc20_bridge_address[2:]),
        bytes.fromhex(net.usdc_address[2:]),
        receiver,
        net.chia_toll_mojos,
    )
    return cu.sha256tree(
        construct_cat_puzzle(bytes.fromhex(net.expected_asset_id), inner)
    )


def outbound_message_contents(net, receiver: bytes, amount_mojos: int) -> list:
    """The three 32-byte atoms the validators sign for an unwrap.

    Reproduces the validator's memo padding exactly: each Chia memo atom is
    left-padded to 32 bytes, and the amount atom is CLVM minimal-int encoded
    before padding -- ``SExp.to`` gives the canonical form, so a value with
    the high bit set carries its leading zero byte the same way the chain
    serialized it.
    """
    def pad32(b: bytes) -> bytes:
        if len(b) > 32:
            raise WarpDriverError(f"memo atom is {len(b)} bytes, over 32")
        return b"\x00" * (32 - len(b)) + b

    amount_atom = SExp.to(int(amount_mojos)).atom or b""
    return [
        pad32(bytes.fromhex(net.usdc_address[2:])),
        pad32(receiver),
        pad32(amount_atom),
    ]


def build_burn_bundle(req: BurnRequest) -> BurnBundle:
    """Assemble the four-spend burn: security, burn CAT, cat_burner, bridging.

    The doc's five-spend table counts the daemon's own ``cat_spend`` -- that
    one has already happened by the time this runs, and is the irreversible
    step. This bundle is re-buildable and re-pushable at will: the nonce (the
    bridging coin's id) is a pure function of its inputs, so a resumed job
    recomputes the identical bundle.
    """
    net = req.net
    toll = int(net.chia_toll_mojos)
    if req.security_coin.amount != toll:
        raise WarpDriverError(
            f"security coin amount {req.security_coin.amount} != toll {toll}"
        )
    if len(req.receiver) != 20:
        raise WarpDriverError(f"receiver must be 20 bytes, got {len(req.receiver)}")

    bridge20 = bytes.fromhex(net.erc20_bridge_address[2:])
    token20 = bytes.fromhex(net.usdc_address[2:])
    tail_hash = bytes.fromhex(net.expected_asset_id)

    # The burn CAT: outer = cat_v2(tail, burn inner). Verify the coin we were
    # handed actually sits at the puzzle this bundle is about to reveal --
    # a mismatch means the cat_spend went somewhere else entirely.
    burn_inner = get_cat_burn_inner_puzzle(b"bse", bridge20, token20, req.receiver, toll)
    burn_cat_puzzle = construct_cat_puzzle(tail_hash, burn_inner)
    if cu.sha256tree(burn_cat_puzzle) != req.burn_cat_coin.puzzle_hash:
        raise WarpDriverError(
            "burn CAT coin is not at the expected cat_v2(burn inner) puzzle hash"
        )

    # The cat_burner coin is created by the security coin; the bridging coin
    # by the cat_burner. Both ids -- and therefore the message nonce -- are
    # computable before anything is pushed.
    burn_ph = bytes.fromhex(net.burn_puzzle_hash)
    cat_burner_coin = Coin(req.security_coin.name(), burn_ph, toll)
    bridging_ph = bytes.fromhex(net.bridging_puzzle_hash)
    bridging_coin = Coin(cat_burner_coin.name(), bridging_ph, toll)

    # 1. Security spend: fund the cat_burner coin, and refuse to be spent in
    # any block that does not also spend it (the toll cannot be stranded).
    conditions = [
        [CREATE_COIN, burn_ph, toll],
        [ASSERT_CONCURRENT_SPEND, cat_burner_coin.name()],
    ]
    security_spend, sig = build_security_coin_spend(
        req.security_coin,
        conditions,
        req.security_sk,
        bytes.fromhex(net.agg_sig_extra_data),
    )

    # 2. Burn CAT spend: ring of one, extra_delta = -amount (the CAT-v2 magic
    # that lets supply shrink), no signature -- the burn inner is announcement
    # -locked to the cat_burner, not key-locked.
    tail_reveal = get_wrapped_tail(
        bytes.fromhex(net.portal_launcher_id),
        net.source_chain.encode(),
        bridge20,
        token20,
    )
    inner_solution = get_burn_inner_puzzle_solution(
        req.security_coin.name(),          # cat_burner's parent == security coin
        req.burn_cat_coin.name(),
        tail_reveal,
    )
    burn_cat_spend = CoinSpend(
        req.burn_cat_coin,
        burn_cat_puzzle,
        get_cat_solution(
            inner_solution,
            req.cat_lineage_proof,
            req.burn_cat_coin.name(),      # ring of one: prev == this == next
            req.burn_cat_coin,
            req.burn_cat_coin,
            0,
            -req.burn_cat_coin.amount,
        ),
    )

    # 3. cat_burner spend: emits the message (the bridging coin) and
    # re-derives the burn CAT's id from scratch -- contents cannot disagree
    # with the coin actually burned.
    cat_burner_spend = CoinSpend(
        cat_burner_coin,
        get_cat_burner_puzzle(b"bse", bridge20),
        get_cat_burner_puzzle_solution(
            req.burn_cat_coin.parent_coin_info,   # the SOURCE CAT's id
            tail_hash,
            req.burn_cat_coin.amount,
            token20,
            req.receiver,
            cat_burner_coin,
        ),
    )

    # 4. Bridging spend: ASSERT_MY_AMOUNT + RESERVE_FEE my_amount -- the toll
    # becomes the bundle's mempool fee and is burned to farmers.
    bridging_spend = CoinSpend(
        bridging_coin,
        BRIDGING_PUZZLE,
        SExp.to([toll]),
    )

    return BurnBundle(
        bundle=SpendBundle(
            coin_spends=[security_spend, burn_cat_spend, cat_burner_spend, bridging_spend],
            aggregated_signature=aggregate_sigs([sig]),
        ),
        nonce=bridging_coin.name(),
        cat_burner_coin=cat_burner_coin,
        bridging_coin=bridging_coin,
    )


def verify_wrapped_asset_anchor(net: WarpNet, configured: str = "") -> bytes:
    """Refuse-to-start anchor: the derived asset id must match what is expected.

    Checks the derivation against :attr:`WarpNet.expected_asset_id` and, when the
    operator pinned one in ``config.yaml``, against ``warp.expected_asset_id``
    too. Raises :class:`WarpDriverError` on any mismatch; returns the derived id.

    This is the check the runbook has always promised ("before any funds move").
    It used to live only in :func:`build_claim_bundle`, which runs during
    ``CLAIMING`` -- after the Base approve, after ``bridgeToChia``, and after the
    Chia funding send. Running it here makes a bad constant or a typo'd config a
    *blocked engine with a banner*, not three irreversible broadcasts.
    """
    if not net.expected_asset_id:
        raise WarpDriverError(
            f"{net.name}.expected_asset_id is empty; refusing to run without the "
            "wrapped-asset anchor"
        )
    derived = derive_wrapped_asset_id(net)
    if derived.hex() != net.expected_asset_id:
        raise WarpDriverError(
            f"derived wrapped-asset id {derived.hex()} != {net.name} anchor "
            f"{net.expected_asset_id}; the deployment constants are wrong or "
            "warp.green redeployed -- refusing to move funds"
        )
    # Every listed bridgeable asset must derive to its pinned id -- the same
    # refuse-to-start discipline, per asset, so a typo in a NEW asset entry
    # can never survive to a live claim or burn.
    # The table key is an alias for humans; identity is the wrapped id
    # (jobs resolve by it), so a key need not equal its descriptor symbol.
    # That makes uniqueness of the wrapped id load-bearing: two entries
    # pinning one id would each pass the per-entry checks below, while
    # lookups returned whichever came first -- so a job stamped from the
    # other would resolve to a descriptor it never froze and die with
    # "terms changed".
    seen_ids: dict = {}
    for key, spec in getattr(net, "assets", ()) or ():
        wid = (spec.expected_asset_id or "").strip().lower()
        if wid and wid in seen_ids:
            raise WarpDriverError(
                f"assets {seen_ids[wid]!r} and {key!r} both pin wrapped id "
                f"{wid}; the table is ambiguous and a job stamped from one "
                "could resolve to the other"
            )
        if wid:
            seen_ids[wid] = key
        if not spec.expected_asset_id:
            raise WarpDriverError(
                f"{net.name} asset {key} has an empty expected_asset_id; "
                "refusing to run without its wrapped-asset anchor"
            )
        if not str(spec.erc20_address or "").strip():
            raise WarpDriverError(
                f"asset {key} has an empty erc20_address; an empty address "
                "would silently derive against the legacy USDC default"
            )
        if int(spec.erc20_decimals) < int(net.cat_decimals):
            raise WarpDriverError(
                f"asset {key} has {spec.erc20_decimals} decimals, fewer than "
                f"the {net.cat_decimals}-decimal wrapped CAT: amounts could "
                "not round-trip and the mojo factor would not be an integer"
            )
        try:
            asset_derived = derive_wrapped_asset_id(net, spec.erc20_address)
        except WarpDriverError as exc:
            raise WarpDriverError(f"asset {key}: {exc}") from None
        if asset_derived.hex() != spec.expected_asset_id:
            raise WarpDriverError(
                f"asset {key}: derived wrapped id {asset_derived.hex()} != "
                f"pinned {spec.expected_asset_id}; the asset entry is wrong "
                "or warp.green redeployed -- refusing to move funds"
            )
    pinned = (configured or "").strip().lower()
    if pinned:
        if pinned.startswith("0x"):
            pinned = pinned[2:]
        if pinned != derived.hex():
            raise WarpDriverError(
                f"configured warp.expected_asset_id {pinned} != derived "
                f"{derived.hex()}; fix config.yaml (the Base-bridged wUSDC.b id "
                "is fa4a180ac326e67ea289b869e3448256f6af05721f7cf934cb9901baa6b7a99d)"
            )
    return derived


def get_cat_minter_puzzle_solution(
    nonce: bytes,
    message,
    my_puzzle_hash: bytes,
    my_coin_id: bytes,
    message_coin_parent_info: bytes,
) -> SExp:
    return SExp.to([nonce, message, my_puzzle_hash, my_coin_id, message_coin_parent_info])


def get_cat_mint_and_payout_inner_puzzle(receiver: bytes) -> SExp:
    return cu.curry(CAT_MINT_AND_PAYOUT_MOD, receiver)


def get_cat_mint_and_payout_inner_puzzle_solution(
    tail_puzzle,
    my_amount: int,
    parent_parent_info: bytes,
) -> SExp:
    return SExp.to([tail_puzzle, my_amount, parent_parent_info])


# --------------------------------------------------------------------------- #
# CAT v2 puzzle + solution (chia cat_utils / warp-ui cat.tsx).
# --------------------------------------------------------------------------- #

def construct_cat_puzzle(tail_hash: bytes, inner_puzzle) -> SExp:
    return cu.curry(CAT_MOD, CAT_MOD_HASH, tail_hash, inner_puzzle)


def get_cat_solution(
    inner_solution,
    lineage_proof,
    prev_coin_id: bytes,
    this_coin: Coin,
    next_coin_proof: Coin,
    prev_subtotal: int,
    extra_delta: int,
) -> SExp:
    return SExp.to([
        inner_solution,
        lineage_proof if lineage_proof is not None else 0,
        prev_coin_id,
        this_coin.as_program(),
        next_coin_proof.as_program(),
        prev_subtotal,
        extra_delta,
    ])


# --------------------------------------------------------------------------- #
# Security coin (raw ephemeral key over p2_delegated_puzzle_or_hidden_puzzle).
# --------------------------------------------------------------------------- #

def p2_puzzle_for_pk(pubkey: bytes) -> SExp:
    """Standard p2 puzzle curried with a *raw* (non-synthetic) BLS pubkey.

    The security key is single-use and app-generated, so we curry the raw pubkey
    and sign with the raw secret key -- no synthetic-offset derivation needed.
    """
    return cu.curry(P2_DELEGATED_MOD, pubkey)


def standard_coin_solution(conditions: Sequence) -> SExp:
    """``(() (q . conditions) ())`` -- the public-key branch of the p2 puzzle."""
    delegated_puzzle = SExp.to((1, list(conditions)))
    return SExp.to([[], delegated_puzzle, []])


def _sign_p2_delegated(conditions, coin_name: bytes, sk_bytes: bytes, agg_sig_extra_data: bytes):
    from chia_rs import AugSchemeMPL, PrivateKey

    sk = PrivateKey.from_bytes(sk_bytes)
    delegated_puzzle = SExp.to((1, list(conditions)))
    data = cu.sha256tree(delegated_puzzle) + coin_name + agg_sig_extra_data
    return bytes(AugSchemeMPL.sign(sk, data)), bytes(sk.get_g1())


def build_security_coin_spend(
    security_coin: Coin,
    conditions: Sequence,
    sk_bytes: bytes,
    agg_sig_extra_data: bytes,
) -> Tuple[CoinSpend, bytes]:
    """Spend the ephemeral funding coin; returns (CoinSpend, 96-byte sig)."""
    sig, pubkey = _sign_p2_delegated(
        conditions, security_coin.name(), sk_bytes, agg_sig_extra_data
    )
    puzzle = p2_puzzle_for_pk(pubkey)
    if cu.sha256tree(puzzle) != security_coin.puzzle_hash:
        raise WarpDriverError(
            "security coin puzzle hash does not match the ephemeral key"
        )
    return CoinSpend(security_coin, puzzle, standard_coin_solution(conditions)), sig


def aggregate_sigs(sig_bytes_list: Sequence[bytes]) -> bytes:
    from chia_rs import AugSchemeMPL, G2Element

    if not sig_bytes_list:
        return bytes(G2Element())
    return bytes(AugSchemeMPL.aggregate([G2Element.from_bytes(s) for s in sig_bytes_list]))


# --------------------------------------------------------------------------- #
# Claim bundle assembly.
# --------------------------------------------------------------------------- #

def _token_amount(contents: Sequence[bytes]) -> int:
    return int.from_bytes(contents[2], "big")


def _require_pinned_asset_id(net: WarpNet, requested: str = "") -> str:
    """The wrapped id a claim may anchor to, lower-cased.

    ``requested`` (the job's asset) must be one this deployment already
    pins and re-derives at engine construction, so a ClaimRequest can
    never introduce an id of its own -- keeping the last gate before
    minting exactly as strict as the single global constant it replaced.
    """
    expected = (requested or net.expected_asset_id or "").lower()
    if not expected:
        raise WarpDriverError(
            f"{net.name}.expected_asset_id is empty; refusing to build a claim"
        )
    known = {(net.expected_asset_id or "").lower()} | {
        (spec.expected_asset_id or "").lower()
        for _key, spec in (getattr(net, "assets", ()) or ())
    }
    known.discard("")
    if expected not in known:
        raise WarpDriverError(
            f"claim expects wrapped id {expected}, which {net.name} does not "
            "pin for any bridgeable asset -- refusing to mint"
        )
    return expected


def build_claim_bundle(req: ClaimRequest) -> ClaimBundle:
    """Assemble the five-spend claim, refusing on any correctness-anchor miss."""
    net = req.net
    launcher_id = bytes.fromhex(net.portal_launcher_id)
    source_chain = net.source_chain.encode()
    agg_sig_extra = bytes.fromhex(net.agg_sig_extra_data)

    if len(req.contents) != 3:
        raise WarpDriverError(f"expected 3 message contents, got {len(req.contents)}")
    erc20_contract, receiver_ph, _amount_b32 = req.contents
    token_amount = _token_amount(req.contents)
    if token_amount <= 0:
        raise WarpDriverError("attested token amount is not positive")

    expected_security = token_amount + req.claim_fee
    if req.security_coin.amount != expected_security:
        raise WarpDriverError(
            f"security coin amount {req.security_coin.amount} != "
            f"post_tip+fee {expected_security}"
        )

    # --- portal singleton spend (verify reconstruction against chain) -------- #
    update_ph = get_portal_update_puzzle_hash(net)
    portal_inner = get_portal_receiver_inner_puzzle(
        launcher_id,
        net.signature_threshold,
        net.validator_bls_keys,
        update_ph,
        req.last_chains_and_nonces,
    )
    portal_full = puzzle_for_singleton(launcher_id, portal_inner)
    if cu.sha256tree(portal_full) != req.portal_coin.puzzle_hash:
        raise WarpDriverError(
            "reconstructed portal puzzle hash does not match the on-chain portal "
            "coin -- portal state is stale or the validator/multisig set changed"
        )
    portal_inner_hash = cu.sha256tree(portal_inner)

    inner_solution = get_portal_receiver_inner_solution(
        source_chain,
        req.nonce,
        req.source,
        req.destination,
        req.contents,
        get_sigs_switch(req.sig_switches),
    )
    lineage = make_lineage_proof(
        req.portal_parent_parent_info, req.portal_parent_inner_puzzle_hash, 1
    )
    portal_spend = CoinSpend(
        req.portal_coin,
        portal_full,
        solution_for_singleton(lineage, 1, inner_solution),
    )

    # --- message coin -------------------------------------------------------- #
    message_program = SExp.to(list(req.contents))
    message_hash = cu.sha256tree(message_program)
    message_coin_puzzle = get_message_coin_puzzle(
        launcher_id, source_chain, req.source, req.nonce, req.destination, message_hash
    )
    message_coin = Coin(
        req.portal_coin.name(), cu.sha256tree(message_coin_puzzle), 0
    )

    # --- cat_minter coin (destination anchor) -------------------------------- #
    minter_puzzle = get_cat_minter_puzzle(launcher_id, source_chain, req.source)
    minter_ph = cu.sha256tree(minter_puzzle)
    if minter_ph != req.destination:
        raise WarpDriverError(
            "derived cat_minter puzzle hash != attested message.destination"
        )
    minter_coin = Coin(req.security_coin.name(), minter_ph, token_amount)

    message_coin_spend = CoinSpend(
        message_coin,
        message_coin_puzzle,
        get_message_coin_solution(
            minter_coin,
            req.portal_parent_parent_info,
            portal_inner_hash,
            message_coin.name(),
        ),
    )
    minter_coin_spend = CoinSpend(
        minter_coin,
        minter_puzzle,
        get_cat_minter_puzzle_solution(
            req.nonce,
            message_program,
            minter_ph,
            minter_coin.name(),
            message_coin.parent_coin_info,
        ),
    )

    # --- eve CAT (asset-id anchor) ------------------------------------------- #
    tail = get_wrapped_tail(launcher_id, source_chain, req.source, erc20_contract)
    tail_hash = cu.sha256tree(tail)
    # Unconditional: defence in depth behind verify_wrapped_asset_anchor(), which
    # already ran at engine construction. This one additionally covers the
    # attested erc20_contract, which the offline anchor cannot see.
    expected = _require_pinned_asset_id(net, req.expected_asset_id)
    if tail_hash.hex() != expected:
        raise WarpDriverError(
            f"derived wrapped-TAIL {tail_hash.hex()} != configured asset id "
            f"{expected}"
        )
    mint_payout_inner = get_cat_mint_and_payout_inner_puzzle(receiver_ph)
    eve_cat_puzzle = construct_cat_puzzle(tail_hash, mint_payout_inner)
    eve_cat = Coin(minter_coin.name(), cu.sha256tree(eve_cat_puzzle), token_amount)
    eve_proof = Coin(
        eve_cat.parent_coin_info,
        cu.sha256tree(mint_payout_inner),  # CAT proof uses the *inner* puzzle hash
        eve_cat.amount,
    )
    eve_cat_spend = CoinSpend(
        eve_cat,
        eve_cat_puzzle,
        get_cat_solution(
            get_cat_mint_and_payout_inner_puzzle_solution(
                tail, token_amount, minter_coin.parent_coin_info
            ),
            None,
            eve_cat.name(),
            eve_cat,
            eve_proof,
            0,
            0,
        ),
    )

    # --- security coin ------------------------------------------------------- #
    conditions = [
        [ASSERT_CONCURRENT_SPEND, eve_cat.name()],
        [CREATE_COIN, minter_ph, token_amount],
    ]
    if req.claim_fee > 0:
        conditions.append([RESERVE_FEE, req.claim_fee])
    security_spend, security_sig = build_security_coin_spend(
        req.security_coin, conditions, req.ephemeral_sk, agg_sig_extra
    )

    # --- aggregate + predicted ids ------------------------------------------- #
    aggregated = aggregate_sigs(list(req.validator_sigs) + [security_sig])
    bundle = SpendBundle(
        [portal_spend, message_coin_spend, minter_coin_spend, eve_cat_spend, security_spend],
        aggregated,
    )

    final_cat_full_ph = curry_hashes(
        CAT_MOD_HASH, _h_atom(CAT_MOD_HASH), _h_atom(tail_hash), receiver_ph
    )
    final_cat_coin_id = cu.coin_name(eve_cat.name(), final_cat_full_ph, token_amount)

    return ClaimBundle(
        bundle=bundle,
        message_coin_id=message_coin.name(),
        minter_coin_id=minter_coin.name(),
        eve_cat_coin_id=eve_cat.name(),
        final_cat_coin_id=final_cat_coin_id,
        security_coin_id=req.security_coin.name(),
    )


def build_sweep_bundle(
    net: WarpNet,
    security_coin: Coin,
    destination_puzzle_hash: bytes,
    ephemeral_sk: bytes,
    sweep_fee: int = 0,
) -> SpendBundle:
    """Recover a funding coin whose claim never landed (or a third party claimed).

    Spends the ephemeral security coin straight back to ``destination_puzzle_hash``
    (the bot wallet), optionally reserving a fee. This is why every failure mode
    is stuck-not-lost: the bot alone holds ``ephemeral_sk``.
    """
    amount_out = security_coin.amount - sweep_fee
    if amount_out <= 0:
        raise WarpDriverError("sweep fee exceeds the security coin amount")
    conditions = [[CREATE_COIN, destination_puzzle_hash, amount_out]]
    if sweep_fee > 0:
        conditions.append([RESERVE_FEE, sweep_fee])
    spend, sig = build_security_coin_spend(
        security_coin, conditions, ephemeral_sk, bytes.fromhex(net.agg_sig_extra_data)
    )
    return SpendBundle([spend], aggregate_sigs([sig]))
