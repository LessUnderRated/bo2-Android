#!/usr/bin/env python3
"""Fold the guest READ accessors and the flag predicates of an already-linked module, as a stream filter
(stdin -> stdout), early in the chain of scripts/fold_lift.sh.

The campaign applies these two folds inside tools/boot.py `fix_ir_text`, on the raw per-function bitcode of remill.
scripts/build_steam.sh only keeps linked, optimised slices (boot_cache/<target>/batch_N.bc), so the same code is
applied here line by line. Nothing is re-implemented: the emitted bodies and the flag pattern come from tools/boot.py
(`_inline_guest_reads`, `_FLAG_CALL`, `_ARM64_DL`), like stream_inline_writes.py does for the writes.

  1. datalayout/triple -> arm64 (boot._retarget_arm64): mandatory with any fold, or `opt -O2` truncates
     `bo2_g_base + a` to 32 bits (P663);
  2. `declare … @__remill_read_memory_{8,16,32,f32}(` removed, internal alwaysinline bodies appended;
  3. `call … @__remill_flag_computation_*(i1 X, …)` -> `or i1 X, false` (identity functions in the runtime);
  4. every lifted `define … @sub_<va>(` becomes `noinline`: the relink redirects 2 000+ `_sub_<va>` ENTRIES
     (the exception adapter, the interposers) by patching their first instructions; a copy
     inlined into a caller at -O2 would keep running the superseded body.

Fails (exit 3) rather than emit a partial fold: a flag call the pattern misses, a read reference without its declare.
Witness on stderr: `[fold] …`."""
import importlib.util
import re
import sys

spec = importlib.util.spec_from_file_location("bootpy", "tools/boot.py")
bootpy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bootpy)

READ_DECL = re.compile(r'^\s*declare\b.*@__remill_read_memory_(8|16|32|f32)\(')
READ_REF = re.compile(r'@__remill_read_memory_(8|16|32|f32)\(')
SUB_DEF = re.compile(r'^(define\b.*@sub_[0-9a-f]+\(.*?)(\s*\{\s*)$')


def fold(lines):
    """Yield the folded lines of one module, then the generated read bodies; `fold.stats` holds the counts."""
    declares, refs, flags, subs, retargeted = {}, set(), 0, 0, 0
    for line in lines:
        if line.startswith('target '):
            new = bootpy._retarget_arm64(line)
            retargeted += new != line
            line = new
        elif '@__remill_read_memory_' in line:
            m = READ_DECL.match(line)
            if m:
                declares[m.group(1)] = line
                continue
            refs.update(READ_REF.findall(line))
        if '@__remill_flag_computation_' in line and not line.lstrip().startswith('declare'):
            line, n = bootpy._FLAG_CALL.subn(r'\1 = or i1 \2, false', line)
            if n != 1:
                raise SystemExit(f'[fold] flag call not folded: {line.strip()[:160]}')
            flags += 1
        elif line.startswith('define'):
            m = SUB_DEF.match(line)
            if m and ' noinline' not in line:
                line = f'{m.group(1)} noinline{m.group(2)}'
                subs += 1
        yield line
    if refs - set(declares):
        raise SystemExit(f'[fold] read references without declare: {sorted(refs - set(declares))}')
    if declares:
        micro = ''.join(declares.values())
        out = bootpy._inline_guest_reads(micro)
        generated = out.split('\n\n', 1)[1] if '\n\n' in out else ''
        if generated.count('define internal') != len(declares):
            raise SystemExit('[fold] boot.py did not emit one body per removed declare')
        yield '\n' + generated
    fold.stats = dict(reads=len(declares), flags=flags, subs=subs, retargeted=retargeted)


def demo():
    """Self-check on a four-line module: one read, one flag call, one lifted define."""
    src = ['target datalayout = "e-m:x-p:32:32"\n', 'target triple = "i386-unknown-windows-msvc-coff"\n',
           'declare dso_local i32 @__remill_read_memory_32(ptr noundef, i32 noundef) local_unnamed_addr #0\n',
           'define ptr @sub_401000(ptr noalias %state, i32 %program_counter, ptr noalias %memory) local_unnamed_addr {\n',
           '  %1 = tail call i32 @__remill_read_memory_32(ptr noundef %memory, i32 noundef 4) #8\n',
           '  %2 = tail call zeroext i1 (i1, ...) @__remill_flag_computation_carry(i1 noundef zeroext false, i32 noundef %1) #8\n',
           '  ret ptr %memory\n', '}\n']
    out = ''.join(fold(src))
    assert fold.stats == dict(reads=1, flags=1, subs=1, retargeted=2), fold.stats
    assert 'arm64-apple' in out and 'local_unnamed_addr noinline {' in out and '%2 = or i1 false, false' in out
    assert 'define internal i32 @__remill_read_memory_32(' in out and 'declare dso_local i32 @__remill_read_memory_32(' not in out
    assert '__remill_read_memory_32_slow' in out and 'g_park_pending' in out
    print('auto-contrôle OK')


if __name__ == '__main__':
    if sys.argv[1:] == ['--demo']:
        demo()
    else:
        w = sys.stdout.write
        for chunk in fold(sys.stdin):
            w(chunk)
        s = fold.stats
        # A single small function (the extras, one module each) may hold no read and no flag; a module without a
        # lifted function or without the two `target` lines is not one of ours.
        if not (s['subs'] and s['retargeted'] == 2):
            raise SystemExit(f'[fold] not a lifted module: {s}')
        sys.stderr.write(f"[fold] {s['reads']} read declare(s) folded, {s['flags']} flag calls, "
                         f"{s['subs']} lifted functions marked noinline\n")
