"""A fresh install must be able to reach the local Chia wallet.

Field failure (2026-08-13, v0.9.6): a clean install bootstrapped
config.example.yaml, which shipped `verify_ssl: true`. Chia's RPC
certificate comes from a private CA and is issued to "Chia", not
"localhost", so every request died with CURL error 60 -- no wallet, no
balances, no mid prices, and the Target Allocation panel reporting a
confident "100% wUSDC.b" over the single hard-pegged asset. The
auto-detect that should have corrected it only filled the key in when it
was ABSENT, so the template's `true` was never touched.

Both halves are pinned here: the shipped template, and the auto-detect."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

import yaml

from gui import main as gui_main

REPO = Path(__file__).resolve().parents[1]
DETECTED_FP = 481_655_774


def test_shipped_example_does_not_break_localhost_wallet_access():
    """The template must be usable as-is on a normal single-machine setup."""
    cfg = yaml.safe_load((REPO / "config.example.yaml").read_text(encoding="utf-8"))
    chia = cfg["chia"]
    assert chia["full_node_host"] in ("localhost", "127.0.0.1", "::1"), \
        "template targets a local node..."
    assert chia["verify_ssl"] is False, (
        "config.example.yaml ships verify_ssl: true against a LOCALHOST Chia "
        "node -- its private-CA cert cannot pass peer verification, so every "
        "fresh install fails to read the wallet (CURL error 60)"
    )


def _fresh_home(tmp_path, monkeypatch, *, verify_ssl_value):
    """A first-run home built from the real template, with the chia.verify_ssl
    value under test, plus a fake local Chia install for the detectors."""
    cfg_path = tmp_path / "config.yaml"
    shutil.copyfile(REPO / "config.example.yaml", cfg_path)
    shutil.copyfile(REPO / "secrets.example.yaml", tmp_path / "secrets.yaml")

    body = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    if verify_ssl_value is None:
        body["chia"].pop("verify_ssl", None)
    else:
        body["chia"]["verify_ssl"] = verify_ssl_value
    cfg_path.write_text(yaml.safe_dump(body, sort_keys=False), encoding="utf-8")

    chia_root = tmp_path / ".chia" / "mainnet"
    for sub in ("full_node", "wallet", "ca"):
        (chia_root / "config" / "ssl" / sub).mkdir(parents=True)
    for rel in ("full_node/private_full_node.crt", "full_node/private_full_node.key",
                "wallet/private_wallet.crt", "wallet/private_wallet.key",
                "ca/chia_ca.crt"):
        (chia_root / "config" / "ssl" / rel).write_text("cert", encoding="utf-8")

    monkeypatch.setattr(gui_main, "_detect_chia_root", lambda: chia_root)
    monkeypatch.setattr(gui_main, "_detect_wallet_fingerprint", lambda: DETECTED_FP)
    monkeypatch.setattr(gui_main, "_detect_chia_rpc_ports", lambda root: {})
    return cfg_path


def _merged(cfg_path: Path) -> dict:
    from gui.services.config_split import load_merged

    return load_merged(cfg_path)


@pytest.mark.parametrize("template_value", [True, None])
def test_autodetect_disables_verification_for_a_local_node(
    tmp_path, monkeypatch, template_value
):
    """Whether the template says true or omits the key, auto-detecting a
    LOCAL Chia install must leave verification off -- otherwise the engine
    cannot talk to it at all."""
    cfg_path = _fresh_home(tmp_path, monkeypatch, verify_ssl_value=template_value)
    gui_main._patch_chia_auto_detect(cfg_path)
    assert _merged(cfg_path)["chia"]["verify_ssl"] is False


def test_autodetect_never_weakens_a_remote_node(tmp_path, monkeypatch):
    """The safety half: a remote host must keep verification ON."""
    cfg_path = _fresh_home(tmp_path, monkeypatch, verify_ssl_value=True)
    body = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    body["chia"]["full_node_host"] = "chia.example.net"
    cfg_path.write_text(yaml.safe_dump(body, sort_keys=False), encoding="utf-8")

    gui_main._patch_chia_auto_detect(cfg_path)
    assert _merged(cfg_path)["chia"]["verify_ssl"] is True, \
        "verification must never be silently disabled for a remote node"


def test_autodetect_respects_an_operator_who_chose_false_explicitly(
    tmp_path, monkeypatch
):
    cfg_path = _fresh_home(tmp_path, monkeypatch, verify_ssl_value=False)
    gui_main._patch_chia_auto_detect(cfg_path)
    assert _merged(cfg_path)["chia"]["verify_ssl"] is False
