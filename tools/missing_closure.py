"""missing_closure — from a set of entry VAs, list every directly-reachable
function that is NOT already in the linked binary.

WHY. A [STUB-CALL] abort names exactly one unlifted function. Fixing it, then
rebuilding (~2 min) and re-running (~7 min) to discover the next one, is a
serial walk down a call chain that can be dozens deep. This walks it statically
in seconds, so one lift closes the whole frontier.

    python3 tools/missing_closure.py 0x62f550
    python3 tools/missing_closure.py --have boot_cache/boot 0x62f550 0x4eb560

"Directly reachable" means `call rel32` / `jmp rel32` followed transitively.
Indirect dispatch (vtables, the asset-type table at 0xbd4898) is NOT visible
here — that class surfaces at runtime as "unknown target" and is what
lift_extra.py's --list is for. So an empty result means "no DIRECT caller is
missing", not "nothing is missing".
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from pe_parser import PEFile

BIN = os.environ.get(
    "BO2_BIN", "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe")
MAX_BYTES = 0x4000


def have_symbols(binpath):
    out = subprocess.run(["nm", binpath], capture_output=True, text=True).stdout
    have = set()
    for line in out.splitlines():
        tok = line.split()
        if tok and tok[-1].startswith("_sub_"):
            try:
                have.add(int(tok[-1][5:], 16))
            except ValueError:
                pass
    return have


def walk(pe, md, va, seen_bytes):
    """Linear sweep from va to its ret/unconditional-jmp terminator; return the
    set of direct call/jmp targets encountered."""
    try:
        off = pe.rva_to_offset(va - pe.image_base)
    except Exception:
        return set(), False
    code = pe._raw[off:off + MAX_BYTES]
    targets = set()
    n = 0
    for ins in md.disasm(code, va):
        n += 1
        m = ins.mnemonic
        if m == "call" and ins.op_str.startswith("0x"):
            targets.add(int(ins.op_str, 16))
        elif m == "jmp" and ins.op_str.startswith("0x"):
            t = int(ins.op_str, 16)
            # a jmp far outside the body is a tail call, i.e. a real callee
            if t < va or t > va + MAX_BYTES:
                targets.add(t)
            break
        elif m == "ret":
            break
        if n > 3000:
            break
    return targets, n > 0


def main():
    a = sys.argv[1:]
    binpath = "boot_cache/boot"
    if a and a[0] == "--have":
        binpath = a[1]
        a = a[2:]
    if not a:
        print(__doc__)
        return 2
    have = have_symbols(binpath)
    print(f"{len(have)} sub_* symbols already in {binpath}")
    pe = PEFile(BIN).parse()
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    roots = [int(x, 16) for x in a]
    todo = list(roots)
    visited, missing, undecodable = set(), [], []
    while todo:
        va = todo.pop()
        if va in visited:
            continue
        visited.add(va)
        if va not in have and va not in roots:
            missing.append(va)
        targets, ok = walk(pe, md, va, None)
        if not ok:
            undecodable.append(va)
        for t in targets:
            if t not in visited:
                # Only descend through functions we would have to lift; a callee
                # already in the binary brings its own closure with it.
                if t not in have:
                    todo.append(t)
                else:
                    visited.add(t)
    for r in roots:
        if r not in have:
            missing.append(r)
    missing = sorted(set(missing))
    print(f"visited {len(visited)}, MISSING {len(missing)}")
    for va in missing:
        print(f"0x{va:x}")
    if undecodable:
        print(f"# {len(undecodable)} undecodable (not mapped / not code): "
              + " ".join(f"0x{v:x}" for v in undecodable[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
