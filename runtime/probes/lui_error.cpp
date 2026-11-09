// wraps: sub_6491b0
// runtime/probes/lui_error.cpp — DEBUG PROBE, built only with PROBES=1 (scripts/package-game.sh). t6zm's popup
// opener at 0x6491b0: the LUI reports a script error by opening a popup through it (0x61bd6c: title "LUI NOTIFY EVENT
// ERROR:", the message from 0x7f3600), and a popup only shows on screen. This logs every call's two strings, so a menu
// script's error reaches the log whatever the popup does.
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>
#include <string>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" uint8_t win32_mem_read8(uint32_t va);
extern "C" void* sub_6491b0_lifted(State*, uint32_t, void*);

namespace {
std::string cstr(uint32_t p) {
    std::string s;
    if (p < 0x10000) return "-";
    for (uint32_t i = 0; i < 400; i++) {
        const uint8_t c = win32_mem_read8(p + i);
        if (!c) break;
        s += (c >= 32 && c < 127) ? (char)c : (c == '\n' ? '|' : '.');
    }
    return s;
}
}  // namespace

extern "C" void* sub_6491b0(State* s, uint32_t pc, void* mem) {
    const uint32_t esp = s->gpr.rsp.dword;
    const uint32_t a0 = win32_mem_read32(esp + 4), a4 = win32_mem_read32(esp + 20);
    fprintf(stderr, "[LUI-ERROR] popup(ret=0x%08x) message=\"%s\" title=\"%s\"\n", win32_mem_read32(esp),
            cstr(a0).c_str(), cstr(a4).c_str());
    return sub_6491b0_lifted(s, pc, mem);
}
