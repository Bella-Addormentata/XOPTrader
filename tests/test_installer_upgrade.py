"""Static guards on the Windows installer's upgrade handling.

The full behaviour (install, upgrade-over, single registration) is exercised
by the release workflow's smoke test on a real Windows runner. These
dependency-free checks catch the one thing that test cannot: a silent drift
between the [Setup] AppId and the hardcoded uninstall-registry GUID in the
[Code] section, which would make PrepareToInstall find nothing and quietly
skip the clean-uninstall-first."""

from __future__ import annotations

import re
from pathlib import Path

ISS = (
    Path(__file__).resolve().parents[1]
    / "packaging" / "windows" / "installer.iss"
).read_text(encoding="utf-8")


def _resolved_appid() -> str:
    """The AppId Inno actually registers, per its escaping rule.

    A leading ``{{`` collapses to a single ``{``; every other brace is
    literal. So ``AppId={{GUID}}`` registers as ``{GUID}}`` -- single
    leading brace, DOUBLE trailing brace -- which is what the live registry
    shows. Deriving it here (rather than eyeballing braces) is exactly what
    would have caught the single-``}`` lookup bug."""
    m = re.search(r"^AppId=(\S+)", ISS, re.MULTILINE)
    assert m, "AppId not found in installer.iss"
    raw = m.group(1)
    return (raw[1:] if raw.startswith("{{") else raw).upper()


def test_uninstall_key_matches_the_resolved_appid():
    key = f"{_resolved_appid()}_is1".upper()
    assert key in ISS.upper(), (
        f"the hardcoded uninstall-registry key must be {key!r} "
        "(resolved AppId + _is1); a drift here makes PrepareToInstall find "
        "no previous version and silently skip the clean uninstall-first"
    )


def test_previous_version_is_uninstalled_before_install():
    # The upgrade handler exists and runs the old uninstaller silently.
    assert "function PrepareToInstall" in ISS
    assert "GetPreviousUninstaller" in ISS
    assert "/VERYSILENT" in ISS and "SUPPRESSMSGBOXES" in ISS
    # It must not hard-fail the upgrade if the old uninstaller is broken --
    # the file copy overwrites the shipped payload regardless.
    assert "ewWaitUntilTerminated" in ISS


def test_wizard_image_is_not_an_ico_again():
    # Regression guard for the v0.9.0 "Bitmap image is not valid" crash.
    assert "WizardSmallImageFile=icon.ico" not in ISS
    assert "WizardImageFile=icon.ico" not in ISS
