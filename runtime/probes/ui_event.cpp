// wraps: sub_930360
// runtime/probes/ui_event.cpp — DEBUG PROBE, built only with PROBES=1 (scripts/package-game.sh). t6zm's engine-to-UI
// notify at 0x930360 (root, event name, ...): 130 call sites hand it a fixed name (gamelobby_update,
// partylobby_update, party_update_status, ...). Logs every event the engine sends the menus, by name, each name
// every time for the first 40 and then every 500th, so a menu waiting on an event that never comes shows up.
#include "remill/Arch/X86/Runtime/State.h"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" uint8_t win32_mem_read8(uint32_t va);
extern "C" void* sub_930360_lifted(State*, uint32_t, void*);

extern "C" void* sub_930360(State* s, uint32_t pc, void* mem) {
    const uint32_t esp = s->gpr.rsp.dword;
    const uint32_t name = win32_mem_read32(esp + 8);
    std::string n;
    if (name > 0x10000)
        for (uint32_t i = 0; i < 64; i++) {
            const uint8_t c = win32_mem_read8(name + i);
            if (!c) break;
            n += (c >= 32 && c < 127) ? (char)c : '.';
        }
    static std::mutex mu;
    static std::unordered_map<std::string, uint32_t> count;
    {
        std::lock_guard<std::mutex> lock(mu);
        const uint32_t k = ++count[n];
        if (k <= 40 || k % 500 == 0)
            fprintf(stderr, "[UI-EVENT] %s #%u (from 0x%08x)\n", n.empty() ? "?" : n.c_str(), k, win32_mem_read32(esp));
    }
    return sub_930360_lifted(s, pc, mem);
}
