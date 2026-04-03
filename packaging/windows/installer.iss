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
WizardSmallImageFile=icon.ico
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

[Run]
Filename: "{tmp}\VC_redist.x64.exe"; \
  Parameters: "/install /quiet /norestart"; \
  StatusMsg: "Installing Microsoft Visual C++ runtime..."; \
  Flags: runhidden waituntilterminated

Filename: "{app}\xop_trader_gui.exe"; \
  Description: "{cm:LaunchProgram,XOPTrader}"; \
  Flags: nowait postinstall skipifsilent
