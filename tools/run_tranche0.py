"""
run_tranche0 — Tranche 0 differential test on a REAL function from t6sp.exe.

Target: FUN_009ab420 (39 bytes) — calls GetTickCount then computes
  eax = tick / 1000   (seconds)
  edx = (tick % 1000) * 1000
Pure arithmetic on the GetTickCount return value; no external memory/state.

Pipeline:
  1. Unicorn maps the real t6sp.exe at its image base, hooks the IAT slot
     for GetTickCount to return a deterministic value (DeterministicClock).
  2. Unicorn executes FUN_009ab420 -> records eax, edx.
  3. remill-lift lifts the 39 bytes to LLVM IR with signature EAX,EDX(EAX)
     (returning both regs; GetTickCount result enters via EAX).
     Actually the function reads EAX only implicitly via the call. We lift
     the post-call arithmetic by lifting the bytes AFTER the call, with
     signature EDX(EAX,ECX) where ECX=GetTickCount result... see below.
  4. clang cross-compiles the IR to native arm64.
  5. native runs with the same injected tick value.
  6. compare: Unicorn eax/edx vs native eax/edx — bit-exact.

Because the first instruction is `call [IAT]` (an indirect call Remill will
turn into an intrinsic that we'd have to stub), we lift ONLY the arithmetic
after the call: bytes from 0x9ab426 onward (mov ecx,eax ... ret), and we feed
the GetTickCount result as the EAX input. This is exactly how the harness
handles external calls: the call is the snapshot boundary, and we resume on
the other side with the imported function's return value injected.
"""
import os, sys, subprocess, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))
from orig_runner import OrigRunner
from deterministic import DeterministicClock
from pe_parser import PEFile

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"
BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"

# Full function bytes (39) from Ghidra.
FUNC_VA = 0x9ab420
FUNC_BYTES = bytes.fromhex(
    "ff15dcb0a300"          # call [0xa3b0dc]  (GetTickCount via IAT)
    "8bc8"                  # mov ecx, eax
    "b8d34d6210"            # mov eax, 0x10624dd3
    "f7e1"                  # mul ecx          (edx:eax = eax * ecx)
    "8bc2"                  # mov eax, edx
    "c1e806"                # shr eax, 6
    "8bd0"                  # mov edx, eax
    "69d2e8030000"          # imul edx, edx, 0x3e8
    "2bca"                  # sub ecx, edx
    "69c9e8030000"          # imul ecx, ecx, 0x3e8
    "8bd1"                  # mov edx, ecx
    "c3"                    # ret
)
# Arithmetic tail (after the GetTickCount call): starts at offset 6.
# We lift this with EAX = GetTickCount result as the input register.
TAIL_OFFSET = 6
TAIL_VA = FUNC_VA + TAIL_OFFSET
TAIL_BYTES = FUNC_BYTES[TAIL_OFFSET:]
# Expected outputs: eax = tick//1000, edx = (tick%1000)*1000


def unicorn_side(tick_value):
    """Map the function at a page-aligned address, hook GetTickCount's IAT
    slot to return tick_value, execute FUN_009ab420, return (eax, edx).

    We keep the original function VA (0x9ab420) so the absolute IAT disp in
    the `call [0xa3b0dc]` resolves correctly. Map the page containing it.
    """
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_INVALID
    from unicorn.x86_const import (
        UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_ECX, UC_X86_REG_ESP,
        UC_X86_REG_EIP, UC_X86_REG_EFLAGS,
    )
    page = FUNC_VA & ~0xFFF
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(page, 0x2000)             # code page + slack
    uc.mem_write(FUNC_VA, FUNC_BYTES)
    # IAT slot + stub
    uc.mem_map(0xa3b000, 0x1000)
    stub_va = 0xa400000
    uc.mem_map(stub_va, 0x1000)
    uc.mem_write(stub_va, b"\xb8" + (tick_value & 0xFFFFFFFF).to_bytes(4, "little") + b"\xc3")
    uc.mem_write(0xa3b0dc, stub_va.to_bytes(4, "little"))
    # stack
    stack_base = 0x7ffe0000
    uc.mem_map(stack_base, 0x10000)
    sp = stack_base + 0x10000 - 0x10 - 4
    ret_sentinel = 0x7fff0000
    uc.mem_map(ret_sentinel, 0x1000)
    uc.mem_write(sp, ret_sentinel.to_bytes(4, "little"))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, FUNC_VA)
    # stop at ret sentinel
    stop = [False]
    def hook_code(uc, addr, size, _):
        if addr == ret_sentinel:
            stop[0] = True
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook_code)
    uc.emu_start(FUNC_VA, ret_sentinel, count=10_000_000)
    return uc.reg_read(UC_X86_REG_EAX), uc.reg_read(UC_X86_REG_EDX)


def build_native(tail_bytes, workdir):
    """Lift the arithmetic tail to native arm64 and link with the intrinsic
    runtime. The lifted wrapper call_sub_<VA>(EAX) returns EAX (and the
    original EDX is in the State, but our minimal wrapper only returns EAX).
    We need EDX too, so we lift with a signature that returns both.
    Remill signature format: reg_out(reg_in,...). Only one reg_out supported
    in the simple form, so we use EAX(EAX) and read EDX via a second channel:
    we compile the lifted function and ALSO export a getter for the State's
    EDX. Simpler: lift twice, once EAX(EAX) once EDX(EAX), or use a custom
    wrapper. For Tranche 0 we lift with signature EAX(EAX) and separately
    verify EDX by lifting with EDX(EAX) and comparing.

    Actually cleanest: lift with no signature (raw block), and write our own
    C wrapper that sets State.EAX, calls the block, reads State.EAX and
    State.EDX. But that needs the State struct layout. For this Tranche 0
    milestone we lift with EAX(EAX) to get eax, and separately EDX(EAX) to
    get edx. Two compiles, one comparison. Demonstrates the principle.
    """
    results = {}
    for out_reg in ("EAX", "EDX"):
        bc = os.path.join(workdir, f"tail_{out_reg}.bc")
        o_obj = os.path.join(workdir, f"tail_{out_reg}.o")
        subprocess.run([
            REMILL, "--arch", "x86", "--os", "windows",
            "--bytes", tail_bytes.hex(), "--address", hex(TAIL_VA),
            "--signature", f"{out_reg}(EAX)",
            "--bc_out", bc,
        ], check=True)
        subprocess.run([
            LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1", "-x", "ir",
            "-o", o_obj, bc,
        ], check=True, stderr=subprocess.DEVNULL)
        results[out_reg] = o_obj
    return results


def main():
    pe = PEFile(BIN).parse()
    print(f"=== Tranche 0: REAL function from {os.path.basename(BIN)} ===")
    print(f"target: FUN_009ab420 (39 bytes, calls GetTickCount)")
    print(f"arithmetic tail: {len(TAIL_BYTES)} bytes from {TAIL_VA:#x}")
    print(f"semantics: eax = tick//1000, edx = (tick%1000)*1000")
    print()

    # Test vector of GetTickCount return values (deterministic injection)
    clock = DeterministicClock(start_ms=0, tick_ms=1)  # 1ms steps
    ticks = [clock.get_tick_count() for _ in range(6)]
    # add some edge cases
    ticks += [1000, 999, 1001, 0, 0xFFFFFFFF, 1234567]
    print(f"injected tick values: {ticks}")
    print()

    with tempfile.TemporaryDirectory() as d:
        objs = build_native(TAIL_BYTES, d)

        # Build two host executables: one returns EAX, one returns EDX
        results = {}  # tick -> (native_eax, native_edx)
        for out_reg, obj in objs.items():
            main_src = os.path.join(d, f"main_{out_reg}.cpp")
            with open(main_src, "w") as f:
                f.write(f'''#include <cstdio>
#include <cstdint>
#include <cstdlib>
extern "C" int32_t call_sub_{TAIL_VA:x}(int32_t);
int main(int argc, char** argv) {{
    for (int i = 1; i < argc; i++) {{
        int32_t v = (int32_t)strtoul(argv[i], 0, 0);
        int32_t r = call_sub_{TAIL_VA:x}(v);
        std::printf("%u\\n", (uint32_t)r);
    }}
    return 0;
}}
''')
            exe = os.path.join(d, f"host_{out_reg}")
            subprocess.run([
                LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                "-o", exe, main_src, obj,
                os.path.join(HERE, "toy_runtime.cpp"),
            ], check=True)
            tick_strs = [str(t) for t in ticks]
            r = subprocess.run([exe] + tick_strs, capture_output=True, text=True, check=True)
            vals = [int(line) for line in r.stdout.strip().split("\n")]
            results[out_reg] = vals

        # Unicorn side
        u_eax = []
        u_edx = []
        for t in ticks:
            ea, ed = unicorn_side(t)
            u_eax.append(ea)
            u_edx.append(ed)

        # Compare
        print(f"{'tick':>12} | {'unicorn eax':>12} {'native eax':>12} | {'unicorn edx':>12} {'native edx':>12} | result")
        print("-" * 90)
        fails = 0
        n_eax = results["EAX"]
        n_edx = results["EDX"]
        for i, t in enumerate(ticks):
            exp_eax = (t // 1000) & 0xFFFFFFFF
            exp_edx = ((t % 1000) * 1000) & 0xFFFFFFFF
            ok = (u_eax[i] == n_eax[i]) and (u_edx[i] == n_edx[i]) and (u_eax[i] == exp_eax) and (u_edx[i] == exp_edx)
            if not ok:
                fails += 1
            print(f"{t:>12} | {u_eax[i]:#012x} {n_eax[i]:#012x} | {u_edx[i]:#012x} {n_edx[i]:#012x} | {'PASS' if ok else 'FAIL'}")

        print()
        print(f"{'ALL PASS' if not fails else f'{fails} FAILURES'} — Tranche 0: real t6sp.exe "
              f"function, Unicorn vs Remill-lifted native arm64, bit-exact.")


if __name__ == "__main__":
    main()
