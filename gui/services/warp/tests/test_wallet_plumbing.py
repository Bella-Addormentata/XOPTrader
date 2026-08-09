"""Base-wallet plumbing through the warp worker: command dispatch, key
adoption after create/rotate, the fail-closed rotation guard, the secrets.yaml
round-trip, the config split, and the widget's amount parser."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")
pytest.importorskip("PySide6")

import yaml  # noqa: E402

from gui.services.basewallet import BaseWallet  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import service as S  # noqa: E402

NET = C.MAINNET


class FakeEvm:
    def __init__(self) -> None:
        self.eth = 10 ** 16
        self.usdc = 5_000_000
        self.sent: list = []
        self.fees = SimpleNamespace(max_fee_per_gas=1_000_000_000,
                                    max_priority_fee_per_gas=1_000_000)

    def get_eth_balance(self, addr):
        return self.eth

    def get_erc20_balance(self, token, addr):
        return self.usdc

    def get_nonce(self, addr, *, pending=True):
        return 7

    def get_fee_data(self, **kw):
        return self.fees

    def estimate_gas(self, **kw):
        return 60_000

    def send_raw_transaction(self, raw):
        self.sent.append(bytes(raw))
        return "0x" + "aa" * 32


class NullProtector:
    def protect(self, data: bytes, entropy: bytes = b"") -> bytes:
        return b"NP" + data

    def unprotect(self, data: bytes, entropy: bytes = b"") -> bytes:
        assert data[:2] == b"NP"
        return data[2:]


def _worker(tmp_path, monkeypatch, *, config=None):
    """A _WarpWorker over a real tmp secrets.yaml, with DPAPI and the chain
    stubbed out -- the file round-trip and the BaseWallet logic stay real."""
    secrets_path = tmp_path / "secrets.yaml"
    worker = S._WarpWorker(secrets_path=secrets_path)
    cfg = {"warp": {"enabled": True,
                    "jobs_db": str(tmp_path / "warp_jobs.db")}}
    if config:
        cfg["warp"].update(config)
    worker.set_config(cfg)
    evm = FakeEvm()
    monkeypatch.setattr(
        worker, "_base_wallet",
        lambda: BaseWallet(NET, evm, S._SecretsFileIO(secrets_path),
                           protector=NullProtector()),
    )
    snaps: list = []
    worker.snapshot_ready.connect(snaps.append)
    return worker, evm, snaps, secrets_path


# --------------------------------------------------------------------------- #
# The secrets round-trip.
# --------------------------------------------------------------------------- #

def test_secrets_io_missing_file_reads_empty_and_round_trips(tmp_path):
    io = S._SecretsFileIO(tmp_path / "secrets.yaml")
    assert io.read() == {}
    io.write({"warp": {"evm_private_key_dpapi": "blob"}})
    assert io.read()["warp"]["evm_private_key_dpapi"] == "blob"


# --------------------------------------------------------------------------- #
# create: the bootstrap for an engine that cannot build without a key.
# --------------------------------------------------------------------------- #

def test_create_writes_the_key_and_points_the_engine_at_it(tmp_path, monkeypatch):
    worker, _evm, snaps, secrets_path = _worker(tmp_path, monkeypatch)

    class DropSentinel:
        closed = False

        def close(self):
            self.closed = True

    sentinel = DropSentinel()
    worker._engine = sentinel
    worker._engine_error = "no EVM hot-wallet key configured"

    worker.wallet_action("create", {})

    on_disk = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))
    blob = on_disk["warp"]["evm_private_key_dpapi"]
    assert blob, "create must persist the key blob to secrets.yaml"
    # The worker's own config now carries the new blob, the stale engine is
    # gone, and the build error is cleared -- the next tick rebuilds with the
    # key instead of refusing until a full config reload.
    assert worker._config["warp"]["evm_private_key_dpapi"] == blob
    assert worker._engine is None and sentinel.closed
    assert worker._engine_error is None

    snap = snaps[-1]
    assert snap["base_wallet"]["configured"] is True
    assert snap["base_wallet"]["backup_confirmed"] is False
    assert "created" in snap["wallet_notice"].lower()
    assert "wallet_action_error" not in snap


def test_a_failed_action_is_surfaced_and_the_prior_notice_survives(
    tmp_path, monkeypatch
):
    worker, _evm, snaps, _sp = _worker(tmp_path, monkeypatch)
    worker.wallet_action("create", {})
    worker.wallet_action("create", {})          # refused: wallet exists

    snap = snaps[-1]
    assert "destroying money" in snap["wallet_action_error"]
    assert "created" in snap["wallet_notice"].lower(), \
        "the last successful action's notice must persist through a failure"


def test_an_unknown_action_is_an_error_not_a_crash(tmp_path, monkeypatch):
    worker, _evm, snaps, _sp = _worker(tmp_path, monkeypatch)
    worker.wallet_action("frobnicate", {})
    assert "unknown wallet action" in snaps[-1]["wallet_action_error"]


# --------------------------------------------------------------------------- #
# rotate: adoption, and the fail-closed open-job guard.
# --------------------------------------------------------------------------- #

def test_rotate_archives_sweeps_and_adopts_the_new_key(tmp_path, monkeypatch):
    worker, evm, snaps, secrets_path = _worker(tmp_path, monkeypatch)
    worker.wallet_action("create", {})
    old_blob = worker._config["warp"]["evm_private_key_dpapi"]

    worker.wallet_action("rotate", {})

    on_disk = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))["warp"]
    assert len(on_disk["retired_keys"]) == 1
    assert on_disk["retired_keys"][0]["blob"] == old_blob
    assert on_disk["evm_private_key_dpapi"] != old_blob
    assert worker._config["warp"]["evm_private_key_dpapi"] == \
        on_disk["evm_private_key_dpapi"]
    assert len(evm.sent) == 2, "USDC sweep then ETH sweep"
    snap = snaps[-1]
    assert "rotated" in snap["wallet_notice"].lower()
    assert snap["base_wallet"]["retired_count"] == 1
    assert snap["base_wallet"]["backup_confirmed"] is False


def test_rotation_refuses_when_the_job_store_is_unreadable(tmp_path, monkeypatch):
    """Fail-closed: an unreadable job DB might hide an open job frozen against
    the current key, so rotation must refuse, not sweep."""
    worker, evm, snaps, _sp = _worker(tmp_path, monkeypatch)
    worker.wallet_action("create", {})
    (tmp_path / "warp_jobs.db").write_bytes(b"this is not a sqlite database")

    worker.wallet_action("rotate", {})

    assert "wallet_action_error" in snaps[-1]
    assert evm.sent == [], "nothing may be swept when the job state is unknown"


def test_rotation_proceeds_when_no_job_db_ever_existed(tmp_path, monkeypatch):
    worker, _evm, _snaps, _sp = _worker(tmp_path, monkeypatch)
    assert worker._wallet_open_job() is None


# --------------------------------------------------------------------------- #
# The snapshot summary.
# --------------------------------------------------------------------------- #

def test_summary_is_unconfigured_before_any_key_exists(tmp_path, monkeypatch):
    worker, _evm, snaps, _sp = _worker(tmp_path, monkeypatch)
    worker.wallet_action("confirm_backup", {})   # any action emits a snapshot
    bw = snaps[-1]["base_wallet"]
    assert bw["configured"] is False and "address" not in bw


def test_summary_rides_the_engine_balances_without_extra_rpc(
    tmp_path, monkeypatch
):
    """With the engine's hot_wallet dict present the summary must not build a
    wallet (and so not spend two more RPCs per tick)."""
    worker, _evm, _snaps, secrets_path = _worker(tmp_path, monkeypatch)
    worker.wallet_action("create", {})
    monkeypatch.setattr(
        worker, "_base_wallet",
        lambda: (_ for _ in ()).throw(AssertionError("summary hit the RPC path")),
    )
    out = worker._wallet_summary(
        {"address": "0x" + "ab" * 20, "eth_wei": 5, "usdc_micros": 7}
    )
    assert (out["address"], out["eth_wei"], out["usdc_micros"]) == \
        ("0x" + "ab" * 20, 5, 7)


def test_summary_never_raises_when_the_balance_read_fails(tmp_path, monkeypatch):
    worker, _evm, _snaps, _sp = _worker(tmp_path, monkeypatch)
    worker.wallet_action("create", {})
    monkeypatch.setattr(
        worker, "_base_wallet",
        lambda: (_ for _ in ()).throw(RuntimeError("rpc down")),
    )
    out = worker._wallet_summary(None)
    assert out["configured"] is True and out["error"] == "rpc down"


# --------------------------------------------------------------------------- #
# The config split: new wallet secrets must never reach public config.yaml.
# --------------------------------------------------------------------------- #

def test_wallet_secret_keys_never_reach_public_config(tmp_path):
    from gui.services.config_split import split_and_save

    config_path = tmp_path / "config.yaml"
    full = {
        "warp": {
            "enabled": True,
            "evm_private_key_dpapi": "blob",
            "relay_private_key_dpapi": "relay-blob",
            "retired_keys": [{"blob": "old-blob", "address": "0xdead"}],
            "evm_key_backup_confirmed": True,
        },
    }
    split_and_save(config_path, full)

    public = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}
    secrets = yaml.safe_load(
        (tmp_path / "secrets.yaml").read_text(encoding="utf-8")
    ) or {}
    assert set(public.get("warp") or {}) == {"enabled"}, \
        "a settings save must not copy key material into git-tracked config.yaml"
    assert secrets["warp"]["evm_private_key_dpapi"] == "blob"
    assert secrets["warp"]["relay_private_key_dpapi"] == "relay-blob"
    assert secrets["warp"]["retired_keys"][0]["blob"] == "old-blob"
    assert secrets["warp"]["evm_key_backup_confirmed"] is True


# --------------------------------------------------------------------------- #
# The widget's amount parser (pure function; no QApplication needed).
# --------------------------------------------------------------------------- #

def test_parse_asset_amount_exact_or_refused():
    from gui.widgets.base_wallet import parse_asset_amount

    assert parse_asset_amount("1.5", decimals=6) == 1_500_000
    assert parse_asset_amount("0.000001", decimals=6) == 1
    assert parse_asset_amount("0.005", decimals=18) == 5 * 10 ** 15
    # Sub-precision is refused, never rounded into an amount the operator
    # did not type (the unwrap field's float bug, kept fixed here too).
    assert parse_asset_amount("0.0000001", decimals=6) is None
    for bad in ("abc", "", "-1", "0", "nan", "inf", "1e309"):
        assert parse_asset_amount(bad, decimals=6) is None, bad
