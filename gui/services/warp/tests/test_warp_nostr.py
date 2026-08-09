"""Offline tests for the warp.green Nostr signature-collection client.

No network. The pure codec/mapping/filter tests need only the bech32m helpers;
the acceptance-gate tests ``importorskip`` ``chia_rs`` and stand up a synthetic
:class:`WarpNet` whose validator slot holds a throwaway BLS keypair, so a real
signature can be produced and verified without any mainnet secret. The collector
tests inject a ``FakeFetcher`` (mirroring the ``FakeCaller`` idiom in
``test_warp_evm.py``) and a stubbed clock, so relay sweeps are deterministic.
"""

from __future__ import annotations

import dataclasses

import pytest

pytest.importorskip("clvm")  # nostr imports clvm_utils, which imports clvm

from gui.services.warp import clvm_utils as cu  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import nostr  # noqa: E402

NET = C.MAINNET


# --------------------------------------------------------------------------- #
# Synthetic-validator + event helpers (BLS-dependent, used by the gate tests).
# --------------------------------------------------------------------------- #

def _sk(index: int):
    from chia_rs import AugSchemeMPL

    return AugSchemeMPL.key_gen(bytes([index + 1] * 32))


def _nostr_hex(index: int) -> str:
    return bytes([0xA0 + index] * 32).hex()


def _net_with(vals: dict):
    """MAINNET with ``{index: (sk, nostr_hex)}`` slotted into the validator sets."""
    bls = list(NET.validator_bls_keys)
    nos = list(NET.nostr_validator_keys)
    for i, (sk, nostr_hex) in vals.items():
        bls[i] = bytes(sk.get_g1()).hex()
        nos[i] = nostr_hex
    return dataclasses.replace(
        NET, validator_bls_keys=tuple(bls), nostr_validator_keys=tuple(nos)
    )


def _signed_event(sk, nostr_hex, digest, routing_tag, coin_tag) -> dict:
    from chia_rs import AugSchemeMPL

    sig = AugSchemeMPL.sign(sk, digest)
    return {
        "kind": nostr.SIG_EVENT_KIND,
        "pubkey": nostr_hex,
        "tags": [["r", routing_tag], ["c", coin_tag]],
        "content": nostr.encode_sig_content(bytes(sig)),
    }


class FakeFetcher:
    """Returns canned events per relay url and records every call."""

    def __init__(self, by_relay: dict) -> None:
        self.by_relay = by_relay
        self.calls: list = []

    def __call__(self, relay: str, filt: dict, timeout: float):
        self.calls.append((relay, filt, timeout))
        return list(self.by_relay.get(relay, []))


# --------------------------------------------------------------------------- #
# bech32m tag / content codecs.
# --------------------------------------------------------------------------- #

def test_routing_tag_encodes_source_dest_nonce():
    nonce = bytes.fromhex("11" * 32)
    hrp, payload = cu.bech32m_decode_bytes(
        nostr.encode_routing_tag(b"bse", b"xch", nonce), max_length=200
    )
    assert hrp == nostr.HRP_ROUTING
    assert payload == b"bse" + b"xch" + nonce
    assert len(payload) == 3 + 3 + 32


def test_routing_tag_for_uses_net_source_and_xch():
    nonce = bytes.fromhex("00" * 31 + "2a")
    hrp, payload = cu.bech32m_decode_bytes(nostr.routing_tag_for(NET, nonce), max_length=200)
    assert hrp == nostr.HRP_ROUTING
    assert payload == NET.source_chain.encode() + b"xch" + nonce


def test_coin_tag_round_trips():
    coin = bytes.fromhex("33" * 32)
    hrp, payload = cu.bech32m_decode_bytes(nostr.coin_tag_for(coin), max_length=200)
    assert hrp == nostr.HRP_COIN and payload == coin


def test_sig_content_round_trips_past_bech32_cap():
    sig = bytes(range(96))
    encoded = nostr.encode_sig_content(sig)
    # 96 bytes -> ~162 chars, which the default 90-char bech32m cap would reject.
    assert len(encoded) > 90
    assert nostr.decode_sig_content(encoded) == sig


def test_decode_sig_content_rejects_wrong_hrp():
    with pytest.raises(nostr.NostrError):
        nostr.decode_sig_content(cu.bech32m_encode_bytes("r", bytes(96)))


def test_decode_sig_content_rejects_wrong_length():
    with pytest.raises(nostr.NostrError):
        nostr.decode_sig_content(cu.bech32m_encode_bytes(nostr.HRP_SIG, bytes(64)))


def test_decode_sig_content_rejects_garbage():
    with pytest.raises(nostr.NostrError):
        nostr.decode_sig_content("not a bech32m string")


def test_encode_sig_content_rejects_wrong_length():
    with pytest.raises(nostr.NostrError):
        nostr.encode_sig_content(bytes(64))


# --------------------------------------------------------------------------- #
# Validator identity mapping.
# --------------------------------------------------------------------------- #

def test_validator_index_for_pubkey_maps_and_misses():
    assert nostr.validator_index_for_pubkey(NET, NET.nostr_validator_keys[3]) == 3
    # Case-insensitive.
    assert nostr.validator_index_for_pubkey(NET, NET.nostr_validator_keys[3].upper()) == 3
    assert nostr.validator_index_for_pubkey(NET, "ff" * 32) is None
    assert nostr.validator_index_for_pubkey(NET, "") is None


# --------------------------------------------------------------------------- #
# BLS acceptance gate.
# --------------------------------------------------------------------------- #

def test_verify_validator_sig_accepts_real_rejects_tampered():
    pytest.importorskip("chia_rs")
    from chia_rs import AugSchemeMPL

    idx = 2
    sk = _sk(idx)
    net = _net_with({idx: (sk, _nostr_hex(idx))})
    digest = bytes(range(32))
    sig = bytes(AugSchemeMPL.sign(sk, digest))

    assert nostr.verify_validator_sig(net, idx, sig, digest)
    # Wrong digest (models a signature for a different / stale portal message).
    assert not nostr.verify_validator_sig(net, idx, sig, bytes(range(1, 33)))
    # Right signature, but checked against a different validator's key.
    assert not nostr.verify_validator_sig(net, 5, sig, digest)
    # Malformed signature bytes verify as False rather than raising.
    assert not nostr.verify_validator_sig(net, idx, b"\x00" * 96, digest)
    assert not nostr.verify_validator_sig(net, idx, b"too short", digest)
    # Out-of-range index (e.g. the infinity pad) is not signable.
    assert not nostr.verify_validator_sig(net, 999, sig, digest)


def test_parse_signature_event_accepts_valid():
    pytest.importorskip("chia_rs")
    idx = 1
    sk = _sk(idx)
    net = _net_with({idx: (sk, _nostr_hex(idx))})
    digest = bytes(range(32))
    rt = nostr.routing_tag_for(net, bytes.fromhex("11" * 32))
    ct = nostr.coin_tag_for(bytes.fromhex("33" * 32))
    event = _signed_event(sk, _nostr_hex(idx), digest, rt, ct)

    vs = nostr.parse_signature_event(net, event, digest=digest, routing_tag=rt, coin_tag=ct)
    assert vs is not None
    assert vs.index == idx
    assert vs.pubkey == _nostr_hex(idx)
    assert vs.sig == nostr.decode_sig_content(event["content"])


def test_parse_rejects_unknown_author():
    pytest.importorskip("chia_rs")
    idx = 0
    sk = _sk(idx)
    net = _net_with({idx: (sk, _nostr_hex(idx))})
    digest = bytes(range(32))
    rt = nostr.routing_tag_for(net, bytes(32))
    ct = nostr.coin_tag_for(bytes(32))
    event = _signed_event(sk, "de" * 32, digest, rt, ct)  # author not a known validator
    assert nostr.parse_signature_event(net, event, digest=digest, routing_tag=rt, coin_tag=ct) is None


def test_parse_rejects_stale_coin_tag():
    pytest.importorskip("chia_rs")
    idx = 0
    sk = _sk(idx)
    net = _net_with({idx: (sk, _nostr_hex(idx))})
    digest = bytes(range(32))
    rt = nostr.routing_tag_for(net, bytes(32))
    current_ct = nostr.coin_tag_for(bytes.fromhex("aa" * 32))
    stale_ct = nostr.coin_tag_for(bytes.fromhex("bb" * 32))
    event = _signed_event(sk, _nostr_hex(idx), digest, rt, stale_ct)  # portal has moved
    assert nostr.parse_signature_event(
        net, event, digest=digest, routing_tag=rt, coin_tag=current_ct
    ) is None


def test_parse_rejects_forged_or_replayed_signature():
    pytest.importorskip("chia_rs")
    idx = 0
    sk = _sk(idx)
    net = _net_with({idx: (sk, _nostr_hex(idx))})
    digest = bytes(range(32))
    stale_digest = bytes(range(1, 33))
    rt = nostr.routing_tag_for(net, bytes(32))
    ct = nostr.coin_tag_for(bytes(32))
    # Correct author + tags, but the signature is over a different digest: the
    # BLS check fails, so the event is rejected even though it looks routed right.
    event = _signed_event(sk, _nostr_hex(idx), stale_digest, rt, ct)
    assert nostr.parse_signature_event(net, event, digest=digest, routing_tag=rt, coin_tag=ct) is None


def test_parse_rejects_non_kind1_and_bad_content():
    pytest.importorskip("chia_rs")
    idx = 0
    sk = _sk(idx)
    net = _net_with({idx: (sk, _nostr_hex(idx))})
    digest = bytes(range(32))
    rt = nostr.routing_tag_for(net, bytes(32))
    ct = nostr.coin_tag_for(bytes(32))

    wrong_kind = _signed_event(sk, _nostr_hex(idx), digest, rt, ct)
    wrong_kind["kind"] = 0
    assert nostr.parse_signature_event(net, wrong_kind, digest=digest, routing_tag=rt, coin_tag=ct) is None

    bad_content = _signed_event(sk, _nostr_hex(idx), digest, rt, ct)
    bad_content["content"] = "not-a-signature"
    assert nostr.parse_signature_event(net, bad_content, digest=digest, routing_tag=rt, coin_tag=ct) is None


def test_collect_from_events_dedups_and_ignores_junk():
    pytest.importorskip("chia_rs")
    vals = {0: (_sk(0), _nostr_hex(0)), 1: (_sk(1), _nostr_hex(1))}
    net = _net_with(vals)
    digest = bytes(range(32))
    rt = nostr.routing_tag_for(net, bytes(32))
    ct = nostr.coin_tag_for(bytes(32))
    ev0 = _signed_event(vals[0][0], vals[0][1], digest, rt, ct)
    ev1 = _signed_event(vals[1][0], vals[1][1], digest, rt, ct)
    junk = {"kind": 1, "pubkey": "ee" * 32, "tags": [], "content": "x"}

    collected = nostr.collect_from_events(
        net, [ev0, dict(ev0), ev1, junk], digest=digest, routing_tag=rt, coin_tag=ct
    )
    assert set(collected) == {0, 1}
    assert collected[0] == nostr.decode_sig_content(ev0["content"])


# --------------------------------------------------------------------------- #
# Filter + collected -> driver inputs.
# --------------------------------------------------------------------------- #

def test_build_filter_shape():
    filt = nostr.build_filter(NET, "r1routing", coin_tag="c1coin")
    assert filt["kinds"] == [nostr.SIG_EVENT_KIND]
    assert filt["#r"] == ["r1routing"]
    assert filt["#c"] == ["c1coin"]
    assert filt["authors"] == list(NET.nostr_validator_keys)

    bare = nostr.build_filter(NET, "r1routing", authors=False)
    assert "authors" not in bare and "#c" not in bare


def test_sig_switches_for_maps_indices_ascending():
    switches, sigs = nostr.sig_switches_for(NET, {5: b"s5", 0: b"s0", 3: b"s3"})
    assert len(switches) == len(NET.validator_bls_keys)  # 10 validators + infinity pad
    assert [i for i, on in enumerate(switches) if on] == [0, 3, 5]
    assert sigs == [b"s0", b"s3", b"s5"]  # ordered to match the set bits


# --------------------------------------------------------------------------- #
# NostrSigCollector relay sweep.
# --------------------------------------------------------------------------- #

def test_collector_reaches_threshold_and_stops_early():
    pytest.importorskip("chia_rs")
    digest = bytes(range(32))
    nonce = bytes(32)
    coin = bytes.fromhex("33" * 32)
    vals = {i: (_sk(i), _nostr_hex(i)) for i in range(6)}  # mainnet threshold is 6
    net = _net_with(vals)
    rt = nostr.routing_tag_for(net, nonce)
    ct = nostr.coin_tag_for(coin)
    by_relay = {
        net.nostr_relays[i]: [_signed_event(vals[i][0], vals[i][1], digest, rt, ct)]
        for i in range(6)
    }
    fetcher = FakeFetcher(by_relay)
    times = iter(float(i) for i in range(50))
    collector = nostr.NostrSigCollector(net, fetcher=fetcher, clock=lambda: next(times))

    res = collector.collect(nonce=nonce, portal_coin_id=coin, digest=digest, deadline_s=1000)
    assert res.complete and res.count == 6
    # Stopped the instant quorum was met: exactly six relays queried, not all ten.
    assert len(fetcher.calls) == 6
    switches, sigs = nostr.sig_switches_for(net, res.collected)
    assert sum(switches) == 6 and len(sigs) == 6


def test_collector_skips_fetch_when_have_meets_threshold():
    have = {i: b"x" for i in range(NET.signature_threshold)}
    fetcher = FakeFetcher({})
    collector = nostr.NostrSigCollector(NET, fetcher=fetcher, clock=lambda: 0.0)
    res = collector.collect(
        nonce=bytes(32), portal_coin_id=bytes(32), digest=bytes(32), have=have
    )
    assert res.complete and len(fetcher.calls) == 0


def test_collector_respects_deadline():
    calls: list = []

    def fetcher(relay, filt, timeout):
        calls.append(relay)
        return []

    times = iter([0.0, 1.0, 2.0, 100.0, 101.0])  # third pass sees elapsed 100 >= deadline 50
    collector = nostr.NostrSigCollector(NET, fetcher=fetcher, clock=lambda: next(times))
    res = collector.collect(
        nonce=bytes(32), portal_coin_id=bytes(32), digest=bytes(32), deadline_s=50
    )
    assert not res.complete
    assert len(calls) == 2


def test_collector_relay_offset_rotates_start():
    seen: list = []

    def fetcher(relay, filt, timeout):
        seen.append(relay)
        return []

    collector = nostr.NostrSigCollector(NET, fetcher=fetcher, clock=lambda: 0.0)
    collector.collect(
        nonce=bytes(32), portal_coin_id=bytes(32), digest=bytes(32),
        deadline_s=1000, relay_offset=3,
    )
    assert seen[0] == NET.nostr_relays[3]
    assert len(seen) == len(NET.nostr_relays)


def test_collector_survives_dead_relay():
    pytest.importorskip("chia_rs")
    digest = bytes(range(32))
    nonce = bytes(32)
    coin = bytes.fromhex("33" * 32)
    vals = {0: (_sk(0), _nostr_hex(0))}
    net = _net_with(vals)
    rt = nostr.routing_tag_for(net, nonce)
    ct = nostr.coin_tag_for(coin)
    good_event = _signed_event(vals[0][0], vals[0][1], digest, rt, ct)

    def fetcher(relay, filt, timeout):
        if relay == net.nostr_relays[0]:
            raise OSError("relay unreachable")
        if relay == net.nostr_relays[1]:
            return [good_event]
        return []

    times = iter(float(i) for i in range(50))
    collector = nostr.NostrSigCollector(net, fetcher=fetcher, clock=lambda: next(times))
    res = collector.collect(nonce=nonce, portal_coin_id=coin, digest=digest, deadline_s=1000)
    # The dead first relay was skipped, not fatal; the signature still landed.
    assert 0 in res.collected


# --------------------------------------------------------------------------- #
# Outbound (ECDSA) collection, pinned against the real unwrap's signatures.
# --------------------------------------------------------------------------- #

def _unwrap_fixture():
    import json, pathlib
    return json.loads(
        (pathlib.Path(__file__).parent / "fixtures_unwrap.json").read_text()
    )


def _real_digest_and_sigs():
    from gui.services.warp import evm

    fx = _unwrap_fixture()
    digest = evm.validator_message_digest(
        bytes.fromhex(NET.eip712_domain_separator),
        bytes.fromhex(fx["nonce"]),
        fx["source_chain"].encode(),
        bytes.fromhex(fx["source"]),
        fx["destination"],
        [bytes.fromhex(c) for c in fx["contents"]],
    )
    packed = bytes.fromhex(fx["sigs_packed"])
    sigs = [packed[i * 65:(i + 1) * 65] for i in range(len(packed) // 65)]
    return fx, digest, sigs


def _outbound_event(sig65: bytes, tag: str, *, pubkey=None) -> dict:
    return {
        "kind": 1,
        "pubkey": pubkey or NET.nostr_validator_keys[0],
        "tags": [["r", tag], ["c", ""]],          # empty c, as on the wire
        "content": cu.bech32m_encode_bytes("s", sig65),
    }


def test_the_real_unwrap_signatures_collect_and_recover(monkeypatch=None):
    pytest.importorskip("eth_keys")
    fx, digest, sigs = _real_digest_and_sigs()
    tag = nostr.outbound_routing_tag(bytes.fromhex(fx["nonce"]))

    events = [
        _outbound_event(s, tag, pubkey=NET.nostr_validator_keys[i % 10])
        for i, s in enumerate(sigs)
    ]
    collected = nostr.collect_ecdsa_from_events(
        NET, events, digest=digest, routing_tag=tag
    )
    assert len(collected) == 6
    validators = {a.lower() for a in NET.evm_validator_addresses}
    assert {a.lower() for a in collected} <= validators

    # The relay tuple set re-packs into the exact on-chain blob.
    from gui.services.warp import evm

    tuples = nostr.ecdsa_sigs_for_relay(collected, 6)
    assert evm.pack_validator_sigs(tuples).hex() == fx["sigs_packed"]


def test_outbound_collection_rejects_what_it_must():
    pytest.importorskip("eth_keys")
    fx, digest, sigs = _real_digest_and_sigs()
    tag = nostr.outbound_routing_tag(bytes.fromhex(fx["nonce"]))

    tampered = bytes([sigs[0][0]]) + b"\x00" * 64
    wrong_tag = nostr.outbound_routing_tag(b"\xab" * 32)
    cases = [
        _outbound_event(tampered, tag),                       # unrecoverable
        _outbound_event(sigs[0], wrong_tag),                  # wrong message
        _outbound_event(sigs[0][:64], tag),                   # wrong length
        {**_outbound_event(sigs[0], tag), "pubkey": "ff" * 32},  # unknown author
        {**_outbound_event(sigs[0], tag), "kind": 7},         # wrong kind
    ]
    collected = nostr.collect_ecdsa_from_events(
        NET, cases, digest=digest, routing_tag=tag
    )
    assert collected == {}

    # A signature over a DIFFERENT digest recovers to a non-validator: the
    # ecrecover gate is what stops a valid-shape wrong-message signature.
    other = nostr.collect_ecdsa_from_events(
        NET, [_outbound_event(sigs[0], tag)], digest=b"\x00" * 32, routing_tag=tag
    )
    assert other == {}


def test_ecdsa_sigs_for_relay_is_deterministic_and_bounded():
    pytest.importorskip("eth_keys")
    _fx, digest, sigs = _real_digest_and_sigs()
    from gui.services.warp import evm as _evm

    with pytest.raises(nostr.NostrError, match="have 0 of 6"):
        nostr.ecdsa_sigs_for_relay({}, 6)
