"""Regression tests for the fourth PR-73 review round: the settings save
fails closed on an unreadable secrets.yaml, and key adoption drops the engine
before anything can fail."""

from __future__ import annotations

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

import yaml  # noqa: E402

from gui.services.basewallet import BaseWallet  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import service as S  # noqa: E402

from .test_base_wallet import FakeEvm, NullProtector  # noqa: E402

NET = C.MAINNET


# --------------------------------------------------------------------------- #
# #1 a Settings save must never overwrite a secrets.yaml it could not read.
# --------------------------------------------------------------------------- #

def _save(config_path, full):
    from gui.services.config_split import split_and_save

    split_and_save(config_path, full)


def test_settings_save_refuses_a_non_mapping_secrets_file(tmp_path):
    cfg = tmp_path / "config.yaml"
    sec = tmp_path / "secrets.yaml"
    # A corrupt-but-present file that still holds key material as text.
    sec.write_text("- evm_private_key_dpapi: PRECIOUS-BLOB\n", encoding="utf-8")
    with pytest.raises(ValueError, match="does not parse to a mapping"):
        _save(cfg, {"coingecko": {"api_key": "abc"}})
    assert "PRECIOUS-BLOB" in sec.read_text(encoding="utf-8"), \
        "the unreadable file is left byte-for-byte alone"


def test_settings_save_refuses_an_unparseable_secrets_file(tmp_path):
    cfg = tmp_path / "config.yaml"
    sec = tmp_path / "secrets.yaml"
    sec.write_text("warp: [unclosed\n  evm_private_key_dpapi: BLOB\n",
                   encoding="utf-8")
    with pytest.raises(ValueError, match="refusing to overwrite"):
        _save(cfg, {"coingecko": {"api_key": "abc"}})
    assert "BLOB" in sec.read_text(encoding="utf-8")


def test_settings_save_still_works_on_empty_and_healthy_files(tmp_path):
    cfg = tmp_path / "config.yaml"
    sec = tmp_path / "secrets.yaml"
    sec.write_text("", encoding="utf-8")           # empty file: fine
    _save(cfg, {"coingecko": {"api_key": "abc"}})
    assert yaml.safe_load(sec.read_text(encoding="utf-8"))["coingecko"][
        "api_key"] == "abc"
    _save(cfg, {"coingecko": {"api_key": "def"}})  # healthy round-trip: fine
    assert yaml.safe_load(sec.read_text(encoding="utf-8"))["coingecko"][
        "api_key"] == "def"


# --------------------------------------------------------------------------- #
# #2 adoption drops the engine FIRST and blocks rebuilds on a failed re-read.
# --------------------------------------------------------------------------- #

class _DropSentinel:
    closed = False

    def close(self):
        self.closed = True


def _worker(tmp_path, monkeypatch):
    secrets_path = tmp_path / "secrets.yaml"
    worker = S._WarpWorker(secrets_path=secrets_path)
    worker.set_config({"warp": {"enabled": True,
                                "jobs_db": str(tmp_path / "warp_jobs.db"),
                                "evm_private_key_dpapi": "STALE-BLOB"}})
    ev = FakeEvm()
    monkeypatch.setattr(
        worker, "_base_wallet",
        lambda: BaseWallet(NET, ev, S._SecretsFileIO(secrets_path),
                           protector=NullProtector()),
    )
    return worker, secrets_path


def test_failed_reread_still_drops_the_engine_and_blocks_rebuild(
    tmp_path, monkeypatch
):
    worker, secrets_path = _worker(tmp_path, monkeypatch)
    sentinel = _DropSentinel()
    worker._engine = sentinel
    # The on-disk file is corrupt: the re-read will raise.
    secrets_path.write_text("- not a mapping\n", encoding="utf-8")

    worker._adopt_key_from_disk()

    assert sentinel.closed and worker._engine is None, \
        "the engine bound to the possibly-retired key must be gone"
    assert "unreadable" in (worker._engine_error or ""), \
        "rebuilds stay blocked until the on-disk key can be read"
    assert "evm_private_key_dpapi" not in worker._config["warp"], \
        "the stale config blob is purged so no rebuild can resurrect it"
    assert worker._ensure_engine() is None


def test_adoption_purges_the_stale_blob_when_no_key_is_on_disk(
    tmp_path, monkeypatch
):
    worker, secrets_path = _worker(tmp_path, monkeypatch)
    secrets_path.write_text("warp: {}\n", encoding="utf-8")   # readable, keyless
    worker._adopt_key_from_disk()
    assert "evm_private_key_dpapi" not in worker._config["warp"]
    assert worker._engine_error is None, \
        "not blocked -- the engine just refuses normally for want of a key"


def test_happy_path_adoption_still_binds_the_new_key(tmp_path, monkeypatch):
    worker, secrets_path = _worker(tmp_path, monkeypatch)
    snaps: list = []
    worker.snapshot_ready.connect(snaps.append)
    secrets_path.unlink(missing_ok=True)
    # create writes a fresh key; the finally-adopt must bind it.
    worker._config["warp"].pop("evm_private_key_dpapi", None)
    worker.wallet_action("create", {})
    on_disk = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))
    assert worker._config["warp"]["evm_private_key_dpapi"] == \
        on_disk["warp"]["evm_private_key_dpapi"]
    assert worker._engine_error is None
