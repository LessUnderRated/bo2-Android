// log.cpp — stdout and stderr to logcat and to a file, and a crash report written before Android's.
//
// C++ port of Chronicle-rs's android/src/logcat.rs (LessUnderRated). bo2-mac's runtime reports
// everything on stderr ([PAD], [INTERP], [DB-LOADXFILE] ...), which Android discards: a pipe takes
// both streams, a reader thread writes each line to logcat (`adb logcat -s BO2`) and to
// files/logs/bo2.log, and the previous run's log is kept as bo2-previous.log for CrashReport.java.
//
// Crash handler and the runtime: bo2-mac installs its own SIGSEGV and SIGBUS handlers
// (win32_runtime.cpp, about lines 13929 and 19830) with a null old action, ending in _exit(139). As
// is, they would silently replace this one. The Android runtime must save the previous action and
// chain to it for faults it does not claim (docs/STEP0_INVENTORY.md); this handler, installed first,
// is that previous action: it reports and hands over to Android's debuggerd.
#include "log.hpp"
#include "remote_log.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

namespace bo2log {

namespace {

constexpr const char* kTag = "BO2";
constexpr uint64_t kLimit = 64ull << 20;  // a run that logs in a loop stops filling the phone here

std::atomic<int> g_file{-1};
std::atomic<uint64_t> g_written{0};
std::atomic<int> g_pipe{-1};
std::atomic<bool> g_busy{false};
timespec g_start{};
// The Logging option (log.hpp): BO2_DIAG at start, the launcher's choice after.
std::atomic<int> g_diag{[] {
    const char* e = std::getenv("BO2_DIAG");
    const int v = e ? std::atoi(e) : 0;
    return v < 0 ? 0 : v > 2 ? 2 : v;
}()};

double seconds_since_start() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_start.tv_sec) + (now.tv_nsec - g_start.tv_nsec) / 1e9;
}

// One write per line, so lines from different threads stay whole in the O_APPEND file.
void append(const char* bytes, size_t n) {
    const int fd = g_file.load(std::memory_order_relaxed);
    if (fd < 0 || g_written.fetch_add(n, std::memory_order_relaxed) > kLimit) return;
    ssize_t ignored = ::write(fd, bytes, n);
    (void)ignored;
}

void start_file(const std::string& dir) {
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    ::mkdir(dir.c_str(), 0700);
    const std::string current = dir + "/" + kCurrent, previous = dir + "/" + kPrevious;
    if (::access(current.c_str(), F_OK) == 0) ::rename(current.c_str(), previous.c_str());
    const int fd = ::open(current.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) g_file.store(fd);
    else __android_log_print(ANDROID_LOG_WARN, kTag, "cannot write %s: %s", current.c_str(), strerror(errno));
}

void wait_for_reader(int limit_ms) {
    const int fd = g_pipe.load();
    if (fd < 0) return;
    for (int i = 0; i < limit_ms / 10; i++) {
        int queued = 0;
        ::ioctl(fd, FIONREAD, &queued);
        if (queued == 0 && !g_busy.load()) break;
        usleep(10000);
    }
    usleep(20000);
}

void drain_at_exit() {
    fflush(nullptr);
    wait_for_reader(500);
}

// High-volume diagnostics from the runtime's bring-up (one line every few draws, waits or frames): hundreds of lines a
// second in a match, each one a logcat write and a file write. Dropped here unless BO2_LOG_ALL=1. [PERF], game
// messages, errors and crash reports are kept.
std::atomic<void (*)(const char*)> g_observer{nullptr};

bool noisy(const std::string& line) {
    static const bool keep_all = [] { const char* e = getenv("BO2_LOG_ALL"); return e && *e && *e != '0'; }();
    if (keep_all) return false;
    static const char* const kPrefixes[] = {
        "[D3D-DRAW]", "[D3D-PRESENT]", "[SRV] bind", "[IMP-HASH]", "[VB-COPIE]", "[WAIT-MAP]", "[STACK-CENSUS]",
        "[XAUDIO]", "[GSC-RATE]", "[VM-ENTER]", "[TICK-RATE]", "[SL-DEVINE]", "[SL-SEAU]", "[TRUELEN]",
        "[SLFREE-VOL]", "[DRAIN-TOURS]", "[CAS-SPIN-HOTE", "[AUDIO-PREEMPT", "[WFSO-DEKKER",
        "[gfx] CopyResource", "[gfx] REAL IMAGE", "[OutputDebugString] STEAM_SERVER: still connecting",
        // Per-call and per-event traces from bring-up (round 7 on the S26: the import trace alone was 92 % of the
        // log; the rest of this list was most of what remained). Each is a debugging switch's witness, not news.
        "[IMPORT]", "[CFN]", "[HS] ", "[SetEvent]", "[ResetEvent]", "[WFSO]", "[WFSO-block]", "[ReadFile]",
        "[D3D-TID]", "[MSL]", "[POCHOIR]", "[CISEAU]", "[LONGJMP-ALL]", "[DWP]", "[GMHW]", "[STEAM-UNIMPL]",
        "[OutputDebugString] Redundant", "[OutputDebugString] Task", "[OutputDebugString] Demonware",
        "[OutputDebugString] Nested", "[OutputDebugString] NESTED", "[OutputDebugString] QQTECH",
        "[OutputDebugString] Setting presence", "[OutputDebugString] Local task", "[gfx] textures created",
        "[gfx] Texture",
    };
    for (const char* p : kPrefixes)
        if (line.compare(0, strlen(p), p) == 0) return true;
    return false;
}

void redirect_stdio() {
    int fds[2];
    if (::pipe(fds) != 0) return;
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[1]);
    // C's stdout is fully buffered once it is not a terminal; lines should reach logcat as written.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::atexit(drain_at_exit);
    g_pipe.store(fds[0]);
    std::thread([fd = fds[0]] {
        pthread_setname_np(pthread_self(), "bo2-log");
        char buffer[4096];
        std::string pending;
        for (;;) {
            g_busy.store(!pending.empty());
            const ssize_t n = ::read(fd, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            g_busy.store(true);
            pending.append(buffer, static_cast<size_t>(n));
            size_t end;
            while ((end = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (auto* obs = g_observer.load()) obs(line.c_str());
                if (line.empty() || noisy(line)) continue;
                write(line.c_str());
            }
        }
    }).detach();
}

// ---- Crashes ------------------------------------------------------------------------------------

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS};
constexpr int kSignalCount = sizeof kSignals / sizeof kSignals[0];
struct sigaction g_previous[kSignalCount];

// A line built on the stack: the handler runs where the heap may be what broke.
struct Line {
    char bytes[512];
    size_t len = 0;
    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        const int n = vsnprintf(bytes + len, sizeof bytes - 1 - len, fmt, ap);
        va_end(ap);
        if (n > 0) len = std::min(sizeof bytes - 2, len + static_cast<size_t>(n));
    }
    void emit() {
        bytes[len] = 0;
        __android_log_write(ANDROID_LOG_FATAL, kTag, bytes);
        bytes[len] = '\n';
        append(bytes, len + 1);
        len = 0;
    }
};

const char* signal_name(int s) {
    switch (s) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
        case SIGSYS: return "SIGSYS";
        default: return "signal";
    }
}

// `pc 0x<offset in its library> <library> (<symbol>+0x<offset>)`, as a tombstone writes a frame, so
// it can be matched against the unstripped libmain.so the build keeps.
void describe(Line& line, uintptr_t address) {
    Dl_info info{};
    if (!address || !dladdr(reinterpret_cast<void*>(address), &info)) {
        line.add("pc %#lx (unknown)", static_cast<unsigned long>(address));
        return;
    }
    const char* library = info.dli_fname ? info.dli_fname : "?";
    if (const char* slash = strrchr(library, '/')) library = slash + 1;
    line.add("pc %#lx %s", static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(info.dli_fbase)), library);
    if (info.dli_sname)
        line.add(" (%s+%#lx)", info.dli_sname, static_cast<unsigned long>(address - reinterpret_cast<uintptr_t>(info.dli_saddr)));
}

struct Frames {
    int count = 0;
};

_Unwind_Reason_Code frame(_Unwind_Context* context, void* argument) {
    Frames& frames = *static_cast<Frames*>(argument);
    if (frames.count >= 48) return _URC_END_OF_STACK;
    const uintptr_t ip = _Unwind_GetIP(context);
    if (!ip) return _URC_END_OF_STACK;
    Line line;
    line.add("  #%02d ", frames.count);
    describe(line, frames.count == 0 ? ip : ip - 1);  // a return address points after the call
    line.emit();
    frames.count++;
    return _URC_NO_REASON;
}

void report(int signal, siginfo_t* info, void* context) {
    wait_for_reader(200);  // what was printed just before, first
    Line line;
    char thread[17] = {};
    prctl(PR_GET_NAME, thread);
    line.add("fatal signal %d (%s), code %d, fault address %#lx, in thread %d (%s)", signal, signal_name(signal),
             info ? info->si_code : 0, info ? reinterpret_cast<unsigned long>(info->si_addr) : 0ul,
             static_cast<int>(gettid()), thread);
    line.emit();
#if defined(__aarch64__)
    if (context) {
        const mcontext_t& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
        line.add("  at ");
        describe(line, mc.pc);
        line.emit();
        line.add("  lr ");
        describe(line, mc.regs[30]);
        line.emit();
        line.add("  sp %#llx", static_cast<unsigned long long>(mc.sp));
        line.emit();
    }
#else
    (void)context;
#endif
    line.add("backtrace:");
    line.emit();
    Frames frames;
    _Unwind_Backtrace(frame, &frames);
}

void on_crash(int signal, siginfo_t* info, void* context) {
    static std::atomic<bool> crashed{false};
    int index = 0;
    for (int i = 0; i < kSignalCount; i++)
        if (kSignals[i] == signal) index = i;
    // Android's handler first, then the report: if the report itself faults (a smashed stack can make
    // the unwinder fault too), that fault goes to debuggerd and still leaves a tombstone. After the
    // report, a fault comes back to debuggerd when the instruction runs again; a sent signal (abort's)
    // is sent again.
    sigaction(signal, &g_previous[index], nullptr);
    if (!crashed.exchange(true)) report(signal, info, context);
    if (!info || info->si_code <= 0) syscall(SYS_tgkill, getpid(), gettid(), signal);
}

void install_crash_handler() {
    for (int i = 0; i < kSignalCount; i++) {
        struct sigaction action {};
        action.sa_sigaction = on_crash;
        // bionic gives every thread an alternate stack, so a stack overflow is reported too.
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(kSignals[i], &action, &g_previous[i]);
    }
}

}  // namespace

void set_line_observer(void (*observer)(const char* line)) { g_observer.store(observer); }

int diagnostics() { return g_diag.load(std::memory_order_relaxed); }

void set_diagnostics(int level) {
    level = level < 0 ? 0 : level > 2 ? 2 : level;
    const int was = g_diag.exchange(level);
    bo2remote::set_enabled(level < 2);
    if (was != level) {
        static const char* const kNames[] = {"Full (logcat, PC log, profiler)", "Normal (logcat, PC log, no sampling "
                                             "profiler)", "Minimal (this file only)"};
        write((std::string("[DIAG] logging: ") + kNames[level]).c_str());
    }
}

void write(const char* line) {
    // Minimal: the file only. Each logcat line is a write to logd, which then parses and stores it in its own process.
    if (g_diag.load(std::memory_order_relaxed) < 2) __android_log_write(ANDROID_LOG_INFO, kTag, line);
    char prefix[32];
    const int n = snprintf(prefix, sizeof prefix, "%9.3f ", seconds_since_start());
    std::string text(prefix, n > 0 ? static_cast<size_t>(n) : 0);
    text += line;
    text += '\n';
    append(text.data(), text.size());
    bo2remote::enqueue(text.data(), text.size());
}

void open(const char* dir) {
    static std::once_flag once;
    std::call_once(once, [dir] {
        start_file(dir);
        // First line of the file: the remote log's session id, so the next launch can send this run's tail there.
        write(("session " + bo2remote::session()).c_str());
        bo2remote::start(dir, kPrevious);
        redirect_stdio();
        install_crash_handler();
        char line[96];
        snprintf(line, sizeof line, "log of process %d, logging level %d (0 Full, 1 Normal, 2 Minimal)",
                 static_cast<int>(getpid()), g_diag.load());
        write(line);
    });
}

}  // namespace bo2log
