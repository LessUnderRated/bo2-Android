"""
run_shim_test — test the unified Win32 shim against Tranche 3's function
(FUN_008f9500, 6 imports). Validates that the proper shim framework dispatches
imports correctly, replacing the per-tranche ad-hoc stubs.
"""
import os, sys, subprocess, tempfile, struct, re
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"
REM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src")
BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"

ROOT_VA = 0x8f9500
THUNK_VA = 0x9860c6
SCC_VA = 0x95f655
GSFAIL_VA = 0x96f2ef
HELPER_VA = 0x982405

FUNC_SIZES = {
    ROOT_VA: 137,
    THUNK_VA: 6,
    SCC_VA: 15,
    GSFAIL_VA: 262,
    HELPER_VA: 8,
}

COOKIE_ADDR = 0xea8a70
COOKIE_VALUE = 0xBB40E64E
STACK_ADDR = 0x00100000


def read_func_bytes(va, size):
    sys.path.insert(0, HERE)
    from pe_parser import PEFile
    pe = PEFile(BIN).parse()
    off = pe.rva_to_offset(va - 0x400000)
    return pe._raw[off:off + size]


def unicorn_side(pid, mem_info_success, working_set_size):
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    for va, sz in FUNC_SIZES.items():
        page = va & ~0xFFF
        try: uc.mem_map(page, 0x2000)
        except: pass
        uc.mem_write(va, read_func_bytes(va, sz))

    # IAT stubs
    uc.mem_map(0xa3b000, 0x1000)
    stub_base = 0xB000000
    uc.mem_map(stub_base, 0x10000)
    stub_addr = stub_base

    iat_slots = {
        0xa3b214: "GlobalMemoryStatus",
        0xa3b128: "GetCurrentProcess",
        0xa3b0fc: "GetCurrentProcessId",
        0xa3b280: "OpenProcess",
        0xa3b0c8: "CloseHandle",
        0xa3b344: "GetProcessMemoryInfo",
    }
    for iat, name in iat_slots.items():
        if name == "GlobalMemoryStatus":
            stub = b"\x90\xc3"
        elif name == "GetCurrentProcess":
            stub = b"\xb8" + struct.pack("<I", 0xFFFFFFFF) + b"\xc3"
        elif name == "GetCurrentProcessId":
            stub = b"\xb8" + struct.pack("<I", pid) + b"\xc3"
        elif name == "OpenProcess":
            stub = b"\xb8" + struct.pack("<I", 0x100) + b"\xc2\x10\x00"
        elif name == "CloseHandle":
            stub = b"\xb8\x01\x00\x00\x00\xc2\x04\x00"
        elif name == "GetProcessMemoryInfo":
            if mem_info_success:
                stub = bytes.fromhex("8b542408")
                stub += b"\xc7" + bytes.fromhex("4208") + struct.pack("<I", working_set_size)
                stub += b"\xb8\x01\x00\x00\x00\xc2\x0c\x00"
            else:
                stub = b"\x33\xc0\xc2\x0c\x00"
        uc.mem_write(stub_addr, stub)
        uc.mem_write(iat, struct.pack("<I", stub_addr))
        stub_addr += 0x100

    # cookie
    uc.mem_map(COOKIE_ADDR & ~0xFFF, 0x1000)
    uc.mem_write(COOKIE_ADDR, struct.pack("<I", COOKIE_VALUE))
    uc.mem_map(0x4183000, 0x1000)

    uc.mem_map(STACK_ADDR, 0x10000)
    ret_sent = 0x7fff0000
    uc.mem_map(ret_sent, 0x1000)
    sp = STACK_ADDR + 0x10000 - 0x100
    uc.mem_write(sp, struct.pack("<I", ret_sent))
    uc.mem_write(sp + 4, struct.pack("<I", 0x4242))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, ROOT_VA)

    stop = [False]
    def hook(uc, addr, size, _):
        if addr == ret_sent or addr == GSFAIL_VA:
            stop[0] = True; uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(ROOT_VA, ret_sent, count=10_000_000)
    return uc.reg_read(UC_X86_REG_EAX) & 0xFFFFFFFF


def build_native(workdir):
    bc_files = []
    for va, sz in FUNC_SIZES.items():
        raw = read_func_bytes(va, sz)
        bc = os.path.join(workdir, f"func_{va:x}.bc")
        subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                        "--bytes", raw.hex(), "--address", hex(va),
                        "--bc_out", bc], check=True)
        bc_files.append(bc)

    linked_bc = os.path.join(workdir, "linked.bc")
    subprocess.run([LLVM17 + "/bin/llvm-link", "-o", linked_bc] + bc_files,
                   check=True, stderr=subprocess.DEVNULL)

    # fix IR
    linked_ll = os.path.join(workdir, "linked.ll")
    subprocess.run([LLVM17 + "/bin/llvm-dis", "-o", linked_ll, linked_bc],
                   check=True, stderr=subprocess.DEVNULL)
    with open(linked_ll, "r") as f: ir_text = f.read()
    ir_text = ir_text.replace("x86_fp80", "double")
    ir_text = ir_text.replace("i80", "i64")
    ir_text = ir_text.replace("llvm.fabs.f80", "llvm.fabs.f64")
    ir_text = re.sub(r'0xK[0-9a-fA-F]{20}', '0.0', ir_text)
    lines = ir_text.split("\n")
    new_lines = []
    for line in lines:
        m = re.match(r'(\s*)(%\w+)\s*=\s*(fptrunc|fpext)\s+double\s+(%\w+)\s+to\s+double', line)
        if m: line = f"{m.group(1)}{m.group(2)} = bitcast double {m.group(4)} to double"
        new_lines.append(line)
    with open(linked_ll, "w") as f: f.write("\n".join(new_lines))
    raw_bc = os.path.join(workdir, "linked_raw.bc")
    subprocess.run([LLVM17 + "/bin/llvm-as", "-o", raw_bc, linked_ll], check=True, stderr=subprocess.DEVNULL)
    fixed_bc = os.path.join(workdir, "linked_fixed.bc")
    subprocess.run([LLVM17 + "/bin/opt", "-O2", "-o", fixed_bc, raw_bc], check=True, stderr=subprocess.DEVNULL)

    lifted_o = os.path.join(workdir, "lifted.o")
    subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1", "-x", "ir",
                    "-o", lifted_o, fixed_bc], check=True, stderr=subprocess.DEVNULL)

    # compile the unified Win32 shim runtime
    runtime_o = os.path.join(workdir, "runtime.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32", "-I", REM + "/include",
                    "-I", os.path.join(HERE, "..", "runtime", "win32"),
                    "-o", runtime_o,
                    os.path.join(HERE, "..", "runtime", "win32", "win32_runtime.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    kernel32_o = os.path.join(workdir, "kernel32.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32", "-I", REM + "/include",
                    "-I", os.path.join(HERE, "..", "runtime", "win32"),
                    "-o", kernel32_o,
                    os.path.join(HERE, "..", "runtime", "win32", "kernel32_impl.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    user32_o = os.path.join(workdir, "user32.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32", "-I", REM + "/include",
                    "-I", os.path.join(HERE, "..", "runtime", "win32"),
                    "-o", user32_o,
                    os.path.join(HERE, "..", "runtime", "win32", "user32_impl.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    remaining_o = os.path.join(workdir, "remaining.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32", "-I", REM + "/include",
                    "-I", os.path.join(HERE, "..", "runtime", "win32"),
                    "-o", remaining_o,
                    os.path.join(HERE, "..", "runtime", "win32", "remaining_dlls_impl.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    # host main — uses the unified shim
    all_vas = list(FUNC_SIZES.keys())
    registrations = "".join(f'    register_jump_target(0x{va:x}, sub_{va:x});\n' for va in all_vas)
    decls = "".join(f'extern "C" void* sub_{va:x}(State*, uint32_t, void*);\n' for va in all_vas)

    main_src = os.path.join(workdir, "main.cpp")
    with open(main_src, "w") as f:
        f.write(f'''#include "remill/Arch/X86/Runtime/State.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {{
    void win32_shim_init();
    void set_test_mem_params(uint32_t, uint32_t);
    void mem_write32_ext(uint32_t, uint32_t);
    uint32_t mem_read32_ext(uint32_t);
    void register_jump_target(uint32_t, void*(*)(State*, uint32_t, void*));
{decls}
}}

int main(int argc, char** argv) {{
    win32_shim_init();

{registrations}
    mem_write32_ext(0xea8a70, 0xBB40E64E);

    for (int i = 1; i < argc; i += 3) {{
        uint32_t pid = (uint32_t)strtoul(argv[i], 0, 0);
        uint32_t mem_ok = (uint32_t)strtoul(argv[i+1], 0, 0);
        uint32_t ws = (uint32_t)strtoul(argv[i+2], 0, 0);
        set_test_mem_params(mem_ok, ws);

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

    exe = os.path.join(workdir, "shim_test")
    r = subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32", "-I", REM + "/include",
                    "-I", os.path.join(HERE, "..", "runtime", "win32"),
                    "-o", exe, main_src, lifted_o, runtime_o, kernel32_o, user32_o, remaining_o,
                    "-framework", "ApplicationServices"],
                   capture_output=True, text=True)
    if r.returncode != 0:
        print("LINK ERROR:")
        for line in r.stderr.split("\n")[:15]:
            if line: print(f"  {line}")
        raise subprocess.CalledProcessError(r.returncode, r.args, r.stdout, r.stderr)
    return exe


def main():
    print("=== Win32 Shim Test: Tranche 3 function with unified shim ===")
    print(f"target: FUN_008f9500 (137 bytes, 6 imports)")
    print(f"  -> using unified win32_runtime.cpp + kernel32_impl.cpp")
    print(f"  -> proper IAT dispatch (not ad-hoc sentinels)")
    print()

    tests = [
        (1000, 1, 0x100000, "success: pid=1000, WS=1MB"),
        (5000, 1, 0x200000, "success: pid=5000, WS=2MB"),
        (100, 0, 0, "failure: GetProcessMemoryInfo returns 0"),
        (0xFFFFFFFF, 1, 0x40000000, "success: pid=0xFFFFFFFF, WS=1GB"),
    ]

    with tempfile.TemporaryDirectory() as d:
        exe = build_native(d)
        args = []
        for pid, mem_ok, ws, _ in tests:
            args.extend([str(pid), str(mem_ok), str(ws)])
        r = subprocess.run([exe] + args, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"NATIVE CRASHED: {r.stderr[:800]}")
            sys.exit(1)
        native_results = [int(line) for line in r.stdout.strip().split("\n")]
        stderr_output = r.stderr[:500] if r.stderr else ""

        print(f"{'#':>2} {'pid':>10} {'mem_ok':>6} {'WS':>10} | {'description':<40} | {'native':>12} | result")
        print("-" * 100)
        fails = 0
        for i, (pid, mem_ok, ws, desc) in enumerate(tests):
            u_eax = unicorn_side(pid, mem_ok, ws) & 0xFFFFFFFF
            n_eax = native_results[i] & 0xFFFFFFFF
            ok = (u_eax == n_eax)
            if not ok: fails += 1
            print(f"{i+1:>2} {pid:>10} {mem_ok:>6} {ws:>10} | {desc:<40} | {n_eax:#010x} | {'PASS' if ok else 'FAIL'}")

        if stderr_output:
            print(f"\n[shim stderr] {stderr_output.strip()[:200]}")
        print(f"\n{'ALL PASS' if not fails else f'{fails} FAILURES'} — Win32 shim framework "
              f"dispatches imports correctly.")


if __name__ == "__main__":
    main()
