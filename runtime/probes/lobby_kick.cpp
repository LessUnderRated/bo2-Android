// wraps: sub_697750
// runtime/probes/lobby_kick.cpp — DEBUG PROBE / EXPERIMENT, built only with PROBES=1. t6zm's per-frame UI update
// dispatcher (0x697750, argument: controller) sends each pending update (bit i of the controller's 64-bit mask at
// 0x2450e28 + 8*controller; names party, lobby, theater, game_options, ...) to the menus. The game lobby's script asks
// for "lobby" (Engine.SystemNeedsUpdate -> 0x6976f0 -> bit 1) only after its fetching_done event, which never comes on
// this port, so gamelobby_update is never sent and SOLO PLAY shows an empty lobby. EXPERIMENT: while the file named by
// BO2_LOBBY_KICK exists, set the "lobby" bit once a second, exactly as 0x6976f0 would, to see whether the lobby then
// builds and starts.
#include "remill/Arch/X86/Runtime/State.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" void win32_mem_write32(uint32_t va, uint32_t v);
extern "C" void* sub_697750_lifted(State*, uint32_t, void*);

extern "C" void* sub_697750(State* s, uint32_t pc, void* mem) {
    static const char* flag = getenv("BO2_LOBBY_KICK");
    static auto last = std::chrono::steady_clock::now();
    static unsigned kicks = 0, calls = 0;
    if ((++calls % 500) == 1) {
        struct stat st {};
        fprintf(stderr, "[LOBBY-KICK] dispatcher call #%u (controller %u, flag %s, game lobby running %u)\n", calls,
                win32_mem_read32(s->gpr.rsp.dword + 4), flag && ::stat(flag, &st) == 0 ? "present" : "absent",
                win32_mem_read32(0x11c7550u + 0x9590u));
    }
    if (flag) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(1)) {
            last = now;
            struct stat st {};
            if (::stat(flag, &st) == 0) {
                const uint32_t ctrl = win32_mem_read32(s->gpr.rsp.dword + 4);
                if (ctrl < 4) {
                    const uint32_t lo = 0x2450e28u + 8u * ctrl;
                    win32_mem_write32(lo, win32_mem_read32(lo) | 2u);   // bit 1: "lobby"
                    fprintf(stderr, "[LOBBY-KICK] #%u controller %u: 'lobby' update requested (EXPERIMENT)\n", ++kicks, ctrl);
                }
            }
        }
    }
    return sub_697750_lifted(s, pc, mem);
}
