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


def _appid_guid() -> str:
    m = re.search(r"^AppId=\{\{([0-9A-Fa-f-]+)\}\}", ISS, re.MULTILINE)
    assert m, "AppId not found in installer.iss"
    return m.group(1).upper()


def test_uninstall_key_guid_matches_the_appid():
    guid = _appid_guid()
    # The [Code] lookup must target "<resolved AppId>_is1" with SINGLE braces.
    assert f"{{{guid}}}_is1".upper() in ISS.upper(), (
        "the hardcoded uninstall-registry key drifted from the AppId; "
        "PrepareToInstall would find no previous version and skip the "
        "clean uninstall-first"
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
