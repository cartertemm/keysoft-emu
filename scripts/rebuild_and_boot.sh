#!/bin/bash
# Rebuild QEMU, boot for a number of seconds, and save the serial output and device log.
# Usage (Git Bash): scripts/rebuild_and_boot.sh <seconds> <output folder>
set -uo pipefail

SECONDS_TO_RUN="$1"
OUT="$2"
PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
MSYS2="${MSYS2:-/c/Users/cartertemm/scoop/apps/msys2/current}"
QEMU_TREE="${QEMU_TREE:-/c/Users/cartertemm/qemu-everest/qemu}"

MSYSTEM=MINGW64 "$MSYS2/usr/bin/bash.exe" -lc "bash '$PROJECT/scripts/build_qemu.sh' '$QEMU_TREE' > '$OUT/build.log' 2>&1"
if [ $? -ne 0 ]; then
	grep -E "error" "$OUT/build.log" | head -20
	exit 1
fi
rm -f "$OUT/device.log"
timeout "$SECONDS_TO_RUN" bash "$PROJECT/scripts/run.sh" -monitor none -d guest_errors,unimp -D "$OUT/device.log" > "$OUT/serial.txt" 2>&1
wc -l "$OUT/serial.txt" "$OUT/device.log"
