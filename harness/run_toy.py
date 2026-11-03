#!/usr/bin/env python3
"""
run_toy — differential harness proof-of-life on a synthetic target.

Proves the harness methodology end-to-end, no T6 binary and no Remill needed:
  1. OrigRunner executes hand-assembled x86-32 bytes under Unicorn.
  2. RecompRunner calls a native arm64 C equivalent (compiled to a dylib).
  3. compare() diffs full architectural state.
  4. Reports PASS/FAIL over a small input vector.

A PASS here means the differential harness principle is sound before T6
complexity enters. This is the foundation described in docs/HARNESS.md.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from orig_runner import OrigRunner, State
from recomp_runner import RecompRunner, build_dylib
from compare import compare

HERE = os.path.dirname(os.path.abspath(__file__))

# Hand-assembled x86-32 (see toy.c for the C equivalent):
#   C1 E0 03       shl  eax, 3
#   6B DB 07       imul ebx, ebx, 7
#   01 D8          add  eax, ebx
#   83 C0 01       add  eax, 1
#   C3             ret
TOY_CODE = bytes.fromhex("C1E0036BDB0701D883C001C3")


def run_once(orig: OrigRunner, recomp: RecompRunner, a: int, b: int):
    # --- original side (Unicorn x86-32) ---
    orig.load_code(TOY_CODE)
    orig.set_arg_regs({"eax": a, "ebx": b, "eip": orig.code_addr})
    orig.setup_ret()
    orig.run()
    o_state = orig.snapshot()

    # --- recomp side (native arm64 via ctypes) ---
    eax_out = recomp.call(a, b)
    r_state = recomp.to_state(a, b, eax_out)

    # --- compare ---
    # Toy scope: result reg (eax) + the reg clobbered by `imul ebx, ebx, 7`.
    # esp/eip/eflags are harness-frame state, not comparable against a ctypes
    # call. In the real harness both sides are full emulations and the full
    # GPR + eip/esp/eflags set is in-scope.
    return compare(o_state, r_state, fields={"eax", "ebx"}), o_state, r_state


def main():
    # Build the native "recomp" dylib once.
    dylib = build_dylib(os.path.join(HERE, "toy.c"), HERE, "toy")
    recomp = RecompRunner(dylib_path=dylib, func_name="toy")

    inputs = [(5, 9), (0, 0), (1, 1), (100, -7), (-3, 13), (0x7FFFFFFF, 0)]
    all_pass = True
    for a, b in inputs:
        orig = OrigRunner()
        res, o_state, r_state = run_once(orig, recomp, a, b)
        expected = ((a << 3) + (b * 7) + 1) & 0xFFFFFFFF
        eax = o_state.regs["eax"]
        tag = "PASS" if res.passed else "FAIL"
        if not res.passed:
            all_pass = False
        print(f"[{tag}] toy({a}, {b})  orig.eax={eax:#010x} expected={expected:#010x}  {res.report()}")

    print("\n" + ("ALL PASS — harness methodology validated on toy target."
                  if all_pass else "FAILURES — see diffs above."))
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
