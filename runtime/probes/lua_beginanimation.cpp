// wraps: sub_930880
// runtime/probes/lua_beginanimation.cpp — DEBUG PROBE, built only with PROBES=1. LUI's beginAnimation (0x930880:
// element, state name, duration ms, ease in, ease out). Logs calls whose duration is not finite or <= 0 (the zombies
// globe's MoveToCenter passes -inf after SOLO PLAY), and the first 30 calls, with every argument.
#include "remill/Arch/X86/Runtime/State.h"
#include "lua_numbers.h"
#include <atomic>
#include <cmath>

extern "C" void* sub_930880_lifted(State*, uint32_t, void*);

extern "C" void* sub_930880(State* s, uint32_t pc, void* mem) {
    static std::atomic<unsigned> n{0}, odd_n{0};
    const uint32_t L = win32_mem_read32(s->gpr.rsp.dword + 4);
    const uint32_t base = win32_mem_read32(L + 0x28), top = win32_mem_read32(L + 0x24);
    const unsigned k = ++n;
    bool odd = false;
    if (top >= base + 24 && (win32_mem_read32(base + 16) & 0xf) == 3) {
        const uint32_t v = win32_mem_read32(base + 20);
        float f; memcpy(&f, &v, 4);
        odd = !std::isfinite(f) || f <= 0;
    }
    if (k <= 30 || (odd && odd_n.fetch_add(1) < 300)) fprintf(stderr, "[LUA-NUM] beginAnimation #%u%s\n", k, lua_slots(base, top).c_str());
    return sub_930880_lifted(s, pc, mem);
}
