#!/usr/bin/env bash
# =============================================================================
# install.sh  —  XOPTrader Linux installer
#
# Installs the standalone GUI binary, the C++ engine binary, and a
# desktop shortcut.  Prompts the user before creating the desktop entry.
#
# Usage:
#   chmod +x install.sh
#   ./install.sh              # interactive
#   ./install.sh --no-desktop # skip desktop shortcut
# =============================================================================

set -euo pipefail

INSTALL_DIR="${HOME}/.local/bin"
ICON_DIR="${HOME}/.local/share/icons/hicolor/256x256/apps"
DESKTOP_DIR="${HOME}/.local/share/applications"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

NO_DESKTOP=false
for arg in "$@"; do
  [[ "$arg" == "--no-desktop" ]] && NO_DESKTOP=true
done

# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------
ask_yes_no() {
  local prompt="$1" default="${2:-y}"
  while true; do
    read -rp "$prompt [Y/n]: " ans
    ans="${ans:-$default}"
    case "${ans,,}" in
      y|yes) return 0 ;;
      n|no)  return 1 ;;
      *)     echo "  Please answer y or n." ;;
    esac
  done
}

# ---------------------------------------------------------------------------
# Install binaries
# ---------------------------------------------------------------------------
mkdir -p "$INSTALL_DIR"

if [[ -f "$SCRIPT_DIR/xop_trader_gui" ]]; then
  install -m 755 "$SCRIPT_DIR/xop_trader_gui" "$INSTALL_DIR/xop_trader_gui"
  echo "Installed GUI binary → $INSTALL_DIR/xop_trader_gui"
fi

if [[ -f "$SCRIPT_DIR/xop_trader" ]]; then
  install -m 755 "$SCRIPT_DIR/xop_trader" "$INSTALL_DIR/xop_trader"
  echo "Installed engine binary → $INSTALL_DIR/xop_trader"
fi

if [[ -f "$SCRIPT_DIR/config.example.yaml" ]] && [[ ! -f "$INSTALL_DIR/config.yaml" ]]; then
  cp "$SCRIPT_DIR/config.example.yaml" "$INSTALL_DIR/config.yaml"
  echo "Copied config.example.yaml → $INSTALL_DIR/config.yaml  (edit before first run)"
fi

# ---------------------------------------------------------------------------
# Desktop shortcut
# ---------------------------------------------------------------------------
create_desktop_shortcut() {
  mkdir -p "$ICON_DIR" "$DESKTOP_DIR"

  # Install icon if present
  if [[ -f "$SCRIPT_DIR/icon.png" ]]; then
    install -m 644 "$SCRIPT_DIR/icon.png" "$ICON_DIR/xop_trader.png"
    gtk-update-icon-cache -qtf "${HOME}/.local/share/icons/hicolor" 2>/dev/null || true
  fi

  # Write .desktop file with the real binary path
  cat > "$DESKTOP_DIR/xop_trader.desktop" <<DESKTOP_EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=XOPTrader
GenericName=Chia DEX Market Maker
Comment=Automated market-making bot for the Chia XOP DEX
Exec=${INSTALL_DIR}/xop_trader_gui
Icon=xop_trader
Categories=Finance;
Terminal=false
StartupNotify=true
StartupWMClass=xop_trader_gui
DESKTOP_EOF

  chmod 644 "$DESKTOP_DIR/xop_trader.desktop"
  update-desktop-database "$DESKTOP_DIR" 2>/dev/null || true
  echo "Desktop shortcut created → $DESKTOP_DIR/xop_trader.desktop"
}

if $NO_DESKTOP; then
  echo "Skipping desktop shortcut (--no-desktop)."
elif ask_yes_no "Create a desktop shortcut for XOPTrader?"; then
  create_desktop_shortcut
else
  echo "Desktop shortcut skipped."
fi

# ---------------------------------------------------------------------------
# PATH reminder
# ---------------------------------------------------------------------------
if [[ ":$PATH:" != *":$INSTALL_DIR:"* ]]; then
  echo ""
  echo "NOTE: $INSTALL_DIR is not in your PATH."
  echo "      Add the following line to your ~/.bashrc or ~/.zshrc:"
  echo '      export PATH="$HOME/.local/bin:$PATH"'
fi

echo ""
echo "Installation complete.  Run:  xop_trader_gui"
