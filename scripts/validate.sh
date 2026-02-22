#!/bin/bash
#
# Validates a Zinkos.driver bundle BEFORE installation.
# Catches the issues that would crash coreaudiod.
#
set -e

BUNDLE="$(cd "$(dirname "${1:?Usage: validate.sh <path-to-Zinkos.driver>}")" && pwd)/$(basename "$1")"
PASS=0
FAIL=0

pass() { echo "  ✓ $1"; PASS=$((PASS + 1)); }
fail() { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

echo "Validating: $BUNDLE"
echo

# --- 1. Bundle structure ---
echo "Bundle structure:"
[ -d "$BUNDLE/Contents/MacOS" ]        && pass "Contents/MacOS exists"       || fail "Contents/MacOS missing"
[ -f "$BUNDLE/Contents/MacOS/Zinkos" ] && pass "Zinkos binary exists"       || fail "Zinkos binary missing"
[ -f "$BUNDLE/Contents/Info.plist" ]   && pass "Info.plist exists"           || fail "Info.plist missing"
[ -d "$BUNDLE/Contents/Frameworks" ]   && pass "Frameworks dir exists"      || fail "Frameworks dir missing"
[ -f "$BUNDLE/Contents/Frameworks/libzinkos_engine.dylib" ] \
    && pass "libzinkos_engine.dylib exists" \
    || fail "libzinkos_engine.dylib missing"
echo

# --- 2. Info.plist keys ---
echo "Info.plist:"
PLIST="$BUNDLE/Contents/Info.plist"
if plutil -lint "$PLIST" > /dev/null 2>&1; then
    pass "plist is valid XML"
else
    fail "plist is malformed"
fi

# Check the Apple-defined AudioServerPlugIn type UUID
if grep -q "443ABAB8-E7B3-491A-B985-BEB9187030DB" "$PLIST"; then
    pass "correct AudioServerPlugIn type UUID"
else
    fail "WRONG AudioServerPlugIn type UUID (must be 443ABAB8-E7B3-491A-B985-BEB9187030DB)"
fi

if grep -q "ZinkosPlugIn_Create" "$PLIST"; then
    pass "factory function declared in plist"
else
    fail "factory function missing from plist"
fi

EXEC_NAME=$(defaults read "$PLIST" CFBundleExecutable 2>/dev/null || echo "")
if [ "$EXEC_NAME" = "Zinkos" ]; then
    pass "CFBundleExecutable = Zinkos"
else
    fail "CFBundleExecutable = '$EXEC_NAME' (expected 'Zinkos')"
fi
echo

# --- 3. Symbol exports ---
echo "Symbols:"
if nm -gU "$BUNDLE/Contents/MacOS/Zinkos" 2>/dev/null | grep -q "_ZinkosPlugIn_Create"; then
    pass "ZinkosPlugIn_Create exported"
else
    fail "ZinkosPlugIn_Create NOT exported"
fi
echo

# --- 4. Dylib linkage ---
echo "Dylib linkage:"
LINK=$(otool -L "$BUNDLE/Contents/MacOS/Zinkos" 2>/dev/null | grep libzinkos_engine || echo "")
if echo "$LINK" | grep -q "@loader_path"; then
    pass "dylib linked via @loader_path"
else
    fail "dylib NOT linked via @loader_path (got: $LINK)"
fi
echo

# --- 5. Code signing ---
echo "Code signing:"
if codesign --verify --deep --strict "$BUNDLE" 2>/dev/null; then
    pass "bundle signature valid"
else
    fail "bundle signature INVALID"
fi

# Check it's NOT ad-hoc (ad-hoc won't load in coreaudiod)
# codesign -dv prints to stderr
SIGN_INFO=$(codesign -dv --verbose=4 "$BUNDLE" 2>&1 || true)
if echo "$SIGN_INFO" | grep -q "Authority=Apple Development"; then
    pass "signed with Apple Development certificate"
elif echo "$SIGN_INFO" | grep -q "Authority=Developer ID"; then
    pass "signed with Developer ID certificate"
elif echo "$SIGN_INFO" | grep -q "Signature=adhoc"; then
    fail "ad-hoc signed — coreaudiod will REJECT this"
else
    # Check if there's any authority at all
    if echo "$SIGN_INFO" | grep -q "Authority="; then
        pass "signed with certificate"
    else
        fail "no signing authority found — coreaudiod will REJECT this"
    fi
fi

# Check inner dylib is also signed
if codesign --verify --strict "$BUNDLE/Contents/Frameworks/libzinkos_engine.dylib" 2>/dev/null; then
    pass "dylib signature valid"
else
    fail "dylib signature INVALID"
fi
echo

# --- 6. Load test (dlopen) ---
echo "Load test:"
# Compile a tiny loader on the fly
LOADER=$(mktemp /tmp/zinkos_loader.XXXXX)
cat > "${LOADER}.c" << 'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char** argv) {
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    void* s = dlsym(h, "ZinkosPlugIn_Create");
    if (!s) { fprintf(stderr, "symbol not found\n"); dlclose(h); return 2; }
    dlclose(h);
    return 0;
}
EOF
if clang -o "$LOADER" "${LOADER}.c" 2>/dev/null; then
    if "$LOADER" "$BUNDLE/Contents/MacOS/Zinkos" 2>/dev/null; then
        pass "dlopen + dlsym succeeded"
    else
        fail "dlopen or dlsym FAILED — driver will crash coreaudiod"
    fi
    rm -f "$LOADER" "${LOADER}.c"
else
    echo "  ? could not compile loader (skipped)"
    rm -f "$LOADER" "${LOADER}.c"
fi
echo

# --- Summary ---
echo "================================"
echo "  $PASS passed, $FAIL failed"
echo "================================"

if [ "$FAIL" -gt 0 ]; then
    echo
    echo "DO NOT INSTALL — fix failures first."
    exit 1
fi

echo
echo "Safe to install."
exit 0
