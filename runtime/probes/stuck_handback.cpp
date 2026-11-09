// wraps: sub_41f060
// runtime/probes/stuck_handback.cpp — DEBUG PROBE, built only with PROBES=1. t6zm's main-thread "take the client
// frame back from the render back end" (0x41f060: wait for the back end's ack [0x34c7640], clear it, then loop on
// Sleep(1)/pump/0x6dd6c0 until it is set again). Records the main thread's entry; stuck_common.h's watchdog prints
// the live chains of every thread seen when it stays there.
#include "stuck_common.h"

extern "C" void* sub_41f060_lifted(State*, uint32_t, void*);

extern "C" void* sub_41f060(State* s, uint32_t pc, void* mem) {
    stuck::Guard g(s, "sub_41f060");
    return sub_41f060_lifted(s, pc, mem);
}
