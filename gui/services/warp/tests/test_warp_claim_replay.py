"""CLVM golden-replay of the warp.green inbound claim path.

Why this file exists: the first live claim (2026-08-23) sat stuck for ~13
hours because ``build_claim_bundle`` passed ``req.portal_parent_parent_info``
-- the portal's GRANDPARENT id, a value that belongs only in the portal
spend's own lineage proof -- into the message coin's solution, whose
``(parent_parent_info . parent_inner_puzzle_hash)`` pair must rebuild the
CURRENT portal coin's id.  The message coin's puzzle turns that pair into an
ASSERT_MY_PARENT_ID (opcode 71) condition; asserting a parent that never
existed failed every push deterministically with ASSERT_MY_PARENT_ID_FAILED.
The one-line fix (PR #105) swapped in ``req.portal_coin.parent_coin_info``.

The bug shipped because nothing ever EXECUTED the claim's CLVM: the bundle
builds fine, the puzzle even evaluates fine -- the failure only appears when
the emitted opcode-71 value is compared against the coin's actual parent,
which mainnet consensus was the first thing to do.  These tests do that
comparison locally: they build the five-spend bundle from a fixed, synthetic,
fully-offline fixture, run each relevant puzzle/solution pair through the
``clvm`` interpreter (the same package the bridge already depends on), and
referee the resulting conditions the way consensus would.

Hermeticity: no network, no wallet, no node.  The puzzles are the vendored,
hash-asserted reveals under ``gui/services/warp/puzzles/`` (the same programs
``drivers.py`` itself loads); every coin is synthetic and deterministic.  The
synthetic portal coin's parent id is derived from its own lineage proof, so
the fixture is consensus-consistent end to end and every ASSERT_MY_PARENT_ID
an executed spend emits can be checked against the spent coin's real parent.
"""

from __future__ import annotations

import functools
import hashlib

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")

from chia_rs import AugSchemeMPL  # noqa: E402
from clvm import run_program  # noqa: E402
from clvm.operators import OPERATOR_LOOKUP  # noqa: E402

from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers as d  # noqa: E402

SExp = cu.SExp
NET = C.MAINNET

# Condition opcodes the executed spends emit (chia condition_opcodes).
AGG_SIG_ME = 50
CREATE_COIN = 51
CREATE_COIN_ANNOUNCEMENT = 60
ASSERT_COIN_ANNOUNCEMENT = 61
ASSERT_MY_COIN_ID = 70
ASSERT_MY_PARENT_ID = 71
ASSERT_MY_AMOUNT = 73

# --------------------------------------------------------------------------- #
# Fixed synthetic fixture (mirrors test_warp_drivers.py, plus a consensus-
# consistent portal parent and enough sig switches for the portal receiver's
# m-of-n gate to evaluate instead of raising).
# --------------------------------------------------------------------------- #

_TOKEN_AMOUNT = 1000  # 1 USDC in CAT mojos
_CLAIM_FEE = 100_000_000
_EPHEMERAL_SEED = bytes(range(32))
_RECEIVER_PH = bytes.fromhex(NET.bridging_puzzle_hash)  # stand-in bot xch ph
_NONCE = bytes.fromhex("11" * 32)
_PORTAL_GRANDPARENT = bytes.fromhex("44" * 32)
_PORTAL_PARENT_INNER_PH = bytes.fromhex("55" * 32)


def _contents() -> list:
    erc20 = b"\x00" * 12 + bytes.fromhex(NET.usdc_address[2:])  # padded to 32
    return [erc20, _RECEIVER_PH, _TOKEN_AMOUNT.to_bytes(32, "big")]


def _security_coin() -> d.Coin:
    sk = AugSchemeMPL.key_gen(_EPHEMERAL_SEED)
    ph = cu.sha256tree(d.p2_puzzle_for_pk(bytes(sk.get_g1())))
    return d.Coin(bytes.fromhex("22" * 32), ph, _TOKEN_AMOUNT + _CLAIM_FEE)


def _portal_inner() -> SExp:
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    return d.get_portal_receiver_inner_puzzle(
        launcher_id,
        NET.signature_threshold,
        NET.validator_bls_keys,
        d.get_portal_update_puzzle_hash(NET),
        (),
    )


def _portal_coin() -> d.Coin:
    """A synthetic portal singleton whose parent id matches its lineage proof.

    The singleton puzzle rebuilds its parent's full puzzle hash from
    (grandparent, parent inner hash, 1) and ASSERT_MY_PARENT_IDs the result;
    deriving ``parent_coin_info`` the same way keeps the synthetic universe
    consensus-consistent, so the referee below can hold EVERY executed spend
    to "asserted parent == actual parent".
    """
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    struct = SExp.to(d._singleton_struct(launcher_id))
    parent_full_ph = d.curry_hashes(
        d.SINGLETON_MOD_HASH, cu.sha256tree(struct), _PORTAL_PARENT_INNER_PH
    )
    parent_id = cu.coin_name(_PORTAL_GRANDPARENT, parent_full_ph, 1)
    full = d.puzzle_for_singleton(launcher_id, _portal_inner())
    return d.Coin(parent_id, cu.sha256tree(full), 1)


def _destination() -> bytes:
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    minter = d.get_cat_minter_puzzle(
        launcher_id,
        NET.source_chain.encode(),
        bytes.fromhex(NET.erc20_bridge_address[2:]),
    )
    return cu.sha256tree(minter)


@functools.lru_cache(maxsize=1)
def _claim() -> tuple:
    """(ClaimRequest, ClaimBundle), built once for the module."""
    n = len(NET.validator_bls_keys)
    switches = [True] * NET.signature_threshold + [False] * (n - NET.signature_threshold)
    req = d.ClaimRequest(
        net=NET,
        portal_coin=_portal_coin(),
        portal_parent_parent_info=_PORTAL_GRANDPARENT,
        portal_parent_inner_puzzle_hash=_PORTAL_PARENT_INNER_PH,
        last_chains_and_nonces=(),
        validator_sigs=(),
        sig_switches=switches,
        nonce=_NONCE,
        source=bytes.fromhex(NET.erc20_bridge_address[2:]),
        destination=_destination(),
        contents=_contents(),
        security_coin=_security_coin(),
        ephemeral_sk=bytes(AugSchemeMPL.key_gen(_EPHEMERAL_SEED)),
        claim_fee=_CLAIM_FEE,
    )
    return req, d.build_claim_bundle(req)


# Bundle spend order, pinned by build_claim_bundle.
_PORTAL, _MESSAGE, _MINTER, _EVE_CAT, _SECURITY = range(5)


# --------------------------------------------------------------------------- #
# The local consensus referee.
# --------------------------------------------------------------------------- #

def _run_spend(spend) -> list:
    """Execute a CoinSpend's CLVM; return [(opcode, [arg atoms]), ...]."""
    _cost, output = run_program(spend.puzzle_reveal, spend.solution, OPERATOR_LOOKUP)
    conditions = []
    for cond in output.as_iter():
        parts = list(cond.as_iter())
        conditions.append(
            (parts[0].as_int(), [bytes(p.atom) for p in parts[1:] if p.atom is not None])
        )
    return conditions


def _only(conditions: list, opcode: int) -> list:
    picked = [args for op, args in conditions if op == opcode]
    assert picked, f"executed spend emitted no opcode-{opcode} condition"
    return picked


def _std_coin_id(parent: bytes, puzzle_hash: bytes, amount: int) -> bytes:
    """The consensus coin-id rule, written out with hashlib alone so these
    goldens do not depend on the very helpers the drivers are built from."""
    if amount == 0:
        encoded = b""
    else:
        encoded = amount.to_bytes((amount.bit_length() + 8) // 8, "big", signed=True)
    return hashlib.sha256(parent + puzzle_hash + encoded).digest()


# --------------------------------------------------------------------------- #
# 1. The portal spend executes and creates the message coin.
# --------------------------------------------------------------------------- #

def test_portal_spend_executes_and_creates_the_message_coin():
    """Run the portal receiver's CLVM: it must recreate itself (amount 1) and
    create the message coin (amount 0) at exactly the puzzle hash the bundle's
    message spend reveals -- which, with the coin-id rule, is what makes the
    portal coin the message coin's parent."""
    req, result = _claim()
    spends = result.bundle.coin_spends
    conditions = _run_spend(spends[_PORTAL])

    creates = _only(conditions, CREATE_COIN)
    by_ph = {args[0]: args for args in creates}

    # The message coin: amount 0 (an empty atom in canonical CLVM encoding).
    message_coin = spends[_MESSAGE].coin
    assert message_coin.puzzle_hash in by_ph
    assert by_ph[message_coin.puzzle_hash][1] == b""

    # Its parent is therefore the portal coin, per the coin-id rule.
    assert message_coin.parent_coin_info == req.portal_coin.name()
    assert message_coin.name() == _std_coin_id(
        req.portal_coin.name(), message_coin.puzzle_hash, 0
    )

    # Self-recreation: the next portal singleton, inner puzzle advanced by the
    # just-claimed (chain, nonce) -- derived independently here.
    launcher_id = bytes.fromhex(NET.portal_launcher_id)
    next_inner = d.get_portal_receiver_inner_puzzle(
        launcher_id,
        NET.signature_threshold,
        NET.validator_bls_keys,
        d.get_portal_update_puzzle_hash(NET),
        ((NET.source_chain.encode(), _NONCE),),
    )
    next_full_ph = cu.sha256tree(d.puzzle_for_singleton(launcher_id, next_inner))
    assert next_full_ph in by_ph
    assert by_ph[next_full_ph][1] == b"\x01"

    # Referee its own parent assertion: the consensus-consistent fixture must
    # satisfy the singleton's lineage check exactly.
    (asserted_parent,) = _only(conditions, ASSERT_MY_PARENT_ID)
    assert asserted_parent[0] == req.portal_coin.parent_coin_info

    # The attestation gate: one AGG_SIG_ME per set sig switch.  Its message is
    # the 32-byte message tree; consensus appends (coin id || extra data), so
    # the full signed payload is get_validator_message_digest keyed to THIS
    # portal coin's id -- recollected sigs whenever the portal advances.
    agg_sigs = _only(conditions, AGG_SIG_ME)
    assert len(agg_sigs) == NET.signature_threshold
    digest = d.get_validator_message_digest(
        NET.source_chain.encode(),
        _NONCE,
        req.source,
        req.destination,
        req.contents,
        req.portal_coin.name(),
        bytes.fromhex(NET.agg_sig_extra_data),
    )
    extra = bytes.fromhex(NET.agg_sig_extra_data)
    for _pubkey, message in agg_sigs:
        assert message + req.portal_coin.name() + extra == digest


# --------------------------------------------------------------------------- #
# 2. The golden replay: the message coin asserts its ACTUAL parent.
# --------------------------------------------------------------------------- #

def test_message_coin_spend_asserts_the_portal_coin_as_its_parent():
    """Execute the claim's message-coin spend and referee opcode 71 the way
    consensus does.  The asserted parent must be the portal coin's own id --
    std_hash(portal.parent_coin_info || portal.puzzle_hash || amount) -- and
    must equal the message coin's actual parent from the bundle.  This is the
    check that was only ever performed on mainnet before 2026-08-23."""
    req, result = _claim()
    spends = result.bundle.coin_spends
    message_coin = spends[_MESSAGE].coin
    conditions = _run_spend(spends[_MESSAGE])

    parents = _only(conditions, ASSERT_MY_PARENT_ID)
    assert len(parents) == 1, "expected exactly one ASSERT_MY_PARENT_ID"
    asserted_parent = parents[0][0]

    # The consensus comparison: asserted parent == the coin's actual parent.
    assert asserted_parent == message_coin.parent_coin_info

    # And that parent is the PORTAL COIN ITSELF, its id recomputed here from
    # first principles (hashlib only; portal amount is always 1).
    expected_portal_id = _std_coin_id(
        req.portal_coin.parent_coin_info, req.portal_coin.puzzle_hash, 1
    )
    assert asserted_parent == expected_portal_id
    assert expected_portal_id == req.portal_coin.name()

    # The puzzle also asserts its own id; referee that too.
    (own_id,) = _only(conditions, ASSERT_MY_COIN_ID)
    assert own_id[0] == message_coin.name()
    assert own_id[0] == _std_coin_id(
        message_coin.parent_coin_info, message_coin.puzzle_hash, 0
    )


def test_message_and_minter_spends_announcement_interlock():
    """The executed message and minter spends must satisfy each other's
    coin announcements (announcement id = sha256(creator coin id || payload)),
    which is what ties the attested message to the coin that mints."""
    _req, result = _claim()
    spends = result.bundle.coin_spends
    message_coin = spends[_MESSAGE].coin
    minter_coin = spends[_MINTER].coin

    message_conditions = _run_spend(spends[_MESSAGE])
    minter_conditions = _run_spend(spends[_MINTER])

    def created(conds, coin):
        return {
            hashlib.sha256(coin.name() + args[0]).digest()
            for args in _only(conds, CREATE_COIN_ANNOUNCEMENT)
        }

    def asserted(conds):
        return {args[0] for args in _only(conds, ASSERT_COIN_ANNOUNCEMENT)}

    assert asserted(message_conditions) <= created(minter_conditions, minter_coin)
    assert asserted(minter_conditions) <= created(message_conditions, message_coin)


# --------------------------------------------------------------------------- #
# 3. The incident, replayed: the grandparent solution fails the referee.
# --------------------------------------------------------------------------- #

def test_grandparent_solution_reproduces_assert_my_parent_id_failed():
    """Re-create what the pre-PR-#105 code built -- the message-coin solution
    carrying ``req.portal_parent_parent_info`` -- and execute it through the
    SAME puzzle reveal the bundle ships.  It evaluates without error (which is
    exactly why every dry-run stayed green), but the ASSERT_MY_PARENT_ID it
    emits names a coin that never existed, so the consensus comparison that
    passes for the fixed construction fails for the buggy one."""
    req, result = _claim()
    spends = result.bundle.coin_spends
    message_spend = spends[_MESSAGE]
    message_coin = message_spend.coin
    minter_coin = spends[_MINTER].coin
    portal_inner_hash = cu.sha256tree(_portal_inner())

    buggy_solution = d.get_message_coin_solution(
        minter_coin,
        req.portal_parent_parent_info,  # the GRANDPARENT -- the original bug
        portal_inner_hash,
        message_coin.name(),
    )
    # The two constructions genuinely differ (the regression is observable).
    assert cu.sha256tree(buggy_solution) != cu.sha256tree(message_spend.solution)

    # CLVM evaluation still succeeds -- the bug is invisible to any check
    # that stops at "does the bundle build / does the puzzle run".
    buggy_conditions = _run_spend(
        d.CoinSpend(message_coin, message_spend.puzzle_reveal, buggy_solution)
    )

    parents = _only(buggy_conditions, ASSERT_MY_PARENT_ID)
    assert len(parents) == 1
    buggy_parent = parents[0][0]

    # The referee that mainnet ran on 2026-08-23: asserted parent vs actual.
    assert buggy_parent != message_coin.parent_coin_info, (
        "the grandparent-based solution asserted the real parent -- this test "
        "can no longer distinguish the fixed construction from the buggy one"
    )

    # Specifically, it asserts a phantom sibling of the portal: the same
    # portal puzzle hash hung off the grandparent -- a coin that was never
    # created, hence deterministic ASSERT_MY_PARENT_ID_FAILED on every push.
    phantom = _std_coin_id(
        req.portal_parent_parent_info, req.portal_coin.puzzle_hash, 1
    )
    assert buggy_parent == phantom


def test_driver_solution_uses_the_portal_parent_not_the_grandparent():
    """Pin the fix at the source level too: the solution the bundle actually
    ships must carry (portal.parent_coin_info, portal inner hash) -- the pair
    that rebuilds the portal coin's id inside the puzzle."""
    req, result = _claim()
    solution = result.bundle.coin_spends[_MESSAGE].solution
    _receiver_pair, parent_pair, _msg_id = list(solution.as_iter())
    assert bytes(parent_pair.first().as_atom()) == req.portal_coin.parent_coin_info
    assert bytes(parent_pair.rest().as_atom()) == cu.sha256tree(_portal_inner())
    assert bytes(parent_pair.first().as_atom()) != req.portal_parent_parent_info
