#!/bin/bash
# Boot the extracted Apex kernel on the Everest board. Extra arguments go to QEMU.
# Usage (Git Bash or MSYS2): scripts/run.sh [qemu arguments]
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_BUILD="${QEMU_BUILD:-/c/Users/cartertemm/qemu-everest/qemu/build}"
MINGW_BIN="${MINGW_BIN:-/c/Users/cartertemm/scoop/apps/msys2/current/mingw64/bin}"
QEMU_IMG="${QEMU_IMG:-/c/Users/cartertemm/scoop/apps/qemu/current/qemu-img.exe}"
STATE_DIR="${STATE_DIR:-$PROJECT/state}"
SERIAL="${SERIAL:-stdio}"
KERNEL_ADDR=0x80200000
# The Everest board puts a small stub here that stands in for the boot loader.
BOOT_STUB_ADDR=0x80010000
NAND_CHIP_COUNT=4
# Each chip: 4096 blocks of 128 pages, each page 4096 data bytes plus 224 spare bytes.
NAND_IMAGE_BYTES=2264924160

mkdir -p "$STATE_DIR"
nand_drives=()
for ((chip = 0; chip < NAND_CHIP_COUNT; chip++)); do
	image="$STATE_DIR/nand$chip.qcow2"
	[ -f "$image" ] || "$QEMU_IMG" create -q -f qcow2 "$image" $NAND_IMAGE_BYTES
	nand_drives+=(-drive "if=mtd,index=$chip,format=qcow2,file=$image")
done

PATH="$MINGW_BIN:$PATH" exec "$QEMU_BUILD/qemu-system-arm.exe" -M everest -display none -serial "$SERIAL" \
	"${nand_drives[@]}" \
	-device loader,file="$PROJECT/extracted/Everest_Kernel/image.bin",addr=$KERNEL_ADDR,force-raw=on \
	-device loader,addr=$BOOT_STUB_ADDR,cpu-num=0 "$@"
