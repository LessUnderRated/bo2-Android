// prof.cpp — see prof.hpp. One thread, bo2_prof, wakes once a second:
//
//   every 5 s  [THREADS]  each thread's share of a core over the window, from /proc/self/task/<tid>/stat (utime +
//                         stime), the core it last ran on (field 39), and the process total. Top 10.
//              [SYSINFO]  what an app may read: GPU busy % and clock (kgsl), the two clusters' current clocks, the
//                         thermal status and headroom (perf.cpp, from PowerManager), the battery's current and voltage.
//                         A source that cannot be read is said once ("n/a") and left out after.
//   every 30 s [PROF]     a sampling profile of the busiest threads: for each, the most frequent program counters as
//                         offsets into libmain.so (with its build id), the rest counted per library and named by
//                         function where the library exports one (below). Symbolised on the PC with the matching
//                         unstripped libmain.so (scripts/phone/symbolize-prof.sh).
//
// Sampling. Each of the 4 busiest threads (re-chosen every 5 s, 5% of a core at least) gets a POSIX timer on its own
// CPU-time clock (CLOCK_THREAD_CPUTIME of that tid, SIGEV_THREAD_ID): the kernel signals it after every 1/HZ second
// of CPU it uses (default 250 Hz). So samples are proportional to CPU time, and a thread that sleeps is never woken
// (a wall-clock sampler would interrupt its waits and count the wait as a sample). The handler stores the
// interrupted pc and the tid in a lock-free ring; this thread drains it every second.
// The signal is SIGRTMIN+4: bionic keeps the real-time signals below SIGRTMIN for itself, and the runtime uses SIGPROF,
// SIGUSR2, SIGHUP, SIGSEGV and SIGBUS, none of these. The handler is async-signal-safe (atomics only) and keeps errno.
// Cost: ~1000 signals a second in all, a few microseconds each, plus a /proc scan every 5 s: well under 1% of a core.
//
// Other libraries (libc, the Vulkan driver, libc++) are named too, on top of the per-library counts (`other=`):
//   fn=<lib>!<name>:<n>              samples in <name>. The name is the exported function the pc lies in (bionic's
//                                    dladdr checks the symbol's size, so a name is never a guess). Hidden code: the
//                                    implementation an ifunc resolved to (memcpy, memset, strlen... as dlsym returns
//                                    it), marked `*`, when the pc is within 4 KB past its entry and no exported
//                                    function covers it; else `?<caller` when the link register is in an exported
//                                    function of the same library (`?<free`: allocator internals free called), else `?`.
//   via=<lib>!<name>@<offset>:<n>   the same samples by caller: the link register when it points into libmain.so,
//                                    as an offset (symbolised like the pcs). For a leaf such as memcpy that is the
//                                    function that called it; inside a non-leaf function the link register may be its
//                                    own last call's, so a `via` share is a lower bound for that caller.
#include "prof.hpp"
#include "log.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "perf.hpp"

namespace bo2prof {
namespace {

// ---- The sample ring, written by the signal handler on the sampled threads ------------------------------------------
constexpr uint32_t kRing = 1u << 16;   // 65 s of samples at 1000/s: drained every second
struct Sample {
    uint64_t pc;
    uint64_t lr;                 // x30: the caller, for a leaf function
    int32_t tid;
    std::atomic<uint64_t> seq;   // index + 1 once pc and tid are written
};
Sample g_ring[kRing];
std::atomic<uint64_t> g_head{0};
int g_sig = 0;

void on_sample(int, siginfo_t* si, void* uc) {
    const int saved = errno;
    const auto& mc = static_cast<ucontext_t*>(uc)->uc_mcontext;
    const uint64_t pc = static_cast<uint64_t>(mc.pc);
    const uint64_t lr = static_cast<uint64_t>(mc.regs[30]);
    const uint64_t i = g_head.fetch_add(1, std::memory_order_relaxed);
    Sample& s = g_ring[i & (kRing - 1)];
    s.pc = pc;
    s.lr = lr;
    s.tid = si->si_value.sival_int;
    s.seq.store(i + 1, std::memory_order_release);
    errno = saved;
}

// ---- Per-thread CPU-time timers -------------------------------------------------------------------------------------
// A thread's CPU clock, as the kernel encodes it: MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED).
clockid_t thread_cpu_clock(int tid) { return static_cast<clockid_t>((~static_cast<uint32_t>(tid) << 3) | 6u); }

struct Timer {
    int tid;
    int id;
};
std::vector<Timer> g_timers;

bool timer_start(int tid, long period_ns) {
    struct sigevent sev;
    std::memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = g_sig;
    sev.sigev_value.sival_int = tid;
    sev.sigev_notify_thread_id = tid;
    int id = -1;
    if (syscall(__NR_timer_create, thread_cpu_clock(tid), &sev, &id) != 0) return false;
    struct itimerspec its;
    std::memset(&its, 0, sizeof its);
    its.it_interval.tv_sec = period_ns / 1000000000L;
    its.it_interval.tv_nsec = period_ns % 1000000000L;
    its.it_value = its.it_interval;
    if (syscall(__NR_timer_settime, id, 0, &its, nullptr) != 0) {
        syscall(__NR_timer_delete, id);
        return false;
    }
    g_timers.push_back({tid, id});
    return true;
}

void timers_retarget(const std::vector<int>& want, long period_ns) {
    for (size_t i = 0; i < g_timers.size();) {
        if (std::find(want.begin(), want.end(), g_timers[i].tid) == want.end()) {
            syscall(__NR_timer_delete, g_timers[i].id);   // fails harmlessly if the thread is gone
            g_timers.erase(g_timers.begin() + static_cast<long>(i));
        } else {
            i++;
        }
    }
    for (int tid : want) {
        const bool have = std::any_of(g_timers.begin(), g_timers.end(), [tid](const Timer& t) { return t.tid == tid; });
        if (!have && !timer_start(tid, period_ns)) {
            static int said = 0;
            if (said++ < 3) std::fprintf(stderr, "[PROF] no CPU-time timer for tid %d (errno %d)\n", tid, errno);
        }
    }
}

// ---- Threads: /proc/self/task ---------------------------------------------------------------------------------------
struct TaskStat {
    std::string comm;
    uint64_t ticks = 0;   // utime + stime
    int cpu = -1;
};

bool read_task(int tid, TaskStat* out) {
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/stat", tid);
    FILE* f = std::fopen(path, "re");
    if (!f) return false;
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = 0;
    char* open = std::strchr(buf, '(');
    char* close = std::strrchr(buf, ')');
    if (!open || !close || close < open) return false;
    out->comm.assign(open + 1, static_cast<size_t>(close - open - 1));
    // Fields after the comm, numbered as in proc(5): 3 state, 14 utime, 15 stime, 39 processor.
    int field = 3;
    uint64_t utime = 0, stime = 0;
    for (char* p = close + 2; *p && field <= 39; field++) {
        char* end = nullptr;
        const unsigned long long v = std::strtoull(p, &end, 10);
        if (field == 14) utime = v;
        else if (field == 15) stime = v;
        else if (field == 39) out->cpu = static_cast<int>(v);
        p = std::strchr(p, ' ');
        if (!p) break;
        p++;
    }
    out->ticks = utime + stime;
    return true;
}

std::vector<int> list_tids() {
    std::vector<int> tids;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] >= '0' && e->d_name[0] <= '9') tids.push_back(std::atoi(e->d_name));
        closedir(d);
    }
    return tids;
}

std::unordered_map<int, TaskStat> g_prev;   // last window's ticks
std::unordered_map<int, std::string> g_names;
int g_self_tid = 0;

// The window's [THREADS] line; returns the busiest threads (for sampling), each at least `min_pct` of a core.
std::vector<int> threads_window(double seconds, int want, double min_pct, bool print = true) {
    static const long hz = sysconf(_SC_CLK_TCK) > 0 ? sysconf(_SC_CLK_TCK) : 100;
    struct Row {
        int tid;
        double pct;
        int cpu;
    };
    std::vector<Row> rows;
    std::unordered_map<int, TaskStat> now;
    double total = 0;
    for (int tid : list_tids()) {
        TaskStat t;
        if (!read_task(tid, &t)) continue;
        g_names[tid] = t.comm;
        const auto p = g_prev.find(tid);
        if (p != g_prev.end() && t.ticks >= p->second.ticks) {
            const double pct = 100.0 * static_cast<double>(t.ticks - p->second.ticks) / (static_cast<double>(hz) * seconds);
            total += pct;
            rows.push_back({tid, pct, t.cpu});
        }
        now[tid] = std::move(t);
    }
    g_prev.swap(now);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    std::string line;
    char tok[96];
    for (size_t i = 0; i < rows.size() && i < 10; i++) {
        if (rows[i].pct < 0.5) break;
        std::snprintf(tok, sizeof tok, " %s=%.0f%%@%d", g_names[rows[i].tid].c_str(), rows[i].pct, rows[i].cpu);
        line += tok;
    }
    if (print) std::fprintf(stderr, "[THREADS] %.1fs total=%.0f%% n=%zu%s\n", seconds, total, g_prev.size(), line.c_str());
    std::vector<int> top;
    for (const Row& r : rows) {
        if (static_cast<int>(top.size()) >= want || r.pct < min_pct) break;
        if (r.tid != g_self_tid) top.push_back(r.tid);
    }
    return top;
}

// ---- [SYSINFO] ------------------------------------------------------------------------------------------------------
bool read_long(const char* path, long long* v) {
    FILE* f = std::fopen(path, "re");
    if (!f) return false;
    const bool ok = std::fscanf(f, "%lld", v) == 1;
    std::fclose(f);
    return ok;
}

void sysinfo_line() {
    static bool said_gpu = false, said_cpu = false, said_batt = false;
    std::string line;
    char tok[128];
    long long busy = 0, mhz = 0;
    if (read_long("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", &busy) &&
        read_long("/sys/class/kgsl/kgsl-3d0/clock_mhz", &mhz)) {
        std::snprintf(tok, sizeof tok, " gpu=%lld%%@%lldMHz", busy, mhz);
        line += tok;
    } else if (!said_gpu) {
        said_gpu = true;
        line += " gpu=n/a";
    }
    long long c0 = 0, c6 = 0;
    const bool f0 = read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", &c0);
    const bool f6 = read_long("/sys/devices/system/cpu/cpu6/cpufreq/scaling_cur_freq", &c6);
    if (f0 || f6) {
        std::snprintf(tok, sizeof tok, " cpu0=%lldMHz cpu6=%lldMHz", f0 ? c0 / 1000 : -1, f6 ? c6 / 1000 : -1);
        line += tok;
    } else if (!said_cpu) {
        said_cpu = true;
        line += " cpufreq=n/a";
    }
    const bo2perf::Summary s = bo2perf::last();
    std::snprintf(tok, sizeof tok, " thermal=%d/%.2f", s.thermal_status, s.thermal_headroom);
    line += tok;
    long long ua = 0, uv = 0;
    if (read_long("/sys/class/power_supply/battery/current_now", &ua) &&
        read_long("/sys/class/power_supply/battery/voltage_now", &uv)) {
        const double a = static_cast<double>(ua) / 1e6, v = static_cast<double>(uv) / 1e6;
        std::snprintf(tok, sizeof tok, " batt=%.2fA@%.2fV=%.1fW", a, v, (a < 0 ? -a : a) * v);
        line += tok;
    } else if (!said_batt) {
        said_batt = true;
        line += " batt=n/a";
    }
    std::fprintf(stderr, "[SYSINFO]%s\n", line.c_str());
}

// ---- Libraries: where a pc falls --------------------------------------------------------------------------------------
struct Dso {
    uint64_t lo, hi;
    std::string name;   // short: libmain, libc, libvulkan...
    bool main;
};
std::vector<Dso> g_dsos;
uint64_t g_main_base = 0;
std::string g_build_id;

std::string short_name(const char* path) {
    const char* b = std::strrchr(path, '/');
    std::string s = b ? b + 1 : path;
    const size_t so = s.find(".so");
    if (so != std::string::npos) s.resize(so);
    return s.empty() ? std::string("?") : s;
}

int collect_dso(dl_phdr_info* info, size_t, void*) {
    const char* path = info->dlpi_name ? info->dlpi_name : "";
    const size_t len = std::strlen(path);
    const bool is_main = len >= 10 && std::strcmp(path + len - 10, "libmain.so") == 0;
    std::string name = short_name(*path ? path : "app");
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
            const uint64_t lo = info->dlpi_addr + ph.p_vaddr;
            g_dsos.push_back({lo, lo + ph.p_memsz, name, is_main});
        }
        if (is_main && ph.p_type == PT_NOTE && g_build_id.empty()) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
            const uint8_t* end = p + ph.p_memsz;
            while (p + sizeof(ElfW(Nhdr)) <= end) {
                const auto* nh = reinterpret_cast<const ElfW(Nhdr)*>(p);
                const uint8_t* nm = p + sizeof(ElfW(Nhdr));
                const uint8_t* desc = nm + ((nh->n_namesz + 3u) & ~3u);
                if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && std::memcmp(nm, "GNU", 4) == 0) {
                    char hex[3];
                    for (uint32_t k = 0; k < nh->n_descsz; k++) {
                        std::snprintf(hex, sizeof hex, "%02x", desc[k]);
                        g_build_id += hex;
                    }
                    break;
                }
                p = desc + ((nh->n_descsz + 3u) & ~3u);
            }
        }
    }
    if (is_main) g_main_base = info->dlpi_addr;
    return 0;
}

void refresh_dsos() {
    g_dsos.clear();
    dl_iterate_phdr(collect_dso, nullptr);
    std::sort(g_dsos.begin(), g_dsos.end(), [](const Dso& a, const Dso& b) { return a.lo < b.lo; });
}

const Dso* dso_of(uint64_t pc) {
    auto it = std::upper_bound(g_dsos.begin(), g_dsos.end(), pc, [](uint64_t v, const Dso& d) { return v < d.lo; });
    if (it == g_dsos.begin()) return nullptr;
    --it;
    return pc < it->hi ? &*it : nullptr;
}

// ---- Names in other libraries ----------------------------------------------------------------------------------------
// The string and memory routines are ifuncs: their exported symbol is the resolver, and the code that runs is a hidden
// variant (__memmove_aarch64_nt...). dlsym resolves the ifunc, so it gives that variant's entry.
struct Anchor {
    uint64_t addr;
    const char* name;
};
std::vector<Anchor> g_anchors;

void load_anchors() {
    static const char* const kNames[] = {
        "memcpy", "memmove", "memset", "memcmp", "bcmp", "memchr", "memrchr", "strlen", "strnlen", "strcmp", "strncmp",
        "strchr", "strchrnul", "strrchr", "strcpy", "stpcpy", "strncpy", "__memcpy_chk", "__memmove_chk", "__memset_chk",
    };
    void* libc = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD);
    for (const char* n : kNames)
        if (void* p = dlsym(libc ? libc : RTLD_DEFAULT, n)) g_anchors.push_back({reinterpret_cast<uint64_t>(p), n});
    std::sort(g_anchors.begin(), g_anchors.end(), [](const Anchor& a, const Anchor& b) { return a.addr < b.addr; });
}

// Exported names hold no spaces; the separators of the [PROF] format go anyway.
std::string clean(const char* s) {
    std::string out(s);
    for (char& c : out)
        if (c == ' ' || c == '=' || c == ':' || c == ',' || c == '!' || c == '@') c = '_';
    return out;
}

// Exported function holding pc, or "" (bionic's dladdr only names a symbol whose [value, value + size) holds it).
std::string exported_at(uint64_t pc) {
    static std::unordered_map<uint64_t, std::string> memo;
    if (memo.size() > 200000) memo.clear();
    const auto it = memo.find(pc);
    if (it != memo.end()) return it->second;
    Dl_info di;
    std::string name;
    if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_sname && di.dli_saddr) name = clean(di.dli_sname);
    memo.emplace(pc, name);
    return name;
}

std::string label_of(uint64_t pc, uint64_t lr) {
    std::string name = exported_at(pc);
    if (!name.empty()) return name;
    auto a = std::upper_bound(g_anchors.begin(), g_anchors.end(), pc, [](uint64_t v, const Anchor& x) { return v < x.addr; });
    if (a != g_anchors.begin()) {
        --a;
        if (pc - a->addr < 4096 && dso_of(a->addr) == dso_of(pc)) return std::string(a->name) + "*";
    }
    if (lr && dso_of(lr) == dso_of(pc)) {
        const std::string caller = exported_at(lr);
        if (!caller.empty()) return "?<" + caller;
    }
    return "?";
}

// ---- Aggregation ------------------------------------------------------------------------------------------------------
std::unordered_map<int, std::unordered_map<uint64_t, uint32_t>> g_hits;   // tid -> pc -> samples
struct PcLr {
    uint64_t pc, lr;
    bool operator==(const PcLr& o) const { return pc == o.pc && lr == o.lr; }
};
struct PcLrHash {
    size_t operator()(const PcLr& k) const { return static_cast<size_t>(k.pc * 0x9E3779B97F4A7C15ull ^ k.lr); }
};
std::unordered_map<int, std::unordered_map<PcLr, uint32_t, PcLrHash>> g_calls;   // tid -> (pc, lr) outside libmain
uint64_t g_tail = 0, g_lost = 0;

void drain() {
    const uint64_t head = g_head.load(std::memory_order_acquire);
    if (head - g_tail > kRing) {   // the ring lapped: what it overwrote is lost
        g_lost += head - g_tail - kRing;
        g_tail = head - kRing;
    }
    for (; g_tail < head; g_tail++) {
        Sample& s = g_ring[g_tail & (kRing - 1)];
        if (s.seq.load(std::memory_order_acquire) != g_tail + 1) continue;   // not written yet, or overwritten
        const uint64_t pc = s.pc;
        const uint64_t lr = s.lr;
        const int tid = s.tid;
        if (s.seq.load(std::memory_order_acquire) != g_tail + 1) continue;
        g_hits[tid][pc]++;
        const Dso* d = dso_of(pc);
        if (!d || !d->main) g_calls[tid][PcLr{pc, lr}]++;
    }
}

// The fn= and via= tokens of one thread (see the top of this file), most samples first.
void named_tokens(int tid, std::vector<std::string>& out) {
    constexpr size_t kTopFns = 20, kTopVia = 30;
    const auto it = g_calls.find(tid);
    if (it == g_calls.end()) return;
    std::unordered_map<std::string, uint32_t> fns, via;
    for (const auto& [k, n] : it->second) {
        const Dso* d = dso_of(k.pc);
        const std::string lib = d ? d->name : std::string("?");
        const std::string fn = lib + "!" + label_of(k.pc, k.lr);
        fns[fn] += n;
        const Dso* c = dso_of(k.lr);
        if (c && c->main) {
            char off[24];
            std::snprintf(off, sizeof off, "@%llx", static_cast<unsigned long long>(k.lr - g_main_base));
            via[fn + off] += n;
        }
    }
    auto top = [&out](std::unordered_map<std::string, uint32_t>& m, const char* key, size_t keep) {
        std::vector<std::pair<std::string, uint32_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > keep) v.resize(keep);
        for (const auto& [name, n] : v) out.push_back(std::string(" ") + key + "=" + name + ":" + std::to_string(n));
    };
    top(fns, "fn", kTopFns);
    top(via, "via", kTopVia);
}

void emit_profile(int seconds) {
    refresh_dsos();
    constexpr size_t kTopPcs = 120, kLine = 900;
    for (auto& [tid, pcs] : g_hits) {
        uint32_t total = 0, in_main = 0;
        std::map<std::string, uint32_t> other;
        std::vector<std::pair<uint64_t, uint32_t>> offs;
        for (const auto& [pc, n] : pcs) {
            total += n;
            const Dso* d = dso_of(pc);
            if (d && d->main) {
                in_main += n;
                offs.emplace_back(pc - g_main_base, n);
            } else {
                other[d ? d->name : std::string("?")] += n;
            }
        }
        if (!total) continue;
        std::sort(offs.begin(), offs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (offs.size() > kTopPcs) offs.resize(kTopPcs);
        std::string others;
        for (const auto& [name, n] : other) others += (others.empty() ? "" : ",") + name + ":" + std::to_string(n);
        const auto nm = g_names.find(tid);
        const std::string tname = nm != g_names.end() ? nm->second : "tid" + std::to_string(tid);
        char head[256];
        std::snprintf(head, sizeof head, "[PROF] build=%s thread=%s tid=%d window=%ds samples=%u libmain=%u",
                      g_build_id.empty() ? "?" : g_build_id.c_str(), tname.c_str(), tid, seconds, total, in_main);
        std::string line = std::string(head) + " other=" + (others.empty() ? "-" : others);
        int part = 1;
        std::vector<std::string> toks;
        char tok[40];
        for (const auto& [off, n] : offs) {
            std::snprintf(tok, sizeof tok, " off=%llx:%u", static_cast<unsigned long long>(off), n);
            toks.emplace_back(tok);
        }
        named_tokens(tid, toks);
        for (const std::string& t : toks) {
            if (line.size() + t.size() > kLine) {
                std::fprintf(stderr, "%s\n", line.c_str());
                std::snprintf(head, sizeof head, "[PROF] build=%s thread=%s tid=%d part=%d",
                              g_build_id.empty() ? "?" : g_build_id.c_str(), tname.c_str(), tid, ++part);
                line = head;
            }
            if (t.size() < kLine / 2) line += t;   // a name that long would not fit a line: left out
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    if (g_lost) std::fprintf(stderr, "[PROF] %llu samples lost (ring lapped)\n", static_cast<unsigned long long>(g_lost));
    g_hits.clear();
    g_calls.clear();
    g_lost = 0;
}

void run() {
    pthread_setname_np(pthread_self(), "bo2_prof");
    g_self_tid = gettid();
    const char* e = std::getenv("BO2_PROF");
    bool sampling = !(e && std::strcmp(e, "0") == 0);
    const char* h = std::getenv("BO2_PROF_HZ");
    const int hz = h && std::atoi(h) > 0 ? std::min(std::atoi(h), 1000) : 250;
    const long period_ns = 1000000000L / hz;
    if (sampling) {
        g_sig = SIGRTMIN + 4;
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_sample;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (g_sig > SIGRTMAX || sigaction(g_sig, &sa, nullptr) != 0) {
            std::fprintf(stderr, "[PROF] no sampling signal (SIGRTMIN+4 = %d): sampling off\n", g_sig);
            sampling = false;
        }
    }
    refresh_dsos();
    if (sampling) load_anchors();
    std::fprintf(stderr, "[PROF] profiler on: [THREADS]/[SYSINFO] every 5 s%s, libmain build %s\n",
                 sampling ? (", [PROF] every 30 s at " + std::to_string(hz) + " Hz on the 4 busiest threads").c_str()
                          : " (sampling off: BO2_PROF=0)",
                 g_build_id.empty() ? "?" : g_build_id.c_str());
    using clock = std::chrono::steady_clock;
    auto window_start = clock::now();
    threads_window(1.0, 0, 100.0, false);   // primes the per-thread counters, silently
    int tick = 0;
    // The launcher's Logging option (log.hpp), followed live: Full samples; Normal keeps [THREADS]/[SYSINFO] and
    // disarms the sampling timers; Minimal prints nothing (the counters are still read silently, so the first window
    // after switching back covers 5 s, not the whole quiet stretch).
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        tick++;
        const int level = bo2log::diagnostics();
        const bool sample_now = sampling && level == 0;
        if (sampling) drain();
        if (tick % 5 == 0) {
            const auto now = clock::now();
            const double secs = std::chrono::duration<double>(now - window_start).count();
            window_start = now;
            if (level >= 2) {
                threads_window(secs, 0, 100.0, false);
                if (sampling) timers_retarget({}, period_ns);
                continue;
            }
            const std::vector<int> top = threads_window(secs, 4, 5.0);
            sysinfo_line();
            if (sampling) timers_retarget(sample_now ? top : std::vector<int>{}, period_ns);
        }
        if (sample_now && tick % 30 == 0) emit_profile(30);
    }
}

}  // namespace

void start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread(run).detach();
}

}  // namespace bo2prof
