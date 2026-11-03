"""find_stubs — list which guest VAs in the linked binary are __stub_call_fatal
STUBS rather than real lifted bodies.

WHY THIS EXISTS. `nm boot_cache/boot | grep -c _sub_62f550` returns 1 for a stub
and 1 for a real function; this repo has burned a session on reading that count
as "it is lifted". The runtime only tells you a VA is a stub when the guest
CALLS it — one abort per ~7-minute run, walking a call chain one link at a time.

The stub body is exactly identifiable, no disassembler needed. boot.py emits

    movz w0, #(va & 0xffff)
    movk w0, #(va >> 16), lsl #16
    bl   __stub_call_fatal

and AArch64 instructions are fixed 32-bit, so the two constants are two exact
words to look for in the symbol's first bytes:

    movz w0,#imm16          = 0x52800000 | (imm16 << 5)
    movk w0,#imm16, lsl 16  = 0x72A00000 | (imm16 << 5)

Usage:
    python3 tools/find_stubs.py                 # every stub in the binary
    python3 tools/find_stubs.py 0x62f550 0x4eb560   # ask about specific VAs
"""
import os, re, struct, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BOOT = os.environ.get("BO2_BOOT", os.path.join(ROOT, "boot_cache", "boot"))
SCAN = 0x40          # a stub is ~0x28 bytes; scan a little past it


def text_map(path):
    """(vmaddr, fileoff) of __TEXT,__text."""
    out = subprocess.run(["otool", "-l", path], capture_output=True,
                         text=True).stdout
    m = re.search(r"sectname __text\s+segname __TEXT\s+addr (0x[0-9a-f]+)\s+"
                  r"size 0x[0-9a-f]+\s+offset (\d+)", out)
    if not m:
        raise SystemExit("could not locate __TEXT,__text")
    return int(m.group(1), 16), int(m.group(2))


def symbols(path):
    out = subprocess.run(["nm", path], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        t = line.split()
        if len(t) >= 3 and t[-1].startswith("_sub_"):
            try:
                syms[int(t[-1][5:], 16)] = int(t[0], 16)
            except ValueError:
                pass
    return syms


def load(path):
    with open(path, "rb") as f:
        return f.read()


def is_stub(blob, off, va):
    """The three instructions must be CONSECUTIVE: movz, movk, then a bl within
    the next two words.

    A looser version (both constants anywhere in the window, any order) returns
    the SAME 103 symbols on this binary, so nothing here rests on the tightening
    — it is kept because the loose form has no reason to stay exact as the
    binary grows. The 13 results outside .text are NOT false positives: they are
    real stubs for garbage VAs harvested from bad indirect-call targets
    (`_sub_14bf9304`), which is why callers must range-check before lifting."""
    movz = 0x52800000 | ((va & 0xFFFF) << 5)
    movk = 0x72A00000 | (((va >> 16) & 0xFFFF) << 5)
    window = blob[off:off + SCAN]
    if len(window) < SCAN:
        return False
    n = len(window) // 4
    words = struct.unpack("<%dI" % n, window[:n * 4])
    # ⚠️ AN ALREADY-REDIRECTED STUB IS NOT A LIVE STUB. patch_redirect overwrites
    # only the ENTRY (adrp x16 / add x16 / br x16), leaving the original
    # movz/movk/bl sitting a few words below — inside this window. Reporting
    # those as stubs made relift_stubs.sh lift them again and the link died with
    # `duplicate symbol '_sub_63a7b0_impl'`. A leading `br x16` means the entry
    # was patched and the body below it is unreachable.
    if 0xD61F0200 in words[:4]:
        return False
    for i in range(n - 3):
        if words[i] == movz and words[i + 1] == movk:
            for j in (i + 2, i + 3):
                if (words[j] & 0xFC000000) == 0x94000000:   # bl
                    return True
    return False


def main():
    vmaddr, fileoff = text_map(BOOT)
    syms = symbols(BOOT)
    blob = load(BOOT)
    want = [int(a, 16) for a in sys.argv[1:]]
    if want:
        for va in want:
            addr = syms.get(va)
            if addr is None:
                print(f"0x{va:x}  NO SYMBOL")
                continue
            off = addr - vmaddr + fileoff
            print(f"0x{va:x}  {'STUB' if is_stub(blob, off, va) else 'real'}")
        return 0
    stubs = [va for va, addr in syms.items()
             if is_stub(blob, addr - vmaddr + fileoff, va)]
    stubs.sort()
    print(f"# {len(stubs)} stubs of {len(syms)} sub_* symbols in {BOOT}")
    for va in stubs:
        print(f"0x{va:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
