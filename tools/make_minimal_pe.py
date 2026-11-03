"""
make_minimal_pe — build a hand-crafted minimal PE32 fixture for testing
pe_parser without a Windows toolchain.

The fixture is a valid PE32 (x86-32) console exe with:
  - 1 section (.text, RX, CODE)
  - an entry point (a single `ret`)
  - an import directory: 1 DLL (testdll.dll) importing 1 named function
    (ImportedFunc)
  - a base relocation directory: 1 block, 2 HIGHLOW entries

It is deliberately tiny and hand-laid-out so the parser test can assert every
field deterministically. See test_pe_parser.py.
"""
import os
import struct
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "minimal_fixture.exe")

# --- layout constants (see design notes above) ---
FILE_SIZE = 0x600
HEADERS_RAW = 0x200          # headers occupy file 0x000..0x1FF
TEXT_RAW_OFF = 0x200         # .text raw data starts here
TEXT_RAW_SIZE = 0x400        # covers content up to reloc block at 0x400..0x40C
TEXT_VADDR = 0x1000          # .text RVA
TEXT_VSIZE = 0x1000
IMAGE_BASE = 0x00400000
ENTRY_RVA = 0x1000
SECTION_ALIGN = 0x1000
FILE_ALIGN = 0x200
SIZE_OF_IMAGE = 0x2000
SIZE_OF_HEADERS = 0x200
E_LFANEW = 0x80

# RVAs of directory content (within .text virtual range)
IMPORT_DIR_RVA = 0x1100
IMPORT_DIR_SIZE = 0x28       # 2 descriptors x 20
ILT_RVA = 0x1140             # Import Lookup Table
IAT_RVA = 0x1150             # Import Address Table
DLLNAME_RVA = 0x1180
IMPORT_BY_NAME_RVA = 0x1190
RELOC_DIR_RVA = 0x1200
RELOC_DIR_SIZE = 0x0C        # 8 header + 2 entries x 2


def rva_to_off(rva: int) -> int:
    return TEXT_RAW_OFF + (rva - TEXT_VADDR)


def build() -> bytes:
    buf = bytearray(FILE_SIZE)

    # DOS header
    struct.pack_into("<H", buf, 0x00, 0x5A4D)         # "MZ"
    struct.pack_into("<I", buf, 0x3C, E_LFANEW)        # e_lfanew

    # PE signature
    struct.pack_into("<I", buf, E_LFANEW, 0x00004550)  # "PE\0\0"

    # COFF header (20 bytes at 0x84)
    coff = E_LFANEW + 4
    struct.pack_into("<H", buf, coff + 0, 0x014C)      # Machine = I386
    struct.pack_into("<H", buf, coff + 2, 1)           # NumberOfSections
    struct.pack_into("<I", buf, coff + 4, 0)           # TimeDateStamp
    struct.pack_into("<I", buf, coff + 8, 0)           # PointerToSymbolTable
    struct.pack_into("<I", buf, coff + 12, 0)          # NumberOfSymbols
    struct.pack_into("<H", buf, coff + 16, 0xE0)       # SizeOfOptionalHeader (PE32)
    struct.pack_into("<H", buf, coff + 18, 0x0102)     # Characteristics (EXEC|32BIT)

    # Optional header PE32 (0xE0 bytes at 0x98)
    opt = coff + 20
    struct.pack_into("<H", buf, opt + 0, 0x10B)        # Magic = PE32
    struct.pack_into("<I", buf, opt + 4, TEXT_RAW_SIZE)   # SizeOfCode
    struct.pack_into("<I", buf, opt + 8, 0x200)        # SizeOfInitializedData
    struct.pack_into("<I", buf, opt + 12, 0)           # SizeOfUninitializedData
    struct.pack_into("<I", buf, opt + 16, ENTRY_RVA)   # AddressOfEntryPoint
    struct.pack_into("<I", buf, opt + 20, TEXT_VADDR)  # BaseOfCode
    struct.pack_into("<I", buf, opt + 24, TEXT_VADDR)  # BaseOfData
    struct.pack_into("<I", buf, opt + 28, IMAGE_BASE)  # ImageBase
    struct.pack_into("<I", buf, opt + 32, SECTION_ALIGN)
    struct.pack_into("<I", buf, opt + 36, FILE_ALIGN)
    struct.pack_into("<H", buf, opt + 40, 4)           # MajorOSVersion
    struct.pack_into("<H", buf, opt + 48, 4)           # MajorSubsystemVersion
    struct.pack_into("<I", buf, opt + 56, SIZE_OF_IMAGE)
    struct.pack_into("<I", buf, opt + 60, SIZE_OF_HEADERS)
    struct.pack_into("<H", buf, opt + 68, 3)           # Subsystem = CONSOLE
    struct.pack_into("<I", buf, opt + 72, 0x100000)    # SizeOfStackReserve
    struct.pack_into("<I", buf, opt + 76, 0x1000)      # SizeOfStackCommit
    struct.pack_into("<I", buf, opt + 80, 0x100000)    # SizeOfHeapReserve
    struct.pack_into("<I", buf, opt + 84, 0x1000)      # SizeOfHeapCommit
    struct.pack_into("<I", buf, opt + 92, 16)          # NumberOfRvaAndSizes

    # Data directories (16 x 8 bytes at opt+96)
    dd = opt + 96
    # [1] Import
    struct.pack_into("<II", buf, dd + 1 * 8, IMPORT_DIR_RVA, IMPORT_DIR_SIZE)
    # [5] BaseReloc
    struct.pack_into("<II", buf, dd + 5 * 8, RELOC_DIR_RVA, RELOC_DIR_SIZE)

    # Section header (.text, 40 bytes at 0x98 + 0xE0 = 0x178)
    sec = opt + 0xE0
    buf[sec:sec + 8] = b".text\x00\x00\x00"
    struct.pack_into("<I", buf, sec + 8, TEXT_VSIZE)        # VirtualSize
    struct.pack_into("<I", buf, sec + 12, TEXT_VADDR)       # VirtualAddress
    struct.pack_into("<I", buf, sec + 16, TEXT_RAW_SIZE)    # SizeOfRawData
    struct.pack_into("<I", buf, sec + 20, TEXT_RAW_OFF)     # PointerToRawData
    struct.pack_into("<I", buf, sec + 36, 0x60000020)       # CODE|EXEC|READ

    # --- .text content (file offsets) ---
    # entry point: a single ret
    buf[rva_to_off(ENTRY_RVA)] = 0xC3

    # import descriptor #1 (20 bytes) + terminator (20 zeros)
    imp_off = rva_to_off(IMPORT_DIR_RVA)
    struct.pack_into("<I", buf, imp_off + 0, ILT_RVA)       # OriginalFirstThunk
    struct.pack_into("<I", buf, imp_off + 4, 0)             # TimeDateStamp
    struct.pack_into("<I", buf, imp_off + 8, 0)             # ForwarderChain
    struct.pack_into("<I", buf, imp_off + 12, DLLNAME_RVA)  # Name
    struct.pack_into("<I", buf, imp_off + 16, IAT_RVA)      # FirstThunk
    # terminator is already zeros

    # ILT: [RVA to import-by-name, 0]
    ilt_off = rva_to_off(ILT_RVA)
    struct.pack_into("<I", buf, ilt_off + 0, IMPORT_BY_NAME_RVA)
    struct.pack_into("<I", buf, ilt_off + 4, 0)

    # IAT: same initial content as ILT
    iat_off = rva_to_off(IAT_RVA)
    struct.pack_into("<I", buf, iat_off + 0, IMPORT_BY_NAME_RVA)
    struct.pack_into("<I", buf, iat_off + 4, 0)

    # DLL name
    dllname = b"testdll.dll\x00"
    buf[rva_to_off(DLLNAME_RVA):rva_to_off(DLLNAME_RVA) + len(dllname)] = dllname

    # Import by name: hint(2) + name + nul
    ibn_off = rva_to_off(IMPORT_BY_NAME_RVA)
    struct.pack_into("<H", buf, ibn_off + 0, 0)             # hint
    name = b"ImportedFunc\x00"
    buf[ibn_off + 2:ibn_off + 2 + len(name)] = name

    # Base reloc block: page=0x1000, size=0xC, two HIGHLOW entries
    rel_off = rva_to_off(RELOC_DIR_RVA)
    struct.pack_into("<I", buf, rel_off + 0, 0x1000)        # VirtualAddress (page)
    struct.pack_into("<I", buf, rel_off + 4, RELOC_DIR_SIZE)  # SizeOfBlock
    struct.pack_into("<H", buf, rel_off + 8, (3 << 12) | 0x000)  # HIGHLOW, offset 0
    struct.pack_into("<H", buf, rel_off + 10, (3 << 12) | 0x050)  # HIGHLOW, offset 0x50

    return bytes(buf)


def main():
    data = build()
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"wrote {OUT} ({len(data)} bytes)")
    print(f"  entry VA: {IMAGE_BASE + ENTRY_RVA:#010x}")
    print(f"  1 import DLL (testdll.dll) -> ImportedFunc")
    print(f"  2 base relocs (HIGHLOW)")


if __name__ == "__main__":
    main()
