#!/usr/bin/env python3
"""flagscan.py <pe_sections.bin> — where the exe breaks "flags are dead across a call" (bo2-dead-flags' assumption).

pe_sections.bin is the one tools/boot.py writes next to the lift (u32 count, then per section u32 va, u32 size,
bytes). Linear sweep of .text (0x401000) with capstone; prints addresses only.

  out: a direct call followed, before any flag writer (8 instructions at most), by a flag reader (jcc, setcc,
       cmovcc, adc/sbb, lahf, pushfd...): the callee returns a status in the flags. Candidate for FLAGS_OUT.
  in:  a direct-call target whose code reads a flag before writing one: it takes flags from its caller.
       Candidate for FLAGS_IN. The call just before such a call is usually a FLAGS_OUT callee (`call A; call B`
       hands A's ZF to B with no reader in between, so "out" alone misses A): printed as "fed by".

A linear sweep also decodes data (jump tables after a noreturn call, alignment): check each candidate by hand —
the context printed is a few mnemonics, never bytes. Results for the Steam Zombies exe are in README.md.
"""
import struct
import sys
from collections import Counter, defaultdict

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

d = open(sys.argv[1], "rb").read()
n = struct.unpack_from("<I", d, 0)[0]
o = 4
secs = []
for _ in range(n):
    va, ln = struct.unpack_from("<II", d, o)
    o += 8
    secs.append((va, ln, o))
    o += ln


def off(va):
    for sva, ln, so in secs:
        if sva <= va < sva + ln:
            return so + va - sva
    return None


READERS = {"adc", "sbb", "rcl", "rcr", "lahf", "pushf", "pushfd", "salc", "into"}
WRITERS = set("""add sub adc sbb and or xor cmp test inc dec neg shl shr sar sal rol ror rcl rcr bt bts btr btc bsf bsr
imul mul div idiv sahf popf popfd comiss comisd ucomiss ucomisd fcomi fucomi fcompi fucompi cmpxchg cmpxchg8b xadd
stc clc cmc shld shrd daa das aaa aas scasb scasw scasd cmpsb cmpsw cmpsd ptest""".split())
STOP = {"call", "ret", "jmp", "int", "int3", "hlt", "ud2"}


def base(m):
    for p in ("lock ", "rep ", "repe ", "repne "):
        if m.startswith(p):
            m = m[len(p):]
    return m


def reads(m):
    return (m.startswith("j") and m not in ("jmp", "jecxz", "jcxz")) or m.startswith("set") \
        or m.startswith("cmov") or m in READERS


sva, ln, so = next(s for s in secs if s[0] == 0x401000)
lo, hi = sva, sva + ln
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True
out_hits, out_sites = Counter(), defaultdict(list)
callers = defaultdict(list)          # target -> [(call site, previous direct call target or None)]
pending, window, prev_call = None, [], None
last = []
for ins in md.disasm(d[so:so + ln], sva):
    m = base(ins.mnemonic)
    if pending is not None:
        if reads(m):
            t = int(pending.op_str, 16)
            out_hits[t] += 1
            out_sites[t].append("0x%x %s[%s]" % (pending.address, m, ",".join(window)))
            pending = None
        elif m in WRITERS or m in STOP or len(window) >= 8:
            pending = None
        else:
            window.append(m)
    if m == "call" and ins.op_str.startswith("0x"):
        t = int(ins.op_str, 16)
        if lo <= t < hi:
            fed = prev_call if last and last[-1] == "call" else None
            callers[t].append((ins.address, fed))
            pending, window = ins, []
            prev_call = t
    last = (last + [m])[-2:]

print("out: %d call sites read a flag their callee left, %d callees" % (sum(out_hits.values()), len(out_hits)))
for t, c in out_hits.most_common():
    print("  0x%x  %d sites  e.g. %s" % (t, c, "; ".join(out_sites[t][:3])))

md2 = Cs(CS_ARCH_X86, CS_MODE_32)
ins_hits = []
for t, sites in callers.items():
    q = off(t)
    seq = []
    for i in md2.disasm(d[q:q + 96], t):
        m = base(i.mnemonic)
        if reads(m):
            ins_hits.append((t, m, seq, sites))
            break
        if m in WRITERS or m in STOP or len(seq) > 24:
            break
        seq.append(m)
print("in: %d of %d call targets read a flag before writing one" % (len(ins_hits), len(callers)))
for t, m, seq, sites in sorted(ins_hits):
    fed = sorted({"0x%x" % f for _, f in sites if f is not None})
    print("  0x%x  %d callers  %s after [%s]%s" % (t, len(sites), m, ",".join(seq),
                                                  ("  fed by " + ",".join(fed)) if fed else ""))
