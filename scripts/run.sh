#!/bin/bash
# Boot the extracted Apex kernel on the Everest board. Extra arguments go to QEMU.
# Usage (MSYS2 MINGW64 shell): scripts/run.sh [qemu arguments]
set -euo pipefail

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
QEMU_BUILD="${QEMU_BUILD:-$HOME/qemu-everest/qemu/build}"
MINGW_BIN="${MINGW_BIN:-/mingw64/bin}"
QEMU_IMG="${QEMU_IMG:-$(command -v qemu-img || echo "/c/Program Files/qemu/qemu-img.exe")}"
STATE_DIR="${STATE_DIR:-$PROJECT/state}"
SERIAL="${SERIAL:-stdio}"
# The guest's telnet server is reachable on this host port.
TELNET_PORT="${TELNET_PORT:-2323}"
# tools/keyboard.exe sends braille key presses to this host port.
KEYS_PORT="${KEYS_PORT:-5555}"
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

# Guest time follows executed instructions. With host-paced time, a slow host
# disk write can make the guest program its tick timer in the past and freeze.
PATH="$QEMU_BUILD/subprojects/slirp:$MINGW_BIN:$PATH" exec "$QEMU_BUILD/qemu-system-arm.exe" -M everest -display none -serial "$SERIAL" \
	-icount shift=auto \
	-nic "user,model=lan9118,hostfwd=tcp:127.0.0.1:$TELNET_PORT-:23" \
	-chardev "socket,id=keys,host=127.0.0.1,port=$KEYS_PORT,server=on,wait=off" \
	"${nand_drives[@]}" \
	-device loader,file="$PROJECT/extracted/Everest_Kernel/image.bin",addr=$KERNEL_ADDR,force-raw=on \
	-device loader,addr=$BOOT_STUB_ADDR,cpu-num=0 "$@"
