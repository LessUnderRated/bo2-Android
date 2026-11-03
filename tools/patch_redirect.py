#!/usr/bin/env python3
"""Post-link symbol redirection for the boot binary.

Lifted->lifted calls compile to a DIRECT `bl _sub_<va>` resolved at link time, so
a strong definition inside lifted.o can never be overridden by another object.
Two cases need overriding anyway:

  * the guest's MSVC longjmp (0x965790), which must run our bo2_longjmp shim;
  * any function boot.py emitted as a __stub_call_fatal STUB (it failed to lift,
    or was outside the lift set) that we have since lifted for real.

Both are handled the same way: overwrite the first instructions of the stale
symbol with ADRP/ADD/BR x16 so every existing `bl` lands on the replacement.
ADRP+ADD reaches +/-4 GB and is PC-relative (ASLR-safe); a single B is too short
because lifted.o's text is hundreds of MB wide. x16 is the intra-procedure
scratch register, safe to clobber at a function entry.

Usage:
    patch_redirect.py <boot-binary> [src_sym=dst_sym ...]

With no pairs it applies the default longjmp redirect. Whatever this writes, the
caller MUST re-sign the binary (`codesign -f -s - <boot>`): patching invalidates
the linker's ad-hoc signature and the arm64 kernel then kills the process on a
page fault of the modified page — a silent early exit that looks nothing like a
signing problem.
"""
import sys, struct, subprocess

DEFAULT_PAIRS = [("_sub_965790", "_bo2_longjmp")]


def symbol_addrs(boot, names):
    """vmaddr of each requested symbol, via nm."""
    want, addrs = set(names), {}
    out = subprocess.run(["nm", boot], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[2] in want:
            addrs[p[2]] = int(p[0], 16)
    return addrs


def text_mapping(data):
    """(vmaddr, file offset) of __TEXT,__text so a vmaddr can be written to."""
    magic = struct.unpack_from("<I", data, 0)[0]
    assert magic == 0xFEEDFACF, f"not a 64-bit Mach-O: {magic:#x}"
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off = 32  # sizeof(mach_header_64)
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            segname = data[off + 8:off + 24].rstrip(b"\x00")
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            so = off + 72
            for _s in range(nsects):
                sect = data[so:so + 16].rstrip(b"\x00")
                addr = struct.unpack_from("<Q", data, so + 32)[0]
                offset = struct.unpack_from("<I", data, so + 48)[0]
                if segname == b"__TEXT" and sect == b"__text":
                    return addr, offset
                so += 80
        off += cmdsize
    raise AssertionError("no __TEXT,__text segment")


def branch_thunk(src, dst):
    """ADRP x16, page(dst); ADD x16, x16, off(dst); BR x16 — 12 bytes."""
    imm21 = ((dst & ~0xFFF) - (src & ~0xFFF)) >> 12
    adrp = 0x90000000 | ((imm21 & 3) << 29) | (((imm21 >> 2) & 0x7FFFF) << 5) | 16
    add = 0x91000000 | ((dst & 0xFFF) << 10) | (16 << 5) | 16
    br = 0xD61F0000 | (16 << 5)
    return (adrp, add, br)


def main(argv):
    boot = argv[1]
    pairs = [tuple(a.split("=", 1)) for a in argv[2:]] or DEFAULT_PAIRS

    addrs = symbol_addrs(boot, [s for pair in pairs for s in pair])
    # Skip pairs whose symbols are absent rather than failing the whole batch:
    # a bulk re-lift set legitimately contains functions that are not in this
    # link (already interposed, or never referenced).
    skipped = [p for p in pairs if p[0] not in addrs or p[1] not in addrs]
    pairs = [p for p in pairs if p[0] in addrs and p[1] in addrs]
    if skipped:
        print(f"patch_redirect: skipping {len(skipped)} pair(s) with missing symbols"
              f"{': ' + ', '.join('='.join(p) for p in skipped[:5]) if len(skipped) <= 5 else ''}")
    if not pairs:
        print("patch_redirect: nothing to patch")
        return 1

    data = bytearray(open(boot, "rb").read())
    text_vm, text_fo = text_mapping(data)

    for src_sym, dst_sym in pairs:
        src, dst = addrs[src_sym], addrs[dst_sym]
        fo = text_fo + (src - text_vm)
        old = struct.unpack_from("<I", data, fo)[0]
        for i, ins in enumerate(branch_thunk(src, dst)):
            struct.pack_into("<I", data, fo + i * 4, ins)
        print(f"patched {src_sym} @0x{src:x} (fo 0x{fo:x}): {old:#010x}.. -> "
              f"ADRP/ADD/BR x16 -> {dst_sym} (0x{dst:x})")

    open(boot, "wb").write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
