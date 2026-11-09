// wraps: sub_92ec80
// runtime/probes/lua_getshadervector2.cpp — DEBUG PROBE, built only with PROBES=1. LUI's getShaderVector2 (0x92ec80:
// pushes the element's 4 floats at +0x78..+0x84). The zombies globe script (T6.Zombie.GameGlobeZombie) reads the
// globe's current rotation with it; after SOLO PLAY its MoveToCenter computes -inf. Logs every call: the results.
#include "remill/Arch/X86/Runtime/State.h"
#include "lua_numbers.h"

extern "C" int win32_current_tid(void);
extern "C" void* sub_92ec80_lifted(State*, uint32_t, void*);

extern "C" void* sub_92ec80(State* s, uint32_t pc, void* mem) {
    const uint32_t L = win32_mem_read32(s->gpr.rsp.dword + 4);
    const uint32_t top0 = win32_mem_read32(L + 0x24);
    void* r = sub_92ec80_lifted(s, pc, mem);
    const uint32_t top1 = win32_mem_read32(L + 0x24);
    fprintf(stderr, "[LUA-NUM] getShaderVector2 (tid %d) ->%s\n", win32_current_tid(), lua_slots(top0, top1).c_str());
    return r;
}
