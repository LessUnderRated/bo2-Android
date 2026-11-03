// bo2_replay.h — deterministic record/replay of the host→guest boundary.
//
// WHY THIS CAN WORK HERE AND IS EXPENSIVE ANYWHERE ELSE: every guest thread is a
// ucontext coroutine multiplexed onto ONE host thread (GUIDE §4.2), so the guest
// program is deterministic by construction. The only entropy in the system enters
// through the host shims — the clock, the input FIFOs, file-I/O results and the
// order in which asynchronous work reports done.
//
// MEASURED BEFORE BUILT (commit 314cf38): two menu runs of the same build with the
// same flags are import-for-import, ret-for-ret, tid-for-tid IDENTICAL for the
// first 8,401 calls and then take one different branch. So the journal does not
// have to capture the program, only a handful of scalars at a boundary that is
// already one function.
//
//   BO2_REC=<path>        record a journal while running normally
//   BO2_REPLAY=<path>     serve that journal instead of the real world
//   BO2_REPLAY_CHECK=1    on replay ALSO call the real shim and report every
//                         place it disagrees with the journal (slow; it is the
//                         measurement of which shims are actually non-deterministic)
//   BO2_JRNL_ALL=1        journal EVERY import, not just the selected ones
//                         (~10x bigger; use to prove a missing selector)
//
// ⚠️ THE DIVERGENCE CHECK IS THE WHOLE INSTRUMENT. Each record carries the
// import ordinal, the IAT slot and the guest tid. If the replayed guest asks for
// a different import, or asks at a different ordinal, replay says so with the
// exact sequence number instead of quietly serving the wrong value. A replay that
// silently "works" by forcing values would be the capped-probe failure again, one
// level up: it could not tell "reproduced" from "papered over".
#pragma once
#include <cstdint>

struct State;

// Hot path: true only while a recorded shim is executing. Checked once per guest
// memory write, so it is an extern bool and not a function call.
extern "C" bool g_bo2_jrnl_capturing;
extern "C" void bo2_w32_armed_recompute();   // defined with the write barrier

extern "C" {

// Called once from win32_shim_init(), after the IAT dispatch table exists.
void bo2_journal_init();

// Is either mode armed at all? (One load; false in a normal run.)
bool bo2_journal_active();
bool bo2_journal_replaying();

// Is this IAT slot one we journal? Cheap: a bitset over the IAT range.
bool bo2_journal_selects(uint32_t iat_va);

// Recording: bracket the real shim call.
void bo2_journal_begin(State& s, uint32_t iat_va, uint32_t seq);
void bo2_journal_end  (State& s, uint32_t iat_va);

// Replaying: returns true when the call was served from the journal (the real
// shim must then NOT run). Returns false after reporting a divergence, so the
// caller falls back to the real shim and the run continues to its own end —
// stopping at the first divergence would hide every later one.
bool bo2_journal_serve(State& s, uint32_t iat_va, uint32_t seq);

// The per-frame window-message drain — the SECOND host->guest boundary, and not
// an import. Wraps gfx_input_pop: records what the host delivered, or serves what
// the journal recorded (discarding anything typed during the replay).
bool bo2_journal_input_pop(uint32_t* msg, uint32_t* wp, uint32_t* lp);

// The audio-completion credit — the THIRD host->guest boundary. CoreAudio's
// consumption rate decides how many times the guest's OnBufferEnd runs per pump.
uint32_t bo2_journal_audio_take(uint32_t unit, uint32_t max);

// Every guest memory write made from the host side, via the write barrier.
void bo2_journal_note_write_slow(uint32_t addr, uint32_t val, uint32_t width);

// Prints counts unconditionally, including the zeros — a silent journal and an
// unarmed one must never read the same. Safe to call more than once.
void bo2_journal_report(const char* why);

}  // extern "C"

static inline void bo2_journal_note_write(uint32_t addr, uint32_t val, uint32_t width) {
    if (__builtin_expect(g_bo2_jrnl_capturing, 0))
        bo2_journal_note_write_slow(addr, val, width);
}
