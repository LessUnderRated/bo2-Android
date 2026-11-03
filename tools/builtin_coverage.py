"""builtin_coverage — which GSC builtins/methods the lift does NOT cover.

A script builtin is reached ONLY through the VM's dispatch table, so boot.py's
static call-graph never sees it: an unlifted builtin is invisible until a level
calls it, and then `__remill_function_call` prints one
"[shim] function_call: unknown target 0x…" line, returns 0, and the script gets
a wrong value instead of a result. That is how `absangleclamp360` (0x7d1800)
was called 2,199 times across the w22-w25 runs while every summary said
"missing=" (the summary counted only "[MISSING] block at").

The tables are arrays of 0x18-byte records {func, 0, name, id, min_args,
max_args} in .rdata. This walks .rdata for that shape and reports every entry
whose function has no _sub_<va> symbol in boot_cache/boot.

    python3 tools/builtin_coverage.py            # print the uncovered ones
    python3 tools/builtin_coverage.py --all      # every entry found
"""
import os, subprocess, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_parser import PEFile

BIN = os.environ.get("BO2_BIN",
    "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe")
BOOT = os.environ.get("BO2_BOOT", "boot_cache/boot")


def entries():
    pe = PEFile(BIN).parse()
    data = open(BIN, "rb").read()
    ib = pe.image_base
    def rd(o): return struct.unpack("<I", data[o:o + 4])[0]
    def name_at(va):
        try:
            o = pe.rva_to_offset(va - ib)
            e = data.index(b"\0", o, o + 80)
            s = data[o:e]
            if 0 < len(s) < 64 and all(97 <= c <= 122 or 48 <= c <= 57 or c == 95 for c in s):
                return s.decode()
        except Exception:
            pass
        return None
    sec = [s for s in pe.sections if s.name.startswith(".rdata")][0]
    lo, hi = sec.pointer_to_raw_data, sec.pointer_to_raw_data + sec.size_of_raw_data
    seen, out = set(), []
    o = lo
    # 🔴 CORRIGE 2026-09-17. Le format REEL d'un enregistrement est
    #     {name, id, min_args, max_args, func, 0}
    # et non {func, 0, name, id, min, max}. Les deux "collent" a la meme
    # fenetre de 24 octets decalee de 8, et l'ancienne lecture appariait donc
    # chaque NOM avec la fonction de l'entree PRECEDENTE.
    # PREUVE (et non raisonnement) : les six accesseurs de Scr_GetMethod
    # (0x5f9a40, 0x4acb80, 0x46ca00, 0x45a540, 0x4c0860, 0x4fec80) cherchent
    # le nom a `base + 24*i` et renvoient la fonction a `base + 24*i + 0x10`.
    # Et sur les six tables, l'ancienne lecture donne exactement UNE fonction
    # invalide (la premiere entree : `attachpath` recevait 0x40490fdb, pi en
    # flottant) la ou la nouvelle n'en donne AUCUNE.
    while o + 0x18 <= hi:
        n = rd(o)
        f = rd(o + 0x10)
        if (0x401000 <= f < 0xa32000 and rd(o + 0x14) == 0 and
                0xa3b000 <= n < 0xbc7000):
            mn, mx = rd(o + 8), rd(o + 0xc)
            if mn <= mx <= 64:
                nm = name_at(n)
                if nm and f not in seen:
                    seen.add(f)
                    out.append((f, nm, rd(o + 4), mn, mx))
        o += 4
    return out


def lifted():
    nm = subprocess.run(["nm", BOOT], capture_output=True, text=True).stdout
    return {ln.split("_sub_")[1] for ln in nm.splitlines() if " T _sub_" in ln}


def main():
    all_ = "--all" in sys.argv
    have, tbl = lifted(), entries()
    miss = [e for e in tbl if "%x" % e[0] not in have]
    for f, nm, i, mn, mx in (tbl if all_ else miss):
        print("0x%06x %-34s id=%-5d args=%d-%d%s" % (f, nm, i, mn, mx,
              "" if all_ else "   NOT LIFTED"))
    print("%d builtins/methods, %d uncovered" % (len(tbl), len(miss)))
    return 1 if miss else 0


if __name__ == "__main__":
    sys.exit(main())
