"""unwind_candidates.py — turn the runtime's [unwind-cand] dump into a lift list.

When a guest coroutine dies in the unwind trampoline for want of a lifted block,
the runtime prints every .text-looking dword still on that thread's stack that
has no block:

    [unwind-cand] tid=3 esp=0x419d8330 unlifted .text dwords above it: 005cb748 ...

⚠️ THAT IS NOT A CALL CHAIN. A guest-stack scan yields plausible addresses, not
frames — §9 of the guide, learned the expensive way on 0x6d6265, which looked
like a frame and was residue from a function that pushes "fx_draw". This script
therefore does NOT treat the dump as a chain. It keeps only the addresses that
are ACTUALLY PRECEDED BY A CALL INSTRUCTION, which is the one property a return
address must have, and throws the rest away. A survivor may still be stale
residue from an older frame; lifting it costs a couple of seconds and nothing
else, whereas a missing one costs a six-minute boot.

Validation is by decode, not by pattern: for each candidate R, try every x86
call encoding length (2..7 bytes) ending exactly at R, decode the bytes at
R-len, and accept only if the result is a `call` of exactly that length. A
`0xe8` byte inside a displacement would pass a substring test and fails this.

    python3 tools/unwind_candidates.py run.log --out tools/unwind_extra_vas.txt
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import capstone  # noqa: E402
from pe_parser import PEFile  # noqa: E402


def registered_vas():
    vas = set()
    main_cpp = os.path.join(ROOT, "boot_cache", "main.cpp")
    with open(main_cpp) as f:
        for m in re.finditer(r"register_jump_target\(0x([0-9a-f]+)", f.read()):
            vas.add(int(m.group(1), 16))
    # 🔴 2026-09-18 : cette boucle ne lisait que DEUX .inc sur la quarantaine
    # que le depot compte, donc elle declarait « non enregistree » une VA que
    # trois vagues avaient deja liftee. Le glob les prend toutes, et une
    # enumeration qui trouve zero fichier est une ERREUR, pas un ensemble vide.
    incs = sorted(glob.glob(os.path.join(ROOT, "runtime", "win32",
                                         "extra_lifted_vas*.inc")))
    incs += [p for p in (os.path.join(ROOT, "runtime", "win32",
                                      "unwind_extra_vas.inc"),)
             if os.path.exists(p)]
    if not incs:
        sys.exit("unwind_candidates: aucune .inc trouvee — un filtre vide "
                 "declarerait TOUTES les VA absentes ; abandon.")
    for p in incs:
        with open(p) as f:
            for m in re.finditer(r"BO2_EXTRA_LIFTED\(([0-9a-f]+)\)", f.read()):
                vas.add(int(m.group(1), 16))
    # Et les SYMBOLES reellement presents dans les objets : une VA peut etre
    # liftee dans un objet qu'aucune .inc ne nomme (boot_cache/unwind_extra.o
    # en porte 864). C'est ce trou qui a produit « ld: 10 duplicate symbols ».
    for o in sorted(glob.glob(os.path.join(ROOT, "boot_cache", "*.o"))):
        nm = subprocess.run(["nm", "-U", o], capture_output=True, text=True).stdout
        for w in nm.split():
            if w.startswith("_sub_") and all(c in "0123456789abcdef" for c in w[5:]):
                vas.add(int(w[5:], 16))
    return vas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--out", help="merge the survivors into this lift list")
    a = ap.parse_args()

    import boot
    pe = PEFile(boot.BIN).parse()
    sec = [s for s in pe.sections if s.is_executable][0]
    base = pe.image_base + sec.virtual_address
    raw = pe._raw[sec.pointer_to_raw_data:
                  sec.pointer_to_raw_data + sec.size_of_raw_data]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    cands, hops = set(), set()
    for path in a.logs:
        with open(path, "rb") as f:
            text = f.read().decode("utf-8", "replace")
        for m in re.finditer(r"\[unwind-cand\][^\n]*dwords above it:([^\n]*)", text):
            for tok in m.group(1).split():
                cands.add(int(tok, 16))
        # the reported death address is a confirmed frame, not a candidate
        for m in re.finditer(r"NO LIFTED BLOCK at 0x([0-9a-f]{8})", text):
            hops.add(int(m.group(1), 16))
    print(f"candidates from the stack scan: {len(cands)}   confirmed deaths: {len(hops)}")

    def preceded_by_call(va):
        for ln in range(2, 8):
            start = va - ln
            off = start - base
            if off < 0 or off + ln > len(raw):
                continue
            ins = next(md.disasm(raw[off:off + ln], start), None)
            if ins and ins.mnemonic == "call" and ins.size == ln:
                return True
        return False

    good = sorted(v for v in cands if preceded_by_call(v))
    print(f"  survive the 'preceded by a call' check: {len(good)} "
          f"(dropped {len(cands) - len(good)} as residue)")
    reg = registered_vas()
    final = sorted((set(good) | hops) - reg)
    print(f"  not already registered: {len(final)}")
    print("  ", " ".join(f"{v:x}" for v in final[:40]))
    if a.out:
        existing = []
        header = []
        if os.path.exists(a.out):
            for line in open(a.out):
                (header if line.startswith("#") else existing).append(line.strip())
        have = {int(v, 16) for v in existing if v}
        added = [v for v in final if v not in have]
        with open(a.out, "w") as f:
            for h in header:
                f.write(h + "\n")
            for v in sorted(have | set(final)):
                f.write(f"{v:x}\n")
        print(f"wrote {a.out}: {len(have) + len(added)} VAs ({len(added)} new)")


if __name__ == "__main__":
    main()
