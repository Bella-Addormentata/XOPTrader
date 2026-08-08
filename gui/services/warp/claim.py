"""Chia-side claim orchestration for the warp.green USDC(Base) -> wUSDC.b bridge.

Once a deposit is attested (the watcher reports ``status == "sent"``) and the
validator BLS signatures are collected from Nostr, the bot must:

1. **Sync the portal singleton.** The portal is a singleton whose puzzle hash
   changes every spend, because its inner puzzle is curried with
   ``last_chains_and_nonces`` (the ``(chain, nonce)`` pairs processed in that
   spend). Empirically (mainnet trace) the rule is a *full replacement*: a
   portal coin's ``last_chains_and_nonces`` equals **exactly** the
   ``new_chains_and_nonces`` list from the solution of the spend that created it
   -- never accumulated, never a per-chain high-water mark. That makes sync
   purely structural: from a recent hint coin id (a validator ``c`` tag or the
   persisted last-known portal coin) walk forward, computing each amount-1 child
   coin id from its parent's solution, until an unspent coin -- the current
   portal -- is reached. It is self-verifying: a wrong reconstruction yields a
   coin id that is not on chain (the walk stalls) and, at the tip, the
   reconstructed puzzle hash is compared against the on-chain coin (fail-closed).

2. **Build and push the five-spend claim** via :mod:`drivers`, which re-checks
   every correctness anchor (portal reconstruction, cat_minter == attested
   destination, wrapped-TAIL == configured asset id) and refuses on any miss.

3. **Confirm completion** by polling the predicted final CAT coin id. Every
   failure mode is stuck-not-lost: the attested message is claimable forever and
   the bot alone holds the ephemeral security key, so :func:`build_and_push_sweep`
   always recovers the funding coin (e.g. if a third party claimed the same
   attested message first -- they pay the *same* attested receiver, so the
   deposit still lands and only our funding needs sweeping back).

Every chain read/write goes through an injected :class:`~coinset.CoinsetClient`,
so the state machine and its tests need no network. ``chia_rs`` is imported
lazily (only to derive the security coin's puzzle hash from its private key), so
importing this module stays cheap.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

from . import clvm_utils as cu
from . import drivers
from .coinset import Coin as CoinsetCoin
from .coinset import CoinsetClient, CoinRecord, CoinsetError
from .constants import WarpNet

SExp = cu.SExp

# A hint that is at most this many portal spends behind the tip is walked
# forward structurally; a staler hint raises so the caller can re-hint from a
# fresh validator ``c`` tag (which is always at/near the tip). Bounds sync
# latency: each hop costs one or two coinset reads.
DEFAULT_MAX_HOPS = 64


class WarpClaimError(RuntimeError):
    """A claim step failed in a way the caller must handle."""


class PortalSyncError(WarpClaimError):
    """The portal singleton could not be synced to its current unspent coin.

    Retryable: usually a stale/wrong hint (re-hint from a validator ``c`` tag) or
    transient chain lag. A *reconstruction mismatch* at the tip additionally
    signals the validator/multisig set changed or the solution shape drifted --
    the service surfaces that as a blocked state, never a fund move.
    """


# --------------------------------------------------------------------------- #
# Value types.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class PortalState:
    """The current unspent portal singleton plus the lineage a claim needs."""

    coin: drivers.Coin
    coin_id: bytes
    last_chains_and_nonces: Tuple[Tuple[bytes, bytes], ...]
    parent_parent_info: bytes
    parent_inner_puzzle_hash: Optional[bytes]  # None => parent is the launcher (eve)


@dataclass(frozen=True)
class ClaimPush:
    """Outcome of building + pushing a claim bundle.

    ``accepted`` is ``True`` when the node took the bundle (or it was already in
    the mempool from a prior tick). ``accepted`` is ``False`` on a mempool
    *conflict* (the portal advanced under us or a third party consumed the
    nonce): the caller should re-sync and either rebuild or, if the deposit has
    provably landed elsewhere, sweep. Transport failures raise instead.
    """

    claim: drivers.ClaimBundle
    accepted: bool
    status: str


# --------------------------------------------------------------------------- #
# Small helpers.
# --------------------------------------------------------------------------- #

def _hx(value) -> str:
    """Coerce bytes or a (``0x``-optional) hex string to lower-case no-``0x`` hex."""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).hex()
    s = str(value).strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower()


def _driver_coin(coin: CoinsetCoin) -> drivers.Coin:
    """Convert a coinset (hex) coin to the drivers' bytes-native coin."""
    return drivers.Coin(
        bytes.fromhex(coin.parent_coin_info),
        bytes.fromhex(coin.puzzle_hash),
        int(coin.amount),
    )


def _as_list(node) -> list:
    """Flatten a CLVM proper list into a Python list of its elements."""
    node = SExp.to(node)
    out: list = []
    while node.pair is not None:
        out.append(node.pair[0])
        node = node.pair[1]
    return out


def _parse_last_chains_and_nonces(solution) -> Tuple[Tuple[bytes, bytes], ...]:
    """Extract ``new_chains_and_nonces`` from a portal singleton spend solution.

    Solution shape (``solution_for_singleton``):
    ``[lineage_proof, amount, inner_solution]`` and
    ``inner_solution = [update, new_chains_and_nonces, messages]``. Each entry of
    ``new_chains_and_nonces`` is a cons pair ``(chain . nonce)``. This list *is*
    the ``last_chains_and_nonces`` curried into the child portal (full-replace
    rule), so it both advances the walk and reconstructs the child puzzle hash.
    """
    top = _as_list(solution)
    if len(top) < 3:
        raise PortalSyncError("portal solution has an unexpected shape")
    inner = _as_list(top[2])
    if len(inner) < 2:
        raise PortalSyncError("portal inner-solution has an unexpected shape")
    out: List[Tuple[bytes, bytes]] = []
    for entry in _as_list(inner[1]):
        node = SExp.to(entry)
        if node.pair is None or node.pair[0].pair is not None or node.pair[1].pair is not None:
            raise PortalSyncError("chains_and_nonces entry is not a (chain . nonce) pair")
        out.append((bytes(node.pair[0].atom), bytes(node.pair[1].atom)))
    return tuple(out)


def _portal_full_puzzle_hash(
    net: WarpNet, last_chains_and_nonces: Sequence[Tuple[bytes, bytes]]
) -> bytes:
    """Tree hash of the portal singleton for a given ``last_chains_and_nonces``."""
    launcher_id = bytes.fromhex(net.portal_launcher_id)
    update_ph = drivers.get_portal_update_puzzle_hash(net)
    inner = drivers.get_portal_receiver_inner_puzzle(
        launcher_id,
        net.signature_threshold,
        net.validator_bls_keys,
        update_ph,
        last_chains_and_nonces,
    )
    return cu.sha256tree(drivers.puzzle_for_singleton(launcher_id, inner))


def _singleton_inner(full_puzzle) -> Tuple[Optional[object], bool]:
    """Return ``(inner_puzzle, True)`` if ``full_puzzle`` is a singleton, else ``(None, False)``.

    Used to hash a portal *parent's* inner puzzle for the lineage proof. A
    non-singleton parent means the launcher itself (the eve case).
    """
    un = cu.uncurry(full_puzzle)
    if un is None:
        return None, False
    module, args = un
    if len(args) < 2 or cu.sha256tree(module) != drivers.SINGLETON_MOD_HASH:
        return None, False
    return args[1], True


# --------------------------------------------------------------------------- #
# Portal sync (structural forward walk).
# --------------------------------------------------------------------------- #

def _structural_child(coinset: CoinsetClient, net: WarpNet, coin_id: str, height: int) -> str:
    """Coin id of the amount-1 singleton child of a spent portal coin.

    Reads the spent coin's solution, derives the child's ``last_chains_and_nonces``
    (full-replace rule), reconstructs the child puzzle hash, and names the coin.
    """
    sol = coinset.get_puzzle_and_solution(coin_id, height)
    child_lcn = _parse_last_chains_and_nonces(cu.program_from_hex(sol.solution))
    child_ph = _portal_full_puzzle_hash(net, child_lcn)
    return cu.coin_name(bytes.fromhex(_hx(coin_id)), child_ph, 1).hex()


def _walk_to_tip(
    coinset: CoinsetClient, net: WarpNet, hint: str, max_hops: int
) -> Tuple[str, CoinRecord]:
    """Follow the singleton lineage from ``hint`` to the current unspent portal."""
    cur = hint
    for _ in range(max_hops):
        rec = coinset.get_coin_record_by_name(cur)
        if rec is None:
            raise PortalSyncError(f"portal coin {cur} not found (stale or wrong hint)")
        if not rec.spent:
            return cur, rec
        cur = _structural_child(coinset, net, cur, rec.spent_block_index)
    raise PortalSyncError(
        f"portal did not reach an unspent tip within {max_hops} hops of {hint}"
    )


def sync_portal(
    coinset: CoinsetClient,
    net: WarpNet,
    *,
    hint: bytes,
    max_hops: int = DEFAULT_MAX_HOPS,
) -> PortalState:
    """Resolve the current portal singleton and the lineage a claim needs.

    ``hint`` is a recent portal coin id -- a validator ``c`` tag (always at/near
    the tip) or the persisted last-known portal coin. Walks forward to the
    unspent tip, derives its lineage from its parent's spend, and refuses
    (fail-closed) unless the reconstructed puzzle hash matches the on-chain coin.
    """
    tip_id, tip_rec = _walk_to_tip(coinset, net, _hx(hint), max_hops)
    coin = _driver_coin(tip_rec.coin)
    if coin.amount != 1:
        raise PortalSyncError(f"portal tip {tip_id} has amount {coin.amount}, expected 1")

    parent_id = tip_rec.coin.parent_coin_info
    parent_rec = coinset.get_coin_record_by_name(parent_id)
    if parent_rec is None or not parent_rec.spent:
        raise PortalSyncError(f"portal parent {parent_id} not found or not spent")
    parent_sol = coinset.get_puzzle_and_solution(parent_id, parent_rec.spent_block_index)

    inner, is_singleton = _singleton_inner(cu.program_from_hex(parent_sol.puzzle_reveal))
    parent_inner_ph = cu.sha256tree(inner) if is_singleton else None
    last_cn = _parse_last_chains_and_nonces(cu.program_from_hex(parent_sol.solution))

    if _portal_full_puzzle_hash(net, last_cn) != coin.puzzle_hash:
        raise PortalSyncError(
            "reconstructed portal puzzle hash does not match the on-chain tip -- "
            "validator/multisig set changed or the solution shape drifted"
        )

    return PortalState(
        coin=coin,
        coin_id=coin.name(),
        last_chains_and_nonces=last_cn,
        parent_parent_info=bytes.fromhex(parent_rec.coin.parent_coin_info),
        parent_inner_puzzle_hash=parent_inner_ph,
    )


# --------------------------------------------------------------------------- #
# Validator digest (keyed to the current portal coin id).
# --------------------------------------------------------------------------- #

def validator_digest(
    net: WarpNet,
    portal_state: PortalState,
    *,
    nonce: bytes,
    source: bytes,
    destination: bytes,
    contents: Sequence[bytes],
) -> bytes:
    """The digest each validator BLS-signs for this message + current portal coin."""
    return drivers.get_validator_message_digest(
        net.source_chain.encode(),
        nonce,
        source,
        destination,
        list(contents),
        portal_state.coin_id,
        bytes.fromhex(net.agg_sig_extra_data),
    )


# --------------------------------------------------------------------------- #
# Security-coin funding (dedupe-scan, never blind re-send).
# --------------------------------------------------------------------------- #

def security_coin_puzzle_hash(ephemeral_sk: bytes) -> bytes:
    """p2 puzzle hash the bot funds and the claim spends as the security coin."""
    from chia_rs import PrivateKey

    sk = PrivateKey.from_bytes(bytes(ephemeral_sk))
    return cu.sha256tree(drivers.p2_puzzle_for_pk(bytes(sk.get_g1())))


def find_security_coin(
    coinset: CoinsetClient, ephemeral_sk: bytes, expected_amount: int
) -> Optional[drivers.Coin]:
    """Locate the funded, unspent security coin, or ``None`` if not landed yet.

    Matching on the exact expected amount (``post_tip + claim_fee``) makes
    funding idempotent across a crash/resume: the caller scans first and only
    ``send_transaction``s when this returns ``None``.
    """
    ph = security_coin_puzzle_hash(ephemeral_sk)
    for rec in coinset.get_coin_records_by_puzzle_hash(_hx(ph), include_spent=False):
        if int(rec.coin.amount) == expected_amount:
            return _driver_coin(rec.coin)
    return None


# --------------------------------------------------------------------------- #
# Push (idempotent; classifies mempool conflicts vs transport failures).
# --------------------------------------------------------------------------- #

def _push_bundle(coinset: CoinsetClient, bundle_json: dict) -> Tuple[str, str]:
    """Push a bundle; return ``(kind, status)``.

    ``kind`` is ``"accepted"`` (node took it), ``"pending"`` (already in the
    mempool -- an idempotent re-push), or ``"conflict"`` (mempool rejection: a
    coin was spent under us). A genuine transport failure re-raises so the caller
    backs off rather than misreading it as a conflict.
    """
    try:
        status = coinset.push_tx(bundle_json)
        return "accepted", status
    except CoinsetError as exc:
        text = str(exc)
        low = text.lower()
        if "request failed" in low:  # transport, not a mempool verdict
            raise
        if any(t in low for t in ("already", "pending", "including")):
            return "pending", text
        return "conflict", text


def build_and_push_claim(
    coinset: CoinsetClient,
    net: WarpNet,
    *,
    portal_state: PortalState,
    nonce: bytes,
    source: bytes,
    destination: bytes,
    contents: Sequence[bytes],
    sig_switches: Sequence[bool],
    validator_sigs: Sequence[bytes],
    security_coin: drivers.Coin,
    ephemeral_sk: bytes,
    claim_fee: int,
) -> ClaimPush:
    """Assemble the five-spend claim from synced state and push it (idempotent).

    Raises :class:`drivers.WarpDriverError` (terminal) on any correctness-anchor
    miss, and re-raises transport failures for backoff. A mempool conflict is
    reported via ``ClaimPush.accepted == False`` for the caller to re-sync.
    """
    req = drivers.ClaimRequest(
        net=net,
        portal_coin=portal_state.coin,
        portal_parent_parent_info=portal_state.parent_parent_info,
        portal_parent_inner_puzzle_hash=portal_state.parent_inner_puzzle_hash,
        last_chains_and_nonces=portal_state.last_chains_and_nonces,
        validator_sigs=list(validator_sigs),
        sig_switches=list(sig_switches),
        nonce=nonce,
        source=source,
        destination=destination,
        contents=list(contents),
        security_coin=security_coin,
        ephemeral_sk=ephemeral_sk,
        claim_fee=claim_fee,
    )
    claim = drivers.build_claim_bundle(req)
    kind, status = _push_bundle(coinset, claim.bundle.to_json())
    return ClaimPush(claim=claim, accepted=kind in ("accepted", "pending"), status=status)


def build_and_push_sweep(
    coinset: CoinsetClient,
    net: WarpNet,
    *,
    security_coin: drivers.Coin,
    destination_puzzle_hash: bytes,
    ephemeral_sk: bytes,
    sweep_fee: int = 0,
) -> Tuple[str, str]:
    """Recover the funding coin back to the bot wallet (idempotent).

    A conflict means the security coin was already spent (our claim landed, or a
    prior sweep) -- reported in the status rather than raised, since there is
    nothing left to recover. Transport failures still raise.

    Returns ``(kind, status)``. ``kind`` is the push classification --
    ``"accepted"``, ``"pending"``, or ``"conflict"``, all of which mean the sweep
    is *resolved* and there is nothing further to recover. The caller needs this
    to decide whether a FAILED job may be closed; it used to be discarded, which
    is why sweeping a failed job never freed the single active-job slot.
    """
    bundle = drivers.build_sweep_bundle(
        net, security_coin, destination_puzzle_hash, ephemeral_sk, sweep_fee
    )
    return _push_bundle(coinset, bundle.to_json())


# --------------------------------------------------------------------------- #
# Completion checks.
# --------------------------------------------------------------------------- #

def claim_landed(coinset: CoinsetClient, final_cat_coin_id: bytes) -> bool:
    """``True`` once the receiver's predicted final CAT coin exists on chain.

    The precise, per-claim COMPLETED signal: this coin id is derived from our own
    bundle, so its presence means *our* claim confirmed.
    """
    return coinset.get_coin_record_by_name(_hx(final_cat_coin_id)) is not None


def security_coin_spent(coinset: CoinsetClient, security_coin_id: bytes) -> bool:
    """``True`` once the security coin is spent (by our claim or a sweep)."""
    rec = coinset.get_coin_record_by_name(_hx(security_coin_id))
    return rec is not None and rec.spent


def message_coin_puzzle_hash(
    net: WarpNet,
    *,
    nonce: bytes,
    source: bytes,
    destination: bytes,
    contents: Sequence[bytes],
) -> bytes:
    """Puzzle hash of the portal message coin for one attested message.

    Every curried field -- the portal launcher id, the warp source chain, the
    Base bridge source, the nonce, the destination, and the message hash -- is
    fixed by the attestation, and **nothing about the claimer appears in it**.
    Only the coin's *parent* varies (it is whichever portal coin the claimer
    spent), so the puzzle hash is a stable, relayer-independent fingerprint for
    "this specific message was claimed by somebody".
    """
    message_hash = cu.sha256tree(drivers.SExp.to(list(contents)))
    puzzle = drivers.get_message_coin_puzzle(
        bytes.fromhex(net.portal_launcher_id),
        net.source_chain.encode(),
        bytes(source),
        bytes(nonce),
        bytes(destination),
        message_hash,
    )
    return cu.sha256tree(puzzle)


def message_claimed_on_chain(
    coinset: CoinsetClient,
    net: WarpNet,
    *,
    nonce: bytes,
    source: bytes,
    destination: bytes,
    contents: Sequence[bytes],
) -> bool:
    """``True`` once *this* message's coin has been **spent** -- the mint ran.

    Replaces an earlier check that compared a count of *all* the receiver's
    wrapped-asset coins against a baseline. That was unsound here: XOPTrader
    trades wUSDC.b continuously, so any ordinary trade or change output landing
    on the receiver puzzle hash made the count grow and could mark a still-
    unclaimed bridge COMPLETED. This check is per-nonce and cannot be tripped by
    unrelated wallet activity.

    **Spent, not merely existing.** The message coin is created by the portal
    spend but consumed by a *separate* spend, and only that consumption enforces
    the payout. A relayer may advance the portal -- burning our nonce so our
    claim can never land -- without funding the minter coin the mint requires,
    leaving the message coin created and unspent while nothing was minted.
    Treating existence as proof would close such a job as COMPLETED and abandon
    a live deposit. Consuming the coin, by contrast, requires concurrently
    spending a coin whose puzzle hash equals the curried ``destination``, which
    is anchored to the cat_minter hash whose puzzle forces mint-and-payout to
    the attested receiver. So ``spent`` proves the mint; ``exists`` does not.

    Fails *safe* in both directions: the message coin has ``amount = 0``, so if
    the coinset index omits zero-amount coins this returns ``False``, and the
    caller stays in ``CLAIMING`` and re-syncs rather than falsely completing.
    """
    ph = message_coin_puzzle_hash(
        net, nonce=nonce, source=source, destination=destination, contents=contents
    )
    records = coinset.get_coin_records_by_puzzle_hash(_hx(ph), include_spent=True)
    return any(getattr(r, "spent", False) for r in records)
