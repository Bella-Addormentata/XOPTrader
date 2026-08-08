"""Offline tests for the Chia-side claim orchestration (:mod:`claim`).

No network, no node, no funds. A ``FakeCoinset`` (mirroring the ``FakeCaller`` /
``FakeFetcher`` idiom in ``test_warp_evm.py`` / ``test_warp_nostr.py``) records
every call and returns canned coin records / solutions, so the portal walk,
push classifier, and completion checks are exercised deterministically.

The load-bearing part is portal sync. We fabricate a *synthetic singleton
lineage*: each portal coin's puzzle hash is reconstructed from its
``last_chains_and_nonces`` exactly as the module does, and a spent coin's
solution carries the *next* coin's ``last_chains_and_nonces`` as its
``new_chains_and_nonces`` (the empirically-proven full-replacement rule). That
makes the fake chain self-consistent with the reconstruction the walk performs,
so a correct walk reaches the tip and a tampered record trips the fail-closed
guard.

The pure structural tests (sync, parse, push classification, completion) need
only ``clvm``; the few that build/sign a real bundle or derive a key from BLS
bytes ``importorskip("chia_rs")``.
"""

from __future__ import annotations

import dataclasses
from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")  # claim imports clvm_utils, which imports clvm

from gui.services.warp import claim  # noqa: E402
from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import drivers  # noqa: E402
from gui.services.warp.coinset import (  # noqa: E402
    Coin,
    CoinRecord,
    CoinSolution,
    CoinsetError,
)

NET = C.MAINNET
SExp = cu.SExp

# Two 3-byte warp chain ids for multi-entry batch coverage.
BSE = NET.source_chain.encode()
ETH = b"eth"

# Mirrors of the driver test's known-passing claim recipe (test_warp_drivers.py).
_TOKEN = 1000
_FEE = 100_000_000
_EPHEMERAL_SEED = bytes(range(32))
_RECEIVER_PH = bytes.fromhex(NET.bridging_puzzle_hash)
_NONCE = bytes.fromhex("11" * 32)
_SOURCE = bytes.fromhex(NET.erc20_bridge_address[2:])


# --------------------------------------------------------------------------- #
# Injected fake chain.
# --------------------------------------------------------------------------- #

def _norm(value) -> str:
    """Coerce bytes / (0x-optional) hex string to lower-case no-0x hex."""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).hex()
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower()


class FakeCoinset:
    """Records calls; serves canned coin records, solutions, and push results."""

    def __init__(self) -> None:
        self._by_name: dict[str, CoinRecord] = {}
        self._by_ph: list[CoinRecord] = []
        self._solutions: dict[str, CoinSolution] = {}
        # str | Exception (raised) | callable(bundle) -> str | Exception.
        self.push_result = "SUCCESS"
        self.pushed: list = []

    def get_coin_record_by_name(self, name):
        return self._by_name.get(_norm(name))

    def get_coin_records_by_puzzle_hash(
        self, puzzle_hash, *, include_spent=False, start_height=None, end_height=None
    ):
        want = _norm(puzzle_hash)
        out = []
        for rec in self._by_ph:
            if _norm(rec.coin.puzzle_hash) != want:
                continue
            if not include_spent and rec.spent:
                continue
            out.append(rec)
        return out

    def get_puzzle_and_solution(self, coin_id, height):
        sol = self._solutions.get(_norm(coin_id))
        if sol is None:
            raise CoinsetError(
                f"get_puzzle_and_solution: coin {_norm(coin_id)} not spent at height {height}"
            )
        return sol

    def push_tx(self, spend_bundle):
        self.pushed.append(spend_bundle)
        r = self.push_result
        if callable(r):
            r = r(spend_bundle)
        if isinstance(r, Exception):
            raise r
        return r


# --------------------------------------------------------------------------- #
# Synthetic portal-singleton lineage.
# --------------------------------------------------------------------------- #

def _portal_coin(net, parent_id: bytes, last_cn) -> SimpleNamespace:
    """A portal coin whose puzzle hash reconstructs from ``last_cn``."""
    launcher = bytes.fromhex(net.portal_launcher_id)
    update_ph = drivers.get_portal_update_puzzle_hash(net)
    inner = drivers.get_portal_receiver_inner_puzzle(
        launcher, net.signature_threshold, net.validator_bls_keys, update_ph, last_cn
    )
    full = drivers.puzzle_for_singleton(launcher, inner)
    ph = cu.sha256tree(full)
    return SimpleNamespace(
        parent_id=parent_id,
        last_cn=tuple(last_cn),
        inner=inner,
        full=full,
        ph=ph,
        cid=cu.coin_name(parent_id, ph, 1),
    )


def _portal_solution(child_last_cn):
    """A portal spend solution whose ``new_chains_and_nonces`` == ``child_last_cn``.

    Shape: ``[lineage_proof, amount, [update, new_chains_and_nonces, messages]]``.
    Each entry is a Python 2-tuple so ``SExp.to`` serialises it as a cons pair
    ``(chain . nonce)`` -- exactly what ``_parse_last_chains_and_nonces`` expects.
    """
    entries = [(bytes(chain), bytes(nonce)) for chain, nonce in child_last_cn]
    lineage = [b"\x00" * 32, b"\x11" * 32, 1]
    return SExp.to([lineage, 1, [0, entries, 0]])


def _build_chain(net, ls, base_parent=b"\xaa" * 32):
    """Build a portal lineage where ``coins[i].last_cn == ls[i]`` and each child's
    ``parent_coin_info`` is the prior coin id."""
    coins = []
    parent = base_parent
    for last_cn in ls:
        c = _portal_coin(net, parent, last_cn)
        coins.append(c)
        parent = c.cid
    return coins


def _seed_chain(fake: FakeCoinset, coins) -> None:
    """Seed the fake so the last coin is the unspent tip and each earlier coin is
    spent with a solution carrying the *next* coin's ``last_chains_and_nonces``."""
    n = len(coins)
    for i, c in enumerate(coins):
        spent = i < n - 1
        coin = Coin(c.parent_id.hex(), c.ph.hex(), 1)
        rec = CoinRecord(
            coin=coin,
            confirmed_block_index=1000 + i,
            spent_block_index=(2000 + i) if spent else 0,
            spent=spent,
            coinbase=False,
            timestamp=None,
        )
        fake._by_name[c.cid.hex()] = rec
        fake._by_ph.append(rec)
        if spent:
            fake._solutions[c.cid.hex()] = CoinSolution(
                coin=coin,
                puzzle_reveal=cu.program_to_bytes(c.full).hex(),
                solution=cu.program_to_bytes(_portal_solution(coins[i + 1].last_cn)).hex(),
            )


def _seed_coin(fake: FakeCoinset, *, parent, ph, amount, spent, spent_index=0, confirmed=1) -> str:
    """Seed a plain coin (security / final CAT / wrapped CAT); returns its coin id hex."""
    coin = Coin(parent.hex(), ph.hex(), amount)
    cid = cu.coin_name(parent, ph, amount)
    rec = CoinRecord(
        coin=coin,
        confirmed_block_index=confirmed,
        spent_block_index=spent_index,
        spent=spent,
        coinbase=False,
        timestamp=None,
    )
    fake._by_name[cid.hex()] = rec
    fake._by_ph.append(rec)
    return cid.hex()


# --------------------------------------------------------------------------- #
# last_chains_and_nonces parsing (shape guards).
# --------------------------------------------------------------------------- #

def test_parse_last_chains_and_nonces_shape_guards():
    n = b"\x01" * 32
    # A well-formed single (chain . nonce) entry round-trips to a tuple.
    ok = SExp.to([b"lin", 1, [0, [(BSE, n)], 0]])
    assert claim._parse_last_chains_and_nonces(ok) == ((BSE, n),)

    # Top-level solution shorter than [lineage, amount, inner_solution].
    with pytest.raises(claim.PortalSyncError):
        claim._parse_last_chains_and_nonces(SExp.to([b"only", b"two"]))

    # inner_solution shorter than [update, new_chains_and_nonces, ...].
    with pytest.raises(claim.PortalSyncError):
        claim._parse_last_chains_and_nonces(SExp.to([b"lin", 1, [0]]))

    # An entry that is a 2-list (proper list) rather than a cons pair.
    bad = SExp.to([b"lin", 1, [0, [[BSE, n]], 0]])
    with pytest.raises(claim.PortalSyncError, match="pair"):
        claim._parse_last_chains_and_nonces(bad)


# --------------------------------------------------------------------------- #
# Portal sync (structural forward walk).
# --------------------------------------------------------------------------- #

def test_sync_portal_zero_hop_reads_tip_and_lineage():
    fake = FakeCoinset()
    n1 = b"\x01" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),)])  # C0 (parent, spent) -> C1 (tip)
    _seed_chain(fake, coins)

    state = claim.sync_portal(fake, NET, hint=coins[1].cid)  # hint already at tip

    assert state.coin_id == coins[1].cid
    assert state.coin.puzzle_hash == coins[1].ph
    assert state.coin.amount == 1
    assert state.last_chains_and_nonces == ((BSE, n1),)
    assert state.parent_parent_info == coins[0].parent_id
    assert state.parent_inner_puzzle_hash == cu.sha256tree(coins[0].inner)


def test_sync_portal_multi_hop_walks_to_tip():
    fake = FakeCoinset()
    n1, n2, n3 = b"\x01" * 32, b"\x02" * 32, b"\x03" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),), ((ETH, n2),), ((BSE, n3),)])
    _seed_chain(fake, coins)

    state = claim.sync_portal(fake, NET, hint=coins[0].cid)  # walk 3 hops to the tip

    assert state.coin_id == coins[3].cid
    assert state.last_chains_and_nonces == ((BSE, n3),)
    # Parent is C2; its own parent is C1's coin id.
    assert state.parent_parent_info == coins[1].cid
    assert state.parent_inner_puzzle_hash == cu.sha256tree(coins[2].inner)


def test_sync_portal_parses_multi_entry_batch():
    fake = FakeCoinset()
    n1, n2, n3 = b"\x01" * 32, b"\x02" * 32, b"\x03" * 32
    # The middle hop processes two messages at once (a 2-entry L on C1).
    coins = _build_chain(NET, [(), ((BSE, n1), (ETH, n2)), ((BSE, n3),)])
    _seed_chain(fake, coins)

    state = claim.sync_portal(fake, NET, hint=coins[0].cid)

    assert state.coin_id == coins[2].cid
    assert state.last_chains_and_nonces == ((BSE, n3),)


def test_sync_portal_raises_on_missing_hint():
    fake = FakeCoinset()
    with pytest.raises(claim.PortalSyncError, match="not found"):
        claim.sync_portal(fake, NET, hint=b"\xde" * 32)


def test_sync_portal_fails_closed_on_reconstruction_mismatch():
    fake = FakeCoinset()
    n1 = b"\x01" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),)])
    _seed_chain(fake, coins)
    # Tamper the tip's on-chain puzzle hash so it no longer matches reconstruct(L).
    tip = coins[1].cid.hex()
    rec = fake._by_name[tip]
    fake._by_name[tip] = dataclasses.replace(
        rec, coin=dataclasses.replace(rec.coin, puzzle_hash="00" * 32)
    )
    with pytest.raises(claim.PortalSyncError, match="reconstructed"):
        claim.sync_portal(fake, NET, hint=coins[1].cid)


def test_sync_portal_rejects_non_singleton_amount():
    fake = FakeCoinset()
    n1 = b"\x01" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),)])
    _seed_chain(fake, coins)
    tip = coins[1].cid.hex()
    rec = fake._by_name[tip]
    fake._by_name[tip] = dataclasses.replace(rec, coin=dataclasses.replace(rec.coin, amount=2))
    with pytest.raises(claim.PortalSyncError, match="amount"):
        claim.sync_portal(fake, NET, hint=coins[1].cid)


def test_sync_portal_requires_spent_parent():
    fake = FakeCoinset()
    n1 = b"\x01" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),)])
    _seed_chain(fake, coins)
    # Make the tip's parent look unspent: sync cannot derive lineage from it.
    par = coins[0].cid.hex()
    rec = fake._by_name[par]
    fake._by_name[par] = dataclasses.replace(rec, spent=False, spent_block_index=0)
    with pytest.raises(claim.PortalSyncError, match="parent"):
        claim.sync_portal(fake, NET, hint=coins[1].cid)


def test_sync_portal_raises_when_tip_beyond_max_hops():
    fake = FakeCoinset()
    n1, n2, n3 = b"\x01" * 32, b"\x02" * 32, b"\x03" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),), ((ETH, n2),), ((BSE, n3),)])
    _seed_chain(fake, coins)
    with pytest.raises(claim.PortalSyncError, match="within 2 hops"):
        claim.sync_portal(fake, NET, hint=coins[0].cid, max_hops=2)


# --------------------------------------------------------------------------- #
# Validator digest keyed to the synced portal coin id.
# --------------------------------------------------------------------------- #

def test_validator_digest_keys_to_synced_portal_coin_id():
    fake = FakeCoinset()
    n1 = b"\x01" * 32
    coins = _build_chain(NET, [(), ((BSE, n1),)])
    _seed_chain(fake, coins)
    state = claim.sync_portal(fake, NET, hint=coins[0].cid)

    dest = bytes.fromhex("cd" * 32)
    contents = [b"\x00" * 32, _RECEIVER_PH, (1000).to_bytes(32, "big")]
    digest = claim.validator_digest(
        NET, state, nonce=_NONCE, source=_SOURCE, destination=dest, contents=contents
    )
    expect = drivers.get_validator_message_digest(
        NET.source_chain.encode(), _NONCE, _SOURCE, dest, contents,
        state.coin_id, bytes.fromhex(NET.agg_sig_extra_data),
    )
    assert digest == expect
    assert len(digest) == 32 + 32 + len(bytes.fromhex(NET.agg_sig_extra_data))


# --------------------------------------------------------------------------- #
# Push classification (mempool conflict vs transport failure vs idempotent).
# --------------------------------------------------------------------------- #

def test_push_bundle_classifies_outcomes():
    fake = FakeCoinset()

    fake.push_result = "SUCCESS"
    assert claim._push_bundle(fake, {}) == ("accepted", "SUCCESS")

    # Already in the mempool from a prior tick -> idempotent "pending".
    fake.push_result = CoinsetError("push_tx failed: ALREADY_INCLUDING_TRANSACTION")
    kind, status = claim._push_bundle(fake, {})
    assert kind == "pending" and "ALREADY" in status

    # A real mempool rejection (a coin was spent under us) -> conflict.
    fake.push_result = CoinsetError("push_tx failed: DOUBLE_SPEND")
    kind, status = claim._push_bundle(fake, {})
    assert kind == "conflict" and "DOUBLE_SPEND" in status

    # A transport failure must re-raise, not be misread as a conflict.
    fake.push_result = CoinsetError("push_tx request failed: connection reset")
    with pytest.raises(CoinsetError):
        claim._push_bundle(fake, {})


# --------------------------------------------------------------------------- #
# Completion checks.
# --------------------------------------------------------------------------- #

def test_claim_landed_detects_final_cat_coin():
    fake = FakeCoinset()
    assert claim.claim_landed(fake, b"\xab" * 32) is False
    fid = _seed_coin(fake, parent=b"\xcc" * 32, ph=b"\xdd" * 32, amount=1000, spent=False)
    assert claim.claim_landed(fake, bytes.fromhex(fid)) is True


def test_security_coin_spent_detects_spend():
    fake = FakeCoinset()
    spent = _seed_coin(fake, parent=b"\x03" * 32, ph=b"\x04" * 32, amount=1, spent=True, spent_index=42)
    unspent = _seed_coin(fake, parent=b"\x05" * 32, ph=b"\x06" * 32, amount=1, spent=False)
    assert claim.security_coin_spent(fake, bytes.fromhex(spent)) is True
    assert claim.security_coin_spent(fake, bytes.fromhex(unspent)) is False
    assert claim.security_coin_spent(fake, b"\xee" * 32) is False


_MSG_KW = dict(
    nonce=bytes.fromhex("00" * 31 + "07"),
    source=bytes.fromhex(NET.erc20_bridge_address[2:]),
    destination=bytes(32),
    contents=[b"\xaa" * 20, _RECEIVER_PH, b"\x13\x79"],
)


def test_message_coin_puzzle_hash_matches_the_driver_currying():
    ph = claim.message_coin_puzzle_hash(NET, **_MSG_KW)
    expect = cu.sha256tree(
        drivers.get_message_coin_puzzle(
            bytes.fromhex(NET.portal_launcher_id),
            NET.source_chain.encode(),
            _MSG_KW["source"],
            _MSG_KW["nonce"],
            _MSG_KW["destination"],
            cu.sha256tree(drivers.SExp.to(list(_MSG_KW["contents"]))),
        )
    )
    assert ph == expect


def test_message_claimed_on_chain_is_per_nonce():
    """Unrelated receiver CAT activity must not register as a claim.

    This is the whole point of the check: the bot trades wUSDC.b continuously,
    so a signal derived from the receiver's coin set is unusable here.
    """
    ph = claim.message_coin_puzzle_hash(NET, **_MSG_KW)

    fake = FakeCoinset()
    assert claim.message_claimed_on_chain(fake, NET, **_MSG_KW) is False

    # An ordinary trade landing coins at the receiver changes nothing.
    _seed_coin(fake, parent=b"\x01" * 32, ph=_RECEIVER_PH, amount=1000, spent=False)
    assert claim.message_claimed_on_chain(fake, NET, **_MSG_KW) is False

    # A *spent* message coin does. It is an amount-0 coin whose parent is
    # whichever portal coin the claimer spent, so only the puzzle hash matches.
    _seed_coin(fake, parent=b"\x02" * 32, ph=ph, amount=0, spent=True, spent_index=10)
    assert claim.message_claimed_on_chain(fake, NET, **_MSG_KW) is True

    # ...and only for *this* nonce.
    other = dict(_MSG_KW, nonce=bytes.fromhex("00" * 31 + "08"))
    assert claim.message_claimed_on_chain(fake, NET, **other) is False


def test_message_coin_must_be_spent_not_merely_created():
    """Creation burns the nonce; only consumption forces the mint.

    A relayer can advance the portal -- killing our claim -- without funding the
    minter coin, leaving the message coin created and unspent while nothing was
    minted. Reading that as "claimed" would close the job and abandon the money.
    """
    ph = claim.message_coin_puzzle_hash(NET, **_MSG_KW)
    fake = FakeCoinset()

    _seed_coin(fake, parent=b"\x03" * 32, ph=ph, amount=0, spent=False)
    assert claim.message_claimed_on_chain(fake, NET, **_MSG_KW) is False

    _seed_coin(fake, parent=b"\x04" * 32, ph=ph, amount=0, spent=True, spent_index=11)
    assert claim.message_claimed_on_chain(fake, NET, **_MSG_KW) is True


# --------------------------------------------------------------------------- #
# Security-coin funding (BLS-derived; dedupe scan).
# --------------------------------------------------------------------------- #

def test_security_coin_puzzle_hash_matches_p2_derivation():
    pytest.importorskip("chia_rs")
    from chia_rs import AugSchemeMPL

    sk = AugSchemeMPL.key_gen(_EPHEMERAL_SEED)
    sk_bytes = bytes(sk)
    ph = claim.security_coin_puzzle_hash(sk_bytes)
    assert ph == cu.sha256tree(drivers.p2_puzzle_for_pk(bytes(sk.get_g1())))


def test_find_security_coin_matches_exact_unspent_amount():
    pytest.importorskip("chia_rs")
    from chia_rs import AugSchemeMPL

    sk_bytes = bytes(AugSchemeMPL.key_gen(_EPHEMERAL_SEED))
    ph = claim.security_coin_puzzle_hash(sk_bytes)
    expected = _TOKEN + _FEE

    fake = FakeCoinset()
    # A decoy of a different amount, and a spent coin of the right amount, are both ignored.
    _seed_coin(fake, parent=b"\x23" * 32, ph=ph, amount=9999, spent=False)
    _seed_coin(fake, parent=b"\x24" * 32, ph=ph, amount=expected, spent=True, spent_index=7)
    assert claim.find_security_coin(fake, sk_bytes, expected) is None  # not funded yet

    _seed_coin(fake, parent=b"\x22" * 32, ph=ph, amount=expected, spent=False)
    found = claim.find_security_coin(fake, sk_bytes, expected)
    assert found is not None and found.amount == expected
    assert isinstance(found, drivers.Coin)
    # Wrong expected amount -> None (funding stays idempotent).
    assert claim.find_security_coin(fake, sk_bytes, expected + 1) is None


# --------------------------------------------------------------------------- #
# Sweep (recover the funding coin).
# --------------------------------------------------------------------------- #

def _ephemeral_sk_bytes():
    from chia_rs import AugSchemeMPL

    return bytes(AugSchemeMPL.key_gen(_EPHEMERAL_SEED))


def _security_coin(amount=None):
    sk_bytes = _ephemeral_sk_bytes()
    ph = claim.security_coin_puzzle_hash(sk_bytes)
    return drivers.Coin(bytes.fromhex("22" * 32), ph, amount if amount is not None else _TOKEN + _FEE)


def test_build_and_push_sweep_reports_status_and_swallows_conflict():
    pytest.importorskip("chia_rs")
    coin = _security_coin(amount=1000)
    dest = bytes.fromhex("77" * 32)

    fake = FakeCoinset()
    fake.push_result = "SUCCESS"
    kind, status = claim.build_and_push_sweep(
        fake, NET, security_coin=coin, destination_puzzle_hash=dest,
        ephemeral_sk=_ephemeral_sk_bytes(), sweep_fee=0,
    )
    assert (kind, status) == ("accepted", "SUCCESS")
    assert len(fake.pushed) == 1
    assert set(fake.pushed[0]) == {"coin_spends", "aggregated_signature"}

    # The security coin was already spent (our claim landed, or a prior sweep):
    # reported in the status, not raised -- nothing left to recover. The caller
    # needs the "conflict" kind to know the sweep is *resolved* and may close a
    # FAILED job, which is what frees the single active-job slot.
    fake.push_result = CoinsetError("push_tx failed: DOUBLE_SPEND")
    kind, status = claim.build_and_push_sweep(
        fake, NET, security_coin=coin, destination_puzzle_hash=dest,
        ephemeral_sk=_ephemeral_sk_bytes(), sweep_fee=0,
    )
    assert kind == "conflict"
    assert "DOUBLE_SPEND" in status


def test_build_and_push_sweep_reraises_transport_error():
    pytest.importorskip("chia_rs")
    coin = _security_coin(amount=1000)
    fake = FakeCoinset()
    fake.push_result = CoinsetError("push_tx request failed: connection reset")
    with pytest.raises(CoinsetError):
        claim.build_and_push_sweep(
            fake, NET, security_coin=coin, destination_puzzle_hash=bytes.fromhex("77" * 32),
            ephemeral_sk=_ephemeral_sk_bytes(), sweep_fee=0,
        )


# --------------------------------------------------------------------------- #
# build_and_push_claim (build via drivers, push via the fake).
# --------------------------------------------------------------------------- #

def _valid_portal_state():
    portal = _portal_coin(NET, bytes.fromhex("33" * 32), ())
    return claim.PortalState(
        coin=drivers.Coin(bytes.fromhex("33" * 32), portal.ph, 1),
        coin_id=portal.cid,
        last_chains_and_nonces=(),
        parent_parent_info=bytes.fromhex("44" * 32),
        parent_inner_puzzle_hash=bytes.fromhex("55" * 32),
    )


def _contents():
    erc20 = b"\x00" * 12 + bytes.fromhex(NET.usdc_address[2:])
    return [erc20, _RECEIVER_PH, _TOKEN.to_bytes(32, "big")]


def _destination():
    launcher = bytes.fromhex(NET.portal_launcher_id)
    minter = drivers.get_cat_minter_puzzle(launcher, NET.source_chain.encode(), _SOURCE)
    return cu.sha256tree(minter)


def _push_claim(fake, **overrides):
    kwargs = dict(
        portal_state=_valid_portal_state(),
        nonce=_NONCE,
        source=_SOURCE,
        destination=_destination(),
        contents=_contents(),
        sig_switches=[False] * 10,
        validator_sigs=(),
        security_coin=_security_coin(),
        ephemeral_sk=_ephemeral_sk_bytes(),
        claim_fee=_FEE,
    )
    kwargs.update(overrides)
    return claim.build_and_push_claim(fake, NET, **kwargs)


def test_build_and_push_claim_happy_path_accepts():
    pytest.importorskip("chia_rs")
    fake = FakeCoinset()
    fake.push_result = "SUCCESS"

    push = _push_claim(fake)

    assert push.accepted is True
    assert push.status == "SUCCESS"
    assert isinstance(push.claim, drivers.ClaimBundle)
    assert len(fake.pushed) == 1
    assert set(fake.pushed[0]) == {"coin_spends", "aggregated_signature"}


def test_build_and_push_claim_reports_mempool_conflict_without_raising():
    pytest.importorskip("chia_rs")
    fake = FakeCoinset()
    fake.push_result = CoinsetError("push_tx failed: DOUBLE_SPEND")

    push = _push_claim(fake)

    # A conflict is a re-sync signal, not an exception.
    assert push.accepted is False
    assert "DOUBLE_SPEND" in push.status
    assert isinstance(push.claim, drivers.ClaimBundle)


def test_build_and_push_claim_surfaces_terminal_driver_error():
    pytest.importorskip("chia_rs")
    fake = FakeCoinset()
    # A mis-sized security coin trips a fail-closed anchor in the driver: terminal,
    # and nothing is ever pushed.
    with pytest.raises(drivers.WarpDriverError, match="security coin amount"):
        _push_claim(fake, security_coin=_security_coin(amount=123))
    assert fake.pushed == []
