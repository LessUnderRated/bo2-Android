#!/usr/bin/env python3
"""Replace a boot.py __stub_call_fatal STUB with a real lifted body.

boot.py emits a stub for every function it could not lift (or that fell outside
the lift set). A stub is a STRONG `_sub_<va>` inside lifted.o, so simply linking
a freshly lifted `func_<va>.o` collides instead of overriding, and every existing
`bl _sub_<va>` would keep hitting the stub even if the collision were resolved.

This wires up the override without re-splicing the whole 50 MB lifted.o:

  1. rename the symbol in boot_cache/lift_wip/func_<va>.o to `_sub_<va>_impl`,
     so it links alongside lifted.o instead of clashing with it;
  2. point boot_cache/main.cpp's extern + register_jump_target at `sub_<va>_impl`,
     so INDIRECT dispatch reaches the real body;
  3. print the `src=dst` pair for tools/patch_redirect.py, which rewrites the
     stub's entry post-link so DIRECT `bl` calls land on the real body too.

Usage:  stub_override.py <hex-va> [<hex-va> ...]
Emits one `_sub_<va>=_sub_<va>_impl` pair per line on stdout; reboot.sh feeds
those to patch_redirect.py (and must re-sign afterwards).
"""
import os, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
WIP = os.path.join(ROOT, "boot_cache", "lift_wip")
MAIN = os.path.join(ROOT, "boot_cache", "main.cpp")
OBJCOPY = "/opt/homebrew/opt/llvm@17/bin/llvm-objcopy"


def override(va):
    name, impl = f"sub_{va:x}", f"sub_{va:x}_impl"
    obj = os.path.join(WIP, f"func_{va:x}.o")
    if not os.path.exists(obj):
        print(f"  0x{va:x}: {obj} missing — lift it first", file=sys.stderr)
        return None

    # 1. rename in the object so it no longer collides with lifted.o's stub
    r = subprocess.run([OBJCOPY, f"--redefine-sym=_{name}=_{impl}", obj],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  0x{va:x}: objcopy failed: {r.stderr[:200]}", file=sys.stderr)
        return None

    # 2. repoint main.cpp's declaration + registration at the real body
    src = open(MAIN).read()
    if f"{impl}(" not in src:
        src = src.replace(f'extern "C" void* {name}(State*, uint32_t, void*);',
                          f'extern "C" void* {impl}(State*, uint32_t, void*);')
        src = src.replace(f"register_jump_target(0x{va:x}, {name});",
                          f"register_jump_target(0x{va:x}, {impl});")
        open(MAIN, "w").write(src)

    print(f"  0x{va:x}: {name} -> {impl}", file=sys.stderr)
    return f"_{name}=_{impl}"


if __name__ == "__main__":
    pairs = [p for p in (override(int(a, 16)) for a in sys.argv[1:]) if p]
    print("\n".join(pairs))
