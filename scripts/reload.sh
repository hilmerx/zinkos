#!/bin/bash
#
# Build, validate, install, and reload Zinkos driver.
# Safety: validates bundle before install, monitors coreaudiod after,
# and auto-removes driver if coreaudiod crashes.
#
set -e

DRIVER_NAME="Zinkos.driver"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
BUILD_DIR="${1:-build}"
BUNDLE_PATH="$BUILD_DIR/driver/$DRIVER_NAME"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "==> Building Rust engine..."
cargo build -p zinkos-engine

echo "==> Building driver bundle..."
cmake --build "$BUILD_DIR"

# --- Validation gate ---
echo "==> Validating bundle..."
if ! "$SCRIPT_DIR/validate.sh" "$BUNDLE_PATH"; then
    echo
    echo "ABORT: validation failed. Driver NOT installed."
    exit 1
fi

# --- Get coreaudiod PID before install ---
OLD_PID=$(pgrep -x coreaudiod || echo "")

echo "==> Installing $DRIVER_NAME..."
sudo rm -rf "$INSTALL_DIR/$DRIVER_NAME"
sudo cp -R "$BUNDLE_PATH" "$INSTALL_DIR/"
sudo xattr -dr com.apple.quarantine "$INSTALL_DIR/$DRIVER_NAME" 2>/dev/null || true

echo "==> Restarting coreaudiod..."
sudo killall coreaudiod
sleep 2

# --- Watchdog: check coreaudiod survived ---
NEW_PID=$(pgrep -x coreaudiod || echo "")
if [ -z "$NEW_PID" ]; then
    echo "!! coreaudiod did not restart — removing driver!"
    sudo rm -rf "$INSTALL_DIR/$DRIVER_NAME"
    sudo killall coreaudiod 2>/dev/null || true
    sleep 1
    echo "!! Driver removed. Audio should recover."
    exit 1
fi

# Check it's not crash-looping (CPU spike = bad sign)
# Wait longer — driver init + AirPlay enumeration causes legitimate CPU spikes
sleep 8
CPU=$(ps -p "$NEW_PID" -o %cpu= 2>/dev/null | tr -d ' ' || echo "0")
# Remove decimal for comparison
CPU_INT=${CPU%%.*}
if [ "${CPU_INT:-0}" -gt 300 ]; then
    echo "!! coreaudiod at ${CPU}% CPU — likely crash-looping. Removing driver!"
    sudo rm -rf "$INSTALL_DIR/$DRIVER_NAME"
    sudo killall -9 coreaudiod 2>/dev/null || true
    sleep 1
    echo "!! Driver removed. Audio should recover."
    exit 1
fi

echo "==> coreaudiod running (PID $NEW_PID, CPU ${CPU}%). Looks healthy."

# --- Check system log for Zinkos driver load ---
echo "==> Checking logs for Zinkos..."
LOG_OUTPUT=$(log show --predicate 'sender == "coreaudiod" OR subsystem == "com.zinkos.driver"' --last 30s --style compact 2>/dev/null | grep -i zinkos || true)
if [ -n "$LOG_OUTPUT" ]; then
    echo "$LOG_OUTPUT"
else
    echo "   (no Zinkos log entries in last 30s)"
fi

echo "==> Done. Zinkos reloaded."
