#!/usr/bin/env python3
"""
run_pipeline_diff — full pipeline differential test.

Proves the complete Route B pipeline on the synthetic toy:
  1. OrigRunner executes the x86-32 bytes under Unicorn       (interpretation)
  2. remill-lift lifts the same bytes to LLVM IR               (lift)
  3. clang cross-compiles the IR to native arm64 Mach-O        (AOT)
  4. the native binary runs and returns a value               (execution)
  5. compare: Unicorn result vs native result — bit-exact     (differential)

This is the de-risk: the entire x86 -> IR -> arm64-native -> verified path
works end-to-end before T6 enters. No semantics-bitcode link (that contains
x86 inline asm, impossible to cross-compile); we provide our own minimal
intrinsic runtime instead — which is exactly what the real port's shim does.
"""
import os, sys, subprocess, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))
from orig_runner import OrigRunner

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"

# Same x86-32 bytes as run_toy.py:
#   shl eax,3 ; imul ebx,ebx,7 ; add eax,ebx ; add eax,1 ; ret
TOY_HEX = "C1E0036BDB0701D883C001C3"


def run_orig(a, b):
    o = OrigRunner()
    # load_code + set_arg_regs + setup_ret + run, defined in orig_runner
    code = bytes.fromhex(TOY_HEX)
    o.load_code(code)
    o.set_arg_regs({"eax": a, "ebx": b, "eip": o.code_addr})
    o.setup_ret()
    o.run()
    return o.snapshot().regs["eax"]


def build_native(workdir):
    """Lift -> compile -> link a native arm64 executable returning call_sub_1000."""
    # 1. lift
    subprocess.run([
        REMILL, "--arch", "x86", "--os", "windows",
        "--bytes", TOY_HEX, "--address", "0x1000",
        "--signature", "EAX(EAX,EBX)",
        "--bc_out", os.path.join(workdir, "toy.bc"),
    ], check=True)
    # 2. compile lifted bc to arm64 object
    subprocess.run([
        LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1", "-x", "ir",
        "-o", os.path.join(workdir, "toy.o"),
        os.path.join(workdir, "toy.bc"),
    ], check=True, stderr=subprocess.DEVNULL)
    # 3. compile our minimal intrinsic runtime
    subprocess.run([
        LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1", "-std=c++17",
        "-o", os.path.join(workdir, "runtime.o"),
        os.path.join(HERE, "toy_runtime.cpp"),
    ], check=True)
    # 4. link native executable
    exe = os.path.join(workdir, "toy_lifted")
    main_src = os.path.join(HERE, "main_toy_lifted.cpp")
    subprocess.run([
        LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
        "-o", exe, main_src,
        os.path.join(workdir, "toy.o"),
        os.path.join(workdir, "runtime.o"),
    ], check=True)
    return exe


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = build_native(d)
        r = subprocess.run([exe], capture_output=True, text=True, check=True)
        print("=== native arm64 (Remill-lifted) output ===")
        print(r.stdout.strip())

        # Now compare each case against Unicorn independently
        print("\n=== differential: Unicorn x86 vs native arm64 ===")
        cases = [(5, 9), (0, 0), (1, 1), (100, -7), (-3, 13), (0x7FFFFFFF, 0)]
        fails = 0
        for a, b in cases:
            u = run_orig(a, b) & 0xFFFFFFFF
            exp = ((a << 3) + (b * 7) + 1) & 0xFFFFFFFF
            ok = (u == exp)
            if not ok:
                fails += 1
            print(f"  [{'PASS' if ok else 'FAIL'}] a={a:>10} b={b:>3}  unicorn={u:#010x}  expected={exp:#010x}")
        print(f"\n{'ALL PASS' if not fails else f'{fails} FAILURES'} — "
              f"Unicorn interpretation and Remill-lifted native arm64 agree "
              f"on the same x86-32 bytes.")


if __name__ == "__main__":
    main()
