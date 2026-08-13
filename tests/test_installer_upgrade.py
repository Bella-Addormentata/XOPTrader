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


def test_running_app_is_closed_before_the_uninstall_first():
    """A 24/7 bot is normally RUNNING when it is upgraded, and Inno cannot
    delete a locked xop_trader_gui.exe -- so without closing the app first
    the clean uninstall-first silently degrades to an in-place overlay
    (observed on the 0.9.2 -> 0.9.5 upgrade). The helper must exist AND be
    called from PrepareToInstall BEFORE the uninstaller runs."""
    assert "procedure CloseRunningXOPTrader" in ISS
    for exe in ("xop_trader_gui.exe", "xop_trader.exe"):
        assert exe in ISS, f"{exe} is not terminated before the uninstall"
    assert "CloseApplications=yes" in ISS, "Restart Manager fallback missing"

    prep = ISS.split("function PrepareToInstall", 1)
    assert len(prep) == 2, "PrepareToInstall not found"
    body = prep[1]
    call = body.find("CloseRunningXOPTrader()")
    run_uninst = body.find("Exec(uninst")
    assert call != -1, "PrepareToInstall never closes the running app"
    assert run_uninst != -1, "PrepareToInstall never runs the old uninstaller"
    assert call < run_uninst, (
        "the app must be closed BEFORE the old uninstaller runs, otherwise "
        "locked files defeat the clean uninstall"
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


def test_code_section_uses_no_brace_comments():
    """Regression guard for the v0.9.4 compile failure. An Inno ``{ }`` comment
    ends at the FIRST ``}``, so a brace-comment whose prose contains a brace
    truncates and the rest becomes invalid Pascal -- iscc then emits no
    installer. Rather than police brace-in-comment (the truncation hides it),
    ban ``{`` comments in [Code] outright: in Inno Pascal a ``{`` outside a
    single-quoted string is ALWAYS a comment start (constants are written
    ExpandConstant('{app}')), so ``//`` is the safe, unambiguous choice."""
    body = ISS.split("[Code]", 1)
    assert len(body) == 2, "no [Code] section found"
    code = body[1]
    # Stop at the next Inno section header, or a later section's constants
    # (e.g. [Run]'s "{tmp}\\...") get scanned as if they were Pascal.
    nxt = re.search(r"^\[[A-Za-z][A-Za-z0-9_]*\]\s*$", code, re.MULTILINE)
    if nxt:
        code = code[:nxt.start()]
    i = 0
    while i < len(code):
        ch = code[i]
        if ch == "/" and code[i + 1: i + 2] == "/":            # // to EOL
            nl = code.find("\n", i)
            i = len(code) if nl == -1 else nl + 1
            continue
        if ch == "'":                                          # 'literal'
            j = code.find("'", i + 1)
            i = len(code) if j == -1 else j + 1
            continue
        assert ch != "{", (
            "a brace comment appears in [Code] near "
            + repr(code[i:i + 60])
            + " -- use // instead; brace comments truncate at the first inner "
            "brace and silently break iscc"
        )
        i += 1
