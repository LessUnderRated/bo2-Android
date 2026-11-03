"""unwind_frontier.py — list the guest return addresses a longjmp unwind can
land on and that HAVE NO LIFTED BLOCK, i.e. every place a guest coroutine can
still die the way tid=3 died on 2026-08-01.

WHY THIS EXISTS. A guest longjmp destroys the host frames of the whole guest
call tree, so thread_trampoline re-dispatches at the setjmp resume PC and then
climbs the guest return chain one `ret` at a time (kernel32_impl.cpp). Each hop
needs a lifted block AT THE RETURN ADDRESS. One missing block ends the thread —
and if that thread was the only signaller of some event, whatever waits on that
event waits forever. That is exactly the post-video black screen: 0x8fb713, the
return address of `call ecx` at 0x8fb711, was the only gap on the path and the
frontend never became ready.

WHAT COUNTS AS A RETURN ADDRESS. The instruction after any `call`. Remill lifts
the fall-through of a DIRECT call inside the calling function most of the time,
so those rarely need a separate entry; the fall-through of an INDIRECT call
(`call reg`, `call [mem]`) is where the gaps cluster — remill cannot follow the
target, so the continuation becomes its own trace, and whether it gets one is
not guaranteed. Both classes are reported, separately, because guessing which
one matters is how this project has wasted days before.

HOW THE CODE IS FOUND. Linear decode from every VA already registered as a
lifted block, stopping at ret/jmp/int3/invalid. That is far more trustworthy
than sweeping .text end to end, which desynchronises on the first jump table
and then reports imaginary instructions as fact.

    python3 tools/unwind_frontier.py                 # summary + first entries
    python3 tools/unwind_frontier.py --out list.txt  # full list, lift_extra format
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import capstone  # noqa: E402
from pe_parser import PEFile  # noqa: E402

MAIN_CPP = os.path.join(ROOT, "boot_cache", "main.cpp")
EXTRA_INC = os.path.join(ROOT, "runtime", "win32", "extra_lifted_vas.inc")


def registered_vas():
    """Every VA the shim can dispatch to: boot.py's generated registrations plus
    the out-of-band ones from lift_extra.py."""
    vas = set()
    with open(MAIN_CPP) as f:
        for m in re.finditer(r"register_jump_target\(0x([0-9a-f]+)", f.read()):
            vas.add(int(m.group(1), 16))
    if os.path.exists(EXTRA_INC):
        with open(EXTRA_INC) as f:
            for m in re.finditer(r"BO2_EXTRA_LIFTED\(([0-9a-f]+)\)", f.read()):
                vas.add(int(m.group(1), 16))
    return vas


def scan(pe, starts, max_bytes=0x4000):
    """From each start, decode linearly to the first terminator. Returns
    {return_va: (call_va, is_indirect)} for every call encountered."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = False
    text = [s for s in pe.sections if s.is_executable]
    if not text:
        raise SystemExit("no executable section")

    def read(va, n):
        for s in text:
            lo = pe.image_base + s.virtual_address
            hi = lo + min(s.virtual_size, s.size_of_raw_data)
            if lo <= va < hi:
                off = s.pointer_to_raw_data + (va - lo)
                return pe._raw[off:off + min(n, hi - va)]
        return b""

    STOP = {"ret", "retf", "jmp", "int3", "ud2", "hlt"}
    out = {}
    seen = set()
    for start in starts:
        if start in seen:
            continue
        buf = read(start, max_bytes)
        if not buf:
            continue
        for ins in md.disasm(buf, start):
            seen.add(ins.address)
            if ins.mnemonic == "call":
                op = ins.op_str
                # `call 0x1234` is direct; `call eax` / `call dword ptr [..]`
                # is not. Capstone renders direct targets as a bare hex literal.
                indirect = not re.fullmatch(r"0x[0-9a-f]+", op)
                out[ins.address + ins.size] = (ins.address, indirect)
            elif ins.mnemonic in STOP:
                break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="write the missing indirect-call return sites here")
    ap.add_argument("--all", action="store_true",
                    help="include direct-call return sites in --out too")
    a = ap.parse_args()

    import boot
    pe = PEFile(boot.BIN).parse()
    reg = registered_vas()
    print(f"registered lifted blocks: {len(reg)}")
    sites = scan(pe, sorted(reg))
    ind = {va for va, (_, i) in sites.items() if i}
    dir_ = {va for va, (_, i) in sites.items() if not i}
    print(f"call return sites reached by linear decode: {len(sites)} "
          f"({len(ind)} indirect, {len(dir_)} direct)")
    miss_i = sorted(ind - reg)
    miss_d = sorted(dir_ - reg)
    print(f"MISSING a lifted block: {len(miss_i)} indirect, {len(miss_d)} direct")
    print("  first indirect gaps:", " ".join(f"0x{v:x}" for v in miss_i[:20]))
    if 0x8fb713 in miss_i:
        print("  (0x8fb713 is in this list — the one that killed the loader thread)")
    elif 0x8fb713 in miss_d:
        print("  (0x8fb713 is in the DIRECT list)")
    if a.out:
        chosen = miss_i + (miss_d if a.all else [])
        with open(a.out, "w") as f:
            f.write("# GENERATED by tools/unwind_frontier.py — return addresses a\n"
                    "# longjmp unwind can land on with no lifted block. Feed to\n"
                    "# tools/lift_extra.py --list.\n")
            for v in sorted(chosen):
                f.write(f"{v:x}\n")
        print(f"wrote {a.out} ({len(chosen)} VAs)")


if __name__ == "__main__":
    main()
