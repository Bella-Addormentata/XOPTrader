"""Worker threads must be joined before their widgets are destroyed.

Qt calls qFatal("QThread: Destroyed while thread is still running") and the
process ABORTS. Reproduced against PySide6 6.11: a run that closes a window
while a parented QThread is mid-work never returns from main().

This matters because of what the sizing-path fix changed. Before it, a frozen
build passed no config path, so the worker raised FileNotFoundError within
microseconds and was effectively never alive. With real paths the config load
succeeds and the worker goes on to a dexie fetch with a 30 second timeout --
a wide window in which a close aborts the application.
"""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


def test_a_running_qthread_destroyed_by_teardown_aborts_the_process():
    """Pins the mechanism, in a subprocess because it kills the interpreter.

    If a future Qt stops aborting here, this fails and the guard below can be
    reconsidered -- rather than being carried forever on faith.
    """
    script = textwrap.dedent(
        """
        import os, sys, time
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PySide6.QtCore import QThread, QObject, Slot, QTimer
        from PySide6.QtWidgets import QApplication, QWidget

        class W(QObject):
            @Slot()
            def run(self):
                time.sleep(3)

        def main():
            app = QApplication(sys.argv)
            host = QWidget()
            t = QThread(host); w = W(); w.moveToThread(t)
            t.started.connect(w.run); t.start()
            QTimer.singleShot(100, host.close)
            QTimer.singleShot(300, app.quit)
            app.exec()

        main()
        print("CLEAN_EXIT")
        """
    )
    result = subprocess.run([sys.executable, "-c", script],
                            capture_output=True, text=True, timeout=90)
    assert "CLEAN_EXIT" not in result.stdout, (
        "Qt no longer aborts on this construct; revisit stop_background_work"
    )
    assert result.returncode != 0


@pytest.mark.parametrize("widget_name", ["wallet", "settings"])
def test_the_widget_can_join_its_worker(app, widget_name):
    """stop_background_work must return with the thread finished."""
    from PySide6.QtCore import QThread

    if widget_name == "wallet":
        from gui.widgets.wallet_balances import WalletBalancesWidget
        widget = WalletBalancesWidget()
        widget._refresh_suggested_allocation = lambda: None
    else:
        from gui.widgets.settings import SettingsWidget
        widget = SettingsWidget()

    thread = QThread(widget)
    thread.start()
    widget._suggest_thread = thread

    widget.stop_background_work(timeout_ms=2000)

    assert widget._suggest_thread is None, "reference not cleared"
    assert not thread.isRunning(), "thread still running after join"


def test_stopping_with_no_worker_is_harmless(app):
    from gui.widgets.wallet_balances import WalletBalancesWidget

    widget = WalletBalancesWidget()
    widget._refresh_suggested_allocation = lambda: None
    widget._suggest_thread = None
    widget.stop_background_work()          # must not raise


def test_the_main_window_joins_its_pages_on_close():
    """Child widgets do NOT receive closeEvent when the top level closes.

    So the join has to be driven from MainWindow; asserting the call exists
    there is the point, since the widgets' own closeEvent would never fire.
    """
    source = Path(__file__).resolve().parent.parent.joinpath(
        "gui", "widgets", "main_window.py").read_text(encoding="utf-8")
    body = source.split("def closeEvent")[1]
    assert "stop_background_work" in body, (
        "closeEvent does not join the pages' worker threads"
    )
