"""Fresh-install auto-detect: pins the v0.9.2 field failure where every new
install's engine refused to start with 'wallet_fingerprint must not be 0'.

Root cause: config.example.yaml's placeholder was 1234567890 but
wallet_fingerprint is a secrets key and secrets.example.yaml ships 0 -- the
merge presented 0, the `== 1234567890` placeholder check never fired, and
the detected fingerprint was never written (while the log printed it as if
it had been). These tests run against the REAL shipped templates, so any
future placeholder drift fails here first."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest

import yaml

from gui import main as gui_main

REPO = Path(__file__).resolve().parents[1]
DETECTED_FP = 481_655_774


def _fresh_install(tmp_path, monkeypatch):
    """A first-run home built from the real example templates, with a fake
    local Chia installation for the detectors to find."""
    cfg = tmp_path / "config.yaml"
    shutil.copyfile(REPO / "config.example.yaml", cfg)
    shutil.copyfile(REPO / "secrets.example.yaml", tmp_path / "secrets.yaml")

    chia_root = tmp_path / ".chia" / "mainnet"
    for sub in ("full_node", "wallet", "ca"):
        (chia_root / "config" / "ssl" / sub).mkdir(parents=True)
    for rel in (
        "full_node/private_full_node.crt", "full_node/private_full_node.key",
        "wallet/private_wallet.crt", "wallet/private_wallet.key",
        "ca/chia_ca.crt",
    ):
        (chia_root / "config" / "ssl" / rel).write_text("cert", encoding="utf-8")

    monkeypatch.setattr(gui_main, "_detect_chia_root", lambda: chia_root)
    monkeypatch.setattr(
        gui_main, "_detect_wallet_fingerprint", lambda: DETECTED_FP
    )
    monkeypatch.setattr(gui_main, "_detect_chia_rpc_ports", lambda root: {})
    return cfg


def test_detected_fingerprint_lands_in_secrets_not_zero(tmp_path, monkeypatch):
    cfg = _fresh_install(tmp_path, monkeypatch)

    assert gui_main._patch_chia_auto_detect(cfg) is True

    secrets = yaml.safe_load(
        (tmp_path / "secrets.yaml").read_text(encoding="utf-8")
    )
    assert secrets["chia"]["wallet_fingerprint"] == DETECTED_FP, (
        "the detected fingerprint must be WRITTEN (to secrets.yaml, where "
        "the merge reads it), not skipped because the template placeholder "
        "was 0 rather than 1234567890"
    )
    # The cert paths land alongside it, pointing at the detected install.
    assert secrets["chia"]["wallet_cert_path"].endswith(
        "wallet/private_wallet.crt"
    )
    # And the engine-facing merged view agrees.
    from gui.services.config_split import load_merged

    merged = load_merged(cfg)
    assert merged["chia"]["wallet_fingerprint"] == DETECTED_FP


def test_manually_set_fingerprint_is_never_overwritten(tmp_path, monkeypatch):
    cfg = _fresh_install(tmp_path, monkeypatch)
    secrets_path = tmp_path / "secrets.yaml"
    body = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))
    body["chia"]["wallet_fingerprint"] = 999_999_999      # operator's choice
    secrets_path.write_text(yaml.safe_dump(body), encoding="utf-8")

    gui_main._patch_chia_auto_detect(cfg)

    after = yaml.safe_load(secrets_path.read_text(encoding="utf-8"))
    assert after["chia"]["wallet_fingerprint"] == 999_999_999


def test_detect_wallet_fingerprint_falls_back_to_local_db_when_cli_absent(tmp_path, monkeypatch):
    """When `chia keys show` is unavailable, read last_used_fingerprint or wallet DB filename."""
    chia_root = tmp_path / ".chia" / "mainnet"
    wallet_db_dir = chia_root / "wallet" / "db"
    wallet_db_dir.mkdir(parents=True)
    (wallet_db_dir / "last_used_fingerprint").write_text("311919707\n", encoding="utf-8")

    # Simulate subprocess failure (no chia executable)
    monkeypatch.setattr(gui_main.subprocess, "run", lambda *a, **kw: (_ for _ in ()).throw(FileNotFoundError("chia not found")))

    assert gui_main._detect_wallet_fingerprint(chia_root) == 311_919_707

    # Remove last_used_fingerprint and test wallet sqlite filename pattern fallback
    (wallet_db_dir / "last_used_fingerprint").unlink()
    (wallet_db_dir / "blockchain_wallet_v2_r1_mainnet_311919707.sqlite").write_text("", encoding="utf-8")

    assert gui_main._detect_wallet_fingerprint(chia_root) == 311_919_707


def test_example_templates_have_no_duplicate_keys():
    """config.example.yaml shipped a duplicated circuit_breaker block, which
    broke every comment-preserving write on fresh installs (ruamel refuses
    duplicate keys and the writer fell back to comment-destroying PyYAML).
    Dependency-free strict loader so this gate runs everywhere -- an
    importorskip on ruamel would silently skip in CI, where ruamel is not
    installed, which is exactly how the duplicate shipped."""

    class _StrictLoader(yaml.SafeLoader):
        pass

    def _reject_duplicates(loader, node, deep=False):
        seen = set()
        for key_node, _value in node.value:
            key = loader.construct_object(key_node, deep=deep)
            assert key not in seen, (
                f"duplicate key {key!r} at line {key_node.start_mark.line + 1}"
            )
            seen.add(key)
        return yaml.SafeLoader.construct_mapping(loader, node, deep)

    _StrictLoader.add_constructor(
        yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _reject_duplicates
    )
    for name in ("config.example.yaml", "secrets.example.yaml"):
        yaml.load(
            (REPO / name).read_text(encoding="utf-8"), Loader=_StrictLoader
        )
