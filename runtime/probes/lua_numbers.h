// runtime/probes/lua_numbers.h — DEBUG PROBE support: format a Havok Script value-stack slot (8 bytes, type tag
// first; 3 = number as a float, 4 = string with its bytes at +12).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

extern "C" uint32_t win32_mem_read32(uint32_t va);

inline std::string lua_slot(uint32_t slot) {
    const uint32_t tag = win32_mem_read32(slot) & 0xf, v = win32_mem_read32(slot + 4);
    char a[64];
    if (tag == 3) { float f; memcpy(&f, &v, 4); snprintf(a, sizeof a, " %g", f); return a; }
    if (tag == 4 && v > 0x10000) {
        std::string s = " \"";
        for (uint32_t k = 0; k < 40; k++) {
            const uint8_t c = (uint8_t)(win32_mem_read32((v + 12 + k) & ~3u) >> (8 * ((v + 12 + k) & 3)));
            if (!c) break;
            s += (c >= 32 && c < 127) ? (char)c : '.';
        }
        return s + "\"";
    }
    snprintf(a, sizeof a, " t%u:%x", tag, v);
    return a;
}
// All values from slot lo to hi (exclusive).
inline std::string lua_slots(uint32_t lo, uint32_t hi) {
    std::string r;
    for (uint32_t p = lo; p < hi && p - lo < 8 * 12; p += 8) r += lua_slot(p);
    return r;
}
