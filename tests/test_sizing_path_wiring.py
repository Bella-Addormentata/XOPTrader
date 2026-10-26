"""Behavioural coverage for the offer-sizing path wiring.

The first version of these tests grepped source text. That passes whenever the
searched-for string exists ANYWHERE in the file, so a renamed method, a moved
call, or a call nested under the wrong condition all slipped through -- and two
of those were real defects in this PR. These exercise the behaviour instead.
"""

from __future__ import annotations

import os
import sys
import inspect
from pathlib import Path
from types import SimpleNamespace

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
        # [review] The warp worker's anchor (its jobs database) moves too.
        assert bridge._warp_svc.secrets_path == second.parent / "secrets.yaml"
        # [review] ...while the store the worker RECORDS in follows the jobs
        # database of the engine this GUI launched for as long as it runs --
        # the one its stuck-transaction guard reads, resolved from the
        # LAUNCHED config (directory and warp.jobs_db) at launch time.
        assert bridge._warp_svc.engine_store is None
        assert bridge._configured_warp_db() == str(second.parent / "data" / "warp_jobs.db")
        bridge._engine_process = SimpleNamespace(poll=lambda: None, pid=1)
        bridge._launched_config_path = first
        bridge._launched_warp_db = str(first.parent / "data" / "old.db")   # first's jobs_db
        bridge._sync_warp_engine_store()
        assert bridge._warp_svc.engine_store == str(first.parent / "data" / "old.db")
        bridge.update_config_path(str(first))                 # back and forth: unchanged
        bridge.update_config_path(str(second))
        assert bridge._warp_svc.engine_store == str(first.parent / "data" / "old.db")
        bridge._engine_process = None                        # the engine stopped
        bridge._sync_warp_engine_store()
        assert bridge._warp_svc.engine_store is None
        # [review] Before an engine starts here (and as the GUI switched
        # here), a previous engine's store with open work is named beside
        # this config's store, synchronously -- the engine's startup cleanup
        # must find the pointer, not race the worker's queued update.
        from gui.services.warp.jobs import WarpJobStore
        from gui.services.warp.service import _RETAINED_POINTER_SUFFIX

        old = first.parent / "data" / "old.db"
        with WarpJobStore(old) as store:
            store.create_job("mainnet", status="FUNDING_CLAIM", state={})
        bridge._publish_retained_warp_store()
        pointer = Path(bridge._configured_warp_db() + _RETAINED_POINTER_SUFFIX)
        assert pointer.read_text(encoding="utf-8").strip() == str(old.resolve())
        # [review] ...and what the PREVIOUS config's store pointed at is
        # carried along: a store kept across an earlier switch is not lost
        # at this one, even before the worker has reported a store.
        assert bridge._previous_warp_db == str(first.parent / "data" / "warp_jobs.db")
        kept_before = tmp_path / "x" / "warp_jobs.db"
        with WarpJobStore(kept_before) as store:
            store.create_job("mainnet", status="FUNDING_CLAIM", state={})
        Path(bridge._previous_warp_db + _RETAINED_POINTER_SUFFIX).write_text(
            f"{kept_before.resolve()}\n", encoding="utf-8")
        bridge._publish_retained_warp_store()
        assert pointer.read_text(encoding="utf-8").split() == [
            str(old.resolve()), str(kept_before.resolve())]
        src = inspect.getsource(EngineBridge.start_engine)
        assert src.index("_publish_retained_warp_store()") < src.index("_start_engine_process()")
        src = inspect.getsource(EngineBridge.update_config_path)
        assert "_publish_retained_warp_store()" in src
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
    from gui.widgets import permuto as permuto_mod
    from gui.widgets import settings as settings_mod

    class _UnregisteredInfo:
        registered = False
        link_attempted = False
        backup_confirmed = False
        listing_verified = False
        user_id = None
        trading_address = None
        pubkey = "ab" * 48
        created_at = None

    class _FakeIdentity:
        info = staticmethod(_UnregisteredInfo)

    real_identity = permuto_mod._default_identity_factory
    real_loader = settings_mod.load_startup_states
    # The class IS the factory -- classes are callable, so this keeps the
    # "call it to get an identity" contract without a wrapper lambda.
    permuto_mod._default_identity_factory = _FakeIdentity
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


# ---------------------------------------------------------------------------
# [review] an engine never starts over warp work it could not be told about
# ---------------------------------------------------------------------------

def test_an_unpublishable_retained_store_blocks_the_engine_launch(app, tmp_path, monkeypatch):
    """Open work in a store the new engine would not guard, and no way to
    name it for the guard: the launch does not happen, and the error says
    so -- never a start whose first cleanup could prune the kept sends."""
    from gui.services import engine_bridge as eb
    from gui.services.engine_bridge import EngineBridge
    from gui.services.warp.service import RetainedPointerError

    cfg = _write_config(tmp_path / "two", "data/xop_trader.db")
    bridge = EngineBridge(config_path=cfg)
    try:
        errors: list = []
        bridge.error.connect(errors.append)
        launched: list = []
        monkeypatch.setattr(bridge, "_start_engine_process",
                            lambda: launched.append(1) or True)

        def refuse(wanted, candidates):
            raise RetainedPointerError(
                "open work (job 1 (FUNDING_CLAIM)) in one/data/old.db could not be named")

        monkeypatch.setattr(eb, "publish_retained_pointer", refuse)
        bridge.start_engine()
        assert launched == [], "no launch while the kept store cannot be named"
        assert errors and "Engine not started" in errors[0] and "old.db" in errors[0]
        # Nothing to name (or named): the launch proceeds.
        monkeypatch.setattr(eb, "publish_retained_pointer", lambda wanted, candidates: None)
        bridge.start_engine()
        assert launched == [1]
    finally:
        bridge.deleteLater()
        app.processEvents()
