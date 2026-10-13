"""Extract a BrailleNote Apex firmware update (HUMACE60 .bex images) to disk."""
import argparse
import struct
from pathlib import Path

BEX_MAGIC = b"HUMACE60"
BEX_HEADER_SIZE = 0x200
BEX_NAME_OFFSET = 0x20

ROM_SIGNATURE = b"ECEC"
ROM_SIGNATURE_OFFSET = 0x40
ROMHDR = struct.Struct("<17I2H3I")
MODULE_ENTRY = struct.Struct("<8I")
FILE_ENTRY = struct.Struct("<7I")
E32_ROM = struct.Struct("<2H2I2H5I18IH")
O32_ROM = struct.Struct("<6I")

PAGE_SIZE = 4096
PAGE_HEADER = struct.Struct("<4I")
SIZE_FIELD_BYTES = 3

LZX_POSITION_SLOTS = 32
LZX_MAIN_SYMBOLS = 256 + 8 * LZX_POSITION_SLOTS
LZX_LENGTH_SYMBOLS = 249
LZX_PRETREE_SYMBOLS = 20
LZX_ALIGNED_SYMBOLS = 8
LZX_BLOCK_VERBATIM = 1
LZX_BLOCK_ALIGNED = 2
LZX_BLOCK_UNCOMPRESSED = 3
LZX_MIN_MATCH = 2
LZX_E8_OPCODE = 0xE8
LZX_E8_TAIL = 10
LZX_EXTRA_BITS = [0, 0] + [min(slot // 2 - 1, 17) for slot in range(2, 51)]
LZX_POSITION_BASE = [sum(1 << bits for bits in LZX_EXTRA_BITS[:slot]) for slot in range(51)]

IMAGE_SCN_CNT_CODE = 0x20
IMAGE_SCN_COMPRESSED = 0x2000
IMAGE_SCN_MEM_WRITE = 0x80000000
PE_FILE_ALIGNMENT = 0x200
PE_SECTION_ALIGNMENT = 0x1000
PE_DOS_HEADER_SIZE = 0x40
PE_OPTIONAL_HEADER_SIZE = 0xE0
PE_DIRECTORY_COUNT = 16
ROM_UNIT_COUNT = 9
UNIT_RESOURCE, UNIT_EXCEPTION, UNIT_FIXUP = 2, 3, 5


class BitReader:
	def __init__(self, data):
		padded = bytearray(data) + bytes(len(data) % 2 + 4)
		padded[0::2], padded[1::2] = padded[1::2], padded[0::2]
		self.data = data
		self.value = int.from_bytes(padded, "big")
		self.total = len(padded) * 8
		self.pos = 0

	def peek(self, count):
		return (self.value >> (self.total - self.pos - count)) & ((1 << count) - 1)

	def read(self, count):
		value = self.peek(count)
		self.pos += count
		return value


class Huffman:
	def __init__(self, lengths):
		self.max_length = max(lengths)
		self.table = []
		for length, symbol in sorted((length, symbol) for symbol, length in enumerate(lengths) if length):
			self.table += [(symbol << 5) | length] * (1 << (self.max_length - length))

	def decode(self, reader):
		entry = self.table[reader.peek(self.max_length)]
		reader.pos += entry & 31
		return entry >> 5


def read_lzx_lengths(reader, lengths, first, last):
	pretree = Huffman([reader.read(4) for _ in range(LZX_PRETREE_SYMBOLS)])
	index = first
	while index < last:
		symbol = pretree.decode(reader)
		if symbol == 17:
			run, value = reader.read(4) + 4, 0
		elif symbol == 18:
			run, value = reader.read(5) + 20, 0
		elif symbol == 19:
			run = reader.read(1) + 4
			value = (lengths[index] - pretree.decode(reader)) % 17
		else:
			run, value = 1, (lengths[index] - symbol) % 17
		run = min(run, last - index)
		lengths[index:index + run] = [value] * run
		index += run


def undo_e8_translation(out, file_size):
	index = out.find(LZX_E8_OPCODE)
	while 0 <= index < len(out) - LZX_E8_TAIL:
		target, = struct.unpack_from("<i", out, index + 1)
		if -index <= target < file_size:
			struct.pack_into("<i", out, index + 1, target - index if target >= 0 else target + file_size)
		index = out.find(LZX_E8_OPCODE, index + 5)


def lzx_decompress(data, out_size):
	reader = BitReader(data)
	e8_file_size = reader.read(32) if reader.read(1) else 0
	out = bytearray()
	r0 = r1 = r2 = 1
	main_lengths = [0] * LZX_MAIN_SYMBOLS
	length_lengths = [0] * LZX_LENGTH_SYMBOLS
	while len(out) < out_size:
		block_type = reader.read(3)
		block_end = len(out) + reader.read(24)
		if block_type == LZX_BLOCK_UNCOMPRESSED:
			start = ((reader.pos + 16) & ~15) // 8
			r0, r1, r2 = struct.unpack_from("<3I", data, start)
			start += 12
			size = block_end - len(out)
			out += data[start:start + size]
			reader.pos = (start + size + size % 2) * 8
			continue
		if block_type not in (LZX_BLOCK_VERBATIM, LZX_BLOCK_ALIGNED):
			raise ValueError(f"bad LZX block type {block_type}")
		if block_type == LZX_BLOCK_ALIGNED:
			aligned_tree = Huffman([reader.read(3) for _ in range(LZX_ALIGNED_SYMBOLS)])
		read_lzx_lengths(reader, main_lengths, 0, 256)
		read_lzx_lengths(reader, main_lengths, 256, LZX_MAIN_SYMBOLS)
		read_lzx_lengths(reader, length_lengths, 0, LZX_LENGTH_SYMBOLS)
		main_tree = Huffman(main_lengths)
		length_tree = Huffman(length_lengths)
		while len(out) < block_end:
			symbol = main_tree.decode(reader)
			if symbol < 256:
				out.append(symbol)
				continue
			symbol -= 256
			match_length = symbol & 7
			if match_length == 7:
				match_length += length_tree.decode(reader)
			match_length += LZX_MIN_MATCH
			slot = symbol >> 3
			if slot == 0:
				offset = r0
			elif slot == 1:
				offset, r1 = r1, r0
				r0 = offset
			elif slot == 2:
				offset, r2 = r2, r0
				r0 = offset
			else:
				extra = LZX_EXTRA_BITS[slot]
				offset = LZX_POSITION_BASE[slot] - 2
				if block_type == LZX_BLOCK_ALIGNED and extra >= 3:
					offset += (reader.read(extra - 3) << 3) + aligned_tree.decode(reader)
				else:
					offset += reader.read(extra)
				r0, r1, r2 = offset, r0, r1
			start = len(out) - offset
			if start < 0:
				raise ValueError("LZX match before start of page")
			if offset >= match_length:
				out += out[start:start + match_length]
			else:
				for index in range(start, start + match_length):
					out.append(out[index])
	del out[out_size:]
	if e8_file_size:
		undo_e8_translation(out, e8_file_size)
	return bytes(out)


def ce_decompress(blob):
	size = int.from_bytes(blob[:SIZE_FIELD_BYTES], "little")
	page_count = -(-size // PAGE_SIZE)
	out = bytearray()
	start = SIZE_FIELD_BYTES * (1 + page_count)
	for page in range(page_count):
		field = SIZE_FIELD_BYTES * (1 + page)
		end = int.from_bytes(blob[field:field + SIZE_FIELD_BYTES], "little")
		header_size, page_size, packed_size, _ = PAGE_HEADER.unpack_from(blob, start)
		out += lzx_decompress(blob[start + header_size:start + header_size + packed_size], page_size)
		start = end
	return bytes(out)


class Rom:
	def __init__(self, image):
		self.image = image
		toc_offset, = struct.unpack_from("<I", image, ROM_SIGNATURE_OFFSET + 8)
		header = ROMHDR.unpack_from(image, toc_offset)
		self.base = header[2]
		self.cpu_type = header[17]
		entry = toc_offset + ROMHDR.size
		self.modules = [MODULE_ENTRY.unpack_from(image, entry + MODULE_ENTRY.size * index) for index in range(header[4])]
		entry += MODULE_ENTRY.size * header[4]
		self.files = [FILE_ENTRY.unpack_from(image, entry + FILE_ENTRY.size * index) for index in range(header[12])]

	def read(self, address, size):
		offset = address - self.base
		return self.image[offset:offset + size]

	def name(self, address):
		offset = address - self.base
		return self.image[offset:self.image.index(0, offset)].decode("latin1")

	def file_data(self, entry):
		_, _, _, size, packed_size, _, address = entry
		data = self.read(address, packed_size)
		return data if packed_size == size else ce_decompress(data)

	def module_data(self, entry):
		e32 = E32_ROM.unpack_from(self.image, entry[5] - self.base)
		objects = [O32_ROM.unpack_from(self.image, entry[6] - self.base + O32_ROM.size * index) for index in range(e32[0])]
		return build_pe(self, e32, objects)


def align(value, alignment):
	return -(-value // alignment) * alignment


def section_name(rva, flags, units):
	for unit, name in ((UNIT_RESOURCE, b".rsrc"), (UNIT_EXCEPTION, b".pdata"), (UNIT_FIXUP, b".reloc")):
		if units[unit][0] == rva:
			return name
	if flags & IMAGE_SCN_CNT_CODE:
		return b".text"
	return b".data" if flags & IMAGE_SCN_MEM_WRITE else b".rdata"


def build_pe(rom, e32, objects):
	(_, image_flags, entry_rva, image_base, subsystem_major, subsystem_minor,
		stack_size, image_size, _, _, timestamp, *unit_fields, subsystem) = e32
	units = list(zip(unit_fields[0::2], unit_fields[1::2]))
	headers_size = align(PE_DOS_HEADER_SIZE + 24 + PE_OPTIONAL_HEADER_SIZE + 40 * len(objects), PE_FILE_ALIGNMENT)
	section_headers = b""
	body = b""
	sizes = {"code": 0, "data": 0, "bss": 0}
	for virtual_size, rva, packed_size, address, _, flags in objects:
		data = rom.read(address, packed_size)
		if flags & IMAGE_SCN_COMPRESSED:
			data = ce_decompress(data)
		data += bytes(-len(data) % PE_FILE_ALIGNMENT)
		kind = "code" if flags & IMAGE_SCN_CNT_CODE else "data" if data else "bss"
		sizes[kind] += len(data) or align(virtual_size, PE_SECTION_ALIGNMENT)
		section_headers += struct.pack("<8s8I", section_name(rva, flags, units), virtual_size, rva, len(data),
			headers_size + len(body) if data else 0, 0, 0, 0, flags & ~IMAGE_SCN_COMPRESSED)
		body += data
	stored = [(rva, rva + virtual_size) for virtual_size, rva, packed_size, *_ in objects if packed_size]
	directories = [unit if any(start <= unit[0] < end for start, end in stored) else (0, 0) for unit in units]
	directories += [(0, 0)] * (PE_DIRECTORY_COUNT - ROM_UNIT_COUNT)
	dos_header = b"MZ" + bytes(PE_DOS_HEADER_SIZE - 6) + struct.pack("<I", PE_DOS_HEADER_SIZE)
	coff_header = b"PE\0\0" + struct.pack("<2H3I2H", rom.cpu_type, len(objects), timestamp, 0, 0,
		PE_OPTIONAL_HEADER_SIZE, image_flags)
	optional_header = struct.pack("<H2B9I6H4I2H6I", 0x10B, 0, 0, sizes["code"], sizes["data"], sizes["bss"],
		entry_rva, objects[0][1], 0, image_base, PE_SECTION_ALIGNMENT, PE_FILE_ALIGNMENT,
		4, 0, 0, 0, subsystem_major, subsystem_minor, 0, image_size, headers_size, 0,
		subsystem, 0, stack_size, PAGE_SIZE, 0x100000, PAGE_SIZE, 0, PE_DIRECTORY_COUNT)
	optional_header += b"".join(struct.pack("<2I", *directory) for directory in directories)
	headers = dos_header + coff_header + optional_header + section_headers
	return headers + bytes(headers_size - len(headers)) + body


def extract_rom(image, out_dir):
	rom = Rom(image)
	for folder, entries, name_field, read in (("modules", rom.modules, 4, rom.module_data), ("files", rom.files, 5, rom.file_data)):
		(out_dir / folder).mkdir(parents=True, exist_ok=True)
		for entry in entries:
			(out_dir / folder / rom.name(entry[name_field])).write_bytes(read(entry))
		print(f"  {len(entries)} {folder}")


def extract_update(update_dir, out_dir):
	for path in sorted(update_dir.iterdir()):
		data = path.read_bytes() if path.is_file() else b""
		if not data.startswith(BEX_MAGIC):
			continue
		name = data[BEX_NAME_OFFSET:data.index(0, BEX_NAME_OFFSET)].decode("ascii")
		image = data[BEX_HEADER_SIZE:]
		image_dir = out_dir / Path(name).stem
		image_dir.mkdir(parents=True, exist_ok=True)
		(image_dir / "image.bin").write_bytes(image)
		print(f"{path.name} -> {image_dir}")
		if image[ROM_SIGNATURE_OFFSET:ROM_SIGNATURE_OFFSET + len(ROM_SIGNATURE)] == ROM_SIGNATURE:
			extract_rom(image, image_dir)


if __name__ == "__main__":
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("update_dir", type=Path, help="folder that holds the extracted CAB contents")
	parser.add_argument("out_dir", type=Path, help="folder to write the extracted firmware to")
	arguments = parser.parse_args()
	extract_update(arguments.update_dir, arguments.out_dir)
