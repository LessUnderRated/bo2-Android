#!/usr/bin/env python3
"""Close a batch of lift holes on a Steam target in minutes, without a full relift and global fold.

Each VA is lifted alone (tools/lift_extra.py: linear sweep, ends on an instruction boundary), compiled -O2 with the
accessors inlined like the folded lifted.o, and joined to boot_cache/<target>/lifted.o by one `clang -r`. The VA goes
to bc_files.txt (dispatch registry) and tools/lift_roots_<target>.txt (next full relift), then the runtime is relinked.
Direct callees missing from lifted.o join the batch.

--thunks : retraduit les corps de lifted.o qui couvrent un vrai `call $+5` (E8 00000000 ; pop r32, au debut d'une
instruction) avec le site reecrit en `push imm32` (FLAC, 2026-10-21).

usage: tools/steam/fast_hole.py zms|mps|sps [--thunks] [--list FILE] [va ...]
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import time
from multiprocessing.pool import ThreadPool

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
L = "/opt/homebrew/opt/llvm@17/bin"
JOBS = 3
MAX_ROUNDS = 6
CHUNK = 64


def nm(path: str, *flags: str) -> set[str]:
    return set(subprocess.check_output([L + "/llvm-nm", *flags, "-j", path], text=True).split())


def thunks(W: str, lifted: str, defined: set[str], lift_extra) -> None:
    """Remplace dans lifted.o les corps qui couvrent un `call $+5` ; le registre de saut prend le nouveau au relink."""
    import boot
    import lift_roots
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs
    from pe_parser import PEFile

    pe = PEFile(boot.BIN).parse()
    image = pe._raw
    text = pe.sections[0]
    sites = [pe.image_base + text.virtual_address + m.start() for m in
             re.finditer(rb"\xe8\0\0\0\0[\x58-\x5f]",
                         image[text.pointer_to_raw_data:text.pointer_to_raw_data + text.size_of_raw_data])]
    corps = sorted(int(s[5:], 16) for s in defined if re.fullmatch(r"_sub_[0-9a-f]+", s))
    sweep = lift_roots._Closure(pe, {})
    cibles: set[int] = set()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    for site in sites:
        for va in (v for v in corps if site - 0x8000 < v <= site):
            taille = sweep._decode(va)[0]
            if va + taille <= site:
                continue
            if site in {i[0] for i in md.disasm_lite(image[pe.rva_to_offset(va - pe.image_base):][:taille], va)}:
                cibles.add(va)
    print(f"{len(sites)} sites, {len(cibles)} corps : {' '.join(hex(v) for v in sorted(cibles))}")
    lift_extra.WORKDIR = os.path.join(W, "thunk_work")
    os.makedirs(lift_extra.WORKDIR, exist_ok=True)
    faits = lift_extra.lift([f"{v:x}" for v in sorted(cibles)], patch_thunks=True)
    objs = []
    for va, bc in faits:
        o = os.path.join(W, f"extra_{va:x}.o")
        subprocess.run([L + "/clang", "-arch", "arm64", "-c", "-O2", "-x", "ir", "-o", o, bc], check=True,
                       capture_output=True)
        manque = {s for s in nm(o, "-u") if s.startswith("_sub_")} - defined
        if manque:
            sys.exit(f"0x{va:x} appelle des absents : {sorted(manque)[:8]}")
        objs.append(o)
    base = lifted + ".renomme"
    subprocess.run([L + "/llvm-objcopy"] + [f"--redefine-sym=_sub_{v:x}=_sub_{v:x}_vieux" for v, _ in faits]
                   + [lifted, base], check=True)
    subprocess.run([L + "/clang", "-arch", "arm64", "-r", "-nostdlib", "-o", lifted + ".new", base] + objs, check=True)
    os.remove(base)
    os.replace(lifted + ".new", lifted)
    for o in objs:
        os.remove(o)
    for va, bc in faits:
        subprocess.run(["cp", bc, os.path.join(W, os.path.basename(bc))], check=True)
    lift_extra.WORKDIR = W


def main() -> None:
    args = sys.argv[1:]
    if not args or args[0] not in ("zms", "mps", "sps"):
        sys.exit(__doc__)
    target = args.pop(0)
    vas: list[int] = []
    avec_thunks = False
    while args:
        a = args.pop(0)
        if a == "--thunks":
            avec_thunks = True
        elif a == "--list":
            vas += [int(ln.split()[0], 16) for ln in open(args.pop(0)) if ln.startswith("0x")]
        else:
            vas.append(int(a, 16))
    if subprocess.run(["pgrep", "-x", "boot"], capture_output=True).returncode == 0:
        sys.exit("un boot tourne")
    os.chdir(ROOT)
    W = os.path.join("boot_cache", target)
    os.environ.update(BO2_TARGET=target, BOOT_WORKDIR=W)
    for k in ("BO2_INLINE_READS", "BO2_INLINE_FLAGS", "BO2_INLINE_TRIV", "BO2_INLINE_WRITES"):  # lifted.o est replié
        os.environ[k] = "1"
    sys.path.insert(0, "tools")
    import lift_extra
    lift_extra.WORKDIR = W

    t0 = time.time()
    lifted = os.path.join(W, "lifted.o")
    defined = nm(lifted, "--defined-only")
    if avec_thunks:
        thunks(W, lifted, defined, lift_extra)
        defined = nm(lifted, "--defined-only")
    todo = sorted({v for v in vas if f"_sub_{v:x}" not in defined})
    print(f"{len(vas)} demandees, {len(todo)} absentes de lifted.o")
    done: dict[int, str] = {}
    objs: list[str] = []
    refus: list[int] = []

    def build(item: tuple[int, str]) -> tuple[int, str, set[str]] | None:
        va, bc = item
        o = os.path.join(W, f"extra_{va:x}.o")
        r = subprocess.run([L + "/clang", "-arch", "arm64", "-c", "-O2", "-x", "ir", "-o", o, bc],
                           capture_output=True, text=True)
        if r.returncode:
            print(f"  ECHEC clang 0x{va:x}: {r.stderr[:160]}")
            return None
        return va, o, {s for s in nm(o, "-u") if s.startswith("_sub_")}

    for manche in range(MAX_ROUNDS):
        if not todo:
            break
        lots = [[f"{v:x}" for v in todo[i:i + CHUNK]] for i in range(0, len(todo), CHUNK)]
        with ThreadPool(JOBS) as pool:
            lifted_now = [x for r in pool.map(lambda lot: lift_extra.lift(lot, patch_thunks=True), lots) for x in r]
            built = [b for b in pool.map(build, lifted_now) if b]
        refus += sorted(set(todo) - {b[0] for b in built})
        und: set[str] = set()
        for va, o, u in built:
            done[va] = os.path.join(W, f"func_{va:x}.bc.fixed")
            objs.append(o)
            und |= u
        have = defined | {f"_sub_{v:x}" for v in done}
        todo = sorted(int(s[5:], 16) for s in und - have)
        print(f"manche {manche}: {len(built)} objets, {len(todo)} appeles a ajouter, {time.time() - t0:.0f} s")
    if todo:
        sys.exit(f"appeles encore absents apres {MAX_ROUNDS} manches : {[hex(v) for v in todo[:8]]}")
    if refus:
        print(f"{len(refus)} refusees : {' '.join(hex(v) for v in refus[:20])}")
    if not objs and not avec_thunks:
        print("rien a joindre")
        return
    if objs:
        with open(os.path.join(W, "extra_objs.txt"), "w") as f:
            f.write("".join(o + "\n" for o in objs))
        subprocess.run([L + "/clang", "-arch", "arm64", "-r", "-nostdlib", "-o", lifted + ".new", lifted,
                        "-Wl,-filelist," + os.path.join(W, "extra_objs.txt")], check=True)
        os.replace(lifted + ".new", lifted)
        for o in objs:
            os.remove(o)
        with open(os.path.join(W, "bc_files.txt"), "a") as f:
            f.write("".join(p + "\n" for p in done.values()))
        roots_path = f"tools/lift_roots_{target}.txt"
        racines = set(open(roots_path).read().split())
        with open(roots_path, "a") as f:
            f.write("".join(f"0x{v:x}\n" for v in sorted(done) if f"0x{v:x}" not in racines))
        print(f"joint {len(objs)} objets, {time.time() - t0:.0f} s ; relink")

    env = dict(os.environ, BOOT_RELINK_ONLY="1", LIFT_ROOTS_FILE=roots_path,
               LIFT_RUNWAY_FILE=os.path.join(W, "runway_vide.txt"))
    for k in ("BO2_INLINE_READS", "BO2_INLINE_FLAGS", "BO2_INLINE_TRIV", "BO2_INLINE_WRITES"):
        env.pop(k)
    out = subprocess.run([sys.executable, "-u", "tools/boot.py", "-n", "200000", "-j", "3", "--build-only"],
                         env=env, capture_output=True, text=True)
    print(next((ln for ln in out.stdout.splitlines() if "READY" in ln), out.stdout[-600:] + out.stderr[-600:]))
    print(f"TOTAL {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
