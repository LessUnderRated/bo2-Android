// wraps: sub_92ff40
// runtime/probes/anim_delta.cpp — DEBUG PROBE, built only with PROBES=1. LUI's per-element animation step (0x92ff40:
// remaining [elem+0x100] -= delta; progress = 1 - remaining/duration [elem+0x104], clamped only above; then the
// interpolation 0x929620). The delta is the 3rd argument, or the 2nd when the element uses game time (flag 0x10 at
// [elem+0xc0]). In the zombies menu the globe's animated shader vector reaches -inf and its Lua angle wrap never ends;
// a negative or huge delta (or a duration of 0 with time remaining) extrapolates the interpolation. Logs the first 40
// anomalous steps, and one line every 2000 steps with the deltas seen.
#include "remill/Arch/X86/Runtime/State.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" uint32_t win32_mem_read32(uint32_t va);
extern "C" void* sub_92ff40_lifted(State*, uint32_t, void*);

extern "C" void* sub_92ff40(State* s, uint32_t pc, void* mem) {
    static std::atomic<unsigned> n{0}, bad{0};
    const uint32_t esp = s->gpr.rsp.dword;
    const uint32_t el = win32_mem_read32(esp + 4);
    const int32_t d_ui = (int32_t)win32_mem_read32(esp + 8), d_game = (int32_t)win32_mem_read32(esp + 12);
    const bool game = (win32_mem_read32(el + 0xc0) & 0x10) != 0;
    const int32_t d = game ? d_ui : d_game;
    const int32_t rem = (int32_t)win32_mem_read32(el + 0x100), dur = (int32_t)win32_mem_read32(el + 0x104);
    const unsigned k = ++n;
    const bool odd = rem > 0 && (d < 0 || d > 5000 || dur <= 0 || rem > dur);
    if (odd && bad.fetch_add(1) < 40)
        fprintf(stderr, "[ANIM-DELTA] odd step #%u el %x delta %d (ui %d game %d, uses %s) remaining %d duration %d\n", k,
                el, d, d_ui, d_game, game ? "2nd" : "3rd", rem, dur);
    if (k % 2000 == 1)
        fprintf(stderr, "[ANIM-DELTA] step #%u el %x delta ui %d game %d remaining %d duration %d (odd so far %u)\n", k,
                el, d_ui, d_game, rem, dur, bad.load());
    void* r = sub_92ff40_lifted(s, pc, mem);
    // The element's shader vector after the step (what getShaderVector2 reads): report the first non-finite value.
    static std::atomic<bool> told{false};
    for (uint32_t off = 0x78; off <= 0x84; off += 4) {
        const uint32_t b = win32_mem_read32(el + off);
        if ((b & 0x7f800000u) == 0x7f800000u && !told.exchange(true)) {
            float f;
            memcpy(&f, &b, 4);
            fprintf(stderr, "[ANIM-DELTA] el %x shader vector +0x%x became %g at step #%u (delta %d remaining %d "
                            "duration %d)\n", el, off, f, k, d, rem, dur);
        }
    }
    return r;
}
