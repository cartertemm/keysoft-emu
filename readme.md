# KeySoft Emulator

This project recreates KeySoft 9.5.1 from the HumanWare BrailleNote Apex and runs the original Windows CE software on a modern Windows computer.

Released in late 2009, the BrailleNote Apex became one of the most widely adopted electronic braille notetakers in educational systems (K-12 and universities) and workplace accessibility programs throughout the 2010s. Support for the BrailleNote Apex has officially been discontinued.

I and many others grew up using the BrailleNote MPower, PK, and later the Apex. The Apex specifically was my computer before I cared about a computer, and what I believe to be a huge driver of my present interest in technology. The last unit I had is now sadly stuck in a boot loop at the bottom of a box in a closet in my home office. So I wanted to see how closely its hardware could be reproduced in software.

This emulator was built for the purposes of nostalgia, and to see how far I could go. It was not built to rip off HumanWare or redistribute their technology. Almost everything in the original KeySoft experience works, including speech, networking, persistent storage, and braille keyboard input.

## Getting started

You will need Windows, Python 3, an MSYS2 MINGW64 build environment, QEMU 11.1.0 source code, and a legally obtained KeySoft 9.5.1 update file. The firmware is not included in this repository.

First, use 7 Zip to extract the update file into a folder named `firmware`. You can use the built in Windows expansion tool instead:

```powershell
mkdir firmware
expand.exe .\KeySoft-9.5.1.hwi -F:* .\firmware
```

Extract the Windows CE images from the unpacked update:

```powershell
py .\scripts\extract_firmware.py .\firmware .\extracted
```

Next, open an MSYS2 MINGW64 shell. Clone QEMU, apply the custom device models, and build it:

```bash
mkdir -p ~/qemu-everest
git clone --branch v11.1.0 https://gitlab.com/qemu-project/qemu.git ~/qemu-everest/qemu
bash scripts/build_qemu.sh ~/qemu-everest/qemu
gcc -O2 -mwindows -o tools/keyboard.exe tools/keyboard.c -lws2_32
```

From PowerShell or Command Prompt, start the emulator through MSYS2:

```powershell
msys2 scripts/run.sh
```

This uses the paths configured near the top of `scripts/run.sh`. If your QEMU or MSYS2 installation is elsewhere, open an MSYS2 shell and override them when starting the emulator:

```bash
QEMU_BUILD=~/qemu-everest/qemu/build \
QEMU_IMG=/c/path/to/qemu-img.exe \
MINGW_BIN=/mingw64/bin \
bash scripts/run.sh
```

Open another terminal and run `tools/keyboard.exe` for braille keyboard input. The emulator also forwards the guest Telnet server to `localhost:2323`.

The first boot creates four persistent NAND images in `state`. Later runs reuse them.

## How it works (the nerdy stuff)

The Apex uses an ARM1136 based Freescale i.MX31 system on a chip. Its KeySoft software runs on Windows CE and expects the original board, known internally as Everest, along with its memory map and peripherals.

The `qemu` directory adds an Everest machine to QEMU. It models the board's NAND flash, SPI controllers, power management chip, real time clock, SD controllers, USB controller, Ethernet adapter, serial ports, keyboard controller, DMA engine, and audio interface. QEMU runs the original ARM kernel while translating those devices to services available on the Windows host.

The firmware extractor reads the update's `HUMACE60` images, decompresses the Windows CE ROM, and reconstructs its files and executable modules. At startup, QEMU loads the extracted kernel and supplies the small amount of configuration normally left behind by the Apex bootloader.

Four sparse QEMU images stand in for the Apex NAND chips. Audio written through the emulated DMA and synchronous serial interfaces is sent to the host audio system. A small Windows program converts PC key events into the protocol used by the Apex keyboard controller. User mode networking exposes the guest's Telnet service without requiring a bridged network adapter.

The machine also applies three narrow patches in guest memory. They bypass license checks that depend on data from unavailable NOR flash, enable the built in Telnet server, and stop a service thread from spinning when the original hardware events are absent.

## Known limitations

Almost everything works, with some notable exceptions:

1. Braille display output is not shown, and connected braille displays are not yet supported.
2. USB devices and removable storage are not supported.
3. VGA display output is not supported.
4. The FM radio is not supported.
