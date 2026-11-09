// wraps: sub_598200
// runtime/probes/init_flag.cpp — DEBUG PROBE, built only with PROBES=1. t6zm's "$init in progress" flag setter
// (0x598200: byte [0x12086c5] = arg). The database wait loop (0x599088..0x5990ec) spins while that flag is set,
// pumping events ("Waiting for $init to finish..."); after SOLO PLAY the main thread stays in it. The flag is set at
// 0x6c8a4b (push 1) at the start of an init sequence and cleared at 0x6c92f5 at its end. Logs every call: the value,
// the caller, the thread, and the code addresses on the guest stack (the call chain).
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" int win32_current_tid(void);
extern "C" void* sub_598200_lifted(State*, uint32_t, void*);

extern "C" void* sub_598200(State* s, uint32_t pc, void* mem) {
    const uint32_t esp = s->gpr.rsp.dword;
    char chain[600];
    int n = 0;
    for (uint32_t i = 1; i < 160 && n < (int)sizeof chain - 12; i++) {
        const uint32_t v = win32_mem_read32(esp + 4 * i);
        if (v >= 0x401000 && v < 0xb00000) n += snprintf(chain + n, sizeof chain - (size_t)n, " %x", v);
    }
    chain[n] = 0;
    fprintf(stderr, "[INIT-FLAG] set %u (caller 0x%08x, tid %d) chain:%s\n", win32_mem_read32(esp + 4) & 0xff,
            win32_mem_read32(esp), win32_current_tid(), chain);
    return sub_598200_lifted(s, pc, mem);
}
