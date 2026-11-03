"""
run_tranche4 — Tranche 4: backward branches (loops) + setcc + movsx.

Target: FUN_00436070 (80 bytes) — string compare with a backward branch.
  - Loop: jne 0x436090 (backward) — validates loops in LLVM br i1
  - setge: conditional set from flags → validates setcc instructions
  - movsx: sign-extend byte to dword → validates sign extension
  - lea eax, [eax+eax-1]: computes -1 or +1 from setge result
  - Multiple exit paths: match (0), mismatch (±1), null input (pointer diff)
  - No imports, no calls — completely self-contained

Function signature (cdecl):
  int strncmp_custom(char* str1, char* str2, int max_len)
  Returns:
    0 if match (up to max_len or null terminator)
    +1 if str1[i] > str2[i] at first difference
    -1 if str1[i] < str2[i] at first difference
    str2 if str1 is NULL
    -str1 if str2 is NULL
"""
import os, sys, subprocess, tempfile, struct, re
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "harness"))

HERE = os.path.dirname(os.path.abspath(__file__))
REMILL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src/build/bin/lift/remill-lift-17")
LLVM17 = "/opt/homebrew/opt/llvm@17"
REM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "remill-src")
BIN = "/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe"

FUNC_VA = 0x436070
FUNC_SIZE = 80

STACK_ADDR = 0x00100000
RET_SENTINEL = 0x7fff0000
STR1_ADDR = 0x20000000
STR2_ADDR = 0x20001000


def read_func_bytes():
    sys.path.insert(0, HERE)
    from pe_parser import PEFile
    pe = PEFile(BIN).parse()
    off = pe.rva_to_offset(FUNC_VA - 0x400000)
    return pe._raw[off:off + FUNC_SIZE]


FUNC_BYTES = read_func_bytes()


def unicorn_side(str1_bytes, str2_bytes, max_len, str1_null=False, str2_null=False):
    """Execute the string compare under Unicorn."""
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    # code
    page = FUNC_VA & ~0xFFF
    uc.mem_map(page, 0x1000)
    uc.mem_write(FUNC_VA, FUNC_BYTES)
    # strings
    uc.mem_map(STR1_ADDR, 0x1000)
    uc.mem_map(STR2_ADDR, 0x1000)
    if not str1_null:
        uc.mem_write(STR1_ADDR, str1_bytes + b"\x00")
    if not str2_null:
        uc.mem_write(STR2_ADDR, str2_bytes + b"\x00")
    # stack
    uc.mem_map(STACK_ADDR, 0x10000)
    uc.mem_map(RET_SENTINEL, 0x1000)
    sp = STACK_ADDR + 0x10000 - 0x100
    uc.mem_write(sp, struct.pack("<I", RET_SENTINEL))
    uc.mem_write(sp + 4, struct.pack("<I", 0 if str1_null else STR1_ADDR))
    uc.mem_write(sp + 8, struct.pack("<I", 0 if str2_null else STR2_ADDR))
    uc.mem_write(sp + 12, struct.pack("<I", max_len))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, FUNC_VA)

    stop = [False]
    def hook(uc, addr, size, _):
        if addr == RET_SENTINEL:
            stop[0] = True
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    uc.emu_start(FUNC_VA, RET_SENTINEL, count=10_000_000)
    eax = uc.reg_read(UC_X86_REG_EAX)
    # sign-extend to 32-bit signed for comparison
    if eax & 0x80000000:
        eax -= 0x100000000
    return eax


def build_native(workdir):
    """Lift, fix IR, compile to native arm64."""
    bc = os.path.join(workdir, "strncmp.bc")
    subprocess.run([REMILL, "--arch", "x86", "--os", "windows",
                    "--bytes", FUNC_BYTES.hex(), "--address", hex(FUNC_VA),
                    "--bc_out", bc], check=True)

    # Fix x86_fp80 → double (even though this function has no FPU, the
    # semantics headers define the type and it must compile)
    linked_ll = os.path.join(workdir, "strncmp.ll")
    subprocess.run([LLVM17 + "/bin/llvm-dis", "-o", linked_ll, bc],
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
    raw_bc = os.path.join(workdir, "strncmp_raw.bc")
    subprocess.run([LLVM17 + "/bin/llvm-as", "-o", raw_bc, linked_ll],
                   check=True, stderr=subprocess.DEVNULL)
    fixed_bc = os.path.join(workdir, "strncmp_fixed.bc")
    subprocess.run([LLVM17 + "/bin/opt", "-O2", "-o", fixed_bc, raw_bc],
                   check=True, stderr=subprocess.DEVNULL)

    # compile to arm64
    lifted_o = os.path.join(workdir, "lifted.o")
    subprocess.run([LLVM17 + "/bin/clang", "-arch", "arm64", "-c", "-O1",
                    "-x", "ir", "-o", lifted_o, fixed_bc],
                   check=True, stderr=subprocess.DEVNULL)

    # runtime (reuse tranche3 — has all needed intrinsics)
    runtime_o = os.path.join(workdir, "runtime.o")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-c", "-O1",
                    "-std=c++17", "-DADDRESS_SIZE_BITS=32",
                    "-I", REM + "/include",
                    "-o", runtime_o,
                    os.path.join(HERE, "tranche3_runtime.cpp")],
                   check=True, stderr=subprocess.DEVNULL)

    # host main
    main_src = os.path.join(workdir, "main.cpp")
    with open(main_src, "w") as f:
        f.write(f'''#include "remill/Arch/X86/Runtime/State.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

extern "C" {{
    void* sub_{FUNC_VA:x}(State*, uint32_t, void*);
    void mem_write32_ext(uint32_t, uint32_t);
    uint32_t mem_read32_ext(uint32_t);
    void mem_write8_ext(uint32_t, uint8_t);
    uint8_t mem_read8_ext(uint32_t);
}}

// write a string to memory
static void write_str(uint32_t addr, const char* s) {{
    for (int i = 0; s[i]; i++) mem_write8_ext(addr + i, (uint8_t)s[i]);
    mem_write8_ext(addr + strlen(s), 0);
}}

int main(int argc, char** argv) {{
    // each test: str1 str2 max_len str1_null str2_null
    for (int i = 1; i < argc; i += 5) {{
        const char* s1 = argv[i];
        const char* s2 = argv[i+1];
        uint32_t max_len = (uint32_t)strtoul(argv[i+2], 0, 0);
        uint32_t s1_null = (uint32_t)strtoul(argv[i+3], 0, 0);
        uint32_t s2_null = (uint32_t)strtoul(argv[i+4], 0, 0);

        // write strings
        if (!s1_null) write_str(0x20000000, s1);
        if (!s2_null) write_str(0x20001000, s2);

        // stack: [sp]=ret, [sp+4]=str1, [sp+8]=str2, [sp+12]=max_len
        uint32_t sp = 0x00100000 + 0x10000 - 0x100;
        mem_write32_ext(sp, 0x7fff0000);
        mem_write32_ext(sp + 4, s1_null ? 0 : 0x20000000);
        mem_write32_ext(sp + 8, s2_null ? 0 : 0x20001000);
        mem_write32_ext(sp + 12, max_len);

        State state;
        memset(&state, 0, sizeof(state));
        state.gpr.rsp.dword = sp;

        sub_{FUNC_VA:x}(&state, 0x{FUNC_VA:x}, nullptr);

        int32_t result = (int32_t)state.gpr.rax.dword;
        std::printf("%d\\n", result);
    }}
    return 0;
}}
''')

    exe = os.path.join(workdir, "tranche4")
    subprocess.run([LLVM17 + "/bin/clang++", "-arch", "arm64", "-O1", "-std=c++17",
                    "-DADDRESS_SIZE_BITS=32",
                    "-I", REM + "/include",
                    "-o", exe, main_src, lifted_o, runtime_o],
                   check=True)
    return exe


def main():
    print("=== Tranche 4: backward branches (loops) + setcc + movsx ===")
    print(f"target: FUN_00436070 ({FUNC_SIZE} bytes, string compare with loop)")
    print(f"  -> backward branch: jne 0x436090 (loop)")
    print(f"  -> setge + lea [eax+eax-1] → ±1 result")
    print(f"  -> movsx (sign-extend byte)")
    print(f"  -> no imports, no calls — self-contained")
    print()

    # Test cases: (str1, str2, max_len, str1_null, str2_null, description)
    tests = [
        ("hello", "hello", 10, False, False, "identical strings"),
        ("hello", "world", 10, False, False, "'h' < 'w' → negative"),
        ("world", "hello", 10, False, False, "'w' > 'h' → positive"),
        ("abc", "abcd", 3, False, False, "match within max_len=3 → 0"),
        ("abc", "abcd", 4, False, False, "null vs 'd' → negative"),
        ("abcd", "abc", 4, False, False, "'d' vs null → positive"),
        ("", "", 10, False, False, "both empty → 0"),
        ("abc", "abd", 10, False, False, "'c' < 'd' → negative"),
        ("abd", "abc", 10, False, False, "'d' > 'c' → positive"),
        ("x", "x", 1, False, False, "single char match, max=1 → 0"),
        ("", "a", 10, False, False, "empty vs 'a' → negative"),
        ("a", "", 10, False, False, "'a' vs empty → positive"),
        ("hello", "hello", 0, False, False, "max_len=0 → 0 (no comparison)"),
        ("hello", "hellx", 4, False, False, "match first 4, max=4 → 0"),
        ("hello", "hellx", 5, False, False, "5th char differs → negative"),
    ]

    with tempfile.TemporaryDirectory() as d:
        exe = build_native(d)
        args = []
        for s1, s2, ml, n1, n2, _ in tests:
            args.extend([s1, s2, str(ml), str(int(n1)), str(int(n2))])
        r = subprocess.run([exe] + args, capture_output=True, text=True)
        if r.returncode != 0:
            print(f"NATIVE CRASHED: {r.stderr[:500]}")
            sys.exit(1)
        native_results = [int(line) for line in r.stdout.strip().split("\n")]

        print(f"{'#':>2} {'str1':<8} {'str2':<8} {'max':>4} | {'description':<35} | {'unicorn':>8} {'native':>8} | result")
        print("-" * 95)
        fails = 0
        for i, (s1, s2, ml, n1, n2, desc) in enumerate(tests):
            u = unicorn_side(s1.encode(), s2.encode(), ml, n1, n2)
            n = native_results[i]
            ok = (u == n)
            if not ok:
                fails += 1
            s1_disp = "NULL" if n1 else f'"{s1}"'
            s2_disp = "NULL" if n2 else f'"{s2}"'
            print(f"{i+1:>2} {s1_disp:<8} {s2_disp:<8} {ml:>4} | {desc:<35} | {u:>8} {n:>8} | {'PASS' if ok else 'FAIL'}")

    print()
    print(f"{'ALL PASS' if not fails else f'{fails} FAILURES'} — Tranche 4: loops + setcc + movsx, "
          f"Unicorn vs native arm64.")


if __name__ == "__main__":
    main()
