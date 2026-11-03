#!/usr/bin/env python3
"""Diagnose post-remill lift failures for guest VAs (the [STUB-CALL] frontier).

boot.py's lift_one() swallows the real error and returns only a coarse category
string ("opt", "llvm-as", "lift: ..."). This script reproduces boot.py's EXACT
per-function pipeline for a target VA and reports which stage fails together with
the FULL captured stderr, so a frontier address can be classified:

  (a) fixable pipeline issue  -- fix_ir_text / opt mishandles a construct we can
      correct with a targeted transform, OR
  (b) genuinely-unliftable    -- a construct remill/LLVM cannot lower on this path,
      needing a hand-written shim / correct-value stub instead of a lift.

Byte provenance is reproduced faithfully: the base bytes and the gap-bounded
runway are computed with the same calltree ∪ root-closure logic and the same
runway bounds boot.py uses, so the .bc we produce matches the boot lift's .bc.

Usage:
  tools/diag_lift_fail.py 0xa22900 [0xVA ...]   # deep per-stage capture
  tools/diag_lift_fail.py --frontier [--limit N] # classify the whole stub set
"""
from __future__ import annotations
import os, sys, re, csv, bisect, subprocess, argparse, tempfile, json
from collections import Counter, defaultdict
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

# x86 instructions remill lowers via __remill_sync_hyper_call -- the intrinsic
# whose symbol name remill emits as all-NUL bytes, which llvm-as then rejects.
# (cpuid/rdtsc/rdtscp/xgetbv/rd|wrmsr/... -> SyncHyperCall::Name; see HyperCall.h.)
HYPERCALL_MNEMONICS = {
    "cpuid", "rdtsc", "rdtscp", "xgetbv", "xsetbv", "rdmsr", "wrmsr",
    "wbinvd", "invd", "lgdt", "lidt", "lldt", "ltr", "lar", "lsl",
    "sgdt", "sidt", "clts", "invlpg",
}
_MD = Cs(CS_ARCH_X86, CS_MODE_32)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# Reuse boot.py's exact constants + fix_ir_text so we can't drift from the lift.
import boot
from pe_parser import PEFile
from lift_roots import extra_functions_from_roots

REMILL, LLVM17, BIN, CALLTREE_CSV = boot.REMILL, boot.LLVM17, boot.BIN, boot.CALLTREE_CSV
RUNWAY = 64
ROOTS_FILE = os.path.join(HERE, "lift_roots_combined.txt")


def build_functions(count: int = 10036):
    """Reproduce boot.py's `functions` list (base bytes) and per-fn runway.

    Returns va -> (size, base_hex, runway_hex, name), identical to what
    boot.py feeds lift_one().
    """
    csv.field_size_limit(1 << 30)
    functions = []
    with open(CALLTREE_CSV) as f:
        for row in csv.DictReader(f):
            if len(functions) >= count:
                break
            size = int(row["size"])
            if size < 4 or size > 100000:
                continue
            functions.append((int(row["va"], 16), int(row["rva"], 16), size,
                              row["bytes_hex"], row["name"]))

    _pe = PEFile(BIN).parse()
    all_starts = sorted({int(r0["va"], 16) for r0 in csv.DictReader(open(CALLTREE_CSV))})

    base_vas = {f[0] for f in functions}
    extra = extra_functions_from_roots(_pe, os.path.dirname(CALLTREE_CSV),
                                       roots_file=ROOTS_FILE)
    added = [e for e in extra if e[0] not in base_vas]
    functions.extend(added)
    all_starts = sorted(set(all_starts) | {e[0] for e in added})

    secs = [(_pe.image_base + s.virtual_address,
             _pe.image_base + s.virtual_address + s.virtual_size) for s in _pe.sections]

    def _sec_end(va):
        for a, b in secs:
            if a <= va < b:
                return b
        return None

    def _img_byte(va):
        try:
            return _pe._raw[_pe.rva_to_offset(va - _pe.image_base)]
        except Exception:
            return None

    out = {}
    for va, rva, size, hex_bytes, name in functions:
        end = va + size
        ni = bisect.bisect_left(all_starts, end)
        limit = end + RUNWAY
        if ni < len(all_starts):
            limit = min(limit, all_starts[ni])
        se = _sec_end(end)
        if se is not None:
            limit = min(limit, se)
        runway = bytearray()
        a = end
        while a < limit:
            b = _img_byte(a)
            if b is None:
                break
            runway.append(b)
            a += 1
        out[va] = (size, hex_bytes, runway.hex(), name)
    return out


def _run(cmd, timeout, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)


def _remill_lift_t(hex_bytes, va, bc_path, timeout):
    return subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                           "--bytes", hex_bytes, "--address", hex(va),
                           "--bc_out", bc_path],
                          capture_output=True, text=True, timeout=timeout)


def lift_stages(va, size, base_hex, runway_hex, name, workdir, verbose=False,
                remill_timeout=30):
    """Run boot.py's exact 5-stage pipeline, capturing full output per stage.

    Returns (fail_stage, detail) where fail_stage is one of
    {'remill','remill-timeout','llvm-dis','llvm-as','opt', None(=ok)}.
    remill_timeout can be shortened to bucket runway-dragged slow lifts quickly.
    """
    bc = os.path.join(workdir, f"func_{va:x}.bc")

    # Stage 1: remill-lift (with boot.py's timeout->base-only fallback)
    try:
        r = _remill_lift_t(base_hex + runway_hex, va, bc, remill_timeout)
    except subprocess.TimeoutExpired:
        if not runway_hex:
            return ("remill-timeout", "TIMEOUT (base-only, no runway)")
        try:
            r = _remill_lift_t(base_hex, va, bc, remill_timeout)
        except subprocess.TimeoutExpired:
            return ("remill-timeout", "TIMEOUT (base-only fallback)")
    if r.returncode != 0:
        return ("remill", f"rc={r.returncode}\n{r.stderr.strip()}")
    if not os.path.exists(bc):
        return ("remill", "no .bc produced (rc=0)")

    # Stage 2: llvm-dis
    ll = bc + ".ll"
    r = _run([LLVM17 + "/bin/llvm-dis", "-o", ll, bc], timeout=15)
    if r.returncode != 0:
        return ("llvm-dis", f"rc={r.returncode}\n{r.stderr.strip()}")

    # fix_ir_text (boot.py's exact transform)
    with open(ll) as f:
        ir = f.read()
    with open(ll, "w") as f:
        f.write(boot.fix_ir_text(ir))

    # Stage 3: llvm-as
    raw = bc + ".raw"
    r = _run([LLVM17 + "/bin/llvm-as", "-o", raw, ll], timeout=15)
    if r.returncode != 0:
        return ("llvm-as", f"rc={r.returncode}\n{r.stderr.strip()}")

    # Stage 4: opt -O2
    fixed = bc + ".fixed"
    try:
        r = _run([LLVM17 + "/bin/opt", "-O2", "-o", fixed, raw], timeout=45)
    except subprocess.TimeoutExpired:
        return ("opt", "TIMEOUT")
    if r.returncode != 0:
        return ("opt", f"rc={r.returncode}\n{r.stderr.strip()}")
    if not os.path.exists(fixed):
        return ("opt", "no .fixed produced (rc=0)")
    return (None, "ok")


def _budget_worker(job):
    import time
    va, size, base_hex, runway_hex, name, workdir, timeout = job
    bc = os.path.join(workdir, f"probe_{va:x}.bc")
    t0 = time.time()
    # base+runway first (what the pipeline tries first), then base-only fallback
    try:
        r = _remill_lift_t(base_hex + runway_hex, va, bc, timeout)
        dt = time.time() - t0
        return (va, "converged", round(dt, 1), "base+runway", r.returncode)
    except subprocess.TimeoutExpired:
        pass
    t1 = time.time()
    if not runway_hex:
        return (va, "hang", round(time.time() - t0, 1), "no-runway", None)
    try:
        r = _remill_lift_t(base_hex, va, bc, timeout)
        return (va, "converged", round(time.time() - t0, 1), "base-only", r.returncode)
    except subprocess.TimeoutExpired:
        return (va, "hang", round(time.time() - t0, 1), "both-timed-out", None)


def budget_probe(funcs, vas, timeout=120, jobs=1):
    """Re-run remill ONLY with a long timeout to distinguish 'converges with more
    budget' (=> targeted re-lift fixes it) from 'genuine hang' (=> needs a shim)."""
    from concurrent.futures import ProcessPoolExecutor, as_completed
    workdir = tempfile.mkdtemp(prefix="budget_")
    print(f"Budget probe: {len(vas)} timeout functions, per-lift timeout={timeout}s, jobs={jobs}")
    print(f"  workdir={workdir}\n")
    jobs_list = []
    for va in vas:
        if va not in funcs:
            print(f"  0x{va:x}: NOT in lift set"); continue
        size, base_hex, runway_hex, name = funcs[va]
        jobs_list.append((va, size, base_hex, runway_hex, name, workdir, timeout))
    conv = hang = 0
    with ProcessPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(_budget_worker, j): j[0] for j in jobs_list}
        for fut in as_completed(futs):
            va, verdict, secs, how, rc = fut.result()
            if verdict == "converged":
                conv += 1
                print(f"  0x{va:x}: CONVERGED in {secs}s ({how}, remill rc={rc})")
            else:
                hang += 1
                print(f"  0x{va:x}: STILL HANGS after {secs}s ({how})")
    print(f"\n=== BUDGET PROBE RESULT: {conv} converged, {hang} still hang "
          f"(of {len(jobs_list)}) at {timeout}s ===")
    return workdir


def deep(vas, funcs):
    workdir = tempfile.mkdtemp(prefix="diag_")
    print(f"work dir: {workdir}\n")
    for va in vas:
        if va not in funcs:
            print(f"=== 0x{va:x}: NOT in lift set (not calltree, not in closure) ===\n")
            continue
        size, base_hex, runway_hex, name = funcs[va]
        print(f"=== 0x{va:x}  name={name}  size={size}B  runway={len(runway_hex)//2}B ===")
        stage, detail = lift_stages(va, size, base_hex, runway_hex, name, workdir, verbose=True)
        if stage is None:
            print(f"  RESULT: lifts OK now (no failure reproduced)\n")
        else:
            print(f"  FAIL STAGE: {stage}")
            print("  --- captured error ---")
            for line in detail.splitlines():
                print(f"    {line}")
            print()
        # keep the .bc/.ll around for inspection
        print(f"  artifacts: {workdir}/func_{va:x}.bc[.ll]\n")
    return workdir


def _load_frontier_vas():
    stub_file = os.path.join(HERE, "..", "boot_cache", "undefined_stubs.txt")
    vas = []
    with open(stub_file) as f:
        for line in f:
            line = line.strip()
            if line:
                vas.append(int(line, 16))
    return vas, stub_file


def scan_hypercalls(funcs):
    """Static, instant: how many frontier stubs contain a hyper-call instruction
    (the construct that yields remill's NUL-named __remill_sync_hyper_call and the
    llvm-as failure). Answers the specific-vs-pattern question without lifting."""
    vas, stub_file = _load_frontier_vas()
    print(f"Static hyper-call scan of {len(vas)} frontier stubs ({stub_file})\n")
    hit, miss, notin = [], [], []
    mnem_counts = Counter()
    for va in vas:
        if va not in funcs:
            notin.append(va)
            continue
        size, base_hex, runway_hex, name = funcs[va]
        code = bytes.fromhex(base_hex)          # extent only, NOT runway
        found = set()
        for insn in _MD.disasm(code, va):
            if insn.mnemonic in HYPERCALL_MNEMONICS:
                found.add(insn.mnemonic)
        if found:
            hit.append((va, name, sorted(found)))
            for m in found:
                mnem_counts[m] += 1
        else:
            miss.append((va, name))
    print(f"=== HYPER-CALL STATIC SCAN ===")
    print(f"  contain a hyper-call insn : {len(hit)}")
    print(f"  no hyper-call insn        : {len(miss)}")
    print(f"  not in lift set           : {len(notin)}")
    print(f"\n  hyper-call mnemonic frequency (functions containing each):")
    for m, n in mnem_counts.most_common():
        print(f"    {m:8s}: {n}")
    print(f"\n  frontier stubs that contain a hyper-call:")
    for va, name, mn in sorted(hit):
        print(f"    0x{va:x}  {name:16s}  {','.join(mn)}")
    return {"hit": [f"0x{v:x}" for v, _, _ in hit],
            "miss": [f"0x{v:x}" for v, _ in miss],
            "not_in_set": [f"0x{v:x}" for v in notin]}


def _frontier_worker(job):
    va, size, base_hex, runway_hex, name, workdir, remill_timeout = job
    stage, detail = lift_stages(va, size, base_hex, runway_hex, name, workdir,
                                remill_timeout=remill_timeout)
    return (va, stage or "ok", _sig(detail))


def frontier(funcs, limit=None, stride=None, jobs=1, remill_timeout=12):
    from concurrent.futures import ProcessPoolExecutor, as_completed
    vas, stub_file = _load_frontier_vas()
    if stride and stride > 1:
        vas = vas[::stride]
    if limit:
        vas = vas[:limit]
    workdir = tempfile.mkdtemp(prefix="diagfront_")
    print(f"Classifying {len(vas)} frontier stubs from {stub_file}")
    print(f"  jobs={jobs}  remill_timeout={remill_timeout}s  workdir={workdir}\n")

    by_stage = Counter()
    stage_sigs = defaultdict(Counter)        # stage -> signature -> count
    stage_examples = defaultdict(list)
    not_in_set = []
    per_va = {}

    jobs_list = []
    for va in vas:
        if va not in funcs:
            not_in_set.append(va)
            per_va[f"0x{va:x}"] = "NOT_IN_LIFT_SET"
            continue
        size, base_hex, runway_hex, name = funcs[va]
        jobs_list.append((va, size, base_hex, runway_hex, name, workdir, remill_timeout))

    done = 0
    with ProcessPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(_frontier_worker, j): j[0] for j in jobs_list}
        for fut in as_completed(futs):
            va, stage, sig = fut.result()
            by_stage[stage] += 1
            per_va[f"0x{va:x}"] = stage
            stage_sigs[stage][sig] += 1
            if len(stage_examples[stage]) < 6:
                stage_examples[stage].append((f"0x{va:x}", sig))
            done += 1
            if done % 10 == 0 or done == len(jobs_list):
                print(f"  [{done}/{len(jobs_list)}] {dict(by_stage)}")

    print("\n=== FRONTIER CLASSIFICATION ===")
    print(f"total classified: {sum(by_stage.values())}  (not-in-lift-set: {len(not_in_set)})")
    for k, n in by_stage.most_common():
        print(f"  {k:14s}: {n}")
    print("\n=== error signatures per failing stage ===")
    for stage in [s for s in by_stage if s != "ok"]:
        print(f"  [{stage}]")
        for sig, n in stage_sigs[stage].most_common():
            print(f"    x{n:<3d} {sig}")
    print("\n=== examples per stage ===")
    for k, exs in stage_examples.items():
        print(f"  {k}:")
        for va_s, sig in exs:
            print(f"    {va_s}: {sig}")

    outjson = os.path.join(workdir, "frontier_classification.json")
    with open(outjson, "w") as f:
        json.dump({"by_stage": dict(by_stage),
                   "stage_signatures": {s: dict(c) for s, c in stage_sigs.items()},
                   "per_va": per_va,
                   "not_in_lift_set": [f"0x{v:x}" for v in not_in_set]}, f, indent=2)
    print(f"\nwrote {outjson}")
    print(f"work dir (artifacts): {workdir}")
    return workdir


def _sig(detail):
    """Collapse a captured error into a stable one-line signature."""
    for line in detail.splitlines():
        s = line.strip()
        if not s:
            continue
        # normalize addresses / value names so signatures cluster
        s = re.sub(r'0x[0-9a-fA-F]+', '0x_', s)
        s = re.sub(r'%[\w.]+', '%_', s)
        s = re.sub(r'func_[0-9a-f]+', 'func_', s)
        s = re.sub(r"'[^']*\.ll'", "'FILE.ll'", s)
        if s.lower().startswith(("warning", "note")):
            continue
        return s[:160]
    return detail.strip()[:160]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vas", nargs="*", help="hex VAs to diagnose deeply")
    ap.add_argument("--frontier", action="store_true",
                    help="classify all of boot_cache/undefined_stubs.txt")
    ap.add_argument("--scan", action="store_true",
                    help="fast static hyper-call scan of the frontier (no lifting)")
    ap.add_argument("--stride", type=int, default=None,
                    help="with --frontier: lift every Nth stub (spread sample)")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=1,
                    help="parallel lift jobs. DEFAULT 1 (SERIAL). Parallel remill "
                         "lifts can exhaust host memory and crash the machine — "
                         "only raise this if you know the box can take it.")
    ap.add_argument("--remill-timeout", type=int, default=12,
                    help="per-remill-lift timeout; short = bucket slow lifts fast")
    ap.add_argument("--budget", nargs="*", default=None,
                    help="remill-only long-timeout probe of these timeout VAs "
                         "(converges-with-budget vs genuine-hang)")
    ap.add_argument("--count", type=int, default=10036,
                    help="calltree rows to include (match the boot lift's -n)")
    args = ap.parse_args()

    print("Building faithful lift set (calltree ∪ root-closure + runway)...")
    funcs = build_functions(args.count)
    print(f"  {len(funcs)} functions in lift set\n")

    if args.budget is not None:
        budget_probe(funcs, [int(v, 16) for v in args.budget],
                     timeout=max(args.remill_timeout, 120), jobs=args.jobs)
    if args.scan:
        scan_hypercalls(funcs)
    if args.frontier:
        frontier(funcs, args.limit, args.stride, args.jobs, args.remill_timeout)
    if args.vas:
        deep([int(v, 16) for v in args.vas], funcs)
    if not args.frontier and not args.vas and not args.scan:
        print("nothing to do: pass VAs and/or --frontier / --scan")


if __name__ == "__main__":
    main()
