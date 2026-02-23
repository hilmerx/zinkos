#!/bin/bash
#
# Uninstall everything Zinkos installs on macOS.
#
set -e

DRIVER_PATH="/Library/Audio/Plug-Ins/HAL/Zinkos.driver"
CLI_PATH="/usr/local/bin/zinkos"
APP_PATH="/Applications/Zinkos.app"
PLIST_PATH="/Library/Preferences/com.zinkos.driver.plist"
VOLUME_PATH="/Library/Preferences/Audio/com.zinkos.volume"

AMBER='\033[38;5;172m'
BOLD='\033[1m'
RESET='\033[0m'
RED='\033[38;5;160m'
GREEN='\033[38;5;107m'
DIM='\033[38;5;137m'

success() { echo -e "  ${GREEN}✓${RESET} $1"; }
skip()    { echo -e "  ${DIM}–${RESET} $1 ${DIM}(not found)${RESET}"; }

echo -e "${BOLD}${AMBER}"
echo '  ╔═══════════════════════════════════╗'
echo '  ║     Zinkos Uninstaller            ║'
echo '  ╚═══════════════════════════════════╝'
echo -e "${RESET}"

FOUND=0
[ -d "$DRIVER_PATH" ] || [ -L "$CLI_PATH" ] || [ -d "$APP_PATH" ] || \
[ -f "$PLIST_PATH" ] || [ -f "$VOLUME_PATH" ] && FOUND=1

if [ "$FOUND" -eq 0 ]; then
    echo "  Nothing to uninstall."
    exit 0
fi

echo -e "  ${RED}This will remove:${RESET}"
[ -d "$DRIVER_PATH" ] && echo "    $DRIVER_PATH"
[ -L "$CLI_PATH" ]    && echo "    $CLI_PATH"
[ -d "$APP_PATH" ]    && echo "    $APP_PATH"
[ -f "$PLIST_PATH" ]  && echo "    $PLIST_PATH"
[ -f "$VOLUME_PATH" ] && echo "    $VOLUME_PATH"
echo ""

read -p "  Continue? [y/N] " confirm
if [[ ! "$confirm" =~ ^[Yy]$ ]]; then
    echo "  Cancelled."
    exit 0
fi

echo ""

# Driver
if [ -d "$DRIVER_PATH" ]; then
    sudo rm -rf "$DRIVER_PATH"
    success "Removed driver"
else
    skip "Driver"
fi

# CLI symlink
if [ -L "$CLI_PATH" ]; then
    sudo rm -f "$CLI_PATH"
    success "Removed CLI"
else
    skip "CLI"
fi

# App
if [ -d "$APP_PATH" ]; then
    sudo rm -rf "$APP_PATH"
    success "Removed app"
else
    skip "App"
fi

# Config plist
if [ -f "$PLIST_PATH" ]; then
    sudo rm -f "$PLIST_PATH"
    success "Removed config"
else
    skip "Config"
fi

# Volume state
if [ -f "$VOLUME_PATH" ]; then
    sudo rm -f "$VOLUME_PATH"
    success "Removed volume state"
else
    skip "Volume state"
fi

# Restart coreaudiod if driver was installed
if [ -d "$DRIVER_PATH" ] 2>/dev/null || true; then
    sudo killall coreaudiod 2>/dev/null || true
    sleep 1
    success "Restarted coreaudiod"
fi

echo ""
echo -e "  ${GREEN}Zinkos fully uninstalled.${RESET}"
echo ""
