"""
pe_parser — PE32/PE32+ parser for the recomp ingester.

Extracts: DOS/PE/COFF headers, optional header, data directories, sections,
import directory, export directory, base relocation directory, and the entry
point. Provides RVA->file-offset resolution.

This is the scaffold for the ingest step in docs/ARCHITECTURE.md. It is
self-tested against a hand-crafted minimal PE32 fixture (see
make_minimal_pe.py / test_pe_parser.py), so it runs on macOS with no Windows
toolchain. For the real T6 binary (SteamStub wrapper removed by you) this
same parser produces the function/import/reloc map the lifter consumes;
hardening with pefile or LLVM's object library can come later if edge cases
demand it.
"""
from __future__ import annotations
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

IMAGE_DOS_SIGNATURE = 0x5A4D      # "MZ"
IMAGE_NT_SIGNATURE = 0x00004550   # "PE\0\0"
IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_AMD64 = 0x8664

# Data directory indices
DIR_EXPORT = 0
DIR_IMPORT = 1
DIR_RESOURCE = 2
DIR_EXCEPTION = 3
DIR_BASERELOC = 5
DIR_DEBUG = 6

# Section characteristics flags of interest
SEC_CODE = 0x00000020
SEC_EXEC = 0x20000000
SEC_READ = 0x40000000
SEC_WRITE = 0x80000000


@dataclass
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    pointer_to_raw_data: int
    size_of_raw_data: int
    characteristics: int

    @property
    def is_code(self) -> bool:
        return bool(self.characteristics & SEC_CODE)

    @property
    def is_executable(self) -> bool:
        return bool(self.characteristics & SEC_EXEC)

    def contains_rva(self, rva: int) -> bool:
        return self.virtual_address <= rva < self.virtual_address + max(self.virtual_size, self.size_of_raw_data)


@dataclass
class ImportThunk:
    name: Optional[str]   # None if ordinal-only
    ordinal: Optional[int]
    iat_rva: int          # RVA of the IAT slot


@dataclass
class ImportEntry:
    dll: str
    thunks: List[ImportThunk] = field(default_factory=list)


@dataclass
class ExportSymbol:
    name: str
    ordinal: int
    rva: int
    forwarder: Optional[str] = None


@dataclass
class RelocEntry:
    page_rva: int
    type_: int
    offset: int


@dataclass
class PEFile:
    path: str
    machine: int = 0
    is_pe32_plus: bool = False
    image_base: int = 0
    entry_point_rva: int = 0
    section_alignment: int = 0
    file_alignment: int = 0
    size_of_image: int = 0
    size_of_headers: int = 0
    characteristics: int = 0
    data_directories: List[Tuple[int, int]] = field(default_factory=list)  # (rva, size)
    sections: List[Section] = field(default_factory=list)
    imports: List[ImportEntry] = field(default_factory=list)
    exports: List[ExportSymbol] = field(default_factory=list)
    relocs: List[RelocEntry] = field(default_factory=list)
    _raw: bytes = b""

    # --- parsing ---
    def parse(self) -> "PEFile":
        with open(self.path, "rb") as f:
            self._raw = f.read()
        self._parse_headers()
        self._parse_sections()
        self._parse_imports()
        self._parse_exports()
        self._parse_relocs()
        return self

    def _u16(self, off): return struct.unpack_from("<H", self._raw, off)[0]
    def _u32(self, off): return struct.unpack_from("<I", self._raw, off)[0]

    def _parse_headers(self) -> None:
        if self._u16(0) != IMAGE_DOS_SIGNATURE:
            raise ValueError("not a PE: missing MZ")
        e_lfanew = self._u32(0x3C)
        if self._u32(e_lfanew) != IMAGE_NT_SIGNATURE:
            raise ValueError("not a PE: missing PE signature")
        coff = e_lfanew + 4
        self.machine = self._u16(coff)
        n_sections = self._u16(coff + 2)
        self.characteristics = self._u16(coff + 18)
        opt = coff + 20
        magic = self._u16(opt)
        self.is_pe32_plus = (magic == 0x20B)
        if magic not in (0x10B, 0x20B):
            raise ValueError(f"bad optional header magic {magic:#x}")
        self.entry_point_rva = self._u32(opt + 16)
        if self.is_pe32_plus:
            self.image_base = struct.unpack_from("<Q", self._raw, opt + 24)[0]
            ndir_off = opt + 108
        else:
            self.image_base = self._u32(opt + 28)
            ndir_off = opt + 92
        self.section_alignment = self._u32(opt + 32)
        self.file_alignment = self._u32(opt + 36)
        self.size_of_image = self._u32(opt + 56)
        self.size_of_headers = self._u32(opt + 60)
        n_rva_sizes = self._u32(ndir_off)
        dd_off = ndir_off + 4
        self.data_directories = []
        for i in range(n_rva_sizes):
            rva = self._u32(dd_off + i * 8)
            size = self._u32(dd_off + i * 8 + 4)
            self.data_directories.append((rva, size))
        self._n_sections = n_sections
        self._opt_off = opt
        # section headers follow the optional header
        opt_size = self._u16(coff + 16)
        self._sec_off = opt + opt_size

    def _parse_sections(self) -> None:
        for i in range(self._n_sections):
            off = self._sec_off + i * 40
            name = self._raw[off:off + 8].rstrip(b"\x00").decode("latin-1")
            vsize = self._u32(off + 8)
            vaddr = self._u32(off + 12)
            raw_size = self._u32(off + 16)
            raw_ptr = self._u32(off + 20)
            chars = self._u32(off + 36)
            self.sections.append(Section(name, vaddr, vsize, raw_ptr, raw_size, chars))

    # --- RVA <-> file offset ---
    def rva_to_offset(self, rva: int) -> int:
        for s in self.sections:
            if s.contains_rva(rva):
                return s.pointer_to_raw_data + (rva - s.virtual_address)
        # headers region
        if rva < (self.sections[0].virtual_address if self.sections else self.size_of_headers):
            return rva
        raise ValueError(f"RVA {rva:#x} not in any section")

    def _read_cstr(self, rva: int) -> str:
        off = self.rva_to_offset(rva)
        end = self._raw.index(b"\x00", off)
        return self._raw[off:end].decode("latin-1")

    def _dir(self, index: int) -> Tuple[int, int]:
        if index < len(self.data_directories):
            return self.data_directories[index]
        return (0, 0)

    # --- imports ---
    def _parse_imports(self) -> None:
        rva, size = self._dir(DIR_IMPORT)
        if rva == 0:
            return
        off = self.rva_to_offset(rva)
        while True:
            ilt_rva = self._u32(off)        # OriginalFirstThunk
            name_rva = self._u32(off + 12)
            iat_rva = self._u32(off + 16)   # FirstThunk
            if ilt_rva == 0 and name_rva == 0 and iat_rva == 0:
                break
            dll = self._read_cstr(name_rva)
            entry = ImportEntry(dll=dll)
            thunk_rva = ilt_rva if ilt_rva else iat_rva
            self._parse_thunks(thunk_rva, iat_rva, entry)
            self.imports.append(entry)
            off += 20

    def _parse_thunks(self, thunk_rva: int, iat_rva: int, entry: ImportEntry) -> None:
        thunk_off = self.rva_to_offset(thunk_rva)
        iat_off = self.rva_to_offset(iat_rva)
        width = 8 if self.is_pe32_plus else 4
        ord_flag = 0x8000000000000000 if self.is_pe32_plus else 0x80000000
        i = 0
        while True:
            if self.is_pe32_plus:
                val = struct.unpack_from("<Q", self._raw, thunk_off + i * width)[0]
            else:
                val = self._u32(thunk_off + i * width)
            if val == 0:
                break
            slot_rva = iat_rva + i * width
            if val & ord_flag:
                entry.thunks.append(ImportThunk(name=None, ordinal=val & 0xFFFF, iat_rva=slot_rva))
            else:
                name_rva = val & 0x7FFFFFFF
                hint = self._u16(self.rva_to_offset(name_rva))
                name = self._read_cstr(name_rva + 2)
                entry.thunks.append(ImportThunk(name=name, ordinal=hint, iat_rva=slot_rva))
            i += 1

    # --- exports ---
    def _parse_exports(self) -> None:
        rva, size = self._dir(DIR_EXPORT)
        if rva == 0:
            return
        off = self.rva_to_offset(rva)
        name_rva = self._u32(off + 12)
        ordinal_base = self._u32(off + 16)
        n_funcs = self._u32(off + 20)
        n_names = self._u32(off + 24)
        funcs_rva = self._u32(off + 28)
        names_rva = self._u32(off + 32)
        ordinals_rva = self._u32(off + 36)
        func_off = self.rva_to_offset(funcs_rva)
        names_off = self.rva_to_offset(names_rva) if n_names else 0
        ord_off = self.rva_to_offset(ordinals_rva) if n_names else 0
        name_to_ordinal: Dict[str, int] = {}
        for i in range(n_names):
            nr = self._u32(names_off + i * 4)
            nm = self._read_cstr(nr)
            o = self._u16(ord_off + i * 2)
            name_to_ordinal[nm] = o
        for i in range(n_funcs):
            frva = self._u32(func_off + i * 4)
            if frva == 0:
                continue
            ordinal = ordinal_base + i
            sym = ExportSymbol(name="", ordinal=ordinal, rva=frva)
            for nm, o in name_to_ordinal.items():
                if o == i:
                    sym.name = nm
            # forwarder?
            exp_dir_rva, exp_dir_size = self._dir(DIR_EXPORT)
            if exp_dir_rva <= frva < exp_dir_rva + exp_dir_size:
                sym.forwarder = self._read_cstr(frva)
            self.exports.append(sym)

    # --- base relocations ---
    def _parse_relocs(self) -> None:
        rva, size = self._dir(DIR_BASERELOC)
        if rva == 0:
            return
        off = self.rva_to_offset(rva)
        end = off + size
        while off + 8 <= end:
            page_rva = self._u32(off)
            block_size = self._u32(off + 4)
            if block_size < 8:
                break
            n = (block_size - 8) // 2
            for i in range(n):
                e = self._u16(off + 8 + i * 2)
                t = (e >> 12) & 0xF
                o = e & 0x0FFF
                self.relocs.append(RelocEntry(page_rva=page_rva, type_=t, offset=o))
            off += block_size

    # --- reporting ---
    def summary(self) -> str:
        lines = []
        lines.append(f"machine: {'x86' if self.machine==IMAGE_FILE_MACHINE_I386 else 'amd64' if self.machine==IMAGE_FILE_MACHINE_AMD64 else hex(self.machine)} ({'PE32+' if self.is_pe32_plus else 'PE32'})")
        lines.append(f"image_base: {self.image_base:#x}  entry: {self.entry_point_rva:#x} (VA {self.image_base + self.entry_point_rva:#x})")
        lines.append(f"section_alignment: {self.section_alignment:#x}  file_alignment: {self.file_alignment:#x}  size_of_image: {self.size_of_image:#x}")
        lines.append(f"sections ({len(self.sections)}):")
        for s in self.sections:
            flags = ("X" if s.is_executable else "-") + ("R" if s.characteristics & SEC_READ else "-") + ("W" if s.characteristics & SEC_WRITE else "-")
            lines.append(f"  {s.name:10s} va={s.virtual_address:#010x} vsize={s.virtual_size:#x} raw={s.pointer_to_raw_data:#x}/{s.size_of_raw_data:#x} [{flags}] {'CODE' if s.is_code else ''}")
        lines.append(f"imports ({len(self.imports)} DLLs):")
        for imp in self.imports:
            fns = ", ".join(t.name or f"#{t.ordinal}" for t in imp.thunks)
            lines.append(f"  {imp.dll}: {fns}")
        lines.append(f"exports: {len(self.exports)}  relocs: {len(self.relocs)}")
        return "\n".join(lines)
