"""One product, one version number.

The C++ engine's version (cpp/CMakeLists.txt project VERSION, compiled into
XOP_VERSION and printed by every engine.log startup line) drifted from the
app version in gui/__init__.py -- engine 0.8.0 while the app shipped 0.9.5.
During a live incident that stale-looking "XOPTrader v0.8.0" was read as
evidence of an old binary still running. Cheap to keep aligned, expensive
to misread, so pin it."""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def _gui_version() -> str:
    text = (REPO / "gui" / "__init__.py").read_text(encoding="utf-8")
    m = re.search(r'__version__:\s*str\s*=\s*"([^"]+)"', text)
    assert m, "gui/__init__.py __version__ not found"
    return m.group(1)


def _engine_version() -> str:
    text = (REPO / "cpp" / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(
        r"project\(\s*xop_trader\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        text,
        re.MULTILINE,
    )
    assert m, "cpp/CMakeLists.txt project VERSION not found"
    return m.group(1)


def _package_version() -> str:
    with open(REPO / "pyproject.toml", "rb") as stream:
        return str(tomllib.load(stream)["project"]["version"])


def test_product_versions_match():
    gui, engine, package = _gui_version(), _engine_version(), _package_version()
    assert gui == engine == package, (
        f"version drift: gui/__init__.py={gui}, cpp/CMakeLists.txt={engine}, "
        f"pyproject.toml={package}. engine.log prints the C++ version and Python "
        "packaging reads pyproject.toml, so bump all three together."
    )
