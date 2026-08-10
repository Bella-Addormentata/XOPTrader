"""Regression tests for the second adversarial review round (branch
feat/warp-altruistic-relay). Each pins one confirmed finding's fix."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

pytest.importorskip("chia_rs")
pytest.importorskip("eth_keys")

from gui.services import basewallet  # noqa: E402
from gui.services.basewallet import BaseWallet, BaseWalletError  # noqa: E402
from gui.services.warp import constants as C  # noqa: E402
from gui.services.warp import service as S  # noqa: E402

from .test_base_wallet import FakeEvm, MemSecrets, NullProtector  # noqa: E402

NET = C.MAINNET
DEST = "0x" + "ab" * 20


def _wallet(ev=None):
    io = MemSecrets()
    ev = ev or FakeEvm()
    w = BaseWallet(NET, ev, io, protector=NullProtector())
    w.create_wallet()
    return w, io, ev


# --------------------------------------------------------------------------- #
# #1 rotate() must refuse when the USDC sweep's gas is unaffordable.
# --------------------------------------------------------------------------- #

def test_rotation_refuses_when_gas_cannot_cover_the_usdc_sweep():
    w, io, ev = _wallet()
    ev.usdc = 500_000_000                 # 500 USDC
    ev.eth = 10                            # far below one sweep's gas
    with pytest.raises(BaseWalletError, match="Fund the wallet"):
        w.rotate(open_job_check=lambda: None)
    # The irreversible key swap must NOT have happened.
    assert ev.sent == []
    assert (io.data["warp"].get("retired_keys") or []) == []
    active = w.active_key().address
    assert active  # still the original key, nothing archived


def test_rotation_still_proceeds_with_gas_and_dust():
    w, io, ev = _wallet()
    ev.usdc = 0
    ev.eth = 1000                          # dust-only, nothing to sweep
    out = w.rotate(open_job_check=lambda: None)
    assert out["sweep_txs"] == []
    assert len(io.data["warp"]["retired_keys"]) == 1


# --------------------------------------------------------------------------- #
# #2 retired-key recovery: the in-app escape from a strand.
# --------------------------------------------------------------------------- #

def test_recover_retired_sweeps_a_stranded_key_back_to_active():
    w, io, ev = _wallet()
    out = w.rotate(open_job_check=lambda: None)   # ev has gas: rotates cleanly
    old_addr = out["old_address"]
    # A late deposit landing at the retired address (FakeEvm reports one
    # balance for any address, which is fine -- recover reads the retired one).
    ev.eth = 5 * 10 ** 15
    ev.usdc = 3_000_000
    rec = w.recover_retired(old_addr)
    assert rec["from"] == old_addr and rec["to"] == out["new_address"]
    assert len(rec["sweep_txs"]) == 2, "USDC then ETH"
    assert rec["swept_usdc_micros"] == 3_000_000


def test_recover_retired_refuses_a_gasless_usdc_strand_clearly():
    w, io, ev = _wallet()
    out = w.rotate(open_job_check=lambda: None)
    old_addr = out["old_address"]
    ev.eth = 1
    ev.usdc = 3_000_000
    with pytest.raises(BaseWalletError, match="Send it a little Base"):
        w.recover_retired(old_addr)


def test_recover_retired_rejects_an_unknown_address():
    w, _io, _ev = _wallet()
    with pytest.raises(BaseWalletError, match="no retired key"):
        w.recover_retired("0x" + "cd" * 20)


# --------------------------------------------------------------------------- #
# #4 manual sends refuse while a warp job is open (nonce race with the engine).
# --------------------------------------------------------------------------- #

def _worker_with_open_job(tmp_path, monkeypatch, *, job):
    from gui.services.warp import jobs as jobs_mod

    secrets_path = tmp_path / "secrets.yaml"
    worker = S._WarpWorker(secrets_path=secrets_path)
    worker.set_config({"warp": {"enabled": True,
                                "jobs_db": str(tmp_path / "warp_jobs.db")}})
    ev = FakeEvm()
    monkeypatch.setattr(
        worker, "_base_wallet",
        lambda: BaseWallet(NET, ev, S._SecretsFileIO(secrets_path),
                           protector=NullProtector()),
    )
    monkeypatch.setattr(worker, "_wallet_open_job", lambda: job)
    snaps: list = []
    worker.snapshot_ready.connect(snaps.append)
    worker.wallet_action("create", {})
    return worker, ev, snaps


def test_send_refuses_while_a_job_is_open(tmp_path, monkeypatch):
    worker, ev, snaps = _worker_with_open_job(
        tmp_path, monkeypatch, job=SimpleNamespace(id=9, status="BRIDGING")
    )
    worker.wallet_action("send_usdc", {"destination": DEST, "amount_micros": 1})
    assert "job 9" in snaps[-1]["wallet_action_error"]
    assert ev.sent == [], "nothing may broadcast while a job holds the slot"


def test_send_proceeds_when_no_job_is_open(tmp_path, monkeypatch):
    worker, ev, snaps = _worker_with_open_job(tmp_path, monkeypatch, job=None)
    ev.usdc = 5_000_000
    worker.wallet_action("send_usdc", {"destination": DEST, "amount_micros": 1})
    assert "wallet_action_error" not in snaps[-1]
    assert len(ev.sent) == 1


# --------------------------------------------------------------------------- #
# #18 a corrupt (non-dict) secrets.yaml must not be masked as empty.
# --------------------------------------------------------------------------- #

def test_corrupt_secrets_file_refuses_rather_than_masking(tmp_path):
    p = tmp_path / "secrets.yaml"
    p.write_text("- this is a list, not a mapping\n", encoding="utf-8")
    io = S._SecretsFileIO(p)
    with pytest.raises(ValueError, match="does not parse to a mapping"):
        io.read()
    # And through the wallet: create_wallet must NOT overwrite it.
    w = BaseWallet(NET, FakeEvm(), io, protector=NullProtector())
    with pytest.raises(ValueError):
        w.create_wallet()
    assert "list, not a mapping" in p.read_text(encoding="utf-8"), \
        "the corrupt file (which may hold a real key) is left untouched"


# --------------------------------------------------------------------------- #
# #20 secrets.yaml is written atomically (no truncate-in-place window).
# --------------------------------------------------------------------------- #

def test_secrets_write_failure_leaves_the_original_intact(tmp_path, monkeypatch):
    """A failure during the write (here: os.replace) must not destroy the
    existing key blob -- the whole point of temp-file-then-replace over
    truncate-in-place."""
    import gui.services.config_split as cs

    p = tmp_path / "secrets.yaml"
    p.write_text("warp:\n  evm_private_key_dpapi: OLD\n", encoding="utf-8")

    def boom_replace(src, dst):
        raise OSError("crash during rename")

    monkeypatch.setattr(cs.os, "replace", boom_replace)
    with pytest.raises(OSError):
        cs._atomic_write_text(p, "warp:\n  evm_private_key_dpapi: NEW\n",
                              newline="\n")
    # The original blob survives; no half-written garbage, no temp residue.
    assert "OLD" in p.read_text(encoding="utf-8")
    assert "NEW" not in p.read_text(encoding="utf-8")
    assert not any(f.name.startswith(".secrets.yaml.tmp") for f in tmp_path.iterdir())


def test_atomic_write_leaves_no_temp_residue(tmp_path):
    import gui.services.config_split as cs

    p = tmp_path / "secrets.yaml"
    cs._atomic_write_text(p, "warp:\n  x: 1\n", newline="\n")
    assert p.read_text(encoding="utf-8").strip().endswith("x: 1")
    assert not any(f.name.startswith(".secrets.yaml.tmp") for f in tmp_path.iterdir())


# --------------------------------------------------------------------------- #
# #15 (critical) the post-click gate is keyed off the worker's action counter,
# so a periodic timer tick carrying the SAME seq cannot re-enable the buttons.
# --------------------------------------------------------------------------- #

def test_pending_gate_survives_a_same_seq_timer_tick():
    pytest.importorskip("PySide6")
    import os

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PySide6.QtWidgets import QApplication

    from gui.widgets.base_wallet import BaseWalletWidget

    app = QApplication.instance() or QApplication([])
    w = BaseWalletWidget()
    w.show()
    base = {"base_wallet": {"configured": True, "address": DEST,
                            "eth_wei": 10 ** 18, "usdc_micros": 5_000_000,
                            "backup_confirmed": True}}
    w.update_data({"warp": {**base, "wallet_action_seq": 4}})
    assert not w._action_pending

    w._emit_action("send_usdc", {"destination": DEST, "amount_micros": 1})
    assert w._action_pending and not w._send_btn.isEnabled()

    # A ~5s master tick delivers a CACHED snapshot (unchanged seq) while the
    # action is still queued on the worker. The gate must hold.
    w.update_data({"warp": {**base, "wallet_action_seq": 4}})
    assert w._action_pending, "a same-seq tick must not re-enable the buttons"
    assert not w._send_btn.isEnabled()

    # The worker finishes: seq advances past the pending mark -> gate clears.
    w.update_data({"warp": {**base, "wallet_action_seq": 5,
                            "wallet_notice": "USDC transfer broadcast: 0xabc"}})
    assert not w._action_pending and w._send_btn.isEnabled()
    w.deleteLater()


# --------------------------------------------------------------------------- #
# [PR-73 Copilot] untrusted error text must never be interpreted as RichText.
# --------------------------------------------------------------------------- #

def test_error_text_cannot_inject_markup_into_the_widgets():
    pytest.importorskip("PySide6")
    import os

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication

    from gui.widgets.base_wallet import BaseWalletWidget
    from gui.widgets.warp import WarpWidget

    app = QApplication.instance() or QApplication([])
    evil = 'boom <img src="x"> & <b>styled</b>'

    # Base Wallet: the notice is PlainText, the banner escapes the error.
    bw = BaseWalletWidget()
    bw.show()
    assert bw._notice.textFormat() == Qt.TextFormat.PlainText
    bw.update_data({"warp": {"base_wallet": {"configured": True, "error": evil}}})
    assert "&lt;img" in bw._banner.text() and "<img" not in bw._banner.text()

    # Warp tab: banner error, action_error, and the activity error all escape.
    ww = WarpWidget()
    ww.show()
    ww.update_data({"warp": {"enabled": True, "error": evil,
                             "action_error": evil,
                             "relay_activity": {"checked_at": 1.0,
                                                "error": evil}}})
    assert "<img" not in ww._banner.text()
    assert "&lt;img" in ww._banner.text()
    assert "<img" not in ww._relay_activity_lbl.text()
    assert "&lt;img" in ww._relay_activity_lbl.text()
    bw.deleteLater()
    ww.deleteLater()
