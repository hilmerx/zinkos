#!/bin/bash
#
# Zinkos receiver installer — builds, picks ALSA device, installs systemd service
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== Zinkos Receiver Installer ==="
echo

# Check dependencies
missing=()
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || missing+=("C compiler (gcc)")
[ -f /usr/include/alsa/asoundlib.h ] || missing+=("ALSA dev headers (libasound2-dev or alsa-lib)")
if [ ${#missing[@]} -gt 0 ]; then
    echo "Missing dependencies:"
    for dep in "${missing[@]}"; do
        echo "  - $dep"
    done
    echo
    echo "Arch:   sudo pacman -S base-devel alsa-lib"
    echo "Debian: sudo apt install build-essential libasound2-dev"
    exit 1
fi

# Build
echo "Building zinkos_rx..."
make -C "$SCRIPT_DIR" clean
make -C "$SCRIPT_DIR"
echo

# List ALSA devices
echo "Available audio devices:"
echo

devices=()
descriptions=()
while IFS= read -r line; do
    if [[ "$line" =~ ^card\ ([0-9]+):.+device\ ([0-9]+):\ (.+) ]]; then
        card="${BASH_REMATCH[1]}"
        dev="${BASH_REMATCH[2]}"
        desc="${BASH_REMATCH[3]}"
        hw="hw:${card},${dev}"
        devices+=("$hw")
        descriptions+=("$desc")
        echo "  ${#devices[@]}) ${hw}  —  ${desc}"
    fi
done < <(aplay -l 2>/dev/null)

if [ ${#devices[@]} -eq 0 ]; then
    echo "  No ALSA playback devices found!"
    echo "  Check that your DAC/speakers are connected."
    exit 1
fi

echo
read -rp "Select device [1-${#devices[@]}]: " choice

if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt ${#devices[@]} ]; then
    echo "Invalid selection."
    exit 1
fi

selected="${devices[$((choice - 1))]}"
echo
echo "Selected: $selected (${descriptions[$((choice - 1))]})"

# Install
echo
echo "Installing (requires sudo)..."
sudo make -C "$SCRIPT_DIR" install-service ALSA_DEVICE="$selected"

# Avahi mDNS discovery (optional)
if command -v avahi-daemon >/dev/null 2>&1; then
    echo
    echo "Installing Avahi mDNS service (makes receiver discoverable)..."
    sudo make -C "$SCRIPT_DIR" install-avahi
else
    echo
    echo "Note: avahi-daemon not found — mDNS discovery won't work."
    echo "  Install it for auto-discovery: sudo apt install avahi-daemon"
    echo "  The receiver still works fine without it (use manual IP)."
fi

echo
echo "=== Done ==="
echo "Zinkos receiver running on $selected"
echo "Check status: systemctl status zinkos-rx"
echo "View logs:    journalctl -u zinkos-rx -f"
