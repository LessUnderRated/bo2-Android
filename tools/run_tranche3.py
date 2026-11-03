"""
run_tranche3 — Tranche 3: multiple imports + security cookie + thunk jump.

Target: FUN_008f9500 (137 bytes) — process memory info query.
  - 5 KERNEL32 imports: GlobalMemoryStatus, GetCurrentProcess,
    GetCurrentProcessId, OpenProcess, CloseHandle
  - 1 PSAPI import via thunk: GetProcessMemoryInfo (jmp [IAT] at 0x9860c6)
  - __security_check_cookie: reads global cookie at 0xea8a70, XORs with ESP,
    checks at function exit → validates global memory read + CRT pattern
  - 2 conditional branches (je, jbe)
  - 5 functions in the call tree

New Tranche 3 challenges:
  1. 6 different imports with different signatures/return values
  2. Global memory read (security cookie at 0xea8a70)
  3. Thunk jump (GetProcessMemoryInfo at 0x9860c6 = jmp [0xa3b344])
  4. __security_check_cookie function (15 bytes, reads cookie, branches)
  5. Multi-level call: root -> thunk -> import

The function computes: returns -1 on error, or (initial_pid - process_id)
on success, via GetProcessMemoryInfo's WorkingSetSize field.
"""
import os, sys, subprocess, tempfile, struct, re
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"
REM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src")
BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"

ROOT_VA = 0x8f9500
THUNK_VA = 0x9860c6      # GetProcessMemoryInfo thunk: jmp [0xa3b344]
SCC_VA = 0x95f655        # __security_check_cookie
GSFAIL_VA = 0x96f2ef     # ___report_gsfailure
HELPER_VA = 0x982405     # FUN_00982405 (8 bytes: clears a global)

# IAT slots
IAT_GLOBAL_MEM_STATUS = 0xa3b214
IAT_GET_CURRENT_PROCESS = 0xa3b128
IAT_GET_CURRENT_PROCESS_ID = 0xa3b0fc
IAT_OPEN_PROCESS = 0xa3b280
IAT_CLOSE_HANDLE = 0xa3b0c8
IAT_GET_PROCESS_MEM_INFO = 0xa3b344

# Sentinels for each import (placed in IAT slots)
SENTINELS = {
    IAT_GLOBAL_MEM_STATUS: 0xDEAD0001,
    IAT_GET_CURRENT_PROCESS: 0xDEAD0002,
    IAT_GET_CURRENT_PROCESS_ID: 0xDEAD0003,
    IAT_OPEN_PROCESS: 0xDEAD0004,
    IAT_CLOSE_HANDLE: 0xDEAD0005,
    IAT_GET_PROCESS_MEM_INFO: 0xDEAD0006,
}

# Security cookie global
COOKIE_ADDR = 0xea8a70
COOKIE_VALUE = 0xBB40E64E

# Memory layout
STACK_ADDR = 0x00100000
RET_SENTINEL = 0x7fff0000


def read_func_bytes(va, size):
    sys.path.insert(0, HERE)
    from pe_parser import PEFile
    pe = PEFile(BIN).parse()
    off = pe.rva_to_offset(va - 0x400000)
    return pe._raw[off:off + size]


FUNC_SIZES = {
    ROOT_VA: 137,
    THUNK_VA: 6,    # ff2544b3a300 = jmp [0xa3b344]
    SCC_VA: 15,
    GSFAIL_VA: 262,
    HELPER_VA: 8,
}


def unicorn_side(pid, mem_info_success, working_set_size):
    """Execute FUN_008f9500 under Unicorn with injected import returns."""
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ECX

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    # map code
    for va, sz in FUNC_SIZES.items():
        page = va & ~0xFFF
        try:
            uc.mem_map(page, 0x2000)
        except:
            pass
        uc.mem_write(va, read_func_bytes(va, sz))

    # map IAT region and write stubs
    uc.mem_map(0xa3b000, 0x1000)
    # stub area for each import
    stub_base = 0xB000000
    uc.mem_map(stub_base, 0x10000)

    stub_addr = stub_base
    import_stubs = {}
    for iat, sentinel in SENTINELS.items():
        # each stub: mov eax, <return_val>; ret  (or ret <n> for stdcall)
        # For simplicity, all return a value in eax and ret (cdecl)
        if iat == IAT_GLOBAL_MEM_STATUS:
            # GlobalMemoryStatus(void) — fills MEMORYSTATUS struct at [esp+4]
            # but we don't care about the struct content for this test
            stub = b"\x90\xc3"  # nop; ret
        elif iat == IAT_GET_CURRENT_PROCESS:
            # returns pseudo handle (-1 = 0xFFFFFFFF)
            stub = b"\xb8" + struct.pack("<I", 0xFFFFFFFF) + b"\xc3"
        elif iat == IAT_GET_CURRENT_PROCESS_ID:
            # returns the PID
            stub = b"\xb8" + struct.pack("<I", pid) + b"\xc3"
        elif iat == IAT_OPEN_PROCESS:
            # returns a fake handle (e.g. 0x100)
            stub = b"\xb8" + struct.pack("<I", 0x100) + b"\xc2\x10\x00"  # ret 16 (stdcall, 4 args)
        elif iat == IAT_CLOSE_HANDLE:
            # returns 1 (success)
            stub = b"\xb8\x01\x00\x00\x00\xc2\x04\x00"  # ret 4 (stdcall, 1 arg)
        elif iat == IAT_GET_PROCESS_MEM_INFO:
            # GetProcessMemoryInfo(handle, @counters, cb) — BOOL, stdcall 3 args
            if mem_info_success:
                stub = bytes.fromhex("8b542408")  # mov edx, [esp+8]  (counters ptr)
                stub += b"\xc7" + bytes.fromhex("4208") + struct.pack("<I", working_set_size)  # mov dword [edx+8], WS
                stub += b"\xb8\x01\x00\x00\x00"  # mov eax, 1
                stub += b"\xc2\x0c\x00"  # ret 12
            else:
                stub = b"\x33\xc0\xc2\x0c\x00"  # xor eax,eax; ret 12

        uc.mem_write(stub_addr, stub)
        uc.mem_write(iat, struct.pack("<I", stub_addr))
        import_stubs[iat] = stub_addr
        stub_addr += 0x100

    # cookie global
    uc.mem_map(COOKIE_ADDR & ~0xFFF, 0x1000)
    uc.mem_write(COOKIE_ADDR, struct.pack("<I", COOKIE_VALUE))

    # helper global at 0x4183a84 (FUN_00982405 clears [0x4183a84])
    uc.mem_map(0x4183000, 0x1000)

    # stack: [sp] = ret sentinel, [sp+4] = arg (PID to compare)
    uc.mem_map(STACK_ADDR, 0x10000)
    uc.mem_map(RET_SENTINEL, 0x1000)
    sp = STACK_ADDR + 0x10000 - 0x100
    uc.mem_write(sp, struct.pack("<I", RET_SENTINEL))
    uc.mem_write(sp + 4, struct.pack("<I", 0x4242))  # arg = some value
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, ROOT_VA)

    stop = [False]
    def hook(uc, addr, size, _):
        if addr == RET_SENTINEL:
            stop[0] = True
            uc.emu_stop()
        if addr == GSFAIL_VA:
            stop[0] = True
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(ROOT_VA, RET_SENTINEL, count=10_000_000)

    eax = uc.reg_read(UC_X86_REG_EAX)
    return eax


def build_native(workdir):
    """Lift all functions, link, compile to native arm64 with Tranche 3 runtime."""
    bc_files = []
    for va, sz in FUNC_SIZES.items():
        raw = read_func_bytes(va, sz)
        bc = os.path.join(workdir, f"func_{va:x}.bc")
        subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                        "--bytes", raw.hex(), "--address", hex(va),
                        "--bc_out", bc], check=True)
        bc_files.append(bc)

    # Link
    linked_bc = os.path.join(workdir, "linked.bc")
    subprocess.run([LLVM17 + "/bin/llvm-link", "-o", linked_bc] + bc_files,
                   check=True, stderr=subprocess.DEVNULL)

    # Fix x86_fp80 → double
    linked_ll = os.path.join(workdir, "linked.ll")
    subprocess.run([LLVM17 + "/bin/llvm-dis", "-o", linked_ll, linked_bc],
                   check=True, stderr=subprocess.DEVNULL)
    with open(linked_ll, "r") as f:
        ir_text = f.read()
    ir_text = ir_text.replace("x86_fp80", "double")
    ir_text = ir_text.replace("llvm.fabs.f80", "llvm.fabs.f64")
    ir_text = re.sub(r'0xK[0-9a-fA-F]{20}', '0.0', ir_text)
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
    raw_bc = os.path.join(workdir, "linked_raw.bc")
    subprocess.run([LLVM17 + "/bin/llvm-as", "-o", raw_bc, linked_ll],
                   check=True, stderr=subprocess.DEVNULL)
    fixed_bc = os.path.join(workdir, "linked_fixed.bc")
    subprocess.run([LLVM17 + "/bin/opt", "-O2", "-o", fixed_bc, raw_bc],
                   check=True, stderr=subprocess.DEVNULL)

    # Compile to arm64 object
    lifted_o = os.path.join(workdir, "lifted.o")
    subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1",
                    "-x", "ir", "-o", lifted_o, fixed_bc],
                   check=True, stderr=subprocess.DEVNULL)

    # Compile runtime
    runtime_o = os.path.join(workdir, "runtime.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1",
                    "-std=c++17", "-DADDRESS_SIZE_BITS=32",
                    "-I", REM + "/include",
                    "-o", runtime_o,
                    os.path.join(HERE, "tranche3_runtime.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    # Host main
    all_vas = list(FUNC_SIZES.keys())
    registrations = ""
    for va in all_vas:
        registrations += f'    register_jump_target(0x{va:x}, sub_{va:x});\n'
    # also register the import sentinels as jump targets? No — imports are handled
    # by __remill_function_call, not __remill_jump. The thunk at 0x9860c6 does
    # jmp [0xa3b344] which becomes __remill_jump with the IAT value (sentinel).
    # We need to register the sentinel as a jump target that calls the import.
    # Actually, __remill_jump dispatches to the lifted function at that VA.
    # The sentinel 0xDEAD0006 is not a lifted function — it's handled by the
    # runtime's __remill_function_call. So the thunk's jmp [IAT] reads the
    # sentinel from memory and calls __remill_jump(sentinel). We need to handle
    # this: register a stub function for each sentinel.
    # Simpler: make __remill_jump handle sentinels too.

    decls = ""
    for va in all_vas:
        decls += f'extern "C" void* sub_{va:x}(State*, uint32_t, void*);\n'

    main_src = os.path.join(workdir, "main.cpp")
    with open(main_src, "w") as f:
        f.write(f'''#include "remill/Arch/X86/Runtime/State.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {{
    void set_test_params(uint32_t pid, uint32_t mem_ok, uint32_t ws);
    void mem_write32_ext(uint32_t, uint32_t);
    uint32_t mem_read32_ext(uint32_t);
    void mem_write64_ext(uint32_t, uint64_t);
    void register_jump_target(uint32_t, void*(*)(State*, uint32_t, void*));
{decls}
}}

int main(int argc, char** argv) {{
    // register all functions for jump dispatch
{registrations}
    for (int i = 1; i < argc; i += 3) {{
        uint32_t pid = (uint32_t)strtoul(argv[i], 0, 0);
        uint32_t mem_ok = (uint32_t)strtoul(argv[i+1], 0, 0);
        uint32_t ws = (uint32_t)strtoul(argv[i+2], 0, 0);

        set_test_params(pid, mem_ok, ws);

        // IAT sentinels
        mem_write32_ext(0xa3b214, 0xDEAD0001);  // GlobalMemoryStatus
        mem_write32_ext(0xa3b128, 0xDEAD0002);  // GetCurrentProcess
        mem_write32_ext(0xa3b0fc, 0xDEAD0003);  // GetCurrentProcessId
        mem_write32_ext(0xa3b280, 0xDEAD0004);  // OpenProcess
        mem_write32_ext(0xa3b0c8, 0xDEAD0005);  // CloseHandle
        mem_write32_ext(0xa3b344, 0xDEAD0006);  // GetProcessMemoryInfo

        // security cookie global
        mem_write32_ext(0xea8a70, 0xBB40E64E);

        // stack: [sp]=ret_sentinel, [sp+4]=arg
        uint32_t sp = 0x00100000 + 0x10000 - 0x100;
        mem_write32_ext(sp, 0x7fff0000);
        mem_write32_ext(sp + 4, 0x4242);

        State state;
        memset(&state, 0, sizeof(state));
        state.gpr.rsp.dword = sp;

        sub_{ROOT_VA:x}(&state, 0x{ROOT_VA:x}, nullptr);

        std::printf("%u\\n", state.gpr.rax.dword);
    }}
    return 0;
}}
''')

    exe = os.path.join(workdir, "tranche3")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32",
                    "-I", REM + "/include",
                    "-o", exe, main_src, lifted_o, runtime_o],
                   check=True)
    return exe


def main():
    print("=== Tranche 3: multiple imports + security cookie + thunk jump ===")
    print(f"root: FUN_008f9500 (137 bytes, process memory info query)")
    print(f"  -> 5 KERNEL32 imports + 1 PSAPI import via thunk")
    print(f"  -> __security_check_cookie (reads global cookie at {COOKIE_ADDR:#x})")
    print(f"  -> 2 conditional branches (je, jbe)")
    print(f"  -> 5 functions in call tree")
    print()

    # Test cases: (pid, mem_info_success, working_set_size, description)
    tests = [
        (1000, 1, 0x100000, "success: pid=1000, WS=1MB"),
        (5000, 1, 0x200000, "success: pid=5000, WS=2MB"),
        (100, 0, 0, "failure: GetProcessMemoryInfo returns 0"),
        (0, 1, 0, "success: pid=0, WS=0"),
        (0xFFFFFFFF, 1, 0x40000000, "success: pid=0xFFFFFFFF, WS=1GB"),
    ]

    with tempfile.TemporaryDirectory() as d:
        exe = build_native(d)
        args = []
        for pid, mem_ok, ws, _ in tests:
            args.extend([str(pid), str(mem_ok), str(ws)])
        r = subprocess.run([exe] + args, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"NATIVE CRASHED: {r.stderr[:500]}")
            sys.exit(1)
        native_results = [int(line) for line in r.stdout.strip().split("\n")]

        print(f"{'#':>2} {'pid':>10} {'mem_ok':>6} {'WS':>10} | {'description':<40} | {'unicorn':>12} {'native':>12} | result")
        print("-" * 110)
        fails = 0
        for i, (pid, mem_ok, ws, desc) in enumerate(tests):
            u_eax = unicorn_side(pid, mem_ok, ws) & 0xFFFFFFFF
            n_eax = native_results[i] & 0xFFFFFFFF

            # expected: if mem_ok and (pid - current_pid) > -1 (unsigned), return (pid - current_pid)
            # actually: the function stores ebx (from GlobalMemoryStatus? no, from [esp+0x14])
            # Let me just compare unicorn vs native (both should agree)
            ok = (u_eax == n_eax)
            if not ok:
                fails += 1
            print(f"{i+1:>2} {pid:>10} {mem_ok:>6} {ws:>10} | {desc:<40} | {u_eax:#010x} {n_eax:#010x} | {'PASS' if ok else 'FAIL'}")

    print()
    print(f"{'ALL PASS' if not fails else f'{fails} FAILURES'} — Tranche 3: 6 imports + security cookie + thunk, "
          f"Unicorn vs native arm64.")


if __name__ == "__main__":
    main()
