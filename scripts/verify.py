import sys, struct, zlib, collections
from pathlib import Path
sys.path.insert(0, sys.argv[2])
import extract_firmware as x
out = Path(sys.argv[3]) / "Everest_Kernel"
rom = x.Rom(open(sys.argv[1], "rb").read()[0x200:])
bad = [rom.name(e[5]) for e in rom.files if (out / "files" / rom.name(e[5])).stat().st_size != e[3]]
print("files with wrong size:", bad)
print("on disk:", len(list((out/"modules").iterdir())), len(list((out/"files").iterdir())))
def rva2off(pe, rva):
	lfa = struct.unpack_from("<I", pe, 0x3c)[0]; n = struct.unpack_from("<H", pe, lfa+6)[0]
	for i in range(n):
		nm, vs, va, rs, rp = struct.unpack_from("<8s4I", pe, lfa+24+0xE0+40*i)
		if va <= rva < va + max(vs, rs) and rva - va < rs: return rp + rva - va
	return None
def cstr(pe, o): return pe[o:pe.index(0, o)].decode("latin1")
def imports(pe):
	lfa = struct.unpack_from("<I", pe, 0x3c)[0]
	rva, size = struct.unpack_from("<2I", pe, lfa+24+96+8)
	if not rva: return []
	o = rva2off(pe, rva); r = []
	if o is None: return ["<import dir unmapped>"]
	while True:
		d = struct.unpack_from("<5I", pe, o); o += 20
		if not d[3]: break
		no = rva2off(pe, d[3]); r.append(cstr(pe, no) if no is not None else "?")
	return r
stats = collections.Counter(); users = collections.Counter(); overlap = []; unmapped = []
for e in rom.modules:
	name = rom.name(e[4]); pe = (out/"modules"/name).read_bytes()
	lfa = struct.unpack_from("<I", pe, 0x3c)[0]; n = struct.unpack_from("<H", pe, lfa+6)[0]
	secs = sorted(struct.unpack_from("<8s4I", pe, lfa+24+0xE0+40*i)[1:3][::-1] for i in range(n))
	if any(a[0]+a[1] > b[0] for a, b in zip(secs, secs[1:])): overlap.append(name)
	fix = struct.unpack_from("<2I", pe, lfa+24+96+40)
	stats["has reloc dir"] += bool(fix[0]); stats["reloc data present"] += bool(fix[0] and rva2off(pe, fix[0]) is not None)
	imps = imports(pe)
	if "<import dir unmapped>" in imps or "?" in imps: unmapped.append(name)
	for i in imps: users[i.lower()] += 1
print("modules", len(rom.modules), dict(stats))
print("overlapping sections:", overlap)
print("import parse problems:", unmapped[:20], len(unmapped))
ks = (out/"modules"/"KeySoft.exe").read_bytes()
print("KeySoft.exe imports:", imports(ks))
print("VAFramework imports:", imports((out/"modules"/"VAFramework.dll").read_bytes()))
print("braille_display.dll (file) imports:", imports((out/"files"/"braille_display.dll").read_bytes()))
print("who imports harp_60_services:", users.get("harp_60_services.dll"))
