#!/bin/bash
# Copy the Everest device models into a QEMU source tree and build qemu-system-arm.
# Run inside an MSYS2 MINGW64 shell: scripts/build_qemu.sh [qemu source tree]
# The default tree is the parent of $QEMU_BUILD, or ~/qemu-everest/qemu.
set -euo pipefail

QEMU_BUILD="${QEMU_BUILD:-$HOME/qemu-everest/qemu/build}"
QEMU_TREE="${1:-$(dirname "$QEMU_BUILD")}"
OVERLAY="$(cd "$(dirname "$0")/../qemu" && pwd)"

[ -d "$QEMU_TREE/.git" ] || { echo "No QEMU source in $QEMU_TREE. Clone it with: git clone --branch v11.1.0 https://gitlab.com/qemu-project/qemu.git $QEMU_TREE" >&2; exit 1; }

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
add_source hw/audio system_ss imx31_ssi.c
add_source hw/input system_ss everest_keys.c

mkdir -p "$QEMU_TREE/build"
cd "$QEMU_TREE/build"
[ -f build.ninja ] || ../configure --target-list=arm-softmmu --disable-docs --disable-werror \
	--disable-tools --disable-guest-agent --disable-gtk --disable-sdl --disable-vnc \
	--enable-slirp --disable-capstone --disable-libcbor
ninja qemu-system-arm.exe
