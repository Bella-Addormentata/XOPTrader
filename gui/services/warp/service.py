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
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

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

# [WARP-LOST-SEND 2026-10-25] Automatic Chia fees.  A warp fee key left at 0
# means: at least the engine's offer fee (strategy.offer_fee_mojos), more when
# the node's own estimate for inclusion within _AUTO_FEE_TARGET_S says the
# mempool demands it, never more than _AUTO_FEE_CAP_MULTIPLE times the floor.
# The costs are CLVM cost ceilings for the two spend shapes warp sends (a
# plain XCH send measured 10.57M on 2026-10-24; a CAT spend is roughly three
# times that), so the estimate is for the bundle actually sent.
_AUTO_FEE_TARGET_S: int = 300
_AUTO_FEE_CAP_MULTIPLE: int = 10
_COST_XCH_SEND: int = 12_000_000
_COST_CAT_SPEND: int = 40_000_000

#: A recorded Chia send that is in no wallet and no mempool, with the coins it
#: spent still unspent, MAY still surface from a peer.  It is never sent again
#: on the handler's own authority (a replacement spends other coins, so both
#: could land; a second burn is a real loss): the job waits, fails at its
#: pending deadline, and the operator's Retry is the authority to send again
#: (_retry_tx_reset).  Only a send the chain PROVES dead -- an input spent by
#: another transaction, at confirmation depth, with no output of ours -- is
#: replaced automatically.

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
    # [WARP-LOST-SEND 2026-10-25] strategy.offer_fee_mojos: the floor of
    # every Chia fee warp sets automatically (a fee key left at 0).  A
    # fee-less send sits at the bottom of a full mempool, and anything that
    # sits for ~10 minutes used to meet the engine's force-delete.
    offer_fee_mojos: int = 10_000_000
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
    # USDC to leave on Base whenever the bridge sizes a job itself: every
    # automatic bridge, and a Bridge now with no amount typed. A typed
    # amount is the operator's explicit choice and is not reduced by it.
    # 0 (the default) keeps the old behaviour: bridge the whole balance.
    keep_usdc_micros: int = 0
    # [MULTI-ASSET-OUT] Cap per wmilliETH.b unwrap, in wmilliETH.b MOJOS
    # (1000 per wmilliETH.b = 1 milliETH = 0.001 ETH). Same contract as
    # max_unwrap_micros: -1 == never configured (a wmilliETH.b unwrap is
    # refused until it is stated), 0 == explicit unlimited.
    max_unwrap_millieth_mojos: int = -1


def _c_wei_per_milli_unit(net) -> int:
    """Wei per milliETH BASE unit (= per wmilliETH.b mojo): 1e18 / 10**(3+3).

    One milliETH is 1e15 wei and the token has 3 decimals, so one base unit
    is 1e12 wei -- the MilliETH wrapper's own granularity.
    """
    return 10 ** (18 - 3 - int(getattr(net, "milli_eth_decimals", 3)))


def _millieth_topup_minimums(max_fee_per_gas: int,
                             wei_per_unit: int) -> Tuple[int, int]:
    """[review] The smallest automatic ETH top-up worth making at
    *max_fee_per_gas*: ``(Base milliETH units to withdraw, wmilliETH.b mojos
    to burn)``.

    Each must deliver MORE wei than the gas it costs to turn into ETH, at
    worst-case limits: held milliETH needs one ``withdraw()``; a burn's
    POST-TIP proceeds also pay for the relay, counted as if self-relayed
    (the job may switch to self-relay when gas arrives).
    """
    from gui.services.basewallet import _MILLIETH_GAS_DEFAULT

    from .evm import _RELAY_GAS_DEFAULT

    fee = max(0, int(max_fee_per_gas))
    per = max(1, int(wei_per_unit))
    units = (int(_MILLIETH_GAS_DEFAULT) * fee) // per + 1
    post_tip = ((int(_MILLIETH_GAS_DEFAULT) + int(_RELAY_GAS_DEFAULT))
                * fee) // per + 1
    mojos = -(-post_tip * 10_000 // (10_000 - rb._UNWRAP_TIP_BPS))
    return int(units), int(mojos)


def _unwrap_xch_need(net: Any, fee_mojos: int) -> int:
    """The spendable XCH an unwrap needs at G10: the toll plus the Chia fee
    TWICE (once on the cat_spend, once on the toll funding send)."""
    return int(net.chia_toll_mojos) + 2 * max(0, int(fee_mojos))


_DEFAULT_OFFER_FEE_MOJOS: int = 10_000_000


def _offer_fee_floor_mojos(config: Optional[dict]) -> int:
    """strategy.offer_fee_mojos as the engine reads it (default 10,000,000),
    the floor of warp's automatic Chia fees.  Garbage reads as the default:
    a typo there must not make warp send fee-less.  [review] Nor may a zero
    or negative value: the engine refuses those at startup
    (config.cpp: "offer_fee_mojos must be > 0"), and here a zero floor would
    cap even a positive node estimate at zero."""
    strat = (config or {}).get("strategy") or {}
    raw = strat.get("offer_fee_mojos", _DEFAULT_OFFER_FEE_MOJOS)
    if isinstance(raw, bool):
        return _DEFAULT_OFFER_FEE_MOJOS
    try:
        value = int(raw)
    except (TypeError, ValueError):
        return _DEFAULT_OFFER_FEE_MOJOS
    return value if value > 0 else _DEFAULT_OFFER_FEE_MOJOS


def _top_up_withdraw_short(eth_wei: int, max_fee_per_gas: int) -> bool:
    """Whether *eth_wei* falls short of a milliETH ``withdraw()``'s
    worst-case gas at *max_fee_per_gas* (BaseWallet.unwrap_millieth's bound)."""
    from gui.services.basewallet import _MILLIETH_GAS_DEFAULT

    return int(eth_wei) < int(_MILLIETH_GAS_DEFAULT) * max(0, int(max_fee_per_gas))


def _parse_millieth_unwrap_cap(warp: dict) -> int:
    """warp.max_unwrap_millieth (wmilliETH.b units) -> mojos; -1 if never set.

    The same fail-closed shape as :func:`_parse_unwrap_cap`, in the asset's
    own units: a USDC-denominated cap applied to milliETH base units would
    read 1000x too permissive, which is why the two are separate keys.
    """
    raw = warp.get("max_unwrap_millieth", None)
    if raw is None or str(raw).strip() == "":
        return -1
    if str(raw).strip().lower() == "unlimited":
        return 0
    value = _parse_usdc_config_value("max_unwrap_millieth", raw)
    # FLOORED to whole mojos, from the exact text: a cap must never round UP
    # into a burn larger than the operator stated (2.0005 -> 2,000 mojos, and
    # 0.0006 is below one mojo, so it is refused, not read as 0.001).
    from decimal import ROUND_FLOOR, Decimal, InvalidOperation

    try:
        mojos = int((Decimal(str(raw).strip()) * 1000).to_integral_value(
            rounding=ROUND_FLOOR))
    except (InvalidOperation, ValueError):
        mojos = int(value * 1000)
    if value <= 0 or mojos <= 0:
        raise WarpError(
            f"warp.max_unwrap_millieth must be positive (got {raw!r}); use "
            "'unlimited' if no cap is genuinely intended"
        )
    return mojos


#: The smallest inbound bridge, in wrapped-CAT mojos. ERC20Bridge's tip is at
#: least one mojo and the contract requires amount > tip
#: (evm.post_tip_amount), so one mojo can never bridge.
_MIN_BRIDGE_MOJOS: int = 2


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

    # [review] A YAML boolean is not a number: float(True) is 1.0, which
    # would read `max_unwrap_millieth: true` as a stated 1,000-mojo cap.
    if isinstance(raw, bool):
        raise WarpError(f"warp.{key} is not a number (got {raw!r})")
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
        offer_fee_mojos=_offer_fee_floor_mojos(config),
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
        keep_usdc_micros=int(round(_parse_non_negative_float(
            "keep_usdc_on_base", warp.get("keep_usdc_on_base"), 0.0
        ) * scale)),
        max_unwrap_millieth_mojos=_parse_millieth_unwrap_cap(warp),
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


def _canon_addr(address: str) -> str:
    """An ERC-20 address in one canonical form: bare, lower-case hex.

    This codebase accepts both ``0x``-prefixed and bare addresses as the
    same address, so a fingerprint must not encode the difference -- a
    formatting-only edit to a descriptor would otherwise read as changed
    terms and terminally strand a live job.
    """
    a = str(address or "").strip().lower()
    return a[2:] if a.startswith("0x") else a


def _asset_fingerprint(spec, cat_decimals: int) -> str:
    """The asset TERMS a job is frozen against.

    Contract, BOTH precisions (every conversion is
    ``10 ** (erc20 - cat)``, so freezing one half would let the ratio move)
    and the wrapped TAIL.

    Version-tagged so the format can change. An unrecognised version is
    NOT treated as unfingerprinted: :meth:`_job_asset` routes it through
    attestation recovery and stops the job when no attested evidence
    exists. Only a row with neither field takes the legacy-USDC path, so
    a future migration must convert existing stamps rather than assume
    old readers will fall back gracefully.
    """
    return (f"v1:{_canon_addr(spec.erc20_address)}:{int(spec.erc20_decimals)}"
            f":{int(cat_decimals)}:{spec.expected_asset_id.strip().lower()}")


def _asset_stamp(spec, cat_decimals: int) -> dict:
    """The asset fields every new job records: the name AND the terms.

    The name is advisory (it makes rows readable); the fingerprint is the
    identity :meth:`_job_asset` actually resolves against.
    """
    return {"asset": spec.symbol,
            "asset_fingerprint": _asset_fingerprint(spec, cat_decimals)}


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
        # [MILLIETH-TOPUP] The asset's NAME, for display only (identity is
        # the fingerprint): a milliETH job's amount_usdc_micros column
        # holds milliETH base units, not micro-USDC.
        "asset": str(job.state.get("asset") or "USDC"),
        "amount_mojos": job.amount_mojos,
        "post_tip_mojos": job.post_tip_mojos,
        "receiver_address": job.receiver_address,
        "bridge_tx_hash": job.bridge_tx_hash,
        "bridge_nonce": job.bridge_nonce,
        "retry_count": job.retry_count,
        "last_error": job.last_error,
        "next_retry_at": job.next_retry_at,
        # [review] What Sweep found and whether every Chia send the job made
        # is ruled out: a Sweep that recovered the coin but could not close
        # the job was otherwise invisible to the operator.
        "swept": job.state.get("swept"),
        "sweep_status": job.state.get("sweep_status"),
        "attempts_open": _open_attempts(job.state),
        "attempts_presumed": _presumed_attempts(job.state),
    }


def _settled_ids(state: dict, tx_key: str) -> set:
    """Retired attempts that no longer keep a job open: proven dead, or
    presumed gone after the grace (kept apart: a presumed-gone send's coins
    are still unspent, so Sweep stays available for it)."""
    return ({t for t in (state.get(f"{tx_key}_prior_dead") or []) if t}
            | {t for t in (state.get(f"{tx_key}_prior_presumed") or []) if t})


def _open_attempts(state: dict) -> int:
    """How many replaced Chia sends of a job are not yet proven dead or
    presumed gone (the ones that keep Sweep / completion waiting)."""
    n = 0
    for tx_key in ("funding_tx_id", "burn_tx_id"):
        done = _settled_ids(state, tx_key)
        n += sum(1 for t in (state.get(f"{tx_key}_prior") or []) if t and t not in done)
    return n


def _presumed_attempts(state: dict) -> int:
    """How many replaced Chia sends were presumed gone with their coins
    still unspent -- the reason Sweep stays offered on a closed job."""
    return sum(len([t for t in (state.get(f"{k}_prior_presumed") or []) if t])
               for k in ("funding_tx_id", "burn_tx_id"))


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
        if job.status == JobStatus.CLAIMING and self._claim_completed(job):
            # [review] The claim DID land; the wait is for a replaced funding
            # send to be ruled out.  "Sweep recovers the funding coin" would
            # misread: that coin is spent.
            return (
                f"the claim completed, but a replaced funding send is not ruled "
                f"out after {polls} polls (~{hours:.1f}h); handing over to the "
                "operator -- Sweep recovers a coin that landed, Retry resumes "
                "the wait, Abandon closes the job"
            )
        if job.state.get("delivered_by"):
            # [WARP-LOST-SEND] The unwrap WAS delivered; the wait is for a
            # replaced toll send to be ruled out before the job may close
            # (_unwrap_delivered).  The generic remedy would misread as an
            # undelivered relay.
            return (
                f"the unwrap was delivered, but a replaced toll send is not "
                f"ruled out after {polls} polls (~{hours:.1f}h); handing over "
                "to the operator -- Sweep recovers a toll coin that landed, "
                "Retry resumes the wait, Abandon closes the job"
            )
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
        # Since warpdotgreen/cli#26 a signature quorum that never forms is not
        # only slowness: validators stop signing on a Nostr control "stop"
        # (and by default until a "start"), and permanently refuse a message
        # their spend policy rejects. Neither is visible from here -- the stop
        # authors are each operator's own config -- so name both causes.
        cause = (
            " -- the validators may be paused, or may have rejected this message"
            if job.status in (JobStatus.COLLECTING_SIGS, JobStatus.COLLECTING_EVM_SIGS)
            else ""
        )
        return (
            f"no progress in {job.status} after {polls} polls (~{hours:.1f}h){cause}; "
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

    def _job_asset(self, job: WarpJob, attested=None):
        """The bridgeable asset *job* was created for, from its frozen state.

        Resolution is by the wrapped-CAT id recorded in the job's
        fingerprint -- the asset's true identity, derived from its token
        contract, and stable across a rename of the table key or symbol.
        The persisted symbol is advisory: it makes rows readable and is
        never used to resolve one. A row carrying a name WITHOUT usable
        terms is a partial write, and is recovered only from attested
        contents (or stopped); a row carrying neither field predates
        stamping, when USDC was the only asset the pipeline could bridge,
        and resolves to USDC directly.

        It refuses rather than guessing when the job's asset is no longer
        pinned by this deployment, when its terms no longer match, and
        when a name was recorded without terms and no attested contents
        exist to recover from. Those are "stop and fetch an operator"
        conditions: a wrong guess here moves the wrong funds.

        Note the corollary of identity-by-wrapped-id: a row whose NAME
        disagrees with its fingerprint resolves to the fingerprint's
        asset. The name is a label, not a claim about identity.
        """
        raw = str(job.state.get("asset_fingerprint") or "").strip()
        stamped = "asset" in job.state

        if raw:
            parts = raw.split(":")
            if parts[0] == "v1" and len(parts) == 5:
                _v, addr, erc20_dec, cat_dec, tail = parts
                try:
                    erc20_dec_i, cat_dec_i = int(erc20_dec), int(cat_dec)
                except ValueError:
                    # Never leak a bare exception: step() classifies unknown
                    # errors as RETRYABLE, so a corrupt stamp would retry
                    # every tick forever.
                    return self._asset_from_attestation(
                        job, f"non-integer precision in {raw!r}", attested)
                try:
                    spec = self._net.asset_by_wrapped_id(tail)
                except KeyError:
                    raise WarpTerminal(
                        f"job {job.id} was frozen against wrapped asset "
                        f"{tail}, which {self._net.name} no longer pins -- "
                        "refusing to resume against a different asset than "
                        "the one its funds moved under"
                    ) from None
                if (_canon_addr(spec.erc20_address) != _canon_addr(addr)
                        or int(spec.erc20_decimals) != erc20_dec_i
                        or int(self._net.cat_decimals) != cat_dec_i):
                    raise WarpTerminal(
                        f"job {job.id} froze {spec.symbol} as {raw}, but this "
                        "build resolves that wrapped id to "
                        f"{_asset_fingerprint(spec, self._net.cat_decimals)}"
                        " -- the asset's terms changed under a live job"
                    )
                return spec
            return self._asset_from_attestation(
                job, f"unrecognised stamp format {raw!r}", attested)

        if stamped:
            # Every writer records the terms with the name, so a name alone
            # is a partial write, not an old row. Resolving it by name would
            # let a job adopt edited contract/precision/TAIL terms -- and for
            # a committed job could sign a replacement for a DIFFERENT token
            # at the original nonce.
            return self._asset_from_attestation(
                job, "asset recorded without its terms", attested)

        # Neither field: the row predates asset stamping, and USDC was the
        # only asset the pipeline could bridge when it was written. Resolve
        # it through the network's legacy wrapped-id anchor rather than the
        # literal name, so legacy rows get the same rename stability as
        # stamped ones -- a renamed key would otherwise raise KeyError,
        # which step() classifies as retryable and would spin forever.
        try:
            return self._net.asset_by_wrapped_id(self._net.expected_asset_id)
        except KeyError:
            raise WarpTerminal(
                f"job {job.id} predates asset stamping, but "
                f"{self._net.name} no longer pins its legacy wrapped asset "
                f"{self._net.expected_asset_id}"
            ) from None

    def _asset_from_attestation(self, job: WarpJob, why: str, attested=None):
        """Last resort for a job whose stamp cannot be read.

        Recovers the asset from the ATTESTED message contents when the job
        has them -- ``contents[0]`` is the ERC-20 the validators witnessed,
        which is durable on-chain evidence rather than a mutable local
        name. Without that evidence there is nothing trustworthy left to
        resolve from, so the job stops for an operator instead of guessing:
        a wrong guess here moves the wrong funds, which is worse than a
        job that needs manual recovery.
        """
        tokens = [_hx(item)[-40:]
                  for item in (job.state.get("message_contents") or [])[:1]]
        if attested is not None:
            # MESSAGE_SENT resolves the asset BEFORE the attestation is
            # persisted, so the live message is the only evidence available
            # at the one point a post-bridge job would otherwise strand.
            live = _hx(getattr(attested, "erc20_source", "") or "")[-40:]
            contents = list(getattr(attested, "contents", None) or [])
            first = _hx(contents[0])[-40:] if contents else ""
            if live and first and live != first:
                raise WarpTerminal(
                    f"job {job.id} cannot be recovered: the attestation is "
                    f"self-inconsistent (source {live} != contents {first})"
                )
            if live:
                tokens.append(live)
        for token in tokens:
            for _key, spec in (self._net.assets or ()):
                if _canon_addr(spec.erc20_address) == token:
                    # Honest about what this costs: for a recovered job the
                    # attested-source anchor below compares the attestation
                    # against an asset derived FROM that attestation, so it
                    # can no longer catch a wrong-asset bridge. The evidence
                    # is validator-signed, which is why this beats both
                    # guessing by name and stranding the funds.
                    _log.error(
                        "job %s has an unreadable asset stamp (%s); "
                        "recovered %s from attested evidence -- its "
                        "source anchor is self-referential from here",
                        job.id, why, spec.symbol,
                    )
                    return spec
        raise WarpTerminal(
            f"job {job.id} has an unreadable asset stamp ({why}) and no "
            "attested contents to recover its asset from; refusing to guess "
            "-- resolve it manually before any further bridging"
        )

    def _h_awaiting_deposit(self, job: WarpJob) -> _Step:
        """Watch the hot wallet for a deposit of the JOB's asset; freeze
        amounts on arrival."""
        from . import evm

        net, p = self._net, self._params
        spec = self._job_asset(job)
        # min_micros/max_micros are scaled by USDC's decimals
        # (warp_params_from_config), but the balance below is in THIS
        # asset's base units -- for a 3-decimal token the blast-radius cap
        # would read 1000x too permissive. Per-asset caps land in B2; until
        # then a non-USDC job is refused rather than run uncapped.
        # Identity, not name: the symbol is advisory, so re-casing the USDC
        # descriptor would strand a live job here, and naming some other
        # descriptor "USDC" would walk straight through the gate.
        if spec.expected_asset_id.lower() != (net.expected_asset_id or "").lower():
            raise WarpTerminal(
                f"{spec.symbol} bridging is not enabled yet: warp's amount "
                "caps are still denominated in USDC, and applying them to "
                f"{spec.symbol} base units would mis-scale the blast radius"
            )

        have = self._evm.get_erc20_balance(spec.erc20_address, self._hot_address)
        target = job.state.get("target_micros")
        exact = bool(job.state.get("exact")) and bool(target)
        if exact:
            # A typed amount is bridged as typed: wait for the balance to
            # cover it rather than bridge whatever happens to be there --
            # and never clamp it to a cap lowered while it waited.
            if p.max_micros > 0 and int(target) > p.max_micros:
                raise WarpPending(
                    f"the requested {int(target)} micros is above the current "
                    f"per-warp cap ({p.max_micros}); raise "
                    "warp.max_auto_bridge_usdc or cancel this job"
                )
            if have < int(target):
                raise WarpPending(
                    f"awaiting a {spec.symbol} balance of {int(target)} micros "
                    f"for the requested amount (have {have})"
                )
            want = int(target)
            keep = 0
        else:
            # Everything the bridge sizes itself leaves warp.keep_usdc_on_base
            # behind (automatic jobs, rebalance jobs, a blank Bridge now).
            keep = max(0, int(getattr(p, "keep_usdc_micros", 0) or 0))
            avail = max(0, have - keep)
            want = min(int(target), avail) if target else avail

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
        # A REBALANCE job whose bridgeable amount fell below what it may move
        # (the keep floor, or a balance that dropped while it waited) is
        # closed, not left pending: a pending job holds the single active
        # slot and would block every gas top-up behind it.
        if job.state.get("rebalance") and want < max(
                p.min_micros,
                _MIN_BRIDGE_MOJOS * 10 ** (net.usdc_decimals - net.cat_decimals)):
            return _advance(
                JobStatus.CANCELLED,
                message=(
                    f"rebalance bridge no longer needed: {want} micros "
                    f"bridgeable (balance {have}, {keep} kept on Base) is "
                    "below the minimum"
                ),
            )
        mojo_micros = 10 ** (net.usdc_decimals - net.cat_decimals)
        if 0 < want < _MIN_BRIDGE_MOJOS * mojo_micros:
            raise WarpPending(
                f"{want} micros bridgeable is below the {_MIN_BRIDGE_MOJOS}-mojo "
                "minimum the bridge's tip leaves room for (have "
                f"{have}, {keep} kept on Base)"
            )
        if want <= 0:
            if keep > 0 and have > 0:
                raise WarpPending(
                    f"awaiting a {spec.symbol} deposit above the {keep} micros "
                    f"kept on Base (have {have})")
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
            state={"tip_bps": tip_bps, **_asset_stamp(spec, self._net.cat_decimals),
                   **self._binding()},
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
            gas_defaulted = False
            try:
                unsigned = self._evm.prepare_bridge(
                    owner=self._hot_address,
                    receiver_ph=bytes.fromhex(job.receiver_ph),
                    mojo_amount=int(job.amount_mojos),
                    asset=spec,
                )
            except Exception as exc:
                # [WARP-DRYRUN-ALLOWANCE 2026-08-22] A rehearsal signs the
                # approve but deliberately never broadcasts it, so the chain's
                # allowance is still zero and the bridge gas estimate ALWAYS
                # reverts at the token's allowance guard on a fresh wallet.
                # Observed on the first real dry run (job 1): the
                # [WARP-ESTIMATE-REVERT] re-raise left the job retrying
                # forever, so DRY_RUN_OK was unreachable on exactly the wallet
                # the rehearsal exists to validate.  That revert is the
                # rehearsal's PROOF -- RPC round-tripped, calldata decoded,
                # contract executed to the transferFrom guard -- so sign at
                # the default gas instead; the transaction is never broadcast
                # in a rehearsal.  Any OTHER revert still fails, exactly as
                # live.
                if not (self._job_dry_run(job)
                        and evm.is_allowance_revert(exc)):
                    raise
                gas_defaulted = True
                unsigned = self._evm.prepare_bridge(
                    owner=self._hot_address,
                    receiver_ph=bytes.fromhex(job.receiver_ph),
                    mojo_amount=int(job.amount_mojos),
                    asset=spec,
                    gas=evm.BRIDGE_GAS_DEFAULT,
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
                    # Recorded so DRY_RUN_OK can report the estimate was
                    # substituted, keeping the rehearsal summary honest.
                    "bridge_gas_defaulted": gas_defaulted or None,
                },
                message="bridge signed; broadcasting next tick",
            )

        # Rehearsal stop -- the last instant before anything becomes irreversible.
        # Everything up to here has been exercised for real: the DPAPI key, the
        # Base RPC, the wallet daemon, the receiver decode, the live message toll
        # and tip, gas estimation, nonce selection, ABI encoding and signing. The
        # only thing that does not happen is the broadcast.
        if self._job_dry_run(job):
            gas_note = (
                " (bridge gas defaulted: the estimate needs the approve's "
                "allowance on-chain, which a rehearsal never broadcasts)"
                if job.state.get("bridge_gas_defaulted") else ""
            )
            return _advance(
                JobStatus.DRY_RUN_OK,
                columns={"bridge_tx_hash": None},
                state={"bridge_raw": None, "dry_run": True},
                message=(
                    "dry run OK: both Base transactions signed but NOT broadcast; "
                    "no funds moved. Set warp.dry_run: false to go live"
                    + gas_note
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
        spec = self._job_asset(job, attested=msg)
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
        # Resolve the asset BEFORE any XCH leaves the wallet. The claim
        # anchors on it later, but funding runs first: without this, a job
        # whose asset was removed from the deployment (or whose frozen
        # precision changed) would spend the operator's security coin and
        # only then fail unresolvable, committing funds to a claim that can
        # never be built. Fail-closed means failing before the send.
        self._job_asset(job)
        sk = self._load_ephemeral_sk(job)
        expected = self._funded_amount(job)

        coin = claim.find_security_coin(self._coinset, sk, expected)
        if coin is not None:
            return _advance(
                JobStatus.CLAIM_FUNDED,
                state={"security_coin_id": coin.name().hex()},
                message=f"security coin funded ({expected} mojos)",
            )

        security_ph = claim.security_coin_puzzle_hash(sk)

        # Cheapest guard first: we already recorded a funding send for this job.
        # The coin is not on chain yet (checked above).  [WARP-LOST-SEND] The
        # send is JUDGED, not assumed in flight: the wallet may have deleted
        # it, or a coin it spent may have gone to another transaction (job 4,
        # 2026-10-24), in which case it is sent again below.
        resend: Dict[str, Any] = {}
        if job.state.get("funding_tx_id"):
            step, resend = self._judge_recorded_send(
                job, tx_key="funding_tx_id", label="funding tx",
                out_ph_hex=security_ph.hex(), out_amount=expected,
                unique_output=True,
                scan=lambda dead, **kw: self._find_existing_funding(
                    security_ph.hex(), expected, exclude_ids=dead,
                    **{**self._history_filters(job, "funding_tx_id"), **kw}),
            )
            if step is not None:
                return step

        priors = [t for t in (resend.get("funding_tx_id_prior")
                              or job.state.get("funding_tx_id_prior") or []) if t]
        existing = self._find_existing_funding(
            security_ph.hex(), expected, exclude_ids=priors,
            **self._history_filters(job, "funding_tx_id", pending=resend))
        if existing:
            return _stay(
                # funding_amount travels with the id here too: a job that
                # resumes through this branch would otherwise leave every
                # later lookup recomputing the amount from live config.
                state={**resend, "funding_tx_id": existing,
                       "funding_amount": expected},
                message="funding tx already in flight (dedupe hit)",
            )

        # Re-check ON THE TICK THAT SENDS: the wrap gates ran long ago and
        # the claim amount (post_tip + claim fee) comes out of spendable XCH.
        fee = self._chia_send_fee(p.chia_funding_fee_mojos, cost=_COST_XCH_SEND)
        self._require_wallet_synced("claim security-coin funding")
        self._require_spendable_xch(
            expected + fee,
            context="claim security-coin funding",
        )
        self._wallet.log_in()
        address = cu.encode_puzzle_hash(security_ph, net.chia_prefix)
        resend = {**resend, **self._refresh_exclusions(
            job, tx_key="funding_tx_id", out_ph_hex=security_ph.hex(),
            out_amount=expected, pending=resend)}
        exclude = [c for c in ((resend if "funding_tx_id_exclude" in resend
                               else job.state).get("funding_tx_id_exclude") or [])
                   if c]
        record = self._send_reserved(
            job, tx_key="funding_tx_id",
            # funding_amount travels with the reservation: a timed-out RPC
            # must leave the amount Sweep looks for, not today's config.
            reserve_patch={**resend, "funding_amount": expected},
            send=lambda: self._wallet.send_transaction(
                1, expected, address, fee_mojos=fee,
                **({"exclude_coin_ids": exclude} if exclude else {}),
            ),
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
        from . import chia_send as cs

        return _stay(
            # Record what was actually sent. A later Sweep must look for *this*
            # amount, not whatever claim_fee_mojos happens to say at the time.
            # The coins it spent let a later tick prove the send dead.
            state={**resend, "funding_tx_id": tx_id, "funding_amount": expected,
                   "funding_tx_id_removals": list(cs.removal_ids(record)) or None,
                   "funding_tx_id_orphan_polls": 0, "funding_fee_mojos": fee,
                   "funding_tx_id_exclude": None, "funding_tx_id_wallet_absent": None},
            message=f"funded {expected} mojos to the security coin (fee {fee})",
        )

    def _security_coin_for(self, job: WarpJob, sk: bytes, expected: int):
        """The job's security coin: the one ``security_coin_id`` pins while it
        is unspent, else the key's first unspent coin of the funded amount.

        [review] Two coins at the key is now a designed state (a replaced
        funding send landing beside its replacement), and the coinset's
        listing order is not stable: without the pin, consecutive ticks
        could build the claim around different coins -- a second bundle
        conflicting on the message coin, our own claim later read as a
        third party's.  A pinned coin not indexed yet falls back to the
        scan (it may simply predate the index); a spent one returns None,
        which the callers read as "awaiting the final CAT coin".
        """
        from . import claim, drivers

        pinned = _hx(job.state.get("security_coin_id") or "")
        if pinned:
            try:
                rec = self._coinset.get_coin_record_by_name(pinned)
            except Exception:  # noqa: BLE001 -- the scan below decides
                rec = None
            if rec is not None:
                # The record must BE the pinned coin (a by-name lookup that
                # answered with something else is not evidence about it).
                from . import clvm_utils as cu

                try:
                    same = cu.coin_name(
                        bytes.fromhex(_hx(rec.coin.parent_coin_info)),
                        bytes.fromhex(_hx(rec.coin.puzzle_hash)),
                        int(rec.coin.amount)).hex() == pinned
                except Exception:  # noqa: BLE001 -- malformed: not evidence
                    same = False
                if not same:
                    rec = None
            if rec is None:
                # [review] Not indexed by name yet, or the lookup failed: the
                # SAME coin by its computed name among the key's records --
                # never another coin of the amount, which would rebuild the
                # claim around a different security coin and overwrite
                # final_cat_coin_id while the first claim may still land.
                try:
                    from . import clvm_utils as cu

                    ph = claim.security_coin_puzzle_hash(sk)
                    for cand in self._coinset.get_coin_records_by_puzzle_hash(
                            _hx(ph), include_spent=True):
                        if cu.coin_name(
                                bytes.fromhex(_hx(cand.coin.parent_coin_info)),
                                ph, int(cand.coin.amount)).hex() == pinned:
                            rec = cand
                            break
                except Exception:  # noqa: BLE001 -- then it is awaited
                    rec = None
            if rec is None:
                # The key scan too, accepted only when it names the pinned
                # coin itself: any other coin of the amount is awaited past.
                try:
                    cand = claim.find_security_coin(self._coinset, sk, expected)
                except Exception:  # noqa: BLE001
                    cand = None
                if cand is not None and _hx(cand.name().hex()) == pinned:
                    return cand
                return None
            if rec.spent:
                return None
            return drivers.Coin(
                bytes.fromhex(_hx(rec.coin.parent_coin_info)),
                bytes.fromhex(_hx(rec.coin.puzzle_hash)),
                int(rec.coin.amount),
            )
        return claim.find_security_coin(self._coinset, sk, expected)

    def _h_claim_funded(self, job: WarpJob) -> _Step:
        """Wait for the security coin to reach the Chia confirmation depth."""
        net = self._net
        sk = self._load_ephemeral_sk(job)
        expected = self._funded_amount(job)

        coin = self._security_coin_for(job, sk, expected)
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
            # [WARP-LOST-SEND] COMPLETED frees the slot and offers no Sweep,
            # so a replaced funding send that could still land -- or already
            # did, leaving a second coin at the key -- keeps the job here: a
            # landed one is swept now, an undecided one waited out.
            settled, note = self._prior_sends_settled(job)
            if not settled:
                extra = claim.find_security_coin(self._coinset, sk, expected)
                if extra is not None:
                    _resolved, status = self._sweep_security(job, extra, sk)
                    return _stay(
                        state={"sweep_status": status},
                        message=(f"claim confirmed at {depth} confirmations; a "
                                 f"replaced funding send landed too -- {status}"),
                    )
                return _stay(
                    message=f"claim confirmed at {depth} confirmations; {note}",
                )
            return _advance(
                JobStatus.COMPLETED,
                message=f"claim confirmed at {depth} confirmations",
            )

        # [WARP-LOST-SEND] A third-party claim already paid the receiver and
        # our funding was swept (persisted above): only the replaced funding
        # sends remain to be ruled out.  A landed one is swept here; once all
        # are settled the job completes.
        if job.state.get("third_party_claim"):
            if not job.state.get("swept"):
                # [review] The recovery sweep was pushed but not confirmed when
                # that tick ended (swept False).  Once the security coin is
                # spent it is swept; until then the same bundle is pushed
                # again.  Falling through to the claim wait instead would have
                # the job await its own final CAT coin, which a third-party
                # claim means can never appear.
                sec_id = job.state.get("security_coin_id")
                spent = bool(sec_id) and claim.security_coin_spent(
                    self._coinset, bytes.fromhex(sec_id))
                if spent:
                    self._store.update_job(job.id, expected_status=job.status,
                                           state_patch={"swept": True})
                else:
                    coin = self._security_coin_for(job, sk, expected)
                    if coin is not None:
                        resolved, status = self._sweep_security(job, coin, sk)
                        return _stay(
                            state={"swept": resolved, "sweep_status": status},
                            message=(f"third-party claim paid the receiver; {status}; "
                                     + ("funding swept back" if resolved else "retrying")),
                        )
                    return _stay(message="third-party claim paid the receiver; awaiting "
                                         "the sweep of our funding coin")
            settled, note = self._prior_sends_settled(job)
            if settled:
                return _advance(
                    JobStatus.COMPLETED,
                    message="third-party claim paid the receiver; funding swept back",
                )
            extra = claim.find_security_coin(self._coinset, sk, expected)
            if extra is not None:
                _resolved, status = self._sweep_security(job, extra, sk)
                return _stay(state={"sweep_status": status},
                             message=f"a replaced funding send landed -- {status}")
            return _stay(message=f"third-party claim paid the receiver; {note}")

        coin = self._security_coin_for(job, sk, expected)
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
            if resolved:
                # [WARP-LOST-SEND] ...and no replaced funding send of this job
                # could still land: COMPLETED would leave its XCH no Sweep.
                # The resolved sweep is PERSISTED FIRST ([review]: the
                # settlement check below can raise -- a wallet login, an
                # output lookup -- and later ticks, which find no unspent
                # security coin, must resume from the checkpoint rather than
                # pend on a coin that is gone).
                self._store.update_job(
                    job.id, expected_status=job.status,
                    state_patch={"third_party_claim": True, "swept": True,
                                 "sweep_status": status})
                settled, note = self._prior_sends_settled(job)
                if not settled:
                    return _stay(
                        state={"third_party_claim": True, "swept": True,
                               "sweep_status": status},
                        message=(f"third-party claim paid the receiver; funding "
                                 f"swept back; {note}"),
                    )
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

    @staticmethod
    def _history_hit_excluded(tx: dict, *, exclude_coin_sets: Sequence[Sequence[str]],
                              exclude_coins: Sequence[str],
                              require_coins: Sequence[str]) -> bool:
        """[review] Whether a wallet-history record is some OTHER attempt's
        (its coins exactly a retired attempt's set, or overlapping an attempt
        still on the books), or cannot be attributed to the attempt sought
        (*require_coins* known, and the record's coins unknown or disjoint).
        A record without a removals list is excluded only when attribution
        is required."""
        from . import chia_send as cs

        coins = {_hx(c) for c in cs.removal_ids(tx)}
        if coins:
            if any(coins == {_hx(c) for c in s if c} for s in exclude_coin_sets if s):
                return True
            if coins.intersection(_hx(c) for c in exclude_coins if c):
                return True
        wanted = {_hx(c) for c in require_coins if c}
        if wanted and not coins.intersection(wanted):
            return True
        return False

    def _history_filters(self, job: WarpJob, tx_key: str,
                         pending: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        """The attempts on the books, as the history scanners' exclusions:
        every retired attempt's exact coin set, and the coins of the attempts
        not proven dead.  *pending* is a state patch not yet written."""
        st = {**job.state, **(pending or {})}
        prior_removals = dict(st.get(f"{tx_key}_prior_removals") or {})
        live, sets = self._attempt_books(prior_removals, list(_settled_ids(st, tx_key)))
        return {"exclude_coin_sets": sets, "exclude_coins": live}

    def _find_existing_funding(self, security_ph_hex: str, expected: int,
                               exclude_ids: Sequence[str] = (), *,
                               exclude_coin_sets: Sequence[Sequence[str]] = (),
                               exclude_coins: Sequence[str] = (),
                               require_coins: Sequence[str] = ()) -> Optional[str]:
        """Scan wallet history for an already-broadcast funding of this coin.

        ``exclude_ids`` are sends already proven dead ([WARP-LOST-SEND]): the
        wallet may keep retrying one whose coin is spent elsewhere, and
        adopting it again would wait forever.  [review] A record that is a
        RETIRED attempt's (its coins exactly one of ``exclude_coin_sets``, or
        overlapping ``exclude_coins`` -- an attempt still on the books) is
        never a hit either: a retired unnamed attempt's real record would
        otherwise be adopted as the current one's.  With ``require_coins``
        (the current attempt's own recorded coins) a hit must spend one of
        them.

        Fails **closed**. This scan is the only thing standing between a lost
        ``send_transaction`` response and a second send of the same amount to the
        same ephemeral puzzle, so a scan that could not run is not evidence of
        anything -- least of all of "not funded yet". Raising ``WarpRetryable``
        routes the step through :meth:`_apply_retry`: backoff, visible
        ``last_error``, and crucially no send.
        """
        try:
            # [WARP-LOST-SEND] The UNCONFIRMED set first, explicitly: the
            # daemon sorts unconfirmed records LAST, so on a busy wallet a
            # just-sent funding pages out of any windowed scan (the hazard
            # _find_existing_burn already guards against) -- and a miss here
            # is what the unnamed-send path counts toward sending again.
            pending = self._wallet.get_transactions(
                1, start=0, end=200, reverse=True, confirmed=False)
            # Then the wide mixed window.  This bot's wallet is busy, so a
            # funding tx can be pushed well down the history while the job
            # waits on confirmations -- a short window would miss it and
            # re-send. The timeout risk is handled by the client's timeout,
            # not by looking at less evidence.
            txs = self._wallet.get_transactions(1, start=0, end=200, reverse=True)
        except Exception as exc:  # noqa: BLE001 -- never read as "not funded"
            raise WarpRetryable(
                f"funding dedupe scan failed ({exc}); refusing to send in case a "
                "prior funding transaction already exists"
            ) from exc
        want = _hx(security_ph_hex)
        dead = {_hx(t) for t in exclude_ids if t}
        for tx in list(pending or []) + list(txs or []):
            if _hx(tx.get("name") or tx.get("transaction_id") or "") in dead:
                continue
            if self._history_hit_excluded(
                    tx, exclude_coin_sets=exclude_coin_sets,
                    exclude_coins=exclude_coins, require_coins=require_coins):
                continue
            for add in tx.get("additions") or []:
                if _hx(add.get("puzzle_hash", "")) == want and int(
                    add.get("amount", -1)
                ) == expected:
                    return tx.get("name") or tx.get("transaction_id")
        return None

    # -- [WARP-LOST-SEND 2026-10-25] Chia sends that may not land ---------- #

    def _chia_send_fee(self, configured: int, *, cost: int) -> int:
        """The fee a warp Chia send pays.

        A configured fee (> 0) is paid as stated.  0 means automatic: at
        least the engine's offer fee (``strategy.offer_fee_mojos``), raised
        to the node's own estimate for inclusion within _AUTO_FEE_TARGET_S
        when the mempool demands more, capped at _AUTO_FEE_CAP_MULTIPLE
        times the floor.  An unavailable estimate pays the floor.
        """
        if int(configured) > 0:
            return int(configured)
        floor = max(0, int(getattr(self._params, "offer_fee_mojos", 0) or 0))
        estimate = 0
        try:
            est = self._coinset.get_fee_estimate(
                cost=int(cost), target_times=[_AUTO_FEE_TARGET_S])
            estimate = max(0, int(est[0])) if est else 0
        except Exception as exc:  # noqa: BLE001 -- the floor still applies
            _log.debug("warp: fee estimate unavailable (%s); paying the floor", exc)
        return max(floor, min(estimate, floor * _AUTO_FEE_CAP_MULTIPLE))

    def _unwrap_xch_budget(self) -> int:
        """The spendable XCH an unwrap needs (G10): the toll plus the fee
        its cat_spend will pay plus the fee its toll funding send will pay,
        each estimated at its own spend's cost."""
        configured = self._params.unwrap_chia_fee_mojos
        return int(self._net.chia_toll_mojos) + (
            self._chia_send_fee(configured, cost=_COST_CAT_SPEND)
            + self._chia_send_fee(configured, cost=_COST_XCH_SEND))

    def _sweep_fee_for(self, job: WarpJob) -> int:
        """A sweep's fee, taken from the recovered coin: the automatic fee,
        never more than half the coin.  A fee-less sweep is what a busy
        mempool drops ([WARP-SWEEP-VERIFY])."""
        try:
            amount = int(self._funded_amount(job))
        except Exception:  # noqa: BLE001 -- no amount, no fee
            return 0
        return min(self._chia_send_fee(0, cost=_COST_XCH_SEND), max(0, amount // 2))

    def _send_reserved(self, job: WarpJob, *, tx_key: str,
                       send: Callable[[], dict],
                       reserve_patch: Optional[Dict[str, Any]] = None) -> dict:
        """Run *send* (one wallet RPC) with a DURABLE reservation first.

        The engine's stuck-transaction guard reads this job's markers from
        warp_jobs.db; a marker written only after the RPC returns leaves the
        send unprotected while it is being made (and while the engine may be
        mid-heartbeat).  So *tx_key* is set to _FUNDING_TX_UNKNOWN before
        the RPC and replaced with the real id by the caller.

        *reserve_patch* is what a REPLACEMENT send has already decided (the
        dead id joining ``<tx_key>_prior``, the cleared removals and output
        pin); it is persisted with the reservation, atomically, so an RPC
        that times out after broadcasting leaves a state the unnamed-send
        path can finish from.  A daemon refusal (``... failed: ...``) means
        nothing was sent: the marker and poll count the tick started with
        are restored, so a replacement is judged again next tick (and keeps
        its coin exclusions) while a first-ever send reverts to no marker.
        A transport failure (``request failed``) may have broadcast, so the
        reservation stands and the history scan decides next tick.
        """
        from .wallet import WalletError

        polls_key = f"{tx_key}_orphan_polls"
        patch = {**{k: v for k, v in (reserve_patch or {}).items()
                    if not k.startswith("_")},
                 tx_key: _FUNDING_TX_UNKNOWN, polls_key: 0,
                 f"{tx_key}_wallet_absent": None}
        # Everything the reservation changes, as the tick found it: a refusal
        # puts it ALL back (the marker, its polls, removals, priors and pin),
        # so the next tick judges the same send with the same evidence.
        previous = {key: job.state.get(key) for key in patch}
        self._store.update_job(job.id, expected_status=job.status,
                               state_patch=patch)
        try:
            return send()
        except Exception as exc:
            # Only the daemon's own rejection ("<endpoint> failed: <error>",
            # WalletClient._post's success=false form) proves nothing was
            # sent.  A transport failure ("request failed") or a malformed
            # reply ("unexpected response") may follow a broadcast: the
            # reservation stands and the history scan decides next tick.
            text = str(exc)
            low = text.lower()
            refused = (isinstance(exc, WalletError)
                       and re.match(r"^\S+ failed: ", text) is not None
                       and "request failed" not in low)
            # [review] A connection that was never made (refused, no route,
            # no such host) carried no bytes: nothing was sent either, and
            # keeping the reservation would leave an attempt nothing can
            # ever rule out.  A timeout or a reset after the request went
            # out is different -- the daemon may have acted on it.
            never_sent = (isinstance(exc, WalletError) and "request failed" in low
                          and any(t in low for t in _NEVER_CONNECTED_MARKERS))
            if refused or never_sent:
                self._store.update_job(job.id, expected_status=job.status,
                                       state_patch=previous)
            raise

    def _require_wallet_synced(self, context: str) -> None:
        """Pend until the Chia wallet reports synced and not syncing.

        Job 4's funding send went out 29 s after the wallet refused the
        first attempt as unsynced, while it was still flapping; the coin it
        selected was one an offer created in the same window also used.
        A send waits for a settled wallet.
        """
        try:
            # The configured fingerprint first: the daemon answers for the
            # key that is logged in, which another client may have changed.
            self._wallet.log_in()
            status = self._wallet.get_sync_status()
        except Exception as exc:  # noqa: BLE001 -- unknown is not synced
            raise WarpRetryable(
                f"{context}: wallet sync status unavailable ({exc})") from exc
        if status.get("synced") is not True or status.get("syncing") is True:
            raise WarpPending(f"{context}: the Chia wallet is syncing; waiting")

    def _coin_spent_depth(self, coin_id_hex: str) -> Optional[int]:
        """Confirmations of the spend of a coin: 0 when unspent, None when
        the coin is not indexed or the peak is unknown (then the depth
        cannot be told, and a spend must not count as proof)."""
        rec = self._coinset.get_coin_record_by_name(coin_id_hex)
        if rec is None:
            return None
        return self._record_spent_depth(rec)

    @staticmethod
    def _unnamed(tx_id: str) -> bool:
        """Whether *tx_id* is a send the daemon accepted but never named:
        the live sentinel, or a retired attempt's ``<sentinel>#n`` key."""
        return str(tx_id or "").startswith(_FUNDING_TX_UNKNOWN)

    def _send_probes(self, tx_id: str) -> tuple:
        """``(wallet_record, mempool_item)`` probes for *tx_id*.  An unnamed
        send has no transaction id to ask the hash-based RPCs about (their
        error would read as "could not tell" forever): the wallet is known
        to hold no record of that name and no mempool item carries it, so
        its recorded coins decide."""
        if self._unnamed(tx_id):
            return (lambda: None), (lambda: None)
        return ((lambda: self._wallet.get_transaction_or_none(tx_id)),
                (lambda: self._coinset.get_mempool_item_by_tx_id(tx_id)))

    def _identify_unnamed(self, scan: Callable[[Sequence[str]], Any], *,
                          exclude_ids: Sequence[str],
                          live_coins: Sequence[str],
                          recorded_sets: Sequence[Sequence[str]]) -> Optional[tuple]:
        """``(tx_id, removals)`` for a send the daemon never named, read back
        from the wallet history ([review]: its coin may have appeared before
        the next tick, leaving the sentinel with no coins).

        One-to-one: a transaction already identified for another attempt
        (*exclude_ids*), one whose coins are those of an attempt still on
        the books (*live_coins*: the coins of every attempt not proven
        dead), or one that IS another attempt's record (its coins exactly
        one of *recorded_sets*) is never handed out again -- one swept
        output must not settle two attempts.  Overlap with a proven-dead
        predecessor's SURVIVING coins is allowed: a replacement may reuse
        them, and refusing its record would block completion for good.
        ``None`` when nothing identifies it or the scan could not run.
        """
        from . import chia_send as cs

        used = [t for t in exclude_ids if t]
        live = {_hx(c) for c in live_coins if c}
        recorded = [frozenset(_hx(c) for c in cs_ if c) for cs_ in recorded_sets if cs_]
        for _ in range(16):
            # [review] A lookup that could not run PROPAGATES: "could not
            # look" must never read as "absent", or the absence-based grace
            # below would settle a send the wallet may still hold.
            found = scan(used)
            if not found:
                return None
            tx_id = str(found[0] if isinstance(found, tuple) else found)
            rec = self._wallet.get_transaction_or_none(tx_id)
            removals = [_hx(c) for c in cs.removal_ids(rec)] if rec else []
            if removals and (live.intersection(removals)
                             or frozenset(removals) in recorded):
                used.append(tx_id)            # already an attempt on the books
                continue
            return tx_id, removals
        return None

    @staticmethod
    def _attempt_books(prior_removals: Dict[str, Any],
                       dead: Sequence[str]) -> tuple:
        """``(live_coins, recorded_sets)`` for :meth:`_identify_unnamed`
        from the attempts' recorded coins and the proven-dead list."""
        dead_set = {t for t in dead if t}
        live = [c for t, cs_ in prior_removals.items()
                if t not in dead_set for c in (cs_ or [])]
        sets = [list(cs_ or []) for cs_ in prior_removals.values()]
        return live, sets

    def _identify_unnamed_funding(self, out_ph_hex: str, out_amount: int, *,
                                  exclude_ids: Sequence[str],
                                  live_coins: Sequence[str] = (),
                                  recorded_sets: Sequence[Sequence[str]] = (),
                                  require_coins: Sequence[str] = ()
                                  ) -> Optional[tuple]:
        return self._identify_unnamed(
            lambda used: self._find_existing_funding(
                out_ph_hex, int(out_amount), exclude_ids=used,
                require_coins=require_coins),
            exclude_ids=exclude_ids, live_coins=live_coins,
            recorded_sets=recorded_sets)

    def _identify_unnamed_burn(self, job: WarpJob, out_ph_hex: str, out_amount: int, *,
                               exclude_ids: Sequence[str],
                               live_coins: Sequence[str] = (),
                               recorded_sets: Sequence[Sequence[str]] = (),
                               require_coins: Sequence[str] = ()
                               ) -> Optional[tuple]:
        """The cat_spend counterpart of :meth:`_identify_unnamed_funding`."""
        wid = int(job.state["cat_wallet_id"])
        return self._identify_unnamed(
            lambda used: self._find_existing_burn(
                wid, out_ph_hex, int(out_amount), exclude_ids=used,
                require_coins=require_coins),
            exclude_ids=exclude_ids, live_coins=live_coins,
            recorded_sets=recorded_sets)

    def _identify_retired_by_coins(self, job: WarpJob, tx_key: str, tx_id: str,
                                   coins: Sequence[str], *, ph: str, amount: int,
                                   named: Sequence[str], live_coins: Sequence[str],
                                   recorded_sets: Sequence[Sequence[str]]) -> Optional[str]:
        """[review] A retired UNNAMED attempt whose coins are recorded is
        probed as absent by construction (the sentinel is no transaction
        id); before any absence-based settlement the wallet history is asked
        for a record spending those very coins, and a hit re-keys the
        attempt under its real id so the wallet and the mempool answer for
        it.  Returns the real id, or None.  A lookup that could not run
        propagates (never absence)."""
        own_sets = [s for s in recorded_sets if set(_hx(c) for c in s) != set(_hx(c) for c in coins)]
        own_live = [c for c in live_coins if _hx(c) not in {_hx(x) for x in coins}]
        if tx_key == "burn_tx_id":
            ident = self._identify_unnamed_burn(
                job, ph, amount, exclude_ids=named, live_coins=own_live,
                recorded_sets=own_sets, require_coins=coins)
        else:
            ident = self._identify_unnamed_funding(
                ph, amount, exclude_ids=named, live_coins=own_live,
                recorded_sets=own_sets, require_coins=coins)
        if not ident or not ident[0]:
            return None
        found = str(ident[0])
        self._persist_identified_attempt(job, tx_key, tx_id, found,
                                         list(ident[1]) or list(coins))
        return found

    def _refresh_exclusions(self, job: WarpJob, *, tx_key: str, out_ph_hex: str,
                            out_amount: int, pending: Dict[str, Any]) -> Dict[str, Any]:
        """Re-judge the retained replaced attempts on the tick that sends.

        [review] An orphan retired by Retry keeps its coins excluded; if a
        later conflicting spend proves it dead, those coins are released
        here -- otherwise its surviving input, were it the only coin large
        enough, would starve every replacement.  Returns the state to merge
        (the dead list and the refreshed exclusions), or {} when nothing
        changed or the chain could not be read.
        """
        from . import chia_send as cs

        st = {**job.state, **pending}
        prior_key, prior_removals_key, dead_key, exclude_key = (
            f"{tx_key}_prior", f"{tx_key}_prior_removals",
            f"{tx_key}_prior_dead", f"{tx_key}_exclude")
        priors = [t for t in (st.get(prior_key) or []) if t]
        prior_removals = dict(st.get(prior_removals_key) or {})
        dead = [t for t in (st.get(dead_key) or []) if t]
        done = _settled_ids(st, tx_key) | set(dead)
        retained = [t for t in priors if t not in done and prior_removals.get(t)]
        if not retained:
            return {}
        try:
            outputs = list(self._coinset.get_coin_records_by_puzzle_hash(
                out_ph_hex, include_spent=True))
        except Exception:  # noqa: BLE001 -- keep the exclusions as they are
            return {}
        min_depth = int(self._net.chia_confirmation_min_height)
        changed = False
        for tx_id in retained:
            probe_wallet, probe_mempool = self._send_probes(tx_id)
            ev = cs.gather(
                wallet_record=probe_wallet, mempool_item=probe_mempool,
                coin_spent=self._coin_spent_depth, outputs_at=lambda: outputs,
                known_removals=prior_removals.get(tx_id) or [],
                out_amount=int(out_amount), unique_output=False, min_depth=min_depth,
            )
            if cs.judge(ev)[0] is cs.Verdict.DEAD:
                dead.append(tx_id)
                changed = True
        if not changed:
            return {}
        done = done | set(dead)
        exclude = sorted({c for t, cs_ in prior_removals.items()
                          if t not in done for c in cs_ if c})
        return {dead_key: dead or None, exclude_key: exclude or None}

    def _unnamed_attempt_key(self, job: WarpJob, tx_key: str) -> str:
        """A distinct key for the unnamed attempt being retired, so each
        is settled on its own evidence ([review]: merged under one key, one
        attempt's dead input would have settled them all)."""
        # [review] Never a key in use, nor one ever used: counting the
        # unnamed entries would hand out "#2" again once "#1" was re-keyed
        # under its real id, and retiring another send under a live key
        # overwrites that attempt's coins.  The highest number ever seen in
        # any book (and a persisted sequence, for re-keyed ones) plus one.
        st = job.state
        prefix = f"{_FUNDING_TX_UNKNOWN}#"
        seen = int(st.get(f"{tx_key}_unnamed_seq") or 0)

        def _num(t: Any) -> int:
            t = str(t or "")
            if not t.startswith(prefix):
                return 0
            try:
                return int(t[len(prefix):])
            except ValueError:
                return 0

        for key in (f"{tx_key}_prior", f"{tx_key}_prior_dead"):
            for t in (st.get(key) or []):
                seen = max(seen, _num(t))
        for key in (f"{tx_key}_prior_removals", f"{tx_key}_prior_retired_at"):
            for t in (st.get(key) or {}).keys():
                seen = max(seen, _num(t))
        return f"{prefix}{seen + 1}"

    def _unnamed_group_dead(self, coins: Sequence[str], min_depth: int) -> bool:
        """A legacy entry that merged several unnamed attempts' coins under
        the bare sentinel is dead only when EVERY coin is spent at depth."""
        for cid in coins:
            depth = self._coin_spent_depth(cid)
            if depth is None or depth < int(min_depth):
                return False
        return True

    def _record_spent_depth(self, rec: Any) -> Optional[int]:
        """Confirmations of a coin record's spend: 0 when unspent, None when
        the peak or the spend height is unknown."""
        if not rec.spent:
            return 0
        peak = int(self._coinset.get_peak_height() or 0)
        spent_at = int(getattr(rec, "spent_block_index", 0) or 0)
        if peak <= 0 or spent_at <= 0 or peak < spent_at:
            return None
        return peak - spent_at + 1

    def _outputs_settled(self, outputs: Sequence[Any], *, amount: int,
                         removals: Sequence[str], min_depth: int) -> tuple:
        """``(settled, why)`` for a landed send's own outputs: every spent
        one must be spent at the network's confirmation depth.

        [review] A recovery spend (a sweep, a claim) one block deep can still
        be reorged out; a job closed on it would leave the coin at the key
        with Sweep gone.  An unspent output is the caller's business (Sweep
        again, or the completion scan); an unknown depth is not proof.
        """
        mine = {_hx(r) for r in removals if r}
        for rec in outputs:
            coin = getattr(rec, "coin", None)
            if coin is None or int(getattr(coin, "amount", -1)) != int(amount):
                continue
            if _hx(getattr(coin, "parent_coin_info", "")) not in mine:
                continue
            depth = self._record_spent_depth(rec)
            if depth is None:
                return False, "the depth of its coin's spend is unknown"
            if 0 < depth < int(min_depth):
                return False, (f"its coin's spend has {depth}/{min_depth} "
                               "confirmations")
        return True, ""

    def _replacement_patch(self, job: WarpJob, *, tx_key: str, tx_id: str,
                           removals: Sequence[str],
                           also_clear: Sequence[str] = (),
                           dead: bool = False) -> Dict[str, Any]:
        """State that retires the send recorded as *tx_id* and prepares its
        replacement: the marker and its polls cleared, the id added to
        ``<tx_key>_prior`` with the coins it spent in
        ``<tx_key>_prior_removals`` (Sweep asks whether it could still land
        before closing the job), those coins in ``<tx_key>_exclude`` so the
        replacement never reuses them, and *also_clear* (an output pin) set
        to None."""
        st = job.state
        prior_key, prior_removals_key, dead_key = (
            f"{tx_key}_prior", f"{tx_key}_prior_removals", f"{tx_key}_prior_dead")
        priors = [t for t in (st.get(prior_key) or []) if t]
        prior_removals = dict(st.get(prior_removals_key) or {})
        proven_dead = [t for t in (st.get(dead_key) or []) if t]
        settled_ids = _settled_ids(st, tx_key) | set(proven_dead)
        retired_at = dict(st.get(f"{tx_key}_prior_retired_at") or {})
        retired_at.setdefault(tx_id, float(self._now()))
        seq_patch: Dict[str, Any] = {}
        prefix = f"{_FUNDING_TX_UNKNOWN}#"
        if tx_id.startswith(prefix):
            try:
                seq_patch[f"{tx_key}_unnamed_seq"] = max(
                    int(st.get(f"{tx_key}_unnamed_seq") or 0), int(tx_id[len(prefix):]))
            except ValueError:
                pass
        coins = [c for c in removals if c]
        if coins:
            prior_removals[tx_id] = coins
        if dead and tx_id not in proven_dead:
            proven_dead.append(tx_id)
        # [review] The replacement excludes the coins of every replaced
        # attempt that COULD STILL LAND, not only the latest: with A
        # replaced by B and B by C, A may still surface, and C must not
        # compete with it either.  A send the chain proved dead can never
        # confirm, so its surviving inputs stay selectable: one conflicting
        # input makes the verdict, and excluding the rest of a multi-input
        # send could starve every replacement.  Every attempt's coins stay
        # on the books for the settlement accounting regardless.
        if dead:
            settled_ids.add(tx_id)
        exclude = sorted({c for t, cs_ in prior_removals.items()
                          if t not in settled_ids for c in cs_ if c})
        return {
            tx_key: None, f"{tx_key}_orphan_polls": 0,
            f"{tx_key}_removals": None,
            prior_key: priors + ([tx_id] if tx_id not in priors else []),
            prior_removals_key: prior_removals or None,
            dead_key: proven_dead or None,
            f"{tx_key}_prior_retired_at": retired_at or None,
            f"{tx_key}_exclude": exclude or None,
            f"{tx_key}_wallet_absent": None,
            **seq_patch,
            **{key: None for key in also_clear},
        }

    def _persist_identified_attempt(self, job: WarpJob, tx_key: str, old_key: str,
                                    tx_id: str, removals: Sequence[str]) -> None:
        """[review] A retired unnamed attempt identified from the wallet
        history is re-keyed under its real id, with the coins it spent, in
        every book (<tx_key>_prior, _prior_removals, _prior_dead,
        _prior_retired_at): later probes then ask the wallet and the
        mempool about THAT id and judge it by those coins -- it is never
        "unobservable" again, so a record that later drops out of the
        history cannot start the absence-based grace while the bundle may
        still sit in a mempool.  Best effort: this tick judges it anyway."""
        # [review] The LATEST books, not the caller's snapshot: a settlement
        # pass may re-key several attempts, and each write must build on the
        # previous one or the earlier identification is lost.
        try:
            fresh = self._store.get_job(job.id)
        except Exception as exc:  # noqa: BLE001 -- judged this tick regardless
            _log.debug("warp job %s: could not re-read before re-keying (%s)", job.id, exc)
            return
        st = fresh.state
        patch: Dict[str, Any] = {}
        priors = [t for t in (st.get(f"{tx_key}_prior") or []) if t]
        if old_key in priors:
            patch[f"{tx_key}_prior"] = [
                (tx_id if t == old_key else t) for t in priors
                if not (t == tx_id and old_key != tx_id)]
        removal_book = dict(st.get(f"{tx_key}_prior_removals") or {})
        removal_book.pop(old_key, None)
        removal_book[tx_id] = [_hx(c) for c in removals if c]
        patch[f"{tx_key}_prior_removals"] = removal_book
        dead = [t for t in (st.get(f"{tx_key}_prior_dead") or []) if t]
        if old_key in dead:
            patch[f"{tx_key}_prior_dead"] = [(tx_id if t == old_key else t) for t in dead]
        stamps = dict(st.get(f"{tx_key}_prior_retired_at") or {})
        if old_key in stamps:
            stamps[tx_id] = stamps.pop(old_key)
            patch[f"{tx_key}_prior_retired_at"] = stamps
        try:
            self._store.update_job(job.id, expected_status=fresh.status, state_patch=patch)
            if old_key == tx_id:
                _log.info("warp job %s: learned the coins of retired %s (%d)",
                          job.id, tx_id[:16], len(removal_book[tx_id]))
            else:
                _log.info("warp job %s: retired %s identified as %s (%d coins)",
                          job.id, old_key, tx_id, len(removal_book[tx_id]))
        except Exception as exc:  # noqa: BLE001 -- judged this tick regardless
            _log.debug("warp job %s: could not persist the identified attempt (%s)",
                       job.id, exc)

    def _mark_attempts_dead(self, job: WarpJob, tx_key: str, ids: Sequence[str], *,
                            presumed: bool = False) -> None:
        """[review] Record retired attempts proven dead -- or presumed gone
        after the grace -- in ``<tx_key>_prior_dead``, from the latest books:
        the engine's guard protects only attempts not listed there, the
        replacement exclusions skip them, and Abandon's record names only the
        rest.  A verdict found during settlement must not be forgotten by
        the next tick.  Best effort."""
        new = [t for t in ids if t]
        if not new:
            return
        # [review] Presumed-gone attempts are listed apart from proven-dead
        # ones: their coins are still unspent, so a job closed over them
        # keeps Sweep available.
        key = f"{tx_key}_prior_presumed" if presumed else f"{tx_key}_prior_dead"
        try:
            fresh = self._store.get_job(job.id)
            dead = [t for t in (fresh.state.get(key) or []) if t]
            add = [t for t in new if t not in dead]
            if not add:
                return
            self._store.update_job(fresh.id, expected_status=fresh.status,
                                   state_patch={key: dead + add})
            _log.info("warp job %s: attempt(s) %s %s", job.id,
                      ", ".join(t[:16] for t in add),
                      "presumed gone after the grace" if presumed else "proven dead")
        except Exception as exc:  # noqa: BLE001 -- judged this tick regardless
            _log.debug("warp job %s: could not record dead attempts (%s)", job.id, exc)

    def _unobservable_settled(self, job: WarpJob, tx_key: str, tx_id: str, *,
                              coins_known: bool = False) -> tuple:
        """``(settled, note)`` for a RETIRED attempt that is nowhere: no
        wallet record, no mempool item, and either no recorded coins or
        coins still unspent (an orphan).  Presumed gone once
        ``_UNOBSERVABLE_ATTEMPT_GRACE_S`` has passed since it was retired (an
        attempt retired before the clock existed starts it now); until then
        it keeps the job open.  [review] An orphan with coins is bounded the
        same way: unbounded, a finished claim would sit in CLAIMING until the
        operator abandoned it; its coins stay on the books, so were it to
        land after all, Sweep (available on a COMPLETED job) recovers it."""
        key = f"{tx_key}_prior_retired_at"
        stamps = dict(job.state.get(key) or {})
        since = stamps.get(tx_id)
        now = float(self._now())
        if not isinstance(since, (int, float)) or isinstance(since, bool):
            stamps[tx_id] = now
            try:
                self._store.update_job(job.id, expected_status=job.status,
                                       state_patch={key: stamps})
            except Exception as exc:  # noqa: BLE001 -- the clock starts next tick
                _log.debug("warp job %s: could not stamp attempt %s (%s)",
                           job.id, tx_id, exc)
            since = now
        waited = now - float(since)
        hours = _UNOBSERVABLE_ATTEMPT_GRACE_S // 3600
        what = ("its coins unspent" if coins_known else "no coins on record")
        if waited >= _UNOBSERVABLE_ATTEMPT_GRACE_S:
            _log.warning("warp job %s: attempt %s has had no wallet record and no "
                         "mempool item for %.1fh (%s); presumed gone",
                         job.id, tx_id[:16], waited / 3600.0, what)
            return True, ""
        if coins_known:
            return False, (f"attempt {tx_id[:16]}... could still land (its coins are "
                           "unspent, though it is in no wallet record and no mempool); "
                           f"presumed gone after {hours}h ({waited / 3600.0:.1f}h so far) "
                           "-- Sweep again later")
        return False, (f"attempt {tx_id[:16]}... is nowhere to be seen (no coins on "
                       f"record); presumed gone after {hours}h "
                       f"({waited / 3600.0:.1f}h so far) -- Sweep again later")

    def _prior_sends_settled(self, job: WarpJob,
                             tx_key: str = "funding_tx_id") -> tuple:
        """``(settled, note)``: once a funding send was REPLACED, whether
        every attempt -- the replaced ones and the current one -- can be
        ruled out, so Sweep (or a completed claim) may close the job.

        A replacement spends other coins, so any attempt not proven dead
        could still land after the job closes -- XCH at the ephemeral key
        with no Sweep left to recover it; and the current attempt may be
        the one still pending when a replaced one lands first and is swept.
        An attempt is settled when the chain proves it dead, or when its own
        output (an output whose parent is one of the coins it spent) is on
        chain and spent (swept or claimed).  An unspent coin at the key means
        "Sweep again"; a wallet-only confirmation, unknown coins, or a send
        still in flight mean "wait".  A job that never replaced its send is
        settled: this is the accounting of replacements.
        """
        from . import chia_send as cs, claim

        st = job.state
        priors = [t for t in (st.get(f"{tx_key}_prior") or []) if t]
        if not priors:
            return True, ""
        try:
            ph = claim.security_coin_puzzle_hash(self._load_ephemeral_sk(job)).hex()
            amount = int(self._funded_amount(job))
        except Exception as exc:  # noqa: BLE001 -- cannot judge: not settled
            return False, f"a replaced funding send cannot be judged ({exc})"
        self._wallet.log_in()
        outputs = list(self._coinset.get_coin_records_by_puzzle_hash(
            ph, include_spent=True))
        unspent = [r for r in outputs
                   if int(r.coin.amount) == amount and not r.spent]
        if unspent:
            return False, ("XCH from a replaced funding send is still at the "
                           "ephemeral key; Sweep again")
        prior_removals = dict(st.get(f"{tx_key}_prior_removals") or {})
        attempts = list(priors)
        current = str(st.get(tx_key) or "")
        if current:
            # [review] The current attempt too: a replaced send that landed
            # first and was swept must not close the job over it.
            # [review] The current attempt's OWN coins, named or not -- an
            # unnamed one under a key of its own, so the legacy merged-group
            # rule (the bare sentinel) never reads one multi-input send as a
            # group of attempts.
            if current == _FUNDING_TX_UNKNOWN:
                current = f"{_FUNDING_TX_UNKNOWN}#current"
            attempts.append(current)
            prior_removals.setdefault(
                current, list(st.get(f"{tx_key}_removals") or []))
        min_depth = int(self._net.chia_confirmation_min_height)
        # [review] Identification is one-to-one: every named attempt, every
        # id handed to an unnamed one, every coin of an attempt still on
        # the books, and every attempt's own record are excluded from the
        # next identification -- while a proven-dead predecessor's
        # surviving coins may legitimately reappear in its replacement.
        named = [t for t in attempts if not self._unnamed(t)]
        dead = list(_settled_ids(st, tx_key))
        live_coins, recorded_sets = self._attempt_books(prior_removals, dead)
        for tx_id in attempts:
            probe_id, coins = tx_id, list(prior_removals.get(tx_id) or [])
            if self._unnamed(tx_id) and coins and tx_id != current:
                # [review] Retired, unnamed, coins known: the history may
                # still name it (its record reappeared, or paged back in);
                # named, it is probed for real instead of read as absent.
                try:
                    found = self._identify_retired_by_coins(
                        job, tx_key, tx_id, coins, ph=ph, amount=amount, named=named,
                        live_coins=live_coins, recorded_sets=recorded_sets)
                except Exception as exc:  # noqa: BLE001 -- unavailable, not absent
                    return False, (f"the wallet history could not be read ({exc}); "
                                   f"attempt {tx_id[:16]}... is not ruled out")
                if found:
                    probe_id = found
                    named.append(found)
            if self._unnamed(tx_id) and not coins:
                # [review] With its coins recorded it is judged like any
                # other attempt (dead once they are spent elsewhere, landed
                # by its output's parent).  Without them the wallet history
                # may still name it -- its coin appeared before the tick
                # that would have adopted the id -- and the current marker is
                # then persisted under that name; otherwise nothing can rule
                # it out.
                try:
                    ident = self._identify_unnamed_funding(
                        ph, amount, exclude_ids=named, live_coins=live_coins,
                        recorded_sets=recorded_sets)
                except Exception as exc:  # noqa: BLE001 -- unavailable, not absent
                    return False, (f"the wallet history could not be read ({exc}); "
                                   f"attempt {tx_id[:16]}... is not ruled out")
                if ident and ident[0] and not ident[1]:
                    # [review] Identified, but its coins could not be read
                    # (the record vanished between the scan and the by-id
                    # lookup): keep the id now and PROBE it next pass -- the
                    # wallet and the mempool answer for a named id -- rather
                    # than fall into the absence rule on this tick.
                    found_id = str(ident[0])
                    if tx_id == current:
                        try:
                            self._store.update_job(
                                job.id, expected_status=job.status,
                                state_patch={tx_key: found_id})
                        except Exception as exc:  # noqa: BLE001
                            _log.debug("warp job %s: could not keep the identified "
                                       "id (%s)", job.id, exc)
                    else:
                        self._persist_identified_attempt(job, tx_key, tx_id, found_id, [])
                    return False, (f"attempt {tx_id[:16]}... identified as "
                                   f"{found_id[:16]}...; its coins are being read")
                if not ident or not ident[1]:
                    if tx_id != current:
                        # [review] Retired and unobservable: the grace clock.
                        gone, note = self._unobservable_settled(job, tx_key, tx_id)
                        if gone:
                            continue
                        return False, note
                    return False, ("a send the wallet never named, whose coins are "
                                   "not known, cannot be ruled out; Sweep again "
                                   "later or Abandon")
                probe_id, coins = ident
                named.append(probe_id)
                live_coins = [*live_coins, *coins]
                recorded_sets = [*recorded_sets, list(coins)]
                if tx_id == f"{_FUNDING_TX_UNKNOWN}#current":
                    try:
                        self._store.update_job(
                            job.id, expected_status=job.status,
                            state_patch={tx_key: probe_id, f"{tx_key}_removals": coins})
                    except Exception as exc:  # noqa: BLE001 -- judged anyway
                        _log.debug("warp job %s: could not persist the identified "
                                   "send (%s)", job.id, exc)
                else:
                    self._persist_identified_attempt(job, tx_key, tx_id, probe_id, coins)
            # unique_output=False: the key holds other attempts' coins too,
            # so an output is THIS send's only when its parent is one of the
            # coins this send spent.  A send whose coins are unknown can
            # therefore never read as landed.
            probe_wallet, probe_mempool = self._send_probes(probe_id)
            ev = cs.gather(
                wallet_record=probe_wallet,
                mempool_item=probe_mempool,
                coin_spent=self._coin_spent_depth,
                outputs_at=lambda: outputs,
                known_removals=coins,
                out_amount=amount, unique_output=False, min_depth=min_depth,
            )
            verdict, why = cs.judge(ev)
            if ev.removals and not coins:
                # [review] A record that reappeared taught the coins this
                # attempt spent: kept in its book, so a record that vanishes
                # again is judged by them (never presumed gone by absence).
                learned = [_hx(c) for c in ev.removals]
                if tx_id == current:
                    if not self._unnamed(tx_id):
                        try:
                            self._store.update_job(
                                job.id, expected_status=job.status,
                                state_patch={f"{tx_key}_removals": learned})
                        except Exception as exc:  # noqa: BLE001 -- judged anyway
                            _log.debug("warp job %s: could not keep the learned coins "
                                       "(%s)", job.id, exc)
                else:
                    self._persist_identified_attempt(job, tx_key, tx_id, tx_id, learned)
                coins = learned
            if verdict is cs.Verdict.ORPHAN and tx_id != current:
                # [review] Retired and nowhere -- wallet and mempool both
                # answered "no".  With no coins on record (a send recorded
                # before coins were kept whose record the wallet has since
                # deleted) nothing can ever prove it either way; with coins
                # still unspent it could in principle still land, but no
                # node holds a transaction for hours without including it.
                # Either way it is presumed gone after the grace rather than
                # keeping a finished job open for good; its coins stay on
                # the books, so a landing after all is still sweepable.
                gone, note = self._unobservable_settled(
                    job, tx_key, tx_id, coins_known=bool(ev.removals))
                if gone:
                    self._mark_attempts_dead(job, tx_key, [tx_id], presumed=True)
                    continue
                return False, note
            if verdict is cs.Verdict.DEAD:
                if (tx_id == _FUNDING_TX_UNKNOWN and len(ev.removals) > 1
                        and not self._unnamed_group_dead(ev.removals, min_depth)):
                    return False, ("unnamed funding sends share one record; not "
                                   "every coin of theirs is spent yet; Sweep "
                                   "again later")
                if tx_id != current and tx_id not in dead:
                    # [review] A verdict found here is kept: the guard and the
                    # exclusions read <tx_key>_prior_dead.
                    self._mark_attempts_dead(job, tx_key, [tx_id])
                    dead.append(tx_id)
                continue
            # [review] Landed ON CHAIN (its own output exists and, by the
            # check above, is spent -- at depth, or the recovery could yet be
            # reorged out); the wallet's word alone keeps waiting.
            if verdict is cs.Verdict.LANDED and ev.output_landed:
                if (tx_id == _FUNDING_TX_UNKNOWN and len(ev.removals) > 1
                        and not self._unnamed_group_dead(ev.removals, min_depth)):
                    return False, ("unnamed funding sends share one record; one "
                                   "landed but not every coin of theirs is spent "
                                   "yet; Sweep again later")
                deep, depth_why = self._outputs_settled(
                    outputs, amount=amount, removals=ev.removals,
                    min_depth=min_depth)
                if deep:
                    continue
                return False, (f"funding send {tx_id[:16]}... landed and "
                               f"{depth_why}; Sweep again later")
            return False, (f"funding send {tx_id[:16]}... could still land "
                           f"({why}); Sweep again later")
        return True, ""

    def _attempt_coins(self, job: WarpJob, tx_key: str) -> set:
        """Every coin any attempt recorded under *tx_key* spent: the current
        send's and each replaced one's."""
        st = job.state
        coins = {_hx(c) for c in (st.get(f"{tx_key}_removals") or []) if c}
        for removals in (st.get(f"{tx_key}_prior_removals") or {}).values():
            coins.update(_hx(c) for c in (removals or []) if c)
        return coins

    def _burn_priors_settled(self, job: WarpJob) -> tuple:
        """``(settled, note)``: whether every REPLACED cat_spend of *job* is
        ruled out.  A replaced burn is settled when the chain proves it dead,
        or when its own burn CAT (an output at the burn puzzle whose parent
        is one of the coins it spent) exists -- the completion scan then
        carries the unwrap forward.  Anything else could still land."""
        from . import chia_send as cs

        st = job.state
        priors = [t for t in (st.get("burn_tx_id_prior") or []) if t]
        if not priors:
            return True, ""
        ph = str(st.get("burn_cat_ph") or "")
        amount = int(job.amount_mojos)
        self._wallet.log_in()
        outputs = list(self._coinset.get_coin_records_by_puzzle_hash(
            ph, include_spent=True))
        prior_removals = st.get("burn_tx_id_prior_removals") or {}
        min_depth = int(self._net.chia_confirmation_min_height)
        named = [t for t in priors if not self._unnamed(t)]
        books = dict(prior_removals)
        current = str(st.get("burn_tx_id") or "")
        if current:
            if not self._unnamed(current):
                named.append(current)
            books.setdefault(current, list(st.get("burn_tx_id_removals") or []))
        dead = list(_settled_ids(st, "burn_tx_id"))
        live_coins, recorded_sets = self._attempt_books(books, dead)
        for tx_id in priors:
            probe_id, coins = tx_id, list(prior_removals.get(tx_id) or [])
            if self._unnamed(tx_id) and coins:
                try:
                    found = self._identify_retired_by_coins(
                        job, "burn_tx_id", tx_id, coins, ph=ph, amount=amount, named=named,
                        live_coins=live_coins, recorded_sets=recorded_sets)
                except Exception as exc:  # noqa: BLE001 -- unavailable, not absent
                    return False, (f"the wallet history could not be read ({exc}); "
                                   "an earlier cat_spend is not ruled out")
                if found:
                    probe_id = found
                    named.append(found)
            if self._unnamed(tx_id) and not coins:
                try:
                    ident = self._identify_unnamed_burn(
                        job, ph, amount, exclude_ids=named, live_coins=live_coins,
                        recorded_sets=recorded_sets)
                except Exception as exc:  # noqa: BLE001 -- unavailable, not absent
                    return False, (f"the wallet history could not be read ({exc}); "
                                   f"an earlier cat_spend is not ruled out")
                if ident and ident[0] and not ident[1]:
                    # [review] Identified without coins: keep the id, probe it
                    # next pass (see _prior_sends_settled).
                    self._persist_identified_attempt(job, "burn_tx_id", tx_id, str(ident[0]), [])
                    return False, (f"an earlier cat_spend identified as "
                                   f"{str(ident[0])[:16]}...; its coins are being read")
                if not ident or not ident[1]:
                    return False, ("an earlier cat_spend the wallet never named, "
                                   "whose coins are not known, cannot be ruled out")
                probe_id, coins = ident
                self._persist_identified_attempt(job, "burn_tx_id", tx_id, probe_id, coins)
                named.append(probe_id)
                live_coins = [*live_coins, *coins]
                recorded_sets = [*recorded_sets, list(coins)]
            probe_wallet, probe_mempool = self._send_probes(probe_id)
            ev = cs.gather(
                wallet_record=probe_wallet,
                mempool_item=probe_mempool,
                coin_spent=self._coin_spent_depth,
                outputs_at=lambda: outputs,
                known_removals=coins,
                out_amount=amount, unique_output=False, min_depth=min_depth,
            )
            verdict, why = cs.judge(ev)
            if ev.removals and not coins:
                # [review] As for the funding sends: coins a reappearing
                # record taught are kept in the attempt's book.
                self._persist_identified_attempt(
                    job, "burn_tx_id", tx_id, tx_id, [_hx(c) for c in ev.removals])
            if verdict is cs.Verdict.ORPHAN:
                # [review] A retired cat_spend that is nowhere: the same
                # bounded grace as the funding sends (a retired burn is
                # DEAD-only by construction, so this is a legacy shape).
                gone, note = self._unobservable_settled(
                    job, "burn_tx_id", tx_id, coins_known=bool(ev.removals))
                if gone:
                    self._mark_attempts_dead(job, "burn_tx_id", [tx_id], presumed=True)
                    continue
                return False, note
            if verdict is cs.Verdict.DEAD:
                if (tx_id == _FUNDING_TX_UNKNOWN and len(ev.removals) > 1
                        and not self._unnamed_group_dead(ev.removals, min_depth)):
                    return False, ("unnamed cat_spends share one record; not "
                                   "every coin of theirs is spent yet")
                if tx_id not in dead:
                    self._mark_attempts_dead(job, "burn_tx_id", [tx_id])
                    dead.append(tx_id)
                continue
            if verdict is cs.Verdict.LANDED and ev.output_landed:
                if (tx_id == _FUNDING_TX_UNKNOWN and len(ev.removals) > 1
                        and not self._unnamed_group_dead(ev.removals, min_depth)):
                    return False, ("unnamed cat_spends share one record; one "
                                   "landed but not every coin of theirs is spent "
                                   "yet")
                deep, depth_why = self._outputs_settled(
                    outputs, amount=amount, removals=ev.removals,
                    min_depth=min_depth)
                if deep:
                    continue
                return False, (f"an earlier cat_spend {tx_id[:16]}... landed "
                               f"and {depth_why}")
            return False, (f"an earlier cat_spend {tx_id[:16]}... could still "
                           f"land ({why})")
        return True, ""

    def _recorded_send_verdict(self, job: WarpJob, *, tx_key: str,
                               out_ph_hex: str, out_amount: int,
                               unique_output: bool) -> tuple:
        """``(Verdict, reason, evidence)`` for the send recorded under
        *tx_key* (chia_send.py).  The configured fingerprint is selected
        first; a login failure propagates (it is not absence)."""
        from . import chia_send as cs

        self._wallet.log_in()
        tx_id = str(job.state.get(tx_key) or "")
        # [review] An unnamed send is never asked about over the hash RPCs
        # (_send_probes); the history scan the caller ran is its only wallet
        # handle, and its recorded coins decide.
        wallet_record, mempool_item = self._send_probes(tx_id)
        if job.state.get(f"{tx_key}_prior"):
            # [review] Once a send was replaced the key holds other attempts'
            # coins too: a replaced send that landed and was swept must not
            # make a dead current one read as landed.  An output is this
            # send's only when its parent is one of the coins it spent.
            unique_output = False
        ev = cs.gather(
            wallet_record=wallet_record,
            mempool_item=mempool_item,
            coin_spent=self._coin_spent_depth,
            outputs_at=lambda: self._coinset.get_coin_records_by_puzzle_hash(
                out_ph_hex, include_spent=True),
            known_removals=job.state.get(f"{tx_key}_removals") or [],
            out_amount=int(out_amount), unique_output=unique_output,
            # [review] A conflicting spend is proof only at the depth the
            # network treats as final: shallower, a reorg could hand the
            # coin back and make the judged send valid again.
            min_depth=int(self._net.chia_confirmation_min_height),
        )
        verdict, why = cs.judge(ev)
        return verdict, why, ev

    def _judge_recorded_send(
        self, job: WarpJob, *, tx_key: str, label: str, out_ph_hex: str,
        out_amount: int, unique_output: bool, scan: Callable[[Sequence[str]], Any],
        also_clear: Sequence[str] = (),
    ) -> tuple:
        """Judge the Chia send recorded under *tx_key* on this tick.

        Returns ``(step, patch)``.  A step means "return it" (the send is
        alive, landed, orphaned, or undecided).  ``step is None`` means the
        chain PROVED the send dead and the caller sends again, merging
        *patch* into the state it records: the dead id joins
        ``<tx_key>_prior`` (so the dedupe scans skip it) with the coins it
        spent in ``<tx_key>_prior_removals`` (so Sweep can later tell whether
        it could still land), and ``<tx_key>_exclude`` names coins the
        replacement must not reuse.  The configured fingerprint is selected
        first: another client may have switched the daemon's key, and the
        configured wallet's own pending send must not read as absent.

        *scan* is the fail-closed wallet-history dedupe scan (given the ids
        already proven dead to skip; returns a tx id or falsy; raises when
        it could not run), used for a send the wallet accepted but never
        named (_FUNDING_TX_UNKNOWN): the history is the only handle on it.
        *also_clear* names state the dead send pinned and a replacement must
        not inherit (the burn's output coin id).
        """
        from . import chia_send as cs

        self._wallet.log_in()
        st = job.state
        tx_id = str(st.get(tx_key) or "")
        polls_key, removals_key, prior_key = (
            f"{tx_key}_orphan_polls", f"{tx_key}_removals", f"{tx_key}_prior")
        polls = int(st.get(polls_key) or 0)
        priors = [t for t in (st.get(prior_key) or []) if t]

        if tx_id == _FUNDING_TX_UNKNOWN:
            recorded = [c for c in (st.get(removals_key) or []) if c]
            # [review] A hit must be THIS attempt's record: never a retired
            # attempt's (the scan knows their coin sets), and, when the
            # daemon listed the coins this send spent, one spending them.
            found = scan(priors, require_coins=recorded)
            found_id = found[0] if isinstance(found, tuple) else found
            if found_id:
                # The coins it spent are kept (or re-learned from the record).
                adopted = {tx_key: str(found_id), polls_key: 0,
                           removals_key: recorded or None}
                # [review] The burn scan also names the output coin: pinned,
                # as the dedupe branch pins it, so a later unpinned scan
                # cannot adopt an unrelated CAT of the same receiver and
                # amount in its place.
                if (isinstance(found, tuple) and len(found) > 1 and found[1]
                        and also_clear):
                    adopted[also_clear[0]] = str(found[1])
                return _stay(
                    state=adopted,
                    message=(f"{label} identified as {found_id} in the wallet "
                             "history; awaiting the coin"),
                ), {}
            polls += 1
            if recorded:
                # [review] The daemon listed the coins it spent: the chain
                # can still prove the send dead (a coin spent elsewhere at
                # depth, no output of ours), and then it is sent again under
                # its own retired key; anything short of that waits.
                verdict, why, ev = self._recorded_send_verdict(
                    job, tx_key=tx_key, out_ph_hex=out_ph_hex,
                    out_amount=out_amount, unique_output=unique_output)
                if verdict is cs.Verdict.DEAD:
                    _log.warning("warp job %s: unnamed %s is dead (%s); sending again",
                                 job.id, label, why)
                    return None, self._replacement_patch(
                        job, tx_key=tx_key,
                        tx_id=self._unnamed_attempt_key(job, tx_key),
                        removals=recorded, also_clear=also_clear, dead=True)
                return _stay(
                    state={polls_key: polls},
                    message=(f"{label} unnamed by the wallet and absent from its "
                             f"history (poll {polls}): {why}; waiting"),
                ), {}
            hint = (("its coins are unknown, so it can never be proven dead; "
                     "Abandon (once the job fails at its deadline) closes it")
                    if tx_key == "burn_tx_id"
                    else "Retry (once the job fails at its deadline) sends it again")
            return _stay(
                state={polls_key: polls},
                message=(f"{label} unnamed by the wallet and absent from its "
                         f"history (poll {polls}); waiting -- {hint}"),
            ), {}

        verdict, why, ev = self._recorded_send_verdict(
            job, tx_key=tx_key, out_ph_hex=out_ph_hex, out_amount=out_amount,
            unique_output=unique_output)
        learned: Dict[str, Any] = {}
        if ev.removals and not st.get(removals_key):
            learned[removals_key] = list(ev.removals)
        # [review] Whether the wallet holds the send, as of THIS judgement:
        # the engine's guard protects a wallet record, so a send the wallet
        # answered "no record" for protects nothing until the wallet holds it
        # again -- whatever the verdict (ORPHAN, then SHALLOW with the record
        # back).  Unanswered leaves the last answer standing.
        if ev.wallet_known is True:
            learned[f"{tx_key}_wallet_absent"] = False
        elif ev.wallet_known is False:
            learned[f"{tx_key}_wallet_absent"] = True
        if verdict is cs.Verdict.DEAD:
            _log.warning("warp job %s: %s %s is dead (%s); sending again",
                         job.id, label, tx_id, why)
            return None, self._replacement_patch(
                job, tx_key=tx_key, tx_id=tx_id,
                removals=list(ev.removals) or (st.get(removals_key) or []),
                also_clear=also_clear, dead=True)
        if verdict is cs.Verdict.ORPHAN:
            polls += 1
            hint = (("it is sent again once those coins are spent by anything "
                     "else, which proves it dead; Abandon (once the job fails at "
                     "its deadline) closes it")
                    if tx_key == "burn_tx_id"
                    else "Retry (once the job fails at its deadline) sends it again")
            return _stay(
                state={**learned, polls_key: polls},
                message=f"{label} {tx_id} is {why} (poll {polls}); waiting -- {hint}",
            ), {}
        if verdict in (cs.Verdict.UNKNOWN, cs.Verdict.SHALLOW):
            return _stay(
                state=learned or None,
                message=f"{label} {tx_id}: {why}; awaiting the coin",
            ), {}
        # LANDED or ALIVE: the completion scan above picks the coin up.
        return _stay(
            state={**learned, polls_key: 0},
            message=f"{label} {tx_id} {why}; awaiting the coin",
        ), {}

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
        if target in (JobStatus.BURN_SENT, JobStatus.FUNDING_CLAIM,
                      JobStatus.BURNING):
            # [WARP-LOST-SEND 2026-10-25] A Chia send that is nowhere -- not
            # in the wallet, not in the mempool, its coins unspent -- is never
            # sent again on the handler's own authority: a replacement spends
            # other coins, so both could land.  For the XCH funding sends the
            # operator's Retry is that authority (the surplus is sweepable);
            # a cat_spend is replaced only once the chain proves it dead, by
            # the handler itself, so Retry leaves an orphaned burn in place.
            # The marker is cleared (the coins it spent become the
            # replacement's exclusions and are kept for Sweep's accounting),
            # so the handler runs its fail-closed history scan and, finding
            # nothing, sends.  A send the wallet or the mempool still holds
            # is kept.
            return self._release_recorded_send(job, target)
        if target == JobStatus.RELAYING:
            if job.state.get("delivered_by"):
                # [WARP-LOST-SEND] Delivered already: the job failed waiting
                # for a replaced toll send to settle, not for the relay.  A
                # fresh relay would only revert "!nonce" and burn its gas;
                # the mined one is re-polled and the wait resumes.
                return {}
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

    def _release_recorded_send(self, job: WarpJob, target: str) -> Dict[str, Any]:
        """Retry's reset for a job that failed waiting on a Chia send: the
        replacement patch when the send is proven dead (an XCH funding send
        also when it is orphaned), {} when it is still alive somewhere, has
        landed, or is a cat_spend the chain has not proven dead."""
        from . import chia_send as cs, claim

        tx_key = "burn_tx_id" if target == JobStatus.BURN_SENT else "funding_tx_id"
        tx_id = str(job.state.get(tx_key) or "")
        if not tx_id:
            # [review] Nothing recorded, nothing to release -- and nothing
            # to derive: a BURNING job that failed generating its toll key
            # has no ephemeral blob to load, and Retry must still work.
            return {}
        if target == JobStatus.BURN_SENT:
            unique = False
            out_ph = str(job.state["burn_cat_ph"])
            out_amount = int(job.amount_mojos)
            also_clear: tuple = ("burn_cat_coin_id",)
            wid = int(job.state["cat_wallet_id"])
            scan = lambda dead, **kw: self._find_existing_burn(  # noqa: E731
                wid, out_ph, out_amount, exclude_ids=dead,
                **{**self._history_filters(job, tx_key), **kw})
        else:
            unique, also_clear = True, ()
            out_ph = claim.security_coin_puzzle_hash(
                self._load_ephemeral_sk(job)).hex()
            out_amount = (int(self._net.chia_toll_mojos)
                          if target == JobStatus.BURNING
                          else int(self._funded_amount(job)))
            scan = lambda dead, **kw: self._find_existing_funding(  # noqa: E731
                out_ph, out_amount, exclude_ids=dead,
                **{**self._history_filters(job, tx_key), **kw})
        priors = [t for t in (job.state.get(f"{tx_key}_prior") or []) if t]
        # [review] A cat_spend is replaced ONLY when the chain proves it dead:
        # a replacement spends other coins, so an orphaned burn that surfaced
        # later would put a second CAT on the forward-only burn puzzle with
        # nothing to bridge it.  The XCH funding sends may be replaced while
        # orphaned -- their surplus is XCH at a key Sweep recovers from.
        burn = target == JobStatus.BURN_SENT
        if tx_id == _FUNDING_TX_UNKNOWN:
            self._wallet.log_in()
            recorded = sorted({_hx(c) for c in (job.state.get(f"{tx_key}_removals") or []) if c})
            found = scan(priors, require_coins=recorded)
            found_id = found[0] if isinstance(found, tuple) else found
            if found_id:
                return {}
            # [review] The daemon may have named no transaction yet listed
            # the coins it spent: they are the replacement's exclusions and
            # the attempt's own handle for the settlement accounting, kept
            # under a key of its own.  With them the send is judged like a
            # named one (a cat_spend still only replaced when dead); without
            # them only an XCH funding send may be sent again.
            key = self._unnamed_attempt_key(job, tx_key)
            if recorded:
                verdict, _why, ev = self._recorded_send_verdict(
                    job, tx_key=tx_key, out_ph_hex=out_ph, out_amount=out_amount,
                    unique_output=unique)
                allowed = ((cs.Verdict.DEAD,) if burn
                           else (cs.Verdict.DEAD, cs.Verdict.ORPHAN))
                if verdict in allowed:
                    return self._replacement_patch(
                        job, tx_key=tx_key, tx_id=key, removals=recorded,
                        also_clear=also_clear, dead=verdict is cs.Verdict.DEAD)
                return {}
            if burn:
                return {}
            return self._replacement_patch(
                job, tx_key=tx_key, tx_id=key, removals=[],
                also_clear=also_clear)
        verdict, _why, ev = self._recorded_send_verdict(
            job, tx_key=tx_key, out_ph_hex=out_ph, out_amount=out_amount,
            unique_output=unique)
        allowed = ((cs.Verdict.DEAD,) if burn
                   else (cs.Verdict.DEAD, cs.Verdict.ORPHAN))
        if verdict in allowed:
            return self._replacement_patch(
                job, tx_key=tx_key, tx_id=tx_id, removals=list(ev.removals),
                also_clear=also_clear, dead=verdict is cs.Verdict.DEAD)
        return {}

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
                # The replacement must bridge the SAME token as the
                # transaction it replaces: re-signing at the pinned nonce
                # without the asset would swap a stuck milliETH bridge for
                # a USDC one, and the attested-source anchor would then
                # kill the job AFTER real funds had moved.
                asset=self._job_asset(job),
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

    def _cat_wallet_ids(self, asset_id: Optional[str] = None) -> list:
        """Every daemon wallet holding the wrapped asset, by TAIL hash.

        Raises :class:`WarpError` (retryable) when any candidate failed to
        answer: absence is only proven when every candidate answered.
        """
        from .wallet import WalletError

        want = (asset_id or self._net.expected_asset_id).lower()
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
        if len(matches) != 1 and errors:
            # One flaky RPC must not FAIL the job: absence is only proven
            # when every candidate answered.
            raise WarpError(
                f"CAT wallet resolution degraded ({errors[0]}); retrying"
            )
        return matches

    def _resolve_cat_wallet_id(self, asset_id: Optional[str] = None) -> int:
        """The daemon wallet holding the wrapped asset, by TAIL hash.

        *asset_id* selects which wrapped CAT (default: wUSDC.b).
        """
        matches = self._cat_wallet_ids(asset_id)
        if len(matches) == 1:
            return matches[0]
        want = (asset_id or self._net.expected_asset_id).lower()
        raise WarpTerminal(
            f"expected exactly one CAT wallet for asset "
            f"{want}, found {matches or 'none'}"
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
        eth_target = job.state.get("rebalance_eth_target_wei")
        if eth_target:
            step, size = self._rebalance_millieth_trim(
                job, amount, int(eth_target))
            if step is None and size < amount:
                _log.info("top-up unwrap trimmed %d -> %d mojos (gas landed "
                          "while the job waited)", amount, size)
            return step, size
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

    def _rebalance_millieth_trim(self, job: WarpJob, amount: int,
                                 eth_target_wei: int):
        """[MULTI-ASSET-OUT] The wmilliETH.b top-up's version of the trim.

        The need is "ETH on Base back to the reserve target", and milliETH
        already on Base counts toward it (the next consult unwraps it to ETH
        for free). If gas landed while the job waited, burning the stale
        amount would overshoot -- so cancel or trim before the commit point.
        1 wmilliETH.b mojo = 1 milliETH base unit = 1e12 wei; the 30 bps tip
        is grossed up the same way the planner does it.
        """
        net = self._net
        wpu = _c_wei_per_milli_unit(net)
        # [review] ONE live fee read for every check below.
        fee = int(self._evm.get_fee_data().max_fee_per_gas)
        min_units, floor_mojos = _millieth_topup_minimums(fee, wpu)
        eth = int(self._evm.get_eth_balance(self._hot_address))
        # [review] The proceeds land as milliETH, and turning them into gas
        # is a withdraw() this wallet pays for (external relay covers only
        # the delivery). Fees rise between queueing and the gate: below that
        # cost now, cancel before anything is committed.
        if _top_up_withdraw_short(eth, fee):
            return _advance(
                JobStatus.CANCELLED,
                message=("top-up unwrap cancelled before the burn: the hot "
                         "wallet's ETH no longer covers the milliETH "
                         "withdraw() at current Base fees"),
            ), amount
        milli = int(self._evm.get_erc20_balance(
            net.milli_eth_address, self._hot_address))
        # [review] Held milliETH counts toward the need only when the planner
        # would withdraw it (at least its economic minimum): a smaller balance
        # waits, and crediting it would cancel this burn while ETH stays low.
        credited = milli if milli >= min_units else 0
        held = eth + credited * wpu
        short = eth_target_wei - held
        if short <= 0:
            return _advance(
                JobStatus.CANCELLED,
                message=("top-up unwrap no longer needed: the ETH fee reserve "
                         "was met while the job waited"),
            ), amount
        units = -(-short // wpu)
        grossed = -(-units * 10_000 // (10_000 - rb._UNWRAP_TIP_BPS))
        # [review] Never trimmed (or left) below the smallest burn worth
        # making at the gas price NOW: a burn whose proceeds cannot pay for
        # their own withdraw and relay would shrink the reserve. Nothing is
        # committed yet, so cancelling frees the slot; the next consult
        # re-plans from live balances.
        size = min(max(grossed, floor_mojos), amount)
        if size < floor_mojos:
            return _advance(
                JobStatus.CANCELLED,
                message=(f"top-up unwrap not worth making now: {size} mojos "
                         "would deliver less ETH than the withdraw and relay "
                         f"gas cost at current Base fees (at least "
                         f"{floor_mojos} needed)"),
            ), amount
        if size < amount:
            return None, int(size)
        return None, amount

    def _h_unwrap_checks(self, job: WarpJob) -> _Step:
        """Every gate, ordered cheap-to-expensive, all before the cat_spend."""
        from . import clvm_utils as cu, drivers, evm

        net, p = self._net, self._params
        receiver = bytes.fromhex(job.state["receiver_evm"])
        amount = int(job.amount_mojos)
        # [MULTI-ASSET-OUT] Every asset-dependent term below comes from the
        # job's frozen asset: token, TAIL, CAT wallet, precision.
        spec = self._job_asset(job)

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
        wallet_id = int(job.state.get("cat_wallet_id")
                        or self._resolve_cat_wallet_id(spec.expected_asset_id))

        # G5: spendable vs confirmed. Over-confirmed is terminal (the amount
        # cannot exist); over-spendable only pends -- the difference is offer
        # collateral that frees as offers close.
        bal = self._wallet.get_wallet_balance(wallet_id)
        confirmed = int(bal.get("confirmed_wallet_balance") or 0)
        spendable = int(bal.get("spendable_balance") or 0)
        if amount > confirmed:
            # [review] An AUTOMATIC unwrap was sized from a balance an engine
            # fill may have spent since; nothing is committed yet, so cancel
            # (freeing the slot for the next consult to re-plan) rather than
            # leave a FAILED job holding it until an operator steps in.
            if job.state.get("rebalance"):
                return _advance(
                    JobStatus.CANCELLED,
                    message=(f"automatic unwrap of {amount} mojos cancelled "
                             f"before the burn: the CAT balance is now "
                             f"{confirmed}; the next consult re-plans"),
                )
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
            amount, tip_bps, int(spec.erc20_decimals), net.cat_decimals
        )

        # G8/G9/G10: liquidity, relay gas, toll funds -- all pends.
        liquidity = self._evm.get_erc20_balance(
            spec.erc20_address, net.erc20_bridge_address
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
        toll_need = self._unwrap_xch_budget()
        if int(xch.get("spendable_balance") or 0) < toll_need:
            raise WarpPending(
                f"spendable XCH below the unwrap budget of {toll_need} mojos "
                "(the toll plus a fee for the cat_spend and one for the toll "
                "funding send -- automatic, at least strategy.offer_fee_mojos "
                "each, unless unwrap_chia_fee_mojos pins them)"
            )

        burn_inner_ph = drivers.expected_burn_inner_puzzle_hash(
            net, receiver, asset=spec)
        state = {
            "cat_wallet_id": wallet_id,
            "post_tip_base_units": post_tip,
            "burn_inner_ph": burn_inner_ph.hex(),
            "burn_cat_ph": drivers.expected_burn_cat_puzzle_hash(
                net, receiver, asset=spec).hex(),
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
        # [review] The size the gates checked IS the size to burn: a
        # rebalance trim above changed it, and _h_burn_sent (and the
        # engine's ledger booking) read the job's columns, not this local.
        return _advance(
            JobStatus.BURN_SENT,
            columns={
                "amount_mojos": amount,
                "amount_usdc_micros": amount * 10 ** (
                    int(spec.erc20_decimals) - net.cat_decimals),
            },
            state=state,
            message=f"all gates passed; sending {amount} mojos to the burn puzzle",
        )

    def _find_existing_burn(self, wallet_id: int, burn_cat_ph_hex: str,
                            amount: int, exclude_ids: Sequence[str] = (), *,
                            exclude_coin_sets: Sequence[Sequence[str]] = (),
                            exclude_coins: Sequence[str] = (),
                            require_coins: Sequence[str] = ()):
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
        dead = {_hx(t) for t in exclude_ids if t}

        def _scan(txs):
            for tx in txs:
                # [WARP-LOST-SEND] Never a send already proven dead.
                if _hx(tx.get("name") or tx.get("transaction_id") or "") in dead:
                    continue
                # [review] Nor another attempt's record (see _find_existing_funding).
                if self._history_hit_excluded(
                        tx, exclude_coin_sets=exclude_coin_sets,
                        exclude_coins=exclude_coins, require_coins=require_coins):
                    continue
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
            # [WARP-LOST-SEND] The pin names a coin that never appeared (its
            # send was replaced); an EARLIER attempt's burn CAT may be there
            # instead.  Any unspent coin of the amount whose parent is a coin
            # one of this job's attempts spent carries the unwrap forward,
            # re-pinned with its provenance.
            mine = self._attempt_coins(job, "burn_tx_id")
            if mine:
                from . import clvm_utils as cu

                for rec in self._coinset.get_coin_records_by_puzzle_hash(burn_cat_ph):
                    parent = _hx(rec.coin.parent_coin_info)
                    if (int(rec.coin.amount) == amount and not rec.spent
                            and parent in mine):
                        coin_id = cu.coin_name(
                            bytes.fromhex(parent), bytes.fromhex(burn_cat_ph),
                            amount).hex()
                        return _advance(
                            JobStatus.BURNING,
                            state={"burn_cat_parent": parent,
                                   "burn_cat_coin_id": coin_id},
                            message=("an earlier cat_spend's burn CAT is on "
                                     "chain; the unwrap is now forward-only"),
                        )
        else:
            # [review] Unpinned (a replacement whose response named no coin,
            # or timed out after broadcasting): once this job has attempts
            # on record, a coin is adopted only with provenance -- its parent
            # is one of the coins an attempt spent, or the wallet history
            # names it as the output of our cat_spend.  A bare ph+amount
            # match could otherwise bridge an unrelated CAT of the same
            # receiver and amount while our own replacement lands untracked.
            mine = self._attempt_coins(job, "burn_tx_id")
            attempted = bool(mine or job.state.get("burn_tx_id_prior"))
            known_ids: set = set()
            if attempted and not job.state.get("burn_tx_id_removals"):
                # The current attempt's inputs are unknown: the wallet
                # history is the only way to name its output.  [review] The
                # configured fingerprint first: another client may have
                # switched the daemon's key, and another wallet's same-amount
                # burn must not read as ours.
                try:
                    self._wallet.log_in()
                    found = self._find_existing_burn(
                        int(job.state["cat_wallet_id"]), burn_cat_ph, amount,
                        exclude_ids=[t for t in (job.state.get("burn_tx_id_prior") or []) if t],
                        **self._history_filters(job, "burn_tx_id"))
                except Exception:  # noqa: BLE001 -- then nothing is adopted here
                    found = None
                if found and found[1]:
                    known_ids.add(_hx(found[1]))
            from . import clvm_utils as cu

            for rec in self._coinset.get_coin_records_by_puzzle_hash(burn_cat_ph):
                if int(rec.coin.amount) != amount or rec.spent:
                    continue
                parent = _hx(rec.coin.parent_coin_info)
                coin_id = cu.coin_name(
                    bytes.fromhex(parent), bytes.fromhex(burn_cat_ph), amount).hex()
                if attempted and parent not in mine and coin_id not in known_ids:
                    continue                      # not provably this job's
                return _advance(
                    JobStatus.BURNING,
                    state={"burn_cat_parent": parent, "burn_cat_coin_id": coin_id},
                    message="burn CAT is on chain; the unwrap is now forward-only",
                )

        # b) The persisted marker.  [WARP-LOST-SEND] Judged, not assumed:
        # a cat_spend whose coin went to another spend can never land and is
        # sent again; one that merely vanished (no wallet, no mempool, coins
        # unspent) is NOT -- not even on Retry: a replacement spends other
        # coins, so both could land, and a second CAT at the forward-only
        # burn puzzle has nothing to bridge it.  It waits until the chain
        # proves it dead; Abandon closes the job.
        resend: Dict[str, Any] = {}
        if job.state.get("burn_tx_id"):
            wid = int(job.state["cat_wallet_id"])
            step, resend = self._judge_recorded_send(
                job, tx_key="burn_tx_id", label="cat_spend",
                out_ph_hex=burn_cat_ph, out_amount=amount, unique_output=False,
                scan=lambda dead, **kw: self._find_existing_burn(
                    wid, burn_cat_ph, amount, exclude_ids=dead,
                    **{**self._history_filters(job, "burn_tx_id"), **kw}),
                also_clear=("burn_cat_coin_id",),
            )
            if step is not None:
                return step
        priors = [t for t in (resend.get("burn_tx_id_prior")
                              or job.state.get("burn_tx_id_prior") or []) if t]

        # c) Fail-closed history scan -- a lost response must not burn twice.
        try:
            found = self._find_existing_burn(
                int(job.state["cat_wallet_id"]), burn_cat_ph, amount,
                exclude_ids=priors,
                **self._history_filters(job, "burn_tx_id", pending=resend),
            )
        except Exception as exc:  # noqa: BLE001 -- cannot prove it was not sent
            raise WarpError(
                f"wallet history scan failed ({exc}); refusing to send the "
                "irreversible cat_spend blindly"
            ) from exc
        if found:
            tx_id, coin_id = found
            patch = {**resend, "burn_tx_id": tx_id}
            if coin_id:
                patch["burn_cat_coin_id"] = coin_id
            return _stay(
                state=patch,
                message="cat_spend already in the wallet history (dedupe hit)",
            )

        # [review] An automatic ETH top-up re-derives, immediately before the
        # irreversible send, everything the gate checked (one fee read): the
        # proceeds can still be withdrawn (external relay skipped G9), the
        # burn is still worth making, and it is still NEEDED at this size --
        # ETH may have arrived since the gate or while this step retried. It
        # is never trimmed here (amount_mojos and post_tip_base_units were
        # frozen at the gate): a satisfied, uneconomic or oversized job is
        # cancelled, which a-c make safe (nothing was sent), and the next
        # consult re-plans from live balances.
        eth_target = job.state.get("rebalance_eth_target_wei")
        if eth_target:
            step, size = self._rebalance_millieth_trim(
                job, amount, int(eth_target))
            if step is not None or size < amount:
                # [review] Never CANCELLED -- a closed job, with no recovery
                # action -- while an earlier cat_spend of this job could
                # still land at the burn puzzle.  It waits until each is
                # proven dead; one that lands is picked up by the completion
                # scan above and the unwrap proceeds.
                settled, note = self._burn_priors_settled(job)
                if not settled:
                    return _stay(
                        message=(f"top-up no longer needed as sized, but {note}; "
                                 "waiting before cancelling"),
                    )
            if step is not None:
                return step
            if size < amount:
                return _advance(
                    JobStatus.CANCELLED,
                    message=(f"top-up unwrap cancelled before the cat_spend: "
                             f"ETH arrived since the gate, so only {size} of "
                             f"its {amount} mojos are still needed; the next "
                             "consult re-plans"),
                )

        # d) Send. Default coin selection ONLY -- it is what honours the trade
        # manager's offer locks (the client has no coins parameter, by design).
        # The XCH fee is re-checked on the sending tick; G10's budget ran at
        # UNWRAP_CHECKS and collateral may have moved.
        fee = self._chia_send_fee(p.unwrap_chia_fee_mojos, cost=_COST_CAT_SPEND)
        self._require_wallet_synced("cat_spend")
        # [review] Not just this fee: after the burn the job is forward-only
        # and must still fund the toll coin (toll + the send's fee).  Fees
        # may have risen since G10, so the whole remaining budget is
        # re-checked on the tick that commits.
        toll_fee = self._chia_send_fee(p.unwrap_chia_fee_mojos, cost=_COST_XCH_SEND)
        self._require_spendable_xch(
            int(net.chia_toll_mojos) + fee + toll_fee,
            context="cat_spend (its fee, then the toll coin and its fee)",
        )
        self._wallet.log_in()
        resend = {**resend, **self._refresh_exclusions(
            job, tx_key="burn_tx_id", out_ph_hex=burn_cat_ph,
            out_amount=amount, pending=resend)}
        exclude = [c for c in ((resend if "burn_tx_id_exclude" in resend
                               else job.state).get("burn_tx_id_exclude") or []) if c]
        record = self._send_reserved(
            job, tx_key="burn_tx_id", reserve_patch=resend,
            send=lambda: self._wallet.cat_spend(
                int(job.state["cat_wallet_id"]),
                amount,
                job.state["burn_address"],
                fee_mojos=fee,
                **({"exclude_coin_ids": exclude} if exclude else {}),
            ),
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
        from . import chia_send as cs

        patch = {**resend, "burn_tx_id": tx_id,
                 "burn_tx_id_removals": list(cs.removal_ids(record)) or None,
                 "burn_tx_id_orphan_polls": 0, "burn_fee_mojos": fee,
                 "burn_tx_id_exclude": None, "burn_tx_id_wallet_absent": None}
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
            else:
                # [review] Not indexed yet: wait.  Re-electing another coin
                # of the amount here (a replaced toll send that landed too)
                # would rebuild the bundle around a different bridging coin
                # than the nonce the job polls.
                raise WarpPending("pinned toll coin is not indexed yet; awaiting it")
        if sec is None:
            sec = claim.find_security_coin(self._coinset, sk, toll)
        if sec is None:
            # Fund it -- the same marker/dedupe/send ladder as the claim path,
            # with funding_amount persisted so Sweep just works.
            ph = claim.security_coin_puzzle_hash(sk)
            resend: Dict[str, Any] = {}
            if job.state.get("funding_tx_id"):
                # [WARP-LOST-SEND] Judged, as the claim's funding is.
                step, resend = self._judge_recorded_send(
                    job, tx_key="funding_tx_id", label="toll tx",
                    out_ph_hex=ph.hex(), out_amount=toll, unique_output=True,
                    scan=lambda dead, **kw: self._find_existing_funding(
                        ph.hex(), toll, exclude_ids=dead,
                        **{**self._history_filters(job, "funding_tx_id"), **kw}),
                )
                if step is not None:
                    return step
            priors = [t for t in (resend.get("funding_tx_id_prior")
                                  or job.state.get("funding_tx_id_prior") or [])
                      if t]
            try:
                existing = self._find_existing_funding(
                    ph.hex(), toll, exclude_ids=priors,
                    **self._history_filters(job, "funding_tx_id", pending=resend))
            except Exception as exc:  # noqa: BLE001
                raise WarpError(
                    f"wallet history scan failed ({exc}); refusing to fund blindly"
                ) from exc
            if existing:
                return _stay(
                    state={**resend, "funding_tx_id": existing,
                           "funding_amount": toll},
                    message="toll funding already in flight (dedupe hit)",
                )
            fee = self._chia_send_fee(p.unwrap_chia_fee_mojos, cost=_COST_XCH_SEND)
            self._require_wallet_synced("toll funding")
            self._require_spendable_xch(toll + fee, context="toll funding")
            self._wallet.log_in()
            resend = {**resend, **self._refresh_exclusions(
                job, tx_key="funding_tx_id", out_ph_hex=ph.hex(),
                out_amount=toll, pending=resend)}
            exclude = [c for c in ((resend if "funding_tx_id_exclude" in resend
                                   else job.state).get("funding_tx_id_exclude") or [])
                       if c]
            record = self._send_reserved(
                job, tx_key="funding_tx_id",
                reserve_patch={**resend, "funding_amount": toll},
                send=lambda: self._wallet.send_transaction(
                    1, toll, cu.encode_puzzle_hash(ph, net.chia_prefix),
                    fee_mojos=fee,
                    **({"exclude_coin_ids": exclude} if exclude else {}),
                ),
            )
            tx_id = (record.get("name") or record.get("transaction_id")
                     or _FUNDING_TX_UNKNOWN)
            from . import chia_send as cs

            return _stay(
                state={**resend, "funding_tx_id": tx_id, "funding_amount": toll,
                       "funding_tx_id_removals": list(cs.removal_ids(record)) or None,
                       "funding_tx_id_orphan_polls": 0, "funding_fee_mojos": fee,
                       "funding_tx_id_exclude": None, "funding_tx_id_wallet_absent": None},
                message=f"funded {toll} mojos to the toll coin (fee {fee})",
            )

        # The burn CAT (created by the cat_spend; unspent until our bundle).
        # [review] By the coin BURN_SENT pinned (burn_cat_coin_id, with its
        # parent), never the first same-amount coin at the shared puzzle:
        # burn_cat_ph is (net, receiver)-scoped, so an unrelated unwrap's
        # CAT can sit there, and a bundle that consumed it would complete
        # this job while its own CAT stayed stranded.  A pinned coin that is
        # not indexed yet, or already spent, waits (a landed bundle resolves
        # via the nonce check next tick).  Only a job pinned before coin
        # ids were recorded falls back to the parent / the bare scan.
        burn_cat = None
        burn_ph_hex = _hx(job.state["burn_cat_ph"])
        pinned_cat = _hx(job.state.get("burn_cat_coin_id") or "")
        pinned_parent = _hx(job.state.get("burn_cat_parent") or "")
        candidates = list(self._coinset.get_coin_records_by_puzzle_hash(burn_ph_hex))
        if pinned_cat:
            rec = self._coinset.get_coin_record_by_name(pinned_cat)
            if rec is None:
                # The by-name index may lag the puzzle-hash index: the same
                # coin, recognised by its computed name.
                for cand in candidates:
                    if cu.coin_name(
                            bytes.fromhex(_hx(cand.coin.parent_coin_info)),
                            bytes.fromhex(burn_ph_hex),
                            int(cand.coin.amount)).hex() == pinned_cat:
                        rec = cand
                        break
            if rec is None:
                raise WarpPending("the job's burn CAT is not indexed yet; awaiting it")
            if rec.spent:
                raise WarpPending(
                    "the job's burn CAT is spent; awaiting the bridging coin index "
                    "(a landed bundle resolves via the nonce check next tick)"
                )
            burn_cat = drivers.Coin(
                bytes.fromhex(_hx(rec.coin.parent_coin_info)),
                bytes.fromhex(burn_ph_hex),
                int(rec.coin.amount),
            )
        else:
            for rec in candidates:
                if int(rec.coin.amount) != amount or rec.spent:
                    continue
                if pinned_parent and _hx(rec.coin.parent_coin_info) != pinned_parent:
                    continue
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
            asset=self._job_asset(job),
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
            net, receiver, int(job.amount_mojos), asset=self._job_asset(job)
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
                and not self._can_self_relay(job)
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
                int(job.amount_mojos), asset=self._job_asset(job),
            ),
            bytes.fromhex(job.state["relay_sigs"]),
        )

    def _h_relaying(self, job: WarpJob) -> _Step:
        """Two-phase receiveMessage; '!nonce' means someone else delivered."""
        from . import evm

        net = self._net
        # [WARP-LOST-SEND] Delivered already: settlement resumes directly,
        # with no re-signing and no threshold check (see the external path).
        if job.state.get("delivered_by"):
            return self._unwrap_delivered(
                job, by=str(job.state["delivered_by"]), tx_hash=job.bridge_tx_hash)

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

        # [WARP-LOST-SEND] Delivered already (the job waits only for replaced
        # toll sends to settle): no relay prerequisite applies any more, and
        # a threshold change must not send a delivered message back to
        # signature collection.  Settlement resumes directly.
        if job.state.get("delivered_by"):
            return self._unwrap_delivered(
                job, by=str(job.state["delivered_by"]), tx_hash=job.bridge_tx_hash)

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

        if self._can_self_relay(job):
            return _advance(
                JobStatus.RELAYING,
                message="hot wallet now holds relay gas; switching to self-relay",
            )
        raise WarpPending(
            "message is live and deliverable; waiting for an altruistic "
            "relayer (or fund the hot wallet with ~0.0003 ETH on Base to "
            f"self-relay). Also claimable any time at {net.status_url}"
        )

    def _can_self_relay(self, job: WarpJob) -> bool:
        """[review] Whether an external-relay job should relay itself: the
        hot wallet holds the relay-gas floor AND the relay's worst-case gas
        at current fees -- plus, for an automatic ETH top-up, the milliETH
        ``withdraw()`` that turns its proceeds into gas. Short of that a
        self-relay would stall unfunded (or leave the proceeds stranded),
        so the job keeps waiting for an altruistic relayer. Unknown fees
        read as no."""
        from gui.services.basewallet import _MILLIETH_GAS_DEFAULT

        from .evm import _RELAY_GAS_DEFAULT

        eth = int(self._evm.get_eth_balance(self._hot_address))
        if eth < _MIN_GAS_WEI:
            return False
        try:
            fee = int(self._evm.get_fee_data().max_fee_per_gas)
        except Exception as exc:  # noqa: BLE001 -- fail closed: stay external
            _log.debug("self-relay check: Base fees unknown (%s)", exc)
            return False
        gas = int(_RELAY_GAS_DEFAULT)
        if job.state.get("rebalance_eth_target_wei"):
            gas += int(_MILLIETH_GAS_DEFAULT)
        return eth >= gas * fee

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
        if job.state.get("funding_tx_id_prior"):
            # [WARP-LOST-SEND] COMPLETED offers no Sweep: a replaced toll
            # send that could still land keeps the job open, and one that
            # landed is swept here first.
            from . import claim

            columns = {"bridge_tx_hash": tx_hash} if tx_hash else None
            # [review] Delivery is on disk BEFORE the fallible lookups below:
            # were they to keep failing until the state's deadline, Retry
            # would otherwise clear the mined relay and prepare another,
            # which can only revert "!nonce".
            if (job.state.get("delivered_by") != by
                    or (tx_hash and job.bridge_tx_hash != tx_hash)):
                self._store.update_job(job.id, expected_status=job.status,
                                       columns=columns,
                                       state_patch={"delivered_by": by})
            settled, note = self._prior_sends_settled(job)
            if not settled:
                sk = self._load_ephemeral_sk(job)
                extra = claim.find_security_coin(
                    self._coinset, sk, int(self._funded_amount(job)))
                if extra is not None:
                    _resolved, status = self._sweep_security(job, extra, sk)
                    return _stay(
                        columns=columns,
                        state={"delivered_by": by, "sweep_status": status},
                        message=(f"unwrap delivered ({by}); a replaced toll "
                                 f"send landed too -- {status}"),
                    )
                return _stay(
                    columns=columns, state={"delivered_by": by},
                    message=f"unwrap delivered ({by}); {note}",
                )
        return _advance(
            JobStatus.COMPLETED,
            columns={"bridge_tx_hash": tx_hash} if tx_hash else None,
            state={"delivered_by": by},
            message=(
                f"unwrap complete ({by}): {post_tip} "
                f"{self._job_asset(job).symbol} base units paid to "
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

        # [WARP-LOST-SEND] One fee per job, fixed on the first sweep: the
        # bundle is a pure function of (coin, destination, fee), so a later
        # Sweep re-pushes the SAME bundle (an idempotent "pending") instead
        # of a differently-priced twin the mempool would reject as a
        # conflict with the first.
        # [review] Per security coin: replaced funding sends can leave more
        # than one coin at the key, each swept on its own.  The same coin
        # re-pushes the identical bundle; another coin prices its own.
        cid = coin.name().hex()
        pins = dict(job.state.get("sweep_fee_by_coin") or {})
        pushed = dict(job.state.get("sweep_pushed_by_coin") or {})
        fee = pins.get(cid)
        if not isinstance(fee, int) or isinstance(fee, bool) or fee < 0:
            fee = self._sweep_fee_for(job)
            pins[cid] = int(fee)
            try:
                self._store.update_job(
                    job.id, expected_status=job.status,
                    state_patch={"sweep_fee_by_coin": pins},
                )
            except Exception as exc:  # noqa: BLE001 -- [review] then no push
                # Unrecorded, a later Sweep could reprice over a bundle a
                # node may hold.  Nothing is broadcast until the pin is on
                # disk.
                return False, f"could not record the sweep fee ({exc}); not swept"

        # [review] Recorded BEFORE the push: a push that times out may still
        # have reached a node, and a later definitive rejection must not
        # reprice over a bundle that could be pending there.  Only a push
        # that no earlier submission precedes may release the pin.
        # [review] The destination is resolved first: its wallet lookup can
        # fail without any bundle going out, and that must not read as a
        # possible broadcast (it would keep a rejected fee pinned for good).
        # And PERSISTED per coin before the first push: the bundle is a
        # function of (coin, destination, fee), so a destination re-resolved
        # after an engine rebuild (the wallet's receive address may have
        # advanced) would make a DIFFERENT bundle that conflicts with the
        # pending one instead of replaying it.
        dests = dict(job.state.get("sweep_destination_by_coin") or {})
        dest_hex = dests.get(cid)
        if isinstance(dest_hex, str) and len(dest_hex) == 64:
            destination_ph = bytes.fromhex(dest_hex)
        else:
            try:
                destination_ph = self._sweep_destination_ph()
            except Exception as exc:  # noqa: BLE001 -- nothing was submitted
                return False, f"sweep failed: {exc}"
            dests[cid] = _hx(destination_ph)
            try:
                self._store.update_job(job.id, expected_status=job.status,
                                       state_patch={"sweep_destination_by_coin": dests})
            except Exception as patch_exc:  # noqa: BLE001 -- then no push
                return False, (f"could not record the sweep destination "
                               f"({patch_exc}); not swept")
        prior_push = pushed.get(cid)
        if not prior_push:
            pushed[cid] = "uncertain"
            try:
                self._store.update_job(job.id, expected_status=job.status,
                                       state_patch={"sweep_pushed_by_coin": pushed})
            except Exception as patch_exc:  # noqa: BLE001 -- [review] then no push
                return False, (f"could not record the sweep submission "
                               f"({patch_exc}); not swept")
        try:
            kind, status = claim.build_and_push_sweep(
                self._coinset,
                self._net,
                security_coin=coin,
                destination_puzzle_hash=destination_ph,
                ephemeral_sk=sk,
                sweep_fee=int(fee),
            )
        except Exception as exc:  # noqa: BLE001 -- operator can re-sweep later
            text = str(exc)
            low = text.lower()
            # A DEFINITIVE fee rejection of a bundle no node ever accepted:
            # the pin is released so the next Sweep reprices from a fresh
            # estimate.  Once a push was accepted or its outcome is unknown,
            # the pin stays -- a repriced twin would only conflict with it.
            fee_rejected = ("request failed" not in low and any(
                t in low for t in ("invalid_fee", "fee_too_low", "low_fee",
                                   "fee too low")))
            if fee_rejected:
                # [review] A definitive rejection of the IDENTICAL bundle by
                # the node we push through proves it holds no copy (a copy
                # it held answers "pending"), and a copy that confirmed
                # shows as the coin spent -- checked first.  Only then is
                # the pin released; an earlier uncertain or accepted push
                # must not keep a bundle the node has since dropped pinned
                # at a fee it now rejects, or the coin is never swept.
                try:
                    if claim.security_coin_spent(self._coinset, coin.name()):
                        return True, f"{text}; the security coin is already spent"
                except Exception as probe_exc:  # noqa: BLE001 -- not proof
                    return False, (f"sweep rejected for its fee ({text}); the "
                                   f"coin's fate is unknown ({probe_exc}); "
                                   "run Sweep again")
                pins.pop(cid, None)
                pushed.pop(cid, None)
                try:
                    self._store.update_job(
                        job.id, expected_status=job.status,
                        state_patch={"sweep_fee_by_coin": pins,
                                     "sweep_pushed_by_coin": pushed or None})
                except Exception as patch_exc:  # noqa: BLE001
                    _log.warning("warp job %s: could not release the sweep fee (%s)",
                                 job.id, patch_exc)
                return False, (f"sweep rejected for its fee ({text}); it is "
                               "repriced on the next Sweep")
            return False, f"sweep failed: {text}"
        if kind in ("accepted", "pending") and pushed.get(cid) is not True:
            pushed[cid] = True
            try:
                self._store.update_job(job.id, expected_status=job.status,
                                       state_patch={"sweep_pushed_by_coin": pushed})
            except Exception as patch_exc:  # noqa: BLE001
                _log.warning("warp job %s: could not record the sweep push (%s)",
                             job.id, patch_exc)

        if kind == "conflict":
            # The coin is spent -- by our own claim, a prior sweep -- or a
            # pending spend of it is in the mempool (an earlier sweep that
            # has not mined).  Only an ON-CHAIN spend means nothing is left
            # to recover; a mempool conflict alone must not close the job
            # over a coin that could still be evicted unspent ([review]).
            try:
                if claim.security_coin_spent(self._coinset, coin.name()):
                    return True, status
            except Exception as exc:  # noqa: BLE001 -- unknown is not resolved
                return False, f"sweep conflicted ({status}); the coin's fate is unknown: {exc}"
            return False, (
                f"sweep conflicts with a pending spend of the coin ({status}); "
                "run Sweep again once it confirms"
            )

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
        # [review] The recorded send is judged, so the status names its fate
        # and the remedy (job 4's shape read "may not be indexed yet").
        tx_id = str(job.state.get("funding_tx_id") or "")
        if tx_id and tx_id != _FUNDING_TX_UNKNOWN:
            from . import chia_send as cs

            try:
                ph = claim.security_coin_puzzle_hash(self._load_ephemeral_sk(job)).hex()
                verdict, why, _ev = self._recorded_send_verdict(
                    job, tx_key="funding_tx_id", out_ph_hex=ph,
                    out_amount=int(self._funded_amount(job)), unique_output=True)
            except Exception as exc:  # noqa: BLE001 -- the generic text below
                verdict, why = None, str(exc)
            if verdict is cs.Verdict.DEAD:
                return False, (f"the recorded funding send can never confirm ({why}); "
                               "Retry sends it again")
            if verdict is cs.Verdict.ORPHAN:
                return False, (f"the recorded funding send is {why}; Retry sends it "
                               "again (its coins are excluded from the replacement)")
            if verdict is cs.Verdict.ALIVE:
                return False, (f"the recorded funding send is still in flight ({why}); "
                               "Retry resumes waiting for its coin")
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
        # Only what sits above the keep-on-Base floor is bridgeable.
        keep = max(0, int(getattr(p, "keep_usdc_micros", 0) or 0))
        if balance - keep < p.min_micros:
            return None
        job = self._store.create_job(
            net.name,
            status=JobStatus.AWAITING_DEPOSIT,
            state={"auto": True, **_asset_stamp(net.asset("USDC"), net.cat_decimals),
                   **self._binding()},
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

        A manual ``target_micros`` is a typed amount: bridged EXACTLY (the job
        waits until the balance covers it), not reduced by
        ``keep_usdc_on_base``, and refused here -- not silently clamped --
        when it exceeds the cap or is below one wUSDC.b mojo.
        """
        # None is the blank Bridge now; any number -- zero included -- is a
        # typed amount and must pass the checks below.
        exact = target_micros is not None and not automatic
        if exact:
            micros = int(target_micros)
            mojo_micros = 10 ** (self._net.usdc_decimals - self._net.cat_decimals)
            if micros % mojo_micros:
                raise WarpError(
                    "bridge amount must be a whole number of wUSDC.b mojos "
                    f"(multiples of {mojo_micros} micro-USDC, i.e. 0.001 USDC)"
                )
            # The bridge's tip is at least one mojo and must leave one, so a
            # single mojo can never bridge -- and a typed amount does not
            # shrink, so such a job would hold the slot forever.
            if micros < _MIN_BRIDGE_MOJOS * mojo_micros:
                raise WarpError(
                    f"bridge amount must be at least {_MIN_BRIDGE_MOJOS} "
                    f"wUSDC.b mojos ({_MIN_BRIDGE_MOJOS * mojo_micros} "
                    "micro-USDC): the bridge's tip takes at least one"
                )
            cap = int(self._params.max_micros or 0)
            if cap > 0 and micros > cap:
                raise WarpError(
                    f"bridge amount of {micros} micro-USDC exceeds "
                    f"warp.max_auto_bridge_usdc ({cap}); refusing rather than "
                    "clamping"
                )
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
                "auto": True, "rebalance": True,
                **_asset_stamp(self._net.asset("USDC"), self._net.cat_decimals), **self._binding()
            }
        else:
            state = {"manual": True, **_asset_stamp(self._net.asset("USDC"),
                            self._net.cat_decimals),
                     **self._binding()}
        if target_micros is not None:
            state["target_micros"] = int(target_micros)
        if exact:
            state["exact"] = True
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
        # [review] Abandon is also the exit for a job whose claim completed
        # or whose unwrap was delivered but whose replaced Chia sends could
        # not be ruled out; the record must say so, not that the message
        # was never claimed.
        unsettled = self._unsettled_attempts_note(job)
        if job.state.get("delivered_by"):
            detail = (f"the unwrap was delivered ({job.state['delivered_by']}); "
                      f"closed by the operator{unsettled}")
        elif job.bridge_nonce and job.state.get("third_party_claim"):
            detail = (f"a third party's claim paid the attested receiver (message "
                      f"nonce {job.bridge_nonce} is consumed); closed by the "
                      f"operator{unsettled}")
        elif job.bridge_nonce and self._claim_completed(job):
            detail = (f"the claim completed (final CAT coin "
                      f"{job.state.get('final_cat_coin_id')}); closed by the "
                      f"operator{unsettled}")
        elif job.bridge_nonce:
            detail = (
                f"message nonce {job.bridge_nonce} was never claimed and remains "
                f"claimable at {self._net.status_url}{unsettled}"
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
        # [review] A delivered unwrap or a landed claim is a completed bridge:
        # closed as COMPLETED (the engine's accounting books the inbound mint
        # from that event), with the abandonment on the record.
        done = bool(job.state.get("delivered_by")) or (
            bool(job.bridge_nonce) and self._claim_completed(job))
        self._store.update_job(
            job.id,
            status=JobStatus.COMPLETED if done else JobStatus.CANCELLED,
            # The job's own status, not FAILED: a binding-refused job is
            # abandonable from whatever state it is frozen in.
            expected_status=job.status,
            columns={"next_retry_at": None},
            state_patch={"abandoned": True, "abandon_recovery": recovery},
            event=("abandon", note, recovery),
        )
        return _job_dict(self._store.get_job(job.id))

    def _claim_completed(self, job: WarpJob) -> bool:
        """Whether the bridge completed: the job's final CAT coin is on chain
        (our claim landed), or a third party's claim paid the attested
        receiver (the persisted ``third_party_claim`` checkpoint -- that
        claim creates no final CAT coin of ours, yet the deposit is paid and
        the mint happened)."""
        if job.state.get("third_party_claim"):
            return True
        final = job.state.get("final_cat_coin_id")
        if not final:
            return False
        try:
            return self._coinset.get_coin_record_by_name(str(final)) is not None
        except Exception:  # noqa: BLE001 -- unknown is not "completed"
            return False

    def _unsettled_attempts_note(self, job: WarpJob) -> str:
        """The replaced Chia sends of *job* not proven dead, for the audit
        record of a close; "" when there are none."""
        parts = []
        for tx_key, label in (("funding_tx_id", "funding"), ("burn_tx_id", "cat_spend")):
            priors = [t for t in (job.state.get(f"{tx_key}_prior") or []) if t]
            done = _settled_ids(job.state, tx_key)
            open_ = [t for t in priors if t not in done]
            if open_:
                parts.append(f"{len(open_)} replaced {label} send(s) not ruled out: "
                             + ", ".join(t[:16] for t in open_))
            presumed = [t for t in (job.state.get(f"{tx_key}_prior_presumed") or []) if t]
            if presumed:
                parts.append(f"{len(presumed)} replaced {label} send(s) presumed gone "
                             "with coins still unspent (Sweep stays available): "
                             + ", ".join(t[:16] for t in presumed))
        return ("; " + "; ".join(parts)) if parts else ""

    def request_unwrap(
        self, amount_mojos: int, receiver: str, external_relay: bool = False,
        *, automatic: bool = False, rebalance_target_micros: int = 0,
        asset: str = "USDC", rebalance_eth_target_wei: int = 0,
    ) -> dict:
        """Operator-triggered unwrap: a wrapped CAT -> its ERC-20 at *receiver*.

        *asset* names the bridgeable asset: ``"USDC"`` (wUSDC.b -> USDC, the
        default and the legacy behaviour) or ``"milliETH"`` (wmilliETH.b ->
        milliETH, which the hot wallet can then unwrap to ETH for gas). Each
        asset has its own cap in its own units -- ``max_unwrap_usdc`` and
        ``max_unwrap_millieth`` -- and an unwrap is refused until that cap is
        stated.

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
        try:
            spec = self._net.asset(asset)
        except KeyError:
            raise WarpError(f"unknown unwrap asset {asset!r}") from None
        is_usdc = (spec.expected_asset_id.lower()
                   == (self._net.expected_asset_id or "").lower())
        if amount < 1:
            raise WarpError(
                f"unwrap amount must be at least 1 mojo (0.001 {spec.symbol})")
        if is_usdc:
            if p.max_unwrap_micros < 0:
                raise WarpError(
                    "warp.max_unwrap_usdc is not set; the unwrap cap must be "
                    "stated before any unwrap (use 'unlimited' to opt out "
                    "explicitly)"
                )
            cap_key, cap = "max_unwrap_usdc", p.max_unwrap_micros
        else:
            cap = int(getattr(p, "max_unwrap_millieth_mojos", -1))
            if cap < 0:
                raise WarpError(
                    "warp.max_unwrap_millieth is not set; the wmilliETH.b "
                    "unwrap cap must be stated before any wmilliETH.b unwrap "
                    "(use 'unlimited' to opt out explicitly)"
                )
            cap_key = "max_unwrap_millieth"
        base_units = amount * 10 ** (
            int(spec.erc20_decimals) - self._net.cat_decimals
        )
        # USDC's cap is in micro-USDC (base units); milliETH's in mojos.
        capped_value = base_units if is_usdc else amount
        if cap and capped_value > cap:
            raise WarpError(
                f"unwrap of {capped_value} {'micro-USDC' if is_usdc else 'mojos'} "
                f"exceeds warp.{cap_key} ({cap}); refusing rather than clamping"
            )
        job = self._store.create_job(
            self._net.name,
            status=JobStatus.UNWRAP_CHECKS,
            columns={"amount_mojos": amount, "amount_usdc_micros": base_units},
            state={
                "direction": "out",
                **_asset_stamp(spec, self._net.cat_decimals),
                "manual": not automatic,
                **({"rebalance": True} if automatic else {}),
                # Lets _h_unwrap_checks re-derive the still-needed amount
                # just before the commit point instead of trusting a
                # possibly-stale snapshot.
                **({"rebalance_target_micros": int(rebalance_target_micros)}
                   if automatic and rebalance_target_micros else {}),
                **({"rebalance_eth_target_wei": int(rebalance_eth_target_wei)}
                   if automatic and rebalance_eth_target_wei else {}),
                "receiver_evm": rec_bytes.hex(),
                "external_relay": bool(external_relay),
                **self._binding(),
            },
            event_message=(
                ("rebalance: automatic " if automatic else "")
                + f"unwrap requested: {amount} mojos of w{spec.symbol}.b -> {receiver}"
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
                        + ((" (the vanished send is cleared; it will be sent "
                            "again)")
                           if reset and target in (JobStatus.BURN_SENT,
                                                   JobStatus.FUNDING_CLAIM,
                                                   JobStatus.BURNING)
                           else (" (re-signing: the previous nonce is spent)"
                                 if reset else "")),
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
            if job.status not in _SWEEPABLE and not (
                    job.status == JobStatus.CANCELLED and _presumed_attempts(job.state)):
                # [review] A CANCELLED job that was closed over attempts
                # presumed gone keeps Sweep: their coins are still unspent,
                # and one that lands after all leaves XCH at the key.
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
                if not resolved and job.state.get("funding_tx_id_prior"):
                    # [review] Replaced sends: when the chain proves EVERY
                    # attempt dead and no coin of the funded amount exists,
                    # nothing was ever funded -- resolved, nothing to sweep.
                    settled, _note = self._prior_sends_settled(job)
                    if settled:
                        resolved, status = True, (
                            "every funding attempt is settled on chain (proven "
                            "dead, presumed gone, or landed and already spent) "
                            "and no coin of the funded amount remains; nothing "
                            "to sweep")
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
        # [WARP-LOST-SEND] A replaced funding send that could still land
        # keeps the job open: closed, its XCH would have no Sweep.
        settled, settled_note = (self._prior_sends_settled(job)
                                 if resolved else (True, ""))
        if resolved and not settled:
            status = f"{status}; {settled_note}"
        close = (resolved and settled and job.status == JobStatus.FAILED
                 and not outbound)
        # [review] A claim that landed is a completed bridge whatever kept the
        # job open afterwards: it closes as COMPLETED (the engine's bridge
        # accounting books the mint from that event), never CANCELLED.
        completed = close and self._claim_completed(job)
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
        if completed:
            status = f"{status}; " + (
                "a third party's claim had paid the attested receiver"
                if job.state.get("third_party_claim")
                else "the claim had completed (final CAT coin on chain)")
        self._store.update_job(
            job.id,
            status=(JobStatus.COMPLETED if completed else JobStatus.CANCELLED) if close else None,
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
            "keep_usdc_micros": int(getattr(self._params, "keep_usdc_micros", 0) or 0),
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

def _http_get_json(url: str, params: dict, headers: dict) -> Any:
    """GET *url* and parse JSON; raises on any transport or HTTP error."""
    import requests  # lazy: only when a real network call is made

    resp = requests.get(url, params=params, headers=headers, timeout=10)
    resp.raise_for_status()
    return resp.json()


#: Chia's TradeStatus values, for wallets that report the number.
_TRADE_STATUS_NAMES: Dict[int, str] = {
    0: "PENDING_ACCEPT", 1: "PENDING_CONFIRM", 2: "PENDING_CANCEL",
    3: "CANCELLED", 4: "CONFIRMED", 5: "FAILED",
}


def _trade_status_name(status: Any) -> str:
    """A trade record's status as its upper-case name, int or string."""
    if isinstance(status, int) and not isinstance(status, bool):
        return _TRADE_STATUS_NAMES.get(status, "")
    text = str(status or "").strip().upper()
    if text.isdigit():
        return _TRADE_STATUS_NAMES.get(int(text), "")
    return text


#: A submitted Dexie take the wallet still cannot read after this long is
#: settled as failed (its record is lost), so it cannot block every later buy.
_TAKE_UNKNOWN_FAIL_S: int = 3600
# [review] How long an unknown-outcome take keeps the engine's wallet
# protection before it is released as failed: a Chia transaction still
# unconfirmed after a day is gone from every mempool, and a take that did
# land shows up as CAT held (and as a readable trade record first).
_TAKE_UNKNOWN_RELEASE_S: int = 24 * 3600
# [review] A retired attempt with no recorded coins, no wallet record and no
# mempool item -- a send recorded before coins were kept whose record the
# wallet has since deleted, or one that never left the GUI -- can never be
# proven anything: there is nothing to watch.  Left as "not ruled out" it
# would keep the job open for good and the engine's cleanup guard with it.
# After this long absent from wallet and mempool it is presumed gone: no node
# holds a transaction that long without including it.
_UNOBSERVABLE_ATTEMPT_GRACE_S: int = 6 * 3600
# Transport failures that prove the request never reached the daemon (the
# connection was never made), so a reservation may be released.  A timeout or
# a reset AFTER the request went out is not among them.
_NEVER_CONNECTED_MARKERS: tuple = (
    "connection refused", "connectionrefused", "actively refused",
    "errno 111", "errno 10061", "winerror 10061", "connecttimeouterror",
    "failed to establish a new connection", "newconnectionerror",
    "name or service not known", "getaddrinfo failed", "nodename nor servname",
    "no route to host", "network is unreachable",
)


def _age_s(iso: str) -> float:
    """Seconds since ISO-8601 *iso*; 0 when it cannot be parsed."""
    from datetime import datetime, timezone

    try:
        then = datetime.fromisoformat(str(iso))
    except ValueError:
        return 0.0
    if then.tzinfo is None:
        then = then.replace(tzinfo=timezone.utc)
    return max(0.0, (datetime.now(timezone.utc) - then).total_seconds())


def _engine_xch_fee_reserve_mojos(config: Optional[dict]) -> int:
    """strategy.fee_reserve_xch (default 1.0) in mojos: the XCH the engine
    keeps for its own fees, which a Dexie top-up buy must never spend."""
    strategy = (config or {}).get("strategy") or {}
    raw = strategy.get("fee_reserve_xch", 1.0) if isinstance(strategy, dict) else 1.0
    try:
        value = float(raw)
    except (TypeError, ValueError):
        value = 1.0
    if not 0.0 <= value < float("inf"):   # also catches NaN
        value = 1.0
    return int(round(value * 10 ** 12))


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
    # [WARP-LOST-SEND] Expanded as the engine expands its own copy of this
    # path (config.cpp expand_env_vars + expand_tilde): ``${VAR}`` and
    # ``${VAR:-default}`` references, then a leading ``~``.  The two
    # processes must open the SAME file, or the engine's stuck-transaction
    # guard protects a database no job is written to.
    text = _expand_config_path(
        str(warp.get("jobs_db") or (Path("data") / jobs.DB_FILENAME)))
    raw = Path(text)
    if raw.is_absolute() or anchor is None:
        return str(raw)
    return str(Path(anchor) / raw)


_ENV_REF = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}")


def _expand_config_path(text: str) -> str:
    """``${VAR}`` / ``${VAR:-default}`` from the environment, then ``~`` to
    the home directory -- the engine's reading of a configured path.  An
    unset ``${VAR}`` with no default is a config error, as it is there."""
    import os

    def _ref(m: "re.Match[str]") -> str:
        value = os.environ.get(m.group(1))
        if value is None:
            if m.group(2) is not None:
                return m.group(2)
            raise WarpError(
                f"warp.jobs_db references ${{{m.group(1)}}}, which is not set")
        return value

    expanded = _ENV_REF.sub(_ref, text)
    if expanded.startswith("~"):
        home = os.environ.get("HOME") or os.environ.get("USERPROFILE")
        if home:
            return home + expanded[1:]
        return os.path.expanduser(expanded)
    return expanded


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
        # wallet_action), and the anchor of every config-relative path
        # (_config_home).  Set before moveToThread; changed afterwards only
        # through the set_secrets_path slot, on this thread.
        self._secrets_path: Optional[Path] = secrets_path
        self._last_wallet_notice: str = ""
        self._rebalance_params = rb.RebalanceParams()
        self._rebalance_error: str = ""
        self._rebalance_next_ok: float = 0.0
        self._rebalance_last: str = ""
        # [MILLIETH-TOPUP] Latched once a MANUAL wmilliETH.b unwrap has
        # completed: the automatic top-up never runs an asset path the
        # operator has not watched succeed end to end.
        # (job store, asset) a manual wmilliETH.b unwrap was proven for.
        self._millieth_unwrap_proven: Optional[tuple] = None
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

    def set_secrets_path(self, path: object) -> None:
        """Follow a config switch (EngineBridge.update_config_path).

        [review] The secrets path anchors the jobs database (_config_home):
        left at the directory fixed at construction, a switched config
        would have this worker recording live sends in the OLD directory's
        store while the C++ engine, restarted on the new config, reads an
        empty store in the new one -- and cleans up a wallet with a warp
        send in flight.  The engine is rebuilt against the new directory's
        store on the next tick.  Delivered on the worker thread.
        """
        new = Path(str(path)) if path else None
        if new == self._secrets_path:
            return
        _log.info("warp: config home changed (%s -> %s); reopening the job store there",
                  self._secrets_path.parent if self._secrets_path else None,
                  new.parent if new else None)
        self._secrets_path = new
        self._drop_engine()
        self._engine_error = None

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

        snap = self._guarded(_run)
        if self._engine is None:
            # [review] Submitted Dexie takes settle whatever the switch that
            # authorises NEW warp work says: with warp disabled (or the
            # engine blocked) a take that confirms must still reach the
            # store, or the engine's ledger never books it.
            self._settle_takes_without_engine()
        self.snapshot_ready.emit(snap)

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
        # owns automatic bridging outright -- and with no USDC band it
        # still honours warp.auto_bridge, as its lowest-priority leg
        # (rb.plan's auto_bridge_floor_micros), AFTER any gas action.
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
        # [MILLIETH-TOPUP] Dexie takes settle whatever the rebalancer's
        # state: switching the source off must not strand a submitted take
        # that the engine would then never book.
        self._settle_takes_quietly(engine)
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
            eth_now = int(hot.get("eth_wei") or 0)
            # [MILLIETH-TOPUP] Chia-side sources for the ETH band. A Dexie
            # buy still confirming blocks another one; spendable wmilliETH.b
            # is read only when the band is actually breached.
            src = p.wmillieth
            buy_in_flight = (
                bool(engine._store.list_takes(status=jobs.TAKE_SUBMITTED,
                                              limit=1)
                     or engine._store.list_takes(status=jobs.TAKE_SUBMITTING,
                                                 limit=1))
                if src is not None and src.enabled else False
            )
            # Only once Base has no milliETH left to withdraw: that is the
            # planner's first, free source, and a Chia RPC failure must not
            # cost it its turn.
            eth_low = p.eth is not None and eth_now < p.eth.low
            # [review] The smallest withdraw and burn worth making at live
            # gas; read only when the band is breached.
            min_units, min_mojos = (
                self._millieth_topup_floor(engine) if eth_low else (1, 1))
            chia_millieth, chia_millieth_held = (
                self._chia_millieth_balances(engine)
                if src is not None and src.enabled and eth_low
                and int(hot.get("millieth_units") or 0) < min_units
                else (0, 0)
            )
            action = rb.plan(
                params=p,
                eth_wei=int(hot.get("eth_wei") or 0),
                usdc_micros=int(hot.get("usdc_micros") or 0),
                millieth_units=int(hot.get("millieth_units") or 0),
                max_bridge_micros=int(ep.max_micros or 0),
                max_unwrap_micros=int(ep.max_unwrap_micros),
                min_bridge_micros=int(ep.min_micros or 0),
                # warp.auto_bridge without a USDC band: the legacy auto
                # bridge, run here so a gas refuel always goes first.
                auto_bridge_floor_micros=(
                    int(getattr(ep, "keep_usdc_micros", 0) or 0)
                    if getattr(ep, "auto_bridge", False) else None
                ),
                # And a USDC band never plans to bridge into the keep floor.
                keep_usdc_micros=int(getattr(ep, "keep_usdc_micros", 0) or 0),
                chia_millieth_mojos=chia_millieth,
                max_unwrap_millieth_mojos=int(
                    getattr(ep, "max_unwrap_millieth_mojos", -1)),
                min_millieth_units=min_units,
                min_wmillieth_mojos=min_mojos,
            )
            if action is None:
                cap = int(getattr(ep, "max_unwrap_millieth_mojos", -1))
                if (eth_low and src is not None and src.enabled
                        and 0 < cap < min_mojos):
                    self._rebalance_last = (
                        f"rebalance waiting: warp.max_unwrap_millieth ({cap} "
                        f"mojos) is below the {min_mojos} mojos a top-up must "
                        "burn to deliver more ETH than its gas costs at "
                        "current Base fees"
                    )
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
            if action.kind in ("unwrap_wmillieth", "buy_wmillieth") and (
                    not self._millieth_unwrap_is_proven(engine)):
                self._rebalance_last = (
                    "rebalance skipped: the ETH top-up from wmilliETH.b starts "
                    "after one manual wmilliETH.b unwrap has completed (Warp "
                    "-> Unwrap, asset milliETH)"
                )
                return
            if action.kind in ("unwrap_wmillieth", "buy_wmillieth") and (
                    not self._can_withdraw_millieth(engine, eth_now)):
                # [review] The burn delivers milliETH, not ETH: turning it into
                # gas takes one withdraw() the hot wallet itself must pay for.
                # External relay covers the bridge's delivery, not that. Below
                # its worst-case cost the top-up would burn wmilliETH.b and
                # strand the proceeds, so it waits for the wallet to be funded.
                self._rebalance_last = (
                    "rebalance skipped: the hot wallet's ETH cannot pay the "
                    "milliETH withdraw() the top-up would need; send it a "
                    "little ETH first"
                )
                return
            if action.kind == "buy_wmillieth" and buy_in_flight:
                self._rebalance_last = (
                    "rebalance waiting: the last Dexie wmilliETH.b buy has "
                    "not confirmed yet"
                )
                return
            if (action.kind == "buy_wmillieth"
                    and chia_millieth_held >= action.amount):
                # Enough held, none of it spendable: locked in the engine's
                # open offers (its gas reserve keeps reserve coins out of
                # them, so this is a reserve set below the need). It frees up
                # as the offers turn over -- buying more would only add to it.
                self._rebalance_last = (
                    "rebalance waiting: wmilliETH.b is held but locked in "
                    "open offers; not buying more on Dexie"
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
            elif action.kind == "unwrap_wmillieth":
                # External relay: the top-up must work with too little Base
                # gas to relay (it self-relays whenever gas is there).
                engine.request_unwrap(
                    mojos, engine._hot_address, True, automatic=True,
                    asset="milliETH",
                    rebalance_eth_target_wei=p.eth.target if p.eth else 0,
                )
            elif action.kind == "buy_wmillieth":
                reason = self._dexie_buy(engine, action.amount, p.wmillieth,
                                         min_mojos=min_mojos)
            self._rebalance_last = reason
        except Exception as exc:  # noqa: BLE001 -- banner + cooldown, never a crash
            # Cooldown on ANY failure, including before the plan (e.g.
            # garbage in the hot cache): a broken consult must not retry
            # and log every tick.
            self._rebalance_next_ok = time.monotonic() + p.cooldown_s
            self._rebalance_last = f"rebalance failed: {exc}"
            _log.warning("rebalance action failed: %s", exc)

    # -- [MILLIETH-TOPUP] Chia-side ETH sources ----------------------------- #

    def _chia_millieth_spendable(self, engine: "WarpEngine") -> int:
        """Spendable wmilliETH.b mojos (see :meth:`_chia_millieth_balances`)."""
        return self._chia_millieth_balances(engine)[0]

    def _chia_millieth_balances(self, engine: "WarpEngine") -> tuple:
        """(spendable, confirmed) wmilliETH.b mojos; (0, 0) only when no
        such CAT wallet exists.

        Spendable, not confirmed: coins the engine has locked into offers are
        not ours to burn (the unwrap's own G5 gate would pend on them). Every
        other failure raises -- two matching wallets, or an RPC that did not
        answer -- because an unknown balance must not read as zero and send
        the planner to Dexie for coins already held.
        """
        spec = engine._net.asset("milliETH")
        matches = engine._cat_wallet_ids(spec.expected_asset_id)
        if not matches:
            return 0, 0           # never held any: the wallet has none
        if len(matches) > 1:
            raise WarpTerminal(
                f"expected one wmilliETH.b CAT wallet, found {matches}")
        bal = engine._wallet.get_wallet_balance(matches[0])
        spendable = bal.get("spendable_balance")
        confirmed = bal.get("confirmed_wallet_balance")
        # Both, or neither: a reply missing one must not read as zero and
        # send the planner to Dexie (or past the held-but-locked guard).
        if spendable is None or confirmed is None:
            raise WarpError(
                "wmilliETH.b balance reply is missing spendable or confirmed; "
                "retrying")
        return max(0, int(spendable)), max(0, int(confirmed))

    def _millieth_topup_floor(self, engine: "WarpEngine") -> Tuple[int, int]:
        """:func:`_millieth_topup_minimums` at live Base fees.

        [review] Unknown fees RAISE (a banner and the cooldown, then a
        retry): with no known minimum the planner could withdraw a single
        base unit whose payout is less than the gas -- if the withdraw's own
        fee read then succeeded, its affordability check would let it
        through."""
        try:
            fee = int(engine._evm.get_fee_data().max_fee_per_gas)
        except Exception as exc:  # noqa: BLE001 -- surfaced, retried
            raise WarpError(
                f"Base fees unknown ({exc}); the ETH top-up waits until its "
                "smallest worthwhile size is known") from exc
        return _millieth_topup_minimums(fee, _c_wei_per_milli_unit(engine._net))

    def _can_withdraw_millieth(self, engine: "WarpEngine", eth_wei: int) -> bool:
        """Whether *eth_wei* covers a milliETH ``withdraw()``'s worst-case gas
        at current fees (the same bound BaseWallet.unwrap_millieth enforces).
        Unknown fees read as no: a burn it cannot finish must not start."""
        from gui.services.basewallet import _MILLIETH_GAS_DEFAULT

        try:
            fees = engine._evm.get_fee_data()
            worst = int(_MILLIETH_GAS_DEFAULT) * int(fees.max_fee_per_gas)
        except Exception as exc:  # noqa: BLE001 -- fail closed
            _log.debug("rebalance: Base fees unknown (%s)", exc)
            return False
        return int(eth_wei) >= worst

    def _millieth_unwrap_is_proven(self, engine: "WarpEngine") -> bool:
        """Whether a MANUAL wmilliETH.b unwrap has ever completed here.

        [review] The cached proof is scoped to the job store and asset it
        was read from: a config reload that points warp.jobs_db at another
        database (or a different asset) re-reads that history instead of
        inheriting a proof it does not contain.
        """
        tail = ":" + engine._net.asset("milliETH").expected_asset_id.lower()
        proof = self._millieth_unwrap_proven
        if (isinstance(proof, tuple) and proof[0] is engine._store
                and proof[1] == tail):
            return True
        for state in engine._store.completed_job_states():
            if (state.get("direction") == "out" and state.get("manual")
                    and str(state.get("asset_fingerprint") or "")
                    .lower().endswith(tail)):
                self._millieth_unwrap_proven = (engine._store, tail)
                return True
        return False

    def _settle_takes_without_engine(self) -> None:
        """Settle open Dexie takes with no warp engine built: just the job
        store and the Chia wallet, which is all settlement reads. Opens the
        store only when its file exists, and the wallet only when a take is
        still open. Contracted never to raise."""
        try:
            path = Path(_job_db_path(self._config, anchor=self._config_home()))
            if not path.is_file():
                return
            with WarpJobStore(str(path)) as store:
                # [review] "unknown" takes too: they settle for real, or are
                # released, only through this path when no engine is built.
                if not (store.list_takes(status=jobs.TAKE_SUBMITTED, limit=1)
                        or store.list_takes(status=jobs.TAKE_SUBMITTING,
                                            limit=1)
                        or store.list_takes(status=jobs.TAKE_UNKNOWN, limit=1)):
                    return
                from types import SimpleNamespace

                self._settle_dexie_takes(SimpleNamespace(
                    _store=store, _wallet=self._chia_wallet_client()))
        except Exception as exc:  # noqa: BLE001 -- a tick must not crash
            _log.debug("Dexie take settlement (no engine) failed: %s", exc)

    def _chia_wallet_client(self) -> Any:
        """The Chia wallet RPC client the config names."""
        from . import wallet as wallet_mod

        chia = self._config.get("chia") or {}
        return wallet_mod.WalletClient(
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

    def _settle_takes_quietly(self, engine: "WarpEngine") -> None:
        """:meth:`_settle_dexie_takes`, contracted never to raise."""
        if getattr(engine._store, "list_takes", None) is None:
            return
        try:
            self._settle_dexie_takes(engine)
        except Exception as exc:  # noqa: BLE001 -- a tick must not crash
            _log.debug("Dexie take settlement failed: %s", exc)

    def _settle_dexie_takes(self, engine: "WarpEngine") -> bool:
        """Settle submitted Dexie takes from the wallet's trade records.

        True while one is still unconfirmed (or unreadable): no second buy
        starts until the first is known to have landed or failed. A take the
        wallet still cannot read after ``_TAKE_UNKNOWN_FAIL_S`` settles as
        failed, so a lost trade record cannot stop every later buy.
        """
        in_flight = False
        # [review] EVERY open take (limit=-1 is SQLite's "no limit"): the
        # default page would let newer rows hide an older one from
        # settlement for good -- a confirmed purchase never booked.
        for take in engine._store.list_takes(status=jobs.TAKE_SUBMITTING, limit=-1):
            # The take RPC's outcome never reached the store (a lost
            # response, a crash): no wallet trade id to look up. It blocks
            # buys until _TAKE_UNKNOWN_FAIL_S, then moves to "unknown" -- by
            # then a buy that did land shows up as wmilliETH.b held, so the
            # planner no longer asks for one.  [review] Not "failed": its
            # transactions are not proven dead, and the engine's guard keeps
            # protecting its wallets until they are (_settle_unknown_takes).
            if _age_s(take.created_at) > _TAKE_UNKNOWN_FAIL_S:
                engine._store.settle_take(
                    take.trade_id, jobs.TAKE_UNKNOWN,
                    error="the take's outcome was never recorded")
                _log.warning("Dexie take of %s: outcome unknown after %ds; "
                             "no longer blocking buys (check the wallet)",
                             take.offer_id, _TAKE_UNKNOWN_FAIL_S)
            else:
                in_flight = True
        submitted = engine._store.list_takes(status=jobs.TAKE_SUBMITTED, limit=-1)
        if submitted:
            # [review] The configured key first, OUTSIDE the per-take handler:
            # after a restart the daemon may be logged out or on another key,
            # and that must defer settlement (this raises), never count
            # toward the timeout that settles a real trade as failed.
            engine._wallet.log_in()
        for take in submitted:
            try:
                record = engine._wallet.get_offer(take.trade_id)
            except Exception as exc:  # noqa: BLE001 -- unknown stays in flight
                if _age_s(take.created_at) > _TAKE_UNKNOWN_FAIL_S:
                    engine._store.settle_take(
                        take.trade_id, jobs.TAKE_UNKNOWN,
                        error=f"the wallet has no readable trade record ({exc})")
                    _log.warning("Dexie take %s: no trade record after %ds; "
                                 "no longer blocking buys", take.trade_id,
                                 _TAKE_UNKNOWN_FAIL_S)
                    continue
                _log.debug("Dexie take %s: status unknown (%s)",
                           take.trade_id, exc)
                in_flight = True
                continue
            status = _trade_status_name(record.get("status"))
            if status == "CONFIRMED":
                height = record.get("confirmed_at_index")
                engine._store.settle_take(
                    take.trade_id, jobs.TAKE_CONFIRMED,
                    confirmed_height=(int(height) if isinstance(height, int)
                                      and not isinstance(height, bool)
                                      and height > 0 else None))
                _log.info("Dexie take %s confirmed: %d wmilliETH.b mojos for "
                          "%d XCH mojos", take.trade_id, take.cat_mojos,
                          take.xch_mojos)
            elif status in ("FAILED", "CANCELLED"):
                engine._store.settle_take(
                    take.trade_id, jobs.TAKE_FAILED,
                    error=f"wallet reports {status}")
                _log.warning("Dexie take %s %s", take.trade_id, status.lower())
            else:
                in_flight = True
        self._settle_unknown_takes(engine)
        return in_flight

    def _settle_unknown_takes(self, engine: "WarpEngine") -> None:
        """[review] Takes the planner stopped waiting for are still open for
        the engine's guard: each is settled for real once its trade record
        reads CONFIRMED or FAILED, and released as failed only after
        ``_TAKE_UNKNOWN_RELEASE_S`` -- a Chia transaction unconfirmed for a
        day is gone from every mempool.  Never blocks a buy."""
        unknown = engine._store.list_takes(status=jobs.TAKE_UNKNOWN, limit=-1)
        if not unknown:
            return
        # [review] The configured key first, outside the per-take handler: a
        # lookup against another client's selected fingerprint reads as "no
        # record", and a login failure must defer settlement, never count
        # toward the release.
        engine._wallet.log_in()
        for take in unknown:
            status = None
            readable = False
            try:
                record = engine._wallet.get_offer(take.trade_id)
                status = _trade_status_name(record.get("status"))
                readable = True
            except Exception as exc:  # noqa: BLE001 -- still unknown
                _log.debug("Dexie take %s: still unknown (%s)", take.trade_id, exc)
            if status == "CONFIRMED":
                height = record.get("confirmed_at_index")
                engine._store.settle_take(
                    take.trade_id, jobs.TAKE_CONFIRMED,
                    confirmed_height=(int(height) if isinstance(height, int)
                                      and not isinstance(height, bool)
                                      and height > 0 else None))
                _log.info("Dexie take %s confirmed after all", take.trade_id)
            elif status in ("FAILED", "CANCELLED"):
                engine._store.settle_take(
                    take.trade_id, jobs.TAKE_FAILED,
                    error=f"wallet reports {status}")
            elif readable:
                # [review] A record that reads PENDING_* is positive evidence
                # the take is live: it stays protected however old it is,
                # until the wallet calls it confirmed or failed.
                _log.debug("Dexie take %s: wallet reports %s; still open",
                           take.trade_id, status)
            elif _age_s(take.created_at) > _TAKE_UNKNOWN_RELEASE_S:
                engine._store.settle_take(
                    take.trade_id, jobs.TAKE_FAILED,
                    error=(f"outcome unknown for {_TAKE_UNKNOWN_RELEASE_S // 3600}h; "
                           "released (an unconfirmed transaction that old is gone)"))
                _log.warning("Dexie take %s: unknown for %dh; released as failed",
                             take.trade_id, _TAKE_UNKNOWN_RELEASE_S // 3600)

    def _dexie_buy(self, engine: "WarpEngine", want_mojos: int,
                   src: "rb.WmilliethSource", *, min_mojos: int = 1) -> str:
        """Take at most ONE Dexie ask for wmilliETH.b; the banner text.

        Every refusal raises (banner + cooldown); finding no eligible ask is
        not an error and buys nothing. See dexie_buy.py for the checks.
        """
        from . import dexie_buy as dx

        asset_id = engine._net.asset("milliETH").expected_asset_id.lower()
        cg = self._config.get("coingecko") or {}
        api_key = str(cg.get("api_key") or "") if isinstance(cg, dict) else ""
        ref = dx.reference_xch_per_millieth(_http_get_json, api_key)
        if ref is None:
            raise WarpError(
                "no CoinGecko ETH and XCH prices to bound the Dexie price; "
                "not buying blind")
        dexie = self._config.get("dexie") or {}
        api_base = str((dexie.get("api_base") if isinstance(dexie, dict)
                        else None) or "https://api.dexie.space/v1")
        asks = dx.fetch_asks(_http_get_json, api_base, asset_id)
        engine._wallet.log_in()
        # [review] Never an offer already attempted (any outcome): its id is
        # the provisional key of that attempt's row, and a failed take of a
        # still-listed ask must not be retried blind.
        # ...nor one our wallet already refused to verify (a listing whose
        # id or amounts the offer file contradicts): it would be picked, and
        # refused, again on every cooldown.
        refused = getattr(self, "_dexie_refused", None)
        if refused is None:
            refused = self._dexie_refused = set()
        own = engine._wallet.get_my_offer_trade_ids() | (
            engine._store.take_offer_ids()) | refused
        # Max XCH per Buy bounds the whole debit: the ask AND our fee.
        # [review] So does what the wallet can spend above the engine's fee
        # reserve: a covering ask it cannot afford must not be picked over an
        # affordable partial one, only to be refused on every cooldown. The
        # final balance recheck below still runs.
        # [review] The floor is the larger of the engine's fee reserve and
        # what the unwrap of this very purchase will need (G10): a buy that
        # spent that too would leave the CAT stuck at the unwrap gate.
        keep = max(_engine_xch_fee_reserve_mojos(self._config),
                   engine._unwrap_xch_budget())
        spendable = int(
            engine._wallet.get_wallet_balance(1).get("spendable_balance") or 0)
        budget = max(0, min(src.max_spend_xch_mojos, spendable - keep)
                     - src.take_fee_mojos)
        ask = dx.pick_ask(
            asks, want_mojos=int(want_mojos), reference_xch_per_unit=ref,
            max_premium_pct=src.max_premium_pct,
            max_spend_xch_mojos=budget,
            own_trade_ids=own,
            # [review] Never an ask too small to be worth unwrapping.
            min_mojos=min_mojos)
        if ask is None:
            return (
                f"rebalance: no Dexie wmilliETH.b ask within "
                f"{src.max_premium_pct:g}% of CoinGecko ({ref:.4f} XCH each) "
                f"for at most {budget / 10 ** 12:g} XCH (Max XCH per Buy and "
                f"the spendable XCH above {keep / 10 ** 12:g} XCH -- the "
                "engine's fee reserve or the unwrap's XCH, whichever is "
                "larger -- less the take fee); nothing bought")
        try:
            dx.verify_offer(engine._wallet, ask, asset_id)
        except dx.DexieBuyError:
            refused.add(ask.trade_id)
            raise
        cost = dx.buy_cost_mojos(ask, src.take_fee_mojos)
        spendable = int(
            engine._wallet.get_wallet_balance(1).get("spendable_balance") or 0)
        if spendable - cost < keep:
            raise WarpError(
                f"a Dexie buy costing {cost / 10 ** 12:.6f} XCH would leave "
                f"{(spendable - cost) / 10 ** 12:.6f} XCH spendable, under the "
                f"{keep / 10 ** 12:g} XCH kept back (the engine's fee reserve "
                "or the unwrap's XCH, whichever is larger)")
        # [review] The intent is recorded BEFORE the irreversible RPC, under
        # the maker's offer id: a lost response, a crash or a failed write
        # after the take must still leave a row that blocks a second buy
        # and that settlement resolves. If this write fails, nothing is taken.
        engine._store.record_take(
            trade_id=ask.trade_id, offer_id=ask.trade_id, asset_id=asset_id,
            cat_mojos=ask.cat_mojos, xch_mojos=ask.xch_mojos,
            fee_mojos=src.take_fee_mojos, status=jobs.TAKE_SUBMITTING)
        record = engine._wallet.take_offer(ask.offer,
                                           fee_mojos=src.take_fee_mojos)
        trade_id = str(record.get("trade_id") or "")
        if trade_id:
            try:
                engine._store.take_submitted(ask.trade_id, trade_id)
            except Exception as exc:  # noqa: BLE001 -- the take happened
                _log.error("Dexie take of %s accepted as %s but not marked "
                           "submitted (%s); it settles as unknown",
                           ask.trade_id, trade_id, exc)
        else:
            _log.warning("Dexie take of %s accepted without a trade id; it "
                         "settles as unknown", ask.trade_id)
        return (
            f"rebalance: took Dexie offer {ask.dexie_id or ask.trade_id[:12]}: "
            f"{ask.cat_mojos / 1000:g} wmilliETH.b for "
            f"{ask.xch_mojos / 10 ** 12:.6f} XCH ({ask.xch_per_unit:.4f} XCH "
            f"each; CoinGecko {ref:.4f})")

    @Slot(object)
    def request_bridge(self, target_micros: object) -> None:
        self.snapshot_ready.emit(
            self._guarded(lambda engine: engine.request_bridge(target_micros))
        )

    @Slot(int, str, bool, str)
    def request_unwrap(
        self, amount_mojos: int, receiver: str, external_relay: bool = False,
        asset: str = "USDC",
    ) -> None:
        self.snapshot_ready.emit(
            self._guarded(
                lambda engine: engine.request_unwrap(
                    int(amount_mojos), receiver, bool(external_relay),
                    asset=str(asset or "USDC"),
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
        notice: Optional[str] = None
        try:
            engine = self._ensure_engine()
            if engine is not None:
                result = fn(engine)
                # [review] A Sweep that did not close the job says why only
                # in state.sweep_status: surface it as a notice, as an error
                # would be, or the operator sees a click do nothing.
                if isinstance(result, dict) and result.get("sweep_status") \
                        and result.get("status") == JobStatus.FAILED:
                    notice = f"Sweep: {result['sweep_status']}"
        except Exception as exc:  # noqa: BLE001 -- never kill the worker
            action_error = str(exc)
            _log.warning("warp worker step failed: %s", exc)
        snap = self._snapshot_or_error()
        if action_error:
            snap["action_error"] = action_error
        elif notice:
            snap["action_notice"] = notice
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

        wallet_client = self._chia_wallet_client()

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
    _trigger_unwrap = Signal(int, str, bool, str)
    _trigger_action = Signal(int, str)
    _trigger_wallet = Signal(str, dict)
    _trigger_secrets = Signal(object)

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
        self._secrets_path: Optional[Path] = Path(secrets_path) if secrets_path else None

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
        self._trigger_secrets.connect(self._worker.set_secrets_path)

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

    @property
    def secrets_path(self) -> Optional[Path]:
        """The secrets file beside the active config: the anchor of the
        worker's config-relative paths (the jobs database among them)."""
        return self._secrets_path

    def set_secrets_path(self, path: Optional[Path]) -> None:
        """[review] Follow a config switch so the worker's job store moves
        with the config, as the engine's does (EngineBridge.update_config_path)."""
        new = Path(path) if path else None
        if new == self._secrets_path:
            return
        self._secrets_path = new
        self._trigger_secrets.emit(new)

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
        self, amount_mojos: int, receiver: str, external_relay: bool = False,
        asset: str = "USDC",
    ) -> None:
        if not self._thread.isRunning():
            self.start()
        self._trigger_unwrap.emit(
            int(amount_mojos), str(receiver), bool(external_relay),
            str(asset or "USDC"),
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
