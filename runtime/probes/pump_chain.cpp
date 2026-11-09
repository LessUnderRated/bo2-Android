// wraps: sub_77e500
// runtime/probes/pump_chain.cpp — DEBUG PROBE, built only with PROBES=1. t6zm's event-pump wrapper (0x77e500: the
// message pump 0x77e1a0), called by the main frame and by every loop that waits while keeping the window alive. After
// SOLO PLAY the client frame stops (its re-entrancy guard 0x11c45c0 stays set) while events are still pumped: some loop
// inside the client frame never returns. Logs the guest call chain (code addresses on the stack) on the 1st and every
// 200th pump, so the chain names the loop the main thread is in.
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" int win32_current_tid(void);
extern "C" void* sub_77e500_lifted(State*, uint32_t, void*);

extern "C" void* sub_77e500(State* s, uint32_t pc, void* mem) {
    static unsigned n = 0;
    if ((++n % 200) == 1) {
        const uint32_t esp = s->gpr.rsp.dword;
        char chain[700];
        int k = 0;
        for (uint32_t i = 0; i < 200 && k < (int)sizeof chain - 12; i++) {
            const uint32_t v = win32_mem_read32(esp + 4 * i);
            if (v >= 0x401000 && v < 0xb00000) k += snprintf(chain + k, sizeof chain - (size_t)k, " %x", v);
        }
        chain[k] = 0;
        fprintf(stderr, "[PUMP-CHAIN] #%u tid %d guard %u:%s\n", n, win32_current_tid(),
                win32_mem_read32(0x11c45c0u) & 0xff, chain);
    }
    return sub_77e500_lifted(s, pc, mem);
}
