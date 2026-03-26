#!/usr/bin/env bash
# =============================================================================
# uninstall.sh  —  XOPTrader Linux uninstaller
#
# Removes binaries, config (optionally), desktop shortcut, and icon
# installed by install.sh.
#
# Usage:
#   ./uninstall.sh              # interactive (prompts before removing config)
#   ./uninstall.sh --purge      # also remove config.yaml without prompting
# =============================================================================

set -euo pipefail

INSTALL_DIR="${HOME}/.local/bin"
ICON_DIR="${HOME}/.local/share/icons/hicolor/256x256/apps"
DESKTOP_DIR="${HOME}/.local/share/applications"

PURGE=false
for arg in "$@"; do
  [[ "$arg" == "--purge" ]] && PURGE=true
done

removed=0

remove_file() {
  if [[ -f "$1" ]]; then
    rm -f "$1"
    echo "Removed $1"
    ((removed++))
  fi
}

# ---------------------------------------------------------------------------
# Binaries
# ---------------------------------------------------------------------------
remove_file "$INSTALL_DIR/xop_trader_gui"
remove_file "$INSTALL_DIR/xop_trader"

# ---------------------------------------------------------------------------
# Desktop shortcut and icon
# ---------------------------------------------------------------------------
remove_file "$DESKTOP_DIR/xop_trader.desktop"
remove_file "$ICON_DIR/xop_trader.png"
update-desktop-database "$DESKTOP_DIR" 2>/dev/null || true
gtk-update-icon-cache -qtf "${HOME}/.local/share/icons/hicolor" 2>/dev/null || true

# ---------------------------------------------------------------------------
# Config file
# ---------------------------------------------------------------------------
if [[ -f "$INSTALL_DIR/config.yaml" ]]; then
  if $PURGE; then
    remove_file "$INSTALL_DIR/config.yaml"
  else
    read -rp "Remove config.yaml? This cannot be undone. [y/N]: " ans
    case "${ans,,}" in
      y|yes) remove_file "$INSTALL_DIR/config.yaml" ;;
      *)     echo "Kept $INSTALL_DIR/config.yaml" ;;
    esac
  fi
fi

echo ""
if (( removed > 0 )); then
  echo "Uninstall complete ($removed items removed)."
else
  echo "Nothing to remove — XOPTrader does not appear to be installed."
fi
