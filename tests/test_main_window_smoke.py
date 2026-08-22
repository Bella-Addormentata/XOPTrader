"""Construct the main window.

No test did this before, so the entire suite passed green while the GUI could
not start at all: an inserted method landed between an existing
``@staticmethod`` and ``_create_page_widget``, stealing the decorator. Every
page then received ``self`` as its ``widget_class`` and construction raised
``TypeError: _create_page_widget() got multiple values for argument
'scrollable'``.

A smoke test is a blunt instrument, but it is the one that catches a whole
class of wiring and decorator damage that unit tests around the edges miss.
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest  # noqa: E402

from PySide6.QtWidgets import QApplication  # noqa: E402


@pytest.fixture(scope="module")
def app():
    instance = QApplication.instance() or QApplication(sys.argv)
    instance.setStyle("Fusion")
    yield instance


def test_the_main_window_constructs(app):
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        assert window._stacked.count() > 0, "no pages were built"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_every_page_is_a_widget_not_a_bound_method_artifact(app):
    """The decorator bug produced pages built from the wrong argument.

    Asserting each page is a real QWidget catches that even if construction
    somehow survives.
    """
    from PySide6.QtWidgets import QWidget
    from gui.widgets.main_window import MainWindow

    window = MainWindow()
    try:
        for index in range(window._stacked.count()):
            page = window._stacked.widget(index)
            assert isinstance(page, QWidget), f"page {index} is {type(page)}"
    finally:
        window.close()
        window.deleteLater()
        app.processEvents()


def test_the_page_builder_stays_a_staticmethod():
    """It takes no self, but is called as self._create_page_widget(...).

    Without the decorator Python binds self to widget_class and every page
    is built from the wrong argument.
    """
    from gui.widgets.main_window import MainWindow

    assert isinstance(
        MainWindow.__dict__["_create_page_widget"], staticmethod
    ), "_create_page_widget lost its @staticmethod"
