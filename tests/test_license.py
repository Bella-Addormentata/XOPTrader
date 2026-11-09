"""The project's license, and the notices that travel with what is not ours.

XOPTrader is WTFPL (LICENSE). A few files under gui/services/warp/ carry code
ported or copied from warp.green (MIT), Chia (Apache-2.0) and Pieter Wuille's
bech32 (MIT). That code keeps its own license, and MIT and Apache-2.0 both
require their notice to go with every copy, so THIRD_PARTY_NOTICES.md holds
the notices and the release builds ship it next to LICENSE.

These checks pin what drifts silently: a vendored puzzle that never reaches
the notices, a packaging edit that stops shipping them, a metadata field that
names another license, or an SPDX header that says MIT out of habit (two did,
before the project had a license at all)."""

from __future__ import annotations

import importlib.util
import json
import re
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LICENSE = REPO / "LICENSE"
NOTICES = REPO / "THIRD_PARTY_NOTICES.md"
PUZZLES = REPO / "gui" / "services" / "warp" / "puzzles"


def _notices() -> str:
    return NOTICES.read_text(encoding="utf-8")


def _registered_puzzles() -> set[str]:
    """Names in the tree-hash table, loaded straight from hashes.py.

    Importing it as gui.services.warp.puzzles.hashes would run
    gui/services/__init__.py, which needs PySide6; these checks need nothing
    beyond the standard library."""
    spec = importlib.util.spec_from_file_location("_puzzle_hashes", PUZZLES / "hashes.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return set(module.EXPECTED_TREE_HASHES)


def test_license_file_is_the_wtfpl_version_2():
    text = " ".join(LICENSE.read_text(encoding="utf-8").split())
    assert text.startswith("DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE Version 2, December 2004")
    assert text.endswith("0. You just DO WHAT THE FUCK YOU WANT TO.")


def test_package_metadata_names_the_license_and_ships_both_files():
    with open(REPO / "pyproject.toml", "rb") as stream:
        meta = tomllib.load(stream)
    project = meta["project"]
    # WTFPL for XOPTrader, plus the licenses of the ported code the package
    # carries (gui/services/warp): see THIRD_PARTY_NOTICES.md.
    assert project["license"] == "WTFPL AND MIT AND Apache-2.0"
    assert set(project["license-files"]) == {"LICENSE", "THIRD_PARTY_NOTICES.md"}
    # setuptools < 77 rejects a string `license` and [project] license-files.
    floors = [
        req for req in meta["build-system"]["requires"] if req.startswith("setuptools")
    ]
    assert floors == ["setuptools>=77.0"], floors


def test_engine_manifest_names_the_license():
    manifest = json.loads((REPO / "cpp" / "vcpkg.json").read_text(encoding="utf-8"))
    assert manifest["license"] == "WTFPL"


def test_every_vendored_puzzle_is_in_the_notices():
    notices = _notices()
    on_disk = {path.name for path in PUZZLES.glob("*.clsp.hex")}
    registered = {f"{name}.clsp.hex" for name in _registered_puzzles()}
    assert on_disk == registered
    # Named on its own (`cat_v2.clsp.hex`) or at the end of a full path.
    missing = sorted(
        name for name in on_disk if not re.search(rf"[`/]{re.escape(name)}`", notices)
    )
    assert not missing, f"vendored puzzles without a notice: {missing}"


def test_spdx_headers_say_wtfpl_unless_the_file_is_third_party():
    notices = _notices()
    roots = ["cpp/include", "cpp/src", "cpp/tests", "gui", "scripts", "tests"]
    suffixes = {".py", ".cpp", ".hpp", ".h", ".inc"}
    wrong = []
    for root in roots:
        for path in (REPO / root).rglob("*"):
            if path.suffix not in suffixes or not path.is_file():
                continue
            with open(path, encoding="utf-8", errors="replace") as stream:
                head = [stream.readline() for _ in range(5)]
            for line in head:
                m = re.search(r"SPDX-License-Identifier:\s*(.+?)\s*(?:\*/)?$", line)
                if not m:
                    continue
                rel = path.relative_to(REPO).as_posix()
                if m.group(1) != "WTFPL" and f"`{rel}`" not in notices:
                    wrong.append(f"{rel}: {m.group(1)}")
    assert not wrong, f"SPDX headers that contradict LICENSE: {wrong}"


def test_release_builds_ship_the_license_and_notices():
    iss = (REPO / "packaging" / "windows" / "installer.iss").read_text(encoding="utf-8")
    release = (REPO / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    for name, installed in (("LICENSE", "LICENSE.txt"),
                            ("THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.txt")):
        # The installer lists it, the release job stages it for iscc, and the
        # smoke test proves it landed.
        assert re.search(
            rf'^Source: "{re.escape(name)}"; DestDir: "{{app}}"; '
            rf'DestName: "{re.escape(installed)}";',
            iss, re.MULTILINE,
        ), f"installer.iss does not install {name}"
        assert re.search(
            rf"^\s*Copy-Item {re.escape(name)}\s+installer-staging\\$", release, re.MULTILINE
        ), f"release.yml does not stage {name} for the installer"
        assert f'"{installed}"' in release, f"installer smoke test does not check {installed}"
        # Every GUI binary embeds it (Unix leg, Windows leg) and the Linux
        # bundle carries it.
        assert f'--add-data "../{name}:."' in release
        assert f"'--add-data', '../{name};.'" in release
        assert re.search(
            rf"^\s*cp {re.escape(name)}\s+linux-bundle/$", release, re.MULTILINE
        ), f"release.yml does not put {name} in the Linux bundle"
