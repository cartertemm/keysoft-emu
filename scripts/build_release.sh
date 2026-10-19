#!/bin/bash
# Build the portable release: dist/keysoft-emu-<version>.zip with start-emu, the keyboard,
# start-emu-qt.bat, QEMU, qemu-img, and the DLLs they need.
# Usage (MSYS2 MINGW64 shell): scripts/build_release.sh [qemu source tree]
# The default tree is the parent of $QEMU_BUILD, or ~/qemu-everest/qemu.
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_BUILD="${QEMU_BUILD:-$HOME/qemu-everest/qemu/build}"
QEMU_TREE="${1:-$(dirname "$QEMU_BUILD")}"
SLIRP_DIR="$QEMU_TREE/build/subprojects/slirp"
DIST="$PROJECT/dist"
STAGE="$DIST/keysoft-emu"
VERSION="$(tr -d '\r\n' < "$PROJECT/VERSION")"
ZIP="$DIST/keysoft-emu-$VERSION.zip"

[ -f "$QEMU_TREE/build/qemu-system-arm.exe" ] || { echo "No QEMU build in $QEMU_TREE/build. Build it first with: bash scripts/build_qemu.sh $QEMU_TREE" >&2; exit 1; }

rm -rf "$STAGE" "$ZIP"
mkdir -p "$STAGE/qemu"
gcc -O2 -DVERSION="\"$VERSION\"" -o "$STAGE/start-emu.exe" "$PROJECT/tools/start-emu.c" -lws2_32
gcc -O2 -mwindows -o "$STAGE/keyboard.exe" "$PROJECT/tools/keyboard.c" -lws2_32

# qemu-img gets its own build folder so the emulator build keeps its options.
if [ ! -f "$QEMU_TREE/build-img/qemu-img.exe" ]; then
	mkdir -p "$QEMU_TREE/build-img"
	(cd "$QEMU_TREE/build-img" &&
		{ [ -f build.ninja ] || ../configure --disable-system --disable-user --enable-tools --disable-docs \
			--disable-werror --disable-guest-agent --disable-capstone --disable-libcbor; } &&
		ninja qemu-img.exe)
fi

cp "$QEMU_TREE/build/qemu-system-arm.exe" "$QEMU_TREE/build-img/qemu-img.exe" "$SLIRP_DIR/libslirp-0.dll" "$STAGE/qemu/"
strip "$STAGE/qemu/"*.exe "$STAGE/"*.exe

dependencies="$(cd "$STAGE/qemu" && PATH="/mingw64/bin:$PATH" ldd ./*.exe | awk '$2 == "=>" { print $3 }' | sort -u)"
outside="$(grep -viE '^/c/windows/|^/mingw64/bin/' <<< "$dependencies" | grep -v "^$STAGE/qemu/" || true)"
if [ -n "$outside" ]; then
	echo "These DLLs are not from MSYS2 mingw64:" >&2
	echo "$outside" >&2
	exit 1
fi
grep '^/mingw64/bin/' <<< "$dependencies" | xargs -r cp -t "$STAGE/qemu/"

printf '@"%%~dp0start-emu.exe" --qt\r\n' > "$STAGE/start-emu-qt.bat"
cp "$PROJECT/readme.md" "$STAGE/"
cp "$QEMU_TREE/COPYING" "$QEMU_TREE/COPYING.LIB" "$STAGE/qemu/"
(cd "$DIST" && /c/Windows/System32/tar.exe -a -c -f "$(basename "$ZIP")" keysoft-emu)
echo "Wrote $ZIP (version $VERSION)"
