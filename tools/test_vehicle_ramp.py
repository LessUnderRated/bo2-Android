#!/usr/bin/env python3
"""Bounded differential test for the vehicle speed regulator.

The original side executes the 336 bytes at ``sub_684ae0`` from t6sp.exe in
Unicorn.  The native side compiles the existing standalone production lift
(``boot_cache/lift_full/func_684ae0.bc.fixed``) to arm64 and calls the same
function with a small flat-memory runtime.  The test deliberately avoids the
407 MB linked module: the regulator's ``dt != 0`` path does not call
``sub_552380`` and needs only the memory intrinsics below.

The command line contains all cases for one native process.  Each output is a
raw 32-bit word, so comparisons are bitwise and do not hide signed-zero or
NaN differences.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXE = Path("/Users/Shared/Call of Duty - Black Ops 2/t6sp.exe")
LLVM = Path("/opt/homebrew/opt/llvm@17")
REMILL_INCLUDE = ROOT / "tools" / "remill-src" / "include"
LIFT_BC = ROOT / "boot_cache" / "lift_full" / "func_684ae0.bc.fixed"

FUNC_VA = 0x684AE0
FUNC_SIZE = 0x150  # PE/Ghidra: 0x684ae0 .. 0x684c2f inclusive
VEH = 0x30000000
OUT_CURRENT = 0x30001000
OUT_TARGET = 0x30001010
STACK = 0x00100000
RET = 0x7FFF0000
CONST_HALF = 0xACF0C8


@dataclass(frozen=True)
class Case:
    name: str
    dt: float
    brake: int
    mode: int
    direct_target: float
    direct_rate: float
    initial_speed: float
    scale: float = 1.0
    mode_target: float = 352.0
    mode_accel: float = 52.8
    mode_decel: float = 26.4


# The first six cases reproduce P387's direct-call shape and expand it across
# both signs.  The remaining cases exercise the opposite (brake-to-zero) arm,
# target clamping, and a negative target.  All retain the real six-argument
# call layout used at 0x7e0067: (V, dt, 0, scale, &out_speed, &out_rate).
CASES = [
    Case("ramp +10 toward 352", 0.05, 0, 1, 352.0, 52.8, 10.0),
    Case("ramp zero toward 352", 0.05, 0, 1, 352.0, 52.8, 0.0),
    Case("ramp -10 toward 352", 0.05, 0, 1, 352.0, 52.8, -10.0),
    Case("ramp at target", 0.05, 0, 1, 352.0, 52.8, 352.0),
    Case("ramp past target", 0.05, 0, 1, 352.0, 52.8, 400.0),
    Case("ramp negative target", 0.05, 0, 1, -352.0, 52.8, -10.0),
    Case("brake +10 to zero", 0.05, 1, 1, 352.0, 52.8, 10.0),
    Case("brake zero", 0.05, 1, 1, 352.0, 52.8, 0.0),
    Case("brake -10 to zero", 0.05, 1, 1, 352.0, 52.8, -10.0),
    Case("brake +1 small step", 0.05, 1, 1, 352.0, 52.8, 1.0),
    Case("brake -1 small step", 0.05, 1, 1, 352.0, 52.8, -1.0),
    Case("ramp large step clamps", 0.50, 0, 1, 20.0, 52.8, 0.0),
]


def read_func_bytes() -> bytes:
    """Read the original function from the PE without relifting anything."""
    if not EXE.is_file():
        raise FileNotFoundError(EXE)
    sys.path.insert(0, str(ROOT / "tools"))
    from pe_parser import PEFile

    pe = PEFile(str(EXE)).parse()
    offset = pe.rva_to_offset(FUNC_VA - 0x400000)
    if offset is None:
        raise RuntimeError(f"VA {FUNC_VA:#x} is not in the PE")
    raw = bytes(pe._raw[offset : offset + FUNC_SIZE])
    if len(raw) != FUNC_SIZE:
        raise RuntimeError(f"short read: expected {FUNC_SIZE}, got {len(raw)}")
    return raw


def f32_word(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def word_f32(word: int) -> float:
    return struct.unpack("<f", struct.pack("<I", word & 0xFFFFFFFF))[0]


def _write_f32(uc, address: int, value: float) -> None:
    uc.mem_write(address, struct.pack("<f", value))


def _read_word(uc, address: int) -> int:
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def unicorn_side(code: bytes, case: Case) -> tuple[int, int, int, int]:
    """Execute one case in the original x86 bytes.

    Returns ``(out_current, out_target, vehicle_current, mode)`` as raw words.
    The output pointers are intentionally separate from the vehicle so a bad
    stack offset or pointer calculation cannot be mistaken for a correct store.
    """
    from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32, Uc
    from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP

    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(FUNC_VA & ~0xFFF, 0x1000)
    uc.mem_write(FUNC_VA, code)
    uc.mem_map(CONST_HALF & ~0xFFF, 0x1000)
    _write_f32(uc, CONST_HALF, 0.5)
    uc.mem_map(VEH, 0x2000)
    uc.mem_map(STACK, 0x10000)
    uc.mem_map(RET, 0x1000)

    _write_f32(uc, VEH + 0x344, case.direct_target)
    _write_f32(uc, VEH + 0x348, case.direct_rate)
    uc.mem_write(VEH + 0x4E4, struct.pack("<I", case.mode))
    _write_f32(uc, VEH + 0x4E8, case.mode_target)
    _write_f32(uc, VEH + 0x4EC, case.mode_accel)
    _write_f32(uc, VEH + 0x4F0, case.mode_decel)
    _write_f32(uc, VEH + 0x4F4, case.initial_speed)
    uc.mem_write(VEH + 0x514, struct.pack("<I", case.brake))
    uc.mem_write(OUT_CURRENT, struct.pack("<I", 0xA5A5A5A5))
    uc.mem_write(OUT_TARGET, struct.pack("<I", 0x5A5A5A5A))

    sp = STACK + 0x10000 - 0x100
    args = [
        RET,
        VEH,
        f32_word(case.dt),
        0,
        f32_word(case.scale),
        OUT_CURRENT,
        OUT_TARGET,
    ]
    for index, value in enumerate(args):
        uc.mem_write(sp + index * 4, struct.pack("<I", value & 0xFFFFFFFF))
    uc.reg_write(UC_X86_REG_ESP, sp)
    uc.reg_write(UC_X86_REG_EIP, FUNC_VA)

    def stop_at_return(emu, address, _size, _user_data):
        if address == RET:
            emu.emu_stop()

    uc.hook_add(UC_HOOK_CODE, stop_at_return)
    uc.emu_start(FUNC_VA, RET, count=1_000_000)
    return (
        _read_word(uc, OUT_CURRENT),
        _read_word(uc, OUT_TARGET),
        _read_word(uc, VEH + 0x4F4),
        _read_word(uc, VEH + 0x4E4),
    )


RUNTIME_CPP = r'''
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

static std::unordered_map<uint32_t, uint8_t> g_mem;

static uint8_t r8(uint32_t a) {
    auto it = g_mem.find(a);
    return it == g_mem.end() ? 0 : it->second;
}
static void w8(uint32_t a, uint8_t v) { g_mem[a] = v; }
static uint16_t r16(uint32_t a) { return uint16_t(r8(a)) | (uint16_t(r8(a + 1)) << 8); }
static uint32_t r32(uint32_t a) {
    return uint32_t(r8(a)) | (uint32_t(r8(a + 1)) << 8) |
           (uint32_t(r8(a + 2)) << 16) | (uint32_t(r8(a + 3)) << 24);
}
static void w16(uint32_t a, uint16_t v) { w8(a, uint8_t(v)); w8(a + 1, uint8_t(v >> 8)); }
static void w32(uint32_t a, uint32_t v) {
    w8(a, uint8_t(v)); w8(a + 1, uint8_t(v >> 8));
    w8(a + 2, uint8_t(v >> 16)); w8(a + 3, uint8_t(v >> 24));
}
static float rf32(uint32_t a) { uint32_t x = r32(a); float f; std::memcpy(&f, &x, 4); return f; }
static void wf32(uint32_t a, float f) { uint32_t x; std::memcpy(&x, &f, 4); w32(a, x); }

extern "C" {
void mem_clear_ext() { g_mem.clear(); }
void mem_write32_ext(uint32_t a, uint32_t v) { w32(a, v); }
uint32_t mem_read32_ext(uint32_t a) { return r32(a); }

uint8_t __remill_read_memory_8(void*, uint32_t a) { return r8(a); }
uint16_t __remill_read_memory_16(void*, uint32_t a) { return r16(a); }
uint32_t __remill_read_memory_32(void*, uint32_t a) { return r32(a); }
float __remill_read_memory_f32(void*, uint32_t a) { return rf32(a); }
void* __remill_write_memory_8(void* m, uint32_t a, uint8_t v) { w8(a, v); return m; }
void* __remill_write_memory_16(void* m, uint32_t a, uint16_t v) { w16(a, v); return m; }
void* __remill_write_memory_32(void* m, uint32_t a, uint32_t v) { w32(a, v); return m; }
void* __remill_write_memory_f32(void* m, uint32_t a, float v) { wf32(a, v); return m; }

bool __remill_flag_computation_carry(bool v, ...) { return v; }
bool __remill_flag_computation_zero(bool v, ...) { return v; }
bool __remill_flag_computation_sign(bool v, ...) { return v; }
bool __remill_flag_computation_overflow(bool v, ...) { return v; }
bool __remill_compare_eq(bool v) { return v; }
bool __remill_compare_neq(bool v) { return v; }
bool __remill_compare_ule(bool v) { return v; }
uint8_t __remill_undefined_8() { return 0; }
void* __remill_function_return(State&, uint32_t, void* m) { return m; }
void* __remill_error(State&, uint32_t pc, void*) {
    std::fprintf(stderr, "unexpected remill error at %#x\n", pc);
    std::abort();
}

// The dt != 0 cases never reach this declaration, but keeping a definition
// makes the standalone production lift link without extracting the 407 MB
// module or pulling in a second lifted function.
void* sub_552380(State*, uint32_t, void* m) { return m; }
}
'''


def build_native(workdir: Path) -> Path:
    """Compile the existing standalone lift and the tiny test runtime."""
    if not LIFT_BC.is_file():
        raise FileNotFoundError(LIFT_BC)
    clang = LLVM / "bin" / "clang"
    clangxx = LLVM / "bin" / "clang++"
    include = str(REMILL_INCLUDE)
    lift_o = workdir / "func_684ae0.o"
    runtime_cpp = workdir / "vehicle_ramp_runtime.cpp"
    runtime_o = workdir / "vehicle_ramp_runtime.o"
    main_cpp = workdir / "vehicle_ramp_main.cpp"
    exe = workdir / "vehicle_ramp_native"
    runtime_cpp.write_text(RUNTIME_CPP)

    run_checked(
        [
            str(clang), "-arch", "arm64", "-c", "-O0", "-fno-fast-math",
            "-ffp-contract=off", "-x", "ir", "-o", str(lift_o), str(LIFT_BC),
        ],
        "compile existing lift",
    )
    run_checked(
        [
            str(clangxx), "-arch", "arm64", "-c", "-O1", "-std=c++17",
            "-DADDRESS_SIZE_BITS=32", "-I", include, "-o", str(runtime_o),
            str(runtime_cpp),
        ],
        "compile test runtime",
    )

    main_cpp.write_text(
        r'''
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void* sub_684ae0(State*, uint32_t, void*);
void mem_clear_ext();
void mem_write32_ext(uint32_t, uint32_t);
uint32_t mem_read32_ext(uint32_t);
}

static uint32_t f32(const char* s) {
    float value = std::strtof(s, nullptr);
    uint32_t raw; std::memcpy(&raw, &value, 4); return raw;
}

int main(int argc, char** argv) {
    // Ten fields per case: dt brake mode direct_target direct_rate initial
    // scale mode_target mode_accel mode_decel.
    if ((argc - 1) % 10 != 0) return 2;
    for (int i = 1; i < argc; i += 10) {
        mem_clear_ext();
        mem_write32_ext(0xacf0c8, f32("0.5"));
        mem_write32_ext(0x30000000 + 0x344, f32(argv[i + 3]));
        mem_write32_ext(0x30000000 + 0x348, f32(argv[i + 4]));
        mem_write32_ext(0x30000000 + 0x4e4, uint32_t(std::strtoul(argv[i + 2], nullptr, 0)));
        mem_write32_ext(0x30000000 + 0x4e8, f32(argv[i + 8]));
        mem_write32_ext(0x30000000 + 0x4ec, f32(argv[i + 9]));
        mem_write32_ext(0x30000000 + 0x4f0, f32(argv[i + 10 - 1]));
        mem_write32_ext(0x30000000 + 0x4f4, f32(argv[i + 5]));
        mem_write32_ext(0x30000000 + 0x514, uint32_t(std::strtoul(argv[i + 1], nullptr, 0)));
        mem_write32_ext(0x30001000, 0xa5a5a5a5);
        mem_write32_ext(0x30001010, 0x5a5a5a5a);

        const uint32_t sp = 0x00100000 + 0x10000 - 0x100;
        mem_write32_ext(sp + 0x00, 0x7fff0000);
        mem_write32_ext(sp + 0x04, 0x30000000);
        mem_write32_ext(sp + 0x08, f32(argv[i + 0]));
        mem_write32_ext(sp + 0x0c, 0);
        mem_write32_ext(sp + 0x10, f32(argv[i + 6]));
        mem_write32_ext(sp + 0x14, 0x30001000);
        mem_write32_ext(sp + 0x18, 0x30001010);

        State state;
        std::memset(&state, 0, sizeof(state));
        state.gpr.rsp.dword = sp;
        sub_684ae0(&state, 0x684ae0, nullptr);
        std::printf("%08x %08x %08x %08x\n",
                    mem_read32_ext(0x30001000), mem_read32_ext(0x30001010),
                    mem_read32_ext(0x30000000 + 0x4f4),
                    mem_read32_ext(0x30000000 + 0x4e4));
    }
    return 0;
}
'''
    )
    run_checked(
        [
            str(clangxx), "-arch", "arm64", "-O1", "-std=c++17",
            "-DADDRESS_SIZE_BITS=32", "-I", include, "-o", str(exe),
            str(main_cpp), str(lift_o), str(runtime_o),
        ],
        "link native test",
    )
    return exe


def run_checked(command: list[str], label: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        details = (result.stderr or result.stdout)[-4000:]
        raise RuntimeError(f"{label} failed ({result.returncode}):\n{details}")
    return result


def native_args(case: Case) -> list[str]:
    return [
        repr(case.dt), str(case.brake), str(case.mode), repr(case.direct_target),
        repr(case.direct_rate), repr(case.initial_speed), repr(case.scale),
        repr(case.mode_target), repr(case.mode_accel), repr(case.mode_decel),
    ]


def format_float(raw: int) -> str:
    return f"{word_f32(raw): .8g} ({raw:#010x})"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-only", action="store_true", help="skip Unicorn comparison")
    args = parser.parse_args()

    code = read_func_bytes()
    expected_bytes = bytes.fromhex(
        "837c240c00568b7424087412f30f108644030000f30f109648030000eb6e"
        "8b86e404000083f801750af30f1086e8040000eb0a83f8027517f30f10460c"
        "f30f108eec040000f30f1096f0040000eb48f686d00200000474140fbf86cc"
        "02000050e83bd8ecfff30f10460ceb150fbf8ecc02000051e827d8ecfff30f"
        "108014010000f30f10901c01000083c4040f28caf30f5915c8f0ac0083be1405"
        "000000f30f594424140f28d87443f30f108ef40400000f57db0f2fd90f28c2"
        "f30f5944240c7609f30f58c10f2fc3eb0af30f5cc80f28c10f2fd876030f28c3"
        "8b4424188b54241cf30f1100f30f1112eb3ef30f1096f40400000f2fda0f28c1"
        "f30f5944240c7609f30f58c20f2fc3eb0af30f5cd00f28c20f2fd876030f28c3"
        "8b4424188b4c241cf30f1100f30f110983bee404000002751bf30f1086f4040000"
        "f2e009ff6c4447a0ac786e4040000000000005ec3"
    )
    if code[: len(expected_bytes)] != expected_bytes:
        raise RuntimeError("the PE bytes at sub_684ae0 do not match the recorded function")

    with tempfile.TemporaryDirectory(prefix="vehicle_ramp_") as temp:
        native_exe = build_native(Path(temp))
        argv: list[str] = [str(native_exe)]
        for case in CASES:
            argv.extend(native_args(case))
        native = run_checked(argv, "run native test").stdout.strip().splitlines()
        if len(native) != len(CASES):
            raise RuntimeError(f"native returned {len(native)} lines for {len(CASES)} cases")

        print(f"source: {EXE}")
        print(f"function: sub_684ae0 @ {FUNC_VA:#x}, {FUNC_SIZE} bytes")
        print(f"lift: {LIFT_BC} (standalone; linked_nostubs.bc not read)")
        print(f"cases: {len(CASES)} (ramp brake arms, positive and negative initial speeds)")
        print()
        failures = 0
        for case, line in zip(CASES, native):
            fields = line.split()
            if len(fields) != 4:
                raise RuntimeError(f"malformed native output for {case.name!r}: {line!r}")
            native_words = tuple(int(field, 16) for field in fields)
            original_words = unicorn_side(code, case) if not args.native_only else native_words
            equal = original_words == native_words
            if not equal:
                failures += 1
            status = "PASS" if equal else "FAIL"
            print(f"[{status}] {case.name}: dt={case.dt:g} brake={case.brake} "
                  f"initial={case.initial_speed:g}")
            labels = ("out_current", "out_target", "vehicle_current", "mode")
            for label, original, native_value in zip(labels, original_words, native_words):
                print(f"       {label:<16} orig={format_float(original):>25} "
                      f"native={format_float(native_value):>25}")
        print()
        if args.native_only:
            print("NATIVE ONLY — Unicorn comparison skipped")
        else:
            print("ALL PASS — original x86 and existing lifted arm64 agree bitwise"
                  if not failures else f"{failures} FAILURES — original x86 and lifted arm64 differ")
        return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
