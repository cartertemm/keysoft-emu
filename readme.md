# KeySoft Emulator

This project recreates KeySoft 9.5.1 from the HumanWare BrailleNote Apex and runs the original Windows CE software on a modern Windows computer. Almost everything in the original KeySoft experience works, including speech, networking, persistent storage, and braille keyboard input.

[Listen to a demo (MP3, 11 MB)](https://ctemm.me/files/keysoft-emu-demo.mp3)

Released in late 2009, the BrailleNote Apex became one of the most widely adopted electronic braille notetakers in educational systems (K-12 and universities) and workplace accessibility programs throughout the 2010s. It remained a primary flagship device for HumanWare for roughly seven years until the introduction of the Android-based BrailleNote Touch in 2016. Support for the BrailleNote Apex has officially been discontinued.

I and many others grew up using the BrailleNote MPower, PK, and later the Apex. The Apex specifically was my computer before I cared about a computer, and what I believe to be a huge driver of my present interest in technology. The last unit I had is now sadly stuck in a boot loop at the bottom of a box in a closet in my home office. So I wanted to see how closely its hardware could be reproduced in software.

This emulator was built for the purposes of nostalgia, and to see how far I could go. It was not built to rip off HumanWare or redistribute their technology. It runs the original KeySoft 9.5 update files (version 9.5.0, which HumanWare had on their website before they shelved the product, and the later version 9.5.1). This repository does not include those files, the ROM, or any other part of the firmware.

This project is not affiliated with or endorsed by HumanWare. HumanWare, BrailleNote, and KeySoft are trademarks of their owners and are used here only to describe the original product. The emulator is for preservation and personal use. It is free, and I don't accept money for it in any form, including donations and ads. It should go without saying, but do not bug Humanware about the issues that you have with this project. They cannot and will not help you.

## Getting started

If you want to get up and running quickly, you do not need to build the emulator. Just download the portable zip file, which contains everything you need to get this working on Windows. The build steps are only for developers that want to work on the project.

### Quick start

1. Download `keysoft-emu.zip` from the [Releases](../../releases) page and unzip it.
2. Get a KeySoft 9.5 update file. The list and hashes in the next section tell you which files work.
3. Put the update file in the `keysoft-emu` folder.
4. Run `start-emu.exe`. The first run extracts the firmware and checks it, then the emulator starts and the braille keyboard window opens.

### Building from source

You will need:

* Windows
* Python 3
* [MSYS2](https://www.msys2.org/)
* [QEMU for Windows](https://qemu.weilnetz.de/w64/), for the `qemu-img` tool. The QEMU build below does not include it.
* A legally obtained KeySoft 9.5 update file. Either of these works:
  * `KeySoft 9 5 0 Build 1280.hwi`, from HumanWare's support site on the [Internet Archive](https://web.archive.org/web/20210510082627/https://support.humanware.com/en-international/support/braillenote_apex/software/keysoft_95). This file belongs to HumanWare.
  * `KeySoft 9 5 1 Build 1285.hwi`, if you have it. As far as I know, no public copy exists.

To make sure you have the right file, compare its SHA-256 hash. In PowerShell, run `Get-FileHash` on the file.

| File | SHA-256 |
| --- | --- |
| `KeySoft 9 5 0 Build 1280.hwi` | `25d38528222e7a94e78fa605558c59ac520dbe1bff49dcd435019789d6b248f6` |
| `KeySoft 9 5 1 Build 1285.hwi` | `d387d8ebe15a7b284883b6c398e2e02f199899b7eb3a519a3b11c363e3bb1269` |

The correct file is about 64 MB. The Internet Archive sends it compressed. A web browser unpacks it for you, but if you download it with `curl`, add `--compressed`.

Put the update file in the root of this repository. Then use 7 Zip to extract it into a folder named `firmware`. If you don't have 7 Zip, you can use the built in Windows expansion tool instead. From PowerShell in the repository root (use your file's name):

```powershell
mkdir firmware
expand.exe ".\KeySoft 9 5 0 Build 1280.hwi" -F:* .\firmware
```

Extract the Windows CE images from the unpacked update:

```powershell
python .\scripts\extract_firmware.py .\firmware .\extracted
```

Next, open an MSYS2 MINGW64 shell. Install the build tools:

```bash
pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-ninja mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python
```

Go to the repository root (for example, `cd /c/path/to/keysoft-emu`). Clone QEMU, apply the custom device models, and build it. Then build the keyboard tool:

```bash
mkdir -p ~/qemu-everest
git clone --branch v11.1.0 https://gitlab.com/qemu-project/qemu.git ~/qemu-everest/qemu
bash scripts/build_qemu.sh ~/qemu-everest/qemu
gcc -O2 -mwindows -o tools/keyboard.exe tools/keyboard.c -lws2_32
```

In MSYS2, `~` is your MSYS2 home folder (for example, `C:\msys64\home\yourname`), not your Windows user folder.

Start the emulator from the same MINGW64 shell, in the repository root:

```bash
bash scripts/run.sh
```

By default, `run.sh` looks for QEMU in `~/qemu-everest/qemu/build` and for `qemu-img` on your PATH or in `C:\Program Files\qemu`. If yours are somewhere else, set these variables when you start the emulator:

```bash
QEMU_BUILD=/c/path/to/qemu/build QEMU_IMG=/c/path/to/qemu-img.exe bash scripts/run.sh
```

Open another terminal and run `tools/keyboard.exe` for braille keyboard input. The emulator also forwards the guest Telnet server to `localhost:2323`.

The first boot creates four persistent NAND images in `state`. Later runs reuse them.

## Keyboard

Type on the Apex braille keyboard with these PC keys. The keyboard window must have focus.

| PC key | Apex key |
| --- | --- |
| F, D, S | Dots 1, 2, 3 |
| J, K, L | Dots 4, 5, 6 |
| Backspace | Dot 7 (Backspace) |
| Enter | Dot 8 (Enter) |
| Space or Alt | Space |
| F12 | Power switch (off and on) |
| Escape | Close the keyboard window |

Press keys together for chords, the same as on the real keyboard. Most PC keyboards can only detect six letter keys at the same time, so a chord of space with all six dots can lose a key. Use Alt for space in large chords, because Alt does not count toward that limit.

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
