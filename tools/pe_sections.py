#!/usr/bin/env python3
"""pe_sections.py <exe> <output> — writes the pe_sections.bin that `boot` loads, from the player's exe.
Same format as tools/boot.py (read_pe_sections + writer): n, then (va, size, bytes) per section,
PE headers included (the CRT validates them in memory)."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_parser import PEFile

exe, sortie = sys.argv[1], sys.argv[2]
pe = PEFile(exe).parse()
sections = [(pe.image_base, pe._raw[:0x1000])]
for s in pe.sections:
    off = pe.rva_to_offset(s.virtual_address)
    sections.append((pe.image_base + s.virtual_address, pe._raw[off:off + s.size_of_raw_data]))
with open(sortie, "wb") as f:
    f.write(struct.pack("<I", len(sections)))
    for va, raw in sections:
        f.write(struct.pack("<II", va, len(raw)))
        f.write(raw)
print(f"{len(sections)} sections -> {sortie}")
