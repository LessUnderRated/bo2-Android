"""
test_pe_parser — parse the hand-crafted minimal PE32 fixture and assert every
field matches the layout baked into make_minimal_pe.py.

Proves the ingester scaffold parses headers, sections, imports, and base
relocations correctly. No Windows toolchain needed — the fixture is built
on macOS by make_minimal_pe.py.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import make_minimal_pe
from pe_parser import PEFile, IMAGE_FILE_MACHINE_I386

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "minimal_fixture.exe")


def main():
    make_minimal_pe.main()
    pe = PEFile(FIXTURE).parse()

    failures = []

    def check(cond, msg):
        if not cond:
            failures.append(msg)

    # headers
    check(pe.machine == IMAGE_FILE_MACHINE_I386, f"machine: expected I386(0x14C), got {pe.machine:#x}")
    check(not pe.is_pe32_plus, "should be PE32, not PE32+")
    check(pe.image_base == 0x00400000, f"image_base: {pe.image_base:#x}")
    check(pe.entry_point_rva == 0x1000, f"entry: {pe.entry_point_rva:#x}")
    check(pe.section_alignment == 0x1000, f"section_alignment: {pe.section_alignment:#x}")
    check(pe.file_alignment == 0x200, f"file_alignment: {pe.file_alignment:#x}")
    check(pe.size_of_image == 0x2000, f"size_of_image: {pe.size_of_image:#x}")
    check(pe.size_of_headers == 0x200, f"size_of_headers: {pe.size_of_headers:#x}")
    check(len(pe.data_directories) == 16, f"data dirs: {len(pe.data_directories)}")

    # sections
    check(len(pe.sections) == 1, f"sections: {len(pe.sections)}")
    s = pe.sections[0]
    check(s.name == ".text", f"section name: {s.name!r}")
    check(s.virtual_address == 0x1000, f"section va: {s.virtual_address:#x}")
    check(s.virtual_size == 0x1000, f"section vsize: {s.virtual_size:#x}")
    check(s.pointer_to_raw_data == 0x200, f"section raw ptr: {s.pointer_to_raw_data:#x}")
    check(s.size_of_raw_data == 0x400, f"section raw size: {s.size_of_raw_data:#x}")
    check(s.is_code and s.is_executable, "section should be CODE+EXEC")

    # imports
    check(len(pe.imports) == 1, f"imports: {len(pe.imports)} DLLs")
    if pe.imports:
        imp = pe.imports[0]
        check(imp.dll == "testdll.dll", f"import dll: {imp.dll!r}")
        check(len(imp.thunks) == 1, f"import thunks: {len(imp.thunks)}")
        if imp.thunks:
            t = imp.thunks[0]
            check(t.name == "ImportedFunc", f"import func: {t.name!r}")
            check(t.iat_rva == 0x1150, f"iat rva: {t.iat_rva:#x}")

    # base relocations
    check(len(pe.relocs) == 2, f"relocs: {len(pe.relocs)}")
    if len(pe.relocs) == 2:
        check(pe.relocs[0].page_rva == 0x1000, f"reloc0 page: {pe.relocs[0].page_rva:#x}")
        check(pe.relocs[0].type_ == 3, f"reloc0 type: {pe.relocs[0].type_}")  # HIGHLOW
        check(pe.relocs[0].offset == 0x000, f"reloc0 offset: {pe.relocs[0].offset:#x}")
        check(pe.relocs[1].offset == 0x050, f"reloc1 offset: {pe.relocs[1].offset:#x}")

    # rva_to_offset sanity
    check(pe.rva_to_offset(0x1000) == 0x200, f"rva 0x1000 -> {pe.rva_to_offset(0x1000):#x}")
    check(pe.rva_to_offset(0x1100) == 0x300, f"rva 0x1100 -> {pe.rva_to_offset(0x1100):#x}")
    check(pe.rva_to_offset(0x1200) == 0x400, f"rva 0x1200 -> {pe.rva_to_offset(0x1200):#x}")

    print("\n--- PE summary ---")
    print(pe.summary())
    print()

    if failures:
        print(f"FAIL — {len(failures)} assertion(s):")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print(f"PASS — all {24} assertions hold. PE parser scaffold verified on the fixture.")


if __name__ == "__main__":
    main()
