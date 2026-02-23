#!/bin/bash
#
# package.sh — Build and package Zinkos into a macOS .pkg installer
#
# Creates: dist/Zinkos-<version>.pkg (signed if Developer ID certs found)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION=$(tr -d '[:space:]' < "$REPO_DIR/VERSION")
IDENTIFIER="com.zinkos"

DIST_DIR="$REPO_DIR/dist"
STAGING_DIR="$DIST_DIR/staging"
SCRIPTS_DIR="$DIST_DIR/scripts"
BUILD_DIR="$REPO_DIR/build"

# ── Colors ──────────────────────────────────────────────────
AMBER='\033[38;5;172m'
DIM='\033[38;5;137m'
BOLD='\033[1m'
RESET='\033[0m'
RED='\033[38;5;160m'
GREEN='\033[38;5;107m'

info()    { echo -e "  ${AMBER}▸${RESET} $1"; }
success() { echo -e "  ${GREEN}✓${RESET} $1"; }
warn()    { echo -e "  ${RED}!${RESET} $1"; }
step()    { echo -e "\n${BOLD}${AMBER}═══ $1${RESET}"; }

# ── Detect code signing identities ─────────────────────────

detect_signing() {
    step "Checking code signing identities"

    INSTALLER_IDENTITY=""
    APPLICATION_IDENTITY=""

    # Developer ID Installer — signs the .pkg itself
    local installer_match
    installer_match=$(security find-identity -v -p basic 2>/dev/null \
        | grep "Developer ID Installer" | head -1 || true)
    if [ -n "$installer_match" ]; then
        INSTALLER_IDENTITY=$(echo "$installer_match" | sed 's/.*"\(.*\)".*/\1/')
        success "Developer ID Installer: ${BOLD}$INSTALLER_IDENTITY${RESET}"
    else
        warn "No Developer ID Installer certificate found"
        info "The .pkg will be unsigned — users will see Gatekeeper warnings"
    fi

    # Developer ID Application — signs binaries and the .app
    local app_match
    app_match=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep "Developer ID Application" | head -1 || true)
    if [ -n "$app_match" ]; then
        APPLICATION_IDENTITY=$(echo "$app_match" | sed 's/.*"\(.*\)".*/\1/')
        success "Developer ID Application: ${BOLD}$APPLICATION_IDENTITY${RESET}"
    else
        # Fall back to Apple Development (works for local install, not distribution)
        app_match=$(security find-identity -v -p codesigning 2>/dev/null \
            | grep "Apple Development" | head -1 || true)
        if [ -n "$app_match" ]; then
            APPLICATION_IDENTITY=$(echo "$app_match" | sed 's/.*"\(.*\)".*/\1/')
            success "Apple Development: ${BOLD}$APPLICATION_IDENTITY${RESET}"
            warn "Apple Development certs work locally but not for distribution"
        else
            warn "No code signing certificate found"
            info "Binaries will be ad-hoc signed — driver may not load"
            info ""
            info "To fix: Xcode → Settings → Accounts → add Apple ID → Manage Certificates"
            info "  Free account  → Apple Development (local install only)"
            info "  Paid account  → Developer ID Application + Installer (distributable)"
        fi
    fi

    echo ""
    if [ -n "$INSTALLER_IDENTITY" ] && [ -n "$APPLICATION_IDENTITY" ]; then
        success "Full signing available — .pkg will be distributable"
    elif [ -n "$APPLICATION_IDENTITY" ]; then
        warn "Binaries will be signed but .pkg will be unsigned"
        info "Users will need to right-click → Open the .pkg"
    else
        warn "No signing — package is for local development only"
    fi
}

# ── Build everything ────────────────────────────────────────

build_all() {
    step "Building Rust engine (release)"
    cargo build --release -p zinkos-engine
    success "Engine dylib built"

    step "Building driver bundle (release)"
    cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
        -DZINKOS_RELEASE=ON \
        ${APPLICATION_IDENTITY:+-DCODESIGN_IDENTITY="$APPLICATION_IDENTITY"} \
        2>&1 | tail -3
    cmake --build "$BUILD_DIR"
    success "Driver bundle built"

    step "Building Zinkos menu app (release)"
    (cd "$REPO_DIR/sender/app" && swift build -c release)
    success "Menu app built"
}

# ── Assemble staging directory ──────────────────────────────

stage() {
    step "Staging files"

    rm -rf "$STAGING_DIR" "$SCRIPTS_DIR"
    mkdir -p "$STAGING_DIR" "$SCRIPTS_DIR"

    # 1. Driver → /Library/Audio/Plug-Ins/HAL/Zinkos.driver
    local driver_dest="$STAGING_DIR/Library/Audio/Plug-Ins/HAL"
    mkdir -p "$driver_dest"
    cp -R "$BUILD_DIR/driver/Zinkos.driver" "$driver_dest/"
    success "Staged Zinkos.driver"

    # 2. Menu app → /Applications/Zinkos.app
    #    SPM builds an executable, we need to wrap it in an .app bundle
    local app_dest="$STAGING_DIR/Applications/Zinkos.app/Contents/MacOS"
    local app_resources="$STAGING_DIR/Applications/Zinkos.app/Contents/Resources"
    mkdir -p "$app_dest" "$app_resources"

    # Find the built executable
    local swift_bin
    swift_bin=$(cd "$REPO_DIR/sender/app" && swift build -c release --show-bin-path)/Zinkos
    cp "$swift_bin" "$app_dest/Zinkos"

    # Copy SPM resources bundle if it exists
    local bin_dir
    bin_dir=$(dirname "$swift_bin")
    if [ -d "$bin_dir/Zinkos_Zinkos.bundle" ]; then
        cp -R "$bin_dir/Zinkos_Zinkos.bundle" "$app_dest/"
    fi

    # Create Info.plist for the .app
    cat > "$STAGING_DIR/Applications/Zinkos.app/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>Zinkos</string>
    <key>CFBundleIdentifier</key>
    <string>com.zinkos.app</string>
    <key>CFBundleName</key>
    <string>Zinkos</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>13.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST
    success "Staged Zinkos.app"

    # Sign the .app if we have a cert
    if [ -n "$APPLICATION_IDENTITY" ]; then
        codesign --force --timestamp --deep --sign "$APPLICATION_IDENTITY" \
            "$STAGING_DIR/Applications/Zinkos.app"
        success "Signed Zinkos.app"
    fi

    # 3. CLI → /usr/local/bin/zinkos
    local cli_dest="$STAGING_DIR/usr/local/bin"
    mkdir -p "$cli_dest"
    cp "$REPO_DIR/scripts/zinkos" "$cli_dest/zinkos"
    chmod +x "$cli_dest/zinkos"
    success "Staged zinkos CLI"

    # 4. VERSION file alongside CLI so it can find it
    cp "$REPO_DIR/VERSION" "$cli_dest/.zinkos-version"
    success "Staged version file"

    # 5. Postinstall script — restart coreaudiod to load the driver
    cat > "$SCRIPTS_DIR/postinstall" << 'POSTINSTALL'
#!/bin/bash
# Restart coreaudiod so it picks up the newly installed driver
killall coreaudiod 2>/dev/null || true
sleep 1
echo "Zinkos driver installed. 'Zinkos' should appear in Sound output."
exit 0
POSTINSTALL
    chmod +x "$SCRIPTS_DIR/postinstall"
    success "Created postinstall script"
}

# ── Build the .pkg ──────────────────────────────────────────

package() {
    step "Building installer package"

    mkdir -p "$DIST_DIR"
    local unsigned_pkg="$DIST_DIR/Zinkos-${VERSION}-unsigned.pkg"
    local final_pkg="$DIST_DIR/Zinkos-${VERSION}.pkg"

    pkgbuild \
        --root "$STAGING_DIR" \
        --scripts "$SCRIPTS_DIR" \
        --identifier "$IDENTIFIER" \
        --version "$VERSION" \
        --install-location "/" \
        "$unsigned_pkg"
    success "Built component package"

    if [ -n "$INSTALLER_IDENTITY" ]; then
        productsign --sign "$INSTALLER_IDENTITY" \
            "$unsigned_pkg" "$final_pkg"
        rm "$unsigned_pkg"
        success "Signed with Developer ID Installer"

        # Notarization hint
        echo ""
        info "To notarize (recommended for distribution):"
        info "  xcrun notarytool submit $final_pkg \\"
        info "    --apple-id YOUR_APPLE_ID \\"
        info "    --team-id YOUR_TEAM_ID \\"
        info "    --password YOUR_APP_SPECIFIC_PASSWORD \\"
        info "    --wait"
        info "  xcrun stapler staple $final_pkg"
    else
        mv "$unsigned_pkg" "$final_pkg"
        warn "Package is unsigned"
    fi

    echo ""
    step "Done"
    success "Package: ${BOLD}$final_pkg${RESET}"
    info "Size: $(du -h "$final_pkg" | cut -f1)"
    info "Install: ${DIM}sudo installer -pkg $final_pkg -target /${RESET}"
    echo ""
}

# ── Main ────────────────────────────────────────────────────

echo -e "${BOLD}${AMBER}"
echo '  ╔═══════════════════════════════════╗'
echo "  ║    Zinkos Packager  v${VERSION}          ║"
echo '  ╚═══════════════════════════════════╝'
echo -e "${RESET}"

detect_signing
build_all
stage
package
