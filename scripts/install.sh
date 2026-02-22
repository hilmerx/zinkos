#!/bin/bash
set -e

DRIVER_NAME="Zinkos.driver"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"
BUILD_DIR="${1:-build}"

BUNDLE_PATH="${BUILD_DIR}/driver/${DRIVER_NAME}"

if [ ! -d "$BUNDLE_PATH" ]; then
    echo "Error: Driver bundle not found at $BUNDLE_PATH"
    echo "Run: cmake --build $BUILD_DIR"
    exit 1
fi

echo "Installing $DRIVER_NAME to $INSTALL_DIR..."
sudo cp -R "$BUNDLE_PATH" "$INSTALL_DIR/"
echo "Restarting coreaudiod..."
sudo killall coreaudiod
echo "Done. 'Zinkos' should appear in System Settings > Sound > Output."
