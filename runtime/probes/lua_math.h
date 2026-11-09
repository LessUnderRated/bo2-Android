// runtime/probes/lua_math.h — DEBUG PROBE support: wrap a Havok Script math library function (lua_State* first stack
// argument) and log its arguments and results, the first 40 calls and then every 50th. The zombies globe script turns
// sane map longitudes into -inf on this port; its only engine calls on the way are math functions, whose x87 bodies
// (fsin, fcos, fsqrt, fpatan, fyl2x ...) run through the lifter's float80 semantics.
#pragma once
#include "remill/Arch/X86/Runtime/State.h"
#include "lua_numbers.h"
#include <atomic>

#define LUA_MATH_PROBE(SYM, NAME)                                                                       \
    extern "C" void* SYM##_lifted(State*, uint32_t, void*);                                             \
    extern "C" void* SYM(State* s, uint32_t pc, void* mem) {                                            \
        static std::atomic<unsigned> n{0};                                                              \
        const unsigned k = ++n;                                                                         \
        const uint32_t L = win32_mem_read32(s->gpr.rsp.dword + 4);                                      \
        const uint32_t base = win32_mem_read32(L + 0x28), top0 = win32_mem_read32(L + 0x24);            \
        const std::string args = lua_slots(base, top0);                                                 \
        void* r = SYM##_lifted(s, pc, mem);                                                             \
        if (k <= 40 || k % 50 == 0)                                                                     \
            fprintf(stderr, "[LUA-MATH] %s #%u (%s ) ->%s\n", NAME, k, args.c_str(),                    \
                    lua_slots(top0, win32_mem_read32(L + 0x24)).c_str());                               \
        return r;                                                                                       \
    }
