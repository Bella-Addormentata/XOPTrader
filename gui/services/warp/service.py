"""Background bridge orchestrator for warp.green USDC(Base) -> wUSDC.b(Chia).

This module is the brain of the automatic bridge. The operator sends USDC to the
app-controlled Base hot wallet from anywhere; XOPTrader then, entirely in the
background, detects the deposit, approves + calls ``bridgeToChia``, waits for the
validator attestation, collects 6-of-10 BLS signatures from Nostr, and builds and
pushes the Chia claim spend, so wUSDC.b (the bot's primary quote asset) lands in
the bot's Chia wallet with no step-by-step operator interaction.

Two layers, cleanly split so the logic is testable without Qt or a network:

* :class:`WarpEngine` -- a **pure synchronous state machine**. Every dependency
  (EVM/coinset/watcher/wallet clients, the Nostr collector, the keystore
  protector, the job store, the clock) is injected, so a test drives it with
  fakes and zero I/O. It performs at most one bounded, idempotent step per
  :meth:`~WarpEngine.step` call.
* :class:`WarpService` + :class:`_WarpWorker` -- a thin Qt shell cloned verbatim
  from :mod:`gui.services.wallet_service`: the worker is ``moveToThread``-ed onto
  a background ``QThread`` and driven by queued ``_trigger_*`` signals, and the
  service exposes a mutex-guarded snapshot cache. The heavy warp submodules and
  the engine itself are built lazily *on the worker thread*, so importing this
  module (which the GUI does at boot) pulls in only stdlib + Qt + the inert
  ``jobs``/``constants`` modules -- never ``chia_rs``/``clvm``/``web3``.

Crash-safety is the design's spine (a GUI relaunch hard-kills the process). The
machine is **persist-then-act**: each state persists its intent *before* or
*with* its one side effect, so a resume re-derives the same action:

* EVM txs are **two-phase** -- tick 1 signs and persists the raw bytes (no
  broadcast); tick 2 broadcasts idempotently and polls the receipt. The raw is
  durable before any broadcast, and rebroadcast is a no-op ("already known"), so
  a crash at any instant re-broadcasts the *same* signed tx rather than
  re-signing at a new nonce.
* Claim funding **dedupe-scans** (coinset by security puzzle hash + wallet
  ``get_transactions``) before it ever ``send_transaction``s, so a crash between
  send and persist never double-funds.
* The per-job ephemeral BLS key is local randomness persisted with the advance;
  a crash before persistence simply regenerates one (nothing was funded to the
  discarded key).

Error taxonomy (raised by handlers, classified by the dispatcher):

* :class:`WarpPending` -- a normal "not yet" wait (deposit/confirmations/watcher/
  signatures pending). Folded into a stay: re-checked next poll, no retry bump,
  ``last_error`` cleared. Funds never move on a pending.
* :class:`WarpRetryable` (and injected client errors) -- a transient failure
  (transport, RPC, low gas). Stay with exponential backoff
  ``min(15s * 2**(n-1), 600s)``, ``retry_count`` bumped, ``last_error`` set.
* :class:`WarpTerminal` (and ``drivers.WarpDriverError``) -- an anchor mismatch or
  a provably unrecoverable condition. The job goes ``FAILED`` (holding the slot
  for the operator to Retry/Sweep); the attested message stays claimable forever
  and the ephemeral funding is always sweepable, so terminal is stuck-not-lost.
"""

from __future__ import annotations

import logging
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from PySide6.QtCore import (
    QMutex,
    QMutexLocker,
    QObject,
    QThread,
    Signal,
    Slot,
)

from . import constants
from . import jobs
from .jobs import JobStatus, WarpJob, WarpJobStore

_log: logging.Logger = logging.getLogger(__name__)


# --------------------------------------------------------------------------- #
# Error taxonomy.
# --------------------------------------------------------------------------- #

class WarpError(RuntimeError):
    """Base class for warp orchestration errors."""


class WarpPending(WarpError):
    """A normal "not yet" wait -- folded into a stay, never an error.

    Raised when a step is healthy but blocked on external progress (a deposit,
    block confirmations, the watcher, or a signature quorum). The dispatcher
    re-checks next poll without bumping ``retry_count`` or setting ``last_error``.
    """


class WarpRetryable(WarpError):
    """A transient failure -- stay in state and back off exponentially."""


class WarpTerminal(WarpError):
    """An unrecoverable, fail-closed condition -- move the job to ``FAILED``."""


# --------------------------------------------------------------------------- #
# Tuning constants.
# --------------------------------------------------------------------------- #

_BACKOFF_BASE_S: float = 15.0
_BACKOFF_CAP_S: float = 600.0
_APPROVE_MAX_ATTEMPTS: int = 3
_BRIDGE_MAX_ATTEMPTS: int = 2
# Conflicting claim pushes that show no evidence the mint ran. A portal race
# resolves in one or two rounds; beyond this the nonce is contested for a reason
# we cannot see, and looping forever would leave the operator with no escape
# (neither Sweep nor Cancel is offered in CLAIMING). Fail into FAILED, which is.
_CLAIM_CONFLICT_MAX_ROUNDS: int = 10
_SIG_COLLECT_DEADLINE_S: float = 12.0

# ETH floor (wei) the hot wallet must hold before we sign approve/bridge: covers
# the Portal message toll (1e13 wei) plus a comfortable Base gas margin.
_MIN_GAS_WEI: int = 300_000_000_000_000  # 0.0003 ETH

# Jobs may only be cancelled before the irreversible on-chain bridge is signed.
_CANCELLABLE = frozenset(
    {JobStatus.AWAITING_DEPOSIT, JobStatus.DEPOSIT_SEEN, JobStatus.APPROVING}
)

# Stand-in for a funding transaction the wallet accepted without returning an
# id. What matters downstream is not the id itself -- the security coin is
# located by puzzle hash and amount -- but that ``funding_tx_id`` stays truthy,
# because that is the flag saying "already sent, do not send again".
_FUNDING_TX_UNKNOWN: str = "sent-id-unknown"


# --------------------------------------------------------------------------- #
# Parameters (parsed from the null-safe config dict).
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class WarpParams:
    """Operator-tunable warp settings, resolved from ``config['warp']``.

    Defaults are deliberately inert (``enabled``/``auto_bridge`` false and
    ``dry_run`` true), so merging the feature changes nothing for the live bot
    until explicitly turned on.

    ``min_micros``/``max_micros`` are in USDC base units (``usdc * 10**decimals``),
    pre-multiplied so the hot path is integer-only. They are **not** symmetric:
    ``min_micros`` is an *auto-bridge* floor only (it exists to avoid bridging
    dust automatically, which is meaningless for an explicit operator click),
    while ``max_micros`` is the blast-radius cap and applies to manual bridges
    too. See :meth:`WarpEngine._h_awaiting_deposit`.
    """

    enabled: bool = False
    dry_run: bool = True
    auto_bridge: bool = False
    base_rpc_url: str = ""
    min_micros: int = 0
    max_micros: int = 0
    claim_fee_mojos: int = 100_000_000
    chia_funding_fee_mojos: int = 0
    poll_interval_s: float = 15.0
    chia_receiver_address: str = ""
    expected_asset_id: str = ""
    portal_hint: Optional[str] = None


def warp_params_from_config(config: Optional[dict]) -> WarpParams:
    """Build :class:`WarpParams` from a config snapshot (all reads null-safe)."""
    warp = (config or {}).get("warp") or {}
    # Loud, not silent: an operator with a stale `testnet: true` would otherwise
    # get full mainnet behaviour -- real USDC, real contracts -- under a config
    # that reads as a rehearsal. Truthiness only, so a leftover `testnet: false`
    # stays harmless.
    if warp.get("testnet"):
        raise WarpError(
            "warp.testnet is no longer supported: the Base Sepolia deployment "
            "could never bridge (no wired USDC) and ran with the wrapped-asset "
            "anchor disabled. Remove the key and rehearse with warp.dry_run: true"
        )
    net = constants.MAINNET
    scale = 10 ** int(net.usdc_decimals)
    min_usdc = float(warp.get("min_auto_bridge_usdc", 0) or 0)
    max_usdc = float(warp.get("max_auto_bridge_usdc", 0) or 0)
    return WarpParams(
        enabled=bool(warp.get("enabled", False)),
        dry_run=bool(warp.get("dry_run", True)),
        auto_bridge=bool(warp.get("auto_bridge", False)),
        base_rpc_url=str(warp.get("base_rpc_url", "") or ""),
        min_micros=int(round(min_usdc * scale)),
        max_micros=int(round(max_usdc * scale)),
        claim_fee_mojos=int(warp.get("claim_fee_mojos", 100_000_000) or 0),
        chia_funding_fee_mojos=int(warp.get("chia_funding_fee_mojos", 0) or 0),
        poll_interval_s=float(warp.get("poll_interval_s", 15) or 15),
        chia_receiver_address=str(warp.get("chia_receiver_address", "") or ""),
        expected_asset_id=str(warp.get("expected_asset_id", "") or ""),
        portal_hint=(str(warp.get("portal_hint")) if warp.get("portal_hint") else None),
    )


# --------------------------------------------------------------------------- #
# Step result + module helpers.
# --------------------------------------------------------------------------- #

@dataclass(frozen=True)
class _Step:
    """A handler's returned intent: advance (``next_status`` set) or stay."""

    next_status: Optional[str] = None
    columns: Optional[Dict[str, Any]] = None
    state: Optional[Dict[str, Any]] = None
    message: str = ""


def _advance(
    next_status: str,
    *,
    columns: Optional[Dict[str, Any]] = None,
    state: Optional[Dict[str, Any]] = None,
    message: str = "",
) -> _Step:
    return _Step(next_status=next_status, columns=columns, state=state, message=message)


def _stay(
    *,
    columns: Optional[Dict[str, Any]] = None,
    state: Optional[Dict[str, Any]] = None,
    message: str = "",
) -> _Step:
    return _Step(next_status=None, columns=columns, state=state, message=message)


def _hx(value: Any) -> str:
    """Coerce bytes or a (``0x``-optional) hex string to lower-case no-``0x`` hex."""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).hex()
    s = str(value or "").strip()
    if s[:2].lower() == "0x":
        s = s[2:]
    return s.lower()


def _word(value: Any) -> str:
    """Left-pad hex to a 32-byte EVM word, for width-agnostic comparison.

    warp messages carry every content item as a ``bytes32``, so the attested
    ERC-20 source arrives as 12 zero bytes followed by the 20-byte address,
    while :attr:`WarpNet.usdc_address` is the bare 20-byte address. Comparing
    those with :func:`_hx` alone is a 64-char-vs-40-char string comparison that
    can never be equal -- which failed every mainnet job at ``MESSAGE_SENT``.
    """
    return _hx(value).rjust(64, "0")


def _encode_sigs(collected: Dict[int, bytes]) -> Dict[str, str]:
    """Serialize ``{index: sig}`` to JSON-safe ``{"index": "hexsig"}``."""
    return {str(i): bytes(s).hex() for i, s in collected.items()}


def _decode_sigs(raw: Any) -> Dict[int, bytes]:
    """Inverse of :func:`_encode_sigs`; tolerates ``None`` (a wiped/absent key)."""
    if not raw:
        return {}
    out: Dict[int, bytes] = {}
    for i, s in raw.items():
        if s:
            out[int(i)] = bytes.fromhex(str(s))
    return out


def _job_dict(job: Optional[WarpJob]) -> Optional[dict]:
    """A GUI-serializable view of a job (no chain objects, all JSON-native)."""
    if job is None:
        return None
    return {
        "id": job.id,
        "status": job.status,
        "network": job.network,
        "created_at": job.created_at,
        "updated_at": job.updated_at,
        "amount_usdc_micros": job.amount_usdc_micros,
        "amount_mojos": job.amount_mojos,
        "post_tip_mojos": job.post_tip_mojos,
        "receiver_address": job.receiver_address,
        "bridge_tx_hash": job.bridge_tx_hash,
        "bridge_nonce": job.bridge_nonce,
        "retry_count": job.retry_count,
        "last_error": job.last_error,
        "next_retry_at": job.next_retry_at,
    }


# --------------------------------------------------------------------------- #
# The state machine.
# --------------------------------------------------------------------------- #

class WarpEngine:
    """Pure, injected, crash-resumable warp bridge state machine.

    Construct with every dependency injected (see :meth:`__init__`); drive with
    :meth:`step`, which advances the single active job by at most one bounded,
    idempotent step and returns a serializable job view. No Qt, no globals, no
    hidden I/O -- the tests build it with fakes and a stub clock.
    """

    def __init__(
        self,
        net: constants.WarpNet,
        store: WarpJobStore,
        *,
        params: WarpParams,
        evm: Any,
        coinset: Any,
        watcher: Any,
        wallet: Any,
        collector: Any,
        evm_key: Any,
        protector: Any,
        clock: Optional[Callable[[], float]] = None,
        nostr_fetcher: Optional[Callable[[str, dict, float], List[dict]]] = None,
    ) -> None:
        self._net = net
        self._store = store
        self._params = params
        self._evm = evm
        self._coinset = coinset
        self._watcher = watcher
        self._wallet = wallet
        self._collector = collector
        self._evm_key = evm_key
        self._protector = protector
        self._nostr_fetcher = nostr_fetcher
        self._clock = clock or time.time
        self._hot_address = evm_key.address
        self._hot_cache: Dict[str, Any] = {"address": evm_key.address}
        self._terminal_cache: Optional[tuple] = None
        # Set when the persisted active job was frozen against a different
        # network or hot wallet than the one now configured; while set, the
        # engine refuses to touch that job (see _binding_mismatch).
        self._binding_error: Optional[str] = None
        # Cached wallet-derived receiver, so snapshot() never does I/O and we
        # never burn a fresh derivation index per call: (address, source, ph_hex).
        self._receiver_cache: Optional[tuple] = None
        self._receiver_snap: Optional[Dict[str, Any]] = None

        self._HANDLERS: Dict[str, Callable[[WarpJob], _Step]] = {
            JobStatus.AWAITING_DEPOSIT: self._h_awaiting_deposit,
            JobStatus.DEPOSIT_SEEN: self._h_deposit_seen,
            JobStatus.APPROVING: self._h_approving,
            JobStatus.BRIDGING: self._h_bridging,
            JobStatus.BRIDGE_CONFIRMED: self._h_bridge_confirmed,
            JobStatus.MESSAGE_SENT: self._h_message_sent,
            JobStatus.FUNDING_CLAIM: self._h_funding_claim,
            JobStatus.CLAIM_FUNDED: self._h_claim_funded,
            JobStatus.COLLECTING_SIGS: self._h_collecting_sigs,
            JobStatus.CLAIMING: self._h_claiming,
        }

    # -- clock -------------------------------------------------------------- #

    def _now(self) -> float:
        return float(self._clock())

    # ================================================================== #
    # Dispatcher.
    # ================================================================== #

    def step(self) -> Optional[dict]:
        """Advance the active job by one bounded step; return its updated view.

        Returns ``None`` when there is nothing to do (no active job and no
        auto-bridge candidate). Never raises: every handler exception is
        classified into a stay/retry/terminal persistence.
        """
        job = self._store.get_active_job()
        if job is None:
            self._binding_error = None  # nothing left to be mismatched about
            self.maybe_start_auto_job()
            job = self._store.get_active_job()
            if job is None:
                return None

        # Binding gate, evaluated before anything else so the banner explains a
        # foreign job even when it is FAILED (and therefore terminal): never
        # process a job frozen under a different network or hot wallet.
        # Reopening the shared job DB after a key rotation would otherwise
        # resume a job whose amounts, allowance and receiver were all computed
        # for wallet A while signing from wallet B. Read-only on mismatch -- no
        # writes, no backoff bump, no side effects.
        mismatch = self._binding_mismatch(job)
        if mismatch != self._binding_error:
            if mismatch:
                _log.error("warp job %s refused: %s", job.id, mismatch)
            self._binding_error = mismatch
        if mismatch:
            return _job_dict(job)

        if job.status in jobs.TERMINAL_STATES:
            return _job_dict(job)

        # Backoff gate: a prior stay/retry scheduled the next attempt.
        if job.next_retry_at:
            try:
                due = float(job.next_retry_at)
            except (TypeError, ValueError):
                due = 0.0
            if self._now() < due:
                return _job_dict(job)

        handler = self._HANDLERS.get(job.status)
        if handler is None:
            return _job_dict(job)

        try:
            result = handler(job)
        except WarpPending as exc:
            return self._apply_stay(job, _stay(message=str(exc)))
        except Exception as exc:  # noqa: BLE001 -- classified below, never escapes
            if isinstance(exc, self._terminal_classes()):
                return self._apply_terminal(job, exc)
            return self._apply_retry(job, exc)

        if result.next_status is not None:
            return self._apply(job, result)
        return self._apply_stay(job, result)

    def _terminal_classes(self) -> tuple:
        """Lazily import + cache the fail-closed exception tuple."""
        if self._terminal_cache is None:
            from . import drivers  # heavy; imported only on first classification

            self._terminal_cache = (WarpTerminal, drivers.WarpDriverError)
        return self._terminal_cache

    # -- persistence of the four outcomes ---------------------------------- #

    def _apply(self, job: WarpJob, result: _Step) -> dict:
        """Advance: reset error bookkeeping and transition to ``next_status``."""
        columns: Dict[str, Any] = {
            "retry_count": 0,
            "last_error": None,
            "next_retry_at": None,
        }
        if result.columns:
            columns.update(result.columns)
        message = result.message or f"{job.status} -> {result.next_status}"
        updated = self._store.update_job(
            job.id,
            status=result.next_status,
            expected_status=job.status,
            columns=columns,
            state_patch=result.state,
            event=("transition", message, None),
        )
        return _job_dict(updated)

    def _apply_stay(self, job: WarpJob, result: _Step) -> dict:
        """Healthy stay/pending: schedule the next poll, clear any prior error."""
        columns: Dict[str, Any] = {
            "retry_count": 0,
            "last_error": None,
            "next_retry_at": repr(self._now() + float(self._params.poll_interval_s)),
        }
        if result.columns:
            columns.update(result.columns)
        event = ("progress", result.message, None) if result.message else None
        updated = self._store.update_job(
            job.id,
            expected_status=job.status,
            columns=columns,
            state_patch=result.state,
            event=event,
        )
        return _job_dict(updated)

    def _apply_retry(self, job: WarpJob, exc: Exception) -> dict:
        """Transient failure: exponential backoff, bump ``retry_count``."""
        n = int(job.retry_count or 0) + 1
        delay = min(_BACKOFF_BASE_S * (2 ** (n - 1)), _BACKOFF_CAP_S)
        message = str(exc)[:500]
        updated = self._store.update_job(
            job.id,
            expected_status=job.status,
            columns={
                "retry_count": n,
                "last_error": message,
                "next_retry_at": repr(self._now() + delay),
            },
            event=("error", message, None),
        )
        return _job_dict(updated)

    def _apply_terminal(self, job: WarpJob, exc: Exception) -> dict:
        """Fail-closed: move to ``FAILED`` (holds the slot for operator action)."""
        message = str(exc)[:500]
        updated = self._store.update_job(
            job.id,
            status=JobStatus.FAILED,
            expected_status=job.status,
            columns={"last_error": message, "next_retry_at": None},
            state_patch={"failed_from": job.status},
            event=("failed", message, None),
        )
        return _job_dict(updated)

    # ================================================================== #
    # State handlers (each: read -> one side effect -> returned _Step).
    # ================================================================== #

    def _h_awaiting_deposit(self, job: WarpJob) -> _Step:
        """Watch the hot wallet for a USDC deposit; freeze amounts on arrival."""
        from . import evm

        net, p = self._net, self._params

        have = self._evm.get_erc20_balance(net.usdc_address, self._hot_address)
        target = job.state.get("target_micros")
        want = min(int(target), have) if target else have

        # max_micros is the blast radius and binds everything, manual included:
        # it is what makes a mis-click on a large balance survivable, and what
        # makes the documented small-amount live test an *enforced* limit.
        clamped = False
        if p.max_micros > 0 and want > p.max_micros:
            want, clamped = p.max_micros, True

        # min_micros is an auto-bridge floor only. Applying it to an explicit
        # "Bridge now" is what made the runbook's ~$5 smoke test hang forever
        # against the shipped default of 100 USDC. An unflagged legacy row is
        # treated as automatic, i.e. the conservative side.
        manual = bool(job.state.get("manual"))
        if want <= 0:
            raise WarpPending(f"awaiting a USDC deposit (have {have})")
        if not manual and p.min_micros > 0 and want < p.min_micros:
            raise WarpPending(
                f"awaiting auto-bridge deposit >= {p.min_micros} micros (have {have})"
            )

        eth = self._evm.get_eth_balance(self._hot_address)
        if eth < _MIN_GAS_WEI:
            raise WarpRetryable(f"insufficient ETH for gas: {eth} < {_MIN_GAS_WEI} wei")

        mojo_amount = evm.bridgeable_mojo(want, net)
        if mojo_amount <= 0:
            raise WarpPending("deposit below one bridgeable mojo")
        amount_base = evm.mojo_to_base_units(mojo_amount, net)
        tip_bps = self._evm.get_tip_bps()
        post_tip = evm.post_tip_amount(mojo_amount, tip_bps)
        address, receiver_ph = self._resolve_receiver()

        clamp_note = (
            f"; clamped to the {p.max_micros}-micro cap, {have - want} left in the "
            "hot wallet for a later job"
            if clamped
            else ""
        )
        return _advance(
            JobStatus.DEPOSIT_SEEN,
            columns={
                "amount_usdc_micros": amount_base,
                "amount_mojos": mojo_amount,
                "post_tip_mojos": post_tip,
                "receiver_address": address,
                "receiver_ph": receiver_ph,
            },
            # Freeze the (network, hot wallet) identity together with the amounts
            # they were computed from; _binding_mismatch refuses to resume this
            # job under any other key. This is the last state where that is still
            # a free choice -- nothing has been signed yet.
            state={"tip_bps": tip_bps, **self._binding()},
            message=(
                f"deposit {amount_base} micros seen; bridging {mojo_amount} mojos "
                f"(post-tip {post_tip}) to {address}{clamp_note}"
            ),
        )

    def _h_deposit_seen(self, job: WarpJob) -> _Step:
        """Gas + receiver preflight before the irreversible on-chain path."""
        eth = self._evm.get_eth_balance(self._hot_address)
        if eth < _MIN_GAS_WEI:
            raise WarpRetryable(f"insufficient ETH for gas: {eth} wei")
        try:
            ok = job.receiver_ph and len(bytes.fromhex(job.receiver_ph)) == 32
        except ValueError:
            ok = False
        if not ok:
            raise WarpTerminal("frozen receiver puzzle hash is invalid")
        return _advance(JobStatus.APPROVING, message="preflight ok; approving allowance")

    def _h_approving(self, job: WarpJob) -> _Step:
        """Ensure the bridge allowance (skip if sufficient); two-phase approve."""
        from . import evm

        needed = int(job.amount_usdc_micros or 0)
        allowance = self._evm.get_bridge_allowance(self._hot_address)
        if allowance >= needed:
            return _advance(
                JobStatus.BRIDGING,
                state={"approve_raw": None},
                message=f"allowance {allowance} >= {needed}; skipping approve",
            )

        raw = job.state.get("approve_raw")
        if not raw:  # phase 1: sign + persist, no broadcast
            unsigned = self._evm.prepare_approve(
                owner=self._hot_address, amount_base_units=needed
            )
            signed = evm.sign_tx(unsigned, self._evm_key.private_key)
            return _stay(
                state={
                    "approve_raw": signed.raw.hex(),
                    "approve_tx_hash": signed.tx_hash,
                    "approve_nonce": signed.nonce,
                },
                message="approve signed; broadcasting next tick",
            )

        # Rehearsal stop: the allowance approve is signed and durable, but never
        # broadcast. Skip straight to BRIDGING so the dry run also exercises
        # prepare_bridge (live toll, gas estimate, encoding, signing).
        if self._params.dry_run:
            return _advance(
                JobStatus.BRIDGING,
                state={"approve_raw": None},
                message="dry run: approve signed, not broadcast",
            )

        # phase 2: idempotent broadcast, then poll the receipt.
        self._evm.send_raw_transaction(bytes.fromhex(raw))
        tx_hash = job.state.get("approve_tx_hash")
        receipt = self._evm.get_transaction_receipt(tx_hash) if tx_hash else None
        if receipt is None:
            return _stay(message="approve broadcast; awaiting receipt")
        if evm.receipt_reverted(receipt):
            attempt = int(job.state.get("approve_attempt", 0)) + 1
            if attempt >= _APPROVE_MAX_ATTEMPTS:
                raise WarpTerminal(f"approve reverted {attempt} times")
            return _stay(
                state={
                    "approve_raw": None,
                    "approve_tx_hash": None,
                    "approve_attempt": attempt,
                },
                message=f"approve reverted; re-preparing (attempt {attempt})",
            )
        return _advance(
            JobStatus.BRIDGING, state={"approve_raw": None}, message="approve confirmed"
        )

    def _h_bridging(self, job: WarpJob) -> _Step:
        """Two-phase ``bridgeToChia``; on success, parse the MessageSent nonce."""
        from . import evm

        net = self._net
        raw = job.state.get("bridge_raw")
        if not raw:  # phase 1: sign at a live toll/nonce + persist, no broadcast
            unsigned = self._evm.prepare_bridge(
                owner=self._hot_address,
                receiver_ph=bytes.fromhex(job.receiver_ph),
                mojo_amount=int(job.amount_mojos),
            )
            signed = evm.sign_tx(unsigned, self._evm_key.private_key)
            return _stay(
                columns={"bridge_tx_hash": signed.tx_hash},
                state={"bridge_raw": signed.raw.hex(), "bridge_tx_nonce": signed.nonce},
                message="bridge signed; broadcasting next tick",
            )

        # Rehearsal stop -- the last instant before anything becomes irreversible.
        # Everything up to here has been exercised for real: the DPAPI key, the
        # Base RPC, the wallet daemon, the receiver decode, the live message toll
        # and tip, gas estimation, nonce selection, ABI encoding and signing. The
        # only thing that does not happen is the broadcast.
        if self._params.dry_run:
            return _advance(
                JobStatus.DRY_RUN_OK,
                columns={"bridge_tx_hash": None},
                state={"bridge_raw": None, "dry_run": True},
                message=(
                    "dry run OK: both Base transactions signed but NOT broadcast; "
                    "no funds moved. Set warp.dry_run: false to go live"
                ),
            )

        # phase 2: idempotent broadcast, then poll the receipt.
        self._evm.send_raw_transaction(bytes.fromhex(raw))
        receipt = (
            self._evm.get_transaction_receipt(job.bridge_tx_hash)
            if job.bridge_tx_hash
            else None
        )
        if receipt is None:
            return _stay(message="bridge broadcast; awaiting receipt")
        if evm.receipt_reverted(receipt):
            attempt = int(job.state.get("bridge_attempt", 0)) + 1
            if attempt >= _BRIDGE_MAX_ATTEMPTS:
                raise WarpTerminal(f"bridge reverted {attempt} times")
            return _stay(
                columns={"bridge_tx_hash": None},
                state={"bridge_raw": None, "bridge_attempt": attempt},
                message=f"bridge reverted; re-preparing (attempt {attempt})",
            )
        msg_nonce = evm.parse_message_sent_nonce(receipt, net.portal_address)
        return _advance(
            JobStatus.BRIDGE_CONFIRMED,
            columns={"bridge_nonce": msg_nonce},
            state={"bridge_raw": None},
            message=f"bridged; MessageSent nonce {msg_nonce}",
        )

    def _h_bridge_confirmed(self, job: WarpJob) -> _Step:
        """Wait for the bridge tx to reach the network's confirmation depth."""
        net = self._net
        confs = self._evm.get_confirmations(job.bridge_tx_hash)
        if confs < net.evm_confirmation_min_height:
            raise WarpPending(
                f"bridge {confs}/{net.evm_confirmation_min_height} confirmations"
            )
        return _advance(
            JobStatus.MESSAGE_SENT, message=f"bridge confirmed ({confs} blocks)"
        )

    def _h_message_sent(self, job: WarpJob) -> _Step:
        """Await the watcher attestation; anchor it, then mint the ephemeral key."""
        from . import keystore

        net = self._net
        msg = self._watcher.get_message(job.bridge_nonce, source_chain=net.source_chain)
        if msg is None:
            raise WarpPending("watcher has not indexed the message yet")
        if not msg.is_sent:
            raise WarpPending(f"watcher status {msg.status!r}; awaiting 'sent'")

        # Fail-closed anchors: funds only proceed on the exact attested terms.
        if _word(msg.receiver_ph) != _word(job.receiver_ph):
            raise WarpTerminal("attested receiver != frozen receiver")
        if msg.amount_mojos is None or int(msg.amount_mojos) != int(job.post_tip_mojos):
            raise WarpTerminal(
                f"attested amount {msg.amount_mojos} != post-tip {job.post_tip_mojos}"
            )
        if _word(msg.erc20_source) != _word(net.usdc_address):
            raise WarpTerminal(
                f"attested source token {_hx(msg.erc20_source)} != configured "
                f"USDC {_hx(net.usdc_address)}"
            )

        # Persist-then-act: generate the ephemeral security key locally; it lands
        # atomically with the advance. A crash before persistence just regenerates
        # (nothing was funded to the discarded key).
        bls = keystore.new_bls_key()
        blob = keystore.protect_bls_key(
            bls, extra_entropy=self._job_entropy(job), protector=self._protector
        )
        return _advance(
            JobStatus.FUNDING_CLAIM,
            state={
                "ephemeral_blob": blob,
                "ephemeral_pk": bls.public_key.hex(),
                "message_destination": _hx(msg.destination),
                "message_contents": list(msg.contents),
            },
            message="attested 'sent'; ephemeral security key generated",
        )

    def _h_funding_claim(self, job: WarpJob) -> _Step:
        """Fund the security coin (post-tip + fee) -- dedupe-scan, never re-send."""
        from . import claim, clvm_utils as cu

        net, p = self._net, self._params
        sk = self._load_ephemeral_sk(job)
        expected = int(job.post_tip_mojos) + int(p.claim_fee_mojos)

        coin = claim.find_security_coin(self._coinset, sk, expected)
        if coin is not None:
            return _advance(
                JobStatus.CLAIM_FUNDED,
                state={"security_coin_id": coin.name().hex()},
                message=f"security coin funded ({expected} mojos)",
            )

        # Cheapest guard first: we already recorded a funding send for this job.
        # The coin is not on chain yet (checked above), so it is still in flight.
        # This marker was previously written and never read, which meant every
        # resume re-derived the decision from the history scan alone.
        prior = job.state.get("funding_tx_id")
        if prior:
            return _stay(message=f"funding tx {prior} already sent; awaiting the coin")

        security_ph = claim.security_coin_puzzle_hash(sk)
        existing = self._find_existing_funding(security_ph.hex(), expected)
        if existing:
            return _stay(
                # funding_amount travels with the id here too: a job that
                # resumes through this branch would otherwise leave every
                # later lookup recomputing the amount from live config.
                state={"funding_tx_id": existing, "funding_amount": expected},
                message="funding tx already in flight (dedupe hit)",
            )

        self._wallet.log_in()
        address = cu.encode_puzzle_hash(security_ph, net.chia_prefix)
        record = self._wallet.send_transaction(
            1, expected, address, fee_mojos=p.chia_funding_fee_mojos
        )
        # [WARP-DUP-FUND 2026-08-08] send_transaction returned, so the coin is
        # on its way whether or not the wallet named the transaction. Persisting
        # None would leave the `if prior:` guard above falsy on the next tick,
        # and the dedupe scan cannot cover the gap because the coin is not on
        # chain yet -- so the security coin would be funded a SECOND time, out
        # of the operator's wallet. Keep the guard armed with a sentinel.
        tx_id = record.get("name") or record.get("transaction_id")
        if not tx_id:
            tx_id = _FUNDING_TX_UNKNOWN
            _log.warning(
                "warp: wallet accepted the funding send for job %s but named no "
                "transaction; recording %r so the in-flight guard still holds",
                job.id,
                tx_id,
            )
        return _stay(
            # Record what was actually sent. A later Sweep must look for *this*
            # amount, not whatever claim_fee_mojos happens to say at the time.
            state={"funding_tx_id": tx_id, "funding_amount": expected},
            message=f"funded {expected} mojos to the security coin",
        )

    def _h_claim_funded(self, job: WarpJob) -> _Step:
        """Wait for the security coin to reach the Chia confirmation depth."""
        from . import claim

        net, p = self._net, self._params
        sk = self._load_ephemeral_sk(job)
        expected = int(job.post_tip_mojos) + int(p.claim_fee_mojos)

        coin = claim.find_security_coin(self._coinset, sk, expected)
        if coin is None:
            raise WarpPending("security coin not yet on chain")
        record = self._coinset.get_coin_record_by_name(coin.name().hex())
        if record is None:
            raise WarpPending("security coin not yet indexed")
        peak = self._coinset.get_peak_height()
        depth = peak - int(record.confirmed_block_index) + 1
        if depth < net.chia_confirmation_min_height:
            raise WarpPending(
                f"funding {depth}/{net.chia_confirmation_min_height} confirmations"
            )
        return _advance(
            JobStatus.COLLECTING_SIGS,
            state={"security_coin_id": coin.name().hex()},
            message=f"security coin confirmed ({depth} blocks)",
        )

    def _h_collecting_sigs(self, job: WarpJob) -> _Step:
        """Sync the portal, compute the digest, collect BLS sigs (resumable)."""
        from . import claim

        net = self._net
        hint = self._resolve_portal_hint(job)
        portal = claim.sync_portal(self._coinset, net, hint=hint)
        fresh = portal.coin_id.hex()
        self._store.set_meta("portal_hint", fresh)

        # Freshness: if the portal advanced under us, prior sigs are keyed to a
        # stale coin id and must be re-collected from scratch.
        prev = job.state.get("portal_coin_id")
        have = {} if (prev and prev != fresh) else _decode_sigs(job.state.get("sigs"))

        digest = claim.validator_digest(
            net,
            portal,
            nonce=bytes.fromhex(job.bridge_nonce),
            source=self._claim_source(),
            destination=bytes.fromhex(job.state["message_destination"]),
            contents=self._claim_contents(job),
        )
        round_ = int(job.state.get("collect_round", 0))
        result = self._collector.collect(
            nonce=bytes.fromhex(job.bridge_nonce),
            portal_coin_id=portal.coin_id,
            digest=digest,
            have=have,
            deadline_s=_SIG_COLLECT_DEADLINE_S,
            relay_offset=round_,
        )
        encoded = _encode_sigs(result.collected)
        if result.complete:
            return _advance(
                JobStatus.CLAIMING,
                state={"sigs": encoded, "portal_coin_id": fresh},
                message=f"collected {result.count}/{result.threshold} signatures",
            )
        return _stay(
            state={"sigs": encoded, "portal_coin_id": fresh, "collect_round": round_ + 1},
            message=f"collected {result.count}/{result.threshold} signatures",
        )

    def _h_claiming(self, job: WarpJob) -> _Step:
        """Re-sync, build + push the claim; handle completion / conflict / drift."""
        from . import claim, nostr

        net, p = self._net, self._params
        sk = self._load_ephemeral_sk(job)
        expected = int(job.post_tip_mojos) + int(p.claim_fee_mojos)

        # Completion first (idempotent resume): our predicted final CAT coin id is
        # derived from our own bundle, so its presence means *our* claim landed.
        final_hex = job.state.get("final_cat_coin_id")
        if final_hex and claim.claim_landed(self._coinset, bytes.fromhex(final_hex)):
            return _advance(
                JobStatus.COMPLETED, message="claim confirmed; final CAT coin on chain"
            )

        coin = claim.find_security_coin(self._coinset, sk, expected)
        if coin is None:
            sec_id = job.state.get("security_coin_id")
            if sec_id and claim.security_coin_spent(
                self._coinset, bytes.fromhex(sec_id)
            ):
                raise WarpPending("security coin spent; awaiting final CAT coin")
            raise WarpPending("security coin not found on chain")

        hint = self._resolve_portal_hint(job)
        portal = claim.sync_portal(self._coinset, net, hint=hint)
        self._store.set_meta("portal_hint", portal.coin_id.hex())
        prev = job.state.get("portal_coin_id")
        if prev and portal.coin_id.hex() != prev:
            return _advance(
                JobStatus.COLLECTING_SIGS,
                state={"sigs": None, "portal_coin_id": portal.coin_id.hex()},
                message="portal advanced under us; re-collecting signatures",
            )

        switches, sigs = nostr.sig_switches_for(net, _decode_sigs(job.state.get("sigs")))
        push = claim.build_and_push_claim(
            self._coinset,
            net,
            portal_state=portal,
            nonce=bytes.fromhex(job.bridge_nonce),
            source=self._claim_source(),
            destination=bytes.fromhex(job.state["message_destination"]),
            contents=self._claim_contents(job),
            sig_switches=switches,
            validator_sigs=sigs,
            security_coin=coin,
            ephemeral_sk=sk,
            claim_fee=p.claim_fee_mojos,
        )
        final_id = push.claim.final_cat_coin_id.hex()
        if push.accepted:
            return _stay(
                state={
                    "final_cat_coin_id": final_id,
                    "security_coin_id": coin.name().hex(),
                },
                message=f"claim pushed ({push.status}); awaiting final CAT coin",
            )

        # Conflict: the portal advanced, or a third party consumed *this* nonce.
        # In the latter case they paid the same attested receiver the same
        # attested amount, so the deposit landed regardless and only our funding
        # needs sweeping back.
        #
        # The evidence must be per-nonce. An earlier version compared a count of
        # all the receiver's wUSDC.b coins against a baseline, which this bot's
        # own trading defeats: any change output landing on the receiver puzzle
        # hash grew the count and could mark a still-unclaimed bridge COMPLETED,
        # sweeping its funding while the attested message sat unclaimed. The
        # message coin commits to the nonce and nothing about the claimer.
        if claim.message_claimed_on_chain(
            self._coinset,
            net,
            nonce=bytes.fromhex(job.bridge_nonce),
            source=self._claim_source(),
            destination=bytes.fromhex(job.state["message_destination"]),
            contents=self._claim_contents(job),
        ):
            resolved, status = self._sweep_security(job, coin, sk)
            if not resolved:
                # The deposit landed, but our funding coin is still unspent and
                # the node was unreachable. COMPLETED is a closed state, so
                # recording it here would bury a live, recoverable coin behind a
                # row that claims it was swept. Stay and retry the sweep.
                return _stay(
                    state={"third_party_claim": True, "sweep_status": status},
                    message=f"third-party claim paid the receiver; {status}; retrying",
                )
            return _advance(
                JobStatus.COMPLETED,
                state={"third_party_claim": True, "swept": True, "sweep_status": status},
                message="third-party claim paid the receiver; funding swept back",
            )

        # Unexplained conflict: the nonce is contested but nothing proves the
        # mint ran. Retrying is right for a portal race, but a nonce burned by a
        # relayer that never funded the mint will conflict forever -- and a
        # _stay resets retry_count, so this would loop silently with no operator
        # escape (Sweep and Cancel are both unavailable in CLAIMING). Give up
        # after a bounded number of rounds so the FAILED -> Sweep path applies.
        rounds = int(job.state.get("conflict_rounds", 0)) + 1
        if rounds >= _CLAIM_CONFLICT_MAX_ROUNDS:
            raise WarpTerminal(
                f"claim conflicted {rounds} times without evidence the mint ran "
                f"({push.status}). The nonce appears consumed; the deposit may "
                "still be claimable through the warp.green portal. Sweep this job "
                "to recover its funding coin"
            )
        return _stay(
            state={"final_cat_coin_id": final_id, "conflict_rounds": rounds},
            message=(
                f"claim conflict ({push.status}); re-syncing "
                f"(round {rounds}/{_CLAIM_CONFLICT_MAX_ROUNDS})"
            ),
        )

    # ================================================================== #
    # Handler helpers.
    # ================================================================== #

    def _resolve_receiver(self) -> tuple:
        """(xch address, receiver puzzle-hash hex) -- config first, else wallet.

        The wallet-derived address is resolved once and cached for the life of
        the engine, for two reasons. It is what :meth:`effective_receiver` can
        then publish to the Warp tab without doing I/O on every snapshot; and
        ``new_address=True`` burned a fresh derivation index on every call, so
        the address the operator saw was never the one a job actually used.
        """
        from . import clvm_utils as cu

        net, p = self._net, self._params
        address = (p.chia_receiver_address or "").strip()
        source = "config"
        if not address:
            cached = self._receiver_cache
            if cached is not None:
                return cached[0], cached[2]
            self._wallet.log_in()
            address = self._wallet.get_next_address(1, new_address=False)
            source = "wallet"
        ph = cu.decode_puzzle_hash(address, expected_prefix=net.chia_prefix)
        self._receiver_cache = (address, source, ph.hex())
        return address, ph.hex()

    def effective_receiver(self) -> Dict[str, Any]:
        """Where wUSDC.b will actually land, for display. Never raises.

        ``chia_receiver_address`` is blank by default and the snapshot used to
        publish that blank straight through, so the tab could never show the
        wallet address the runbook says it falls back to.
        """
        configured = (self._params.chia_receiver_address or "").strip()
        if configured:
            return {"receiver_address": configured, "receiver_source": "config"}
        cached = self._receiver_cache
        if cached is not None:
            return {"receiver_address": cached[0], "receiver_source": cached[1]}
        try:
            address, _ph = self._resolve_receiver()
        except Exception as exc:  # noqa: BLE001 -- display only, never abort
            return {"receiver_address": "", "receiver_source": f"unavailable: {exc}"}
        return {"receiver_address": address, "receiver_source": "wallet"}

    def _find_existing_funding(self, security_ph_hex: str, expected: int) -> Optional[str]:
        """Scan wallet history for an already-broadcast funding of this coin.

        Fails **closed**. This scan is the only thing standing between a lost
        ``send_transaction`` response and a second send of the same amount to the
        same ephemeral puzzle, so a scan that could not run is not evidence of
        anything -- least of all of "not funded yet". Raising ``WarpRetryable``
        routes the step through :meth:`_apply_retry`: backoff, visible
        ``last_error``, and crucially no send.
        """
        try:
            # Keep the window wide. This bot's wallet is busy, so a funding tx
            # can be pushed well down the history while the job waits on
            # confirmations -- a short window would miss it and re-send. The
            # timeout risk is handled by the client's timeout, not by looking
            # at less evidence.
            txs = self._wallet.get_transactions(1, start=0, end=200, reverse=True)
        except Exception as exc:  # noqa: BLE001 -- never read as "not funded"
            raise WarpRetryable(
                f"funding dedupe scan failed ({exc}); refusing to send in case a "
                "prior funding transaction already exists"
            ) from exc
        want = _hx(security_ph_hex)
        for tx in txs or []:
            for add in tx.get("additions") or []:
                if _hx(add.get("puzzle_hash", "")) == want and int(
                    add.get("amount", -1)
                ) == expected:
                    return tx.get("name") or tx.get("transaction_id")
        return None

    def _load_ephemeral_sk(self, job: WarpJob) -> bytes:
        """Decrypt the per-job ephemeral BLS private key (32 bytes)."""
        from . import keystore

        blob = job.state.get("ephemeral_blob")
        if not blob:
            raise WarpTerminal("ephemeral key blob missing from job state")
        key = keystore.load_bls_key(
            blob, extra_entropy=self._job_entropy(job), protector=self._protector
        )
        return key.private_key

    def _job_entropy(self, job: WarpJob) -> bytes:
        return f"warp-job-{job.id}".encode()

    def _binding(self) -> Dict[str, Any]:
        """The (network, hot wallet) identity a new job is frozen against."""
        return {"network": self._net.name, "hot_address": _hx(self._hot_address)}

    def _binding_mismatch(self, job: WarpJob) -> Optional[str]:
        """Why this job must not be processed under the current config, or ``None``.

        A job's amounts, allowance, frozen receiver and signed-but-unbroadcast
        transactions all belong to one hot wallet on one network. Resuming it
        under another is not a recoverable state -- it is a different bridge --
        so this refuses rather than guessing.

        A job still in ``AWAITING_DEPOSIT`` has committed to nothing yet, so a
        missing binding there is self-healed by :meth:`_h_awaiting_deposit`
        rather than treated as a mismatch. Past that point an *absent* binding
        is itself a mismatch: it means the row predates this guard and we cannot
        prove which wallet it was frozen against.
        """
        if job.network and job.network != self._net.name:
            return (
                f"job {job.id} was created on network {job.network!r} but warp is "
                f"configured for {self._net.name!r}; resolve it under the original "
                "configuration"
            )
        bound = _hx(job.state.get("hot_address") or "")
        mine = _hx(self._hot_address)
        if bound:
            if bound != mine:
                return (
                    f"job {job.id} was frozen against Base hot wallet 0x{bound} but "
                    f"the configured key is 0x{mine}; restore the original key and "
                    "resolve (or sweep) that job before rotating"
                )
            return None
        if job.status == JobStatus.AWAITING_DEPOSIT:
            return None
        return (
            f"job {job.id} is in {job.status} with no recorded hot wallet, so it "
            "cannot be proven to belong to the configured key; resolve it under "
            "the configuration that created it"
        )

    def _claim_source(self) -> bytes:
        """The message source = the Base ERC20 bridge contract address (20 bytes)."""
        return bytes.fromhex(self._net.erc20_bridge_address[2:])

    def _claim_contents(self, job: WarpJob) -> List[bytes]:
        return [bytes.fromhex(_hx(c)) for c in job.state.get("message_contents", [])]

    def _resolve_portal_hint(self, job: WarpJob) -> bytes:
        """A recent portal coin id to walk forward from (never the launcher)."""
        pcid = job.state.get("portal_coin_id")
        if pcid:
            return bytes.fromhex(pcid)
        if self._params.portal_hint:
            return bytes.fromhex(_hx(self._params.portal_hint))
        meta = self._store.get_meta("portal_hint")
        if meta:
            return bytes.fromhex(_hx(meta))
        return self._bootstrap_hint_from_nostr(job)

    def _bootstrap_hint_from_nostr(self, job: WarpJob) -> bytes:
        """Seed the portal hint from any validator ``c`` tag (always near the tip)."""
        from . import clvm_utils as cu, nostr

        net = self._net
        routing_tag = nostr.routing_tag_for(net, bytes.fromhex(job.bridge_nonce))
        filt = nostr.build_filter(net, routing_tag)
        fetcher = self._nostr_fetcher or nostr._default_fetcher()
        for relay in net.nostr_relays[:4]:
            try:
                events = fetcher(relay, filt, 6.0)
            except Exception:  # noqa: BLE001 -- dead relay, try the next
                continue
            for event in events or []:
                tag = nostr._first_tag(event.get("tags") or [], nostr.HRP_COIN)
                if not tag:
                    continue
                hrp, payload = cu.bech32m_decode_bytes(tag, max_length=200)
                if hrp == nostr.HRP_COIN and payload:
                    return payload
        raise WarpPending("no portal coin hint available from Nostr yet")

    def _sweep_security(self, job: WarpJob, coin: Any, sk: bytes) -> tuple:
        """Recover the ephemeral funding coin back to the bot wallet.

        Returns ``(resolved, status)``. ``resolved`` is ``True`` when the chain
        gave a verdict -- accepted, already pending, or a conflict meaning the
        coin was already spent -- i.e. there is nothing further to recover. It is
        ``False`` only when the push could not reach the node at all, which is
        what distinguishes "swept" from "we have no idea" and therefore decides
        whether a FAILED job may be closed.
        """
        from . import claim

        try:
            _kind, status = claim.build_and_push_sweep(
                self._coinset,
                self._net,
                security_coin=coin,
                destination_puzzle_hash=bytes.fromhex(job.receiver_ph),
                ephemeral_sk=sk,
                sweep_fee=0,
            )
            return True, status
        except Exception as exc:  # noqa: BLE001 -- operator can re-sweep later
            return False, f"sweep failed: {exc}"

    def _funding_provably_gone(self, job: WarpJob) -> tuple:
        """``(resolved, status)`` for a job with no coin at the expected amount.

        Resolved only when nothing was ever funded, or the recorded security
        coin is provably spent. "Not found" on its own is not proof: an
        unindexed coin, or one funded under a different ``claim_fee_mojos``,
        looks identical and its XCH is still sitting at the ephemeral puzzle.
        """
        from . import claim

        if not job.state.get("funding_tx_id") and not job.state.get("security_coin_id"):
            return True, "no funding was ever recorded for this job"
        sec_id = job.state.get("security_coin_id")
        if sec_id:
            try:
                if claim.security_coin_spent(self._coinset, bytes.fromhex(sec_id)):
                    return True, "security coin already spent; nothing left to sweep"
            except Exception as exc:  # noqa: BLE001 -- unknown is not resolved
                return False, f"could not confirm the security coin's fate: {exc}"
        return False, (
            "no unspent security coin at the expected amount -- it may not be "
            "indexed yet, or claim_fee_mojos changed since funding. Leaving the "
            "job open so Sweep stays available."
        )

    # ================================================================== #
    # Public API (driven by the worker / GUI).
    # ================================================================== #

    def maybe_start_auto_job(self) -> Optional[dict]:
        """Open an auto-bridge job when a fresh deposit crosses the min threshold."""
        p, net = self._params, self._net
        if not (p.enabled and p.auto_bridge):
            return None
        # A dry run cannot spend the balance, and DRY_RUN_OK is a closed state
        # that frees the slot -- so auto-bridging a rehearsal has no fixed point
        # and would open jobs forever. Rehearsal is a manual "Bridge now".
        if p.dry_run:
            return None
        if self._store.get_active_job() is not None:
            return None
        if p.min_micros <= 0:
            return None
        try:
            balance = self._evm.get_erc20_balance(net.usdc_address, self._hot_address)
        except Exception:  # noqa: BLE001 -- transient RPC; try again next tick
            return None
        if balance < p.min_micros:
            return None
        job = self._store.create_job(
            net.name,
            status=JobStatus.AWAITING_DEPOSIT,
            state={"auto": True, **self._binding()},
            event_message="auto-bridge: deposit detected",
        )
        return _job_dict(job)

    def request_bridge(self, target_micros: Optional[int] = None) -> dict:
        """Operator "Bridge now": open a job (raises if one is already active).

        A manual job bridges whatever is in the hot wallet, ignoring
        ``min_auto_bridge_usdc`` -- that floor exists to stop *automatic* dust
        bridging and has no meaning for an explicit click. ``max_auto_bridge_usdc``
        still applies; see :meth:`_h_awaiting_deposit`.
        """
        active = self._store.get_active_job()
        if active is not None:
            raise WarpError(
                f"a warp job is already active (job {active.id}, {active.status}); "
                "resolve it first -- Retry, Cancel, or Sweep from the jobs table"
            )
        state: Dict[str, Any] = {"manual": True, **self._binding()}
        if target_micros:
            state["target_micros"] = int(target_micros)
        job = self._store.create_job(
            self._net.name,
            status=JobStatus.AWAITING_DEPOSIT,
            state=state,
            event_message="manual bridge requested",
        )
        return _job_dict(job)

    def job_action(self, job_id: int, action: str) -> Optional[dict]:
        """Operator affordance: ``retry`` | ``cancel`` | ``sweep``."""
        action = str(action).lower()
        job = self._store.get_job(job_id)
        # Retry and Sweep both sign or resume against the configured hot wallet;
        # neither is meaningful for a job bound to another one. Cancel is allowed
        # through: it is a pure DB write and the only escape for a foreign job
        # that never made it on chain.
        if action in ("retry", "sweep"):
            mismatch = self._binding_mismatch(job)
            if mismatch:
                raise WarpError(mismatch)
        if action == "retry":
            target = (
                job.state.get("failed_from")
                if job.status == JobStatus.FAILED
                else None
            )
            if job.status == JobStatus.FAILED and target:
                self._store.update_job(
                    job_id,
                    status=target,
                    expected_status=JobStatus.FAILED,
                    columns={"retry_count": 0, "last_error": None, "next_retry_at": None},
                    event=("retry", f"operator retry -> {target}", None),
                )
            else:
                self._store.update_job(
                    job_id,
                    expected_status=job.status,
                    columns={"retry_count": 0, "last_error": None, "next_retry_at": None},
                    event=("retry", "operator retry", None),
                )
            return _job_dict(self._store.get_job(job_id))
        if action == "cancel":
            if job.status not in _CANCELLABLE:
                raise WarpError(f"cannot cancel a job in {job.status}")
            self._store.update_job(
                job_id,
                status=JobStatus.CANCELLED,
                expected_status=job.status,
                columns={"next_retry_at": None},
                event=("cancel", "operator cancelled", None),
            )
            return _job_dict(self._store.get_job(job_id))
        if action == "sweep":
            return self._sweep_job(job)
        raise WarpError(f"unknown job action: {action!r}")

    def _sweep_job(self, job: WarpJob) -> dict:
        """Force-sweep a job's ephemeral funding coin back to the bot wallet.

        Sweep is the operator's only escape from a FAILED job: FAILED is
        deliberately not a closed state (it holds the single active-job slot so a
        failure cannot be buried under a new job) and Cancel is refused once the
        bridge is on chain. So a sweep that *resolves* must also close the job,
        or the bridge is permanently unable to start another one.

        Closing is a one-way door -- a CANCELLED row offers neither Retry nor
        Sweep -- so it is only taken on proof that nothing is left to recover.
        Two cases look like "nothing to sweep" but are not:

        * **No ephemeral key, but the bridge is on chain.** Three attested-terms
          anchors fail *after* ``bridgeToChia`` confirmed and before the key is
          minted, so such a job holds a live, unclaimed message. Closing it
          would discard the only in-app record of a real deposit.
        * **No coin at the expected amount.** That can mean already spent, not
          yet indexed, or ``claim_fee_mojos`` edited since funding. Only the
          first is resolved, so the funded amount is read back from the job
          rather than recomputed from live config.
        """
        from . import claim

        blob = job.state.get("ephemeral_blob")
        if not blob:
            if job.bridge_nonce:
                raise WarpError(
                    f"job {job.id} failed after its Base bridge confirmed "
                    f"(nonce {job.bridge_nonce}) but before a claim key existed, so "
                    "there is no funding coin to sweep and the attested message is "
                    "still unclaimed. It stays claimable forever -- recover it "
                    "through the warp.green portal, which pays the attested Chia "
                    "receiver. This job is kept open deliberately as the record."
                )
            resolved, status = True, "no ephemeral key: nothing was ever funded"
        else:
            sk = self._load_ephemeral_sk(job)
            # What was actually sent, not what current config would send.
            expected = int(
                job.state.get("funding_amount")
                or int(job.post_tip_mojos or 0) + int(self._params.claim_fee_mojos)
            )
            coin = claim.find_security_coin(self._coinset, sk, expected)
            if coin is None:
                resolved, status = self._funding_provably_gone(job)
            else:
                resolved, status = self._sweep_security(job, coin, sk)

        # Only a resolved sweep frees the slot. A transport failure leaves the
        # job FAILED so the operator can try again with the funds still known
        # recoverable.
        close = resolved and job.status == JobStatus.FAILED
        self._store.update_job(
            job.id,
            status=JobStatus.CANCELLED if close else None,
            expected_status=job.status,
            columns={"next_retry_at": None},
            state_patch={"swept": resolved, "sweep_status": status},
            event=(
                "sweep",
                f"{status}; job closed, active slot freed" if close else status,
                None,
            ),
        )
        return _job_dict(self._store.get_job(job.id))

    def refresh_hot_wallet(self) -> Dict[str, Any]:
        """Refresh the cached Base hot-wallet balances (never raises).

        Also resolves the effective receiver. This runs on the worker thread and
        is contracted never to raise, which makes it the right place for the one
        wallet round-trip the destination display needs -- :meth:`snapshot` must
        stay I/O-free.
        """
        snap: Dict[str, Any] = {"address": self._hot_address, "error": None}
        try:
            snap["eth_wei"] = self._evm.get_eth_balance(self._hot_address)
            snap["usdc_micros"] = self._evm.get_erc20_balance(
                self._net.usdc_address, self._hot_address
            )
        except Exception as exc:  # noqa: BLE001 -- surface, don't abort the tick
            snap["error"] = str(exc)
        self._hot_cache = snap
        self._receiver_snap = self.effective_receiver()
        return snap

    def snapshot(self) -> Dict[str, Any]:
        """A GUI-serializable snapshot of the whole warp subsystem.

        Pure reads only -- no RPC, no wallet calls. The GUI polls this.
        """
        active = self._store.get_active_job()
        receiver = getattr(self, "_receiver_snap", None) or {
            "receiver_address": (self._params.chia_receiver_address or "").strip(),
            "receiver_source": "config" if self._params.chia_receiver_address else "",
        }
        snap: Dict[str, Any] = {
            "enabled": self._params.enabled,
            "dry_run": self._params.dry_run,
            "auto_bridge": self._params.auto_bridge,
            "network": self._net.name,
            "hot_wallet": dict(self._hot_cache),
            "active_job": _job_dict(active),
            "jobs": [_job_dict(j) for j in self._store.list_jobs(limit=25)],
            "min_micros": self._params.min_micros,
            "max_micros": self._params.max_micros,
            "expected_asset_id": self._net.expected_asset_id,
            **receiver,
        }
        if self._binding_error:
            snap["binding_error"] = self._binding_error
            # Mirror into the generic key the banner already renders.
            snap["error"] = self._binding_error
        return snap


# --------------------------------------------------------------------------- #
# Lazy engine construction helpers (run on the worker thread).
# --------------------------------------------------------------------------- #

def _job_db_path(config: Optional[dict]) -> str:
    """Resolve the warp jobs DB path (own file; the engine DB stays read-only)."""
    warp = (config or {}).get("warp") or {}
    explicit = warp.get("jobs_db")
    if explicit:
        return str(explicit)
    return str(Path("data") / jobs.DB_FILENAME)


def _coinset_url(config: Optional[dict], net: constants.WarpNet) -> str:
    warp = (config or {}).get("warp") or {}
    return (str(warp.get("coinset_url", "") or "").strip()) or net.coinset_url


# ===================================================================== #
# Qt worker -- runs the blocking engine step on a background QThread.
# ===================================================================== #

class _WarpWorker(QObject):
    """Background worker that owns the :class:`WarpEngine` and ticks it.

    Moved onto a ``QThread`` and driven exclusively through queued signals, so
    the multi-second bridge step (chain reads, RPC, relay sweeps) never runs on
    the GUI thread. The engine and all heavy warp submodules are imported and
    constructed lazily here, on the worker thread, keeping GUI boot cheap.
    """

    snapshot_ready = Signal(dict)

    def __init__(self, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._config: dict = {}
        self._engine: Optional[WarpEngine] = None
        self._engine_error: Optional[str] = None

    @Slot(dict)
    def set_config(self, config: dict) -> None:
        """Adopt a new config snapshot and force a lazy engine rebuild."""
        self._config = dict(config or {})
        self._engine = None
        self._engine_error = None

    @Slot()
    def tick(self) -> None:
        """Advance the active job one step and refresh the hot-wallet balances."""

        def _run(engine: WarpEngine) -> None:
            engine.step()
            engine.refresh_hot_wallet()

        self.snapshot_ready.emit(self._guarded(_run))

    @Slot(object)
    def request_bridge(self, target_micros: object) -> None:
        self.snapshot_ready.emit(
            self._guarded(lambda engine: engine.request_bridge(target_micros))
        )

    @Slot(int, str)
    def job_action(self, job_id: int, action: str) -> None:
        self.snapshot_ready.emit(
            self._guarded(lambda engine: engine.job_action(job_id, action))
        )

    # -- internals ---------------------------------------------------------- #

    def _guarded(self, fn: Callable[[WarpEngine], Any]) -> dict:
        """Run ``fn`` against the engine, swallowing errors, return a snapshot.

        The error is stashed into the returned snapshot as ``action_error`` --
        without it a rejected Bridge now / Retry / Sweep is completely invisible
        to the operator, who sees an enabled button that silently does nothing.
        """
        action_error: Optional[str] = None
        try:
            engine = self._ensure_engine()
            if engine is not None:
                fn(engine)
        except Exception as exc:  # noqa: BLE001 -- never kill the worker
            action_error = str(exc)
            _log.warning("warp worker step failed: %s", exc)
        snap = self._snapshot_or_error()
        if action_error:
            snap["action_error"] = action_error
        return snap

    def _snapshot_or_error(self) -> dict:
        if self._engine is not None:
            try:
                return self._engine.snapshot()
            except Exception as exc:  # noqa: BLE001
                return {"enabled": self._config_enabled(), "error": str(exc)}
        return {
            # Report what the operator *asked* for, not what we managed to
            # build. Otherwise a failed anchor renders as "Bridge disabled --
            # set warp.enabled: true", which is both wrong and misleading
            # advice; the documented banner for this is "blocked".
            "enabled": self._config_enabled(),
            "error": self._engine_error or "warp engine not built",
        }

    def _config_enabled(self) -> bool:
        """Whether ``warp.enabled`` is set, independent of whether it started."""
        warp = (self._config or {}).get("warp") or {}
        return bool(warp.get("enabled", False))

    def _ensure_engine(self) -> Optional[WarpEngine]:
        if self._engine is not None:
            return self._engine
        if self._engine_error is not None:
            return None
        try:
            self._engine = self._build_engine()
        except Exception as exc:  # noqa: BLE001 -- disabled/misconfigured is normal
            self._engine_error = str(exc)
            _log.info("warp engine not started: %s", exc)
            self._engine = None
        return self._engine

    def _build_engine(self) -> WarpEngine:
        """Construct the engine + all clients lazily (heavy imports live here)."""
        params = warp_params_from_config(self._config)
        if not params.enabled:
            raise WarpError("warp disabled")

        from . import (
            coinset as coinset_mod,
            drivers as drivers_mod,
            evm as evm_mod,
            keystore,
            nostr,
            wallet as wallet_mod,
            watcher as watcher_mod,
        )

        net = constants.MAINNET

        # Refuse-to-start anchor, before a single client exists. Re-derives the
        # wrapped-asset TAIL offline and checks it against both the deployment
        # constant and any operator-pinned warp.expected_asset_id. This is the
        # "before any funds move" guarantee the runbook has always claimed; it
        # previously ran only inside build_claim_bundle, i.e. after the Base
        # approve, the bridge, and the Chia funding send had all happened.
        try:
            drivers_mod.verify_wrapped_asset_anchor(net, params.expected_asset_id)
        except Exception as exc:  # noqa: BLE001 -- surfaced as a blocked banner
            raise WarpError(f"wrapped-asset anchor failed: {exc}") from exc

        store = WarpJobStore(_job_db_path(self._config))
        rpc_url = params.base_rpc_url or net.evm_default_rpc_url
        evm_client = evm_mod.EvmClient(net, rpc_url=rpc_url)
        coinset_client = coinset_mod.CoinsetClient(_coinset_url(self._config, net))
        watcher_client = watcher_mod.WatcherClient(net.watcher_api_url)

        chia = self._config.get("chia") or {}
        wallet_client = wallet_mod.WalletClient(
            {
                "host": chia.get("wallet_host") or "localhost",
                "port": int(chia.get("wallet_port") or 9256),
                "fingerprint": chia.get("wallet_fingerprint"),
                "cert_path": str(chia.get("wallet_cert_path") or ""),
                "key_path": str(chia.get("wallet_key_path") or ""),
            },
            # The default 5s is too tight for the 200-row history query that
            # guards against double-funding; a timeout there now (correctly)
            # blocks the send, so make it unlikely rather than merely safe.
            timeout=30.0,
        )

        protector = keystore.default_protector()
        warp_cfg = self._config.get("warp") or {}
        blob = warp_cfg.get("evm_private_key_dpapi")
        if not blob:
            raise WarpError("no EVM hot-wallet key configured (warp.evm_private_key_dpapi)")
        evm_key = keystore.load_evm_key(blob, protector=protector)

        return WarpEngine(
            net,
            store,
            params=params,
            evm=evm_client,
            coinset=coinset_client,
            watcher=watcher_client,
            wallet=wallet_client,
            collector=nostr.NostrSigCollector(net),
            evm_key=evm_key,
            protector=protector,
            nostr_fetcher=None,
        )


# ===================================================================== #
# Main service -- lives on the GUI thread.
# ===================================================================== #

class WarpService(QObject):
    """GUI-thread façade over the background warp worker.

    Mirrors :class:`gui.services.wallet_service.WalletService`: a dedicated
    ``QThread`` worker does all blocking work; :meth:`tick` only *triggers* a
    step and returns immediately, overlapping ticks are dropped (never queued),
    and :meth:`get_snapshot` returns the last mutex-guarded snapshot. Owned by
    :class:`gui.services.engine_bridge.EngineBridge`.
    """

    # -- Qt signals (GUI thread) ------------------------------------------- #
    jobs_updated = Signal(list)
    hot_wallet_updated = Signal(dict)
    service_state = Signal(dict)

    # -- Internal triggers (queued connections to the worker thread) ------- #
    _trigger_tick = Signal()
    _trigger_params = Signal(dict)
    _trigger_bridge = Signal(object)
    _trigger_action = Signal(int, str)

    def __init__(self, config: Optional[dict] = None, parent: Optional[QObject] = None) -> None:
        super().__init__(parent)
        self._mutex = QMutex()
        self._snapshot: dict = {}
        self._tick_in_flight: bool = False

        self._thread = QThread(self)
        self._thread.setObjectName("WarpWorkerThread")
        self._worker = _WarpWorker()
        self._worker.moveToThread(self._thread)

        self._worker.snapshot_ready.connect(self._on_snapshot_ready)
        self._trigger_tick.connect(self._worker.tick)
        self._trigger_params.connect(self._worker.set_config)
        self._trigger_bridge.connect(self._worker.request_bridge)
        self._trigger_action.connect(self._worker.job_action)

        self._trigger_params.emit(dict(config or {}))

    # -- lifecycle ---------------------------------------------------------- #

    def start(self) -> None:
        if self._thread.isRunning():
            return
        _log.info("Starting WarpService worker thread.")
        self._thread.start()

    def stop(self) -> None:
        _log.info("Stopping WarpService.")
        if self._thread.isRunning():
            self._thread.quit()
            if not self._thread.wait(5_000):
                _log.warning("WarpService worker thread did not exit in time.")

    # -- public API --------------------------------------------------------- #

    def update_config(self, config: dict) -> None:
        self._trigger_params.emit(dict(config or {}))

    def tick(self) -> None:
        """Trigger one bridge step on the worker thread (dropped if in flight)."""
        if self._tick_in_flight:
            return
        if not self._thread.isRunning():
            self.start()
        self._tick_in_flight = True
        self._trigger_tick.emit()

    def request_bridge(self, target_micros: Optional[int] = None) -> None:
        if not self._thread.isRunning():
            self.start()
        self._trigger_bridge.emit(target_micros)

    def job_action(self, job_id: int, action: str) -> None:
        if not self._thread.isRunning():
            self.start()
        self._trigger_action.emit(int(job_id), str(action))

    def get_snapshot(self) -> dict:
        """Return the last snapshot without triggering work (mutex-guarded)."""
        with QMutexLocker(self._mutex):
            return dict(self._snapshot)

    # -- internal slot (GUI thread) ---------------------------------------- #

    @Slot(dict)
    def _on_snapshot_ready(self, snap: dict) -> None:
        self._tick_in_flight = False
        snap = dict(snap or {})
        with QMutexLocker(self._mutex):
            self._snapshot = snap
        self.service_state.emit(dict(snap))
        self.hot_wallet_updated.emit(dict(snap.get("hot_wallet") or {}))
        self.jobs_updated.emit(list(snap.get("jobs") or []))
