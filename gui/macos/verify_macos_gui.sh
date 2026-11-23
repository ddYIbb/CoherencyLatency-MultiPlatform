#!/usr/bin/env bash
# Post-build verification for the CoherencyLatency macOS GUI.
#   wsl -e bash /mnt/c/Users/Administrator/AppData/Local/Temp/cl_repo/gui/macos/verify_macos_gui.sh
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$SCRIPT_DIR/build"
APP="$BUILD/CoherencyLatency.app"
OTOOL="${OTOOL:-llvm-otool-21}"
FILE="${FILE:-file}"
LIPO="${LIPO:-llvm-lipo-21}"

echo "### Mach-O headers"
for arch in x86_64 arm64; do
    echo "--- CoherencyLatency-$arch"
    "$FILE" -b "$BUILD/CoherencyLatency-$arch"
    "$OTOOL" -l "$BUILD/CoherencyLatency-$arch" | grep -E 'LC_VERSION_MIN_MACOSX|LC_BUILD_VERSION|sdk |version |minos |platform ' | head -8
    echo "--- linked libraries"
    "$OTOOL" -L "$BUILD/CoherencyLatency-$arch" | sed 's/^/    /'
done

echo
echo "### universal binary"
"$FILE" -b "$BUILD/CoherencyLatency"
"$LIPO" -info "$BUILD/CoherencyLatency" | sed 's/^/    /'


echo
echo "### app bundle"
find "$APP" -type f | sort | while read -r path; do
    printf '    %10s  %s\n' "$(stat -c %s "$path")" "${path#$BUILD/}"
done
echo "--- Info.plist keys"
python3 - "$APP/Contents/Info.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], 'rb') as handle:
    data = plistlib.load(handle)
for key in sorted(data):
    print('    %-28s %s' % (key, data[key]))
PY
echo "--- bundle executable"
"$FILE" -b "$APP/Contents/MacOS/CoherencyLatency"
echo "--- bundle icon"
"$FILE" -b "$APP/Contents/Resources/app.icns"
cmp -s "$APP/Contents/Resources/app.icns" "$SCRIPT_DIR/../assets/app.icns" && echo "    icon identical to gui/assets/app.icns"
echo "--- PkgInfo"
printf '    %s\n' "$(cat "$APP/Contents/PkgInfo")"

echo
echo "### icon payloads"
python3 - "$APP/Contents/Resources/app.icns" <<'PY'
import struct, sys
blob = open(sys.argv[1], 'rb').read()
magic, length = struct.unpack('>4sI', blob[:8])
print('    magic %s length %d (file %d)' % (magic.decode('latin1'), length, len(blob)))
offset = 8
while offset + 8 <= len(blob):
    kind, size = struct.unpack('>4sI', blob[offset:offset + 8])
    print('    %s %d bytes' % (kind.decode('latin1'), size))
    offset += size
PY

echo
echo "### layout self-test (numeric check of the window geometry)"
if [ -x "$SCRIPT_DIR/layout-selftest" ]; then
    "$SCRIPT_DIR/layout-selftest" | sed 's/^/    /'
else
    echo "    layout-selftest missing - run build_macos_gui.sh first"
fi

echo
echo "### warnings recorded by the build"
for log in "$BUILD"/obj/*.log "$BUILD"/obj/*.log.*; do
    [ -e "$log" ] || continue
    count=$(grep -c 'warning:' "$log" || true)
    printf '    %-30s %s warnings\n' "$(basename "$log")" "$count"
done
