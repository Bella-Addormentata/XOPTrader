"""Settings > Wallet Link: Chia wallet + optional Sage link.

Pins three things:
  * gui/services/wallet_link.py: detection of the Chia and Sage installs
    and the read-only probes, against a fake ``post`` (no network).
  * The settings page: the Sage link loads OFF when the config has no
    wallet_link section (every install made before this page), and the
    section round-trips through Save with the cert paths in secrets.yaml.
  * The Chia wallet keys are untouched by the move to the new page.

Runs headless (``QT_QPA_PLATFORM=offscreen``).
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

_REPO = Path(__file__).resolve().parents[1]
if str(_REPO) not in sys.path:
    sys.path.insert(0, str(_REPO))

import pytest  # noqa: E402
import yaml  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402

from gui.services import wallet_link  # noqa: E402
from gui.widgets.settings import SettingsWidget, _WalletProbeWorker  # noqa: E402


# ===================================================================
# Fakes
# ===================================================================

class _Resp:
    def __init__(self, data=None, status=200, text=""):
        self._data = data
        self.status_code = status
        self.text = text

    def json(self):
        if self._data is None:
            raise ValueError("not json")
        return self._data


def _fake_post(routes: dict, calls: list | None = None):
    def post(url, json=None, cert=None, verify=None, timeout=None,
             allow_redirects=True):
        endpoint = url.rsplit("/", 1)[-1]
        if calls is not None:
            calls.append((url, json, cert, verify))
        reply = routes[endpoint]
        if isinstance(reply, Exception):
            raise reply
        return reply
    return post


@pytest.fixture
def certs(tmp_path):
    cert = tmp_path / "wallet.crt"
    key = tmp_path / "wallet.key"
    cert.write_text("cert")
    key.write_text("key")
    return str(cert), str(key)


# ===================================================================
# Detection
# ===================================================================

def test_chia_root_prefers_chia_root_env(tmp_path):
    root = tmp_path / "custom"
    (root / "config" / "ssl").mkdir(parents=True)
    assert wallet_link.chia_root({"CHIA_ROOT": str(root)}) == root


def test_detect_chia_wallet_reads_port_certs_and_fingerprint(tmp_path):
    root = tmp_path / "mainnet"
    ssl = root / "config" / "ssl" / "wallet"
    ssl.mkdir(parents=True)
    (ssl / "private_wallet.crt").write_text("c")
    (ssl / "private_wallet.key").write_text("k")
    (root / "config" / "config.yaml").write_text(
        "wallet:\n  rpc_port: 19256\n", encoding="utf-8")
    (root / "wallet" / "db").mkdir(parents=True)
    (root / "wallet" / "db" / "last_used_fingerprint").write_text("123456")

    found = wallet_link.detect_chia_wallet(root)
    assert found["found"] is True
    assert found["port"] == 19256
    assert found["cert_path"].endswith("ssl/wallet/private_wallet.crt")
    assert found["key_path"].endswith("ssl/wallet/private_wallet.key")
    assert found["fingerprint"] == 123456


def test_detect_chia_wallet_without_certs_is_not_found(tmp_path):
    root = tmp_path / "mainnet"
    (root / "config" / "ssl").mkdir(parents=True)
    assert wallet_link.detect_chia_wallet(root)["found"] is False


@pytest.mark.parametrize(
    "platform, env, expected",
    [
        ("win32", {"APPDATA": "C:/Users/u/AppData/Roaming"},
         "C:/Users/u/AppData/Roaming/com.rigidnetwork.sage"),
        ("darwin", {},
         "/home/u/Library/Application Support/com.rigidnetwork.sage"),
        ("linux", {}, "/home/u/.local/share/com.rigidnetwork.sage"),
        ("linux", {"XDG_DATA_HOME": "/data"}, "/data/com.rigidnetwork.sage"),
    ],
)
def test_sage_data_dirs_per_platform(platform, env, expected):
    dirs = wallet_link.sage_data_dirs(platform, env, Path("/home/u"))
    assert dirs[0].as_posix() == expected


def test_detect_sage_reads_rpc_settings(tmp_path):
    data = tmp_path / "com.rigidnetwork.sage"
    (data / "ssl").mkdir(parents=True)
    (data / "ssl" / "wallet.crt").write_text("c")
    (data / "ssl" / "wallet.key").write_text("k")
    (data / "config.toml").write_text(
        "version = 2\n\n[rpc]\nenabled = true\nport = 9333\n")

    found = wallet_link.detect_sage([data])
    assert found["found"] is True
    assert found["port"] == 9333
    assert found["rpc_enabled"] is True
    assert found["cert_path"].endswith("ssl/wallet.crt")


def test_detect_sage_defaults_when_rpc_section_missing(tmp_path):
    data = tmp_path / "sage"
    (data / "ssl").mkdir(parents=True)
    (data / "ssl" / "wallet.crt").write_text("c")
    (data / "ssl" / "wallet.key").write_text("k")
    (data / "config.toml").write_text("version = 2\n")

    found = wallet_link.detect_sage([data])
    assert found["port"] == 9257
    assert found["rpc_enabled"] is False


def test_detect_sage_not_installed(tmp_path):
    assert wallet_link.detect_sage([tmp_path / "nope"]) == {"found": False}


# ===================================================================
# Probes
# ===================================================================

def test_probe_sage_success(certs):
    calls: list = []
    post = _fake_post({
        "get_version": _Resp({"version": "0.12.7"}),
        "get_key": _Resp({"key": {"fingerprint": 42, "name": "Main"}}),
        "get_sync_status": _Resp({
            "selectable_balance": "2500000000000",
            "unit": {"ticker": "XCH", "precision": 12},
            "synced_coins": 10, "total_coins": 10,
        }),
    }, calls)
    result = wallet_link.probe_sage("127.0.0.1", 9257, *certs, post=post)

    assert result.ok is True
    assert result.details["fingerprint"] == 42
    assert result.details["xch_mojos"] == 2_500_000_000_000
    assert result.details["synced"] is True
    assert "2.5000 XCH" in result.summary
    # mutual TLS with the wallet's own cert; no server-cert verification
    url, _body, cert, verify = calls[0]
    assert url == "https://127.0.0.1:9257/get_version"
    assert cert == certs and verify is False


def test_probe_sage_no_wallet_logged_in(certs):
    post = _fake_post({
        "get_version": _Resp({"version": "0.12.7"}),
        "get_key": _Resp({"key": None}),
    })
    result = wallet_link.probe_sage("127.0.0.1", 9257, *certs, post=post)
    assert result.ok is False
    assert "no wallet is logged in" in result.summary


def test_probe_sage_connection_refused_mentions_rpc_toggle(certs):
    post = _fake_post({"get_version": ConnectionError("refused")})
    result = wallet_link.probe_sage("127.0.0.1", 9257, *certs, post=post)
    assert result.ok is False
    assert "RPC server turned on" in result.summary


def test_probe_sage_401(certs):
    post = _fake_post({"get_version": _Resp(status=401, text="Unauthorized")})
    result = wallet_link.probe_sage("127.0.0.1", 9257, *certs, post=post)
    assert result.ok is False and "401" in result.summary


def test_probe_missing_cert_files_never_calls_out(tmp_path):
    def post(*_a, **_k):
        raise AssertionError("must not connect without cert files")
    result = wallet_link.probe_sage(
        "127.0.0.1", 9257, str(tmp_path / "x.crt"), str(tmp_path / "x.key"),
        post=post)
    assert result.ok is False and "not found" in result.summary
    assert wallet_link.probe_chia_wallet(
        "localhost", 9256, "", "", post=post).ok is False


def test_probe_chia_wallet_success(certs):
    post = _fake_post({
        "get_sync_status": _Resp({"success": True, "synced": True, "syncing": False}),
        "get_logged_in_fingerprint": _Resp({"success": True, "fingerprint": 77}),
        "get_wallet_balance": _Resp({"success": True, "wallet_balance": {
            "confirmed_wallet_balance": 1_000_000_000_000}}),
    })
    result = wallet_link.probe_chia_wallet("localhost", 9256, *certs, post=post)
    assert result.ok is True
    assert result.summary == "Connected, synced, fingerprint 77, 1.0000 XCH"


def test_probe_chia_wallet_in_band_error(certs):
    post = _fake_post({
        "get_sync_status": _Resp({"success": False, "error": "not logged in"}),
    })
    result = wallet_link.probe_chia_wallet("localhost", 9256, *certs, post=post)
    assert result.ok is False and "not logged in" in result.summary


# ===================================================================
# Settings page
# ===================================================================

@pytest.fixture(scope="session")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def panel(qapp, monkeypatch):
    monkeypatch.setattr(
        SettingsWidget, "_refresh_suggested_targets", lambda self: None
    )
    w = SettingsWidget()
    yield w
    w.stop_background_work()
    w.deleteLater()


@pytest.fixture
def cfg_path(tmp_path):
    dest = tmp_path / "config.yaml"
    shutil.copyfile(_REPO / "config.example.yaml", dest)
    return dest


def test_page_is_registered(panel):
    assert panel._tab_titles[12] == "Wallet Link"


def test_shipped_default_is_sage_off(panel, cfg_path):
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    assert raw["wallet_link"]["sage_enabled"] is False
    panel.load_config(str(cfg_path))
    assert panel._sage_enabled.isChecked() is False
    assert panel._sage_port.value() == 9257


def test_existing_install_without_section_loads_off(panel, cfg_path):
    """Dorkmo's config predates the page: no wallet_link at all."""
    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    raw.pop("wallet_link")
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")

    panel.load_config(str(cfg_path))
    assert panel._sage_enabled.isChecked() is False
    assert panel._sage_host.text() == "127.0.0.1"
    assert panel._sage_port.value() == 9257
    assert panel._sage_cert.text() == ""


def test_round_trip_puts_sage_paths_in_secrets(panel, cfg_path, certs):
    panel.load_config(str(cfg_path))
    panel._sage_enabled.setChecked(True)
    panel._sage_cert.setText(certs[0])
    panel._sage_key.setText(certs[1])
    assert panel._tab_dirty[12] is True
    assert panel.save_config(str(cfg_path)) is True

    public = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    secrets = yaml.safe_load(
        (cfg_path.parent / "secrets.yaml").read_text(encoding="utf-8"))
    assert public["wallet_link"]["sage_enabled"] is True
    assert "sage_cert_path" not in public["wallet_link"]
    assert secrets["wallet_link"]["sage_cert_path"] == certs[0]
    assert secrets["wallet_link"]["sage_key_path"] == certs[1]

    fresh = SettingsWidget()
    try:
        fresh._refresh_suggested_targets = lambda: None  # type: ignore[method-assign]
        fresh.load_config(str(cfg_path))
        assert fresh._sage_enabled.isChecked() is True
        assert fresh._sage_cert.text() == certs[0]
    finally:
        fresh.deleteLater()


def test_chia_wallet_keys_unchanged_by_the_move(panel, cfg_path):
    panel.load_config(str(cfg_path))
    panel._wl_port.setValue(19256)
    assert panel._tab_dirty[12] is True
    chia = panel._collect_config_dict()["chia"]
    assert chia["wallet_port"] == 19256
    for key in ("wallet_host", "wallet_cert_path", "wallet_key_path",
                "wallet_fingerprint"):
        assert key in chia


def test_detect_fills_sage_fields(panel, cfg_path, tmp_path, monkeypatch):
    panel.load_config(str(cfg_path))
    monkeypatch.setattr(wallet_link, "detect_sage", lambda: {
        "found": True, "data_dir": "/d", "cert_path": "/d/ssl/wallet.crt",
        "key_path": "/d/ssl/wallet.key", "port": 9300, "rpc_enabled": False,
    })
    panel._on_wallet_link_detect("sage")
    assert panel._sage_cert.text() == "/d/ssl/wallet.crt"
    assert panel._sage_port.value() == 9300
    assert "turned off" in panel._sage_test_result.text()


def test_detect_not_found_leaves_fields(panel, cfg_path, monkeypatch):
    panel.load_config(str(cfg_path))
    panel._sage_cert.setText("/keep/me.crt")
    monkeypatch.setattr(wallet_link, "detect_sage", lambda: {"found": False})
    panel._on_wallet_link_detect("sage")
    assert panel._sage_cert.text() == "/keep/me.crt"
    assert "not found" in panel._sage_test_result.text()


def test_probe_worker_reports_result(qapp, monkeypatch):
    monkeypatch.setattr(
        wallet_link, "probe_sage",
        lambda **kw: wallet_link.ProbeResult(True, f"ok {kw['port']}"))
    got: list = []
    worker = _WalletProbeWorker("sage", {
        "host": "127.0.0.1", "port": 9257, "cert_path": "c", "key_path": "k"})
    worker.done.connect(lambda *a: got.append(a))
    worker.run()
    assert got == [("sage", True, "ok 9257")]


def test_probe_done_updates_label_and_reenables(panel):
    panel._sage_test_btn.setEnabled(False)
    panel._on_wallet_probe_done("sage", False, "Nothing answered")
    assert panel._sage_test_btn.isEnabled() is True
    assert panel._sage_test_result.text() == "Nothing answered"


# ===================================================================
# Review follow-ups (PR #188)
# ===================================================================

def test_expand_path_env_refs():
    env = {"CHIA_ROOT": "/srv/chia"}
    assert wallet_link.expand_path(
        "${CHIA_ROOT}/config/ssl/wallet/private_wallet.crt", env
    ) == "/srv/chia/config/ssl/wallet/private_wallet.crt"
    assert wallet_link.expand_path("${NOPE:-/d}/x.crt", env) == "/d/x.crt"
    # unset, no default: left as written so the error shows it
    assert wallet_link.expand_path("${NOPE}/x.crt", env) == "${NOPE}/x.crt"


def test_probe_resolves_env_ref_cert_paths(tmp_path, monkeypatch):
    (tmp_path / "w.crt").write_text("c")
    (tmp_path / "w.key").write_text("k")
    monkeypatch.setenv("XOP_TEST_SSL", str(tmp_path))
    calls: list = []
    post = _fake_post({
        "get_sync_status": _Resp({"synced": True}),
        "get_logged_in_fingerprint": _Resp({"fingerprint": 1}),
        "get_wallet_balance": _Resp({"wallet_balance": {}}),
    }, calls)
    result = wallet_link.probe_chia_wallet(
        "localhost", 9256, "${XOP_TEST_SSL}/w.crt", "${XOP_TEST_SSL}/w.key",
        post=post)
    assert result.ok is True
    assert calls[0][2] == (str(tmp_path / "w.crt"), str(tmp_path / "w.key"))


def test_chia_probe_honours_verify_ssl(certs, tmp_path):
    ca = tmp_path / "ca.crt"
    ca.write_text("ca")
    routes = {
        "get_sync_status": _Resp({"synced": True}),
        "get_logged_in_fingerprint": _Resp({"fingerprint": 1}),
        "get_wallet_balance": _Resp({"wallet_balance": {}}),
    }
    calls: list = []
    wallet_link.probe_chia_wallet("localhost", 9256, *certs,
                                  post=_fake_post(routes, calls))
    assert calls[0][3] is False                      # default: off

    calls.clear()
    wallet_link.probe_chia_wallet("node", 9256, *certs, verify_ssl=True,
                                  ca_cert_path=str(ca),
                                  post=_fake_post(routes, calls))
    assert calls[0][3] == str(ca)                    # pinned to the CA

    calls.clear()
    wallet_link.probe_chia_wallet("node", 9256, *certs, verify_ssl=True,
                                  post=_fake_post(routes, calls))
    assert calls[0][3] is True                       # system bundle

    missing = wallet_link.probe_chia_wallet(
        "node", 9256, *certs, verify_ssl=True,
        ca_cert_path=str(tmp_path / "nope.crt"), post=_fake_post(routes))
    assert missing.ok is False and "CA file" in missing.summary


def test_probe_sage_zero_precision_unit(certs):
    post = _fake_post({
        "get_version": _Resp({"version": "0.12.7"}),
        "get_key": _Resp({"key": {"fingerprint": 42, "name": ""}}),
        "get_sync_status": _Resp({
            "selectable_balance": 1_000_000,
            "unit": {"ticker": "Mojos", "precision": 0},
            "synced_coins": 1, "total_coins": 1,
        }),
    })
    result = wallet_link.probe_sage("127.0.0.1", 9257, *certs, post=post)
    assert "1,000,000.0000 Mojos spendable" in result.summary


def test_fingerprint_above_int32_round_trips(panel, cfg_path, monkeypatch):
    big = 3_000_000_000
    panel.load_config(str(cfg_path))
    monkeypatch.setattr(wallet_link, "detect_chia_wallet", lambda: {
        "found": True, "root": "/r", "port": 9256, "cert_path": "/r/c.crt",
        "key_path": "/r/c.key", "fingerprint": big,
    })
    panel._wl_fingerprint.setValue(0)
    panel._on_wallet_link_detect("chia")
    assert panel._collect_config_dict()["chia"]["wallet_fingerprint"] == big

    raw = yaml.safe_load(cfg_path.read_text(encoding="utf-8"))
    raw["chia"]["wallet_fingerprint"] = 4_294_967_295
    cfg_path.write_text(yaml.safe_dump(raw), encoding="utf-8")
    panel.load_config(str(cfg_path))
    assert panel._collect_config_dict()["chia"]["wallet_fingerprint"] == 4_294_967_295
