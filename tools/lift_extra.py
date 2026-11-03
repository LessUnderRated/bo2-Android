"""lift_extra.py — lift INDIVIDUAL entry points by VA, reusing boot.py's exact
pipeline (remill -> fix_ir_text -> llvm-as -> opt -O2), and emit both a linkable
object and the .inc the shim uses to register them.

Why this exists. boot.py's lift set is Ghidra's function catalogue plus a
direct-call closure, and its generated main.cpp registers exactly that set. Two
kinds of address the running guest reaches are in neither, and both surface as a
hard stop:

  * "[shim] function_call/__remill_jump: unknown target 0xVA" — a dispatch-table
    target Ghidra never catalogued as a function (GSC builtins), or a case block
    of a computed `jmp [table]` inside a function whose catalogued extent was
    TRUNCATED at that jmp (BoxOnPlaneSide 0x64f260: 0x89 recorded, 0x236 real).

  * "[MISSING] block at 0xVA not lifted" — remill declined to follow a DIRECT
    jmp while tracing and emitted __remill_missing_block for the target, so the
    block exists in no lifted function at all. The shim's missing-block handler
    already retries via the jump table, so registering the block as its own
    entry point is a complete fix.

Both are cured the same way: lift the VA as a standalone entry (extent
synthesized by lift_roots' linear sweep = "this block plus everything to the
shared epilogue"), link it, and register it. The guest State carries all
registers and the x87 stack, so entering mid-function behaves identically.

Usage:
    python3 tools/lift_extra.py 86fb60:f0 64f2e8            # ad-hoc, one-off
    python3 tools/lift_extra.py --list tools/extra_lifted_vas.txt \
            --obj boot_cache/extra_lifted.o \
            --inc runtime/win32/extra_lifted_vas.inc

":size" pins the byte extent; omit it to synthesize. --list reads one VA per
line ("#" comments allowed). VAs that fail to lift are reported and left out of
both the object and the .inc, so a partial frontier still builds.

To find the frontier of the [MISSING] class statically, grep the lift workdir's
*.bc.fixed for __remill_missing_block, llvm-dis the hits, and read the
`add i32 %program_counter, <const>` feeding each call: func_va + const is the
unlifted block. Drop the ones main.cpp already registers.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKDIR = os.environ.get("LIFT_WIP", os.path.join(ROOT, "boot_cache", "lift_wip"))  # construire_objets.py : un par tranche
LLVM17 = os.environ.get("LLVM17", "/opt/homebrew/opt/llvm@17")


def read_list(path):
    specs = []
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if line:
                specs.append(line[2:] if line.lower().startswith("0x") else line)
    return specs


def lift(specs, patch_thunks=False):
    """Lift each "va_hex[:size_hex]" spec. Returns [(va, bc_fixed_path)] for the
    ones that succeeded; failures are printed and skipped."""
    sys.path.insert(0, HERE)
    os.chdir(ROOT)
    import boot
    import lift_roots
    from pe_parser import PEFile

    pe = PEFile(boot.BIN).parse()
    os.makedirs(WORKDIR, exist_ok=True)   # remill aborts (signal 6) on a missing output folder: a fresh build has none
    # Empty index on purpose: a catalogued (and here WRONG, truncated) Ghidra
    # extent would take priority over the linear sweep, and the sweep is exactly
    # what we want for a VA whose recorded extent is the problem.
    sweeper = lift_roots._Closure(pe, {})
    done, failed = [], []
    for spec in specs:
        va_s, _, size_s = spec.partition(":")
        va = int(va_s, 16)
        size = int(size_s, 16) if size_s else sweeper._decode(va)[0]
        if size <= 0:
            failed.append((va, "no extent synthesized"))
            continue
        try:
            off = pe.rva_to_offset(va - pe.image_base)
        except Exception as e:
            failed.append((va, f"not mapped: {e}"))
            continue
        body = bytearray(pe._raw[off:off + size])
        # `call $+5` IS NOT A CALL. It is MSVC's position-independent idiom:
        # push the current address so the following `pop reg` can read it. Remill
        # lifts it as a real call into a separately-registered block, so the
        # callee pops the return address (correct for the guest) and then runs on
        # to the enclosing function's `ret` — which therefore pops a slot that is
        # NOT a return address and dispatches into the guest heap. That is what
        # kills BO2's sound-decode thread.
        #
        # `E8 00 00 00 00` (call $+5) and `68 <next>` (push imm32) are both five
        # bytes and do exactly the same thing to the machine state, so rewriting
        # one into the other before lifting is exact, not an approximation — and
        # it leaves Remill nothing to mis-model.
        #
        # 🔴 P748 : SEULEMENT A UN DEBUT D'INSTRUCTION. La recherche d'octets bruts a reecrit 30 faux sites en
        # juillet (`cmp dword ptr [ebx+0xe8], 0` = 83 BB E8 00 00 00 00 devenait une lecture a [ebx+0x5c038e68]),
        # et la_2 et haiti bouclaient dans sub_5c02f0. Les debuts viennent d'un balayage lineaire du corps.
        if patch_thunks:
            from capstone import Cs, CS_ARCH_X86, CS_MODE_32
            starts = {ins.address - va for ins in Cs(CS_ARCH_X86, CS_MODE_32).disasm(bytes(body), va)}
            i = 0
            while i + 5 <= len(body):
                if i in starts and body[i:i + 5] == b"\xe8\x00\x00\x00\x00":
                    nxt = va + i + 5
                    body[i] = 0x68
                    body[i + 1:i + 5] = nxt.to_bytes(4, "little")
                    print(f"  0x{va + i:x}: call $+5 -> push 0x{nxt:x}")
                    i += 5
                else:
                    i += 1
        body = bytes(body)
        if len(body) != size:
            failed.append((va, f"only {len(body)} of {size} bytes readable"))
            continue
        # Drop a stale negative-result marker so a rerun actually retries (the
        # marker is boot.py's cache for "this VA already failed the pipeline").
        fail_marker = os.path.join(WORKDIR, f"func_{va:x}.bc.fail")
        if os.path.exists(fail_marker):
            os.remove(fail_marker)
        # runway = "": the extent already ends at a ret, and trailing bytes are
        # what drags an adjacent body into remill's tracer.
        _va, fixed, ok, err = boot.lift_one(
            (va, va - pe.image_base, size, body.hex(), "", f"sub_{va:x}", WORKDIR))
        if ok:
            done.append((va, fixed))
        else:
            failed.append((va, str(err)))
    print(f"lifted {len(done)}/{len(done) + len(failed)}")
    for va, why in failed:
        print(f"  FAIL 0x{va:x}: {why}")
    return done


def _ld_r(out, objs):
    """`ld -r` in chunks. Thousands of paths blow past the argv limit, and a
    single merge of thousands of objects is slow, so fold in batches."""
    CHUNK = 400
    if len(objs) <= CHUNK:
        r = subprocess.run(["ld", "-r", "-arch", "arm64", "-o", out] + objs,
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("ld -r failed: " + r.stderr.strip()[:400])
        return
    stage = []
    for i in range(0, len(objs), CHUNK):
        s = f"{out}.stage{i // CHUNK}.o"
        _ld_r(s, objs[i:i + CHUNK])
        stage.append(s)
    _ld_r(out, stage)
    for s in stage:
        os.remove(s)


def build_obj(done, obj_path):
    """Compile each .bc.fixed and merge into one relocatable object. A VA whose
    object fails to assemble is dropped from the returned list. Duplicate VAs are
    dropped too: the same VA reachable from two harvest lists would otherwise
    define _sub_<va> twice and fail the final link with a duplicate symbol."""
    parts, kept, seen = [], [], set()
    for va, bc in done:
        if va in seen:
            continue
        seen.add(va)
        part = f"{obj_path}.part_{va:x}.o"
        r = subprocess.run([f"{LLVM17}/bin/clang", "-arch", "arm64", "-c", "-O1",
                            "-x", "ir", bc, "-o", part],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"  FAIL compile 0x{va:x}: {r.stderr.strip()[:120]}")
            continue
        parts.append(part)
        kept.append(va)
    _ld_r(obj_path, parts)
    for p in parts:
        os.remove(p)
    print(f"wrote {obj_path} ({len(kept)} entries)")
    return kept


def write_inc(vas, inc_path):
    with open(inc_path, "w") as f:
        f.write("// GENERATED by tools/lift_extra.py — do not edit by hand.\n"
                "// Entry points lifted out-of-band (see that script's docstring for\n"
                "// why boot.py's lift set misses them). win32_runtime.cpp includes\n"
                "// this twice: once to declare sub_<va>, once to register it.\n")
        for va in sorted(vas):
            f.write(f"BO2_EXTRA_LIFTED({va:x})\n")
    print(f"wrote {inc_path} ({len(vas)} entries)")


def main():
    args = sys.argv[1:]
    obj = inc = None
    patch_thunks = False
    specs = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--obj":
            obj = args[i + 1]; i += 2
        elif a == "--inc":
            inc = args[i + 1]; i += 2
        elif a == "--list":
            specs += read_list(args[i + 1]); i += 2
        elif a == "--patch-thunks":
            patch_thunks = True; i += 1
        else:
            specs.append(a); i += 1
    if not specs:
        print(__doc__)
        return 2
    done = lift(specs, patch_thunks)
    if not done:
        return 1
    kept = [va for va, _ in done]
    if obj:
        kept = build_obj(done, obj)
    if inc:
        write_inc(kept, inc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
