#!/bin/bash
# Close the __stub_call_fatal frontier in one pass.
#
# boot.py emits an aborting STUB for every VA it did not lift. The runtime only
# reveals one when the guest CALLS it — one abort per ~7-minute run, walking a
# call chain one link at a time. That is how the campaign map load surfaced
# 0x62f550 (reached from the asset-type dispatch at 0x779642 while unloading the
# frontend zones): the map load worked, and died on a function that was simply
# not in the binary.
#
# tools/find_stubs.py finds them all statically, by their exact emitted body
# (movz w0,#lo ; movk w0,#hi ; bl __stub_call_fatal). This lifts every one that
# lies inside .text, renames each to _impl so it links ALONGSIDE lifted.o's stub
# instead of colliding with it, and writes the patch_redirect pairs that make
# both direct and indirect calls land on the real body.
#
# Re-run after any full re-lift. It is idempotent.
set -e
cd "$(dirname "$0")/.."
BC=boot_cache
L=/opt/homebrew/opt/llvm@17/bin
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ⚠️ SURVIVE A MISSING boot. find_stubs.py reads the LINKED binary, and the
# situation that most needs this script is the one where the last link FAILED
# and deleted it — which is exactly how it deadlocked the first time: the fix
# for a broken link required the binary the broken link did not produce. The
# discovered list (including everything the closure loop adds) is therefore
# cached in boot_cache/stub_vas.txt and used as the seed when boot is absent.
CACHE=$BC/stub_vas.txt
if python3 tools/find_stubs.py > "$TMP/all.txt" 2>/dev/null && [ -s "$TMP/all.txt" ]; then
    # Range-check: some stubs are for garbage VAs harvested from bad indirect
    # call targets (_sub_14bf9304 and friends) — not code, cannot lift.
    python3 - "$TMP" "$CACHE" <<'PY'
import os, sys
tmp, cache = sys.argv[1], sys.argv[2]
vas = [int(l, 16) for l in open(f"{tmp}/all.txt") if l.startswith("0x")]
good = {v for v in vas if 0x401000 <= v <= 0xA31BB0}
# Union with the cache: once a body is overridden it is no longer a stub, so a
# rescan of the FIXED binary would return an empty list and silently undo the
# whole override set on the next build.
if os.path.exists(cache):
    good |= {int(l, 16) for l in open(cache) if l.startswith("0x")}
open(f"{tmp}/vas.txt", "w").write("\n".join("0x%x" % v for v in sorted(good)) + "\n")
print(f"[stubs] {len(vas)} stubs found, {len(good)} to lift (incl. cache)")
PY
elif [ -s "$CACHE" ]; then
    cp "$CACHE" "$TMP/vas.txt"
    echo "[stubs] boot unreadable — seeding from $CACHE ($(grep -c . "$CACHE") VAs)"
else
    echo "[stubs] no boot and no cache; build once, then re-run" >&2
    exit 1
fi

# ⚠️ DROP VAs THAT ALREADY HAVE AN _impl SOMEWHERE. find_stubs.py reports them
# as stubs and is not wrong to: patch_redirect rewrites only the function ENTRY,
# so an already-overridden stub still carries its `movz/movk/bl` a few words
# further down, inside the scan window. Lifting those again produces
# `duplicate symbol '_sub_63a7b0_impl'` against extra_lifted2.o / thunks_all.o.
python3 - "$TMP" <<'PY'
import glob, subprocess, sys
tmp = sys.argv[1]
have_impl = set()
for o in glob.glob('boot_cache/*.o'):
    if o.endswith('stub_impl.o'):
        continue
    out = subprocess.run(['nm', '-g', '--defined-only', o],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        t = line.split()
        if t and t[-1].startswith('_sub_') and t[-1].endswith('_impl'):
            have_impl.add(t[-1][5:-5])
vas = [l.strip() for l in open(f"{tmp}/vas.txt") if l.startswith("0x")]
keep = [v for v in vas if v[2:] not in have_impl]
drop = [v for v in vas if v[2:] in have_impl]
open(f"{tmp}/vas.txt", "w").write("\n".join(keep) + "\n")
if drop:
    print(f"[stubs] {len(drop)} already overridden elsewhere, skipped: "
          + " ".join(drop))
PY

# ITERATE TO CLOSURE. A freshly lifted body can call a function that is in NO
# object at all — 0x9b1640 calls 0x9b0c30, 0x9c4434 calls 0x9c4532, and neither
# callee exists anywhere. Those surface as `Undefined symbols for architecture
# arm64` after a 90-second link, so resolve them here, where it costs seconds.
for pass in 1 2 3 4 5; do
    python3 tools/lift_extra.py --list "$TMP/vas.txt" --obj "$TMP/raw.o" \
        --inc "$TMP/unused.inc" | tail -1
    NEW=$("$L/llvm-nm" --undefined-only "$TMP/raw.o" | python3 -c "
import sys, glob, subprocess
und = {t[-1] for t in (l.split() for l in sys.stdin)
       if t and t[-1].startswith('_sub_')}
defined = set()
for o in glob.glob('boot_cache/*.o'):
    if o.endswith('stub_impl.o'):      # the object we are rebuilding
        continue
    out = subprocess.run(['nm', '-g', '--defined-only', o],
                         capture_output=True, text=True).stdout
    defined |= {t[-1] for t in (l.split() for l in out.splitlines())
                if t and t[-1].startswith('_sub_')}
missing = sorted(und - defined)
print(' '.join('0x' + m[5:] for m in missing
               if m[5:].isalnum() and 0x401000 <= int(m[5:], 16) <= 0xA31BB0))
")
    [ -z "$NEW" ] && { echo "[stubs] closure reached on pass $pass"; break; }
    echo "[stubs] pass $pass: $(echo $NEW | wc -w | tr -d ' ') callee(s) exist in no object: $NEW"
    for v in $NEW; do echo "$v" >> "$TMP/vas.txt"; done
done

# --redefine-syms takes "old new" per line; build it from what actually LIFTED,
# not from what we asked for — lift_extra skips failures, and renaming a symbol
# the object does not define would silently produce no override at all.
"$L/llvm-nm" --defined-only "$TMP/raw.o" \
  | python3 -c "
import sys
vas = []
for line in sys.stdin:
    t = line.split()
    if t and t[-1].startswith('_sub_') and t[-1][5:].isalnum():
        try: int(t[-1][5:], 16)
        except ValueError: continue
        vas.append(t[-1])
vas = sorted(set(vas))
with open('$TMP/rename.txt', 'w') as f:
    for v in vas: f.write(f'{v} {v}_impl\n')
with open('$BC/stub_impl_pairs.txt', 'w') as f:
    for v in vas: f.write(f'{v}={v}_impl\n')
print(f'[stubs] {len(vas)} bodies renamed to _impl')
"
"$L/llvm-objcopy" --redefine-syms="$TMP/rename.txt" "$TMP/raw.o" "$BC/stub_impl.o"
cp "$TMP/vas.txt" "$CACHE"
echo "[stubs] wrote $BC/stub_impl.o and $BC/stub_impl_pairs.txt"
echo "[stubs] build_only.sh links the object and feeds the pairs to patch_redirect"
