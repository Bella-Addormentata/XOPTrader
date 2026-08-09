"""Altruistic relayer tests: goldens against the real delivered message,
and the sweeper's safety rails (grace, preflight, skip-list, budget)."""

from __future__ import annotations

import json
import pathlib
from types import SimpleNamespace

import pytest

pytest.importorskip("clvm")
pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import evm as evm_mod  # noqa: E402
from gui.services.warp import relayer  # noqa: E402
from gui.services.warp.nostr import EcdsaSigResult  # noqa: E402

NET = C.MAINNET
FX = json.loads(
    (pathlib.Path(__file__).parent / "fixtures_unwrap.json").read_text()
)

NOW = 1_786_000_000.0


def _watcher_msg(**over):
    m = {
        "nonce": "cd" * 32,
        "source": NET.burn_puzzle_hash,
        "destination": "ab" * 20,
        "contents": ["11" * 32, "22" * 32, "0000001388".rjust(64, "0")],
        "status": "sent",
        "destination_transaction_hash": None,
        "source_timestamp": NOW - 7200,          # stuck 2h
    }
    m.update(over)
    return m


class FakeRelayEvm:
    def __init__(self) -> None:
        self.preflight_error: Exception | None = None
        self.broadcast_error: Exception | None = None
        self.sent: list = []

    def verify_eip712_domain(self):
        return bytes.fromhex(NET.eip712_domain_separator)

    def get_signature_threshold(self):
        return 6

    def _eth_call(self, to, data):
        if self.preflight_error is not None:
            raise self.preflight_error
        return b""

    def prepare_relay(self, *, owner, calldata, nonce=None, fees=None):
        return SimpleNamespace(
            kind="relay", data=calldata, nonce=nonce or 1,
            gas=150_000, max_fee_per_gas=1_000_000,
            max_priority_fee_per_gas=100_000,
        )

    def send_raw_transaction(self, raw):
        if self.broadcast_error is not None:
            raise self.broadcast_error
        self.sent.append(bytes(raw))
        return "0x" + "cd" * 32


class FakeCollector:
    """Returns the REAL six signatures for the fixture's digest, keyed by
    their recovered addresses -- or a shortfall when told to."""

    def __init__(self, *, short: bool = False) -> None:
        self.short = short

    def collect_ecdsa(self, *, nonce, digest, threshold, have=None,
                      deadline_s=20.0, relay_offset=0):
        if self.short:
            return EcdsaSigResult(collected={}, threshold=threshold)
        from eth_keys import keys

        packed = bytes.fromhex(FX["sigs_packed"])
        collected = {}
        for i in range(6):
            chunk = packed[i * 65:(i + 1) * 65]
            sig = keys.Signature(vrs=(chunk[0] - 27,
                                      int.from_bytes(chunk[1:33], "big"),
                                      int.from_bytes(chunk[33:65], "big")))
            addr = sig.recover_public_key_from_msg_hash(digest).to_address()
            collected[addr] = chunk
        return EcdsaSigResult(collected=collected, threshold=threshold)


def _key():
    return SimpleNamespace(address="0x" + "ab" * 20, private_key=b"\x11" * 32)


def _relayer(watcher_msgs, *, evm=None, collector=None, **over):
    return relayer.AltruisticRelayer(
        net=NET,
        evm_client=evm or FakeRelayEvm(),
        collector=collector or FakeCollector(),
        watcher_fetch=lambda path: watcher_msgs,
        evm_key=_key(),
        clock=lambda: NOW,
        **over,
    )


# --------------------------------------------------------------------------- #
# Golden: the real delivered message rebuilds byte-for-byte.
# --------------------------------------------------------------------------- #

def test_build_relay_calldata_reproduces_the_real_delivery():
    msg = relayer.StuckMessage(
        nonce=bytes.fromhex(FX["nonce"]),
        source=bytes.fromhex(FX["source"]),
        destination=FX["destination"],
        contents=[bytes.fromhex(c) for c in FX["contents"]],
        age_s=7200,
    )
    calldata = relayer.build_relay_calldata(
        FakeRelayEvm(), FakeCollector(), NET, msg
    )
    assert calldata.hex() == FX["calldata"]


# --------------------------------------------------------------------------- #
# Discovery: grace period, delivered-exclusion, ordering.
# --------------------------------------------------------------------------- #

def test_fetch_stuck_messages_filters_and_orders():
    msgs = [
        _watcher_msg(nonce="aa" * 32, source_timestamp=NOW - 60),       # too fresh
        _watcher_msg(nonce="bb" * 32, source_timestamp=NOW - 7200),
        _watcher_msg(nonce="cc" * 32, source_timestamp=NOW - 90000),    # oldest
        _watcher_msg(nonce="dd" * 32,
                     destination_transaction_hash="0x" + "12" * 32,
                     status="received"),                                # delivered
    ]
    out = relayer.fetch_stuck_messages(
        lambda p: msgs, grace_s=1800.0, now=NOW
    )
    assert [m.nonce.hex()[:2] for m in out] == ["cc", "bb"], \
        "oldest first; fresh and delivered excluded"


# --------------------------------------------------------------------------- #
# The sweeper's rails.
# --------------------------------------------------------------------------- #

def test_signature_shortfall_is_skipped_without_spending():
    ev = FakeRelayEvm()
    r = _relayer([_watcher_msg()], evm=ev, collector=FakeCollector(short=True))
    reports = r.sweep()
    assert "signatures on Nostr" in reports[0]["action"]
    assert ev.sent == []


def test_preflight_failure_never_broadcasts_and_eventually_skiplists():
    ev = FakeRelayEvm()
    ev.preflight_error = evm_mod.EvmRpcError("execution reverted: whatever")
    r = _relayer([_watcher_msg()], evm=ev)
    for i in range(relayer.SKIP_AFTER_FAILURES):
        reports = r.sweep()
        assert "would revert" in reports[0]["action"]
    assert ev.sent == []
    assert r.sweep()[0]["action"] == "skiplisted"


def test_already_delivered_preflight_is_not_penalised():
    ev = FakeRelayEvm()
    ev.preflight_error = evm_mod.EvmRpcError("execution reverted: !nonce")
    r = _relayer([_watcher_msg()], evm=ev)
    for _ in range(relayer.SKIP_AFTER_FAILURES + 1):
        assert r.sweep()[0]["action"] == "preflight: already delivered"


def test_budget_cap_blocks_the_broadcast():
    ev = FakeRelayEvm()
    r = _relayer([_watcher_msg()], evm=ev, daily_gas_budget_wei=1)
    assert r.sweep()[0]["action"] == "budget exhausted"
    assert ev.sent == []


def test_a_lost_race_is_reported_not_penalised(monkeypatch):
    ev = FakeRelayEvm()
    ev.broadcast_error = evm_mod.EvmRpcError("execution reverted: !nonce")
    r = _relayer([_watcher_msg()], evm=ev)
    monkeypatch.setattr(
        evm_mod, "sign_tx",
        lambda u, k: SimpleNamespace(raw=b"\x02\x01", tx_hash="0x" + "ee" * 32,
                                     nonce=1),
    )
    assert r.sweep()[0]["action"] == "raced: delivered by someone else"
    assert r._failures == {}


def test_rehearsal_signs_but_never_broadcasts():
    ev = FakeRelayEvm()
    r = _relayer([_watcher_msg()], evm=ev)

    import unittest.mock as mock

    with mock.patch.object(
        evm_mod, "sign_tx",
        return_value=SimpleNamespace(raw=b"\x02\x01", tx_hash="0x" + "ee" * 32,
                                     nonce=1),
    ):
        reports = r.sweep(broadcast=False)
    assert reports[0]["action"] == "signed, not broadcast (rehearsal)"
    assert reports[0]["tx_hash"] == "0x" + "ee" * 32
    assert ev.sent == []


def test_a_successful_relay_spends_budget(monkeypatch):
    ev = FakeRelayEvm()
    r = _relayer([_watcher_msg()], evm=ev)
    monkeypatch.setattr(
        evm_mod, "sign_tx",
        lambda u, k: SimpleNamespace(raw=b"\x02\x01", tx_hash="0x" + "ee" * 32,
                                     nonce=1),
    )
    assert r.sweep()[0]["action"] == "relayed"
    assert len(ev.sent) == 1
    assert r._spent_today_wei == 150_000 * 1_000_000


# --------------------------------------------------------------------------- #
# The unforgeable altruism signal.
# --------------------------------------------------------------------------- #

def test_recent_third_party_relays_derives_evidence_correctly():
    receiver_hex = "22" * 20
    msgs = [
        # Delivered promptly -> owner, not altruism.
        _watcher_msg(nonce="a1" * 32, status="received",
                     destination_transaction_hash="0x" + "01" * 32,
                     source_timestamp=NOW - 7200,
                     destination_timestamp=NOW - 7100),
        # Stuck 3h then delivered by a stranger -> altruism.
        _watcher_msg(nonce="a2" * 32, status="received",
                     destination_transaction_hash="0x" + "02" * 32,
                     contents=["11" * 32, "00" * 12 + receiver_hex, "05" * 32],
                     source_timestamp=NOW - 20000,
                     destination_timestamp=NOW - 9000),
        # Stuck then delivered by the RECEIVER -> self-rescue, not altruism.
        _watcher_msg(nonce="a3" * 32, status="received",
                     destination_transaction_hash="0x" + "03" * 32,
                     contents=["11" * 32, "00" * 12 + "ab" * 20, "05" * 32],
                     source_timestamp=NOW - 20000,
                     destination_timestamp=NOW - 9000),
    ]

    class Ev:
        def _call(self, method, params):
            return {"from": "0x" + ("ab" * 20 if params[0].endswith("03" * 32)
                                    else "99" * 20)}

    out = relayer.recent_third_party_relays(lambda p: msgs, Ev(), grace_s=1800)
    assert [o["nonce"][:2] for o in out] == ["a2"]
    assert out[0]["relayer"] == "0x" + "99" * 20
    assert out[0]["stuck_for_s"] == 11000
