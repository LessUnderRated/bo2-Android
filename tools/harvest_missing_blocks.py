"""harvest_missing_blocks.py — list every guest block address that some lifted
function gives up on with __remill_missing_block.

Why: remill's trace lifter sometimes declines to follow a DIRECT jmp and emits
`__remill_missing_block(state, <pc expr>, mem)` for the target instead of lifting
it. At runtime the shim's handler retries via the jump table, and if the block is
in no lifted function it prints "[MISSING] block at 0xVA not lifted" and aborts.
Registering that VA as its own lifted entry point is a complete fix
(tools/lift_extra.py), so the whole class can be closed statically instead of
one 5-minute rebuild per abort.

The target is read from remill's RAW output (func_<va>.bc, before `opt -O2`). There,
NEXT_PC lives in an alloca and every lifted instruction is a block that loads it, adds
its own length and stores it back, so a forward dataflow over the CFG gives each block
the exact set of addresses it can start at. Two details make it exact rather than an
over-approximation: a CALL saves the return address in the RETURN_PC alloca, and a jcc
stores `select(taken, target, fallthrough)` then branches on the same condition, so the
taken successor gets the select's first arm and the other successor the second.

The earlier resolver evaluated the -O2 IR (add/select/phi over constants) with a depth
bound of 8. Long chains through the loop phis of NEXT_PC silently returned {}: in the
multi, sub_615680 gives up on 0x6166d2 = pc + 4178 and the harvest never listed
it (run 2026-10-20 22:22, [MISSING] 0x6166d2). On that function the raw dataflow gives
exactly its 5 give-up targets, 0x6166d2 included.

Usage:
    python3 tools/harvest_missing_blocks.py [--workdir boot_cache/lift_wip]
                                            [--newer FILE] [--jobs 8]
                                            [--out FILE]

--newer restricts the scan to func_*.bc files newer than FILE, which is how you
re-harvest after a lift wave (new lifts introduce new give-up sites, so this is a
fixpoint: harvest, lift, build, harvest again until a run stops hitting
[MISSING]). Output is one VA per line, ready for lift_extra.py --list. Addresses
already registered by boot_cache/main.cpp are NOT filtered here — do that when
building the list, so this tool stays a pure "what does the IR say" report.
"""

import argparse
import multiprocessing
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

LLVM17 = os.environ.get("LLVM17", "/opt/homebrew/opt/llvm@17")
DIS = f"{LLVM17}/bin/llvm-dis"

CELLS = r"(NEXT_PC|PC|RETURN_PC)"
RE_BLK = re.compile(r"^(\d+|[A-Za-z_.][\w.]*):")
RE_LD = re.compile(r"^\s*(%[\w.]+) = load i32, ptr %" + CELLS + r"\b")
RE_ST = re.compile(r"^\s*store i32 (%[\w.]+|-?\d+), ptr %" + CELLS + r"\b")
RE_ADD = re.compile(r"^\s*(%[\w.]+) = (add|sub) (?:nuw |nsw )*i32 (%[\w.]+|-?\d+), (%[\w.]+|-?\d+)")
RE_SEL = re.compile(r"^\s*(%[\w.]+) = select i1 [^,]+, i32 (%[\w.]+|-?\d+), i32 (%[\w.]+|-?\d+)")
RE_MB = re.compile(r"@__remill_missing_block\(ptr [^,]+, i32 (%[\w.]+|-?\d+)")
RE_LABEL = re.compile(r"label %([\w.]+)")
RE_INT = re.compile(r"^-?\d+$")
# Extent CUT: remill reached a pc outside the byte buffer it was given and emitted INVALID_INSTRUCTION
# (`store i32 32, ptr %state` = kInvalidInstruction), then __remill_error(pc). The runtime resumes there when
# the pc is a lifted entry (win32_runtime.cpp, __remill_error), so it is harvested like a missing block
# (Steam campaign P961: 0x7ef456 in sub_7ef393).
RE_CUT = re.compile(r"(store i32 32, ptr %state, align \d+\n(?:  [^\n]*\n)*?  %[\w.]+ = tail call ptr @)__remill_error\(")
STEAM_CODE_END = {"sps": 0x9ef000, "mps": 0xb09000, "zms": 0xb07000}
MAX_VISITS = 200000
MAX_SET = 64            # a block can start at more addresses than this only by imprecision: stop propagating it


def _blocks(text):
    """Basic blocks of the first function in `text`, as {label: [lines]} plus their order."""
    blocks, order, cur = {"entry": []}, ["entry"], "entry"
    for ln in text[text.index("define "):].split("\n")[1:]:
        if ln.startswith("}"):
            break
        m = RE_BLK.match(ln)
        if m:
            cur = m.group(1)
            blocks[cur] = []
            order.append(cur)
        else:
            blocks[cur].append(ln)
    return blocks, order


def targets_in(text, va):
    """Guest VAs that the __remill_missing_block calls of a raw remill function can receive.
    Values are tracked as offsets from %program_counter (the function's VA)."""
    blocks, order = _blocks(text)
    val, arms, res = {}, {}, set()

    def get(tok):
        if tok == "%program_counter":
            return {0}
        if RE_INT.match(tok):
            return None                       # an absolute constant, not pc-relative
        return val.get(tok, set())

    inn = dict.fromkeys(order)
    inn["entry"] = {"NEXT_PC": set(), "PC": set(), "RETURN_PC": set()}
    work, visits = ["entry"], 0
    while work and visits < MAX_VISITS:
        visits += 1
        b = work.pop()
        env = {k: set(v) for k, v in inn[b].items()}
        succ, last_sel, cond = [], None, False
        for ln in blocks[b]:
            m = RE_LD.match(ln)
            if m:
                val.setdefault(m.group(1), set()).update(env[m.group(2)])
                continue
            m = RE_ST.match(ln)
            if m:
                env[m.group(2)] = set(get(m.group(1)) or ())
                if m.group(2) == "NEXT_PC":
                    last_sel = arms.get(m.group(1))
                continue
            m = RE_ADD.match(ln)
            if m:
                op, x, y = m.group(2), m.group(3), m.group(4)
                dst = val.setdefault(m.group(1), set())
                if RE_INT.match(y) and x.startswith("%"):
                    k = int(y) if op == "add" else -int(y)
                    dst.update(o + k for o in get(x))
                elif op == "add" and RE_INT.match(x) and y.startswith("%"):
                    dst.update(o + int(x) for o in get(y))
                continue
            m = RE_SEL.match(ln)
            if m:
                a, c = set(get(m.group(2)) or ()), set(get(m.group(3)) or ())
                arms[m.group(1)] = (a, c)
                val.setdefault(m.group(1), set()).update(a | c)
                continue
            m = RE_MB.search(ln)
            if m:
                tok = m.group(1)
                if RE_INT.match(tok):
                    res.add(int(tok) & 0xffffffff)
                else:
                    res.update((va + o) & 0xffffffff for o in get(tok))
            s = ln.lstrip()
            if s.startswith(("br ", "switch ")):
                succ += RE_LABEL.findall(ln)
                cond = s.startswith("br i1")
        for i, s in enumerate(succ):
            e = env
            if cond and last_sel and len(succ) == 2:
                # jcc: br on BRANCH_TAKEN == 1 -> first label gets the taken arm
                e = dict(env, NEXT_PC=set(last_sel[i]))
            if s not in inn:
                continue
            if inn[s] is None:
                inn[s] = {k: set(v) for k, v in e.items()}
                work.append(s)
            elif any(not e[k] <= inn[s][k] for k in e):
                grew = False
                for k in e:
                    u = inn[s][k] | e[k]
                    if len(u) <= MAX_SET and u != inn[s][k]:
                        inn[s][k] = u
                        grew = True
                if grew:
                    work.append(s)
    return res


def scan_one(path):
    base = os.path.basename(path)
    try:
        va = int(base[len("func_"):-len(".bc")], 16)
    except ValueError:
        return set()
    r = subprocess.run([DIS, "-o", "-", path], capture_output=True)
    if r.returncode != 0:
        return set()
    text = r.stdout.decode("utf-8", "replace")
    if "store i32 32, ptr %state" in text:
        text = RE_CUT.sub(lambda m: m.group(1) + "__remill_missing_block(", text)
    if "__remill_missing_block(ptr" not in text:
        return set()
    return targets_in(text, va)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workdir", default="boot_cache/lift_wip")
    ap.add_argument("--newer", help="only scan func_*.bc newer than this file")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out", default="-")
    a = ap.parse_args()
    # code range (sps/mps/zms): .text then .interpr, up to the start of .rdata
    ranges = [(0x401000, STEAM_CODE_END[os.environ.get("BO2_TARGET", "sps")])]

    cutoff = os.path.getmtime(a.newer) if a.newer else None
    files = []
    with os.scandir(a.workdir) as it:
        for e in it:
            if not (e.name.startswith("func_") and e.name.endswith(".bc")):
                continue
            if cutoff is not None and e.stat().st_mtime <= cutoff:
                continue
            files.append(e.path)
    print(f"scanning {len(files)} lifted functions", file=sys.stderr)

    found = set()
    with ProcessPoolExecutor(max_workers=a.jobs, mp_context=multiprocessing.get_context("fork")) as ex:
        for s in ex.map(scan_one, files, chunksize=64):
            found |= s
    found = sorted(v for v in found if any(lo <= v < hi for lo, hi in ranges))
    out = sys.stdout if a.out == "-" else open(a.out, "w")
    for v in found:
        print(f"0x{v:x}", file=out)
    print(f"{len(found)} distinct targets", file=sys.stderr)


if __name__ == "__main__":
    main()
