"""Tests for the warp thin HTTP clients (coinset / watcher / wallet).

All three clients take an injected transport, so these tests drive them with
canned responses and never touch the network. Pure standard library.
"""

from __future__ import annotations

import types

import pytest

from gui.services.warp.coinset import (
    CoinRecord,
    CoinsetClient,
    CoinsetError,
    CoinSolution,
)
from gui.services.warp.wallet import (
    WalletClient,
    WalletError,
    wallet_params_from_config,
)
from gui.services.warp.watcher import WatcherClient, WatcherError


# --------------------------------------------------------------------------- #
# Fakes.
# --------------------------------------------------------------------------- #

class FakePoster:
    """Records (endpoint, body) and returns a canned response per endpoint.

    A response may be a dict or a ``callable(body) -> dict`` (use a callable that
    raises to simulate a transport failure).
    """

    def __init__(self, responses: dict) -> None:
        self.responses = responses
        self.calls: list[tuple[str, dict]] = []

    def __call__(self, url: str, body: dict) -> dict:
        endpoint = url.rsplit("/", 1)[-1]
        self.calls.append((endpoint, body))
        r = self.responses.get(endpoint)
        if r is None:
            raise AssertionError(f"unexpected endpoint: {endpoint}")
        return r(body) if callable(r) else r

    def body_for(self, endpoint: str) -> dict:
        for ep, body in self.calls:
            if ep == endpoint:
                return body
        raise AssertionError(f"endpoint not called: {endpoint}")


class FakeGetter:
    def __init__(self, response) -> None:
        self.response = response
        self.calls: list[tuple[str, dict]] = []

    def __call__(self, url: str, params: dict):
        self.calls.append((url, params))
        if callable(self.response):
            return self.response(url, params)
        return self.response


# --------------------------------------------------------------------------- #
# coinset.
# --------------------------------------------------------------------------- #

def test_coinset_peak_height():
    poster = FakePoster(
        {"get_blockchain_state": {"success": True, "blockchain_state": {"peak": {"height": 5_123_456}}}}
    )
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    assert c.get_peak_height() == 5_123_456


def test_coinset_coin_record_spent_derivation_and_prefix():
    poster = FakePoster(
        {
            "get_coin_record_by_name": {
                "success": True,
                "coin_record": {
                    "coin": {"parent_coin_info": "0xAA", "puzzle_hash": "0xBB", "amount": 1000},
                    "confirmed_block_index": 100,
                    "spent_block_index": 200,  # spent even though "spent" bool is absent
                    "coinbase": False,
                    "timestamp": 1690000000,
                },
            }
        }
    )
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    rec = c.get_coin_record_by_name("cd")
    assert isinstance(rec, CoinRecord)
    assert rec.spent is True and rec.spent_block_index == 200
    assert rec.coin.parent_coin_info == "aa" and rec.coin.puzzle_hash == "bb"
    assert rec.coin.amount == 1000
    # Names are re-prefixed with 0x for the RPC.
    assert poster.body_for("get_coin_record_by_name")["name"] == "0xcd"


def test_coinset_coin_record_missing_is_none():
    poster = FakePoster({"get_coin_record_by_name": {"success": True, "coin_record": None}})
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    assert c.get_coin_record_by_name("00") is None


def test_coinset_records_by_puzzle_hash_passes_filters():
    poster = FakePoster(
        {
            "get_coin_records_by_puzzle_hash": {
                "success": True,
                "coin_records": [
                    {
                        "coin": {"parent_coin_info": "01", "puzzle_hash": "ph", "amount": 7},
                        "confirmed_block_index": 10,
                        "spent_block_index": 0,
                    }
                ],
            }
        }
    )
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    recs = c.get_coin_records_by_puzzle_hash("ph", include_spent=True, start_height=5)
    assert len(recs) == 1 and recs[0].spent is False
    body = poster.body_for("get_coin_records_by_puzzle_hash")
    assert body["puzzle_hash"] == "0xph"
    assert body["include_spent_coins"] is True
    assert body["start_height"] == 5
    assert "end_height" not in body  # omitted when not supplied


def test_coinset_puzzle_and_solution_strips_prefix():
    poster = FakePoster(
        {
            "get_puzzle_and_solution": {
                "success": True,
                "coin_solution": {
                    "coin": {"parent_coin_info": "0x01", "puzzle_hash": "0x02", "amount": 1},
                    "puzzle_reveal": "0xDEAD",
                    "solution": "0xBEEF",
                },
            }
        }
    )
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    sol = c.get_puzzle_and_solution("ab", 123)
    assert isinstance(sol, CoinSolution)
    assert sol.puzzle_reveal == "dead" and sol.solution == "beef"
    assert poster.body_for("get_puzzle_and_solution")["height"] == 123


def test_coinset_puzzle_and_solution_missing_raises():
    poster = FakePoster({"get_puzzle_and_solution": {"success": True, "coin_solution": None}})
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    with pytest.raises(CoinsetError):
        c.get_puzzle_and_solution("ab", 123)


def test_coinset_push_tx_ok_and_reject():
    ok = FakePoster({"push_tx": {"success": True, "status": "SUCCESS"}})
    c = CoinsetClient("https://api.coinset.org", poster=ok)
    assert c.push_tx({"a": 1}) == "SUCCESS"
    assert ok.body_for("push_tx")["spend_bundle"] == {"a": 1}

    rej = FakePoster({"push_tx": {"success": False, "error": "DOUBLE_SPEND"}})
    c2 = CoinsetClient("https://api.coinset.org", poster=rej)
    with pytest.raises(CoinsetError) as ei:
        c2.push_tx({"a": 1})
    assert "push_tx" in str(ei.value)


def test_coinset_transport_error_is_wrapped():
    def boom(_body):
        raise ValueError("connection reset")

    poster = FakePoster({"get_blockchain_state": boom})
    c = CoinsetClient("https://api.coinset.org", poster=poster)
    with pytest.raises(CoinsetError) as ei:
        c.get_peak_height()
    assert "get_blockchain_state" in str(ei.value)


# --------------------------------------------------------------------------- #
# watcher.
# --------------------------------------------------------------------------- #

def _msg(nonce="2a", status="sent"):
    return {
        "nonce": "0x" + nonce,
        "status": status,
        "source_chain": "bse",
        "destination_chain": "xch",
        "destination": "0xDEAD",
        "contents": ["0x833589FC", "0xABCD", "0x1368"],
    }


def test_watcher_parses_sent_message():
    getter = FakeGetter([_msg()])
    w = WatcherClient("https://watcher-api.warp.green", getter=getter)
    m = w.get_message("2a")
    assert m is not None and m.is_sent
    assert m.nonce == "2a"
    assert m.destination == "dead"
    assert m.erc20_source == "833589fc"
    assert m.receiver_ph == "abcd"
    assert m.amount_mojos == 0x1368
    # Nonce is 0x-prefixed for the API; default source_chain is "bse".
    _url, params = getter.calls[0]
    assert params == {"source_chain": "bse", "nonce": "0x2a"}


def test_watcher_messages_envelope_shape():
    getter = FakeGetter({"messages": [_msg()]})
    w = WatcherClient("https://watcher-api.warp.green", getter=getter)
    assert w.get_message("2a") is not None


def test_watcher_empty_is_none():
    w = WatcherClient("https://watcher-api.warp.green", getter=FakeGetter([]))
    assert w.get_message("2a") is None


def test_watcher_nonce_mismatch_is_none():
    # API echoed an unrelated message -> do not claim the wrong nonce.
    getter = FakeGetter([_msg(nonce="ff")])
    w = WatcherClient("https://watcher-api.warp.green", getter=getter)
    assert w.get_message("2a") is None


def test_watcher_pending_message_is_not_sent():
    getter = FakeGetter([_msg(status="pending")])
    w = WatcherClient("https://watcher-api.warp.green", getter=getter)
    m = w.get_message("2a")
    assert m is not None and not m.is_sent


def test_watcher_transport_error_is_wrapped():
    def boom(_url, _params):
        raise ValueError("timeout")

    w = WatcherClient("https://watcher-api.warp.green", getter=FakeGetter(boom))
    with pytest.raises(WatcherError):
        w.get_message("2a")


# --------------------------------------------------------------------------- #
# wallet.
# --------------------------------------------------------------------------- #

def _wallet(poster):
    params = {
        "host": "localhost",
        "port": 9256,
        "fingerprint": 12345,
        "cert_path": "c.crt",
        "key_path": "k.key",
    }
    return WalletClient(params, poster=poster)


def test_wallet_get_next_address():
    poster = FakePoster({"get_next_address": {"success": True, "address": "xch1dest"}})
    w = _wallet(poster)
    assert w.get_next_address() == "xch1dest"
    body = poster.body_for("get_next_address")
    assert body == {"wallet_id": 1, "new_address": True}


def test_wallet_send_transaction_returns_record():
    poster = FakePoster(
        {
            "send_transaction": {
                "success": True,
                "transaction": {"transaction_id": "0xtx", "name": "0xtx"},
                "transaction_id": "0xtx",
            }
        }
    )
    w = _wallet(poster)
    tx = w.send_transaction(1, 5_000_000, "xch1dest", fee_mojos=100_000_000)
    assert tx["transaction_id"] == "0xtx"
    body = poster.body_for("send_transaction")
    assert body["amount"] == 5_000_000
    assert body["fee"] == 100_000_000
    assert body["address"] == "xch1dest"
    assert body["wallet_id"] == 1


def test_wallet_failure_is_raised():
    poster = FakePoster({"send_transaction": {"success": False, "error": "insufficient funds"}})
    w = _wallet(poster)
    with pytest.raises(WalletError) as ei:
        w.send_transaction(1, 1, "xch1dest")
    assert "send_transaction" in str(ei.value)


def test_wallet_log_in_uses_fingerprint():
    poster = FakePoster({"log_in": {"success": True, "fingerprint": 12345}})
    w = _wallet(poster)
    w.log_in()
    assert poster.body_for("log_in") == {"fingerprint": 12345}


def test_wallet_log_in_without_fingerprint_is_noop():
    poster = FakePoster({})  # nothing should be called
    params = {"host": "localhost", "port": 9256, "cert_path": "", "key_path": ""}
    w = WalletClient(params, poster=poster)
    assert w.log_in() == {}
    assert poster.calls == []


def test_wallet_get_transactions():
    poster = FakePoster(
        {"get_transactions": {"success": True, "transactions": [{"name": "0x1"}, {"name": "0x2"}]}}
    )
    w = _wallet(poster)
    txs = w.get_transactions(1, start=0, end=10)
    assert [t["name"] for t in txs] == ["0x1", "0x2"]
    body = poster.body_for("get_transactions")
    assert body["start"] == 0 and body["end"] == 10 and body["reverse"] is True


def test_wallet_transport_error_is_wrapped():
    def boom(_body):
        raise ValueError("refused")

    w = _wallet(FakePoster({"get_next_address": boom}))
    with pytest.raises(WalletError):
        w.get_next_address()


# --------------------------------------------------------------------------- #
# wallet_params_from_config.
# --------------------------------------------------------------------------- #

def test_wallet_params_from_config_reads_chia_block():
    cfg = types.SimpleNamespace(
        chia=types.SimpleNamespace(
            wallet_host="host1",
            wallet_port="9999",
            wallet_fingerprint=42,
            wallet_cert_path="c.crt",
            wallet_key_path="k.key",
        )
    )
    p = wallet_params_from_config(cfg)
    assert p == {
        "host": "host1",
        "port": 9999,
        "fingerprint": 42,
        "cert_path": "c.crt",
        "key_path": "k.key",
    }


def test_wallet_params_from_config_defaults_when_missing():
    p = wallet_params_from_config(types.SimpleNamespace())
    assert p == {
        "host": "localhost",
        "port": 9256,
        "fingerprint": None,
        "cert_path": "",
        "key_path": "",
    }
