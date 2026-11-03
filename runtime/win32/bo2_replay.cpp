// bo2_replay.cpp — see bo2_replay.h for the argument this rests on.
#include "remill/Arch/X86/Runtime/State.h"
#include "bo2_getenv.h"
#include "win32_shim.h"
#include "bo2_replay.h"
#if defined(BO2_TARGET_SPS)
#include "iat_table_sps.h"
#elif defined(BO2_TARGET_MPS)
#include "iat_table_mps.h"
#else
#include "iat_table_zms.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" int  win32_current_tid();
extern "C" int  g_import_call_count_probe();
extern "C" bool gfx_input_pop(uint32_t* msg, uint32_t* wp, uint32_t* lp);
extern "C" uint32_t bo2_audio_take_completed(uint32_t unit, uint32_t max);
extern "C" void win32_err_catalogue_report();
extern "C" void win32_w32audit_report();
extern "C" void win32_tickgate_report();
extern "C" void win32_mtpop_report();
extern "C" void win32_allocindex_report();
extern "C" void bo2_probe_report(const char* why);
extern "C" void bo2_event_report(const char* why);
extern "C" void bo2_jobq_report(const char* why);
extern "C" void bo2_rdraw_report(const char* why);
extern "C" void bo2_sndworker_report(const char* why);
extern "C" void bo2_sndsplit_report(const char* why);
extern "C" void bo2_sndpool_report(const char* tag, int verbose);
extern "C" void bo2_sndcall_report(const char* why);
extern "C" void bo2_yield_report(const char* why);
extern "C" void bo2_soundhandle_report(const char* why);
extern "C" void bo2_gscsweep_report(const char* why);
extern "C" void bo2_mtindex_report(const char* why);
extern "C" void bo2_gscconcat_report(const char* why);
extern "C" void bo2_internwho_report(const char* why);
extern "C" void bo2_slget_report(const char* why);
extern "C" void bo2_dispatch_report(const char* why);
extern "C" void bo2_freevars_report(const char* why);
extern "C" void bo2_errrank_report(const char* why);
extern "C" void bo2_removeent_report(const char* why);
extern "C" void bo2_freezero_report(const char* why);
extern "C" void win32_sl_band_report(void);
extern "C" void bo2_draintag_report(const char* why);
extern "C" void bo2_selfnull_report(const char* why);
extern "C" void bo2_unlinkwild_report(const char* why);
extern "C" void bo2_newvarblock_report(const char* why);
extern "C" void bo2_threadinstall_report(const char* why);
extern "C" void bo2_whoinstalls_report(const char* why);
extern "C" void bo2_scopenew_report(const char* why);
extern "C" void bo2_allocthread_report(const char* why);
extern "C" void bo2_lifehist_report(const char* why);
extern "C" void bo2_loopobj_report(const char* why);
extern "C" void bo2_varfree_report(const char* why);
extern "C" void bo2_order_report(const char* why);
extern "C" void bo2_varpool_report(const char* why);
extern "C" uint32_t win32_mem_read32(uint32_t a);
extern "C" void win32_mem_write16(uint32_t a, uint16_t v);

// ── the journal file format ────────────────────────────────────────────────
// Fixed-size records so a reader can seek, and so a truncated tail is obvious
// rather than silently reinterpreted.
namespace {

constexpr char   kMagic[8] = {'B','O','2','J','R','N','L','1'};
constexpr uint32_t kVersion = 1;

#pragma pack(push, 1)
struct Header {
    char     magic[8];
    uint32_t version;
    uint32_t flags;          // bit0 = journalled every import
};
struct Rec {
    uint32_t seq;            // the import ordinal (g_import_call_count)
    uint32_t iat_va;
    uint16_t tid;
    uint16_t nwrites;
    uint32_t eax;
    uint32_t edx;
    uint32_t esp_in;
    uint32_t esp_out;
};
struct Wr { uint32_t addr; uint32_t val; uint32_t width; };
#pragma pack(pop)

// ── the selector ───────────────────────────────────────────────────────────
// The imports whose result the host, not the guest, decides. Grouped by WHY,
// because the reason is what the next reader needs in order to add to the list.
const char* const kSelected[] = {
    // the clock — the dominant source. 4,615 timeGetTime + 1,279 QPC in the
    // first 100k imports of a menu run, and 3951-vs-3853 between two runs.
    "timeGetTime","QueryPerformanceCounter","QueryPerformanceFrequency",
    "GetTickCount","GetTickCount64","GetSystemTimeAsFileTime","GetLocalTime",
    "GetSystemTime","GetSystemTimeAdjustment","FileTimeToLocalFileTime",
    "FileTimeToSystemTime","SystemTimeToFileTime",
    // entropy
    "CryptGenRandom","CryptAcquireContextA","CryptReleaseContext",
    "SystemFunction036","RtlGenRandom",
    // input — the FIFOs and the live macOS cursor (impl_GetCursorPos reads
    // CGEventGetLocation, so it is a different value every single call)
    "GetCursorPos","PeekMessageA","GetMessageA","GetAsyncKeyState","GetKeyState",
    "GetKeyboardState","GetForegroundWindow","GetActiveWindow",
    // waiting and completion ORDER — the reason two runs take a different
    // number of turns through the same poll loop
    "WaitForSingleObject","WaitForSingleObjectEx","WaitForMultipleObjects",
    "Sleep","SleepEx","SwitchToThread","GetQueuedCompletionStatus",
    "GetOverlappedResult","MsgWaitForMultipleObjects",
    // file I/O RESULTS ONLY. The data itself is not journalled and does not need
    // to be: the same files are on the same disk. What is not reproducible is the
    // handle numbering, the byte count and the error.
    "ReadFile","WriteFile","CreateFileA","CreateFileW","SetFilePointer",
    "SetFilePointerEx","GetFileSize","GetFileSizeEx","FindFirstFileA",
    "FindNextFileA","FindClose","GetFileAttributesA","GetFileType","GetFileTime",
    "DeleteFileA","CloseHandle",
    // the host machine itself
    "GetLastError","GlobalMemoryStatus","GlobalMemoryStatusEx","GetSystemInfo",
    "GetDiskFreeSpaceA","GetDiskFreeSpaceExA","GetCurrentProcessId",
    "GetComputerNameA","GetEnvironmentVariableA","GetEnvironmentVariableW",
    "GetModuleFileNameA","GetModuleFileNameW","GetCommandLineA",
};

// ── TWO CLASSES, AND THE DIFFERENCE IS THE WHOLE CORRECTNESS ARGUMENT ──────
//
// SERVE — the journal REPLACES the call. Only for values the host alone decides
// and that cannot be recomputed: the clock, entropy, the live macOS cursor, the
// machine's own description. Nothing here has a side effect worth reproducing.
//
// CHECK — the shim RUNS FOR REAL and its result is compared against the journal.
// Everything else selected. This is not a weaker form of replay, it is the
// correct one, and the first replay proved it: file I/O was in SERVE, so
// ReadFile returned the recorded byte count while the real read never happened
// and the guest's BUFFER WAS NEVER FILLED. It diverged 3 imports later
// (expected ReadFile at seq 831, got CloseHandle at 834). "The same files are on
// the same disk" is an argument for RE-RUNNING the read, not for skipping it.
//
// The same applies with more force to Sleep and WaitForSingleObject: their
// point is that they YIELD. Serving a return value without yielding would leave
// every other coroutine unscheduled, which is not a replay of the run, it is a
// different program.
//
// The divergence detector covers BOTH classes, because it keys on the ordinal,
// the slot and the thread — not on the value. So a CHECK-class import that turns
// out to be non-deterministic reports itself and can be promoted to SERVE with a
// measurement behind the move instead of a guess.
const char* const kServe[] = {
    "timeGetTime","QueryPerformanceCounter","QueryPerformanceFrequency",
    "GetTickCount","GetTickCount64","GetSystemTimeAsFileTime","GetLocalTime",
    "GetSystemTime","GetSystemTimeAdjustment",
    "CryptGenRandom","SystemFunction036","RtlGenRandom",
    "GetCursorPos",
    "GetCurrentProcessId","GlobalMemoryStatus","GlobalMemoryStatusEx",
    "GetSystemInfo","GetDiskFreeSpaceA","GetDiskFreeSpaceExA","GetComputerNameA",
};

// ⚠️ THESE SHIMS YIELD, AND A YIELD RUNS SOMEBODY ELSE'S CODE.
// impl_Sleep and impl_WaitForSingleObject call into the cooperative scheduler,
// so between bo2_journal_begin and bo2_journal_end another guest coroutine runs
// its OWN lifted code — and every guest store it makes goes through the same
// write barrier. The first recording captured 29,270,294 writes for 1,254,940
// records: 23 per call, almost none of them host->guest. They are the other
// threads' ordinary work, and replaying them would overwrite live guest memory
// with a stale snapshot.
//
// None of these has an out-parameter — they return a status in eax and nothing
// else — so nothing is lost by not capturing. bo2_journal_note_write_slow also
// carries a tid guard as the general net, for the next shim that learns to yield.
const char* const kNoCapture[] = {
    "Sleep","SleepEx","SwitchToThread","WaitForSingleObject","WaitForSingleObjectEx",
    "WaitForMultipleObjects","MsgWaitForMultipleObjects",
    // yields under BO2_CS_COOP when another guest thread holds the section
    "EnterCriticalSection",
};

// ── state ──────────────────────────────────────────────────────────────────
// ⚠️ RAW fd, NOT a FILE*. scripts/menu_run.sh and every other harness end a run
// with `kill`, and SIGTERM's default action skips atexit and stdio flushing — a
// journal written through stdio would lose its tail EVERY TIME and read as a
// short recording rather than as a lost one. write(2) is async-signal-safe, so
// the same flush works from the handler.
int    g_fd        = -1;
std::vector<uint8_t> g_buf;
constexpr size_t kFlushAt = 1u << 16;   // 64 KiB
bool   g_recording = false;
bool   g_replaying = false;
bool   g_check     = false;      // BO2_REPLAY_CHECK
bool   g_all       = false;      // BO2_JRNL_ALL
bool   g_inited    = false;
bool   g_dead      = false;      // journal exhausted / unusable
volatile sig_atomic_t g_closing = 0;

void jrnl_flush() {
    if (g_fd < 0 || g_buf.empty()) return;
    size_t off = 0;
    while (off < g_buf.size()) {
        ssize_t n = write(g_fd, g_buf.data() + off, g_buf.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    g_buf.clear();
}
void jrnl_put(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    g_buf.insert(g_buf.end(), b, b + n);
    if (g_buf.size() >= kFlushAt) jrnl_flush();
}

std::unordered_set<uint32_t> g_sel;      // selected IAT slots
std::unordered_set<uint32_t> g_nocap;    // ...of which these must not capture writes
std::unordered_set<uint32_t> g_serve;    // ...of which these are REPLACED on replay
uint64_t g_n_replaced = 0, g_n_rerun = 0, g_n_valuediff = 0;

// ⚠️ THE FRAME STACK IS PER GUEST THREAD, AND IT HAS TO BE.
// One shared stack was wrong in a way that took two verify cycles to see: tid 0
// enters Sleep -> begin() pushes ITS frame -> Sleep yields -> tid 2 enters a
// recorded shim -> begin() pushes ANOTHER frame -> control returns to tid 0 ->
// Sleep returns -> end() pops back() and gets TID 2'S FRAME. The record then
// carries one call's ordinal and another call's slot, and the ordinal it should
// have had is never written at all. That is what produced
// "[JRNL-DIVERGE] seq=8743 ... the recording has NO record at this ordinal"
// while the two runs' [IMPORT] streams were identical for all 100,000 traced
// calls. Same for the write buffer: a shared one cannot be truncated back to a
// base index when two coroutines are appending to it in turn.
constexpr int kMaxTid = 64;
struct Frame { uint32_t seq, iat_va, esp_in; size_t base; uint16_t tid; bool capture; };
std::vector<Frame> g_frames[kMaxTid];
std::vector<Wr>    g_wr[kMaxTid];
inline int cur_slot() {
    const int t = win32_current_tid();
    return (t >= 0 && t < kMaxTid) ? t : kMaxTid - 1;
}

// Replay: the whole journal in memory. A menu run is ~30 MiB of records; the
// machine this runs on has 16 GB and no swap headroom, so the reader mmaps
// nothing clever and simply streams with a one-record lookahead.
// ⚠️ THE FILE IS NOT IN ORDINAL ORDER, and assuming it was cost a whole replay.
// A shim that yields (Sleep, WaitForSingleObject) runs other coroutines between
// begin and end, and THEIR records are complete first — so the yielding call's
// own record is written after every record produced during its yield. Consuming
// the file sequentially therefore serves the wrong record from the first yield
// onward, and reports a divergence at the wrong place: it claimed seq 8743 while
// the raw [IMPORT] streams of the two runs agreed to 45,876.
//
// So records are indexed BY ORDINAL. g_import_call_count is unique per dispatch,
// which makes it an exact key and removes the ordering assumption entirely.
std::vector<Rec> g_recs;
// ⚠️ A FLAT ARRAY, NOT A HASH MAP. Ordinals are dense (0..last_seq with ~60%
// occupancy on a real journal), so std::unordered_map cost ~48 bytes per entry
// — roughly 170 MB for a 3.5M-record journal, on top of the records themselves.
// That memory is not free here: it is paged, it grows the macOS swap file, and
// swap growth during a replay has twice filled this machine's disk mid-run. A
// uint32 per ordinal is 4 bytes and O(1) with no hashing or rehash spikes.
// kNoRec marks an ordinal the recording never made.
constexpr uint32_t kNoRec = 0xffffffffu;
std::vector<uint32_t> g_by_seq;                    // ordinal -> index into g_recs
std::vector<bool> g_used;
std::vector<Rec> g_inputs;
size_t g_next_input = 0;
uint64_t g_n_input_written = 0, g_n_input_served = 0, g_n_input_dropped = 0;
std::vector<Rec> g_audio;
size_t g_next_audio = 0;
uint64_t g_n_audio_written = 0, g_n_audio_served = 0;
std::vector<Wr>  g_wrs;          // flattened, indexed by Rec order
std::vector<uint32_t> g_wroff;   // offset of each record's writes in g_wrs
size_t g_next = 0;

// counters — every one of these prints even when zero
uint64_t g_n_written = 0, g_n_served = 0, g_n_diverge = 0, g_n_checkfail = 0;
uint64_t g_n_inflight = 0, g_n_inflight_hit = 0, g_n_past_end = 0;
bool     g_said_past = false;
uint64_t g_n_seen = 0, g_n_unselected = 0;
uint32_t g_first_diverge_seq = 0xffffffffu;
char     g_first_diverge[256] = {0};

// A complete journal ends with this sentinel. Without it the reader must say
// TRUNCATED rather than quietly serve a short recording — "the replay ran past
// the end" and "the recording was cut off" are opposite findings.
constexpr uint32_t kEndSeq = 0xffffffffu;

// A record whose tid carries this bit was STILL IN FLIGHT when the recording
// ended: the shim had been entered and had not returned, so journal_end never
// ran and nothing was ever written for it. Without this the replay reports
// "the recording has NO record at this ordinal" — which is the message for a
// genuine divergence, and the two are opposite findings. Measured: every one of
// the 8 "divergences" in the first clean replay was one of these, all at >=92%
// of the recording's length, with the [IMP-HASH] stream still identical past
// them. Threads parked in WaitForSingleObject when the harness `kill`s the run.
constexpr uint16_t kInFlight = 0x8000u;

// ── THE SECOND BOUNDARY ────────────────────────────────────────────────────
// Input does NOT arrive through an import. metal_backend.mm turns NSEvents (and
// the BO2_KEYPIPE / BO2_CLICKPIPE FIFOs) into WM_ messages on a host queue, and
// win32_runtime's per-frame pump drains that queue straight into the guest's
// WNDPROC. So a journal that only covered imports would replay a run with the
// keyboard unplugged and call the resulting divergence the guest's fault.
//
// Input records share the file with import records and are told apart by
// iat_va == 0, which is not a valid IAT slot. They are kept in their OWN cursor
// on load, because the import stream and the input stream are consumed by
// different code at different moments and interleaving them in one cursor would
// make each look like a divergence to the other.
constexpr uint32_t kInputSlot = 0u;

// ── THE THIRD BOUNDARY ─────────────────────────────────────────────────────
// bo2_audio_take_completed() returns HOW MANY audio buffers CoreAudio has
// consumed since the last pump — pure host timing — and bo2_xaudio_pump then
// runs the guest's OnBufferEnd callback that many times, entering guest code
// directly. So the audio device's consumption rate is a host->guest channel with
// the same standing as the clock, and it was the first thing the ordinal-exact
// replay pointed at: recording and replay ran identically for 87,404 imports and
// then diverged ON TID 10, the sound thread.
constexpr uint32_t kAudioSlot = 1u;

const char* import_name(uint32_t iat_va) {
    for (int i = 0; iat_table[i].iat_va != 0; i++)
        if (iat_table[i].iat_va == iat_va) return iat_table[i].name;
    return "?";
}

void jrnl_close() {
    if (g_fd < 0) return;
    // Every shim still inside its call gets a record saying so, before the end
    // marker. They are the calls the recording could not finish observing.
    for (int c = 0; c < kMaxTid; c++)
        for (const Frame& f : g_frames[c]) {
            Rec r{};
            r.seq = f.seq; r.iat_va = f.iat_va; r.tid = (uint16_t)(f.tid | kInFlight);
            r.nwrites = 0; r.esp_in = f.esp_in;
            jrnl_put(&r, sizeof r);
            g_n_inflight++;
        }
    Rec end{}; end.seq = kEndSeq; end.iat_va = 0; end.tid = 0; end.nwrites = 0;
    jrnl_put(&end, sizeof end);
    jrnl_flush();
    ::close(g_fd);
    g_fd = -1;
}

std::unordered_map<int, struct sigaction> g_prev;

// Flush the journal, then hand the signal to whoever had it before us, so the
// existing [CRASH] dump still runs and still says what it always said.
void jrnl_fatal_handler(int sig, siginfo_t* info, void* uc) {
    if (!g_closing) {
        g_closing = 1;
        jrnl_close();
        // ⚠️ THE REPORTS HAVE TO PRINT ON THIS PATH TOO. A run that ends by
        // [RUNAWAY] calls abort(), i.e. SIGABRT — not SIGTERM — so hanging the
        // end-of-run reports off the terminate handler alone means the runs that
        // FAIL are exactly the ones that report nothing. That is the same shape
        // as every report in this repo that hung off a guard the symptom could
        // suppress, and it cost a whole replay here.
        win32_err_catalogue_report();
        bo2_event_report("fatal");
        bo2_jobq_report("fatal");
        bo2_rdraw_report("fatal");
        bo2_sndworker_report("fatal");
        bo2_sndsplit_report("fatal");
    bo2_sndpool_report("fatal", 1);
        bo2_sndcall_report("fatal");
        bo2_yield_report("fatal");
        bo2_soundhandle_report("fatal");
        bo2_gscsweep_report("fatal");
        bo2_mtindex_report("fatal");
        bo2_gscconcat_report("fatal");
        bo2_internwho_report("fatal");
        bo2_slget_report("fatal");
        bo2_dispatch_report("fatal");
        bo2_freevars_report("fatal");
        bo2_errrank_report("fatal");
        bo2_removeent_report("fatal");
        bo2_freezero_report("fatal");
        win32_sl_band_report(); win32_w32audit_report(); win32_tickgate_report(); win32_mtpop_report(); win32_allocindex_report();
        bo2_draintag_report("fatal");
        bo2_selfnull_report("fatal");
        bo2_unlinkwild_report("fatal");
        bo2_newvarblock_report("fatal");
        bo2_threadinstall_report("fatal");
        bo2_whoinstalls_report("fatal");
        bo2_scopenew_report("fatal");
        bo2_allocthread_report("fatal");
        bo2_lifehist_report("fatal");
        bo2_loopobj_report("fatal");
        bo2_varfree_report("fatal");
        bo2_order_report("fatal");
        bo2_varpool_report("fatal");
        bo2_probe_report("fatal");
        fflush(stderr);
    }
    auto it = g_prev.find(sig);
    if (it != g_prev.end()) {
        const struct sigaction& p = it->second;
        sigaction(sig, &p, nullptr);
        if (p.sa_flags & SA_SIGINFO) { if (p.sa_sigaction) { p.sa_sigaction(sig, info, uc); return; } }
        else if (p.sa_handler && p.sa_handler != SIG_DFL && p.sa_handler != SIG_IGN) {
            p.sa_handler(sig); return;
        }
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// EVERY harness here ends a run with `kill`, so SIGTERM is the NORMAL way a run
// finishes — not an anomaly. Anything that only prints from atexit or from a
// failure guard is therefore lost on almost every run, which is how an angola
// run that reached [SETORIGIN] and presented 2,000 frames reported an empty
// error catalogue. This handler is installed whether or not the journal is
// armed, and it prints the end-of-run reports before letting the process go.
void jrnl_term_handler(int sig) {
    if (g_closing) _exit(128 + sig);
    g_closing = 1;
    jrnl_close();
    win32_err_catalogue_report();
    bo2_event_report("terminated");
    bo2_jobq_report("terminated");
    bo2_rdraw_report("terminated");
    bo2_sndworker_report("terminated");
    bo2_sndsplit_report("terminated");
    bo2_sndpool_report("terminated", 1);
    bo2_sndcall_report("terminated");
    bo2_yield_report("terminated");
    bo2_soundhandle_report("terminated");
    bo2_gscsweep_report("terminated");
    bo2_mtindex_report("terminated");
    bo2_gscconcat_report("terminated");
    bo2_internwho_report("terminated");
    bo2_slget_report("terminated");
    bo2_dispatch_report("terminated");
    bo2_freevars_report("terminated");
    bo2_errrank_report("terminated");
    bo2_removeent_report("terminated");
    bo2_freezero_report("terminated");
    win32_sl_band_report(); win32_w32audit_report(); win32_tickgate_report(); win32_mtpop_report(); win32_allocindex_report();
    bo2_draintag_report("terminated");
    bo2_selfnull_report("terminated");
    bo2_unlinkwild_report("terminated");
    bo2_newvarblock_report("terminated");
    bo2_threadinstall_report("terminated");
    bo2_whoinstalls_report("terminated");
    bo2_scopenew_report("terminated");
    bo2_allocthread_report("terminated");
    bo2_lifehist_report("terminated");
    bo2_loopobj_report("terminated");
    bo2_varfree_report("terminated");
    bo2_order_report("terminated");
    bo2_varpool_report("terminated");
    bo2_probe_report("terminated");
    fflush(stderr);
    _exit(128 + sig);
}

void die_journal(const char* why) {
    if (g_dead) return;
    g_dead = true;
    fprintf(stderr, "[JRNL] DISABLED: %s — the run continues WITHOUT replay, so "
                    "nothing after this line is a replayed result\n", why);
}

}  // namespace

extern "C" bool g_bo2_jrnl_capturing = false;

extern "C" void bo2_journal_init() {
    if (g_inited) return;
    g_inited = true;
    const char* rec = bo2_getenv("BO2_REC");
    const char* rep = bo2_getenv("BO2_REPLAY");
    // ⚠️ Value-checked, not presence-checked: GUIDE §9 — "a flag whose off value
    // turns it ON will be set to 0 by somebody eventually", and that has already
    // cost this project a control run.
    g_check = bo2_getenv("BO2_REPLAY_CHECK") && atoi(bo2_getenv("BO2_REPLAY_CHECK")) != 0;
    g_all   = bo2_getenv("BO2_JRNL_ALL")     && atoi(bo2_getenv("BO2_JRNL_ALL"))     != 0;
    if (rec && rep) {
        fprintf(stderr, "[JRNL] BO2_REC and BO2_REPLAY are both set — refusing "
                        "to guess; set exactly one\n");
        exit(2);
    }
    // Armed unconditionally: see jrnl_term_handler. A run with no journal still
    // needs its end-of-run reports when the harness kills it.
    { struct sigaction sa{}; sa.sa_handler = jrnl_term_handler;
      sigemptyset(&sa.sa_mask);
      sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr); }
    // ⚠️ AND THE FATAL SIGNALS, ON EVERY MODE — NOT JUST WHILE RECORDING.
    // These were installed inside the `if (rec)` branch, so a REPLAY had no
    // fatal handler at all: a run ending by [RUNAWAY] -> abort() went straight
    // to main.cpp's crash_handler and _exit(1), and every end-of-run report was
    // lost. The runs that FAIL are precisely the ones whose reports matter, and
    // two replays produced no [EVCT] output at all before this was found.
        for (int sig : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) {
            struct sigaction old{};
            sigaction(sig, nullptr, &old);
            g_prev[sig] = old;
            struct sigaction sa{};
            sa.sa_sigaction = jrnl_fatal_handler;
            sa.sa_flags = SA_SIGINFO | (old.sa_flags & SA_ONSTACK);
            sigemptyset(&sa.sa_mask);
            sigaction(sig, &sa, nullptr);
        }
    if (!rec && !rep) return;

    // build the selector
    std::unordered_set<std::string> want;
    for (const char* n : kSelected) want.insert(n);
    std::unordered_set<std::string> nocap, serve;
    for (const char* n : kNoCapture) nocap.insert(n);
    for (const char* n : kServe)     serve.insert(n);
    for (int i = 0; iat_table[i].iat_va != 0; i++) {
        if (g_all || want.count(iat_table[i].name)) g_sel.insert(iat_table[i].iat_va);
        if (nocap.count(iat_table[i].name)) g_nocap.insert(iat_table[i].iat_va);
        if (serve.count(iat_table[i].name)) g_serve.insert(iat_table[i].iat_va);
    }

    if (rec) {
        g_fd = ::open(rec, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g_fd < 0) { fprintf(stderr, "[JRNL] cannot write %s\n", rec); exit(2); }
        g_buf.reserve(kFlushAt + 4096);
        Header h{}; memcpy(h.magic, kMagic, 8); h.version = kVersion; h.flags = g_all ? 1u : 0u;
        jrnl_put(&h, sizeof h);
        atexit(jrnl_close);
        atexit([]{ bo2_journal_report("exit"); });
        // ⚠️ SIGABRT MATTERS AS MUCH AS SIGTERM HERE. The first recording ended
        // with signal 6 (the engine's own abort) and lost its end marker, so the
        // journal read as "cut off" — which is the same shape as a journal that
        // was simply short, and those are opposite findings. Chain the existing
        // crash handlers rather than replacing them: they print the [CRASH] dump
        // this project debugs from, and losing that to save a journal would be a
        // bad trade.
        { struct sigaction sa{}; sa.sa_handler = jrnl_term_handler;
          sigemptyset(&sa.sa_mask);
          sigaction(SIGTERM, &sa, nullptr); sigaction(SIGINT, &sa, nullptr); }
        g_recording = true;
        fprintf(stderr, "[JRNL] RECORDING to %s — %zu of %d IAT slots selected%s\n",
                rec, g_sel.size(), (int)(sizeof(iat_table)/sizeof(iat_table[0])) - 1,
                g_all ? " (BO2_JRNL_ALL: every import)" : "");
        return;
    }

    FILE* fp = fopen(rep, "rb");
    if (!fp) { fprintf(stderr, "[JRNL] cannot read %s\n", rep); exit(2); }
    Header h{};
    if (fread(&h, sizeof h, 1, fp) != 1 || memcmp(h.magic, kMagic, 8) != 0 ||
        h.version != kVersion) {
        fprintf(stderr, "[JRNL] %s is not a v%u journal\n", rep, kVersion);
        exit(2);
    }
    g_all = (h.flags & 1u) != 0;
    if (g_all) {
        g_sel.clear();
        for (int i = 0; iat_table[i].iat_va != 0; i++) g_sel.insert(iat_table[i].iat_va);
    }
    bool complete = false;
    for (;;) {
        Rec r{};
        if (fread(&r, sizeof r, 1, fp) != 1) break;
        if (r.seq == kEndSeq && r.iat_va == 0 && r.nwrites == 0) { complete = true; break; }
        if (r.iat_va == kInputSlot) { g_inputs.push_back(r); continue; }
        if (r.iat_va == kAudioSlot) { g_audio.push_back(r);  continue; }
        g_wroff.push_back((uint32_t)g_wrs.size());
        for (uint16_t k = 0; k < r.nwrites; k++) {
            Wr w{};
            if (fread(&w, sizeof w, 1, fp) != 1) {
                fprintf(stderr, "[JRNL] TRUNCATED at record %zu — the recording "
                                "run did not finish writing\n", g_recs.size());
                goto done;
            }
            g_wrs.push_back(w);
        }
        if (r.seq >= g_by_seq.size()) g_by_seq.resize((size_t)r.seq + 1u, kNoRec);
        g_by_seq[r.seq] = (uint32_t)g_recs.size();
        g_recs.push_back(r);
    }
    g_used.assign(g_recs.size(), false);
done:
    fclose(fp);
    if (!complete)
        fprintf(stderr, "[JRNL] ⚠️ %s has NO end marker — the recording was cut "
                        "off (killed, crashed, or still running). Replay will "
                        "report a divergence when it runs past the end, and that "
                        "divergence is the RECORDING's fault, not the guest's.\n", rep);
    g_replaying = true;
    atexit([]{ bo2_journal_report("exit"); });
    fprintf(stderr, "[JRNL] REPLAYING %s — %zu import records, %zu input records, "
                    "%zu audio credits, %zu writes, first seq=%u last seq=%u%s — %zu slots REPLACED, "
                    "the rest re-run and checked\n",
            rep, g_recs.size(), g_inputs.size(), g_audio.size(), g_wrs.size(),
            g_recs.empty() ? 0u : g_recs.front().seq,
            g_recs.empty() ? 0u : g_recs.back().seq,
            g_check ? ", CHECK mode (the real shim also runs)" : "", g_serve.size());
}

extern "C" bool bo2_journal_active()    { return g_recording || g_replaying; }
extern "C" bool bo2_journal_replaying() { return g_replaying && !g_dead; }

extern "C" bool bo2_journal_selects(uint32_t iat_va) {
    g_n_seen++;
    if (g_sel.count(iat_va)) return true;
    g_n_unselected++;
    return false;
}

extern "C" void bo2_journal_note_write_slow(uint32_t addr, uint32_t val, uint32_t width) {
    // The write belongs to whatever shim THIS thread is inside, or to nothing.
    // A yield resumes another coroutine whose ordinary guest stores also arrive
    // here; those are its own execution, not this shim's result.
    const int c = cur_slot();
    if (g_frames[c].empty() || !g_frames[c].back().capture) return;
    g_wr[c].push_back(Wr{addr, val, width});
}

extern "C" void bo2_journal_begin(State& s, uint32_t iat_va, uint32_t seq) {
    const int c = cur_slot();
    const bool cap = !g_nocap.count(iat_va);
    g_frames[c].push_back(Frame{seq, iat_va, s.gpr.rsp.dword, g_wr[c].size(),
                                (uint16_t)win32_current_tid(), cap});
    // The hot-path flag is a cheap "somebody is inside a recorded shim"; the
    // precise per-thread test lives in note_write_slow, where it costs nothing
    // extra because that path is already off the fast route.
    g_bo2_jrnl_capturing = true;
    bo2_w32_armed_recompute();   // the write barrier keeps this flag in its armed word
}

extern "C" void bo2_journal_end(State& s, uint32_t iat_va) {
    const int c = cur_slot();
    if (g_frames[c].empty()) return;                // cannot happen; not a crash
    Frame f = g_frames[c].back(); g_frames[c].pop_back();
    { bool any = false;
      for (int i = 0; i < kMaxTid && !any; i++) any = !g_frames[i].empty();
      g_bo2_jrnl_capturing = any; bo2_w32_armed_recompute(); }
    if (g_fd < 0) { g_wr[c].resize(f.base); return; }

    size_t n = g_wr[c].size() - f.base;
    // No cap, on purpose: probe-design-rules — a capped probe lies in the shape
    // of the thing you are looking for. If a shim ever writes more than 65535
    // dwords through the barrier, say so instead of silently truncating.
    if (n > 0xffff) {
        fprintf(stderr, "[JRNL] %s made %zu barrier writes — over the record "
                        "limit; the journal is NOT replayable\n",
                import_name(iat_va), n);
        die_journal("a shim exceeded 65535 writes in one call");
        g_wr[c].resize(f.base);
        return;
    }
    Rec r{};
    // Every field comes from the FRAME, never from the caller's argument or the
    // live registers — mixing the two is what scrambled the first journals.
    r.seq = f.seq; r.iat_va = f.iat_va; r.tid = f.tid;
    r.nwrites = (uint16_t)n;
    r.eax = s.gpr.rax.dword; r.edx = s.gpr.rdx.dword;
    r.esp_in = f.esp_in; r.esp_out = s.gpr.rsp.dword;
    jrnl_put(&r, sizeof r);
    if (n) jrnl_put(&g_wr[c][f.base], sizeof(Wr) * n);
    g_wr[c].resize(f.base);
    g_n_written++;
}

extern "C" bool bo2_journal_serve(State& s, uint32_t iat_va, uint32_t seq) {
    if (g_dead) return false;
    const int tid = win32_current_tid();

    const uint32_t slot_idx = (seq < g_by_seq.size()) ? g_by_seq[seq] : kNoRec;
    if (slot_idx == kNoRec) {
        // The recording never made an import at this ordinal. Either the replay
        // reached a call the recording did not, or it ran past the recording's end.
        // ⚠️ PAST THE END IS NOT A DIVERGENCE, AND MUST NOT BE COUNTED AS ONE.
        // A replay does not spend the same real time as its recording, so a
        // harness that kills on a wall clock lets the replay run much further —
        // 5.9M imports against a 599k recording on angola. Counting each of
        // those as a divergence produced "divergences 552373" for a replay whose
        // [IMP-HASH] matched the recording on every line it had. Same trap as a
        // capped probe, inverted: a number that looks catastrophic and means
        // "the journal ended".
        const bool past = g_recs.empty() || seq > g_recs.back().seq;
        if (past) {
            if (!g_said_past) {
                g_said_past = true;
                fprintf(stderr, "[JRNL-END] seq=%u: the replay has passed the last "
                                "recorded ordinal (%u). Everything from here runs on "
                                "LIVE values and is not a replay of anything — count "
                                "below, not divergences.\n",
                        seq, g_recs.empty() ? 0u : g_recs.back().seq);
            }
            g_n_past_end++;
            return false;
        }
        g_n_diverge++;
        if (g_n_diverge <= 8)
            fprintf(stderr, "[JRNL-DIVERGE] #%llu  seq=%u %s tid=%d — the recording "
                            "has NO record at this ordinal (and it is INSIDE the "
                            "recorded range, so this is a real one)\n",
                    (unsigned long long)g_n_diverge, seq, import_name(iat_va), tid);
        if (g_first_diverge_seq == 0xffffffffu) {
            g_first_diverge_seq = seq;
            snprintf(g_first_diverge, sizeof g_first_diverge,
                     "no record at seq=%u (%s, tid=%d)%s",
                     seq, import_name(iat_va), tid, past ? " — past the end" : "");
        }
        return false;
    }
    g_next = slot_idx;
    const Rec& r = g_recs[g_next];
    if (r.tid & kInFlight) {
        // Not a divergence: the recording was cut off inside this very call, so
        // it never observed a result to record. Run it for real and say so.
        g_n_inflight_hit++;
        if (g_n_inflight_hit <= 4)
            fprintf(stderr, "[JRNL-INFLIGHT] seq=%u %s tid=%d — the RECORDING was cut "
                            "off inside this call and never saw it return; running it "
                            "for real. This is not a divergence.\n",
                    seq, import_name(iat_va), tid);
        g_used[g_next] = true;
        extern void bo2_journal_check_call(State&, uint32_t);
        bo2_journal_check_call(s, iat_va);
        return true;
    }
    if (r.iat_va != iat_va || r.tid != (uint16_t)tid) {
        g_n_diverge++;
        if (g_n_diverge <= 8)
            fprintf(stderr, "[JRNL-DIVERGE] #%llu  at seq=%u the recording did "
                            "%s on tid=%u, the replay did %s on tid=%d\n",
                    (unsigned long long)g_n_diverge, seq,
                    import_name(r.iat_va), r.tid, import_name(iat_va), tid);
        if (g_first_diverge_seq == 0xffffffffu) {
            g_first_diverge_seq = seq;
            snprintf(g_first_diverge, sizeof g_first_diverge,
                     "at seq=%u the recording did %s tid=%u, the replay did %s tid=%d",
                     seq, import_name(r.iat_va), r.tid, import_name(iat_va), tid);
        }
        return false;   // run the real shim; keep going so later divergences show
    }
    g_used[g_next] = true;

    if (!g_serve.count(iat_va)) {
        // CHECK class: run it for real, then say whether it agreed. The value is
        // NOT overwritten — forcing it would make the replay look successful
        // while hiding the one thing this is built to find.
        extern void bo2_journal_check_call(State&, uint32_t);
        const uint32_t esp_in = s.gpr.rsp.dword;
        bo2_journal_check_call(s, iat_va);
        // ⚠️ COMPARE THE STDCALL CLEANUP DELTA, NOT THE ABSOLUTE esp. The first
        // version compared s.gpr.rsp against r.esp_out and reported 24 shims as
        // "not reproducible" — with eax IDENTICAL in every one of them, and the
        // esp alternating between two values 0x14 apart. That is the guest being
        // at a different call depth, which is not what this is asking. What a
        // shim owes its caller is how much stack it pops.
        const int32_t d_live = (int32_t)(s.gpr.rsp.dword - esp_in);
        const int32_t d_rec  = (int32_t)(r.esp_out - r.esp_in);
        // Sleep and the waits leave eax undefined (they return void or a status
        // the caller re-derives), so only their cleanup is comparable.
        const bool eax_matters = !g_nocap.count(iat_va);
        if ((eax_matters && s.gpr.rax.dword != r.eax) || d_live != d_rec) {
            g_n_valuediff++;
            if (g_n_valuediff <= 24)
                fprintf(stderr, "[JRNL-VALUE] seq=%u %s: live eax=0x%08x pops %d  "
                                "journal eax=0x%08x pops %d  — a CHECK-class shim is "
                                "not reproducible; it belongs in kServe\n",
                        seq, import_name(iat_va), s.gpr.rax.dword, d_live,
                        r.eax, d_rec);
        }
        g_n_served++;
        g_n_rerun++;
        return true;
    }

    // SERVE class: the journal replaces the call outright.
    if (g_check) {
        extern void bo2_journal_check_call(State&, uint32_t);
        bo2_journal_check_call(s, iat_va);
        if (s.gpr.rax.dword != r.eax || s.gpr.rsp.dword != r.esp_out) {
            g_n_checkfail++;
            if (g_n_checkfail <= 24)
                fprintf(stderr, "[JRNL-CHECK] seq=%u %s: live eax=0x%08x esp=0x%08x  "
                                "journal eax=0x%08x esp=0x%08x\n",
                        seq, import_name(iat_va), s.gpr.rax.dword, s.gpr.rsp.dword,
                        r.eax, r.esp_out);
        }
    }

    const uint32_t off = g_wroff[g_next];
    for (uint16_t k = 0; k < r.nwrites; k++) {
        const Wr& w = g_wrs[off + k];
        if      (w.width == 1) win32_mem_write8 (w.addr, (uint8_t)w.val);
        else if (w.width == 2) win32_mem_write16(w.addr, (uint16_t)w.val);
        else                   win32_mem_write32(w.addr, w.val);
    }
    s.gpr.rax.dword = r.eax;
    s.gpr.rdx.dword = r.edx;
    s.gpr.rsp.dword = r.esp_out;
    g_n_served++;
    g_n_replaced++;
    return true;
}

// Wraps the host input drain. Returns true when a message should be delivered.
extern "C" bool bo2_journal_input_pop(uint32_t* msg, uint32_t* wp, uint32_t* lp) {
    if (!bo2_journal_active() || g_dead) return gfx_input_pop(msg, wp, lp);

    if (g_replaying) {
        // Empty the LIVE queue and throw it away. Without this a keystroke typed
        // during a replay would be delivered on top of the recorded ones, and the
        // replay would no longer be a replay of anything.
        uint32_t a, b, c;
        while (gfx_input_pop(&a, &b, &c)) g_n_input_dropped++;
        const uint32_t now = (uint32_t)g_import_call_count_probe();
        if (g_next_input < g_inputs.size() && g_inputs[g_next_input].seq <= now) {
            const Rec& r = g_inputs[g_next_input++];
            *msg = r.eax; *wp = r.edx; *lp = r.esp_in;
            g_n_input_served++;
            return true;
        }
        return false;
    }

    if (!gfx_input_pop(msg, wp, lp)) return false;
    if (g_fd >= 0) {
        Rec r{};
        r.seq = (uint32_t)g_import_call_count_probe();
        r.iat_va = kInputSlot;
        r.tid = (uint16_t)win32_current_tid();
        r.nwrites = 0;
        r.eax = *msg; r.edx = *wp; r.esp_in = *lp; r.esp_out = 0;
        jrnl_put(&r, sizeof r);
        g_n_input_written++;
    }
    return true;
}

// Wraps the audio-completion credit. On replay the REAL device is never asked:
// its answer is a host-timed number and asking would also consume completions.
extern "C" uint32_t bo2_journal_audio_take(uint32_t unit, uint32_t max) {
    if (!bo2_journal_active() || g_dead) return bo2_audio_take_completed(unit, max);

    const uint32_t now = (uint32_t)g_import_call_count_probe();
    if (g_replaying) {
        if (g_next_audio < g_audio.size() && g_audio[g_next_audio].seq <= now) {
            g_n_audio_served++;
            return g_audio[g_next_audio++].eax;
        }
        return 0;
    }

    const uint32_t done = bo2_audio_take_completed(unit, max);
    // Record even a ZERO. The pump is called every frame and mostly gets nothing;
    // if only the non-zero credits were journalled, a replay could not tell "no
    // completion here" from "this record belongs to a later pump", and the audio
    // thread would drift exactly the way it already did.
    if (g_fd >= 0) {
        Rec r{};
        r.seq = now; r.iat_va = kAudioSlot; r.tid = (uint16_t)win32_current_tid();
        r.nwrites = 0; r.eax = done;
        jrnl_put(&r, sizeof r);
        g_n_audio_written++;
    }
    return done;
}

extern "C" void bo2_journal_report(const char* why) {
    if (!bo2_journal_active()) {
        // An unarmed journal and a silent one must not read the same. This line
        // is the whole reason the report prints unconditionally.
        fprintf(stderr, "[JRNL] not armed (set BO2_REC=<path> or BO2_REPLAY=<path>) "
                        "— this run says NOTHING about determinism\n");
        return;
    }
    fprintf(stderr, "=== [JRNL] %s ===\n", why ? why : "report");
    fprintf(stderr, "[JRNL] mode              %s%s\n",
            g_recording ? "RECORD" : "REPLAY", g_dead ? "  (DISABLED mid-run)" : "");
    fprintf(stderr, "[JRNL] imports seen      %llu   (of which unselected %llu)\n",
            (unsigned long long)g_n_seen, (unsigned long long)g_n_unselected);
    if (g_recording) {
        fprintf(stderr, "[JRNL] records written   %llu   (of which in-flight at close %llu)\n",
                (unsigned long long)g_n_written, (unsigned long long)g_n_inflight);
        fprintf(stderr, "[JRNL] input messages    %llu   (window messages are a SECOND "
                        "host->guest boundary; 0 means nothing was typed)\n",
                (unsigned long long)g_n_input_written);
        fprintf(stderr, "[JRNL] audio credits     %llu   (CoreAudio buffer completions "
                        "— the THIRD boundary; zeros are recorded too, on purpose)\n",
                (unsigned long long)g_n_audio_written);
        jrnl_flush();
    } else {
        fprintf(stderr, "[JRNL] records in file   %zu\n", g_recs.size());
        fprintf(stderr, "[JRNL] records served    %llu   (REPLACED %llu, re-run and "
                        "CHECKED %llu)\n",
                (unsigned long long)g_n_served, (unsigned long long)g_n_replaced,
                (unsigned long long)g_n_rerun);
        fprintf(stderr, "[JRNL] value mismatches  %llu   (CHECK-class shims whose live "
                        "result differed — each one is a missing kServe entry)\n",
                (unsigned long long)g_n_valuediff);
        {
            size_t unused = 0;
            for (size_t i = 0; i < g_used.size(); i++) if (!g_used[i]) unused++;
            fprintf(stderr, "[JRNL] records UNUSED    %zu   (in the recording and never "
                            "asked for by the replay)\n", unused);
        }
        fprintf(stderr, "[JRNL] input in file     %zu   served %llu   unused %zu\n",
                g_inputs.size(), (unsigned long long)g_n_input_served,
                g_inputs.size() > g_next_input ? g_inputs.size() - g_next_input : 0);
        fprintf(stderr, "[JRNL] audio in file     %zu   served %llu   unused %zu\n",
                g_audio.size(), (unsigned long long)g_n_audio_served,
                g_audio.size() > g_next_audio ? g_audio.size() - g_next_audio : 0);
        fprintf(stderr, "[JRNL] live input DROPPED %llu   (typed during the replay "
                        "and discarded, as it must be)\n",
                (unsigned long long)g_n_input_dropped);
        fprintf(stderr, "[JRNL] in-flight records %llu   (calls the recording was cut "
                        "off inside — NOT divergences)\n",
                (unsigned long long)g_n_inflight_hit);
        fprintf(stderr, "[JRNL] past the end      %llu   (the replay outlived the "
                        "recording — expected, not divergences)\n",
                (unsigned long long)g_n_past_end);
        fprintf(stderr, "[JRNL] divergences       %llu   (INSIDE the recorded range)\n",
                (unsigned long long)g_n_diverge);
        if (g_n_diverge)
            fprintf(stderr, "[JRNL] FIRST divergence  %s\n", g_first_diverge);
        else if (g_n_served)
            fprintf(stderr, "[JRNL] 🎯 NO DIVERGENCE — every one of the %llu selected "
                            "imports matched the journal in slot, ordinal and thread\n",
                    (unsigned long long)g_n_served);
        if (g_check)
            fprintf(stderr, "[JRNL] check mismatches  %llu   (shims whose live result "
                            "differed from the recording)\n",
                    (unsigned long long)g_n_checkfail);
    }
}
