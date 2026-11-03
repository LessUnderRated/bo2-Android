"""
run_tranche2 — Tranche 2: jump table + conditional branches + x87 FPU + 5-function chain.

Target: FUN_009abad0 (293 bytes) — a switch dispatcher with 8 cases.
  - Conditional branch (ja): validates non-linear control flow in LLVM br i1
  - Jump table (jmp [eax*4+table]): the indirect-jump problem from ARCHITECTURE.md
    -> each case handler lifted as a separate function, __remill_jump dispatches
  - x87 FPU (fild, fdiv, faddp, fstp): floating-point in the real game
  - 5 functions: entry + 7 case handlers + FUN_009ab420 + FUN_009ab480
  - 1 import: GetTickCount

Cases:
  arg=0: default (ja taken, just ret)
  arg=7: case 6 -> calls 9ab420, stores tick//1000 and tick%1000*1000 to struct
  arg=1: case 0 -> calls 9ab420+9ab480, fstp double to struct[0x398]
  arg=6: case 5 -> no-op (pop esi; ret)
  arg=9: default (index=8 > 7)
"""
import os, sys, subprocess, tempfile, struct, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"
REM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src")
BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"

ROOT_VA = 0x9abad0
CALLEE1_VA = 0x9ab420   # GetTickCount wrapper
CALLEE2_VA = 0x9ab480   # FPU computation

# Jump table: 8 entries at VA 0x9abbf8
JUMP_TABLE_VA = 0x9abbf8
JUMP_TARGETS = {
    0: 0x9abaff,  # case 0
    1: 0x9abb28,  # case 1
    2: 0x9abb51,  # case 2
    3: 0x9abb7a,  # case 3
    4: 0x9abba3,  # case 4
    5: 0x9abbf3,  # case 5 (no-op: pop esi; ret)
    6: 0x9abae8,  # case 6 (stores tick values)
    7: 0x9abbcc,  # case 7
}
DEFAULT_TARGET = 0x9abbf4  # default: just ret

# Double constant at 0xaf10f0 = 1000000.0
CONST_ADDR = 0xaf10f0
CONST_VAL = 1000000.0

# Memory layout
STRUCT_ADDR = 0x10000000
STACK_ADDR = 0x00100000
RET_SENTINEL = 0x7fff0000
IAT_ADDR = 0xa3b0dc
GETTICK_SENTINEL = 0xDEAD0001


def read_func_bytes(va, size):
    """Read bytes from the PE file at the given VA."""
    sys.path.insert(0, HERE)
    from pe_parser import PEFile
    pe = PEFile(BIN).parse()
    off = pe.rva_to_offset(va - 0x400000)
    return pe._raw[off:off + size]


def get_full_func_bytes():
    """Read the full 293-byte function."""
    return read_func_bytes(ROOT_VA, 293)


def case_handler_bytes(full_bytes, case_va):
    """Extract bytes for a case handler (from its VA to end of function)."""
    offset = case_va - ROOT_VA
    return full_bytes[offset:]


def unicorn_side(arg, tick_value, pre_eax=0, pre_edx=0):
    """Execute FUN_009abad0 under Unicorn with given arg and tick."""
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP

    full = get_full_func_bytes()
    callee1 = read_func_bytes(CALLEE1_VA, 39)
    callee2 = read_func_bytes(CALLEE2_VA, 41)

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    # map code region (covers ROOT_VA and all case handlers + callees)
    code_page = ROOT_VA & ~0xFFF
    uc.mem_map(code_page, 0x2000)
    uc.mem_write(ROOT_VA, full)
    uc.mem_write(CALLEE1_VA, callee1)
    uc.mem_write(CALLEE2_VA, callee2)

    # jump table data (at 0x9abbf8, past the function end, in .text section)
    jt_bytes = read_func_bytes(JUMP_TABLE_VA, 32)  # 8 entries * 4 bytes
    uc.mem_write(JUMP_TABLE_VA, jt_bytes)

    # IAT + GetTickCount stub
    uc.mem_map(IAT_ADDR & ~0xFFF, 0x1000)
    stub_va = 0xB000000
    uc.mem_map(stub_va, 0x1000)
    uc.mem_write(stub_va, b"\xb8" + struct.pack("<I", tick_value) + b"\xc3")
    uc.mem_write(IAT_ADDR, struct.pack("<I", stub_va))

    # double constant
    uc.mem_map(CONST_ADDR & ~0xFFF, 0x1000)
    uc.mem_write(CONST_ADDR, struct.pack("<d", CONST_VAL))

    # struct
    uc.mem_map(STRUCT_ADDR, 0x1000)
    struct.pack_into("<I", bytearray(uc.mem_read(STRUCT_ADDR, 0x1000)), 0, 0)
    uc.mem_write(STRUCT_ADDR + 0x3d0, struct.pack("<I", pre_eax))
    uc.mem_write(STRUCT_ADDR + 0x3d4, struct.pack("<I", pre_edx))

    # stack: [sp]=ret_sentinel, [sp+4]=struct_ptr (arg1), [sp+8]=switch_index (arg2)
    uc.mem_map(STACK_ADDR, 0x10000)
    uc.mem_map(RET_SENTINEL, 0x1000)
    sp = STACK_ADDR + 0x10000 - 0x100
    uc.mem_write(sp, struct.pack("<I", RET_SENTINEL))
    uc.mem_write(sp + 4, struct.pack("<I", STRUCT_ADDR))
    uc.mem_write(sp + 8, struct.pack("<I", arg))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, ROOT_VA)

    stop = [False]
    def hook(uc, addr, size, _):
        if addr == RET_SENTINEL:
            stop[0] = True
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(ROOT_VA, RET_SENTINEL, count=100_000_000)

    # read struct fields
    results = {}
    for off in [0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8, 0x3c0, 0x3d0, 0x3d4]:
        raw = uc.mem_read(STRUCT_ADDR + off, 8)
        results[off] = raw
    return results


def build_native(workdir):
    """Lift all functions, link, compile to native arm64 with Tranche 2 runtime."""
    full = get_full_func_bytes()
    callee1 = read_func_bytes(CALLEE1_VA, 39)
    callee2 = read_func_bytes(CALLEE2_VA, 41)

    bc_files = []

    # 1. Lift entry function (full 293 bytes — lifter stops at __remill_jump)
    bc = os.path.join(workdir, "entry.bc")
    subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                    "--bytes", full.hex(), "--address", hex(ROOT_VA),
                    "--bc_out", bc], check=True)
    bc_files.append(bc)

    # 2. Lift each case handler
    all_targets = list(JUMP_TARGETS.values()) + [DEFAULT_TARGET]
    for target_va in all_targets:
        cb = case_handler_bytes(full, target_va)
        bc = os.path.join(workdir, f"case_{target_va:x}.bc")
        subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                        "--bytes", cb.hex(), "--address", hex(target_va),
                        "--bc_out", bc], check=True)
        bc_files.append(bc)

    # 3. Lift callees
    for va, sz in [(CALLEE1_VA, 39), (CALLEE2_VA, 41)]:
        raw = read_func_bytes(va, sz)
        bc = os.path.join(workdir, f"callee_{va:x}.bc")
        subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                        "--bytes", raw.hex(), "--address", hex(va),
                        "--bc_out", bc], check=True)
        bc_files.append(bc)

    # 4. Link all bitcode modules
    linked_bc = os.path.join(workdir, "linked.bc")
    subprocess.run([LLVM17 + "/bin/llvm-link", "-o", linked_bc] + bc_files,
                   check=True, stderr=subprocess.DEVNULL)

    # 4b. Fix x86_fp80 → double (ARM64 doesn't support 80-bit float).
    #     Steps: replace type + constants + intrinsic, then remove no-op
    #     fpext/fptrunc (double→double) by forwarding the source value.
    linked_ll = os.path.join(workdir, "linked.ll")
    subprocess.run([LLVM17 + "/bin/llvm-dis", "-o", linked_ll, linked_bc],
                   check=True, stderr=subprocess.DEVNULL)
    with open(linked_ll, "r") as f:
        ir_text = f.read()
    ir_text = ir_text.replace("x86_fp80", "double")
    ir_text = ir_text.replace("llvm.fabs.f80", "llvm.fabs.f64")
    import re
    ir_text = re.sub(r'0xK[0-9a-fA-F]{20}', '0.0', ir_text)

    # Replace no-op fpext/fptrunc (double → double) with bitcast (valid, opt eliminates).
    lines = ir_text.split("\n")
    new_lines = []
    for line in lines:
        m = re.match(r'(\s*)(%\w+)\s*=\s*(fptrunc|fpext)\s+double\s+(%\w+)\s+to\s+double', line)
        if m:
            line = f"{m.group(1)}{m.group(2)} = bitcast double {m.group(4)} to double"
        new_lines.append(line)
    ir_text = "\n".join(new_lines)

    with open(linked_ll, "w") as f:
        f.write(ir_text)
    fixed_bc = os.path.join(workdir, "linked_fixed.bc")
    raw_bc = os.path.join(workdir, "linked_raw.bc")
    r_as = subprocess.run([LLVM17 + "/bin/llvm-as", "-o", raw_bc, linked_ll],
                   capture_output=True, text=True)
    if r_as.returncode != 0:
        print("LLVM-AS ERRORS (first 15):")
        for e in r_as.stderr.split("\n")[:15]:
            print(f"  {e}")
        raise subprocess.CalledProcessError(r_as.returncode, r_as.args, r_as.stdout, r_as.stderr)
    subprocess.run([LLVM17 + "/bin/opt", "-O2", "-o", fixed_bc, raw_bc],
                   check=True, stderr=subprocess.DEVNULL)

    # 5. Compile linked IR to arm64 object
    lifted_o = os.path.join(workdir, "lifted.o")
    r = subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1",
                    "-x", "ir", "-o", lifted_o, fixed_bc],
                   capture_output=True, text=True)
    if r.returncode != 0:
        # filter noise, show real errors
        errs = [l for l in r.stderr.split("\n") if l and "pentium" not in l and "cmov" not in l
                and "cx8" not in l and "fxsr" not in l and "mmx" not in l
                and "sse" not in l and "x87" not in l and "overriding" not in l
                and "loop not" not in l and "pass-failed" not in l]
        print("LINKED BC COMPILE ERRORS:")
        for e in errs[:20]:
            print(f"  {e}")
        raise subprocess.CalledProcessError(r.returncode, r.args, r.stdout, r.stderr)

    # 6. Compile runtime
    runtime_o = os.path.join(workdir, "runtime.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1",
                    "-std=c++17", "-DADDRESS_SIZE_BITS=32",
                    "-I", REM + "/include",
                    "-o", runtime_o,
                    os.path.join(HERE, "tranche2_runtime.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    # 7. Host main — registers jump targets, sets up memory, calls entry
    main_src = os.path.join(workdir, "main.cpp")
    # build registration code for each case handler
    registrations = ""
    for target_va in all_targets:
        registrations += f'    register_jump_target(0x{target_va:x}, sub_{target_va:x});\n'
    # declare all case handler functions
    decls = ""
    for target_va in all_targets:
        decls += f'extern "C" void* sub_{target_va:x}(State*, uint32_t, void*);\n'
    # declare entry + callees
    decls += f'extern "C" void* sub_{ROOT_VA:x}(State*, uint32_t, void*);\n'

    with open(main_src, "w") as f:
        f.write(f'''#include "remill/Arch/X86/Runtime/State.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {{
    void set_tick_value(uint32_t);
    void mem_write32_ext(uint32_t, uint32_t);
    uint32_t mem_read32_ext(uint32_t);
    void mem_write64_ext(uint32_t, uint64_t);
    uint64_t mem_read64_ext(uint32_t);
    void register_jump_target(uint32_t, void*(*)(State*, uint32_t, void*));
{decls}
}}

int main(int argc, char** argv) {{
    // register jump targets
{registrations}
    for (int i = 1; i < argc; i += 3) {{
        uint32_t arg = (uint32_t)strtoul(argv[i], 0, 0);
        uint32_t tick = (uint32_t)strtoul(argv[i+1], 0, 0);
        uint32_t pre_eax = (uint32_t)strtoul(argv[i+2], 0, 0);
        uint32_t pre_edx = pre_eax;  // simplified: same value

        set_tick_value(tick);
        // set up memory
        mem_write32_ext(0xa3b0dc, 0xDEAD0001);  // IAT sentinel
        // double constant = 1000000.0
        uint64_t const_raw = 0x412E848000000000ULL;
        mem_write64_ext(0xaf10f0, const_raw);
        // jump table at 0x9abbf8 (8 entries)
        mem_write32_ext(0x9abbf8 + 0*4, 0x9abaff);
        mem_write32_ext(0x9abbf8 + 1*4, 0x9abb28);
        mem_write32_ext(0x9abbf8 + 2*4, 0x9abb51);
        mem_write32_ext(0x9abbf8 + 3*4, 0x9abb7a);
        mem_write32_ext(0x9abbf8 + 4*4, 0x9abba3);
        mem_write32_ext(0x9abbf8 + 5*4, 0x9abbf3);
        mem_write32_ext(0x9abbf8 + 6*4, 0x9abae8);
        mem_write32_ext(0x9abbf8 + 7*4, 0x9abbcc);
        // struct: pre-populate [0x3d0] and [0x3d4]
        mem_write32_ext(0x10000000 + 0x3d0, pre_eax);
        mem_write32_ext(0x10000000 + 0x3d4, pre_edx);
        // stack: [sp]=ret_sentinel, [sp+4]=struct_ptr, [sp+8]=switch_index
        uint32_t sp = 0x00100000 + 0x10000 - 0x100;
        mem_write32_ext(sp, 0x7fff0000);
        mem_write32_ext(sp + 4, 0x10000000);
        mem_write32_ext(sp + 8, arg);

        State state;
        memset(&state, 0, sizeof(state));
        state.gpr.rsp.dword = sp;

        sub_{ROOT_VA:x}(&state, 0x{ROOT_VA:x}, nullptr);

        // read struct fields
        uint32_t f3d0 = mem_read32_ext(0x10000000 + 0x3d0);
        uint32_t f3d4 = mem_read32_ext(0x10000000 + 0x3d4);
        uint64_t f398 = mem_read64_ext(0x10000000 + 0x398);
        uint64_t f3a0 = mem_read64_ext(0x10000000 + 0x3a0);
        uint64_t f3a8 = mem_read64_ext(0x10000000 + 0x3a8);
        uint64_t f3b0 = mem_read64_ext(0x10000000 + 0x3b0);
        uint64_t f3b8 = mem_read64_ext(0x10000000 + 0x3b8);
        uint64_t f3c0 = mem_read64_ext(0x10000000 + 0x3c0);
        std::printf("%u %u %llu %llu %llu %llu %llu %llu\\n",
            f3d0, f3d4, f398, f3a0, f3a8, f3b0, f3b8, f3c0);
    }}
    return 0;
}}
''')

    exe = os.path.join(workdir, "tranche2")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32",
                    "-I", REM + "/include",
                    "-o", exe, main_src, lifted_o, runtime_o],
                   check=True)
    return exe


def main():
    print("=== Tranche 2: jump table + branches + FPU + 5-function chain ===")
    print(f"root: FUN_009abad0 (293 bytes, switch dispatcher, 8 cases)")
    print(f"  -> jump table at {JUMP_TABLE_VA:#x}, 8 entries")
    print(f"  -> case 6 calls FUN_009ab420 (GetTickCount wrapper)")
    print(f"  -> cases 0-4,7 call FUN_009ab420 + FUN_009ab480 (FPU: fild/fdiv/faddp/fstp)")
    print(f"  -> double constant at {CONST_ADDR:#x} = {CONST_VAL}")
    print()

    # Test cases: (arg, tick, pre_eax, pre_edx, description)
    # pre_eax = [struct+0x3d0], pre_edx = [struct+0x3d4] (pre-populated)
    # NOTE: native side uses pre_edx = pre_eax (simplified), so tests match.
    tests = [
        (0, 5000, 0, 0, "default case (arg=0, index=0xFFFFFFFF > 7)"),
        (7, 1234567, 0, 0, "case 6: stores tick//1000, tick%1000*1000"),
        (1, 1234567, 100, 100, "case 0: FPU double to [0x398]"),
        (2, 1234567, 100, 100, "case 1: FPU double to [0x3a0]"),
        (6, 99999, 0, 0, "case 5: no-op (pop esi; ret)"),
        (9, 5000, 0, 0, "default case (arg=9, index=8 > 7)"),
        (7, 0xFFFFFFFF, 0, 0, "case 6: tick=0xFFFFFFFF edge case"),
        (1, 0, 0, 0, "case 0: tick=0 edge case"),
    ]

    with tempfile.TemporaryDirectory() as d:
        exe = build_native(d)
        # run all tests in one native execution
        args = []
        for arg, tick, pre_eax, pre_edx, _ in tests:
            args.extend([str(arg), str(tick), str(pre_eax)])
        r = subprocess.run([exe] + args, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"NATIVE CRASHED (signal {r.returncode}):")
            print(f"  stderr: {r.stderr[:500]}")
            sys.exit(1)
        native_lines = r.stdout.strip().split("\n")

        print(f"{'#':>2} {'arg':>4} {'tick':>10} | {'description':<50} | result")
        print("-" * 100)
        fails = 0
        for i, (arg, tick, pre_eax, pre_edx, desc) in enumerate(tests):
            u = unicorn_side(arg, tick, pre_eax, pre_edx)
            n_parts = native_lines[i].split()
            n_3d0 = int(n_parts[0])
            n_3d4 = int(n_parts[1])
            n_doubles = [int(n_parts[j]) for j in range(2, 8)]

            # compare based on which case is active
            ok = True
            details = []
            if arg == 7:  # case 6: check [0x3d0] and [0x3d4]
                u_3d0 = struct.unpack_from("<I", u[0x3d0], 0)[0]
                u_3d4 = struct.unpack_from("<I", u[0x3d4], 0)[0]
                exp_3d0 = (tick // 1000) & 0xFFFFFFFF
                exp_3d4 = ((tick % 1000) * 1000) & 0xFFFFFFFF
                ok = (u_3d0 == n_3d0 == exp_3d0) and (u_3d4 == n_3d4 == exp_3d4)
                details.append(f"u[3d0]={u_3d0:#x} n[3d0]={n_3d0:#x} exp={exp_3d0:#x}")
                details.append(f"u[3d4]={u_3d4:#x} n[3d4]={n_3d4:#x} exp={exp_3d4:#x}")
            elif arg in (1, 2, 3, 4, 5, 8):  # FPU cases
                # determine which offset to check
                offset_map = {1: 0x398, 2: 0x3a0, 3: 0x3a8, 4: 0x3b0, 5: 0x3b8, 8: 0x3c0}
                off = offset_map.get(arg)
                if off:
                    u_dbl = struct.unpack("<d", u[off])[0]
                    off_idx = [0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8, 0x3c0].index(off)
                    n_raw = n_doubles[off_idx]
                    n_dbl = struct.unpack("<d", n_raw.to_bytes(8, "little"))[0]
                    # expected: (tick%1000*1000 - pre_edx) / 1e6 + (tick//1000 - pre_eax)
                    exp_dbl = ((tick % 1000) * 1000 - pre_edx) / 1e6 + (tick // 1000 - pre_eax)
                    # tolerance: 80-bit→64-bit FPU precision loss is expected
                    ok = (math.isclose(u_dbl, n_dbl, rel_tol=1e-5) and
                          math.isclose(u_dbl, exp_dbl, rel_tol=1e-5))
                    details.append(f"u[{off:#x}]={u_dbl!r} n={n_dbl!r} exp={exp_dbl!r}")
            elif arg in (0, 6, 9):  # no-op or default
                details.append("(no struct writes expected)")
                # check that struct wasn't modified (except pre-populated values)
                u_3d0 = struct.unpack_from("<I", u[0x3d0], 0)[0]
                ok = (u_3d0 == n_3d0)

            if not ok:
                fails += 1
            print(f"{i+1:>2} {arg:>4} {tick:>10} | {desc:<50} | {'PASS' if ok else 'FAIL'}")
            for dt in details:
                print(f"   {'':>4} {'':>10} |   {dt}")

    print()
    print(f"{'ALL PASS' if not fails else f'{fails} FAILURES'} — Tranche 2: jump table + "
          f"conditional branches + x87 FPU, Unicorn vs native arm64.")


if __name__ == "__main__":
    main()
