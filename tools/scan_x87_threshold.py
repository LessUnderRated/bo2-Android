#!/usr/bin/env python3
"""Find lifted functions whose x87 FISTP overflow check lost its threshold.

remill lowers FISTP to `fptosi` plus a range check: if |value| exceeds the
destination's max, the result is the x86 "integer indefinite" value (INT_MIN).
The threshold is an x86_fp80 literal, and an early version of boot.py's
fix_ir_text rewrote every `0xK...` fp80 constant to 0.0 — which turns the check
into `|value| > 0.0`, so EVERY non-zero float converts to INT_MIN.

That was fixed in the lifter long ago, but functions lifted before the fix keep
their stale cached IR forever: 154 of them were still live in lifted.o, and one
(sub_880e17, the GSC `wait` opcode) turned `wait 0.05` into a negative frame
count, an engine error path, and a ~4-billion-iteration hang.

Streams the linked IR (llvm-dis to stdout, no multi-GB temp file) and reports
functions whose threshold is still 0.0. To repair: delete the cached
func_<va>.bc* in the lift workdir, re-lift, then interpose the fresh body with
tools/stub_override.py + tools/patch_redirect.py (a strong symbol already in
lifted.o cannot be overridden at link time).

Usage: scan_x87_threshold.py boot_cache/linked_nostubs.bc
Writes the full list of broken function names to /tmp/x87_broken.txt.
"""
import re, subprocess, sys

DIS = "/opt/homebrew/opt/llvm@17/bin/llvm-dis"
bc = sys.argv[1]
p = subprocess.Popen([DIS, "-o", "-", bc], stdout=subprocess.PIPE, bufsize=1 << 20)

cur = None
pending = {}           # ssa name -> (threshold_text, line_no)
bad, good, nlines = {}, {}, 0
re_def = re.compile(rb'^define .*@([\w.$]+)\(')
re_cmp = re.compile(rb'^\s*(%\w+) = fcmp ogt double %\w+, (\S+)')
re_sel = re.compile(rb'^\s*%\w+ = select i1 (%\w+), i(32|64) (-2147483648|-9223372036854775808),')

for line in p.stdout:
    nlines += 1
    m = re_def.match(line)
    if m:
        cur = m.group(1).decode(); pending.clear(); continue
    m = re_cmp.match(line)
    if m:
        pending[m.group(1)] = m.group(2).decode(); continue
    m = re_sel.match(line)
    if m:
        thr = pending.get(m.group(1))
        if thr is None: continue
        tgt = bad if thr in ("0.000000e+00", "0.0") else good
        tgt.setdefault(cur, set()).add(thr)

p.stdout.close(); p.wait()
print("scanned %d IR lines" % nlines)
print("functions with a CORRECT fistp threshold : %d" % len(good))
print("functions with a ZEROED  fistp threshold : %d" % len(bad))
if bad:
    names = sorted(bad)
    print("first 60 broken:", " ".join(names[:60]))
    with open("/tmp/x87_broken.txt", "w") as f:
        f.write("\n".join(names) + "\n")
    print("full list -> /tmp/x87_broken.txt")
