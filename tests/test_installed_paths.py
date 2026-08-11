"""Installed-mode path resolution: the v0.9.x installers put the app in
Program Files and every write (config bootstrap, engine.log, database)
targeted the exe directory, so on a normal non-elevated launch the engine
never started. These tests pin the per-user data-dir redirection and the
migration from a previously-elevated first run."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from gui import utils as U


def _freeze(monkeypatch, exe_dir: Path) -> None:
    """Simulate the frozen Windows install these tests describe.

    sys.platform is pinned to win32 so the LOCALAPPDATA branch is exercised
    deterministically on every CI platform -- without this, the Linux runner
    took the POSIX branch (~/.xoptrader) and the assertions were wrong there.
    """
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "platform", "win32")
    monkeypatch.setattr(sys, "executable", str(exe_dir / "xop_trader_gui.exe"))


# --------------------------------------------------------------------------- #
# user_data_dir / default_config_path
# --------------------------------------------------------------------------- #

def test_dev_behaviour_is_unchanged():
    assert U.user_data_dir() == U.install_dir()
    assert U.default_config_path() == Path("config.yaml").resolve()


def test_frozen_writable_install_dir_is_kept(tmp_path, monkeypatch):
    """Portable layouts (unzipped anywhere writable) keep state together."""
    exe_dir = tmp_path / "portable"
    exe_dir.mkdir()
    _freeze(monkeypatch, exe_dir)
    assert U.user_data_dir() == exe_dir
    assert U.default_config_path() == exe_dir / "config.yaml"


def test_frozen_unwritable_install_dir_redirects_per_user(tmp_path, monkeypatch):
    """The Program Files case: state must land in a per-user app-data dir."""
    exe_dir = tmp_path / "Program Files" / "XOPTrader"
    exe_dir.mkdir(parents=True)
    _freeze(monkeypatch, exe_dir)
    monkeypatch.setattr(U, "_dir_writable", lambda p: False)
    appdata = tmp_path / "AppData" / "Local"
    appdata.mkdir(parents=True)
    monkeypatch.setenv("LOCALAPPDATA", str(appdata))

    home = U.user_data_dir()
    assert home == appdata / "XOPTrader"
    assert home.is_dir(), "the data dir is created eagerly"
    assert U.default_config_path() == home / "config.yaml"


def test_frozen_unwritable_install_dir_posix_branch(tmp_path, monkeypatch):
    """The same redirect on POSIX lands in ~/.xoptrader."""
    exe_dir = tmp_path / "opt" / "xoptrader"
    exe_dir.mkdir(parents=True)
    monkeypatch.setattr(sys, "frozen", True, raising=False)
    monkeypatch.setattr(sys, "platform", "linux")
    monkeypatch.setattr(sys, "executable", str(exe_dir / "xop_trader_gui"))
    monkeypatch.setattr(U, "_dir_writable", lambda p: False)
    fake_home = tmp_path / "home"
    fake_home.mkdir()
    monkeypatch.setattr(U.Path, "home", staticmethod(lambda: fake_home))

    assert U.user_data_dir() == fake_home / ".xoptrader"


# --------------------------------------------------------------------------- #
# First-run bootstrap + migration from an elevated v0.9.x run
# --------------------------------------------------------------------------- #

def _installed_layout(tmp_path, monkeypatch):
    """Frozen app under an unwritable-flagged install dir, per-user home set."""
    exe_dir = tmp_path / "pf" / "XOPTrader"
    exe_dir.mkdir(parents=True)
    _freeze(monkeypatch, exe_dir)
    monkeypatch.setattr(U, "_dir_writable", lambda p: False)
    appdata = tmp_path / "local"
    appdata.mkdir()
    monkeypatch.setenv("LOCALAPPDATA", str(appdata))
    (exe_dir / "config.example.yaml").write_text(
        "warp:\n  enabled: false\n", encoding="utf-8"
    )
    return exe_dir, appdata / "XOPTrader"


def test_first_run_bootstraps_into_the_user_data_dir(tmp_path, monkeypatch):
    from gui import main as gui_main

    exe_dir, home = _installed_layout(tmp_path, monkeypatch)
    cfg_path, created = gui_main._bootstrap_config_info(None)
    assert cfg_path == home / "config.yaml"
    assert created and cfg_path.is_file(), \
        "the example must be copied into the WRITABLE home, not Program Files"
    assert "warp" in cfg_path.read_text(encoding="utf-8")


def test_elevated_first_run_state_is_migrated_not_reset(tmp_path, monkeypatch):
    """A v0.9.x install launched once elevated wrote real (possibly edited)
    config/secrets into Program Files; those move to the user home instead
    of being recreated from the examples."""
    from gui import main as gui_main

    exe_dir, home = _installed_layout(tmp_path, monkeypatch)
    (exe_dir / "config.yaml").write_text(
        "warp:\n  enabled: true   # operator edited\n", encoding="utf-8"
    )
    (exe_dir / "secrets.yaml").write_text(
        "warp:\n  evm_private_key_dpapi: LEGACY-BLOB\n", encoding="utf-8"
    )

    cfg_path, created = gui_main._bootstrap_config_info(None)
    assert cfg_path == home / "config.yaml"
    assert "operator edited" in cfg_path.read_text(encoding="utf-8"), \
        "the legacy config was migrated, not rebuilt from the example"
    assert "LEGACY-BLOB" in (home / "secrets.yaml").read_text(encoding="utf-8")
    assert (exe_dir / "config.yaml").is_file(), "the legacy copy is left intact"


def test_explicit_config_argument_still_wins(tmp_path, monkeypatch):
    from gui import main as gui_main

    _installed_layout(tmp_path, monkeypatch)
    explicit = tmp_path / "elsewhere" / "my.yaml"
    explicit.parent.mkdir()
    explicit.write_text("x: 1\n", encoding="utf-8")
    cfg_path, created = gui_main._bootstrap_config_info(explicit)
    assert cfg_path == explicit.resolve() and not created


# --------------------------------------------------------------------------- #
# EngineBridge: log path and launch dir must be writable when frozen
# --------------------------------------------------------------------------- #

def test_bridge_defaults_follow_the_config_home(tmp_path, monkeypatch):
    pytest.importorskip("PySide6")
    from gui.services.engine_bridge import EngineBridge

    exe_dir, home = _installed_layout(tmp_path, monkeypatch)
    home.mkdir(parents=True, exist_ok=True)
    (home / "config.yaml").write_text("warp:\n  enabled: false\n",
                                      encoding="utf-8")
    bridge = EngineBridge.__new__(EngineBridge)  # path logic only, no Qt init
    from gui.utils import default_config_path

    cfg = default_config_path()
    assert cfg == home / "config.yaml"
    # The launch dir (engine CWD, where data/ and logs/ are created) must be
    # the writable home, never the exe dir.
    bridge._config_path = cfg
    assert bridge._determine_engine_launch_dir(exe_dir / "xop_trader.exe") == home
    # And with NO config yet, frozen still refuses the exe dir.
    bridge._config_path = home / "missing.yaml"
    assert bridge._determine_engine_launch_dir(exe_dir / "xop_trader.exe") == home


# --------------------------------------------------------------------------- #
# XOP_CONFIG_PATH override and the warp jobs-DB anchor
# --------------------------------------------------------------------------- #

def test_xop_config_path_env_override_wins_everywhere(tmp_path, monkeypatch):
    """The durable way to point an installed build at an existing home
    (shortcut edits are reset by upgrades; the env var survives)."""
    target = tmp_path / "repo" / "config.yaml"
    target.parent.mkdir()
    target.write_text("x: 1\n", encoding="utf-8")
    monkeypatch.setenv("XOP_CONFIG_PATH", str(target))
    # Wins unfrozen...
    assert U.default_config_path() == target.resolve()
    # ...and frozen, over the user-data-dir default.
    exe_dir = tmp_path / "pf"
    exe_dir.mkdir()
    _freeze(monkeypatch, exe_dir)
    monkeypatch.setattr(U, "_dir_writable", lambda p: False)
    assert U.default_config_path() == target.resolve()


def test_warp_jobs_db_anchors_to_the_config_home(tmp_path):
    """The relative default (and relative explicit values) must resolve
    against the config home, never the process CWD -- for an installed
    build the CWD is the unwritable shortcut directory, so the warp job
    store could never open."""
    pytest.importorskip("PySide6")
    from gui.services.warp import service as S

    worker = S._WarpWorker(secrets_path=tmp_path / "home" / "secrets.yaml")
    worker.set_config({"warp": {}})
    default = Path(S._job_db_path(worker._config, anchor=worker._config_home()))
    assert default == tmp_path / "home" / "data" / "warp_jobs.db"

    worker.set_config({"warp": {"jobs_db": "custom/w.db"}})
    rel = Path(S._job_db_path(worker._config, anchor=worker._config_home()))
    assert rel == tmp_path / "home" / "custom" / "w.db"

    worker.set_config({"warp": {"jobs_db": str(tmp_path / "abs.db")}})
    abs_ = Path(S._job_db_path(worker._config, anchor=worker._config_home()))
    assert abs_ == tmp_path / "abs.db", "absolute values pass through untouched"
