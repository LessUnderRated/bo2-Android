// win32_shim.h — Win32 import dispatch framework for the native port.
//
// Each imported function from the PE is registered with its IAT slot address
// and a native C++ implementation. At runtime, __remill_function_call reads
// the sentinel from the IAT slot, looks up the implementation, and calls it.
//
// Each implementation reads its args from the x86 stack (State.gpr.esp),
// does the work via POSIX/macOS APIs, writes the return value to EAX, and
// adjusts ESP for stdcall cleanup (callee pops args).
#pragma once
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <functional>

// An import implementation. Reads args from the x86 stack, writes return
// value to state.gpr.rax.dword, and adjusts ESP for stdcall cleanup.
// mem_read32/write32 are provided by the runtime for stack/memory access.
using ImportImpl = void(*)(State& state);

struct ImportRegistration {
    uint32_t iat_va;        // IAT slot address (used as sentinel)
    const char* dll;
    const char* name;
    ImportImpl impl;
    int n_args;             // stdcall arg count (for ESP cleanup)
};

// Initialize the shim: register all known imports, populate IAT slots.
extern "C" void win32_shim_init();

// Dispatch an import call. Called by __remill_function_call when the target
// is an IAT sentinel. Looks up the implementation and calls it.
extern "C" void win32_dispatch_import(State& state, uint32_t iat_va);

// Memory accessors (implemented in win32_runtime.cpp)
extern "C" uint32_t win32_mem_read32(uint32_t a);
extern "C" void win32_mem_write32(uint32_t a, uint32_t v);
extern "C" uint8_t win32_mem_read8(uint32_t a);
extern "C" void win32_mem_write8(uint32_t a, uint8_t v);

// BULK ACCESS. Guest memory is one flat 4 GiB mapping, so the byte at guest
// address `a` is simply `win32_guest_ptr(a)` and a bulk transfer is one host
// memcpy — no per-byte loop, no barrier per byte.
//
// ⚠️ WHICH IS EXACTLY WHY IT NEEDS ASKING FIRST. win32_mem_write8/32 special-case
// the IAT window, the render-handshake globals, the fastfile ring bookkeeping,
// the MT pool watchers and the write watchpoint; a bulk store landing in any of
// those silently loses the side effect. win32_bulk_dst_hooked() answers "would
// [a, a+n) lose one?" and is THE one list — do not write a second copy, because
// the copy that drifts will be the one that skips a hook. It also reports true
// for the whole run whenever BO2_WATCH_WRITE is armed.
//
// Returns nullptr if [a, a+n) leaves the 4 GiB space; callers must fall back to
// the byte loop for both that and a `true` from win32_bulk_dst_hooked.
extern "C" uint8_t* win32_guest_ptr(uint32_t a);
extern "C" bool win32_bulk_dst_hooked(uint32_t a, uint32_t n);

// Helper: read the Nth argument from the x86 stack.
// Stack layout at import entry: [esp]=return_addr, [esp+4]=arg0, [esp+8]=arg1...
// Note: Remill's State uses unified register names (rsp for esp, rax for eax)
inline uint32_t win32_arg(State& state, int index) {
    return win32_mem_read32(state.gpr.rsp.dword + 4 + index * 4);
}

// Helper: stdcall cleanup (pop return addr + n_args * 4)
inline void win32_stdcall_ret(State& state, int n_args) {
    state.gpr.rsp.dword += 4 + n_args * 4;
}

// Helper: cdecl cleanup (pop return addr only)
inline void win32_cdecl_ret(State& state) {
    state.gpr.rsp.dword += 4;
}

// Window messages. We ARE the window system, so the guest's WNDPROC is entered
// directly (win32_runtime.cpp). Only ever call win32_send_message on the guest
// thread that owns `state` — the procedure touches engine state owned by tid 0.
// Poll counters for the input probe. The [IMPORT] trace CANNOT answer "does the
// engine poll input?" — it is capped at 100k lines and the frame loop starts
// after the cap, so every per-frame call is invisible there. Count directly.
extern "C" unsigned g_n_cursor_poll;
extern "C" unsigned g_n_pad_poll;

extern "C" void win32_set_wndproc(uint32_t va);
extern "C" uint32_t win32_send_message(State* state, uint32_t hwnd, uint32_t msg,
                                       uint32_t wparam, uint32_t lparam);
extern "C" uint32_t win32_guest_hwnd();

// Import entry (name → implementation). Defined in kernel32_impl.cpp.
struct ImportEntry {
    const char* name;
    ImportImpl impl;
};
extern "C" const ImportEntry* get_kernel32_impls();
