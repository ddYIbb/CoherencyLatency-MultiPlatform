#!/usr/bin/env bash
# Build the CoherencyLatency macOS GUI with osxcross on Linux (WSL).
#
#   wsl -e bash /mnt/c/Users/Administrator/AppData/Local/Temp/cl_repo/gui/macos/build_macos_gui.sh
#
# Produces
#   gui/macos/build/CoherencyLatency-x86_64   Mach-O x86_64, minos 10.10
#   gui/macos/build/CoherencyLatency-arm64    Mach-O arm64,  minos 11.0
#   gui/macos/build/CoherencyLatency          universal (lipo merged)
#   gui/macos/build/CoherencyLatency.app      .app bundle with the icon
#
# The script is deliberately strict: -Wall -Wextra with -Werror on our own
# translation unit, so a warning fails the build.  The shared engine translation
# unit (gui/engine_api.cpp, which #includes windows/src/CoherencyLatency.cpp) is
# not ours to edit and carries pre-existing -Wunused-function warnings from the
# CLI-only helpers in that file; it is compiled with -Wno-unused-function for
# that reason only.  Both translation units are otherwise held to the same flags.

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths.  The script is invoked through WSL, so the Windows checkout path is
# converted to its /mnt/c/... form: GUI_DIR is this script's own directory,
# REPO_DIR is two levels up.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUI_DIR="$SCRIPT_DIR"
REPO_DIR="$(cd "$GUI_DIR/../.." && pwd)"
BUILD_DIR="$GUI_DIR/build"
OBJ_DIR="$BUILD_DIR/obj"
APP_DIR="$BUILD_DIR/CoherencyLatency.app"

SDK="/opt/osxcross/MacOSX11.3.sdk"
CXX="${CXX:-clang++}"
LIPO="${LIPO:-llvm-lipo-21}"
FILE_TOOL="${FILE_TOOL:-file}"

if [ ! -d "$SDK" ]; then
    echo "error: macOS SDK not found at $SDK" >&2
    exit 1
fi
if [ ! -f "$GUI_DIR/main.mm" ]; then
    echo "error: $GUI_DIR/main.mm is missing" >&2
    exit 1
fi
if [ ! -f "$GUI_DIR/../engine_api.cpp" ]; then
    echo "error: $GUI_DIR/../engine_api.cpp is missing" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR" "$OBJ_DIR"

# ---------------------------------------------------------------------------
# Layout self-test.  cl_layout.h holds every sub-rect of the window and is
# AppKit-free, so the geometry of the .app can be verified numerically on this
# host even though the Mach-O cannot be executed here.  The same CXX without a
# -target compiles for the build host.
# ---------------------------------------------------------------------------
SELFTEST_BIN="$GUI_DIR/layout-selftest"
echo "==> layout self-test (host build, pure C++)"
"$CXX" -std=c++17 -O2 -Wall -Wextra -I "$GUI_DIR" \
    "$GUI_DIR/layout_selftest.cpp" -o "$SELFTEST_BIN" 2>&1 | tee "$OBJ_DIR/selftest.log"
if [ -s "$OBJ_DIR/selftest.log" ]; then
    WARNING_COUNT=$((WARNING_COUNT + $(grep -c 'warning:' "$OBJ_DIR/selftest.log" || true)))
fi
"$SELFTEST_BIN" | tee "$OBJ_DIR/layout-selftest.txt"

COMMON_FLAGS=(
    -std=c++17
    -fobjc-arc
    -O2
    -Wall
    -Wextra
    -isysroot "$SDK"
    -stdlib=libc++
    -fvisibility=hidden
    -DNDEBUG
)

# -fuse-ld=lld is only meaningful while linking; passing it to a -c compile would
# itself raise -Wunused-command-line-argument, and the compile steps are -Werror.
LINK_FLAGS=(
    -fuse-ld=lld
    -framework Cocoa
)

# Warnings that belong to the shared engine file only (pre-existing, CLI helpers).
ENGINE_SUPPRESS=(
    -Wno-unused-function
    -Wno-unused-command-line-argument
)

WARNING_COUNT=0

compile_slice() {
    local arch="$1"          # x86_64 | arm64
    local triple="$2"        # x86_64-apple-macos10.10 | arm64-apple-macos11.0
    local gui_obj="$OBJ_DIR/main_${arch}.o"
    local engine_obj="$OBJ_DIR/engine_api_${arch}.o"
    local log="$OBJ_DIR/${arch}.log"

    echo "==> compiling $arch slice ($triple)"

    # Our translation unit: zero warnings allowed.
    "$CXX" "${COMMON_FLAGS[@]}" -target "$triple" -Werror \
        -c "$GUI_DIR/main.mm" -o "$gui_obj" 2>&1 | tee "$log".main
    if [ -s "$log".main ]; then
        WARNING_COUNT=$((WARNING_COUNT + $(grep -c 'warning:' "$log".main || true)))
    fi

    # Shared engine translation unit: pre-existing warnings suppressed.
    "$CXX" "${COMMON_FLAGS[@]}" "${ENGINE_SUPPRESS[@]}" -target "$triple" \
        -c "$REPO_DIR/gui/engine_api.cpp" -o "$engine_obj" 2>&1 | tee "$log".engine
    if [ -s "$log".engine ]; then
        echo "---- engine warnings (expected: none after suppression) ----" >&2
        cat "$log".engine >&2
        WARNING_COUNT=$((WARNING_COUNT + $(grep -c 'warning:' "$log".engine || true)))
    fi

    # Link.
    "$CXX" "${COMMON_FLAGS[@]}" "${LINK_FLAGS[@]}" -target "$triple" \
        "$gui_obj" "$engine_obj" \
        -o "$BUILD_DIR/CoherencyLatency-$arch" 2>&1 | tee "$log".link
    if [ -s "$log".link ]; then
        WARNING_COUNT=$((WARNING_COUNT + $(grep -c 'warning:' "$log".link || true)))
    fi

    echo "    -> $BUILD_DIR/CoherencyLatency-$arch"
}

compile_slice x86_64 x86_64-apple-macos10.10
compile_slice arm64  arm64-apple-macos11.0

echo "==> merging universal binary"
"$LIPO" -create -output "$BUILD_DIR/CoherencyLatency" \
    "$BUILD_DIR/CoherencyLatency-x86_64" \
    "$BUILD_DIR/CoherencyLatency-arm64"

# ---------------------------------------------------------------------------
# .app bundle
# ---------------------------------------------------------------------------
echo "==> assembling $APP_DIR"
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
cp "$BUILD_DIR/CoherencyLatency" "$APP_DIR/Contents/MacOS/CoherencyLatency"
chmod +x "$APP_DIR/Contents/MacOS/CoherencyLatency"
cp "$GUI_DIR/Info.plist" "$APP_DIR/Contents/Info.plist"
printf 'APPL????' > "$APP_DIR/Contents/PkgInfo"

ICON_SOURCE="$REPO_DIR/gui/assets/app.icns"
if [ -f "$ICON_SOURCE" ]; then
    cp "$ICON_SOURCE" "$APP_DIR/Contents/Resources/app.icns"
else
    echo "warning: $ICON_SOURCE not found - bundle will use the default icon" >&2
fi

# ---------------------------------------------------------------------------
# Verification (the Mach-O cannot be executed here, so inspect the headers).
# ---------------------------------------------------------------------------
echo "==> file(1) on the slices and the merged binary"
for target in "CoherencyLatency-x86_64" "CoherencyLatency-arm64" "CoherencyLatency"; do
    printf '%-34s %s\n' "$target" "$("$FILE_TOOL" -b "$BUILD_DIR/$target")"
done

echo
echo "warnings: $WARNING_COUNT"
if [ "$WARNING_COUNT" -ne 0 ]; then
    echo "error: build produced warnings" >&2
    exit 1
fi
echo "done."
