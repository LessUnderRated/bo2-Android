"""
run_tranche1 — Tranche 1 differential test on a REAL 2-function chain from t6sp.exe.

Chain: FUN_009abc20 (34 bytes) -> calls -> FUN_009ab420 (39 bytes) -> calls -> GetTickCount (import)

FUN_009abc20: reads struct ptr from [esp+8], writes 0 to [ptr+0x438],
              calls FUN_009ab420, stores eax (=tick//1000) to [ptr+0x3c8],
              stores edx (=tick%1000*1000) to [ptr+0x3cc], ret
FUN_009ab420: calls GetTickCount, computes eax=tick//1000, edx=(tick%1000)*1000, ret

New Tranche 1 challenges vs Tranche 0:
  1. Internal direct call (call 0x9ab420) -> Remill emits call @sub_9ab420
  2. Memory writes (struct fields) -> need a real memory model
  3. Stack argument ([esp+8]) -> need a valid stack with the struct ptr
  4. Indirect import call via IAT -> __remill_function_call dispatch

Differential: Unicorn executes the full chain with GetTickCount hooked;
native arm64 executes the lifted+linked chain. Compare the 3 struct writes.
"""
import os, sys, subprocess, tempfile, struct
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"

CALLER_VA = 0x9abc20
CALLEE_VA = 0x9ab420
CALLER_BYTES = bytes.fromhex(
    "568b742408c7863804000000000000e8ecf7ffff8986c80300008996cc0300005ec3")
CALLEE_BYTES = bytes.fromhex(
    "ff15dcb0a3008bc8b8d34d6210f7e18bc2c1e8068bd069d2e80300002bca69c9e80300008bd1c3")

IAT_GETTICK_ADDR = 0xa3b0dc   # the IAT slot address used by `call [0xa3b0dc]`
GETTICK_SENTINEL = 0xDEAD0001  # sentinel placed in IAT slot for native dispatch

STRUCT_ADDR = 0x10000000       # where we place the struct buffer
STACK_ADDR  = 0x00100000       # stack base
RET_SENTINEL = 0x7fff0000      # return sentinel address


def unicorn_side(tick_value):
    """Execute the full chain under Unicorn with GetTickCount hooked."""
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import (
        UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_ESP, UC_X86_REG_EIP)

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    # map code: both functions in one page region
    code_page = CALLER_VA & ~0xFFF
    uc.mem_map(code_page, 0x2000)
    uc.mem_write(CALLER_VA, CALLER_BYTES)
    uc.mem_write(CALLEE_VA, CALLEE_BYTES)
    # IAT + GetTickCount stub
    uc.mem_map(IAT_GETTICK_ADDR & ~0xFFF, 0x1000)
    stub_va = 0xB000000
    uc.mem_map(stub_va, 0x1000)
    uc.mem_write(stub_va, b"\xb8" + struct.pack("<I", tick_value) + b"\xc3")
    uc.mem_write(IAT_GETTICK_ADDR, struct.pack("<I", stub_va))
    # struct buffer
    uc.mem_map(STRUCT_ADDR, 0x1000)
    # stack
    uc.mem_map(STACK_ADDR, 0x10000)
    uc.mem_map(RET_SENTINEL, 0x1000)
    sp = STACK_ADDR + 0x10000 - 0x100
    # [sp] = return sentinel, [sp+4] = struct_ptr (first arg)
    uc.mem_write(sp, struct.pack("<I", RET_SENTINEL))
    uc.mem_write(sp + 4, struct.pack("<I", STRUCT_ADDR))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, CALLER_VA)

    stop = [False]
    def hook(uc, addr, size, _):
        if addr == RET_SENTINEL:
            stop[0] = True
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(CALLER_VA, RET_SENTINEL, count=100_000_000)

    # read the 3 struct fields written by FUN_009abc20
    field_438 = struct.unpack("<I", uc.mem_read(STRUCT_ADDR + 0x438, 4))[0]
    field_3c8 = struct.unpack("<I", uc.mem_read(STRUCT_ADDR + 0x3c8, 4))[0]
    field_3cc = struct.unpack("<I", uc.mem_read(STRUCT_ADDR + 0x3cc, 4))[0]
    eax = uc.reg_read(UC_X86_REG_EAX)
    edx = uc.reg_read(UC_X86_REG_EDX)
    return field_438, field_3c8, field_3cc, eax, edx


def build_native(workdir):
    """Lift caller + callee, link, compile to native arm64 with Tranche 1 runtime."""
    # 1. lift both functions as raw blocks (no signature)
    caller_bc = os.path.join(workdir, "caller.bc")
    callee_bc = os.path.join(workdir, "callee.bc")
    subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                    "--bytes", CALLER_BYTES.hex(),
                    "--address", hex(CALLER_VA),
                    "--bc_out", caller_bc], check=True)
    subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                    "--bytes", CALLEE_BYTES.hex(),
                    "--address", hex(CALLEE_VA),
                    "--bc_out", callee_bc], check=True)
    # 2. link the two bitcode modules (caller calls @sub_9ab420)
    linked_bc = os.path.join(workdir, "linked.bc")
    subprocess.run([LLVM17 + "/bin/llvm-link", "-o", linked_bc,
                    caller_bc, callee_bc], check=True,
                   stderr=subprocess.DEVNULL)
    # 3. compile linked IR to arm64 object
    lifted_o = os.path.join(workdir, "lifted.o")
    subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1",
                    "-x", "ir", "-o", lifted_o, linked_bc],
                   check=True, stderr=subprocess.DEVNULL)
    # 4. compile runtime
    runtime_o = os.path.join(workdir, "runtime.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1",
                    "-std=c++17", "-DADDRESS_SIZE_BITS=32",
                    "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/include"),
                    "-o", runtime_o,
                    os.path.join(HERE, "tranche1_runtime.cpp")],
                   check=True, stderr=subprocess.DEVNULL)
    # 5. host main
    main_src = os.path.join(workdir, "main.cpp")
    with open(main_src, "w") as f:
        f.write(r'''#include "remill/Arch/X86/Runtime/State.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {
    void* sub_9abc20(State*, uint32_t, void*);
    void set_tick_value(uint32_t);
    void mem_write32_ext(uint32_t, uint32_t);
    uint32_t mem_read32_ext(uint32_t);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        uint32_t tick = (uint32_t)strtoul(argv[i], 0, 0);
        set_tick_value(tick);
        // set up State
        State state;
        memset(&state, 0, sizeof(state));
        // IAT slot: write the GetTickCount sentinel
        mem_write32_ext(0xa3b0dc, 0xDEAD0001);
        // stack: [sp]=ret_sentinel, [sp+4]=struct_ptr
        uint32_t sp = 0x00100000 + 0x10000 - 0x100;
        mem_write32_ext(sp, 0x7fff0000);
        mem_write32_ext(sp + 4, 0x10000000);
        state.gpr.rsp.dword = sp;
        // call the lifted caller function
        sub_9abc20(&state, 0x9abc20, nullptr);
        // read results from struct
        uint32_t f438 = mem_read32_ext(0x10000000 + 0x438);
        uint32_t f3c8 = mem_read32_ext(0x10000000 + 0x3c8);
        uint32_t f3cc = mem_read32_ext(0x10000000 + 0x3cc);
        std::printf("%u %u %u %u %u\n",
                    f438, f3c8, f3cc,
                    state.gpr.rax.dword, state.gpr.rdx.dword);
    }
    return 0;
}
''')
    exe = os.path.join(workdir, "tranche1")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32",
                    "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/include"),
                    "-o", exe, main_src, lifted_o, runtime_o],
                   check=True)
    return exe


def main():
    print("=== Tranche 1: REAL 2-function chain from t6sp.exe ===")
    print(f"caller:  FUN_009abc20 ({len(CALLER_BYTES)} bytes)")
    print(f"  -> calls FUN_009ab420 ({len(CALLEE_BYTES)} bytes)")
    print(f"      -> calls GetTickCount (import)")
    print(f"semantics: struct[0x438]=0, struct[0x3c8]=tick//1000, struct[0x3cc]=tick%1000*1000")
    print()

    ticks = [0, 1000, 999, 1001, 5000, 1234567, 0xFFFFFFFF, 42, 0x7FFFFFFF, 0x80000000]

    with tempfile.TemporaryDirectory() as d:
        exe = build_native(d)
        tick_strs = [str(t) for t in ticks]
        r = subprocess.run([exe] + tick_strs, capture_output=True, text=True, check=True)
        native_lines = r.stdout.strip().split("\n")

        print(f"{'tick':>12} | {'field':>6} | {'unicorn':>12} {'native':>12} | result")
        print("-" * 75)
        fails = 0
        for i, tick in enumerate(ticks):
            u_438, u_3c8, u_3cc, u_eax, u_edx = unicorn_side(tick)
            n_parts = list(map(int, native_lines[i].split()))
            n_438, n_3c8, n_3cc, n_eax, n_edx = n_parts

            exp_438 = 0
            exp_3c8 = (tick // 1000) & 0xFFFFFFFF
            exp_3cc = ((tick % 1000) * 1000) & 0xFFFFFFFF

            ok = (u_438 == n_438 == exp_438 and
                  u_3c8 == n_3c8 == exp_3c8 and
                  u_3cc == n_3cc == exp_3cc)
            if not ok:
                fails += 1
            status = 'PASS' if ok else 'FAIL'
            print(f"{tick:>12} | 0x438  | {u_438:#010x} {n_438:#010x} | {status}")
            print(f"{'':>12} | 0x3c8  | {u_3c8:#010x} {n_3c8:#010x} | {status}")
            print(f"{'':>12} | 0x3cc  | {u_3cc:#010x} {n_3cc:#010x} | {status}")

        print()
        print(f"{'ALL PASS' if not fails else f'{fails} FAILURES'} — Tranche 1: real 2-function chain, "
              f"Unicorn vs Remill-lifted native arm64, bit-exact.")


if __name__ == "__main__":
    main()
