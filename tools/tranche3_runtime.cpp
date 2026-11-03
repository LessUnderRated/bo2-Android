// tranche3_runtime.cpp — runtime for Tranche 3: 6 imports + security cookie
// + thunk jump dispatch.
//
// New beyond Tranche 2:
//   - 6 different imports with different calling conventions and returns
//   - __remill_jump handles both lifted-function targets AND import sentinels
//     (the thunk at 0x9860c6 does jmp [IAT] → __remill_jump reads sentinel
//     from memory → must dispatch to import handler, not a lifted function)
//   - Security cookie: the host writes the cookie at 0xea8a70, the lifted
//     code reads it via __remill_read_memory_32 and checks it at exit

#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>

static std::unordered_map<uint32_t, uint8_t> g_mem;
static uint32_t g_pid = 0;
static uint32_t g_mem_ok = 0;
static uint32_t g_ws = 0;

static uint8_t  mem_read8(uint32_t a) { auto it=g_mem.find(a); return it!=g_mem.end()?it->second:0; }
static void     mem_write8(uint32_t a, uint8_t v) { g_mem[a]=v; }
static uint32_t mem_read32(uint32_t a) { return mem_read8(a)|(mem_read8(a+1)<<8)|(mem_read8(a+2)<<16)|(mem_read8(a+3)<<24); }
static void     mem_write32(uint32_t a, uint32_t v) { mem_write8(a,v&0xff); mem_write8(a+1,(v>>8)&0xff); mem_write8(a+2,(v>>16)&0xff); mem_write8(a+3,(v>>24)&0xff); }
static uint16_t mem_read16(uint32_t a) { return mem_read8(a)|(mem_read8(a+1)<<8); }
static void     mem_write16(uint32_t a, uint16_t v) { mem_write8(a,v&0xff); mem_write8(a+1,(v>>8)&0xff); }
static uint64_t mem_read64(uint32_t a) { return (uint64_t)mem_read32(a)|((uint64_t)mem_read32(a+4)<<32); }
static void     mem_write64(uint32_t a, uint64_t v) { mem_write32(a,v&0xffffffff); mem_write32(a+4,v>>32); }

typedef void* (*BlockFn)(State*, uint32_t, void*);
static std::unordered_map<uint32_t, BlockFn> g_jump_table;

extern "C" {

void set_test_params(uint32_t pid, uint32_t mem_ok, uint32_t ws) {
    g_pid = pid; g_mem_ok = mem_ok; g_ws = ws;
}
void mem_write32_ext(uint32_t a, uint32_t v) { mem_write32(a, v); }
uint32_t mem_read32_ext(uint32_t a) { return mem_read32(a); }
void mem_write8_ext(uint32_t a, uint8_t v) { mem_write8(a, v); }
uint8_t mem_read8_ext(uint32_t a) { return mem_read8(a); }
void mem_write64_ext(uint32_t a, uint64_t v) { mem_write64(a, v); }
uint64_t mem_read64_ext(uint32_t a) { return mem_read64(a); }
void register_jump_target(uint32_t va, BlockFn fn) { g_jump_table[va] = fn; }

// --- memory intrinsics ---
[[gnu::used]] uint8_t   __remill_read_memory_8(void*, uint32_t a) { return mem_read8(a); }
[[gnu::used]] uint16_t  __remill_read_memory_16(void*, uint32_t a) { return mem_read16(a); }
[[gnu::used]] uint32_t  __remill_read_memory_32(void*, uint32_t a) { return mem_read32(a); }
[[gnu::used]] uint64_t  __remill_read_memory_64(void*, uint32_t a) { return mem_read64(a); }
[[gnu::used]] double    __remill_read_memory_f64(void*, uint32_t a) {
    uint64_t raw = mem_read64(a); double d; memcpy(&d, &raw, 8); return d;
}
[[gnu::used]] float     __remill_read_memory_f32(void*, uint32_t a) {
    uint32_t raw = mem_read32(a); float f; memcpy(&f, &raw, 4); return f;
}
[[gnu::used]] void* __remill_write_memory_8(void* m, uint32_t a, uint8_t v) { mem_write8(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_16(void* m, uint32_t a, uint16_t v) { mem_write16(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_32(void* m, uint32_t a, uint32_t v) { mem_write32(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_64(void* m, uint32_t a, uint64_t v) { mem_write64(a,v); return m; }
[[gnu::used]] void* __remill_write_memory_f64(void* m, uint32_t a, double v) {
    uint64_t raw; memcpy(&raw, &v, 8); mem_write64(a, raw); return m;
}
[[gnu::used]] void* __remill_write_memory_f32(void* m, uint32_t a, float v) {
    uint32_t raw; memcpy(&raw, &v, 4); mem_write32(a, raw); return m;
}

// --- import dispatch ---
// Each sentinel is an IAT entry. __remill_function_call is invoked for
// `call [IAT]` indirect calls. The sentinel is read from memory and passed
// as the target address.
[[gnu::used]] void* __remill_function_call(State &state, uint32_t addr, void* mem) {
    switch (addr) {
        case 0xDEAD0001:  // GlobalMemoryStatus — fills struct at [esp+4], no return value
            // the lifted IR pushed the return addr; pop it
            state.gpr.rsp.dword += 4;
            // pop the arg (struct ptr) — stdcall, callee cleans 1 arg
            state.gpr.rsp.dword += 4;
            break;
        case 0xDEAD0002:  // GetCurrentProcess — returns -1 (pseudo handle)
            state.gpr.rax.dword = 0xFFFFFFFF;
            state.gpr.rsp.dword += 4;
            break;
        case 0xDEAD0003:  // GetCurrentProcessId — returns PID
            state.gpr.rax.dword = g_pid;
            state.gpr.rsp.dword += 4;
            break;
        case 0xDEAD0004:  // OpenProcess(access, inherit, pid) — stdcall, 3 args
            state.gpr.rax.dword = 0x100;  // fake handle
            state.gpr.rsp.dword += 4 + 12;  // ret addr + 3 args
            break;
        case 0xDEAD0005:  // CloseHandle(handle) — stdcall, 1 arg
            state.gpr.rax.dword = 1;
            state.gpr.rsp.dword += 4 + 4;
            break;
        case 0xDEAD0006:  // GetProcessMemoryInfo(handle, @counters, cb) — stdcall, 3 args
            if (g_mem_ok) {
                // read counters ptr from stack: [esp+8] (after ret addr)
                uint32_t counters = mem_read32(state.gpr.rsp.dword + 4 + 4);
                mem_write32(counters + 0x08, g_ws);  // WorkingSetSize at offset 8
                state.gpr.rax.dword = 1;  // success
            } else {
                state.gpr.rax.dword = 0;  // failure
            }
            state.gpr.rsp.dword += 4 + 12;  // ret addr + 3 args
            break;
        default:
            fprintf(stderr, "function_call: unknown sentinel %#x\n", addr);
            abort();
    }
    return mem;
}

[[gnu::used]] void* __remill_function_return(State &, uint32_t, void* mem) { return mem; }

// __remill_jump: dispatch to a lifted function, OR handle an import sentinel
// (the thunk at 0x9860c6 does jmp [0xa3b344] → reads sentinel 0xDEAD0006
// from memory → calls __remill_jump with 0xDEAD0006 as target).
[[gnu::used]] void* __remill_jump(State *state, uint32_t target, void* mem) {
    // check if it's an import sentinel
    if (target >= 0xDEAD0001 && target <= 0xDEAD0006) {
        // handle as import call (but via jump, not call — no return addr pushed)
        // The thunk is a jmp, so the caller's stack is intact.
        // We need to handle the import and then return to the thunk's caller.
        // But __remill_jump is a tail call in the lifted IR, so after we handle
        // the import, we need to return to the thunk's caller's return addr.
        // Actually, the thunk does: jmp [IAT]. In the original code, the caller
        // did `call 0x9860c6` which pushed a return addr. The thunk jumps to
        // the import. The import does `ret` which pops the return addr.
        // In our lifted model: caller calls sub_9860c6 (lifted thunk), which
        // calls __remill_jump(sentinel). We handle the import here and then
        // need to "return" — but __remill_jump is a tail call, so returning
        // from it returns to the thunk's caller, which is correct IF we
        // simulate the import's ret (pop the return addr that the original
        // caller pushed before calling the thunk).
        // Actually, the flow is:
        //   caller: call 0x9860c6  → pushes ret_addr, jumps to thunk
        //   thunk:  jmp [IAT]      → jumps to import (no push)
        //   import: ret            → pops ret_addr, returns to caller
        // In lifted IR:
        //   caller: call sub_9860c6 → C++ call to sub_9860c6
        //   sub_9860c6: __remill_jump(sentinel) → tail call to us
        //   we handle import, then return → returns to caller
        // The problem: the original caller pushed a return addr on the x86
        // stack before calling the thunk. The import's ret popped it. In our
        // model, the C++ call stack handles the return, but the x86 stack
        // still has the pushed return addr. We need to pop it.
        // BUT: in the lifted caller, `call 0x9860c6` was lifted as:
        //   push return_addr via __remill_write_memory_32
        //   call sub_9860c6
        // So the x86 stack has the return addr. The import's ret would pop it.
        // Since we're handling the import here, we must pop it.
        // However, GetProcessMemoryInfo is stdcall with 3 args, so it also
        // pops 3 args. The thunk caller pushed args before the call.
        // Actually, the caller of the thunk is FUN_008f9500 which did:
        //   push esi; push ecx; push 0x28; call 0x9860c6
        // Wait no, looking at the disasm:
        //   push 0x28; lea ecx,[esp+0x30]; mov esi,eax; push ecx; push esi; call 0x9860c6
        // So 3 args pushed (0x28, ecx, esi) + call (pushes ret addr).
        // The import (GetProcessMemoryInfo) is stdcall: ret 12 (pops 3 args)
        // plus the ret itself pops the ret addr. Total: 16 bytes popped.
        // But we're in a jmp (thunk), so the ret addr was pushed by the call
        // to the thunk. The import pops ret_addr + 3 args = 16 bytes.
        // In our model: the lifted caller pushed ret_addr + 3 args on the
        // x86 stack. We need to pop all 4 (16 bytes) to clean up.
        if (target == 0xDEAD0006) {  // GetProcessMemoryInfo, stdcall 3 args
            if (g_mem_ok) {
                uint32_t counters = mem_read32(state->gpr.rsp.dword + 4 + 4);
                mem_write32(counters + 0x08, g_ws);
                state->gpr.rax.dword = 1;
            } else {
                state->gpr.rax.dword = 0;
            }
            // pop ret_addr (4) + 3 args (12) = 16 bytes
            state->gpr.rsp.dword += 16;
        }
        return mem;  // tail call return → goes back to thunk's caller
    }

    // not a sentinel: dispatch to lifted function
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
[[gnu::used]] bool __remill_compare_uge(bool r) { return r; }
[[gnu::used]] bool __remill_compare_ult(bool r) { return r; }
[[gnu::used]] bool __remill_compare_ule(bool r) { return r; }
[[gnu::used]] bool __remill_compare_eq(bool r) { return r; }
[[gnu::used]] bool __remill_compare_neq(bool r) { return r; }
[[gnu::used]] bool __remill_compare_sgt(bool r) { return r; }
[[gnu::used]] bool __remill_compare_sge(bool r) { return r; }
[[gnu::used]] bool __remill_compare_slt(bool r) { return r; }
[[gnu::used]] bool __remill_compare_sle(bool r) { return r; }

// --- FPU exception stubs ---
[[gnu::used]] void __remill_fpu_exception_clear(uint32_t) {}
[[gnu::used]] uint32_t __remill_fpu_exception_test(uint32_t) { return 0; }

// --- symbolic bases ---
[[gnu::used]] uint32_t __remill_symbolic_FSBASE(void) { return 0; }
[[gnu::used]] uint32_t __remill_symbolic_GSBASE(void) { return 0; }
[[gnu::used]] uint32_t __remill_symbolic_STACK(void) { return 0; }

}  // extern "C"
