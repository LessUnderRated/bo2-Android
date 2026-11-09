// wraps: sub_6dd6a0
// runtime/probes/stuck_seen_ack.cpp — DEBUG PROBE, built only with PROBES=1. The render back end's frame ack
// (0x6dd6a0); registers the calling thread with stuck_common.h so its live chain is printed when another thread is stuck.
#include "stuck_common.h"

extern "C" void* sub_6dd6a0_lifted(State*, uint32_t, void*);

extern "C" void* sub_6dd6a0(State* s, uint32_t pc, void* mem) {
    stuck::seen(s);
    return sub_6dd6a0_lifted(s, pc, mem);
}
