// wraps: sub_92e0a0
// runtime/probes/lua_setshadervector.cpp — DEBUG PROBE, built only with PROBES=1. LUI's setShaderVector (0x92e0a0:
// element, index, x, y, z, w; written into the element's pending animation state when it has one). Logs every call's
// arguments, to see which call first carries a non-finite value in the zombies globe script.
#include "remill/Arch/X86/Runtime/State.h"
#include "lua_numbers.h"

extern "C" void* sub_92e0a0_lifted(State*, uint32_t, void*);

extern "C" void* sub_92e0a0(State* s, uint32_t pc, void* mem) {
    const uint32_t L = win32_mem_read32(s->gpr.rsp.dword + 4);
    fprintf(stderr, "[LUA-NUM] setShaderVector%s\n",
            lua_slots(win32_mem_read32(L + 0x28), win32_mem_read32(L + 0x24)).c_str());
    return sub_92e0a0_lifted(s, pc, mem);
}
