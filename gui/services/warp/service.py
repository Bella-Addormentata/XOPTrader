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
from . import rebalance as rb
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
# Receipts that carry no usable status. Base is post-Byzantium so this should
# never happen; if it does, it means we cannot tell success from revert, and
# guessing either way is worse than handing the job to the operator.
_AMBIGUOUS_RECEIPT_POLLS: int = 5
# Polls of an unmined bridge before re-signing it at a higher fee. Base blocks
# every ~2s, so a correctly-priced transaction mines almost immediately; this is
# deliberately generous before spending more on gas.
_STUCK_TX_POLLS: int = 20
# Fee escalations before giving up. bump_fees caps its own multiplier at 3, so
# beyond that a further attempt would re-sign at an identical fee and be
# rejected as a non-replacement.
_MAX_FEE_BUMPS: int = 3

# How long a job may sit pending in a given state before it is handed to the
# operator. _apply_stay resets retry_count and clears last_error on every pend
# -- correctly, since nothing moved -- but that also made these loops invisible
# and unbounded. None of these states offers Cancel, Retry is a no-op on a
# non-FAILED job, and Sweep closes only FAILED, so a job stuck here held the
# single active slot forever. FAILED is the one status with affordances, so
# expiry routes there rather than nowhere.
#
# Counted in polls rather than wall time, matching _CLAIM_CONFLICT_MAX_ROUNDS
# and _STUCK_TX_POLLS above. Times below assume the default 15s poll interval.
#
# Deliberately generous: every one of these waits on somebody else's
# infrastructure (the warp.green watcher, the validator quorum, a Chia block),
# and failing a live bridge early is worse than waiting too long.
_PENDING_MAX_POLLS: Dict[str, int] = {
    JobStatus.MESSAGE_SENT: 1440,       # ~6h -- watcher indexing the message
    JobStatus.COLLECTING_SIGS: 1440,    # ~6h -- validator quorum
    JobStatus.FUNDING_CLAIM: 480,       # ~2h -- a Chia send confirming
    JobStatus.CLAIM_FUNDED: 480,        # ~2h -- the funding coin reaching depth
    JobStatus.CLAIMING: 1440,           # ~6h -- the final CAT coin appearing
    JobStatus.BRIDGE_CONFIRMED: 480,    # ~2h -- Base confirmation depth
    # BRIDGING bounds its own pend paths (_STUCK_TX_POLLS, revert attempts,
    # ambiguous-receipt polls), but a persistent transport error loops through
    # _apply_retry and would otherwise never expire. Failing out of BRIDGING is
    # safe: _retry_tx_reset keeps the raw while the nonce is live, and Abandon
    # refuses while a signed bridge could still mine.
    JobStatus.BRIDGING: 480,            # ~2h -- Base RPC unreachable
    # Outbound (unwrap).
    JobStatus.UNWRAP_CHECKS: 1440,      # ~6h -- liquidity/spendable waits
    JobStatus.BURN_SENT: 480,           # ~2h -- the cat_spend confirming
    JobStatus.BURNING: 1440,            # ~6h -- funding + bundle + depth
    JobStatus.COLLECTING_EVM_SIGS: 1440,  # ~6h -- validator quorum
    JobStatus.RELAYING: 480,            # ~2h -- Base receipt/confirmations
    # Generous on purpose: an altruistic relay is best-effort by nature and
    # the message stays deliverable forever. The deadline exists to hand the
    # wait back to the operator eventually (fund gas + Retry, or the portal),
    # never because anything expires.
    JobStatus.AWAITING_EXTERNAL_RELAY: 5760,  # ~24h -- volunteer relay wait
}
_SIG_COLLECT_DEADLINE_S: float = 12.0

# ETH floor (wei) the hot wallet must hold before we sign approve/bridge: covers
# the Portal message toll (1e13 wei) plus a comfortable Base gas margin.
_MIN_GAS_WEI: int = 300_000_000_000_000  # 0.0003 ETH

# Altruistic-relay cadence. The sweep hits the watcher plus (per candidate) the
# Nostr relays, so it runs far below the ~30s tick; the activity refresh feeds
# a GUI hint and can be lazier still.
_RELAY_SWEEP_INTERVAL_S: float = 600.0
_ACTIVITY_REFRESH_INTERVAL_S: float = 900.0
_ACTIVITY_LOOKBACK: int = 100

# Jobs may only be cancelled before the irreversible on-chain bridge is signed.
_CANCELLABLE = frozenset(
    {JobStatus.AWAITING_DEPOSIT, JobStatus.DEPOSIT_SEEN, JobStatus.APPROVING,
     # The only cancellable outbound state: pure reads, nothing moved.
     # From BURN_SENT on, the cat_spend makes the job forward-only.
     JobStatus.UNWRAP_CHECKS}
)

# Stand-in for a funding transaction the wallet accepted without returning an
# id. What matters downstream is not the id itself -- the security coin is
# located by puzzle hash and amount -- but that ``funding_tx_id`` stays truthy,
# because that is the flag saying "already sent, do not send again".
_FUNDING_TX_UNKNOWN: str = "sent-id-unknown"

# The only two states whose behaviour depends on whether the job is a rehearsal.
# Everywhere else the flag is never consulted, so a row that predates the freeze
# is harmless there and must not be refused.
_DRY_RUN_SENSITIVE = frozenset({JobStatus.APPROVING, JobStatus.BRIDGING})

# Sweep spends the job's own ephemeral security coin, which is an input to the
# claim the state machine may still be assembling. Only safe once the machine
# has stopped: FAILED (the operator's escape) or COMPLETED (nothing left).
_SWEEPABLE = frozenset({JobStatus.FAILED, JobStatus.COMPLETED})


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
    # Outbound (unwrap). -1 == never configured: request_unwrap refuses
    # until the operator states a cap, but an absent key cannot break an
    # existing enabled inbound config on upgrade.  0 == explicit unlimited.
    max_unwrap_micros: int = -1
    unwrap_chia_fee_mojos: int = 0
    # Altruistic relay (opt-in): deliver strangers' stuck xch -> bse messages
    # by paying their ~145k relay gas. Inert by default.
    altruistic_relay: bool = False
    relay_grace_min: float = 30.0
    relay_daily_gas_budget_eth: float = 0.0002
    # Operator-facing Base gas reserve: the ETH balance the hot wallet is
    # meant to keep on hand for relay/bridge gas. Distinct from the hard
    # _MIN_GAS_WEI floor, which is the protocol minimum required to sign at
    # all -- this is the "top me up" line the GUI warns against, so an
    # operator is told to refuel BEFORE jobs start refusing. 0 disables the
    # warning.
    min_base_eth: float = 0.005


def _parse_unwrap_cap(warp: dict) -> int:
    """warp.max_unwrap_usdc -> micros; -1 when the key was never set.

    Same fail-closed shape as max_auto_bridge_usdc ([WARP-CAP-FAIL-OPEN]),
    with one difference: absence refuses the unwrap ACTION, not the config --
    the key did not exist when existing inbound configs were written, so a
    parse-time refusal would break every enabled deployment on upgrade.
    """
    raw = warp.get("max_unwrap_usdc", None)
    if raw is None or str(raw).strip() == "":
        return -1
    if str(raw).strip().lower() == "unlimited":
        return 0
    value = _parse_usdc_config_value("max_unwrap_usdc", raw)
    if value <= 0 or int(round(value * 1_000_000)) <= 0:
        raise WarpError(
            f"warp.max_unwrap_usdc must be positive (got {raw!r}); use "
            "'unlimited' if no cap is genuinely intended"
        )
    return int(round(value * 1_000_000))


def _parse_usdc_config_value(key: str, raw: object) -> float:
    """A finite float from a user-facing config knob, or a clear WarpError.

    A bare ``float(raw)`` let ``'abc'`` raise a raw ValueError that surfaced
    as an unhelpful engine-start failure, ``1e309`` overflow on the later
    ``int()``, and ``nan`` slip PAST the positivity check entirely -- NaN
    comparisons are always False -- before crashing downstream.
    """
    import math

    try:
        value = float(raw)  # type: ignore[arg-type]
    except (TypeError, ValueError) as exc:
        raise WarpError(f"warp.{key} is not a number (got {raw!r})") from exc
    if not math.isfinite(value):
        raise WarpError(f"warp.{key} must be finite (got {raw!r})")
    return value


def _parse_positive_float(key: str, raw: object) -> float:
    """A positive finite float from a config knob (same rigour as the USDC keys)."""
    value = _parse_usdc_config_value(key, raw)
    if value <= 0:
        raise WarpError(f"warp.{key} must be positive (got {raw!r})")
    return value


def _parse_non_negative_float(key: str, raw: object, default: float = 0.0) -> float:
    """A finite float >= 0 from a config knob; absent/blank -> *default*.

    Zero is meaningful here (it switches a warning off), so this cannot
    reuse _parse_positive_float -- but junk and negatives still fail loudly
    rather than silently disabling a balance guard.
    """
    if raw is None or str(raw).strip() == "":
        return default
    value = _parse_usdc_config_value(key, raw)
    if value < 0:
        raise WarpError(f"warp.{key} must be >= 0 (got {raw!r})")
    return value


def _parse_config_bool(key: str, raw: object, default: bool) -> bool:
    """A strict boolean from a config knob -- ``bool()`` fails open.

    A YAML-quoted ``"false"`` is a non-empty string, so ``bool(raw)`` reads
    it as True -- which for ``altruistic_relay`` silently enables gas
    spending, and for ``enabled`` starts a bridge the operator wrote
    ``"false"`` for. Accept real booleans, 0/1, and the strings
    "true"/"false" (case-insensitive); refuse everything else loudly.
    """
    if raw is None:
        return default
    if isinstance(raw, bool):
        return raw
    if isinstance(raw, int) and raw in (0, 1):
        return bool(raw)
    if isinstance(raw, str):
        s = raw.strip().lower()
        if s == "":
            return default
        if s == "true":
            return True
        if s == "false":
            return False
    raise WarpError(
        f"warp.{key} must be a boolean (got {raw!r}); use true or false"
    )


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
    min_usdc = _parse_usdc_config_value(
        "min_auto_bridge_usdc", warp.get("min_auto_bridge_usdc", 0) or 0
    )

    # [WARP-CAP-FAIL-OPEN 2026-08-08] The cap must be stated, not inferred.
    # This resolved absent / commented-out / 0 / null all to 0, and the clamp
    # is guarded on `if p.max_micros > 0` -- so a missing cap meant NO cap.
    # Bridge now takes no amount argument and has no confirmation dialog, and
    # the manual path deliberately skips the floor with want = have, so one
    # click bridged the entire hot-wallet balance. The key is not exposed in
    # any settings widget either, so it can only be hand-edited.
    # Only enforced when the bridge is actually on: a disabled warp section
    # must not stop the GUI from loading a config.
    enabled = _parse_config_bool("enabled", warp.get("enabled"), False)
    raw_cap = warp.get("max_auto_bridge_usdc", None)
    blank_cap = raw_cap is None or str(raw_cap).strip() == ""
    if enabled and blank_cap:
        raise WarpError(
            "warp.max_auto_bridge_usdc is required when warp.enabled is true: "
            "it is the blast radius for both auto-bridging and the Bridge now "
            "button, and an absent value meant no limit at all. Set a number, "
            "or 'unlimited' to state explicitly that any balance may be bridged"
        )
    if blank_cap or str(raw_cap).strip().lower() == "unlimited":
        max_usdc = 0.0                      # 0 == uncapped
    else:
        max_usdc = _parse_usdc_config_value("max_auto_bridge_usdc", raw_cap or 0)
        if max_usdc <= 0:
            if enabled:
                raise WarpError(
                    f"warp.max_auto_bridge_usdc must be positive (got {raw_cap!r}); "
                    "use 'unlimited' if no cap is genuinely intended"
                )
            max_usdc = 0.0
        elif enabled and int(round(max_usdc * scale)) <= 0:
            # A stated cap below half a micro-USDC rounds to 0 micros, and the
            # clamp is guarded on max_micros > 0 -- so a tiny positive cap
            # would silently mean NO cap, the exact fail-open this key was
            # made mandatory to prevent.
            raise WarpError(
                f"warp.max_auto_bridge_usdc of {raw_cap!r} rounds to zero "
                "micro-USDC and would disable the cap; set at least 0.000001"
            )
    return WarpParams(
        enabled=enabled,
        dry_run=_parse_config_bool("dry_run", warp.get("dry_run"), True),
        auto_bridge=_parse_config_bool(
            "auto_bridge", warp.get("auto_bridge"), False
        ),
        base_rpc_url=str(warp.get("base_rpc_url", "") or ""),
        min_micros=int(round(min_usdc * scale)),
        max_micros=int(round(max_usdc * scale)),
        claim_fee_mojos=int(warp.get("claim_fee_mojos", 100_000_000) or 0),
        chia_funding_fee_mojos=int(warp.get("chia_funding_fee_mojos", 0) or 0),
        poll_interval_s=float(warp.get("poll_interval_s", 15) or 15),
        chia_receiver_address=str(warp.get("chia_receiver_address", "") or ""),
        expected_asset_id=str(warp.get("expected_asset_id", "") or ""),
        portal_hint=(str(warp.get("portal_hint")) if warp.get("portal_hint") else None),
        max_unwrap_micros=_parse_unwrap_cap(warp),
        unwrap_chia_fee_mojos=int(warp.get("unwrap_chia_fee_mojos", 0) or 0),
        altruistic_relay=_parse_config_bool(
            "altruistic_relay", warp.get("altruistic_relay"), False
        ),
        relay_grace_min=_parse_positive_float(
            "relay_grace_min", warp.get("relay_grace_min", 30) or 30
        ),
        relay_daily_gas_budget_eth=_parse_positive_float(
            "relay_daily_gas_budget_eth",
            warp.get("relay_daily_gas_budget_eth", 0.0002) or 0.0002,
        ),
        # The default is passed to the parser, not to .get(): a key that is
        # present but blank or null ("min_base_eth:" with nothing after it)
        # must fall back too, or it would resolve to 0 and silently switch
        # the low-gas warning off -- the one outcome an emptied guard should
        # never produce.
        min_base_eth=_parse_non_negative_float(
            "min_base_eth", warp.get("min_base_eth"), 0.005
        ),
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
        relayer: Any = None,
        relay_key: Any = None,
        heartbeat_publisher: Optional[Callable[[str, dict, float], bool]] = None,
        relay_state_saver: Optional[Callable[[], None]] = None,
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
        # Altruistic relay (opt-in): the sweeper instance, the key it signs
        # with (a dedicated gas-only key when configured, else the hot key),
        # and an injectable heartbeat publisher for tests.
        self._relayer = relayer
        self._relay_key = relay_key
        self._heartbeat_publisher = heartbeat_publisher
        self._relay_state_saver = relay_state_saver
        self._next_relay_sweep: float = 0.0
        self._relay_report: Dict[str, Any] = {}
        self._relay_activity: Dict[str, Any] = {}
        self._next_activity_refresh: float = 0.0
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
        self._sweep_dest_cache: Optional[bytes] = None
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
            # Outbound (unwrap).
            JobStatus.UNWRAP_CHECKS: self._h_unwrap_checks,
            JobStatus.BURN_SENT: self._h_burn_sent,
            JobStatus.BURNING: self._h_burning,
            JobStatus.COLLECTING_EVM_SIGS: self._h_collecting_evm_sigs,
            JobStatus.RELAYING: self._h_relaying,
            JobStatus.AWAITING_EXTERNAL_RELAY: self._h_awaiting_external_relay,
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

        expired = self._pending_expired(job)
        if expired is not None:
            return self._apply_terminal(job, WarpTerminal(expired))

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
        # A new state starts its own count; see _pending_expired.
        state = dict(result.state or {})
        state["pending_polls"] = 0
        updated = self._store.update_job(
            job.id,
            status=result.next_status,
            expected_status=job.status,
            columns=columns,
            state_patch=state,
            event=("transition", message, None),
        )
        return _job_dict(updated)

    def _pending_expired(self, job: WarpJob) -> Optional[str]:
        """Why this job has pended too long in its current state, or ``None``."""
        limit = _PENDING_MAX_POLLS.get(job.status)
        if limit is None:
            return None
        polls = int(job.state.get("pending_polls") or 0)
        if polls < limit:
            return None
        hours = polls * float(self._params.poll_interval_s) / 3600.0
        # No Sweep suggestion for the Base-side states: they have no funding
        # coin, and pointing the operator at Sweep there aimed them straight at
        # the close-over-a-live-bridge hazard the sweep guard now blocks.
        # Sweep is only suggested where a sweepable funding coin exists: the
        # inbound Chia-side states. Suggesting it for BURN_SENT pointed the
        # operator straight at burying a live burn commitment.
        sweepable_pends = (
            JobStatus.MESSAGE_SENT, JobStatus.FUNDING_CLAIM,
            JobStatus.CLAIM_FUNDED, JobStatus.COLLECTING_SIGS,
            JobStatus.CLAIMING, JobStatus.BURNING,
        )
        remedy = (
            "Retry re-attempts it, Sweep recovers the funding coin"
            if job.status in sweepable_pends
            else "Retry re-attempts it"
        )
        return (
            f"no progress in {job.status} after {polls} polls (~{hours:.1f}h); "
            f"handing over to the operator -- {remedy}"
        )

    def _apply_stay(self, job: WarpJob, result: _Step) -> dict:
        """Healthy stay/pending: schedule the next poll, clear any prior error."""
        columns: Dict[str, Any] = {
            "retry_count": 0,
            "last_error": None,
            "next_retry_at": repr(self._now() + float(self._params.poll_interval_s)),
        }
        if result.columns:
            columns.update(result.columns)
        state = dict(result.state or {})
        # Count pends in a deadline-bearing state. Reset on every advance, so
        # this measures time in *this* state rather than the age of the job.
        if job.status in _PENDING_MAX_POLLS:
            state["pending_polls"] = int(job.state.get("pending_polls") or 0) + 1
        event = ("progress", result.message, None) if result.message else None
        updated = self._store.update_job(
            job.id,
            expected_status=job.status,
            columns=columns,
            state_patch=state or None,
            event=event,
        )
        return _job_dict(updated)

    def _apply_retry(self, job: WarpJob, exc: Exception) -> dict:
        """Transient failure: exponential backoff, bump ``retry_count``."""
        n = int(job.retry_count or 0) + 1
        delay = min(_BACKOFF_BASE_S * (2 ** (n - 1)), _BACKOFF_CAP_S)
        message = str(exc)[:500]
        # A retryable error counts toward the state's deadline too. Only
        # _apply_stay advanced the counter, so a state that kept *erroring*
        # rather than pending never expired -- and the push allow-list added on
        # this branch turns an unrecognised claim rejection into exactly that
        # kind of loop.
        state = (
            {"pending_polls": int(job.state.get("pending_polls") or 0) + 1}
            if job.status in _PENDING_MAX_POLLS
            else None
        )
        updated = self._store.update_job(
            job.id,
            expected_status=job.status,
            columns={
                "retry_count": n,
                "last_error": message,
                "next_retry_at": repr(self._now() + delay),
            },
            state_patch=state,
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
            # Clear the pending counter with the failure. _pending_expired runs
            # before the handler and only _apply resets the counter, so a job
            # failed by the deadline kept it at the limit: Retry restored the
            # status and the very next tick re-expired it. That made Retry a
            # dead button for all five deadline states -- and pushed the
            # operator toward Sweep, which on a CLAIMING job with a pushed
            # claim bundle would race the bot's own claim for the same coin.
            state_patch={"failed_from": job.status, "pending_polls": 0},
            event=("failed", message, None),
        )
        return _job_dict(updated)

    # ================================================================== #
    # State handlers (each: read -> one side effect -> returned _Step).
    # ================================================================== #

    def _job_asset(self, job: WarpJob):
        """The bridgeable asset *job* was created for, from its frozen state.

        Jobs stamp their asset at creation, exactly like the network and
        hot-wallet binding, so a config edit mid-flight cannot re-point a
        live job at a different token. Rows written before asset stamping
        existed carry no symbol and are USDC by definition -- that was the
        only asset the pipeline could bridge. An unknown symbol is terminal
        rather than a silent fallback: guessing the token here would size
        amounts and derive the wrapped TAIL against the wrong asset.
        """
        symbol = str(job.state.get("asset") or "USDC")
        try:
            return self._net.asset(symbol)
        except KeyError:
            raise WarpTerminal(
                f"job {job.id} names asset {symbol!r}, which {self._net.name} "
                "does not list as bridgeable -- refusing to guess"
            ) from None

    def _h_awaiting_deposit(self, job: WarpJob) -> _Step:
        """Watch the hot wallet for a USDC deposit; freeze amounts on arrival."""
        from . import evm

        net, p = self._net, self._params
        spec = self._job_asset(job)

        have = self._evm.get_erc20_balance(spec.erc20_address, self._hot_address)
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
            raise WarpPending(
                f"awaiting a {spec.symbol} deposit (have {have})")
        if not manual and p.min_micros > 0 and want < p.min_micros:
            raise WarpPending(
                f"awaiting auto-bridge deposit >= {p.min_micros} micros (have {have})"
            )

        eth = self._evm.get_eth_balance(self._hot_address)
        if eth < _MIN_GAS_WEI:
            raise WarpRetryable(f"insufficient ETH for gas: {eth} < {_MIN_GAS_WEI} wei")

        mojo_amount = evm.bridgeable_mojo(want, net, spec)
        if mojo_amount <= 0:
            raise WarpPending("deposit below one bridgeable mojo")
        amount_base = evm.mojo_to_base_units(mojo_amount, net, spec)
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

        spec = self._job_asset(job)
        needed = int(job.amount_usdc_micros or 0)
        allowance = self._evm.get_bridge_allowance(self._hot_address, asset=spec)
        if allowance >= needed:
            return _advance(
                JobStatus.BRIDGING,
                state={"approve_raw": None},
                message=f"allowance {allowance} >= {needed}; skipping approve",
            )

        raw = job.state.get("approve_raw")
        if not raw:  # phase 1: sign + persist, no broadcast
            unsigned = self._evm.prepare_approve(
                owner=self._hot_address, amount_base_units=needed, asset=spec
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
        if self._job_dry_run(job):
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
        if not evm.receipt_succeeded(receipt):
            # [WARP-RECEIPT-AMBIG 2026-08-08] Not reverted is not the same as
            # succeeded. Branching on receipt_reverted alone sent a receipt with
            # no parseable status down the success path, advancing to BRIDGING
            # on an allowance that may never have taken effect.
            polls = int(job.state.get("approve_status_polls", 0)) + 1
            if polls >= _AMBIGUOUS_RECEIPT_POLLS:
                raise WarpTerminal(
                    f"approve receipt still carries no status after {polls} polls; "
                    "check the transaction on Base before retrying"
                )
            return _stay(
                state={"approve_status_polls": polls},
                message=f"approve receipt has no status; re-polling ({polls})",
            )
        return _advance(
            JobStatus.BRIDGING, state={"approve_raw": None}, message="approve confirmed"
        )

    def _h_bridging(self, job: WarpJob) -> _Step:
        """Two-phase ``bridgeToChia``; on success, parse the MessageSent nonce."""
        from . import evm

        net = self._net
        spec = self._job_asset(job)
        raw = job.state.get("bridge_raw")
        if not raw:  # phase 1: sign at a live toll/nonce + persist, no broadcast
            unsigned = self._evm.prepare_bridge(
                owner=self._hot_address,
                receiver_ph=bytes.fromhex(job.receiver_ph),
                mojo_amount=int(job.amount_mojos),
                asset=spec,
            )
            signed = evm.sign_tx(unsigned, self._evm_key.private_key)
            return _stay(
                columns={"bridge_tx_hash": signed.tx_hash},
                state={
                    "bridge_raw": signed.raw.hex(),
                    "bridge_tx_nonce": signed.nonce,
                    # Kept so a replacement escalates from the fee this was
                    # actually signed at. bump_fees derives every attempt from
                    # one base, which is what keeps consecutive attempts a full
                    # 1.3x apart -- comfortably over the node's 10% replacement
                    # floor. Re-reading live fees each time would lose that.
                    "bridge_fees": [
                        unsigned.max_fee_per_gas,
                        unsigned.max_priority_fee_per_gas,
                    ],
                },
                message="bridge signed; broadcasting next tick",
            )

        # Rehearsal stop -- the last instant before anything becomes irreversible.
        # Everything up to here has been exercised for real: the DPAPI key, the
        # Base RPC, the wallet daemon, the receiver decode, the live message toll
        # and tip, gas estimation, nonce selection, ABI encoding and signing. The
        # only thing that does not happen is the broadcast.
        if self._job_dry_run(job):
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
        mined_hash, receipt = self._bridge_receipt(job)
        if receipt is None:
            return self._bridge_unmined_step(job)
        if evm.receipt_reverted(receipt):
            attempt = int(job.state.get("bridge_attempt", 0)) + 1
            if attempt >= _BRIDGE_MAX_ATTEMPTS:
                raise WarpTerminal(f"bridge reverted {attempt} times")
            return _stay(
                columns={"bridge_tx_hash": None},
                state={
                    "bridge_raw": None,
                    "bridge_attempt": attempt,
                    # The abandoned nonce's replacement bookkeeping must go with
                    # it. Left behind, _bridge_hashes keeps returning the old
                    # hashes, so the very next poll finds THIS reverted receipt
                    # again and fails a freshly signed bridge at a different
                    # nonce -- while that new transaction is live in the mempool.
                    "bridge_prior_hashes": None,
                    "bridge_fee_bumps": None,
                    "bridge_unmined_polls": None,
                    "bridge_fees": None,
                    # Receipt-specific like the rest: left behind, attempt 2's
                    # first ambiguous receipt inherits attempt 1's count and
                    # goes terminal at a fraction of the documented budget.
                    "bridge_status_polls": None,
                },
                message=f"bridge reverted; re-preparing (attempt {attempt})",
            )
        if not evm.receipt_succeeded(receipt):
            # See _h_approving: an unparseable status is neither outcome, and
            # here the wrong guess advances a bridge that may not have moved
            # anything -- taking the MessageSent nonce with it.
            polls = int(job.state.get("bridge_status_polls", 0)) + 1
            if polls >= _AMBIGUOUS_RECEIPT_POLLS:
                raise WarpTerminal(
                    f"bridge receipt still carries no status after {polls} polls; "
                    "check the transaction on Base before retrying"
                )
            return _stay(
                state={"bridge_status_polls": polls},
                message=f"bridge receipt has no status; re-polling ({polls})",
            )
        msg_nonce = evm.parse_message_sent_nonce(receipt, net.portal_address)
        return _advance(
            JobStatus.BRIDGE_CONFIRMED,
            # Pin the column to the signing that actually mined. After a fee
            # bump this may be an earlier hash than the one we last signed, and
            # _h_bridge_confirmed polls this column for its confirmation count.
            columns={"bridge_nonce": msg_nonce, "bridge_tx_hash": mined_hash},
            state={"bridge_raw": None, "bridge_prior_hashes": None},
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
        spec = self._job_asset(job)
        if _word(msg.erc20_source) != _word(spec.erc20_address):
            raise WarpTerminal(
                f"attested source token {_hx(msg.erc20_source)} != this job's "
                f"{spec.symbol} {_hx(spec.erc20_address)}"
            )

        # Persist-then-act: generate the ephemeral security key locally; it lands
        # atomically with the advance. A crash before persistence just regenerates
        # (nothing was funded to the discarded key).
        # [WARP-MSG-WIDTH 2026-08-08] Validate the attested fields before
        # anything is minted or funded. Nothing checked them, and _classify
        # treats a ValueError as retryable with no cap -- so a malformed
        # attestation degraded into a silent infinite backoff, with the
        # ephemeral key already generated and the failure surfacing only much
        # later, deep in the claim build.
        #
        # The destination is a puzzle hash and is always 32 bytes. The contents
        # are NOT: they are heterogeneous CLVM atoms, and the amount in
        # particular is a minimal-encoding integer (a couple of bytes), so any
        # uniform width rule here would reject every real message. What is
        # checked is that each one is decodable at all -- which is exactly what
        # would otherwise raise later, out of reach of a terminal state.
        dest_hex = _hx(msg.destination)
        if len(dest_hex) != 64:
            raise WarpTerminal(
                f"watcher returned a {len(dest_hex) // 2}-byte message destination, "
                "expected 32; the attestation cannot build a claim"
            )
        try:
            contents_hex = [_hx(c) for c in msg.contents]
            for atom in contents_hex:
                bytes.fromhex(atom)
        except (ValueError, TypeError) as exc:
            raise WarpTerminal(
                f"watcher returned undecodable message contents ({exc}); the "
                "attestation cannot build a claim"
            ) from exc

        bls = keystore.new_bls_key()
        blob = keystore.protect_bls_key(
            bls, extra_entropy=self._job_entropy(job), protector=self._protector
        )
        return _advance(
            JobStatus.FUNDING_CLAIM,
            state={
                "ephemeral_blob": blob,
                "ephemeral_pk": bls.public_key.hex(),
                "message_destination": dest_hex,
                "message_contents": contents_hex,
            },
            message="attested 'sent'; ephemeral security key generated",
        )

    def _require_spendable_xch(self, needed: int, *, context: str) -> None:
        """Pend with a NAMED shortfall when wallet 1 cannot cover *needed*.

        The gates at UNWRAP_CHECKS run once, but the trading engine can lock
        XCH into offer collateral between that gate and the spend that needs
        it.  Without this re-check the send fails deep inside the wallet
        daemon and the job retries on a generic error -- the operator sees a
        silent stall instead of a message naming the missing mojos.  Always
        WarpPending, never terminal: collateral frees when offers close, and
        the job resumes on its own.  needed <= 0 is a no-op so the default
        zero-fee paths add no wallet RPC.
        """
        if needed <= 0:
            return
        bal = self._wallet.get_wallet_balance(1)
        spendable = int(bal.get("spendable_balance") or 0)
        if spendable < needed:
            raise WarpPending(
                f"{context}: spendable XCH {spendable} mojos < required "
                f"{needed} mojos (offer collateral may hold the rest; "
                "waiting for it to free)"
            )

    def _h_funding_claim(self, job: WarpJob) -> _Step:
        """Fund the security coin (post-tip + fee) -- dedupe-scan, never re-send."""
        from . import claim, clvm_utils as cu

        net, p = self._net, self._params
        sk = self._load_ephemeral_sk(job)
        expected = self._funded_amount(job)

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

        # Re-check ON THE TICK THAT SENDS: the wrap gates ran long ago and
        # the claim amount (post_tip + claim fee) comes out of spendable XCH.
        self._require_spendable_xch(
            expected + p.chia_funding_fee_mojos,
            context="claim security-coin funding",
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

        net = self._net
        sk = self._load_ephemeral_sk(job)
        expected = self._funded_amount(job)

        coin = claim.find_security_coin(self._coinset, sk, expected)
        if coin is None:
            raise WarpPending("security coin not yet on chain")
        depth = self._chia_depth(coin.name().hex())
        if depth is None:
            raise WarpPending("security coin not yet indexed")
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
        portal = self._sync_portal(job)
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

        net = self._net
        sk = self._load_ephemeral_sk(job)
        expected = self._funded_amount(job)
        # The bundle must balance against the coin that actually exists, so the
        # fee it spends is the one that was funded, not today's config value.
        claim_fee = expected - int(job.post_tip_mojos or 0)

        # Completion first (idempotent resume): our predicted final CAT coin id is
        # derived from our own bundle, so its presence means *our* claim landed.
        final_hex = job.state.get("final_cat_coin_id")
        if final_hex and claim.claim_landed(self._coinset, bytes.fromhex(final_hex)):
            # [WARP-CLAIM-DEPTH 2026-08-08] Existence is not finality. This
            # advanced to COMPLETED as soon as the coin appeared -- depth 1 --
            # even though chia_confirmation_min_height is 32 and
            # _h_claim_funded already waits that long for the *funding* coin.
            # COMPLETED is a closed state that frees the active slot, so a
            # reorg afterwards would reopen a claim the operator had been told
            # was finished, with the job no longer tracking it.
            depth = self._chia_depth(final_hex)
            if depth is None:
                raise WarpPending("final CAT coin not indexed yet")
            if depth < net.chia_confirmation_min_height:
                raise WarpPending(
                    f"claim {depth}/{net.chia_confirmation_min_height} confirmations"
                )
            return _advance(
                JobStatus.COMPLETED,
                message=f"claim confirmed at {depth} confirmations",
            )

        coin = claim.find_security_coin(self._coinset, sk, expected)
        if coin is None:
            sec_id = job.state.get("security_coin_id")
            if sec_id and claim.security_coin_spent(
                self._coinset, bytes.fromhex(sec_id)
            ):
                raise WarpPending("security coin spent; awaiting final CAT coin")
            raise WarpPending("security coin not found on chain")

        portal = self._sync_portal(job)
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
            claim_fee=claim_fee,
            # Anchor the claim against the asset THIS job bridged, not a
            # single global constant: the TAIL derived from the attested
            # token must match the job's own expected wrapped id.
            expected_asset_id=self._job_asset(job).expected_asset_id,
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

    def _sweep_destination_ph(self) -> bytes:
        """Where recovered XCH goes: the bot's own wallet, always.

        [WARP-SWEEP-DEST 2026-08-08] The sweep used ``job.receiver_ph``, which
        is the *wUSDC.b* receiver -- and once ``warp.chia_receiver_address`` is
        set that can be anywhere, including an address the bot does not
        control. build_sweep_bundle emits a bare CREATE_COIN of raw XCH, not a
        CAT, so the recovered funding fee was paid to whoever holds the CAT
        receiver rather than back to the wallet that put it up. Dust, and
        spendable by that holder, so misdirected rather than destroyed -- but
        it contradicted both docstrings, and the third-party-claim auto-sweep
        does it unattended.

        Cached like :meth:`_resolve_receiver`, and for the same reason:
        ``new_address=True`` would burn a derivation index per sweep.
        """
        from . import clvm_utils as cu

        if self._sweep_dest_cache is not None:
            return self._sweep_dest_cache
        self._wallet.log_in()
        address = self._wallet.get_next_address(1, new_address=False)
        ph = cu.decode_puzzle_hash(address, expected_prefix=self._net.chia_prefix)
        self._sweep_dest_cache = ph
        return ph

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
        """The (network, hot wallet, rehearsal) identity a new job is frozen against."""
        return {
            "network": self._net.name,
            "hot_address": _hx(self._hot_address),
            # [WARP-DRYRUN-FREEZE 2026-08-08] Frozen, never re-read: a job
            # created live must not become a rehearsal because the flag moved
            # under it. See _job_dry_run.
            "dry_run": bool(self._params.dry_run),
        }

    def _job_dry_run(self, job: WarpJob) -> bool:
        """Whether *job* was created as a rehearsal, per the job itself.

        Read from job state rather than live config. The rehearsal stops in
        :meth:`_h_approving` and :meth:`_h_bridging` used ``self._params``, so
        flipping ``warp.dry_run`` while a live job sat in BRIDGING closed it as
        DRY_RUN_OK and nulled ``bridge_tx_hash`` -- freeing the slot and leaving
        an audit trail reading "no funds moved" while real USDC was in flight.
        ``dry_run`` defaults to true, so a plain restart was enough to do it.

        Rows written before the freeze have no recorded value; those are refused
        outright in the two states where it decides anything (see
        :meth:`_binding_mismatch`), because neither default is safe there.
        """
        frozen = job.state.get("dry_run")
        return bool(self._params.dry_run if frozen is None else frozen)

    def _retry_tx_reset(self, job: WarpJob) -> Dict[str, Any]:
        """State to clear so a retry re-signs, rather than replaying a dead tx.

        [WARP-RETRY-RAW 2026-08-08] A job that FAILED out of APPROVING or
        BRIDGING still holds the raw it was last signed as -- the final revert
        raises WarpTerminal without clearing it, unlike the earlier attempts.
        Retry restored the status and phase 2 rebroadcast that same raw, so the
        job reverted again immediately and the operator's only affordance did
        nothing.

        Clearing it is only safe once the nonce cannot mine. The stuck-tx path
        also lands in FAILED, and there the transaction may still be sitting in
        the mempool: re-signing would take a *new* nonce and bridge a second
        time. So the nonce is checked against the chain first, and while it is
        still open the raw is kept and the retry just resumes polling it.
        """
        target = job.state.get("failed_from")
        if target == JobStatus.RELAYING:
            # Unconditional, and safe ONLY here: a relay re-signed at a
            # fresh nonce cannot double-deliver -- the second attempt
            # reverts "!nonce". The bridge below has no such guard, which
            # is why its clear is receipt-gated.
            return {"relay_raw": None, "relay_tx_hash": None,
                    "relay_status_polls": None, "relay_unmined_polls": None,
                    "relay_fee_bumps": None, "relay_prior_hashes": None,
                    "relay_fees": None, "relay_attempt": None}
        if target not in (JobStatus.APPROVING, JobStatus.BRIDGING):
            return {}

        # "approve_nonce" is what _h_approving actually persists. This read
        # said "approve_tx_nonce", a key nothing writes, so the guard below was
        # inert for APPROVING: nonce was always None, the chain check was
        # skipped, and the raw was cleared unconditionally -- exactly the
        # unsafe behaviour the docstring above says it prevents.
        nonce = job.state.get(
            "bridge_tx_nonce" if target == JobStatus.BRIDGING else "approve_nonce"
        )
        if nonce is not None:
            try:
                mined = int(self._evm.get_nonce(self._hot_address, pending=False))
            except Exception:  # noqa: BLE001 -- cannot prove it is dead, so keep it
                return {}
            if mined <= int(nonce):
                return {}          # still live: replaying is safer than re-signing
            if target == JobStatus.BRIDGING:
                # A consumed nonce is ambiguous: someone else took it, or OUR
                # bridge mined during the outage that failed the job. Only a
                # receipt distinguishes them, and clearing on a mined bridge
                # re-signs a second bridgeToChia at a fresh nonce -- a true
                # double bridge against the same deposit. Keep everything: the
                # resumed phase 2's rebroadcast is benign ("nonce too low") and
                # _bridge_receipt routes the mined receipt to success/revert.
                _mined_hash, receipt = self._bridge_receipt(job)
                if receipt is not None:
                    return {}

        if target == JobStatus.BRIDGING:
            return {
                "bridge_raw": None,
                "bridge_status_polls": None,
                "bridge_unmined_polls": None,
                "bridge_fee_bumps": None,
                "bridge_prior_hashes": None,
            }
        return {"approve_raw": None, "approve_tx_hash": None, "approve_status_polls": None}

    def _bridge_hashes(self, job: WarpJob) -> list:
        """Every hash this job's bridge nonce has been signed under, newest first."""
        hashes = [job.bridge_tx_hash] if job.bridge_tx_hash else []
        hashes += [h for h in (job.state.get("bridge_prior_hashes") or []) if h]
        return hashes

    def _bridge_receipt(self, job: WarpJob) -> tuple:
        """``(tx_hash, receipt)`` for whichever signing of this nonce mined.

        A fee replacement reuses the nonce, so at most one of these can ever
        have a receipt -- but which one is the node's choice, not ours. Polling
        only the newest hash would miss the case where the original mined just
        as the replacement went out.

        The winning hash is returned, not just the receipt: ``bridge_tx_hash``
        still points at the last thing we signed, and if the *original* mined
        that column names a transaction which will never exist. _h_bridge_confirmed
        polls it, get_confirmations returns 0 forever, and the job stalls until
        the BRIDGE_CONFIRMED deadline fails it -- with real USDC bridged and the
        MessageSent nonce correctly recorded. The caller writes this back.
        """
        for tx_hash in self._bridge_hashes(job):
            receipt = self._evm.get_transaction_receipt(tx_hash)
            if receipt is not None:
                return tx_hash, receipt
        return None, None

    def _bridge_unmined_step(self, job: WarpJob) -> "_Step":
        """Nothing has mined yet: escalate the fee, or stop if the nonce is gone.

        [WARP-STUCK-TX 2026-08-08] This path used to be a bare
        ``_stay("awaiting receipt")``. :meth:`_apply_stay` resets ``retry_count``
        and clears ``last_error`` on every pend, so the loop was invisible and
        unbounded; ``_BRIDGE_MAX_ATTEMPTS`` only counts *reverted* receipts,
        never an absent one. BRIDGING is not cancellable, Retry on a non-FAILED
        job only clears backoff, and Sweep closes only FAILED -- so a bridge
        that never mined held the single active job row forever and every later
        bridge raised ActiveJobExists. Recovery meant editing warp_jobs.db by
        hand.

        The likeliest cause is plain underpricing: ``suggest_fees`` sets
        maxFee = base*2 + priority, so any sustained climb past 2x strands the
        transaction with nothing to escalate it. ``bump_fees`` was written for
        exactly this and had no callers.
        """
        from . import evm

        nonce = job.state.get("bridge_tx_nonce")
        if nonce is not None:
            mined = int(self._evm.get_nonce(self._hot_address, pending=False))
            if mined > int(nonce):
                # The account has moved past our nonce and none of our hashes
                # has a receipt, so something else consumed it. Nothing we
                # signed can mine now, and no funds of ours moved.
                raise WarpTerminal(
                    f"bridge nonce {nonce} was consumed by another transaction "
                    f"(account is at {mined}) and nothing this job signed "
                    "mined; the USDC deposit is untouched"
                )

        polls = int(job.state.get("bridge_unmined_polls", 0)) + 1
        if polls < _STUCK_TX_POLLS:
            return _stay(
                state={"bridge_unmined_polls": polls},
                message=f"bridge broadcast; awaiting receipt (poll {polls})",
            )

        bumps = int(job.state.get("bridge_fee_bumps", 0)) + 1
        base = job.state.get("bridge_fees")
        if bumps > _MAX_FEE_BUMPS or not base:
            raise WarpTerminal(
                f"bridge has not mined after {bumps - 1} fee escalations; it is "
                "stuck and needs an operator decision (the nonce can be cleared "
                "with a replacement from the wallet)"
            )

        fees = evm.bump_fees(
            evm.EIP1559Fees(max_fee_per_gas=int(base[0]),
                            max_priority_fee_per_gas=int(base[1])),
            bumps,
        )
        signed = evm.sign_tx(
            self._evm.prepare_bridge(
                owner=self._hot_address,
                receiver_ph=bytes.fromhex(job.receiver_ph),
                mojo_amount=int(job.amount_mojos),
                nonce=int(nonce) if nonce is not None else None,
                fees=fees,
            ),
            self._evm_key.private_key,
        )
        return _stay(
            columns={"bridge_tx_hash": signed.tx_hash},
            state={
                "bridge_raw": signed.raw.hex(),
                # The old hash stays pollable: the node may still mine it.
                "bridge_prior_hashes": self._bridge_hashes(job),
                "bridge_fee_bumps": bumps,
                "bridge_unmined_polls": 0,
            },
            message=(
                f"bridge unmined after {polls} polls; re-signed nonce {nonce} at a "
                f"higher fee (escalation {bumps} of {_MAX_FEE_BUMPS})"
            ),
        )

    def _chia_depth(self, coin_id_hex: str) -> Optional[int]:
        """Confirmations behind the peak for a coin, or ``None`` if unindexed."""
        record = self._coinset.get_coin_record_by_name(coin_id_hex)
        if record is None:
            return None
        return self._coinset.get_peak_height() - int(record.confirmed_block_index) + 1

    def _funded_amount(self, job: WarpJob) -> int:
        """The amount the security coin was actually funded with.

        [WARP-FEE-FREEZE 2026-08-08] ``find_security_coin`` matches the on-chain
        amount exactly, so every lookup has to use what was *sent*, not what
        ``claim_fee_mojos`` says now. The three hot-path handlers recomputed it
        from live params: editing that key and reloading made them scan for an
        amount no coin has, so the claim raised WarpPending forever, and because
        :meth:`_apply_stay` resets ``retry_count`` and clears ``last_error`` on
        every pend, the job never reached FAILED and its context menu stayed
        empty. Only reverting the config recovered it.

        Falls back to live config for rows funded before the amount was
        recorded, which is what :meth:`_sweep_job` already did.
        """
        return int(
            job.state.get("funding_amount")
            or int(job.post_tip_mojos or 0) + int(self._params.claim_fee_mojos)
        )

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
        # Checked before the hot-wallet binding, which returns early on a match:
        # a row created between the two freezes carries hot_address but not
        # dry_run, and that is exactly the row this has to catch.
        if job.status in _DRY_RUN_SENSITIVE and job.state.get("dry_run") is None:
            # Neither default is safe here. Assume rehearsal and a live job is
            # closed DRY_RUN_OK with its broadcast tx hash discarded; assume
            # live and a rehearsal broadcasts real funds. Only the operator can
            # say which this row is.
            return (
                f"job {job.id} is in {job.status} but predates the dry_run "
                "freeze, so it cannot be proven to be a rehearsal or a live "
                "bridge; check the Base hot wallet for a broadcast transaction "
                "and resolve it manually"
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


    # ================================================================== #
    # Outbound (unwrap) handlers: wUSDC.b on Chia -> native USDC on Base.
    #
    # The commit point is the cat_spend in _h_burn_sent. Every gate runs
    # before it; after it the job is forward-only -- there is no outbound
    # Sweep of the burned CAT, only of the toll security coin, and the
    # failure mode is "stuck pending a relay", never "stolen": RECEIVER is
    # curried into the burn inner puzzle, so even a third party pushing the
    # burn pays us (docs/warp-unwrap-design.md par.3).
    # ================================================================== #

    def _resolve_cat_wallet_id(self) -> int:
        """The daemon wallet holding the wrapped asset, by TAIL hash."""
        from .wallet import WalletError

        want = self._net.expected_asset_id.lower()
        self._wallet.log_in()
        matches, errors = [], []
        for w in self._wallet.get_wallets(wallet_type=6):
            try:
                wid = int(w.get("id"))
                asset = _hx(self._wallet.cat_get_asset_id(wid)).lower()
            except (WalletError, TypeError, ValueError) as exc:
                errors.append(str(exc))
                continue
            if asset == want:
                matches.append(wid)
        if len(matches) == 1:
            return matches[0]
        if errors:
            # One flaky RPC must not FAIL the job: absence is only proven
            # when every candidate answered.
            raise WarpError(
                f"CAT wallet resolution degraded ({errors[0]}); retrying"
            )
        raise WarpTerminal(
            f"expected exactly one CAT wallet for asset "
            f"{self._net.expected_asset_id}, found {matches or 'none'}"
        )

    def _rebalance_unwrap_trim(self, job: WarpJob, amount: int):
        """Re-derive an automatic unwrap's size just before the commit.

        The deficit a rebalance job was sized from is a snapshot, and an
        inbound deposit does NOT touch this wallet's nonce -- so one can
        land while the job waits at the gate and shrink or erase the need.
        Burning the stale amount would overshoot the target and pay to
        bridge the excess back. Returns ``(step, amount)``: a step means
        stop here (nothing has been committed yet, so CANCELLED frees the
        slot cleanly); otherwise the possibly-reduced amount to burn.
        Manual unwraps carry no target and are never touched.
        """
        target = job.state.get("rebalance_target_micros")
        if not target:
            return None, amount
        held = int(self._evm.get_erc20_balance(
            self._net.usdc_address, self._hot_address))
        still_needed = int(target) - held
        if still_needed <= 0:
            return _advance(
                JobStatus.CANCELLED,
                message=("rebalance unwrap no longer needed: the USDC target "
                         "was met while the job waited"),
            ), amount
        # The planner's tip gross-up, re-derived from the live shortfall.
        grossed = -(-still_needed * 10_000 // (10_000 - rb._UNWRAP_TIP_BPS))
        needed_mojos = grossed // 1000
        if needed_mojos < 1:
            return _advance(
                JobStatus.CANCELLED,
                message=("rebalance unwrap no longer needed: the remaining "
                         "shortfall is below one mojo"),
            ), amount
        if needed_mojos < amount:
            _log.info("rebalance unwrap trimmed %d -> %d mojos (a deposit "
                      "landed while the job waited)", amount, needed_mojos)
            return None, int(needed_mojos)
        return None, amount

    def _h_unwrap_checks(self, job: WarpJob) -> _Step:
        """Every gate, ordered cheap-to-expensive, all before the cat_spend."""
        from . import clvm_utils as cu, drivers, evm

        net, p = self._net, self._params
        receiver = bytes.fromhex(job.state["receiver_evm"])
        amount = int(job.amount_mojos)

        # G0 (rebalance jobs only): re-derive the size at the gate.
        step, amount = self._rebalance_unwrap_trim(job, amount)
        if step is not None:
            return step

        # G1/G2: the burn anchor, offline then live. A mismatch on either is
        # a code defect or a bridge redeploy -- both stop everything.
        derived = cu.sha256tree(
            drivers.get_cat_burner_puzzle(
                b"bse", bytes.fromhex(net.erc20_bridge_address[2:])
            )
        )
        if derived.hex() != net.burn_puzzle_hash:
            raise WarpTerminal(
                "offline burn puzzle hash derivation does not match the anchor"
            )
        live = self._evm.get_burn_puzzle_hash()
        if live.hex() != net.burn_puzzle_hash:
            raise WarpTerminal(
                f"live burnPuzzleHash 0x{live.hex()} != anchor; bridge redeploy"
            )
        # G3: the EIP-712 domain. A MISMATCH is terminal (redeploy); transport
        # failures stay retryable -- split on the anchor wording.
        try:
            self._evm.verify_eip712_domain()
        except evm.EvmError as exc:
            if "redeploy" in str(exc) or "anchor" in str(exc):
                raise WarpTerminal(str(exc)) from exc
            raise

        # G4: the CAT wallet.
        wallet_id = int(job.state.get("cat_wallet_id") or self._resolve_cat_wallet_id())

        # G5: spendable vs confirmed. Over-confirmed is terminal (the amount
        # cannot exist); over-spendable only pends -- the difference is offer
        # collateral that frees as offers close.
        bal = self._wallet.get_wallet_balance(wallet_id)
        confirmed = int(bal.get("confirmed_wallet_balance") or 0)
        spendable = int(bal.get("spendable_balance") or 0)
        if amount > confirmed:
            raise WarpTerminal(
                f"unwrap amount {amount} exceeds the confirmed balance {confirmed}"
            )
        if amount > spendable:
            raise WarpPending(
                f"amount {amount} > spendable {spendable} "
                f"({confirmed - spendable} mojos are offer collateral)"
            )

        # G6/G7: live threshold sanity + the receiver's actual take.
        threshold = int(self._evm.get_signature_threshold())
        if not 0 < threshold <= len(net.evm_validator_addresses):
            raise WarpTerminal(f"live signatureThreshold {threshold} out of range")
        tip_bps = self._evm.get_tip_bps()
        post_tip = evm.unwrap_post_tip_base_units(
            amount, tip_bps, net.usdc_decimals, net.cat_decimals
        )

        # G8/G9/G10: liquidity, relay gas, toll funds -- all pends.
        liquidity = self._evm.get_erc20_balance(
            net.usdc_address, net.erc20_bridge_address
        )
        if liquidity < post_tip:
            raise WarpPending(
                f"bridge liquidity {liquidity} < post-tip {post_tip} base units"
            )
        # G9 is what an external-relay unwrap exists to not need: the operator
        # holds only Chia assets, and delivery gas is a volunteer's. Every
        # other gate still applies -- the burn itself is entirely Chia-side.
        if not job.state.get("external_relay"):
            if self._evm.get_eth_balance(self._hot_address) < _MIN_GAS_WEI:
                raise WarpPending("hot wallet ETH is below the relay gas floor")
        xch = self._wallet.get_wallet_balance(1)
        # The extra Chia fee is paid TWICE per unwrap -- once on the
        # cat_spend (BURN_SENT) and once on the toll-coin funding send
        # (BURNING) -- so the gate must budget both.  Counting it once let
        # a nonzero fee pass here and stall mid-burn with the CAT already
        # committed to the burn address.  Harmless at the default fee of 0,
        # which is exactly why it survived until the 2026-08 fee study.
        toll_need = net.chia_toll_mojos + 2 * p.unwrap_chia_fee_mojos
        if int(xch.get("spendable_balance") or 0) < toll_need:
            raise WarpPending(
                f"spendable XCH below the unwrap budget of {toll_need} mojos "
                "(toll + 2x unwrap_chia_fee_mojos: the fee is paid on both "
                "the cat_spend and the toll funding send)"
            )

        burn_inner_ph = drivers.expected_burn_inner_puzzle_hash(net, receiver)
        state = {
            "cat_wallet_id": wallet_id,
            "post_tip_base_units": post_tip,
            "burn_inner_ph": burn_inner_ph.hex(),
            "burn_cat_ph": drivers.expected_burn_cat_puzzle_hash(net, receiver).hex(),
            "burn_address": cu.encode_puzzle_hash(burn_inner_ph, net.chia_prefix),
        }
        if self._job_dry_run(job):
            return _advance(
                JobStatus.DRY_RUN_OK,
                state=state,
                message=(
                    "dry run OK: every unwrap gate passed (anchors, domain, "
                    f"wallet {wallet_id}, {amount} mojos spendable, liquidity, "
                    "gas, toll); the cat_spend was NOT sent -- nothing moved. "
                    "Set warp.dry_run: false to go live"
                ),
            )
        return _advance(
            JobStatus.BURN_SENT,
            state=state,
            message=f"all gates passed; sending {amount} mojos to the burn puzzle",
        )

    def _find_existing_burn(self, wallet_id: int, burn_cat_ph_hex: str,
                            amount: int):
        """An in-flight or live transaction that already created OUR burn CAT.

        Two hazards shaped this, both reproduced by review:

        * The daemon's default sort is confirmed-height descending and
          unconfirmed records sort LAST, so on a busy wallet (the quote-asset
          CAT wallet trades constantly) the just-sent cat_spend pages out of
          any windowed scan -- the crash-window dedupe would miss it and burn
          twice. The UNCONFIRMED set is scanned first, explicitly.
        * burn_cat_ph is a pure function of (net, receiver), so repeat unwraps
          to the same address hit the SAME puzzle hash: a prior unwrap's
          confirmed cat_spend must not be latched onto. Confirmed hits only
          count if the created coin is not already spent, and the amount must
          match; the returned coin id gives the completion scan provenance.

        Returns ``(tx_id, coin_id_hex or None)`` or ``None``.
        """
        def _scan(txs):
            for tx in txs:
                for add in tx.get("additions") or []:
                    if _hx(str(add.get("puzzle_hash") or "")) != burn_cat_ph_hex:
                        continue
                    if int(add.get("amount") or 0) != amount:
                        continue
                    parent = _hx(str(add.get("parent_coin_info") or ""))
                    coin_id = None
                    if parent:
                        from . import clvm_utils as cu

                        coin_id = cu.coin_name(
                            bytes.fromhex(parent),
                            bytes.fromhex(burn_cat_ph_hex),
                            amount,
                        ).hex()
                    if tx.get("confirmed") and coin_id:
                        rec = self._coinset.get_coin_record_by_name(coin_id)
                        if rec is not None and rec.spent:
                            continue          # a finished unwrap, not ours
                    tx_id = str(tx.get("name") or tx.get("transaction_id") or "found")
                    return tx_id, coin_id
            return None

        pending = self._wallet.get_transactions(
            wallet_id, start=0, end=200, confirmed=False
        )
        hit = _scan(pending)
        if hit:
            return hit
        return _scan(self._wallet.get_transactions(wallet_id, start=0, end=200))

    def _h_burn_sent(self, job: WarpJob) -> _Step:
        """THE COMMIT POINT: one cat_spend, guarded like the funding send."""
        net, p = self._net, self._params
        amount = int(job.amount_mojos)
        burn_cat_ph = job.state["burn_cat_ph"]

        # a) Completion first (idempotent resume). Provenance when we have it:
        # burn_cat_ph is (net, receiver)-scoped, not job-scoped, so a bare
        # ph+amount match can adopt an OLDER job's stranded coin -- reproduced
        # in review. The coin id captured from our own send wins; the scan is
        # only the fallback for a send whose response carried no additions.
        want_id = job.state.get("burn_cat_coin_id")
        if want_id:
            rec = self._coinset.get_coin_record_by_name(want_id)
            if rec is not None and not rec.spent:
                return _advance(
                    JobStatus.BURNING,
                    state={"burn_cat_parent": _hx(rec.coin.parent_coin_info)},
                    message="burn CAT is on chain; the unwrap is now forward-only",
                )
        else:
            for rec in self._coinset.get_coin_records_by_puzzle_hash(burn_cat_ph):
                if int(rec.coin.amount) == amount and not rec.spent:
                    return _advance(
                        JobStatus.BURNING,
                        state={"burn_cat_parent": _hx(rec.coin.parent_coin_info)},
                        message="burn CAT is on chain; the unwrap is now forward-only",
                    )

        # b) The persisted marker.
        prior = job.state.get("burn_tx_id")
        if prior:
            return _stay(message=f"cat_spend {prior} already sent; awaiting the coin")

        # c) Fail-closed history scan -- a lost response must not burn twice.
        try:
            found = self._find_existing_burn(
                int(job.state["cat_wallet_id"]), burn_cat_ph, amount
            )
        except Exception as exc:  # noqa: BLE001 -- cannot prove it was not sent
            raise WarpError(
                f"wallet history scan failed ({exc}); refusing to send the "
                "irreversible cat_spend blindly"
            ) from exc
        if found:
            tx_id, coin_id = found
            patch = {"burn_tx_id": tx_id}
            if coin_id:
                patch["burn_cat_coin_id"] = coin_id
            return _stay(
                state=patch,
                message="cat_spend already in the wallet history (dedupe hit)",
            )

        # d) Send. Default coin selection ONLY -- it is what honours the trade
        # manager's offer locks (the client has no coins parameter, by design).
        # The XCH fee (when configured) is re-checked on the sending tick;
        # G10's budget ran at UNWRAP_CHECKS and collateral may have moved.
        self._require_spendable_xch(
            p.unwrap_chia_fee_mojos, context="cat_spend fee"
        )
        self._wallet.log_in()
        record = self._wallet.cat_spend(
            int(job.state["cat_wallet_id"]),
            amount,
            job.state["burn_address"],
            fee_mojos=p.unwrap_chia_fee_mojos,
        )
        tx_id = record.get("name") or record.get("transaction_id")
        if not tx_id:
            tx_id = _FUNDING_TX_UNKNOWN
            _log.warning(
                "warp: daemon accepted the cat_spend for job %s but named no "
                "transaction; recording %r so the guard holds", job.id, tx_id,
            )
        # Provenance: the response's additions identify OUR burn CAT exactly,
        # which is what keeps repeat unwraps to one receiver disentangled.
        patch = {"burn_tx_id": tx_id}
        for add in record.get("additions") or []:
            if _hx(str(add.get("puzzle_hash") or "")) == burn_cat_ph \
                    and int(add.get("amount") or 0) == amount:
                parent = _hx(str(add.get("parent_coin_info") or ""))
                if parent:
                    from . import clvm_utils as cu

                    patch["burn_cat_coin_id"] = cu.coin_name(
                        bytes.fromhex(parent),
                        bytes.fromhex(burn_cat_ph),
                        amount,
                    ).hex()
                break
        return _stay(
            state=patch,
            message=f"cat_spend sent ({amount} mojos); the coin is forward-only",
        )

    def _h_burning(self, job: WarpJob) -> _Step:
        """Fund the toll coin, build + push the burn bundle, wait for depth."""
        from . import claim, clvm_utils as cu, drivers, keystore

        net, p = self._net, self._params
        amount = int(job.amount_mojos)
        receiver = bytes.fromhex(job.state["receiver_evm"])
        toll = int(net.chia_toll_mojos)

        # Completion first: the bridging coin (the nonce) confirmed at depth.
        nonce_hex = job.state.get("unwrap_nonce")
        if nonce_hex:
            depth = self._chia_depth(nonce_hex)
            if depth is not None:
                if depth < net.chia_confirmation_min_height:
                    raise WarpPending(
                        f"burn {depth}/{net.chia_confirmation_min_height} confirmations"
                    )
                return _advance(
                    JobStatus.COLLECTING_EVM_SIGS,
                    columns={"bridge_nonce": nonce_hex},
                    message=f"burn confirmed; message nonce {nonce_hex}",
                )

        # The ephemeral toll key, persisted before anything is funded to it.
        blob = job.state.get("ephemeral_blob")
        if not blob:
            bls = keystore.new_bls_key()
            return _stay(
                state={
                    "ephemeral_blob": keystore.protect_bls_key(
                        bls, extra_entropy=self._job_entropy(job),
                        protector=self._protector,
                    ),
                    "ephemeral_pk": bls.public_key.hex(),
                },
                message="ephemeral toll key generated",
            )
        sk = self._load_ephemeral_sk(job)

        # The persisted nonce is a pure function of ONE security coin. If a
        # different coin were elected on a later tick (double-funded rows, or
        # index races), the rebuilt bundle would carry a different bridging
        # coin id than the nonce the job polls -- stalling forever on a value
        # that can never appear. Reproduced in review; hence the pin: once a
        # coin is chosen, only that exact coin is used, and if it vanishes
        # unspent the nonce is re-derived WITH the replacement, atomically.
        sec = None
        pinned = job.state.get("security_coin_id")
        if pinned:
            rec = self._coinset.get_coin_record_by_name(pinned)
            if rec is not None and not rec.spent:
                from . import drivers as _drv

                sec = _drv.Coin(
                    bytes.fromhex(_hx(rec.coin.parent_coin_info)),
                    bytes.fromhex(_hx(rec.coin.puzzle_hash)),
                    int(rec.coin.amount),
                )
            elif rec is not None and rec.spent:
                raise WarpPending(
                    "pinned toll coin is spent; awaiting the bridging coin index"
                )
        if sec is None:
            sec = claim.find_security_coin(self._coinset, sk, toll)
        if sec is None:
            # Fund it -- the same marker/dedupe/send ladder as the claim path,
            # with funding_amount persisted so Sweep just works.
            prior = job.state.get("funding_tx_id")
            if prior:
                return _stay(message=f"toll tx {prior} sent; awaiting the coin")
            ph = claim.security_coin_puzzle_hash(sk)
            try:
                existing = self._find_existing_funding(ph.hex(), toll)
            except Exception as exc:  # noqa: BLE001
                raise WarpError(
                    f"wallet history scan failed ({exc}); refusing to fund blindly"
                ) from exc
            if existing:
                return _stay(
                    state={"funding_tx_id": existing, "funding_amount": toll},
                    message="toll funding already in flight (dedupe hit)",
                )
            self._require_spendable_xch(
                toll + p.unwrap_chia_fee_mojos, context="toll funding"
            )
            self._wallet.log_in()
            record = self._wallet.send_transaction(
                1, toll, cu.encode_puzzle_hash(ph, net.chia_prefix),
                fee_mojos=p.unwrap_chia_fee_mojos,
            )
            tx_id = (record.get("name") or record.get("transaction_id")
                     or _FUNDING_TX_UNKNOWN)
            return _stay(
                state={"funding_tx_id": tx_id, "funding_amount": toll},
                message=f"funded {toll} mojos to the toll coin",
            )

        # The burn CAT (created by the cat_spend; unspent until our bundle).
        burn_cat = None
        for rec in self._coinset.get_coin_records_by_puzzle_hash(
            job.state["burn_cat_ph"]
        ):
            if int(rec.coin.amount) == amount and not rec.spent:
                burn_cat = drivers.Coin(
                    bytes.fromhex(_hx(rec.coin.parent_coin_info)),
                    bytes.fromhex(_hx(rec.coin.puzzle_hash)),
                    int(rec.coin.amount),
                )
                break
        if burn_cat is None:
            raise WarpPending(
                "burn CAT not found unspent; awaiting index (a landed bundle "
                "resolves via the nonce check next tick)"
            )

        lineage = claim.cat_lineage_proof_for(self._coinset, burn_cat.parent_coin_info)
        out = drivers.build_burn_bundle(drivers.BurnRequest(
            net=net, burn_cat_coin=burn_cat, cat_lineage_proof=lineage,
            security_coin=sec, security_sk=sk, receiver=receiver,
        ))
        # Persist the nonce AND the coin it derives from BEFORE pushing --
        # together, always: a nonce without its coin is the incoherence the
        # pin above exists to prevent. Deterministic, so a crash on either
        # side of the push re-derives identical values.
        self._store.update_job(
            job.id, expected_status=job.status,
            state_patch={
                "unwrap_nonce": out.nonce.hex(),
                "security_coin_id": sec.name().hex(),
            },
        )
        kind, status = claim.push_burn_bundle(self._coinset, out.bundle)
        return _stay(
            state={"unwrap_nonce": out.nonce.hex()},
            message=f"burn bundle pushed ({kind}: {status}); nonce {out.nonce.hex()}",
        )

    def _h_collecting_evm_sigs(self, job: WarpJob) -> _Step:
        """Collect ECDSA signatures over the EIP-712 digest until quorum."""
        from . import drivers, evm, nostr

        net = self._net
        nonce = bytes.fromhex(job.bridge_nonce)
        receiver = bytes.fromhex(job.state["receiver_evm"])

        separator = self._evm.verify_eip712_domain()
        contents = drivers.outbound_message_contents(
            net, receiver, int(job.amount_mojos)
        )
        digest = evm.validator_message_digest(
            separator, nonce, b"xch",
            bytes.fromhex(net.burn_puzzle_hash),   # the SOURCE is the cat_burner
            net.erc20_bridge_address,              # PUZZLE HASH, never a coin id
            contents,
        )
        threshold = int(self._evm.get_signature_threshold())

        have = {
            a: bytes.fromhex(v)
            for a, v in (job.state.get("evm_sigs") or {}).items()
        }
        round_ = int(job.state.get("collect_round", 0))
        result = self._collector.collect_ecdsa(
            nonce=nonce, digest=digest, threshold=threshold, have=have,
            deadline_s=_SIG_COLLECT_DEADLINE_S, relay_offset=round_,
        )
        encoded = {a: v.hex() for a, v in result.collected.items()}
        if result.complete:
            tuples = nostr.ecdsa_sigs_for_relay(result.collected, threshold)
            packed = evm.pack_validator_sigs(tuples)
            state = {
                "evm_sigs": encoded,
                "relay_sigs": packed.hex(),
                "relay_threshold": threshold,
            }
            # External-relay mode only bites when the gas is actually absent:
            # if the wallet can self-relay, it should -- faster for the
            # operator and one less message in the network's stuck tail.
            if (
                job.state.get("external_relay")
                and self._evm.get_eth_balance(self._hot_address) < _MIN_GAS_WEI
            ):
                return _advance(
                    JobStatus.AWAITING_EXTERNAL_RELAY,
                    state=state,
                    message=(
                        f"collected {result.count}/{threshold} EVM signatures; "
                        "no Base gas -- awaiting an altruistic relayer"
                    ),
                )
            return _advance(
                JobStatus.RELAYING,
                state=state,
                message=f"collected {result.count}/{threshold} EVM signatures",
            )
        return _stay(
            state={"evm_sigs": encoded, "collect_round": round_ + 1},
            message=f"collected {result.count}/{threshold} EVM signatures",
        )

    def _relay_calldata(self, job: WarpJob) -> bytes:
        from . import drivers, evm

        net = self._net
        return evm.encode_receive_message(
            bytes.fromhex(job.bridge_nonce), b"xch",
            bytes.fromhex(net.burn_puzzle_hash),
            net.erc20_bridge_address,
            drivers.outbound_message_contents(
                net, bytes.fromhex(job.state["receiver_evm"]),
                int(job.amount_mojos),
            ),
            bytes.fromhex(job.state["relay_sigs"]),
        )

    def _h_relaying(self, job: WarpJob) -> _Step:
        """Two-phase receiveMessage; '!nonce' means someone else delivered."""
        from . import evm

        net = self._net
        nonce = bytes.fromhex(job.bridge_nonce)

        raw = job.state.get("relay_raw")
        if not raw:  # phase 1: encode + sign + persist, no broadcast
            # The contract checks the sig blob length EXACTLY against the
            # owner-mutable live threshold; a change since collection would
            # make every relay revert forever. Signatures never expire, so
            # bouncing back to collection is lossless.
            live = int(self._evm.get_signature_threshold())
            if live != int(job.state.get("relay_threshold") or live):
                return _advance(
                    JobStatus.COLLECTING_EVM_SIGS,
                    state={"relay_sigs": None, "relay_threshold": None},
                    message=(
                        f"signatureThreshold changed to {live} since "
                        "collection; re-selecting signatures"
                    ),
                )
            unsigned = self._evm.prepare_relay(
                owner=self._hot_address, calldata=self._relay_calldata(job)
            )
            signed = evm.sign_tx(unsigned, self._evm_key.private_key)
            return _stay(
                state={
                    "relay_raw": signed.raw.hex(),
                    "relay_tx_hash": signed.tx_hash,
                    "relay_tx_nonce": signed.nonce,
                    "relay_fees": [unsigned.max_fee_per_gas,
                                   unsigned.max_priority_fee_per_gas],
                },
                message="relay signed; broadcasting next tick",
            )

        # phase 2: broadcast (benign-idempotent), then poll every hash.
        try:
            self._evm.send_raw_transaction(bytes.fromhex(raw))
        except evm.EvmError as exc:
            if evm.is_already_delivered(exc):
                return self._unwrap_delivered(job, by="a third-party relayer")
            raise

        mined_hash, receipt = self._relay_receipt(job)
        if receipt is None:
            return self._relay_unmined_step(job)
        if evm.receipt_reverted(receipt):
            # A mined-but-reverted own relay may be the front-run case:
            # someone else delivered between our broadcast and inclusion.
            # Receipts carry no revert reason, so replay the calldata as an
            # eth_call -- a '!nonce' revert NOW proves delivery, and the
            # receiver was paid either way.
            try:
                self._evm._eth_call(net.portal_address, self._relay_calldata(job))
            except evm.EvmError as exc:
                if evm.is_already_delivered(exc):
                    return self._unwrap_delivered(job, by="a third-party relayer")
            attempt = int(job.state.get("relay_attempt", 0)) + 1
            if attempt >= _BRIDGE_MAX_ATTEMPTS:
                raise WarpTerminal(
                    f"relay reverted {attempt} times; the message stays "
                    f"deliverable forever -- relay manually at {net.status_url}"
                )
            return _stay(
                state={"relay_raw": None, "relay_tx_hash": None,
                       "relay_attempt": attempt, "relay_prior_hashes": None,
                       "relay_unmined_polls": None, "relay_fees": None,
                       "relay_status_polls": None},
                message=f"relay reverted; re-preparing (attempt {attempt})",
            )
        if not evm.receipt_succeeded(receipt):
            polls = int(job.state.get("relay_status_polls", 0)) + 1
            if polls >= _AMBIGUOUS_RECEIPT_POLLS:
                raise WarpTerminal(
                    f"relay receipt has no status after {polls} polls; check "
                    "the transaction on Base before retrying"
                )
            return _stay(
                state={"relay_status_polls": polls},
                message=f"relay receipt has no status; re-polling ({polls})",
            )

        if not evm.parse_message_received(receipt, net.portal_address, nonce):
            raise WarpTerminal(
                "relay succeeded but the Portal emitted no MessageReceived for "
                "our nonce -- refusing to guess; check the transaction on Base"
            )
        confs = self._evm.get_confirmations(mined_hash)
        if confs < net.evm_confirmation_min_height:
            raise WarpPending(
                f"relay {confs}/{net.evm_confirmation_min_height} confirmations"
            )
        return self._unwrap_delivered(job, by="our relay", tx_hash=mined_hash)

    def _h_awaiting_external_relay(self, job: WarpJob) -> _Step:
        """Chia-only mode: wait for a stranger's relay, proven on-chain.

        Delivery detection is an eth_call replay of OUR calldata: a ``!nonce``
        revert is the Portal's own ``usedNonces`` refusal -- unforgeable
        evidence the receiver was paid. The watcher's
        ``destination_transaction_hash`` is recorded when present but is
        display provenance, never the completion signal. Whenever the hot
        wallet turns out to hold relay gas after all, the job switches to
        self-relay -- whichever path completes first wins.
        """
        from . import evm

        net = self._net

        # Threshold drift invalidates the packed blob exactly as in RELAYING;
        # signatures never expire, so bouncing back to collection is lossless.
        live = int(self._evm.get_signature_threshold())
        if live != int(job.state.get("relay_threshold") or live):
            return _advance(
                JobStatus.COLLECTING_EVM_SIGS,
                state={"relay_sigs": None, "relay_threshold": None},
                message=(
                    f"signatureThreshold changed to {live} while waiting; "
                    "re-selecting signatures"
                ),
            )

        try:
            self._evm._eth_call(net.portal_address, self._relay_calldata(job))
        except evm.EvmError as exc:
            if evm.is_already_delivered(exc):
                return self._unwrap_delivered(
                    job,
                    by="an altruistic relayer",
                    tx_hash=self._external_delivery_hash(job),
                )
            raise  # transport or an unexpected revert: retryable per taxonomy

        if self._evm.get_eth_balance(self._hot_address) >= _MIN_GAS_WEI:
            return _advance(
                JobStatus.RELAYING,
                message="hot wallet now holds relay gas; switching to self-relay",
            )
        raise WarpPending(
            "message is live and deliverable; waiting for an altruistic "
            "relayer (or fund the hot wallet with ~0.0003 ETH on Base to "
            f"self-relay). Also claimable any time at {net.status_url}"
        )

    def _external_delivery_hash(self, job: WarpJob) -> Optional[str]:
        """The watcher's destination tx hash -- display only, never the proof."""
        try:
            msg = self._watcher.get_message(job.bridge_nonce, source_chain="xch")
            raw = msg.raw if msg is not None else {}
            return str(raw.get("destination_transaction_hash") or "") or None
        except Exception:  # noqa: BLE001 -- provenance is best-effort
            return None

    def _unwrap_delivered(self, job: WarpJob, *, by: str, tx_hash=None) -> _Step:
        post_tip = job.state.get("post_tip_base_units")
        return _advance(
            JobStatus.COMPLETED,
            columns={"bridge_tx_hash": tx_hash} if tx_hash else None,
            state={"delivered_by": by},
            message=(
                f"unwrap complete ({by}): {post_tip} USDC base units paid to "
                f"0x{job.state.get('receiver_evm')}"
            ),
        )

    def _relay_hashes(self, job: WarpJob) -> list:
        hashes = []
        if job.state.get("relay_tx_hash"):
            hashes.append(job.state["relay_tx_hash"])
        hashes += [h for h in (job.state.get("relay_prior_hashes") or []) if h]
        return hashes

    def _relay_receipt(self, job: WarpJob) -> tuple:
        for tx_hash in self._relay_hashes(job):
            receipt = self._evm.get_transaction_receipt(tx_hash)
            if receipt is not None:
                return tx_hash, receipt
        return None, None

    def _relay_unmined_step(self, job: WarpJob) -> _Step:
        """The bridge's stuck-tx ladder with one safe difference: a relay
        replacement may take a FRESH nonce, because double delivery is
        structurally impossible ('!nonce'). A consumed nonce with no receipt
        therefore clears the raw and re-signs -- it never terminals."""
        from . import evm

        nonce = job.state.get("relay_tx_nonce")
        if nonce is not None:
            mined = int(self._evm.get_nonce(self._hot_address, pending=False))
            if mined > int(nonce):
                _mh, receipt = self._relay_receipt(job)
                if receipt is None:
                    return _stay(
                        state={"relay_raw": None, "relay_tx_hash": None,
                               "relay_unmined_polls": None},
                        message="relay nonce consumed elsewhere; re-signing",
                    )
        polls = int(job.state.get("relay_unmined_polls", 0)) + 1
        if polls < _STUCK_TX_POLLS:
            return _stay(
                state={"relay_unmined_polls": polls},
                message=f"relay broadcast; awaiting receipt (poll {polls})",
            )
        bumps = int(job.state.get("relay_fee_bumps", 0)) + 1
        base = job.state.get("relay_fees")
        if bumps > _MAX_FEE_BUMPS or not base:
            raise WarpTerminal(
                f"relay unmined after {bumps - 1} fee escalations; the message "
                f"stays deliverable forever -- relay manually at "
                f"{self._net.status_url}"
            )
        fees = evm.bump_fees(evm.EIP1559Fees(int(base[0]), int(base[1])), bumps)
        unsigned = self._evm.prepare_relay(
            owner=self._hot_address, calldata=self._relay_calldata(job),
            nonce=int(job.state["relay_tx_nonce"]), fees=fees,
        )
        signed = evm.sign_tx(unsigned, self._evm_key.private_key)
        return _stay(
            state={
                "relay_raw": signed.raw.hex(),
                "relay_tx_hash": signed.tx_hash,
                "relay_prior_hashes": self._relay_hashes(job),
                "relay_fee_bumps": bumps,
                "relay_unmined_polls": 0,
            },
            message=(
                f"relay unmined after {polls} polls; re-signed at a higher fee "
                f"(escalation {bumps} of {_MAX_FEE_BUMPS})"
            ),
        )

    def _sync_portal(self, job: WarpJob):
        """Resolve the portal tip, re-bootstrapping if the hint is too far behind.

        [WARP-PORTAL-HINT 2026-08-08] ``sync_portal`` walks forward a bounded
        number of hops from the hint. Once the portal has advanced further than
        that, every sync raises PortalSyncError and the job retries forever --
        the Nostr bootstrap that would fix it was consulted only when no hint
        existed at all, never when a persisted one had gone stale. A stored
        hint is therefore self-perpetuating: it can never be replaced by the
        mechanism designed to replace it.

        On failure the stored hint is cleared and one fresh bootstrap is tried.
        A second failure propagates as a normal retryable error.
        """
        from . import claim

        hint = self._resolve_portal_hint(job)
        try:
            return claim.sync_portal(self._coinset, self._net, hint=hint)
        except claim.PortalSyncError as exc:
            _log.warning(
                "warp: portal hint %s unusable (%s); discarding it and "
                "re-bootstrapping from Nostr",
                _hx(hint), exc,
            )
            self._store.set_meta("portal_hint", "")
            fresh = self._bootstrap_hint_from_nostr(job)
            return claim.sync_portal(self._coinset, self._net, hint=fresh)

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
            kind, status = claim.build_and_push_sweep(
                self._coinset,
                self._net,
                security_coin=coin,
                destination_puzzle_hash=self._sweep_destination_ph(),
                ephemeral_sk=sk,
                sweep_fee=0,
            )
        except Exception as exc:  # noqa: BLE001 -- operator can re-sweep later
            return False, f"sweep failed: {exc}"

        if kind == "conflict":
            # The coin is already spent -- by our own claim, or a prior sweep.
            # Nothing left to recover, so this genuinely is resolved.
            return True, status

        # [WARP-SWEEP-VERIFY 2026-08-08] "accepted" and "pending" only say the
        # node took the bundle into its mempool. Treating that as resolved
        # closed the job as CANCELLED with swept: True -- and CANCELLED offers
        # neither Retry nor Sweep -- so an evicted bundle left the funding coin
        # sitting unspent with no route back to it. The sweep pushes at zero
        # fee, which is exactly what a busy mempool drops.
        if claim.security_coin_spent(self._coinset, coin.name()):
            return True, status
        return False, (
            f"sweep pushed ({status}) but the security coin is not spent yet; "
            "run Sweep again once it confirms"
        )

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
        # A worker-managed USDC target band owns bridging: the legacy
        # auto-start would race it, opening an UNTARGETED job for any
        # balance above the minimum and bridging the full balance/cap
        # instead of only the amount above the target.
        if getattr(self, "auto_bridge_suppressed", False):
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
            state={"auto": True, "asset": "USDC", **self._binding()},
            event_message="auto-bridge: deposit detected",
        )
        return _job_dict(job)

    def request_bridge(
        self, target_micros: Optional[int] = None, *, automatic: bool = False
    ) -> dict:
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
                "resolve it first -- Retry, Cancel, Sweep or Abandon from the "
                "jobs table"
            )
        # Provenance matters twice over: the audit trail must say WHY the
        # funds moved, and _h_awaiting_deposit re-checks the auto min_micros
        # floor only for non-manual jobs -- a rebalance stamped manual could
        # bridge a sub-floor amount if the balance dropped after the consult.
        if automatic:
            state: Dict[str, Any] = {
                "auto": True, "rebalance": True, "asset": "USDC",
                **self._binding()
            }
        else:
            state = {"manual": True, "asset": "USDC", **self._binding()}
        if target_micros:
            state["target_micros"] = int(target_micros)
        job = self._store.create_job(
            self._net.name,
            status=JobStatus.AWAITING_DEPOSIT,
            state=state,
            event_message=("rebalance: automatic bridge requested"
                           if automatic else "manual bridge requested"),
        )
        return _job_dict(job)

    def _refuse_while_bridge_unresolved(self, job: WarpJob, action: str) -> None:
        """Refuse to close a job whose signed bridge is live or already mined.

        [WARP-NONCE-AMBIGUITY 2026-08-09] ``get_nonce`` alone cannot tell the
        two meanings of a consumed nonce apart: something *else* took it (the
        deposit is untouched -- closing is fine) versus *our own transaction
        mined* (real USDC is bridged and the job must be completed via Retry,
        which rebroadcasts benignly and routes the receipt through phase 2).
        The first version of this guard checked only liveness, so a mined
        bridge could be abandoned -- and Sweep's no-key branch, which concluded
        "nothing was ever funded" and closed, had no guard at all: the exact
        bypass the abandon guard existed to block, one button over.

        Raises unless the nonce is provably dead AND nothing this job signed
        has a receipt. Callers must be same-binding (the nonce check reads the
        configured hot wallet).
        """
        raw_nonce = job.state.get("bridge_tx_nonce")
        if not (job.state.get("bridge_raw") and raw_nonce is not None):
            return
        try:
            mined = int(self._evm.get_nonce(self._hot_address, pending=False))
        except Exception as exc:  # noqa: BLE001 -- cannot prove it is dead
            raise WarpError(
                f"cannot {action}: unable to verify whether the signed bridge "
                f"at nonce {raw_nonce} is still live ({exc}); try again when "
                "the Base RPC is reachable"
            ) from exc
        if mined <= int(raw_nonce):
            raise WarpError(
                f"cannot {action}: this job's signed bridge is still live at "
                f"nonce {raw_nonce}. Wait for it to mine (then Retry), or "
                "clear the nonce with a self-send replacement from the hot "
                f"wallet, then {action}"
            )
        _mined_hash, receipt = self._bridge_receipt(job)
        if receipt is not None:
            raise WarpError(
                f"cannot {action}: this job's bridge mined at nonce {raw_nonce}. "
                "Use Retry -- it picks up the receipt and completes the job"
            )

    def _abandon_job(self, job: WarpJob) -> dict:
        """Close a FAILED job the machine cannot resolve, freeing the slot.

        [WARP-ABANDON 2026-08-08] A job that trips one of the attested-terms
        anchors is a permanent dead end: the anchors run after bridgeToChia
        confirmed and before the ephemeral key is minted, so the row has a
        bridge_nonce and no ephemeral_blob. Sweep raises before touching it,
        Cancel is refused (FAILED is not cancellable once the bridge is on
        chain), and Retry restores the same status to re-fail against the same
        immutable attestation. The GUI gives such a job an empty context menu.
        Since FAILED holds the single active slot, the bridge could then never
        open another job -- with no in-app way out at all.

        No funds are lost in that state: the message stays claimable at the
        warp.green portal, which is why the recovery details are written into
        the audit log before the row is closed rather than discarded with it.

        Only FAILED is accepted. Loosening _sweep_job's FAILED check instead
        would let a sweep flip a COMPLETED job to CANCELLED.
        """
        # A binding-refused job is the other kind the engine cannot resolve, and
        # it has strictly fewer exits than a FAILED one: step() is read-only,
        # Retry and Sweep raise on the binding check, and BRIDGING is not
        # cancellable. Refusing Abandon there too left an upgraded installation
        # with a job holding the single active slot permanently -- recoverable
        # only by hand-editing warp_jobs.db, which is what this whole branch
        # set out to eliminate. Abandon is a pure DB write that records the
        # recovery details first, so it is safe for a job we refuse to touch.
        # Never a closed one, though: a hot-wallet rotation makes every historic
        # row binding-mismatched, and without this a COMPLETED job could be
        # flipped to CANCELLED -- rewriting the record of a finished bridge.
        if job.status in jobs.CLOSED_STATES:
            raise WarpError(f"job {job.id} is already closed ({job.status})")

        mismatch = self._binding_mismatch(job)
        # A status this build has no handler for (version skew: a newer schema
        # ran against this DB) would otherwise park forever -- the dispatcher
        # skips it and every other action refuses it -- so it is abandonable too.
        known_open = job.status == JobStatus.FAILED or job.status in self._HANDLERS
        if job.status != JobStatus.FAILED and not mismatch and known_open:
            raise WarpError(
                f"cannot abandon a job in {job.status}; abandon is the escape "
                "from a job the engine cannot resolve"
            )

        # [WARP-ABANDON-LIVE-TX 2026-08-09] A signed-but-unmined bridge must not
        # be buried. The stuck-tx terminal lands in FAILED with bridge_raw still
        # set and the nonce unconsumed; abandoning that frees the slot, the next
        # bridge reads the same untouched USDC balance and signs a second
        # bridgeToChia -- and if the stuck transaction later mines, real USDC
        # bridges under a nonce no job tracks. Refused until the nonce is
        # provably dead (something else consumed it) or provably resolved (it
        # mined -- in which case Retry, not Abandon, completes the job).
        # Skipped only when the job's nonce belongs to a DIFFERENT hot wallet
        # than the configured key, where the check would interrogate the wrong
        # account; the recovery blob records what to verify by hand instead.
        # A dry_run-legacy row is binding-refused but same-wallet, so the
        # check still runs -- and still protects -- there.
        raw_nonce = job.state.get("bridge_tx_nonce")
        bound = _hx(job.state.get("hot_address") or "")
        same_wallet = not bound or bound == _hx(self._hot_address)
        if not mismatch or same_wallet:
            self._refuse_while_bridge_unresolved(job, "abandon")

        recovery = {
            # Outbound rows: the burn is forward-only and the message is
            # deliverable forever -- record everything a manual relay or a
            # later bundle push needs before the row closes.
            "unwrap": {
                k: job.state.get(k)
                for k in ("burn_address", "burn_cat_ph", "burn_tx_id",
                          "unwrap_nonce", "receiver_evm")
                if job.state.get(k) is not None
            } or None,
            "bridge_nonce": job.bridge_nonce,
            "bridge_tx_hash": job.bridge_tx_hash,
            "bridge_tx_nonce": raw_nonce,
            "signed_raw_present": bool(job.state.get("bridge_raw")),
            "amount_usdc_micros": job.amount_usdc_micros,
            "receiver_ph": job.receiver_ph,
            "failed_from": job.state.get("failed_from"),
            "last_error": job.last_error,
            "portal": self._net.status_url,
        }
        if job.bridge_nonce:
            detail = (
                f"message nonce {job.bridge_nonce} was never claimed and remains "
                f"claimable at {self._net.status_url}"
            )
        elif job.state.get("bridge_raw"):
            if not mismatch or same_wallet:
                # The guard above ran and passed: nonce consumed by something
                # else, and nothing this job signed has a receipt.
                detail = (
                    f"its signed bridge is provably dead (nonce {raw_nonce} was "
                    "consumed by another transaction, with no receipt for "
                    "anything this job signed); the USDC deposit is untouched"
                )
            else:
                # A foreign-wallet job: the guard could not interrogate the
                # right account, so the operator has to.
                detail = (
                    f"a signed bridge transaction may still be live at nonce "
                    f"{raw_nonce} on the job's own hot wallet -- verify on "
                    "BaseScan before treating the deposit as untouched"
                )
        else:
            detail = "no bridge message was ever attested"
        note = f"operator abandoned: {detail}"
        self._store.update_job(
            job.id,
            status=JobStatus.CANCELLED,
            # The job's own status, not FAILED: a binding-refused job is
            # abandonable from whatever state it is frozen in.
            expected_status=job.status,
            columns={"next_retry_at": None},
            state_patch={"abandoned": True, "abandon_recovery": recovery},
            event=("abandon", note, recovery),
        )
        return _job_dict(self._store.get_job(job.id))


    def request_unwrap(
        self, amount_mojos: int, receiver: str, external_relay: bool = False,
        *, automatic: bool = False, rebalance_target_micros: int = 0
    ) -> dict:
        """Operator-triggered unwrap: wUSDC.b -> native USDC at *receiver*.

        Refusals here are synchronous and create no job. Unlike Bridge now,
        the operator states an exact amount and destination -- there is no
        balance-sized default and no clamping: a cap violation refuses rather
        than silently shrinking what was typed.

        ``external_relay`` is the Chia-only mode: skip the hot-wallet gas
        gate and, if the gas really is absent after signature collection,
        wait in ``AWAITING_EXTERNAL_RELAY`` for an altruistic relayer instead
        of self-relaying.
        """
        p = self._params
        active = self._store.get_active_job()
        if active is not None:
            raise WarpError(
                f"a warp job is already active (job {active.id}, {active.status}); "
                "resolve it first -- Retry, Cancel, Sweep or Abandon from the "
                "jobs table"
            )
        receiver = str(receiver).strip()
        if not (receiver.startswith("0x") and len(receiver) == 42):
            raise WarpError("receiver must be a 0x-prefixed 20-byte Base address")
        try:
            rec_bytes = bytes.fromhex(receiver[2:])
        except ValueError as exc:
            raise WarpError("receiver is not valid hex") from exc
        if rec_bytes == b"\x00" * 20:
            raise WarpError("receiver is the zero address")
        hex_part = receiver[2:]
        if hex_part != hex_part.lower() and hex_part != hex_part.upper():
            # Mixed case asserts an EIP-55 checksum; all-lower and all-upper
            # carry no checksum information and are accepted as-is.
            from eth_utils import is_checksum_address

            if not is_checksum_address(receiver):
                raise WarpError(
                    "receiver mixed-case checksum (EIP-55) is invalid -- "
                    "a typo would send the USDC to a stranger"
                )
        amount = int(amount_mojos)
        if amount < 1:
            raise WarpError("unwrap amount must be at least 1 mojo (0.001 USDC)")
        if p.max_unwrap_micros < 0:
            raise WarpError(
                "warp.max_unwrap_usdc is not set; the unwrap cap must be stated "
                "before any unwrap (use 'unlimited' to opt out explicitly)"
            )
        micros = amount * 10 ** (
            self._net.usdc_decimals - self._net.cat_decimals
        )
        if p.max_unwrap_micros and micros > p.max_unwrap_micros:
            raise WarpError(
                f"unwrap of {micros} micro-USDC exceeds warp.max_unwrap_usdc "
                f"({p.max_unwrap_micros}); refusing rather than clamping"
            )
        job = self._store.create_job(
            self._net.name,
            status=JobStatus.UNWRAP_CHECKS,
            columns={"amount_mojos": amount, "amount_usdc_micros": micros},
            state={
                "direction": "out",
                "asset": "USDC",
                "manual": not automatic,
                **({"rebalance": True} if automatic else {}),
                # Lets _h_unwrap_checks re-derive the still-needed amount
                # just before the commit point instead of trusting a
                # possibly-stale snapshot.
                **({"rebalance_target_micros": int(rebalance_target_micros)}
                   if automatic and rebalance_target_micros else {}),
                "receiver_evm": rec_bytes.hex(),
                "external_relay": bool(external_relay),
                **self._binding(),
            },
            event_message=(
                ("rebalance: automatic " if automatic else "")
                + f"unwrap requested: {amount} mojos -> {receiver}"
                + (" (external relay: no Base gas needed)" if external_relay else "")
            ),
        )
        return _job_dict(job)

    def job_action(self, job_id: int, action: str) -> Optional[dict]:
        """Operator affordance: ``retry`` | ``cancel`` | ``sweep`` | ``abandon``."""
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
                reset = self._retry_tx_reset(job)
                self._store.update_job(
                    job_id,
                    status=target,
                    expected_status=JobStatus.FAILED,
                    columns={"retry_count": 0, "last_error": None, "next_retry_at": None},
                    state_patch=reset or None,
                    event=(
                        "retry",
                        f"operator retry -> {target}"
                        + (" (re-signing: the previous nonce is spent)" if reset else ""),
                        None,
                    ),
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
        if action == "abandon":
            return self._abandon_job(job)
        if action == "sweep":
            # The GUI already restricts Sweep to these two (widgets/warp.py),
            # but it acts on a polled snapshot that can be a full interval
            # stale. Sweep spends the job's own security coin, which is the
            # input to the claim the machine may still be building, so a race
            # could pull it out from under an in-flight job. Enforce it here
            # too, where the state is authoritative.
            if job.status not in _SWEEPABLE:
                raise WarpError(
                    f"cannot sweep a job in {job.status}; the security coin is "
                    "still an input to the claim being built"
                )
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
            # Same guard as Abandon: this branch closes the job, and a FAILED
            # BRIDGING row has no ephemeral key either -- concluding "nothing
            # was ever funded" over a signed bridge that can still mine (or
            # already did) frees the slot for a second bridge against the same
            # deposit. Sweep is same-binding here: job_action checks the
            # binding before calling, and the auto-sweep path always has a key.
            self._refuse_while_bridge_unresolved(job, "sweep")
            resolved, status = True, "no ephemeral key: nothing was ever funded"
        coin_id: Optional[str] = None
        if blob:
            sk = self._load_ephemeral_sk(job)
            expected = self._funded_amount(job)
            coin = claim.find_security_coin(self._coinset, sk, expected)
            if coin is None:
                resolved, status = self._funding_provably_gone(job)
            else:
                coin_id = coin.name().hex()
                resolved, status = self._sweep_security(job, coin, sk)

        # Only a resolved sweep frees the slot. A transport failure leaves the
        # job FAILED so the operator can try again with the funds still known
        # recoverable.
        #
        # Outbound rows are NEVER closed by sweep, resolved or not. Their
        # no-key shape ("nothing was ever funded") is a live burn commitment
        # -- a cat_spend in flight or a burn CAT sitting at the burn puzzle
        # hash -- and closing over it records a falsehood while real wUSDC.b
        # is forward-only (reproduced in review). Sweep recovers the toll
        # where one exists; closing an outbound job is Abandon's job, which
        # writes the recovery blob first.
        outbound = job.state.get("direction") == "out"
        close = resolved and job.status == JobStatus.FAILED and not outbound
        if outbound and resolved:
            status = (
                f"{status}; outbound job left open -- Retry resumes it, "
                "Abandon closes it with the recovery details recorded"
            )
        patch: Dict[str, Any] = {"swept": resolved, "sweep_status": status}
        if coin_id and not job.state.get("security_coin_id"):
            # The follow-up Sweep proves the spend from this id. A job that
            # failed out of FUNDING_CLAIM by the pending deadline never had it
            # recorded (only _h_funding_claim's advance writes it), so an
            # unresolved first sweep -- bundle pushed, coin not yet spent --
            # could never be closed by the second: find_security_coin returns
            # None once the sweep mines, and _funding_provably_gone had nothing
            # to check.
            patch["security_coin_id"] = coin_id
        self._store.update_job(
            job.id,
            status=JobStatus.CANCELLED if close else None,
            expected_status=job.status,
            columns={"next_retry_at": None},
            state_patch=patch,
            event=(
                "sweep",
                f"{status}; job closed, active slot freed" if close else status,
                None,
            ),
        )
        return _job_dict(self._store.get_job(job.id))

    def relay_sweep_if_due(self) -> Optional[dict]:
        """One altruistic sweep (plus heartbeat) when enabled and due.

        Never raises: this rides the worker tick behind the operator's own
        job step and must not be able to break it. ``dry_run`` sweeps are
        full rehearsals (fetch, verify, preflight, sign -- no broadcast) and
        deliberately publish NO heartbeat: a rehearsal must not tell
        Chia-only users that help is online.
        """
        if self._relayer is None:
            return None
        now = self._now()
        if now < self._next_relay_sweep:
            return None
        self._next_relay_sweep = now + _RELAY_SWEEP_INTERVAL_S
        # Nonce-collision guard. When the relay shares the engine's hot key
        # (the no-dedicated-key fallback), broadcasting a relay grabs the same
        # pending nonce the engine may hold a signed-but-unbroadcast tx at --
        # the relay mines first and the engine's approve/bridge/relay then
        # silently wedges on a benign 'nonce too low'. A dedicated relay key
        # has its own nonce space and is unaffected; only the shared case is
        # skipped, and only while a job actually occupies the slot.
        if (
            self._relay_key is not None
            and str(self._relay_key.address).lower() == str(self._hot_address).lower()
            and self._store.get_active_job() is not None
        ):
            self._relay_report = {
                "at": now,
                "skipped": "relay shares the hot key with an active bridge job",
            }
            return self._relay_report
        broadcast = not self._params.dry_run
        report: Dict[str, Any] = {"at": now, "broadcast": broadcast}
        try:
            entries = self._relayer.sweep(broadcast=broadcast)
            report["entries"] = entries
            report["relayed"] = sum(
                1 for e in entries if e.get("action") == "relayed"
            )
        except Exception as exc:  # noqa: BLE001 -- must not break the tick
            report["error"] = str(exc)
            _log.warning("altruistic relay sweep failed: %s", exc)
        if broadcast and self._relay_key is not None:
            from . import nostr

            try:
                report["heartbeat_accepted"] = nostr.publish_heartbeat(
                    self._net,
                    self._relay_key.private_key,
                    self._relay_key.address,
                    created_at=now,
                    publisher=self._heartbeat_publisher,
                )
            except Exception as exc:  # noqa: BLE001 -- advertising is best-effort
                report["heartbeat_error"] = str(exc)
        # Persist the budget ledger + skip-list so the daily ceiling and the
        # poison list survive a process restart -- an in-memory-only ledger
        # would grant a fresh full budget on every relaunch.
        if self._relay_state_saver is not None:
            try:
                self._relay_state_saver()
            except Exception as exc:  # noqa: BLE001 -- persistence is best-effort
                _log.warning("relay state save failed: %s", exc)
        self._relay_report = report
        return report

    def refresh_relay_activity_if_due(self) -> Optional[dict]:
        """Refresh the cached liveness evidence for the GUI (never raises).

        Two independent layers, joined by relayer address:

        * on-chain third-party deliveries (unforgeable, but lagging), and
        * verified Nostr heartbeats (fresh, but only a claim).

        A heartbeat whose address also appears in the on-chain evidence is a
        proven volunteer saying it is still running (``online_proven``).
        """
        now = self._now()
        if now < self._next_activity_refresh:
            return None
        self._next_activity_refresh = now + _ACTIVITY_REFRESH_INTERVAL_S
        out: Dict[str, Any] = {"checked_at": now}
        try:
            from . import relayer as relayer_mod

            third = relayer_mod.recent_third_party_relays(
                self._watcher.fetch_path, self._evm,
                portal_address=self._net.portal_address,
                lookback=_ACTIVITY_LOOKBACK,
                # Our own late self-relays (hot key or the dedicated relay
                # key) must never read back as volunteer evidence about us.
                exclude_addresses=[
                    self._hot_address,
                    getattr(self._relay_key, "address", None),
                ],
            )
            out["third_party"] = third[:10]
            out["third_party_count"] = len(third)
            out["last_third_party_at"] = max(
                (t["delivered_at"] for t in third), default=None
            )
            # Join heartbeats against the FULL evidence set, not the truncated
            # display slice -- a volunteer past the 10th recent delivery is
            # still proven, and dropping it would silently downgrade a genuine
            # "online (proven)" to a mere "online (claim)".
            out["_proven_addrs"] = sorted({
                str(t.get("relayer") or "").lower() for t in third
            })
        except Exception as exc:  # noqa: BLE001 -- evidence is best-effort
            out["error"] = str(exc)
        try:
            from . import nostr

            beats = nostr.fetch_recent_heartbeats(
                self._net, now=now, fetcher=self._nostr_fetcher,
            )
            proven = set(out.get("_proven_addrs") or [])
            out["heartbeats"] = beats[:10]
            out["online_now"] = bool(beats)
            # Proven requires BOTH halves verified: the heartbeat must be
            # cryptographically bound to the address it claims (evm_sig
            # recovers to it), and that address must have receipt-verified
            # late deliveries. An unbound heartbeat naming a proven address
            # is exactly the spoof the binding exists to kill.
            out["online_proven"] = any(
                b.get("bound") and b["relayer"] in proven for b in beats
            )
        except Exception as exc:  # noqa: BLE001 -- evidence is best-effort
            out.setdefault("error", str(exc))
            out["online_now"] = False
            out["online_proven"] = False
        out.pop("_proven_addrs", None)  # internal join key; keep it out of the snapshot
        self._relay_activity = out
        return out

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
            snap["millieth_units"] = int(self._evm.get_erc20_balance(
                self._net.milli_eth_address, self._hot_address
            ))
        except Exception as exc:  # noqa: BLE001 -- surface, don't abort the tick
            snap["error"] = str(exc)
        self._hot_cache = snap
        self._receiver_snap = self.effective_receiver()
        return snap

    def close(self) -> None:
        """Release the job store's sqlite connection.

        [WARP-STORE-LEAK 2026-08-08] WarpJobStore.close() had no caller
        anywhere: set_config drops the engine and _build_engine opens a fresh
        store, so every config reload leaked a connection plus its WAL and SHM
        handles against warp_jobs.db. Not a correctness bug -- the partial
        unique index still holds -- but unbounded in a process left running for
        days, and on Windows an open handle is also what turns the documented
        hard-kill relaunch into a file still in use.
        """
        try:
            self._store.close()
        except Exception as exc:  # noqa: BLE001 -- teardown must not raise
            _log.debug("warp: job store close failed: %s", exc)

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
            # The operator's Base gas reserve, in wei, so the GUI warns
            # against the CONFIGURED floor instead of a hardcoded constant
            # that could not be raised for a busier wallet.
            "min_base_eth_wei": int(self._params.min_base_eth * 10 ** 18),
            "altruistic_relay": {
                "enabled": self._relayer is not None,
                "dry_run": self._params.dry_run,
                "relayer_address": getattr(self._relay_key, "address", None),
                "last_sweep": dict(self._relay_report),
            },
            "relay_activity": dict(self._relay_activity),
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

def _job_db_path(
    config: Optional[dict], *, anchor: Optional[Path] = None
) -> str:
    """Resolve the warp jobs DB path (own file; the engine DB stays read-only).

    Relative paths (including the default) anchor to *anchor* -- the config
    home -- when given. Without anchoring they resolve against the process
    CWD, which for an installed build is the Program Files shortcut
    directory: unwritable, so the store could never open. The last member
    of the CWD-relative-default class fixed for installed mode.
    """
    warp = (config or {}).get("warp") or {}
    raw = Path(str(warp.get("jobs_db") or (Path("data") / jobs.DB_FILENAME)))
    if raw.is_absolute() or anchor is None:
        return str(raw)
    return str(Path(anchor) / raw)


def _coinset_url(config: Optional[dict], net: constants.WarpNet) -> str:
    warp = (config or {}).get("warp") or {}
    return (str(warp.get("coinset_url", "") or "").strip()) or net.coinset_url


class _SecretsFileIO:
    """``read()/write()`` over secrets.yaml for :class:`~gui.services.basewallet.BaseWallet`.

    The write path reuses :func:`gui.services.config_split.dump_preserving` --
    the same comment-preserving round-trip the settings page uses -- so the
    operator's own annotations in secrets.yaml survive every key write.
    """

    def __init__(self, path: Path) -> None:
        self._path = Path(path)

    def transaction(self):
        """Hold the shared per-file lock across a read-modify-write.

        The same lock the settings-save path takes, so a BaseWallet key
        mutation and a settings save can never interleave and clobber each
        other (see gui.services.config_split.file_transaction)."""
        from gui.services.config_split import file_transaction

        return file_transaction(self._path)

    def read(self) -> dict:
        import yaml

        if not self._path.is_file():
            return {}
        with open(self._path, "r", encoding="utf-8") as fh:
            data = yaml.safe_load(fh)
        if data is None:
            return {}
        if not isinstance(data, dict):
            # A present-but-corrupt secrets.yaml (a manual edit left it parsing
            # as a list/scalar). Masking it as {} would make create_wallet's
            # overwrite guard read an empty store and rewrite the file from
            # scratch, erasing whatever key blob it actually held. Fail loud;
            # _wallet_summary surfaces this as an error banner, and the write
            # side never runs.
            raise ValueError(
                f"{self._path} does not parse to a mapping "
                f"(got {type(data).__name__}); refusing to treat a corrupt "
                "secrets file as empty"
            )
        return data

    def write(self, data: dict) -> None:
        from gui.services.config_split import dump_preserving

        dump_preserving(self._path, data)


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
    # One-shot recovery material for the modal backup dialog. Deliberately
    # separate from snapshot_ready: snapshots are cached and repeatedly
    # forwarded through the GUI, so secret key bytes must never ride them.
    # object is intentional: PySide carries the same Python bytearray through
    # the queued signal, so the GUI can overwrite that exact object on close.
    key_backup_ready = Signal(str, object, int)

    def __init__(
        self,
        parent: Optional[QObject] = None,
        *,
        secrets_path: Optional[Path] = None,
    ) -> None:
        super().__init__(parent)
        self._config: dict = {}
        self._engine: Optional[WarpEngine] = None
        self._engine_error: Optional[str] = None
        # Base hot-wallet plumbing (independent of the engine -- see
        # wallet_action). Set once before moveToThread, never mutated after.
        self._secrets_path: Optional[Path] = secrets_path
        self._last_wallet_notice: str = ""
        self._rebalance_params = rb.RebalanceParams()
        self._rebalance_error: str = ""
        self._rebalance_next_ok: float = 0.0
        self._rebalance_last: str = ""
        # Monotonic count of PROCESSED wallet actions. Rides every snapshot so
        # the GUI can tell a wallet-action-completion snapshot apart from the
        # timer-driven ticks that carry the same cached value -- without it,
        # the ~5s master tick clears the widget's post-click gate before the
        # action finishes, re-enabling a duplicate irreversible transfer.
        self._wallet_action_seq: int = 0
        # The altruistic relay's daily-gas ledger, failure counts, and
        # skip-list, owned by the worker so they survive an engine rebuild on
        # config reload (the engine, and its relayer, are dropped and rebuilt;
        # this dict is passed by reference into each new relayer).
        self._relay_state: Dict[str, Any] = {}

    @Slot(dict)
    def set_config(self, config: dict) -> None:
        """Adopt a new config snapshot and force a lazy engine rebuild."""
        self._config = dict(config or {})
        self._drop_engine()
        self._engine_error = None
        # Fail-closed rebalance parse: a malformed block disables the
        # rebalancer WITH a banner, never a silently-defaulted band.
        try:
            self._rebalance_params = rb.parse_rebalance_config(
                self._config.get("warp"), min_gas_wei=_MIN_GAS_WEI
            )
            self._rebalance_error = ""
        except rb.RebalanceConfigError as exc:
            self._rebalance_params = rb.RebalanceParams()
            self._rebalance_error = str(exc)
            _log.error("rebalance disabled: %s", exc)

    def _drop_engine(self) -> None:
        """Discard the engine, closing its sqlite connection first."""
        engine, self._engine = self._engine, None
        if engine is not None:
            engine.close()

    def _config_home(self) -> Optional[Path]:
        """The directory the config/secrets live in -- the anchor for
        relative data paths. None only when no secrets path was wired
        (tests), which preserves the old CWD-relative behaviour there."""
        return self._secrets_path.parent if self._secrets_path else None

    # -- altruistic-relay ledger persistence (survives process restart) ----- #

    def _relay_state_path(self) -> Optional[Path]:
        try:
            return Path(_job_db_path(self._config, anchor=self._config_home())).with_name(
                "warp_relay_state.json"
            )
        except Exception:  # noqa: BLE001 -- no path is a safe "don't persist"
            return None

    def _load_relay_state(self) -> None:
        """Populate ``_relay_state`` from disk (types restored: skiplist->set)."""
        path = self._relay_state_path()
        if path is None or not path.is_file():
            return
        import json

        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            self._relay_state = {
                "budget": dict(data.get("budget") or {}),
                "failures": {str(k): int(v)
                             for k, v in (data.get("failures") or {}).items()},
                "skiplist": set(data.get("skiplist") or []),
            }
        except Exception as exc:  # noqa: BLE001 -- a bad ledger must not block start
            _log.warning("could not load relay state from %s: %s", path, exc)

    def _save_relay_state(self) -> None:
        path = self._relay_state_path()
        if path is None:
            return
        import json

        from gui.services.config_split import _atomic_write_text

        payload = {
            "budget": self._relay_state.get("budget") or {},
            "failures": self._relay_state.get("failures") or {},
            "skiplist": sorted(self._relay_state.get("skiplist") or []),
        }
        _atomic_write_text(path, json.dumps(payload), newline="\n")

    @Slot()
    def tick(self) -> None:
        """Advance the active job one step and refresh the hot-wallet balances.

        The altruistic sweep and the activity refresh ride behind the job
        step; both are internally throttled and contracted never to raise.
        """

        def _run(engine: WarpEngine) -> None:
            engine.auto_bridge_suppressed = (
                self._legacy_auto_bridge_suppressed()
            )
            engine.step()
            self._rebalance_tick(engine)
            engine.relay_sweep_if_due()
            engine.refresh_relay_activity_if_due()

        self.snapshot_ready.emit(self._guarded(_run))

    def _legacy_auto_bridge_suppressed(self) -> bool:
        """Whether the engine's own auto-bridge must stand down this tick.

        True whenever the rebalancer is enabled -- it then owns every
        automatic bridge, sized to the USDC band (configure one to keep
        automatic bridging; an ETH-only band means USDC moves only on an
        operator's Bridge now) -- and ALSO while a rebalance config FAILED
        to parse. Falling back to the legacy auto job in either case is a
        fail-open on a money path: the operator asked for band-managed
        bridging, and a typo would instead bridge the whole balance up to
        the cap, untargeted. The Warp banner reports both states, so the
        wallet simply holds until the config is fixed.
        """
        if self._rebalance_error:
            return True
        # ANY enabled band, not just a USDC one. With an ETH-only band the
        # legacy auto-start could still open a USDC job first (step() runs
        # before the consult); if ETH is under the bridge gas floor that
        # job pends forever holding the single slot, and the milliETH
        # refuel that would have fixed the gas can never run -- a deadlock
        # whose only exit is an operator. An enabled rebalancer therefore
        # owns automatic bridging outright.
        return bool(self._rebalance_params.enabled)

    def _wallet_settled(self, engine: "WarpEngine") -> bool:
        """Whether the hot wallet has no broadcast-but-unmined transactions.

        latest before pending, as in :meth:`BaseWallet._require_settled`, so
        a tx landing between the two reads can only widen the gap. Any RPC
        failure reads as UNSETTLED: unknown settlement must not authorise
        an automatic transfer.
        """
        try:
            addr = engine._hot_address
            latest = int(engine._evm.get_nonce(addr, pending=False))
            pending = int(engine._evm.get_nonce(addr, pending=True))
            return pending == latest
        except Exception as exc:  # noqa: BLE001 -- fail closed, never raise
            _log.debug("rebalance: settlement unknown (%s)", exc)
            return False

    def _rebalance_tick(self, engine: "WarpEngine") -> None:
        """Refresh the hot wallet ONCE per tick and consult the planner.

        The settle check must precede the balance read -- a tx mining
        between them would make the nonces agree while the cache was still
        pre-mine -- and the snapshot's refresh is the same one planning
        consumes, so the two nonce reads are the only added RPC, and only
        while the rebalancer is live.
        """
        p = self._rebalance_params
        if not p.enabled or self._rebalance_error:
            engine.refresh_hot_wallet()          # snapshot only
            return
        settled = self._wallet_settled(engine)
        engine.refresh_hot_wallet()
        self._maybe_rebalance(engine, settled=settled)

    def _maybe_rebalance(
        self, engine: "WarpEngine", *, settled: Optional[bool] = None
    ) -> None:
        """One planner consult per tick; at most one action per cooldown.

        Runs only while no warp job is active, never in dry-run, and only
        on settled balances -- ``settled`` comes from :meth:`_rebalance_tick`,
        which checks it BEFORE the balance refresh this reads; passing None
        re-derives it. Contracted never to raise: a failed action is a
        logged banner and a cooldown, not a crashed tick.
        """
        p = self._rebalance_params
        if not p.enabled or self._rebalance_error:
            return
        try:
            ep = engine._params
            if not ep.enabled or ep.dry_run:
                return
            if engine._store.get_active_job() is not None:
                return
            now = time.monotonic()
            if now < self._rebalance_next_ok:
                return
            # Settled balances only: the ETH actuators re-check the nonce
            # gap inside the wallet, but the bridge/unwrap JOB paths do
            # not -- with a transfer pending, the confirmed cache could
            # size a bridge against funds already leaving. Not a failure,
            # so no cooldown: the next tick re-checks.
            if settled is None:
                settled = self._wallet_settled(engine)
            if not settled:
                return
            # The cache was refreshed by _rebalance_tick AFTER the settle
            # check, so it cannot be pre-mine relative to that check.
            hot = getattr(engine, "_hot_cache", None) or {}
            if hot.get("error") or hot.get("eth_wei") is None:
                return
            action = rb.plan(
                params=p,
                eth_wei=int(hot.get("eth_wei") or 0),
                usdc_micros=int(hot.get("usdc_micros") or 0),
                millieth_units=int(hot.get("millieth_units") or 0),
                max_bridge_micros=int(ep.max_micros or 0),
                max_unwrap_micros=int(ep.max_unwrap_micros),
                min_bridge_micros=int(ep.min_micros or 0),
            )
            if action is None:
                return
            # Both USDC actions create a JOB whose gas gates would pend a
            # gasless wallet indefinitely -- squatting the single active
            # slot. Skip (no cooldown: a refuel should act promptly) and
            # say why.
            if action.kind in ("bridge_usdc", "unwrap_usdc") and int(
                hot.get("eth_wei") or 0
            ) < _MIN_GAS_WEI:
                self._rebalance_last = (
                    f"rebalance skipped: {action.kind} needs Base gas but "
                    "ETH is below the relay-gas floor; fund the wallet or "
                    "let an ETH refuel land first"
                )
                return
            reason = action.reason
            mojos = action.amount
            if action.kind == "unwrap_usdc":
                # Preflight spendable wUSDC.b and clamp: an exact-size job
                # bigger than held inventory would go FAILED, and FAILED
                # deliberately retains the single active slot -- an
                # automatic action must never squat it waiting for an
                # operator. Resolved BEFORE the cooldown is armed so a
                # nothing-to-do skip does not delay the next consult.
                wallet_id = engine._resolve_cat_wallet_id()
                bal = engine._wallet.get_wallet_balance(wallet_id)
                spendable = int(bal.get("spendable_balance") or 0)
                mojos = min(action.amount, spendable)
                if mojos < 1:
                    self._rebalance_last = (
                        "rebalance skipped: no spendable wUSDC.b to unwrap"
                    )
                    return
                if mojos < action.amount:
                    # The banner must report what actually moved, not the
                    # planned amount the inventory could not cover.
                    reason = (
                        f"{action.reason} (clamped to {mojos} mojos by "
                        "spendable wUSDC.b inventory)"
                    )
            # Committed to acting: the cooldown starts here, success or
            # not, so a failing actuator cannot be retried every tick
            # against live money. Skips above leave it untouched.
            self._rebalance_next_ok = now + p.cooldown_s
            _log.info("rebalance: %s", reason)
            if action.kind == "bridge_usdc":
                engine.request_bridge(
                    target_micros=action.amount, automatic=True
                )
            elif action.kind == "unwrap_usdc":
                # Receiver is ALWAYS our own hot wallet -- never config.
                engine.request_unwrap(
                    mojos, engine._hot_address, automatic=True,
                    rebalance_target_micros=p.usdc.target if p.usdc else 0,
                )
            elif action.kind == "wrap_eth":
                self._base_wallet().wrap_eth(
                    action.amount, reserve_wei=_MIN_GAS_WEI
                )
            elif action.kind == "unwrap_millieth":
                self._base_wallet().unwrap_millieth(action.amount)
            self._rebalance_last = reason
        except Exception as exc:  # noqa: BLE001 -- banner + cooldown, never a crash
            # Cooldown on ANY failure, including before the plan (e.g.
            # garbage in the hot cache): a broken consult must not retry
            # and log every tick.
            self._rebalance_next_ok = time.monotonic() + p.cooldown_s
            self._rebalance_last = f"rebalance failed: {exc}"
            _log.warning("rebalance action failed: %s", exc)

    @Slot(object)
    def request_bridge(self, target_micros: object) -> None:
        self.snapshot_ready.emit(
            self._guarded(lambda engine: engine.request_bridge(target_micros))
        )

    @Slot(int, str, bool)
    def request_unwrap(
        self, amount_mojos: int, receiver: str, external_relay: bool = False
    ) -> None:
        self.snapshot_ready.emit(
            self._guarded(
                lambda engine: engine.request_unwrap(
                    int(amount_mojos), receiver, bool(external_relay)
                )
            )
        )

    @Slot(int, str)
    def job_action(self, job_id: int, action: str) -> None:
        self.snapshot_ready.emit(
            self._guarded(lambda engine: engine.job_action(job_id, action))
        )

    @Slot(str, dict)
    def wallet_action(self, action: str, payload: dict) -> None:
        """Run one operator Base-wallet command and emit a fresh snapshot.

        Deliberately NOT routed through :meth:`_guarded`: wallet commands must
        work while the engine cannot build -- a missing hot-wallet key is the
        engine's ordinary refusal, and ``create`` is how the operator fixes it.
        """
        payload = dict(payload or {})
        error: Optional[str] = None
        backup: tuple[str, bytearray] | None = None
        try:
            if str(action) == "reveal_backup":
                backup = self._base_wallet().recovery_key()
                notice = "Recovery key opened for offline backup."
            else:
                notice = self._wallet_op(str(action), payload)
            if notice:
                self._last_wallet_notice = notice
        except Exception as exc:  # noqa: BLE001 -- never kill the worker
            error = str(exc)
            _log.warning("wallet action %r failed: %s", action, exc)
        # Mark this action processed BEFORE snapshotting, so the emitted
        # snapshot carries the bumped seq the GUI keys its gate off.
        self._wallet_action_seq += 1
        snap = self._snapshot_or_error()
        if error:
            snap["wallet_action_error"] = error
        self.snapshot_ready.emit(snap)
        if backup is not None and error is None:
            address, recovery = backup
            self.key_backup_ready.emit(
                address, recovery, self._wallet_action_seq
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
        snap: dict
        if self._engine is not None:
            try:
                snap = self._engine.snapshot()
            except Exception as exc:  # noqa: BLE001
                snap = {"enabled": self._config_enabled(), "error": str(exc)}
        else:
            snap = {
                # Report what the operator *asked* for, not what we managed to
                # build. Otherwise a failed anchor renders as "Bridge disabled --
                # set warp.enabled: true", which is both wrong and misleading
                # advice; the documented banner for this is "blocked".
                "enabled": self._config_enabled(),
                "error": self._engine_error or "warp engine not built",
            }
        # Base-wallet fields ride every snapshot, so the wallet page works in
        # all three shapes (engine built, blocked, warp disabled).
        snap["base_wallet"] = self._wallet_summary(snap.get("hot_wallet"))
        snap["wallet_action_seq"] = self._wallet_action_seq
        if self._last_wallet_notice:
            snap["wallet_notice"] = self._last_wallet_notice
        if self._rebalance_params.enabled or self._rebalance_error:
            snap["rebalance"] = {
                "enabled": self._rebalance_params.enabled,
                "error": self._rebalance_error,
                "last": self._rebalance_last,
                # The tab's "automatic bridging is on/off" line would
                # otherwise misdescribe a suppressed legacy auto-bridge.
                "owns_bridging": self._legacy_auto_bridge_suppressed(),
            }
        return snap

    def _config_enabled(self) -> bool:
        """Whether ``warp.enabled`` is set, independent of whether it started."""
        # The raw value, not `or {}`: every non-dict shape (None, [], "")
        # must reach the type guard below rather than be masked into an
        # empty mapping. A malformed warp block reads as disabled, never
        # raises -- this runs inside the snapshot path, and an exception
        # here would swallow the very banner reporting the malformation.
        warp = (self._config or {}).get("warp")
        if not isinstance(warp, dict):
            return False
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

        # Only drivers is used here; the client imports moved with the code
        # that constructs them into _build_engine_with.
        from . import (
            drivers as drivers_mod,
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

        # The store opens a sqlite connection, and everything below it can still
        # raise -- a missing hot-wallet key is the ordinary case. Without this,
        # each failed build leaked a connection, and _ensure_engine is retried
        # on every config reload.
        store = WarpJobStore(_job_db_path(self._config, anchor=self._config_home()))
        try:
            return self._build_engine_with(net, params, store)
        except Exception:
            store.close()
            raise

    def _build_engine_with(
        self, net: Any, params: WarpParams, store: WarpJobStore
    ) -> WarpEngine:
        """Construct the clients and the engine around an open *store*."""
        from . import (
            coinset as coinset_mod,
            evm as evm_mod,
            keystore,
            nostr,
            wallet as wallet_mod,
            watcher as watcher_mod,
        )

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

        # Opt-in altruistic relay: a dedicated gas-only key wins when present
        # (warp.relay_private_key_dpapi), else the hot key pays the gas.
        relayer_obj = None
        relay_key = None
        if params.altruistic_relay:
            from . import relayer as relayer_mod

            # Refresh the ledger from disk before the relayer adopts it by
            # reference, so a restart resumes today's spend and skip-list.
            self._load_relay_state()
            relay_blob = warp_cfg.get("relay_private_key_dpapi")
            relay_key = (
                keystore.load_evm_key(relay_blob, protector=protector)
                if relay_blob else evm_key
            )
            relayer_obj = relayer_mod.AltruisticRelayer(
                net=net,
                evm_client=evm_client,
                collector=nostr.NostrSigCollector(net),
                watcher_fetch=watcher_client.fetch_path,
                evm_key=relay_key,
                grace_s=params.relay_grace_min * 60.0,
                daily_gas_budget_wei=int(
                    params.relay_daily_gas_budget_eth * 10 ** 18
                ),
                # Ledger/skip-list survive engine rebuilds (see _relay_state).
                persistent=self._relay_state,
            )

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
            relayer=relayer_obj,
            relay_key=relay_key,
            relay_state_saver=(
                self._save_relay_state if relayer_obj is not None else None
            ),
        )

    # -- base wallet (operator commands; independent of the engine) --------- #

    def _base_wallet(self):
        """Construct a :class:`~gui.services.basewallet.BaseWallet` over secrets.yaml.

        Built fresh per use (the EVM client is a stateless HTTP wrapper) and
        never through the engine, because the wallet must operate exactly when
        the engine cannot: no key yet, or warp disabled.
        """
        from gui.services.basewallet import BaseWallet
        from . import evm as evm_mod

        if self._secrets_path is None:
            raise WarpError("no secrets.yaml path configured for the wallet")
        net = constants.MAINNET
        params = warp_params_from_config(self._config)
        rpc_url = params.base_rpc_url or net.evm_default_rpc_url
        evm_client = evm_mod.EvmClient(net, rpc_url=rpc_url)
        return BaseWallet(net, evm_client, _SecretsFileIO(self._secrets_path))

    def _wallet_op(self, action: str, payload: dict) -> str:
        """Dispatch one wallet command; returns the operator-facing notice."""
        from gui.services.basewallet import BaseWalletError

        wallet = self._base_wallet()
        if action == "create":
            try:
                address = wallet.create_wallet()
            finally:
                # create writes the key before returning; adopt regardless so
                # a raise after the write still points the engine at it.
                self._adopt_key_from_disk()
            return (
                f"Wallet created: {address}. Use Back up key on the Base "
                "Wallet page before funding it."
            )
        if action == "confirm_backup":
            wallet.mark_backup_confirmed()
            return "Key backup confirmed."
        if action == "send_eth":
            self._refuse_wallet_send_while_job_open("send_eth")
            tx = wallet.send_eth(
                str(payload.get("destination") or ""),
                int(payload.get("amount_wei") or 0),
            )
            return f"ETH transfer broadcast: {tx}"
        if action == "send_usdc":
            self._refuse_wallet_send_while_job_open("send_usdc")
            tx = wallet.send_usdc(
                str(payload.get("destination") or ""),
                int(payload.get("amount_micros") or 0),
            )
            return f"USDC transfer broadcast: {tx}"
        if action == "wrap_eth":
            self._refuse_wallet_send_while_job_open("wrap_eth")
            # Hard floor: never wrap the relay-gas minimum away.  The GUI's
            # min_base_eth banner warns at the operator's comfort line; this
            # gate only protects the protocol floor the engine cannot sign
            # below (_MIN_GAS_WEI), so a small test wallet can still wrap
            # most of its ETH.
            tx = wallet.wrap_eth(
                int(payload.get("amount_wei") or 0),
                reserve_wei=_MIN_GAS_WEI,
            )
            return f"ETH wrapped to milliETH: {tx}"
        if action == "unwrap_millieth":
            self._refuse_wallet_send_while_job_open("unwrap_millieth")
            tx = wallet.unwrap_millieth(int(payload.get("amount_units") or 0))
            return f"milliETH unwrapped to ETH: {tx}"
        if action == "recover_retired":
            out = wallet.recover_retired(str(payload.get("address") or ""))
            txs = ", ".join(out["sweep_txs"])
            return (
                f"Recovered {out['from']} -> {out['to']}: {txs}. "
                f"({out['swept_usdc_micros']} USDC micros, "
                f"{out.get('swept_millieth_units', 0)} milliETH units, "
                f"{out['swept_eth_wei']} wei ETH swept.)"
            )
        if action == "rotate":
            try:
                out = wallet.rotate(open_job_check=self._wallet_open_job)
            finally:
                # rotate persists the NEW key before broadcasting the sweeps;
                # if a sweep broadcast raised, the on-disk key already changed,
                # so the engine must adopt it regardless or it keeps signing
                # with the now-retired key. A pre-persist raise (open-job or
                # gas gate) just re-reads the unchanged blob -- harmless.
                self._adopt_key_from_disk()
            txs = ", ".join(out["sweep_txs"]) or "none (nothing to sweep)"
            return (
                f"Key rotated: {out['old_address']} -> {out['new_address']}. "
                f"Sweep txs: {txs}. The old key is archived in secrets.yaml. "
                "Use Back up key to save and confirm the NEW recovery key."
            )
        raise BaseWalletError(f"unknown wallet action: {action!r}")

    def _refuse_wallet_send_while_job_open(self, action: str) -> None:
        """A manual send shares the hot wallet with the engine's bridge txs.

        A send grabs the pending nonce, which does not see the engine's
        signed-but-unbroadcast approve/bridge/relay raw; broadcasting the send
        first reuses that nonce and wedges the bridge. Refuse while a job holds
        the slot -- the operator resolves it first (Bridge already serialises
        against a single active job for the same reason)."""
        from gui.services.basewallet import BaseWalletError

        open_job = self._wallet_open_job()
        if open_job is not None:
            raise BaseWalletError(
                f"cannot {action}: warp job {getattr(open_job, 'id', '?')} "
                f"({getattr(open_job, 'status', '?')}) is open and shares this "
                "hot wallet; resolve it first so the send cannot race its "
                "pending nonce"
            )

    def _wallet_open_job(self) -> Optional[WarpJob]:
        """The rotation guard's open-job probe -- fail-closed.

        With the engine built, ask its store. Without it a job can still be
        open (warp disabled mid-job), so probe the DB directly; an unreadable
        DB refuses rotation rather than sweeping out from under a frozen job.
        A missing DB file means no job ever ran, which is safely ``None``.
        """
        from gui.services.basewallet import BaseWalletError

        if self._engine is not None:
            return self._engine._store.get_active_job()
        db = Path(_job_db_path(self._config, anchor=self._config_home()))
        if not db.is_file():
            return None
        try:
            store = WarpJobStore(str(db))
        except Exception as exc:  # noqa: BLE001 -- fail closed, with the reason
            raise BaseWalletError(
                f"cannot read the warp job store ({exc}); refusing rotation"
            ) from exc
        try:
            return store.get_active_job()
        finally:
            store.close()

    def _adopt_key_from_disk(self) -> None:
        """Bind the engine to the key now on disk -- drop-first, fail closed.

        The engine is dropped BEFORE anything else: after a key mutation the
        on-disk active key may have changed, and an engine still holding the
        pre-rotation key must not sign another transaction no matter what
        happens next. If the re-read then fails, the stale blob is purged
        from the worker's config and ``_engine_error`` blocks rebuilds --
        rebuilding from the old config would silently resurrect the retired
        key, the exact binding violation the drop exists to prevent.
        """
        self._drop_engine()
        try:
            secrets = _SecretsFileIO(self._secrets_path).read()
            blob = (secrets.get("warp") or {}).get("evm_private_key_dpapi")
        except Exception as exc:  # noqa: BLE001 -- surfaced as a blocked banner
            self._config.setdefault("warp", {}).pop("evm_private_key_dpapi", None)
            self._engine_error = (
                f"secrets.yaml unreadable after a key change ({exc}); the "
                "bridge stays blocked until it can be re-read -- the on-disk "
                "key may differ from the one the engine held"
            )
            _log.warning("could not re-read secrets.yaml after key change: %s", exc)
            return
        warp_cfg = self._config.setdefault("warp", {})
        if blob:
            warp_cfg["evm_private_key_dpapi"] = blob
        else:
            # No key on disk (e.g. the file was replaced): never rebuild with
            # the stale one from the old config snapshot.
            warp_cfg.pop("evm_private_key_dpapi", None)
        self._engine_error = None

    def _wallet_summary(self, hot: Optional[dict]) -> dict:
        """Operator-facing wallet state for the Base Wallet page.

        Balance reads ride the engine's own refresh when it is built (the
        ``hot`` dict); with no engine they are read directly -- two RPCs per
        ~30s tick, and only once a key exists.
        """
        out: Dict[str, Any] = {"configured": False}
        if self._secrets_path is None:
            out["error"] = "no secrets.yaml path configured"
            return out
        try:
            warp_sec = _SecretsFileIO(self._secrets_path).read().get("warp") or {}
        except Exception as exc:  # noqa: BLE001 -- the summary must never raise
            out["error"] = f"secrets.yaml unreadable: {exc}"
            return out
        out["retired_count"] = len(warp_sec.get("retired_keys") or [])
        out["backup_confirmed"] = bool(warp_sec.get("evm_key_backup_confirmed"))
        if not warp_sec.get("evm_private_key_dpapi"):
            return out
        out["configured"] = True
        if hot and hot.get("address") and not hot.get("error"):
            out["address"] = str(hot.get("address"))
            out["eth_wei"] = hot.get("eth_wei")
            out["usdc_micros"] = hot.get("usdc_micros")
            # Pure pass-through: the engine's refresh_hot_wallet reads
            # milliETH alongside ETH/USDC, so the hot path stays exactly
            # what its test pins -- zero wallet builds, zero extra RPCs
            # here. (An older hot dict without the key simply omits it.)
            if "millieth_units" in hot:
                out["millieth_units"] = hot.get("millieth_units")
            return out
        try:
            info = self._base_wallet().info()
        except Exception as exc:  # noqa: BLE001 -- summary must never raise
            out["error"] = str(exc)
            return out
        out["address"] = info.address
        out["eth_wei"] = info.eth_wei
        out["usdc_micros"] = info.usdc_micros
        out["millieth_units"] = info.millieth_units
        return out


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
    key_backup_ready = Signal(str, object, int)

    # -- Internal triggers (queued connections to the worker thread) ------- #
    _trigger_tick = Signal()
    _trigger_params = Signal(dict)
    _trigger_bridge = Signal(object)
    _trigger_unwrap = Signal(int, str, bool)
    _trigger_action = Signal(int, str)
    _trigger_wallet = Signal(str, dict)

    def __init__(
        self,
        config: Optional[dict] = None,
        parent: Optional[QObject] = None,
        *,
        secrets_path: Optional[Path] = None,
    ) -> None:
        super().__init__(parent)
        self._mutex = QMutex()
        self._snapshot: dict = {}
        self._tick_in_flight: bool = False

        self._thread = QThread(self)
        self._thread.setObjectName("WarpWorkerThread")
        self._worker = _WarpWorker(
            secrets_path=Path(secrets_path) if secrets_path else None
        )
        self._worker.moveToThread(self._thread)

        self._worker.snapshot_ready.connect(self._on_snapshot_ready)
        self._worker.key_backup_ready.connect(self._on_key_backup_ready)
        self._trigger_tick.connect(self._worker.tick)
        self._trigger_params.connect(self._worker.set_config)
        self._trigger_bridge.connect(self._worker.request_bridge)
        self._trigger_unwrap.connect(self._worker.request_unwrap)
        self._trigger_action.connect(self._worker.job_action)
        self._trigger_wallet.connect(self._worker.wallet_action)

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
        # Only quitting the thread left warp_jobs.db open. On Windows that is
        # the handle that turns the documented hard-kill relaunch into "the
        # process cannot access the file".
        try:
            self._worker._drop_engine()
        except Exception as exc:  # noqa: BLE001 -- shutdown must not raise
            _log.debug("warp: engine teardown failed: %s", exc)

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

    def request_unwrap(
        self, amount_mojos: int, receiver: str, external_relay: bool = False
    ) -> None:
        if not self._thread.isRunning():
            self.start()
        self._trigger_unwrap.emit(
            int(amount_mojos), str(receiver), bool(external_relay)
        )

    def job_action(self, job_id: int, action: str) -> None:
        if not self._thread.isRunning():
            self.start()
        self._trigger_action.emit(int(job_id), str(action))

    def wallet_action(self, action: str, payload: Optional[dict] = None) -> None:
        """Queue one Base-wallet command onto the worker thread.

        Supported actions include create, reveal/confirm backup, sends, and
        rotation. Recovery key bytes return only through key_backup_ready.
        """
        if not self._thread.isRunning():
            self.start()
        self._trigger_wallet.emit(str(action), dict(payload or {}))

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

    @Slot(str, object, int)
    def _on_key_backup_ready(
        self, address: str, recovery: object, action_seq: int
    ) -> None:
        """Forward one-shot key material, then scrub it as a fallback.

        The live widget is a direct connection on this GUI thread, so its modal
        completes before ``emit`` returns. The ``finally`` also covers a
        disconnected or destroyed widget during shutdown.
        """
        try:
            self.key_backup_ready.emit(str(address), recovery, int(action_seq))
        finally:
            if isinstance(recovery, bytearray):
                recovery[:] = b"\x00" * len(recovery)
