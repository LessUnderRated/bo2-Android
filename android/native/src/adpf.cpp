// adpf.cpp — Android's performance hints (ADPF) for the game's frame threads.
//
// Without hints, the CPU governor only sees load: a thread that is busy gets its core clocked up, whether or not the
// frame needed it. A hint session tells it the frame's deadline (1 / frame cap) and, every frame, how long the frame's
// threads actually worked. With slack it can lower their clocks (less power, less heat); near the deadline it boosts
// them before a frame is missed rather than after.
//
// The session holds the threads on the frame's path: the game's main loop (the runtime's scheduler thread), the D3D
// backend, the server, the job workers (when hosted) and the Vulkan render thread. They register themselves
// (bo2_perf_register_thread, called by bo2-mac's kernel32_impl.cpp at thread start, and by the render thread on its
// first presented frame). The audio threads are not in it: they run on the device's clock, not the frame's.
//
// "Actual work" is CPU time, not wall time: each registered thread's CPU clock (pthread_getcpuclockid) since the last
// presented frame, and the busiest one is reported. Time spent blocked (the frame queue waiting for the display,
// a wait on an event) is not work, so the governor sees the slack there is.
//
// BO2_ADPF=0 turns it off. One log line, [ADPF], says what it did.
#include <android/performance_hint.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "graphics.hpp"

namespace {

struct Reg {
    pid_t tid;
    clockid_t clock;
    int role;
    int64_t last_cpu_ns = -1;
};

const char* role_name(int role) {
    switch (role) {
        case 0: return "main";
        case 1: return "backend";
        case 2: return "server";
        case 3: return "job";
        case 4: return "render";
        default: return "?";
    }
}

bool enabled() {
    static const bool on = [] {
        const char* e = getenv("BO2_ADPF");
        return !(e && strcmp(e, "0") == 0);
    }();
    return on;
}

std::mutex g_mu;                 // g_regs, g_gen
std::vector<Reg> g_regs;
int g_gen = 0;                   // bumped by each registration

// Render thread only (the frame hook).
APerformanceHintManager* g_mgr = nullptr;
APerformanceHintSession* g_session = nullptr;
int g_session_gen = -1;
int64_t g_target_ns = 0;
std::vector<Reg> g_live;         // the render thread's copy, with the per-thread CPU clocks
bool g_failed = false;

int64_t cpu_ns(clockid_t c) {
    timespec ts{};
    if (clock_gettime(c, &ts) != 0) return -1;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t target_ns() {
    const int cap = bo2gfx::settings().frame_cap;
    return 1000000000LL / (cap > 0 ? cap : 60);
}

// APerformanceHint_setThreads is API 34; the app's floor is 33, so it is looked up.
using SetThreadsFn = int (*)(APerformanceHintSession*, const pid_t*, size_t);
SetThreadsFn set_threads_fn() {
    static SetThreadsFn fn = reinterpret_cast<SetThreadsFn>(dlsym(RTLD_DEFAULT, "APerformanceHint_setThreads"));
    return fn;
}

void rebuild_session() {
    int gen;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        gen = g_gen;
        // Keep the CPU clocks already read for threads that stay.
        std::vector<Reg> next = g_regs;
        for (Reg& r : next)
            for (const Reg& o : g_live)
                if (o.tid == r.tid) r.last_cpu_ns = o.last_cpu_ns;
        g_live.swap(next);
    }
    g_session_gen = gen;
    if (g_live.empty()) return;
    std::vector<pid_t> tids;
    for (const Reg& r : g_live) tids.push_back(r.tid);
    g_target_ns = target_ns();
    if (g_session && set_threads_fn()) {
        if (set_threads_fn()(g_session, tids.data(), tids.size()) == 0) return;
    }
    if (g_session) APerformanceHint_closeSession(g_session);
    g_session = APerformanceHint_createSession(g_mgr, tids.data(), tids.size(), g_target_ns);
    char roles[160] = "";
    for (const Reg& r : g_live) {
        const size_t n = strlen(roles);
        snprintf(roles + n, sizeof roles - n, "%s%s", n ? "," : "", role_name(r.role));
    }
    std::fprintf(stderr, "[ADPF] %s for %zu threads (%s), target %.2f ms\n",
                 g_session ? "hint session" : "no hint session (createSession refused)", tids.size(), roles,
                 g_target_ns / 1e6);
}

}  // namespace

extern "C" void bo2_perf_register_thread(int role) {
    if (!enabled()) return;
    clockid_t c;
    if (pthread_getcpuclockid(pthread_self(), &c) != 0) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_regs.size() >= 16) return;
    const pid_t tid = gettid();
    for (const Reg& r : g_regs)
        if (r.tid == tid) return;
    g_regs.push_back(Reg{tid, c, role});
    g_gen++;
}

// Once per presented frame, on the render thread (game_host.cpp, hook_frame_presented).
void bo2_adpf_frame() {
    if (!enabled() || g_failed) return;
    static thread_local bool render_registered = false;
    if (!render_registered) {
        render_registered = true;
        bo2_perf_register_thread(4);
    }
    if (!g_mgr) {
        g_mgr = APerformanceHint_getManager();
        if (!g_mgr) {
            g_failed = true;
            std::fprintf(stderr, "[ADPF] no performance hint manager on this device: off\n");
            return;
        }
    }
    int gen;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        gen = g_gen;
    }
    if (gen != g_session_gen) rebuild_session();
    if (!g_session) return;
    const int64_t want = target_ns();
    if (want != g_target_ns) {
        g_target_ns = want;
        APerformanceHint_updateTargetWorkDuration(g_session, want);
    }
    int64_t busiest = 0;
    for (Reg& r : g_live) {
        const int64_t now = cpu_ns(r.clock);
        if (now < 0) continue;   // the thread is gone
        if (r.last_cpu_ns >= 0 && now > r.last_cpu_ns) busiest = std::max<int64_t>(busiest, now - r.last_cpu_ns);
        r.last_cpu_ns = now;
    }
    if (busiest > 0) APerformanceHint_reportActualWorkDuration(g_session, busiest);
}
