"""One product, one version number.

The C++ engine's version (cpp/CMakeLists.txt project VERSION, compiled into
XOP_VERSION and printed by every engine.log startup line) drifted from the
app version in gui/__init__.py -- engine 0.8.0 while the app shipped 0.9.5.
During a live incident that stale-looking "XOPTrader v0.8.0" was read as
evidence of an old binary still running. Cheap to keep aligned, expensive
to misread, so pin it.

CHANGELOG.md is the fourth face of the same number. Release 0.10.13's notes
landed while all three sources still read 0.10.12, and the agree-with-each-
other check above passed precisely because they agreed -- on the stale value.
The changelog says which code you are running; the engine banner says which
code you think you are running. Same misread, different cause, so the top
changelog entry is pinned too."""

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


def _changelog_version() -> str:
    """Version of the top-most *released* entry in CHANGELOG.md.

    Headings look like `## [0.10.13] - 2026-09-01 - one side of a book ...`
    (em dashes in the file), so only the bracketed tag is parsed -- the date
    and title are free text. This changelog keeps no "Unreleased" section
    today, but Keep a Changelog sanctions one and a legitimate Unreleased
    heading must not turn CI red, so skip it and read the release beneath.
    """
    text = (REPO / "CHANGELOG.md").read_text(encoding="utf-8")
    tags = [t.strip() for t in re.findall(r"^##\s*\[([^\]]+)\]", text, re.MULTILINE)]
    released = [t for t in tags if t.lower() != "unreleased"]
    assert released, "CHANGELOG.md has no '## [<version>] ...' release heading"
    top = released[0]
    assert re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", top), (
        f"CHANGELOG.md top release heading is '## [{top}]', which is not an "
        "X.Y.Z version -- the version-sync test cannot pin an unparseable heading."
    )
    return top


def test_product_versions_match():
    gui, engine, package = _gui_version(), _engine_version(), _package_version()
    assert gui == engine == package, (
        f"version drift: gui/__init__.py={gui}, cpp/CMakeLists.txt={engine}, "
        f"pyproject.toml={package}. engine.log prints the C++ version and Python "
        "packaging reads pyproject.toml, so bump all three together."
    )


def test_product_versions_match_changelog():
    changelog = _changelog_version()
    stale = [
        f"{path}={value}"
        for path, value in (
            ("gui/__init__.py", _gui_version()),
            ("cpp/CMakeLists.txt", _engine_version()),
            ("pyproject.toml", _package_version()),
        )
        if value != changelog
    ]
    assert not stale, (
        f"CHANGELOG.md's newest entry is [{changelog}] but {', '.join(stale)}. "
        f"Bump the listed file(s) to {changelog}: writing the release notes is "
        "not the release, and shipping this way makes the binary announce "
        "itself as a version it is not, which is exactly what a live incident "
        "reads as a stale build."
    )
