// tranche1_runtime.cpp — runtime for Tranche 1: real memory model + State struct
// + import dispatch via __remill_function_call.
//
// This is the shim boundary between lifted IR and our native execution:
//   - __remill_read/write_memory_* : byte-exact flat memory model
//   - __remill_function_call        : dispatch indirect calls (IAT → imports)
//   - __remill_function_return      : no-op (C++ call stack handles return)
//   - __remill_flag_*               : echo the precomputed flag
//   - __remill_undefined_8          : zero
//
// Compiled with -DADDRESS_SIZE_BITS=32 to match the x86-32 State struct layout
// that remill-lift --arch x86 bakes into the bitcode.

#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>

// --- flat byte-addressed memory model (slow but correct for testing) ---
static std::unordered_map<uint32_t, uint8_t> g_mem;

static uint8_t mem_read8(uint32_t addr) {
    auto it = g_mem.find(addr);
    return it != g_mem.end() ? it->second : 0;
}
static void mem_write8(uint32_t addr, uint8_t v) {
    g_mem[addr] = v;
}
static uint32_t mem_read32(uint32_t addr) {
    return mem_read8(addr) | (mem_read8(addr+1)<<8) |
           (mem_read8(addr+2)<<16) | (mem_read8(addr+3)<<24);
}
static void mem_write32(uint32_t addr, uint32_t v) {
    mem_write8(addr, v & 0xff);
    mem_write8(addr+1, (v>>8)&0xff);
    mem_write8(addr+2, (v>>16)&0xff);
    mem_write8(addr+3, (v>>24)&0xff);
}
static uint16_t mem_read16(uint32_t addr) {
    return mem_read8(addr) | (mem_read8(addr+1)<<8);
}
static void mem_write16(uint32_t addr, uint16_t v) {
    mem_write8(addr, v & 0xff);
    mem_write8(addr+1, (v>>8)&0xff);
}
static uint64_t mem_read64(uint32_t addr) {
    uint64_t lo = mem_read32(addr);
    uint64_t hi = mem_read32(addr+4);
    return lo | (hi << 32);
}
static void mem_write64(uint32_t addr, uint64_t v) {
    mem_write32(addr, v & 0xffffffff);
    mem_write32(addr+4, v >> 32);
}

// Deterministic tick value set by the host before calling the lifted function.
static uint32_t g_tick_value = 0;

extern "C" {

void set_tick_value(uint32_t v) { g_tick_value = v; }
void mem_write32_ext(uint32_t addr, uint32_t v) { mem_write32(addr, v); }
uint32_t mem_read32_ext(uint32_t addr) { return mem_read32(addr); }

// --- memory intrinsics ---
[[gnu::used]] uint8_t  __remill_read_memory_8(void*, uint32_t a) { return mem_read8(a); }
[[gnu::used]] uint16_t __remill_read_memory_16(void*, uint32_t a) { return mem_read16(a); }
[[gnu::used]] uint32_t __remill_read_memory_32(void*, uint32_t a) { return mem_read32(a); }
[[gnu::used]] uint64_t __remill_read_memory_64(void*, uint32_t a) { return mem_read64(a); }
[[gnu::used]] void* __remill_write_memory_8(void* m, uint32_t a, uint8_t v) { mem_write8(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_16(void* m, uint32_t a, uint16_t v) { mem_write16(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_32(void* m, uint32_t a, uint32_t v) { mem_write32(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_64(void* m, uint32_t a, uint64_t v) { mem_write64(a,v); return m; }

// --- control flow ---
// __remill_function_call: dispatch indirect calls. For Tranche 1, the only
// indirect call is to GetTickCount via the IAT slot at 0xa3b0dc. The IAT
// slot contains a sentinel (0xDEAD0001) that we put there. When we see it,
// we write the deterministic tick into State.EAX and return.
[[gnu::used]] void* __remill_function_call(State &state, uint32_t addr, void* mem) {
    if (addr == 0xDEAD0001) {  // GetTickCount
        state.gpr.rax.dword = g_tick_value;
    } else {
        std::fprintf(stderr, "remill_function_call: unknown target %#x\n", addr);
        std::abort();
    }
    return mem;
}

[[gnu::used]] void* __remill_function_return(State &, uint32_t, void* mem) {
    return mem;  // C++ call stack handles the actual return
}

[[gnu::used]] void* __remill_jump(State &, uint32_t, void* mem) { return mem; }
[[gnu::used]] void* __remill_missing_block(State &, uint32_t, void*) {
    std::fprintf(stderr, "remill: missing block\n");
    std::abort();
}
[[gnu::used]] void* __remill_error(State &, uint32_t, void*) {
    std::fprintf(stderr, "remill: error\n");
    std::abort();
}

// --- flags ---
[[gnu::used]] uint8_t __remill_undefined_8(void) { return 0; }
[[gnu::used]] bool __remill_flag_computation_carry(bool r, ...) { return r; }
[[gnu::used]] bool __remill_flag_computation_zero(bool r, ...) { return r; }
[[gnu::used]] bool __remill_flag_computation_sign(bool r, ...) { return r; }
[[gnu::used]] bool __remill_flag_computation_overflow(bool r, ...) { return r; }

// --- symbolic bases (zero = flat addressing) ---
[[gnu::used]] uint32_t __remill_symbolic_FSBASE(void) { return 0; }
[[gnu::used]] uint32_t __remill_symbolic_GSBASE(void) { return 0; }
[[gnu::used]] uint32_t __remill_symbolic_STACK(void) { return 0; }

}  // extern "C"
