// tranche2_runtime.cpp — runtime for Tranche 2: jump-table dispatch + FPU.
//
// New beyond Tranche 1:
//   - __remill_jump: dispatch table mapping case-handler VAs to function ptrs
//   - __remill_read_memory_f64: read 8-byte double from flat memory
//   - __remill_fpu_exception_clear/test: no-op stubs (no FPU exceptions in test)
//   - __remill_compare_ugt: echo precomputed flag (unsigned greater-than)
//
// The jump dispatch table is populated by the host before calling the entry
// function. This is the trampoline-table approach from ARCHITECTURE.md.

#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>

// --- flat memory model ---
static std::unordered_map<uint32_t, uint8_t> g_mem;
static uint32_t g_tick_value = 0;

static uint8_t  mem_read8(uint32_t a) { auto it=g_mem.find(a); return it!=g_mem.end()?it->second:0; }
static void     mem_write8(uint32_t a, uint8_t v) { g_mem[a]=v; }
static uint32_t mem_read32(uint32_t a) { return mem_read8(a)|(mem_read8(a+1)<<8)|(mem_read8(a+2)<<16)|(mem_read8(a+3)<<24); }
static void     mem_write32(uint32_t a, uint32_t v) { mem_write8(a,v&0xff); mem_write8(a+1,(v>>8)&0xff); mem_write8(a+2,(v>>16)&0xff); mem_write8(a+3,(v>>24)&0xff); }
static uint16_t mem_read16(uint32_t a) { return mem_read8(a)|(mem_read8(a+1)<<8); }
static void     mem_write16(uint32_t a, uint16_t v) { mem_write8(a,v&0xff); mem_write8(a+1,(v>>8)&0xff); }
static uint64_t mem_read64(uint32_t a) { return (uint64_t)mem_read32(a)|((uint64_t)mem_read32(a+4)<<32); }
static void     mem_write64(uint32_t a, uint64_t v) { mem_write32(a,v&0xffffffff); mem_write32(a+4,v>>32); }

// --- jump dispatch table: target VA -> function pointer ---
typedef void* (*BlockFn)(State*, uint32_t, void*);
static std::unordered_map<uint32_t, BlockFn> g_jump_table;

extern "C" {

void set_tick_value(uint32_t v) { g_tick_value = v; }
void mem_write32_ext(uint32_t a, uint32_t v) { mem_write32(a, v); }
uint32_t mem_read32_ext(uint32_t a) { return mem_read32(a); }
void mem_write64_ext(uint32_t a, uint64_t v) { mem_write64(a, v); }
uint64_t mem_read64_ext(uint32_t a) { return mem_read64(a); }
void register_jump_target(uint32_t va, BlockFn fn) { g_jump_table[va] = fn; }

// --- memory intrinsics ---
[[gnu::used]] uint8_t   __remill_read_memory_8(void*, uint32_t a) { return mem_read8(a); }
[[gnu::used]] uint16_t  __remill_read_memory_16(void*, uint32_t a) { return mem_read16(a); }
[[gnu::used]] uint32_t  __remill_read_memory_32(void*, uint32_t a) { return mem_read32(a); }
[[gnu::used]] uint64_t  __remill_read_memory_64(void*, uint32_t a) { return mem_read64(a); }
[[gnu::used]] double    __remill_read_memory_f64(void*, uint32_t a) {
    uint64_t raw = mem_read64(a);
    double d; memcpy(&d, &raw, 8); return d;
}
[[gnu::used]] void* __remill_write_memory_8(void* m, uint32_t a, uint8_t v) { mem_write8(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_16(void* m, uint32_t a, uint16_t v) { mem_write16(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_32(void* m, uint32_t a, uint32_t v) { mem_write32(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_64(void* m, uint32_t a, uint64_t v) { mem_write64(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_f64(void* m, uint32_t a, double v) {
    uint64_t raw; memcpy(&raw, &v, 8); mem_write64(a, raw); return m;
}

// --- control flow ---
[[gnu::used]] void* __remill_function_call(State &state, uint32_t addr, void* mem) {
    if (addr == 0xDEAD0001) { state.gpr.rax.dword = g_tick_value; }
    else { fprintf(stderr, "function_call: unknown target %#x\n", addr); abort(); }
    // Simulate the called function's ret: pop the return address that the
    // lifted IR pushed before calling us. Without this, ESP is off by 4 after
    // every import call, misaligning all subsequent stack reads.
    state.gpr.rsp.dword += 4;
    return mem;
}

[[gnu::used]] void* __remill_function_return(State &, uint32_t, void* mem) { return mem; }

[[gnu::used]] void* __remill_jump(State *state, uint32_t target, void* mem) {
    auto it = g_jump_table.find(target);
    if (it == g_jump_table.end()) {
        fprintf(stderr, "__remill_jump: unknown target %#x\n", target);
        abort();
    }
    return it->second(state, target, mem);
}

[[gnu::used]] void* __remill_missing_block(State &, uint32_t, void*) { fprintf(stderr,"missing_block\n"); abort(); }
[[gnu::used]] void* __remill_error(State &, uint32_t, void*) { fprintf(stderr,"error\n"); abort(); }

// --- flags + comparison ---
[[gnu::used]] uint8_t __remill_undefined_8(void) { return 0; }
[[gnu::used]] bool __remill_flag_computation_carry(bool r, ...) { return r; }
[[gnu::used]] bool __remill_flag_computation_zero(bool r, ...) { return r; }
[[gnu::used]] bool __remill_flag_computation_sign(bool r, ...) { return r; }
[[gnu::used]] bool __remill_flag_computation_overflow(bool r, ...) { return r; }
[[gnu::used]] bool __remill_compare_ugt(bool r) { return r; }

// --- FPU exception stubs ---
[[gnu::used]] void __remill_fpu_exception_clear(uint32_t) {}
[[gnu::used]] uint32_t __remill_fpu_exception_test(uint32_t) { return 0; }

// --- symbolic bases ---
[[gnu::used]] uint32_t __remill_symbolic_FSBASE(void) { return 0; }
[[gnu::used]] uint32_t __remill_symbolic_GSBASE(void) { return 0; }
[[gnu::used]] uint32_t __remill_symbolic_STACK(void) { return 0; }

}  // extern "C"
