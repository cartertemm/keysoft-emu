"""Translate virtual addresses inside a ROM module to physical addresses in the kernel image.

Usage: module_address.py <image.bin> <module name> <hex virtual address>...
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import extract_firmware


def locate(rom, module_name, address):
	for entry in rom.modules:
		if rom.name(entry[4]).lower() != module_name.lower():
			continue
		e32 = extract_firmware.E32_ROM.unpack_from(rom.image, entry[5] - rom.base)
		for index in range(e32[0]):
			_, rva, stored_size, data_address, _, _ = extract_firmware.O32_ROM.unpack_from(
				rom.image, entry[6] - rom.base + extract_firmware.O32_ROM.size * index)
			start = e32[3] + rva
			if start <= address < start + stored_size:
				return data_address + address - start
	return None


if __name__ == "__main__":
	rom = extract_firmware.Rom(Path(sys.argv[1]).read_bytes())
	for text in sys.argv[3:]:
		physical = locate(rom, sys.argv[2], int(text, 16))
		word, = struct.unpack_from("<I", rom.image, physical - rom.base)
		print("%s %s -> physical 0x%08x, word 0x%08x" % (sys.argv[2], text, physical, word))
