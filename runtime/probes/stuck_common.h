// runtime/probes/stuck_common.h — DEBUG PROBE support, shared by the stuck_*.cpp probes. Each wrapped call records the
// thread's guest State and entry time; a watchdog host thread, for any call still in progress after 5 s, prints the
// thread's LIVE guest call chain (code addresses on its stack from its current esp, read from the State while it
// waits inside a host call) and the same for every other thread seen, every 10 s while it stays stuck.
#pragma once
#include "remill/Arch/X86/Runtime/State.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" int win32_current_tid(void);
// bo2-mac's LUI tracer (BO2_LUI_PROBE + BO2_LUI_TABLE): last named engine calls, and the value stack of a Lua state.
extern "C" __attribute__((weak)) void bo2_lui_ring_dump(uint32_t L);

namespace stuck {
struct Slot { State* s = nullptr; const char* fn = nullptr; std::chrono::steady_clock::time_point at; int depth = 0; uint32_t entry_esp = 0; bool dumped = false; };
inline std::mutex mu;
inline Slot slots[64];

inline void live_chain(State* s, char* out, size_t cap) {
    const uint32_t esp = s->gpr.rsp.dword;
    size_t k = (size_t)snprintf(out, cap, " esp=%x:", esp);
    for (uint32_t i = 0; i < 1500 && k + 12 < cap; i++) {
        const uint32_t v = win32_mem_read32(esp + 4 * i);
        if (v >= 0x401000 && v < 0xb00000) k += (size_t)snprintf(out + k, cap - k, " %x", v);
    }
}

inline void watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::lock_guard<std::mutex> lock(mu);
        const auto now = std::chrono::steady_clock::now();
        bool any = false;
        for (int t = 0; t < 64; t++)
            if (slots[t].depth > 0 && now - slots[t].at > std::chrono::seconds(5)) any = true;
        if (!any) continue;
        for (int t = 0; t < 64; t++) {
            if (!slots[t].s) continue;
            char c[1400];
            live_chain(slots[t].s, c, sizeof c);
            const long long secs = slots[t].depth > 0
                ? (long long)std::chrono::duration_cast<std::chrono::seconds>(now - slots[t].at).count() : -1;
            fprintf(stderr, "[STUCK] tid %d in %s for %lld s (entry esp %x):%s\n", t,
                    slots[t].depth > 0 ? slots[t].fn : "-", secs, slots[t].entry_esp, c);
            // Once per stuck call: if the innermost frame is a Havok Script call (0x8ff980, called with the Lua state
            // as its first stack argument), the script's last engine calls and that state's value stack.
            if (secs >= 5 && !slots[t].dumped && bo2_lui_ring_dump) {
                slots[t].dumped = true;
                // The Lua state: the first stack word naming a struct whose top (+0x24) and base (+0x28) look like a
                // value stack (base <= top, under 0x2000 bytes apart, 8-byte aligned difference).
                const uint32_t esp = slots[t].s->gpr.rsp.dword;
                uint32_t L = 0;
                for (uint32_t i = 0; i < 64 && !L; i++) {
                    const uint32_t v = win32_mem_read32(esp + 4 * i);
                    if (v < 0x10000 || (v & 3)) continue;
                    const uint32_t top = win32_mem_read32(v + 0x24), base = win32_mem_read32(v + 0x28);
                    if (base > 0x10000 && base <= top && top - base < 0x2000 && ((top - base) & 7) == 0) L = v;
                }
                bo2_lui_ring_dump(L);
                // Printable strings (>= 6 chars) on the stack between the live esp and the call's entry esp: names
                // the code above built locally (the UI's "transition_complete_<state>" event name, for one).
                std::string strs;
                std::string cur;
                const uint32_t hi = slots[t].entry_esp > esp && slots[t].entry_esp - esp < 0x8000 ? slots[t].entry_esp
                                                                                                    : esp + 0x2000;
                for (uint32_t a = esp; a < hi && strs.size() < 3000; a++) {
                    const uint8_t ch = (uint8_t)(win32_mem_read32(a & ~3u) >> (8 * (a & 3)));
                    if (ch >= 32 && ch < 127) { cur += (char)ch; continue; }
                    if (cur.size() >= 6) { char at[24]; snprintf(at, sizeof at, " [%x]", a - (uint32_t)cur.size()); strs += at; strs += cur; }
                    cur.clear();
                }
                fprintf(stderr, "[STUCK-STRINGS] tid %d:%s\n", t, strs.c_str());
            }
        }
    }
}

struct Guard {
    int t;
    Guard(State* s, const char* fn) {
        static std::once_flag once;
        std::call_once(once, [] { std::thread(watchdog).detach(); });
        const int tid = win32_current_tid();
        t = tid >= 0 && tid < 64 ? tid : 63;
        std::lock_guard<std::mutex> lock(mu);
        Slot& sl = slots[t];
        sl.s = s;
        if (sl.depth++ == 0) { sl.fn = fn; sl.at = std::chrono::steady_clock::now(); sl.entry_esp = s->gpr.rsp.dword; sl.dumped = false; }
    }
    ~Guard() { std::lock_guard<std::mutex> lock(mu); slots[t].depth--; }
};
}  // namespace stuck

namespace stuck {
// Records the calling thread's State without counting a call (for threads that never return to a wrapped caller).
inline void seen(State* s) {
    const int tid = win32_current_tid();
    const int t = tid >= 0 && tid < 64 ? tid : 63;
    std::lock_guard<std::mutex> lock(mu);
    if (!slots[t].s) slots[t].s = s;
}
}  // namespace stuck
