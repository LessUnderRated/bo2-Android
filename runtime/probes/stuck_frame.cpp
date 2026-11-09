// wraps: sub_55c5d0
// runtime/probes/stuck_frame.cpp — DEBUG PROBE, built only with PROBES=1. t6zm's client-frame driver (0x55c5d0), run
// by the main frame and, while the main thread is busy, by the render back end (0x43d0f0, while [0x34c7644] != 0).
// After SOLO PLAY the main thread waits in 0x41f060 for the back end to hand back ([0x34c7640]); this records which
// thread is inside a client frame, and stuck_common.h's watchdog prints its live call chain if it stays there.
#include "stuck_common.h"

extern "C" void* sub_55c5d0_lifted(State*, uint32_t, void*);

extern "C" void* sub_55c5d0(State* s, uint32_t pc, void* mem) {
    stuck::Guard g(s, "sub_55c5d0");
    return sub_55c5d0_lifted(s, pc, mem);
}
