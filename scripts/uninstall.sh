#!/bin/bash
set -e

DRIVER_NAME="Zinkos.driver"
INSTALL_DIR="/Library/Audio/Plug-Ins/HAL"

if [ ! -d "$INSTALL_DIR/$DRIVER_NAME" ]; then
    echo "$DRIVER_NAME is not installed."
    exit 0
fi

echo "Removing $INSTALL_DIR/$DRIVER_NAME..."
sudo rm -rf "$INSTALL_DIR/$DRIVER_NAME"
echo "Restarting coreaudiod..."
sudo killall coreaudiod
echo "Done. Zinkos has been uninstalled."
