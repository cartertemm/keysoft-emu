#!/bin/bash
# Copy the Everest device models into a QEMU source tree and build qemu-system-arm.
# Run inside an MSYS2 MINGW64 shell: scripts/build_qemu.sh /c/Users/cartertemm/qemu-everest/qemu
set -euo pipefail

QEMU_TREE="$1"
OVERLAY="$(cd "$(dirname "$0")/../qemu" && pwd)"

add_source() {
	local meson_file="$QEMU_TREE/$1/meson.build" source_set="$2" file="$3"
	grep -q "'$file'" "$meson_file" ||
		echo "$source_set.add(when: 'CONFIG_FSL_IMX31', if_true: files('$file'))" >> "$meson_file"
}

for patch in "$OVERLAY"/patches/*.patch; do
	git -C "$QEMU_TREE" apply --reverse --check "$patch" 2>/dev/null || git -C "$QEMU_TREE" apply "$patch"
done
cp -r "$OVERLAY/hw" "$QEMU_TREE/"
add_source hw/arm arm_common_ss everest.c
add_source hw/ssi system_ss imx31_cspi.c
add_source hw/misc system_ss mc13783.c
add_source hw/dma system_ss imx31_sdma.c
add_source hw/rtc system_ss imx31_rtc.c
add_source hw/block system_ss everest_nand.c
add_source hw/usb system_ss imx31_usb.c
add_source hw/sd system_ss imx31_sdhc.c

mkdir -p "$QEMU_TREE/build"
cd "$QEMU_TREE/build"
[ -f build.ninja ] || ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
	--disable-tools --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
	--disable-slirp --disable-capstone --disable-libcbor
ninja qemu-system-arm.exe
