; =============================================================================
; installer.iss  —  XOPTrader Windows installer (Inno Setup 6)
;
; Wraps the PyInstaller-built xop_trader_gui.exe in a proper Windows
; installer that:
;   • Installs to Program Files\XOPTrader
;   • Creates a Start-Menu shortcut
;   • Optionally creates a Desktop shortcut (checkbox, unchecked by default)
;   • Ships an uninstaller
;
; Build with:
;   iscc /DAppVersion=0.1 installer.iss
; =============================================================================

#ifndef AppVersion
  #define AppVersion "0.1"
#endif

[Setup]
AppId={{B4E3A1C2-7D56-4F89-A012-9E3C0B5D7F21}}
AppName=XOPTrader
AppVersion={#AppVersion}
AppPublisher=XOPTrader Project
AppPublisherURL=https://github.com/dorkmo/XOPTrader
AppSupportURL=https://github.com/dorkmo/XOPTrader/issues
AppUpdatesURL=https://github.com/dorkmo/XOPTrader/releases
DefaultDirName={autopf}\XOPTrader
DefaultGroupName=XOPTrader
; ---- Icon ----
SetupIconFile=icon.ico
UninstallDisplayIcon={app}\xop_trader_gui.exe
; ---- Output ----
OutputDir=.
OutputBaseFilename=xop_trader-installer-windows-x64
; ---- Compression ----
Compression=lzma2/ultra64
SolidCompression=yes
; ---- UI ----
WizardStyle=modern
; NOTE: no WizardSmallImageFile. Inno accepts only BMP (or PNG on 6.3+)
; there -- an .ico compiles fine (embedded raw) and then kills the
; installer AT LAUNCH with "Bitmap image is not valid". The default
; built-in wizard image is used instead; if branding is wanted later,
; generate a real BMP/PNG, never point this at icon.ico. (v0.9.0's
; installer shipped broken exactly this way; the smoke test in
; release.yml now runs every built installer so this class cannot
; reach a release again.)
; ---- Misc ----
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

; =============================================================================
; Tasks — the "Create desktop shortcut" checkbox
; =============================================================================
[Tasks]
Name: "desktopicon"; \
  Description: "{cm:CreateDesktopIcon}"; \
  GroupDescription: "{cm:AdditionalIcons}"

[Files]
; GUI binary (required)
Source: "xop_trader_gui.exe"; DestDir: "{app}"; Flags: ignoreversion

; C++ engine (required — GUI auto-launches this for click-and-play)
Source: "xop_trader.exe"; DestDir: "{app}"; Flags: ignoreversion

; Runtime dependencies for the C++ engine (copied from CI staging)
Source: "*.dll"; DestDir: "{app}"; Flags: ignoreversion

; Microsoft VC++ redistributable for clean Windows machines
Source: "VC_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall ignoreversion

; Reference config
Source: "config.example.yaml"; DestDir: "{app}"; Flags: ignoreversion

; Application icon (for Add/Remove Programs)
Source: "icon.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Start Menu
Name: "{group}\XOPTrader";                    Filename: "{app}\xop_trader_gui.exe"
Name: "{group}\{cm:UninstallProgram,XOPTrader}"; Filename: "{uninstallexe}"

; Desktop — created by default (user can uncheck during install)
Name: "{autodesktop}\XOPTrader"; Filename: "{app}\xop_trader_gui.exe"; \
  IconFilename: "{app}\icon.ico"; \
  Tasks: desktopicon

; =============================================================================
; Upgrade handling — silently uninstall any previous version first.
;
; Upgrade-in-place (same AppId) replaces shipped files but leaves STALE ones
; behind: a renamed binary or a dropped DLL from an old release would linger
; in {app} forever and could still be found/loaded. A clean uninstall-first
; removes that class. It is deliberately automatic, not a question -- the
; only sensible answer is yes, and it is safe by design:
;   * the uninstaller removes only files it installed (its own log), and
;   * user state lives in %LOCALAPPDATA%\XOPTrader (v0.9.2+), untouched;
;     even legacy config/secrets an elevated v0.9.x first run wrote into
;     {app} are NOT in the uninstall log, so they survive for the app's
;     first-run migration to adopt.
; =============================================================================
[Code]
function GetPreviousUninstaller(): String;
var
  key: String;
  uninst: String;
begin
  { The uninstall key is "<resolved AppId>_is1". The AppId is declared with
    a leading "{{" (Inno's escape for a literal "{"), so the RESOLVED value
    uses single braces -- hardcode that here, because SetupSetting("AppId")
    would return the raw double-brace text and never match. test_installer_
    upgrade.py asserts this GUID stays in sync with the [Setup] AppId. }
  { The resolved AppId is "{GUID}}" -- Inno collapses the leading "{{" to a
    single "{" but leaves the trailing "}}" as TWO literal braces, so the
    uninstall subkey ends "...7F21}}_is1" (verified against the live
    registry; a single "}" here finds nothing). test_installer_upgrade.py
    derives this from the [Setup] AppId per Inno's escaping rule. }
  key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
         '{B4E3A1C2-7D56-4F89-A012-9E3C0B5D7F21}}_is1';
  uninst := '';
  { A 64-bit-mode install registers its uninstaller in the 64-bit registry
    view, but plain HKLM from [Code] reads the 32-bit (WOW6432Node) view --
    so the key was never found and the old version was never removed. Try
    the 64-bit views first, then the 32-bit ones for a belt-and-braces
    match against any earlier 32-bit-registered build. }
  if not RegQueryStringValue(HKLM64, key, 'UninstallString', uninst) then
    if not RegQueryStringValue(HKCU64, key, 'UninstallString', uninst) then
      if not RegQueryStringValue(HKLM, key, 'UninstallString', uninst) then
        RegQueryStringValue(HKCU, key, 'UninstallString', uninst);
  Result := uninst;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  uninst: String;
  code: Integer;
begin
  Result := '';
  uninst := GetPreviousUninstaller();
  if uninst <> '' then
  begin
    uninst := RemoveQuotes(uninst);
    if FileExists(uninst) then
    begin
      { /VERYSILENT so the nested wizard never appears; failures are
        non-fatal -- the file copy below overwrites everything shipped,
        so a broken old uninstaller must not block the upgrade. }
      Exec(uninst, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART',
           '', SW_HIDE, ewWaitUntilTerminated, code);
    end;
  end;
end;

[Run]
Filename: "{tmp}\VC_redist.x64.exe"; \
  Parameters: "/install /quiet /norestart"; \
  StatusMsg: "Installing Microsoft Visual C++ runtime..."; \
  Flags: runhidden waituntilterminated

Filename: "{app}\xop_trader_gui.exe"; \
  Description: "{cm:LaunchProgram,XOPTrader}"; \
  Flags: nowait postinstall skipifsilent
