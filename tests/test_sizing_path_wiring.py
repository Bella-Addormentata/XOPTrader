"""Behavioural coverage for the offer-sizing path wiring.

The first version of these tests grepped source text. That passes whenever the
searched-for string exists ANYWHERE in the file, so a renamed method, a moved
call, or a call nested under the wrong condition all slipped through -- and two
of those were real defects in this PR. These exercise the behaviour instead.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


#: A config that passes ConfigService validation.  It MUST: an invalid one
#: makes switch_path() return False, and the database path is deliberately
#: only re-resolved on a SUCCESSFUL switch -- so an invalid fixture would
#: make this suite pass for the wrong reason.
_VALID_CONFIG = """chia:
  wallet_host: localhost
  wallet_port: 9256
pairs:
  - name: XCH/wUSDC.b
    base_asset_id: xch
    quote_asset_id: aa
    enabled: true
strategy:
  gamma: 0.1
  kappa: 1.5
  num_tiers: 3
  min_profit_margin_bps: 25
risk:
  soft_limit_pct: 0.6
  hard_limit_pct: 0.8
database:
  path: {db_rel}
"""


def _write_config(directory: Path, db_rel: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    cfg = directory / "config.yaml"
    cfg.write_text(_VALID_CONFIG.format(db_rel=db_rel), encoding="utf-8")
    return cfg


# ---------------------------------------------------------------------------
# B: a config switch must re-resolve the config-relative database path
# ---------------------------------------------------------------------------

def test_switching_config_moves_the_database_path(app, tmp_path):
    """_apply_configured_database_path ran only at construction.

    A switched config therefore left db_path in the PREVIOUS config's
    directory, handing consumers a new config paired with an old database.
    """
    from gui.services.engine_bridge import EngineBridge

    first = _write_config(tmp_path / "one", "data/xop_trader.db")
    second = _write_config(tmp_path / "two", "data/xop_trader.db")

    bridge = EngineBridge(config_path=first)
    try:
        before = bridge.db_path
        assert (tmp_path / "one") in before.parents, before

        bridge.update_config_path(str(second))
        assert bridge.config_service.path == second, (
            "the config switch itself did not take effect"
        )
        after = bridge.db_path

        assert after != before, "database path did not follow the config switch"
        assert (tmp_path / "two") in after.parents, (
            f"db path {after} still points at the previous config's directory"
        )
    finally:
        bridge.deleteLater()
        app.processEvents()


# ---------------------------------------------------------------------------
# A: the initial push must not depend on the Settings widget existing
# ---------------------------------------------------------------------------

def test_paths_reach_the_wallet_page_without_a_settings_widget(app, tmp_path):
    """SettingsWidget is a guarded import.

    The initial _push_sizing_paths() was nested inside the branch that
    handles it, so a failed Settings import silently left the calculator on
    its bundle-relative defaults -- the original fault.
    """
    from gui.widgets.main_window import MainWindow

    cfg = _write_config(tmp_path / "cfg", "data/xop_trader.db")

    class _Wallet:
        def __init__(self) -> None:
            self.got: tuple | None = None

        def set_sizing_paths(self, config_path, db_path) -> None:
            self.got = (config_path, db_path)

    # [PERMUTO MASTER SWITCH / v0.10.4 field report] This is the only test
    # that calls set_bridge on a REAL window, and set_bridge schedules the
    # Permuto startup arm. On a box whose operator registered an identity
    # and stored "Permuto: On at startup", that arm places live orders from
    # a test run. Seal both, exactly as
    # gui/services/permuto/tests/test_venue_control.py does.
    import gui.widgets.permuto as permuto_mod
    import gui.widgets.settings as settings_mod

    class _UnregisteredInfo:
        registered = False
        link_attempted = False
        backup_confirmed = False
        listing_verified = False
        user_id = None
        trading_address = None
        pubkey = "ab" * 48
        created_at = None

    real_identity = permuto_mod._default_identity_factory
    real_loader = settings_mod.load_startup_states
    permuto_mod._default_identity_factory = lambda: type(
        "_FakeIdentity", (), {"info": staticmethod(_UnregisteredInfo)})()
    settings_mod.load_startup_states = lambda: ("adopt", "off")

    window = MainWindow()
    try:
        from gui.services.engine_bridge import EngineBridge
        bridge = EngineBridge(config_path=cfg)
        wallet = _Wallet()
        window._settings_widget = None          # the guarded import failed
        window._wallet_balances = wallet

        window.set_bridge(bridge)

        assert wallet.got is not None, (
            "wallet page received no paths when Settings was unavailable"
        )
        cfg_arg, db_arg = wallet.got
        assert cfg_arg == str(cfg)
        assert db_arg and "xop_trader.db" in db_arg
        bridge.deleteLater()
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()
        permuto_mod._default_identity_factory = real_identity
        settings_mod.load_startup_states = real_loader


# ---------------------------------------------------------------------------
# The workers must actually forward what they are given
# ---------------------------------------------------------------------------

def test_the_allocation_worker_forwards_its_paths(app, monkeypatch):
    """Asserted by capturing the call, not by grepping for the argument name."""
    import gui.utils as utils
    from gui.widgets.wallet_balances import _SuggestedAllocationWorker

    seen = {}

    class _FakeSizing:
        @staticmethod
        def suggested_portfolio_allocation(config_path=None, db_path=None,
                                           **_kw):
            seen["config"] = config_path
            seen["db"] = db_path
            return {}

    monkeypatch.setattr(utils, "load_offer_sizing", lambda: _FakeSizing)

    worker = _SuggestedAllocationWorker("C:/cfg.yaml", "C:/db.sqlite")
    worker.run()

    assert seen == {"config": "C:/cfg.yaml", "db": "C:/db.sqlite"}


def test_the_targets_worker_forwards_its_paths(app, monkeypatch):
    import gui.utils as utils
    from gui.widgets.settings import _SuggestedTargetsWorker

    seen = {}

    class _FakeSizing:
        @staticmethod
        def compute_suggested_targets(config_path=None, db_path=None, **_kw):
            seen["config"] = config_path
            seen["db"] = db_path
            return {"pairs": {}}

    monkeypatch.setattr(utils, "load_offer_sizing", lambda: _FakeSizing)

    worker = _SuggestedTargetsWorker("C:/cfg.yaml", "C:/db.sqlite")
    worker.run()

    assert seen == {"config": "C:/cfg.yaml", "db": "C:/db.sqlite"}


# ---------------------------------------------------------------------------
# C: a falsy-but-present value must not reach the bundled default
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("value", [None, "", "   "])
def test_an_absent_or_blank_path_is_refused_in_a_bundle(value):
    """Both call sites read `_require_explicit(...) or DEFAULT`.

    Returning "" therefore fell through to the bundle-relative default while
    frozen, silently restoring the failure this guard exists to prevent.
    """
    import importlib.util

    root = Path(__file__).resolve().parent.parent
    spec = importlib.util.spec_from_file_location(
        "offer_sizing_blank_check", root / "scripts" / "offer_sizing.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["offer_sizing_blank_check"] = module
    spec.loader.exec_module(module)
    module._FROZEN = True

    with pytest.raises(RuntimeError):
        module._require_explicit("config", value)
