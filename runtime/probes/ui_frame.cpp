// wraps: sub_9345b0
// runtime/probes/ui_frame.cpp — DEBUG PROBE, built only with PROBES=1. t6zm's per-frame UI update for one controller
// (0x9345b0: the pending-update dispatcher 0x697750, then the menu frame), called from five places in the client frame.
// Counts its calls (a line every 500th) to see whether it still runs while the Zombies game lobby is up.
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" void* sub_9345b0_lifted(State*, uint32_t, void*);

extern "C" void* sub_9345b0(State* s, uint32_t pc, void* mem) {
    static unsigned calls = 0;
    if ((++calls % 500) == 1)
        fprintf(stderr, "[UI-FRAME] call #%u (from 0x%08x, lua state 0x%08x)\n", calls, win32_mem_read32(s->gpr.rsp.dword),
                win32_mem_read32(0x31708a0u));
    return sub_9345b0_lifted(s, pc, mem);
}
